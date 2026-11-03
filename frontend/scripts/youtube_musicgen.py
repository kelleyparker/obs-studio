#!/usr/bin/env python3
"""
Local background music for the YouTube pipeline with ACE-Step 1.5.

youtube_pipeline.py runs this with the Python of the ACE-Step venv (~/OBSYouTubeMusicGen/venv, PyTorch
for CUDA 12.8, which the RTX 50 series needs), so the pipeline's own Python doesn't need any of it.
The job comes in as JSON on stdin and one JSON line is printed per event:
{"type": "loading"|"loaded"|"progress"|"song"|"error", ...}.

Job:
  {"ace_dir": ACE-Step-1.5 checkout (with checkpoints/), "dit": "acestep-v15-turbo",
   "lm": "acestep-5Hz-lm-0.6B" (or "" for none), "offload": true, "offload_dit": false, "parent_pid": pid,
   "songs": [{"caption", "lyrics", "instrumental", "bpm", "keyscale", "duration", "seed", "out"}]}
  writes each song as a WAV file to its "out"

With --list-gpus it prints the NVIDIA GPUs instead, as one JSON list of {name, gb, uuid, bus}, straight from
the CUDA driver (no PyTorch, no nvidia-smi), so the pipeline can pick one and hand it over in CUDA_VISIBLE_DEVICES.

With --serve it stays running with the models loaded, so the next music step doesn't load them again. Its
setup comes in on stdin: {"token", "key", "info", "obs_pid", "idle_s"}. It listens on 127.0.0.1 and writes
{"pid", "port", "token", "key"} to the "info" file. Each connection sends one line, {"token", "job"} (the
job's "key" must match the server's) or {"token", "quit": true}, and gets the same JSON lines back, ending
with {"type": "done"}. It exits when OBS (obs_pid) does, after idle_s with no job (0 for never), when a
client goes away in the middle of a job (Stop in OBS), or when the models don't load. When it can't
watch OBS, idle_s defaults to half an hour.
"""

import hmac
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time

# ACE-Step and its libraries print to stdout; keep it for our JSON lines only
_events = sys.stdout
sys.stdout = sys.stderr


def emit(kind: str, **fields):
    try:
        print(json.dumps({"type": kind, **fields}), file=_events, flush=True)
    except OSError:
        os._exit(1)  # the pipeline is gone


def exit_with_parent(pid: int) -> bool:
    """
    Stop when `pid` does (the pipeline: Cancel kills it, not us), so the GPU isn't kept busy. False if
    it can't watch it.
    """
    if os.name != "nt" or not pid:
        return False
    import ctypes

    SYNCHRONIZE = 0x00100000
    handle = ctypes.windll.kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if not handle:
        return False

    def wait():
        ctypes.windll.kernel32.WaitForSingleObject(handle, 0xFFFFFFFF)
        os._exit(1)

    threading.Thread(target=wait, daemon=True).start()
    return True


def list_gpus() -> list[dict]:
    """The NVIDIA GPUs the CUDA driver sees, in PCI bus order."""
    import ctypes

    cu = ctypes.WinDLL("nvcuda.dll") if os.name == "nt" else ctypes.CDLL("libcuda.so.1")
    if cu.cuInit(0) != 0:
        return []
    count = ctypes.c_int()
    cu.cuDeviceGetCount(ctypes.byref(count))
    gpus = []
    for i in range(count.value):
        dev = ctypes.c_int()
        if cu.cuDeviceGet(ctypes.byref(dev), i) != 0:
            continue
        name = ctypes.create_string_buffer(256)
        mem = ctypes.c_size_t()
        bus = ctypes.create_string_buffer(64)
        uuid = (ctypes.c_ubyte * 16)()
        cu.cuDeviceGetName(name, 256, dev)
        cu.cuDeviceTotalMem_v2(ctypes.byref(mem), dev)
        cu.cuDeviceGetPCIBusId(bus, 64, dev)
        if cu.cuDeviceGetUuid_v2(uuid, dev) != 0:
            cu.cuDeviceGetUuid(uuid, dev)
        h = bytes(uuid).hex()
        gpus.append({"name": name.value.decode(errors="replace"), "gb": round(mem.value / 2 ** 30, 1),
                     "uuid": f"GPU-{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}",
                     "bus": bus.value.decode(errors="replace")})
    return sorted(gpus, key=lambda g: g["bus"])


class Models:
    """The loaded DiT, and the 5 Hz LM that plans the song, loaded the first time a job wants it."""

    def __init__(self):
        self.dit = None
        self.llm = None
        self.lm_ok = False
        self.lm_tried = False

    def load(self, job: dict) -> bool:
        """Loads what `job` needs and isn't loaded yet. Returns True if the DiT was loaded already."""
        from acestep.llm_inference import LLMHandler

        warm = self.dit is not None
        ace_dir = job["ace_dir"]
        offload = bool(job.get("offload", True))
        if not warm:
            from acestep.handler import AceStepHandler

            dit = AceStepHandler()
            message, ok = dit.initialize_service(
                project_root=ace_dir, config_path=job.get("dit", "acestep-v15-turbo"), device="cuda",
                offload_to_cpu=offload, offload_dit_to_cpu=offload and bool(job.get("offload_dit", False)))
            if not ok:
                raise RuntimeError(f"ACE-Step didn't load: {message}")
            self.dit, self.llm = dit, LLMHandler()
        if not job.get("lm") or self.lm_tried:
            return warm
        self.lm_tried = True
        checkpoints = os.path.join(ace_dir, "checkpoints")
        lm = pick_lm(checkpoints, job["lm"])
        if not lm:
            emit("warning", message=f"The song-planning LM ({job['lm']}) isn't downloaded, so songs are made "
                                    "without it.")
            return warm
        try:
            message, self.lm_ok = self.llm.initialize(checkpoint_dir=checkpoints, lm_model_path=lm, backend="pt",
                                                      device="cuda", offload_to_cpu=offload)
            if not self.lm_ok:
                emit("warning", message="The song-planning LM didn't load, so songs are made without it: "
                                        + first_line(message))
        except Exception as e:
            emit("warning", message=f"The song-planning LM didn't load, so songs are made without it: {first_line(e)}")
        return warm


def first_line(message) -> str:
    """ACE-Step's errors carry the whole traceback; the first line says what went wrong."""
    return next((line.strip(" ❌") for line in str(message).splitlines() if line.strip()), "")[:300]


def pick_lm(checkpoints: str, wanted: str) -> str:
    """`wanted` if its weights are downloaded, else the smallest other 5 Hz LM that is, else ""."""
    def complete(name):
        folder = os.path.join(checkpoints, name)
        return os.path.isdir(folder) and any(f.endswith(".safetensors") for f in os.listdir(folder))

    if not wanted or complete(wanted):
        return wanted
    try:
        others = [n for n in os.listdir(checkpoints) if n.startswith("acestep-5Hz-lm-") and complete(n)]
    except OSError:
        return ""
    sizes = {n: sum(os.path.getsize(os.path.join(checkpoints, n, f)) for f in os.listdir(os.path.join(checkpoints, n))
                    if f.endswith(".safetensors")) for n in others}
    return min(others, key=sizes.get, default="")


def make_song(dit, llm, lm_ok: bool, song: dict, index: int, count: int):
    from acestep.inference import GenerationConfig, GenerationParams, generate_music

    instrumental = bool(song.get("instrumental", True))
    params = GenerationParams(
        task_type="text2music",
        caption=song["caption"][:500],
        lyrics="[Instrumental]" if instrumental else song.get("lyrics", ""),
        instrumental=instrumental,
        bpm=song.get("bpm") or None,
        keyscale=song.get("keyscale", ""),
        timesignature="4",
        vocal_language="en",
        duration=float(song["duration"]),
        inference_steps=8,
        guidance_scale=1.0,
        shift=3.0,
        seed=int(song.get("seed", -1)),
        thinking=lm_ok,
        # Keep the style prompt as written; the LM only fills in what's missing. A kept-loaded LM
        # isn't used for a job that doesn't want it.
        use_cot_metas=lm_ok,
        use_cot_caption=False,
        use_cot_language=False,
    )
    config = GenerationConfig(batch_size=1, use_random_seed=False, seeds=[int(song.get("seed", 0))],
                              audio_format="wav")

    def progress(value=0.0, desc="", *args, **kwargs):
        try:
            emit("progress", index=index, count=count, fraction=max(0.0, min(1.0, float(value))), desc=str(desc))
        except (TypeError, ValueError):
            pass

    with tempfile.TemporaryDirectory(prefix="acestep_") as tmp:
        result = generate_music(dit, llm, params=params, config=config, save_dir=tmp, progress=progress)
        if not result.success or not result.audios or not result.audios[0].get("path"):
            raise RuntimeError(result.error or result.status_message or "ACE-Step made no audio")
        os.makedirs(os.path.dirname(os.path.abspath(song["out"])), exist_ok=True)
        shutil.move(result.audios[0]["path"], song["out"])


def run_job(job: dict, models: Models) -> int:
    """Loads what the job needs, makes its songs and emits the events. 2 if the models didn't load."""
    if job["ace_dir"] not in sys.path:
        sys.path.insert(0, job["ace_dir"])
    songs = job.get("songs", [])

    started = time.time()
    emit("loading")
    try:
        warm = models.load(job)
    except Exception as e:
        emit("error", message=str(e))
        return 2
    lm_ok = models.lm_ok and bool(job.get("lm"))
    emit("loaded", seconds=round(time.time() - started, 1), lm=lm_ok, warm=warm)

    failed = 0
    for i, song in enumerate(songs):
        t0 = time.time()
        try:
            make_song(models.dit, models.llm, lm_ok, song, i, len(songs))
            emit("song", index=i, path=song["out"], seconds=round(time.time() - t0, 1))
        except Exception as e:
            failed += 1
            emit("error", index=i, message=str(e)[:300])
    return 1 if failed == len(songs) and songs else 0


def free_gpu_cache():
    try:
        import torch

        if torch.cuda.is_available():
            torch.cuda.empty_cache()
    except Exception:
        pass


def serve(setup: dict) -> int:
    """Keeps the models loaded and makes the songs for each job that connects (see the top of this file)."""
    global _events
    watching = exit_with_parent(int(setup.get("obs_pid", 0)))
    token, key = str(setup["token"]), setup.get("key")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    # Without OBS to watch, it goes after half an hour with nothing to do
    idle = float(setup.get("idle_s", 0) or (0 if watching else 1800))
    listener.settimeout(idle or None)
    info = setup["info"]
    with open(info + ".tmp", "w", encoding="utf-8") as f:
        json.dump({"pid": os.getpid(), "port": listener.getsockname()[1], "token": token, "key": key}, f)
    os.replace(info + ".tmp", info)

    models = Models()
    while True:
        try:
            conn, _ = listener.accept()
        except socket.timeout:
            return 0
        conn.settimeout(15)
        reader = conn.makefile("r", encoding="utf-8")
        try:
            request = json.loads(reader.readline())
        except (OSError, ValueError):
            request = None
        finally:
            reader.close()  # else closing conn wouldn't close the socket
        if request is None:
            conn.close()
            continue
        if not isinstance(request, dict) or not hmac.compare_digest(str(request.get("token", "")), token):
            conn.close()
            continue
        if request.get("quit"):
            conn.close()
            return 0
        conn.settimeout(None)
        _events = conn.makefile("w", encoding="utf-8")

        def finish(c=conn):
            global _events
            emit("done")
            try:
                _events.close()
                c.close()
            except OSError:
                pass
            _events = sys.stderr  # between jobs anything said goes to the log

        job = request.get("job") or {}
        if job.get("key") != key:
            emit("error", message="The music server was started for another GPU or setup.")
            finish()
            continue

        running = threading.Event()
        running.set()

        def watch(c=conn, flag=running):
            # The client sends nothing more, so the read ends when it goes away (Stop in OBS)
            try:
                gone = not c.recv(1)
            except OSError:
                gone = True
            if gone and flag.is_set():
                os._exit(1)

        threading.Thread(target=watch, daemon=True).start()
        result = run_job(job, models)
        free_gpu_cache()
        running.clear()
        finish()
        if result == 2:
            return 1  # a fresh process loads them next time


def main() -> int:
    if "--list-gpus" in sys.argv[1:]:
        emit("gpus", gpus=list_gpus())
        return 0
    try:
        config = json.loads(sys.stdin.read())
    except ValueError as e:
        emit("error", message=f"Bad job: {e}")
        return 1
    if "--serve" in sys.argv[1:]:
        return serve(config)
    exit_with_parent(int(config.get("parent_pid", 0)))
    return 1 if run_job(config, Models()) else 0


if __name__ == "__main__":
    sys.exit(main())
