#!/usr/bin/env python3
"""
Local Stable Diffusion and background removal for YouTube thumbnails.

youtube_pipeline.py runs this with the Python of a separate venv that has PyTorch (CUDA), diffusers
and rembg, so the pipeline's own Python doesn't need them. The job comes in as JSON on stdin and one
JSON line is printed per event: {"type": "loading"|"loaded"|"progress"|"image"|"error"|"cutout", ...}.

Jobs:
  {"model", "prompts": [...], "negative_prompt", "width", "height", "steps", "guidance", "out_dir"}
      text-to-image, writes sd_1.png, sd_2.png, ... in out_dir
  the same plus "init_images": [...] (one per prompt) and "strength"
      image-to-image (Refine), same outputs
  {"task": "cutout", "image", "out"}
      removes the background of a photo of a person and writes an RGBA PNG cropped to the person
"""

import json
import os
import sys
import time


def emit(kind: str, **fields):
    print(json.dumps({"type": kind, **fields}), flush=True)


def pick_device(torch):
    if not torch.cuda.is_available():
        return "cpu"
    # Use the GPU with the most memory (e.g. the RTX 5070, not a smaller second card)
    best = max(range(torch.cuda.device_count()), key=lambda i: torch.cuda.get_device_properties(i).total_memory)
    return f"cuda:{best}"


def load_pipeline(model: str, device: str, img2img: bool):
    import torch
    from diffusers import AutoPipelineForImage2Image, AutoPipelineForText2Image

    cls = AutoPipelineForImage2Image if img2img else AutoPipelineForText2Image
    dtype = torch.float16 if device.startswith("cuda") else torch.float32
    try:
        pipe = cls.from_pretrained(model, torch_dtype=dtype, variant="fp16", use_safetensors=True)
    except (OSError, ValueError):
        # Models without an fp16 variant
        pipe = cls.from_pretrained(model, torch_dtype=dtype, use_safetensors=True)
    pipe.set_progress_bar_config(disable=True)
    if hasattr(pipe, "vae") and hasattr(pipe.vae, "enable_tiling"):
        # SDXL decodes in fp32; a full 1344x768 decode at once can top out a 12 GB card
        pipe.vae.enable_tiling()
    return pipe.to(device)


def cutout(job: dict) -> int:
    """Background removal with rembg (u2net_human_seg), cropped to the person."""
    try:
        from PIL import Image, ImageOps
        from rembg import new_session, remove

        image = ImageOps.exif_transpose(Image.open(job["image"])).convert("RGB")
        person = remove(image, session=new_session(job.get("model") or "u2net_human_seg"))
        box = person.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox()
        if not box:
            emit("error", message="No person found in the photo.")
            return 1
        person.crop(box).save(job["out"])
        emit("cutout", path=job["out"])
        return 0
    except Exception as e:
        emit("error", message=f"Background removal failed: {e}")
        return 1


def main() -> int:
    job = json.loads(sys.stdin.read())
    if job.get("task") == "cutout":
        return cutout(job)

    out_dir = job["out_dir"]
    os.makedirs(out_dir, exist_ok=True)
    init_images = job.get("init_images") or []

    try:
        import torch

        device = pick_device(torch)
        emit("loading")
        started = time.time()
        pipe = load_pipeline(job["model"], device, bool(init_images))
        name = torch.cuda.get_device_name(device) if device.startswith("cuda") else "CPU"
        emit("loaded", device=name, seconds=round(time.time() - started, 1))
    except Exception as e:
        emit("error", message=f"Could not load {job['model']}: {e}")
        return 1

    from PIL import Image

    width, height = int(job.get("width", 1344)), int(job.get("height", 768))
    steps = int(job.get("steps", 30))
    last_emit = [0.0]

    for i, prompt in enumerate(job["prompts"]):
        def on_step(p, step, timestep, kwargs, i=i):
            now = time.time()
            if now - last_emit[0] >= 0.5:
                last_emit[0] = now
                emit("progress", index=i, step=step + 1, steps=getattr(p, "num_timesteps", steps))
            return kwargs

        args = dict(prompt=prompt, negative_prompt=job.get("negative_prompt") or None,
                    num_inference_steps=steps, guidance_scale=float(job.get("guidance", 7.0)),
                    callback_on_step_end=on_step)
        if init_images:
            init = Image.open(init_images[i % len(init_images)]).convert("RGB").resize((width, height))
            args.update(image=init, strength=float(job.get("strength", 0.5)))
        else:
            args.update(width=width, height=height)

        # One retry: a failure is usually a one-off CUDA out-of-memory that clearing the cache fixes
        for attempt in range(2):
            try:
                started = time.time()
                image = pipe(**args).images[0]
                path = os.path.join(out_dir, f"sd_{i + 1}.png")
                image.save(path)
                emit("image", index=i, path=path, seconds=round(time.time() - started, 1))
                break
            except Exception as e:
                if attempt:
                    emit("error", index=i, message=str(e))
            finally:
                if device.startswith("cuda"):
                    torch.cuda.empty_cache()
    return 0


if __name__ == "__main__":
    sys.exit(main())
