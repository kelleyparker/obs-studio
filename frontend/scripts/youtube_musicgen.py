#!/usr/bin/env python3
"""
Local background music for the YouTube pipeline with ACE-Step 1.5.

youtube_pipeline.py runs this with the Python of the ACE-Step venv (~/OBSYouTubeMusicGen/venv, PyTorch
for CUDA 12.8, which the RTX 50 series needs), so the pipeline's own Python doesn't need any of it.
The job comes in as JSON on stdin and one JSON line is printed per event:
{"type": "loading"|"loaded"|"progress"|"song"|"error", ...}.

Job:
  {"ace_dir": ACE-Step-1.5 checkout (with checkpoints/), "dit": "acestep-v15-turbo",
   "lm": "acestep-5Hz-lm-0.6B" (or "" for none), "offload": true, "parent_pid": pid,
   "songs": [{"caption", "lyrics", "instrumental", "bpm", "keyscale", "duration", "seed", "out"}]}
  writes each song as a WAV file to its "out"
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time

# ACE-Step and its libraries print to stdout; keep it for our JSON lines only
_events = sys.stdout
sys.stdout = sys.stderr


def emit(kind: str, **fields):
    print(json.dumps({"type": kind, **fields}), file=_events, flush=True)


def exit_with_parent(pid: int):
    """Stop when the pipeline is stopped (Cancel kills it, not us), so the GPU isn't kept busy."""
    if os.name != "nt" or not pid:
        return
    import ctypes

    SYNCHRONIZE = 0x00100000
    handle = ctypes.windll.kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if not handle:
        return

    def wait():
        ctypes.windll.kernel32.WaitForSingleObject(handle, 0xFFFFFFFF)
        os._exit(1)

    threading.Thread(target=wait, daemon=True).start()


def load_models(job: dict):
    """The DiT, and the 5 Hz LM that plans the song (skipped if it can't load). Returns (dit, llm, lm_ok)."""
    from acestep.handler import AceStepHandler
    from acestep.llm_inference import LLMHandler

    ace_dir = job["ace_dir"]
    offload = bool(job.get("offload", True))
    dit = AceStepHandler()
    message, ok = dit.initialize_service(project_root=ace_dir, config_path=job.get("dit", "acestep-v15-turbo"),
                                         device="cuda", offload_to_cpu=offload)
    if not ok:
        raise RuntimeError(f"ACE-Step didn't load: {message}")

    llm = LLMHandler()
    lm_ok = False
    if job.get("lm"):
        try:
            message, lm_ok = llm.initialize(checkpoint_dir=os.path.join(ace_dir, "checkpoints"),
                                            lm_model_path=job["lm"], backend="pt", device="cuda",
                                            offload_to_cpu=offload)
            if not lm_ok:
                emit("warning", message=f"The song-planning LM didn't load, so songs are made without it: {message}")
        except Exception as e:
            emit("warning", message=f"The song-planning LM didn't load, so songs are made without it: {e}")
    return dit, llm, lm_ok


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
        # Keep the style prompt as written; the LM only fills in what's missing
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


def main() -> int:
    try:
        job = json.loads(sys.stdin.read())
    except ValueError as e:
        emit("error", message=f"Bad job: {e}")
        return 1
    exit_with_parent(int(job.get("parent_pid", 0)))
    sys.path.insert(0, job["ace_dir"])
    songs = job.get("songs", [])

    started = time.time()
    emit("loading")
    try:
        dit, llm, lm_ok = load_models(job)
    except Exception as e:
        emit("error", message=str(e))
        return 1
    emit("loaded", seconds=round(time.time() - started, 1), lm=lm_ok)

    failed = 0
    for i, song in enumerate(songs):
        t0 = time.time()
        try:
            make_song(dit, llm, lm_ok, song, i, len(songs))
            emit("song", index=i, path=song["out"], seconds=round(time.time() - t0, 1))
        except Exception as e:
            failed += 1
            emit("error", index=i, message=str(e)[:300])
    return 1 if failed == len(songs) and songs else 0


if __name__ == "__main__":
    sys.exit(main())
