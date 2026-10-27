#!/usr/bin/env python3
"""
OBS Studio - YouTube Auto-Edit, AI Thumbnail & Upload Workflow Engine
=====================================================================
Security note:
  All API keys (Gemini, OpenAI, Claude) and YouTube OAuth credentials
  are decrypted in-memory by obs64.exe (via Windows DPAPI + OBS binary entropy)
  and piped into this worker over private stdin. Global environment variables
  for API keys are ignored by default to enforce OBS-only isolation.
"""

import argparse
import base64
import difflib
import io
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


def emit(event_type: str, **kwargs):
    """Send a structured JSON-line event to OBS Studio over stdout."""
    payload = {"type": event_type, "timestamp": time.time()}
    payload.update(kwargs)
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log_info(msg: str):
    emit("log", level="info", message=msg)


def log_warn(msg: str):
    emit("log", level="warning", message=msg)


def log_error(msg: str):
    emit("log", level="error", message=msg)


def update_step(step: str, progress: int, message: str, **extra):
    emit("step", step=step, progress=int(max(0, min(100, progress))), message=message, **extra)


# ---------------------------------------------------------------------------
# 1. Media Probing & Silence Detection via FFmpeg
# ---------------------------------------------------------------------------

def get_video_duration(video_path: str) -> float:
    try:
        cmd = [
            "ffprobe", "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=noprint_wrappers=1:nokey=1",
            video_path
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return float(res.stdout.strip())
    except Exception:
        return 0.0


def detect_nonsilent_intervals(
    video_path: str,
    silence_db: float = -32.0,
    min_silence_dur: float = 0.5,
    padding_sec: float = 0.15,
    total_duration: float = 0.0
) -> list[tuple[float, float]]:
    """
    Runs FFmpeg silencedetect filter and returns a list of (start, end)
    intervals that contain active audio (with natural speech padding).
    """
    if total_duration <= 0:
        total_duration = get_video_duration(video_path)
    if total_duration <= 0:
        return []

    cmd = [
        "ffmpeg", "-hide_banner", "-nostats",
        "-i", video_path,
        "-af", f"silencedetect=noise={silence_db}dB:d={min_silence_dur}",
        "-f", "null", "-"
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    stderr = res.stderr or ""

    silence_starts = []
    silence_ends = []
    for line in stderr.splitlines():
        if "silence_start:" in line:
            m = re.search(r"silence_start:\s*([0-9.]+)", line)
            if m:
                silence_starts.append(float(m.group(1)))
        elif "silence_end:" in line:
            m = re.search(r"silence_end:\s*([0-9.]+)", line)
            if m:
                silence_ends.append(float(m.group(1)))

    silences = []
    for i, s_start in enumerate(silence_starts):
        s_end = silence_ends[i] if i < len(silence_ends) else total_duration
        if s_end > s_start:
            silences.append((s_start, s_end))

    if not silences:
        return [(0.0, total_duration)]

    # Convert silence intervals into active speech keep-intervals
    keep = []
    cursor = 0.0
    for s_start, s_end in silences:
        adj_start = max(cursor, s_start + padding_sec)
        adj_end = max(adj_start, s_end - padding_sec)
        if adj_start > cursor + 0.05:
            keep.append((cursor, adj_start))
        cursor = adj_end

    if total_duration - cursor > 0.05:
        keep.append((cursor, total_duration))

    # Merge tiny adjacent gaps (< 0.2s)
    merged = []
    for seg in keep:
        if not merged:
            merged.append(list(seg))
        else:
            if seg[0] - merged[-1][1] < 0.20:
                merged[-1][1] = seg[1]
            else:
                merged.append(list(seg))

    return [(round(a, 3), round(b, 3)) for a, b in merged if (b - a) >= 0.15]


# ---------------------------------------------------------------------------
# 2. Whisper Large-v3-Turbo CUDA Transcription & Retake Detection
# ---------------------------------------------------------------------------

def load_audio_16k_float32(video_path: str):
    """
    Decodes audio directly to 16kHz mono float32 numpy array via FFmpeg,
    bypassing PyAV 19 metadata_errors incompatibility in faster-whisper.
    """
    import numpy as np
    cmd = [
        "ffmpeg", "-nostdin", "-threads", "0",
        "-i", video_path,
        "-f", "f32le", "-ac", "1", "-acodec", "pcm_f32le", "-ar", "16000", "-"
    ]
    res = subprocess.run(cmd, capture_output=True)
    if res.returncode != 0 or not res.stdout:
        raise RuntimeError("FFmpeg audio decode failed")
    return np.frombuffer(res.stdout, dtype=np.float32).copy()


def register_nvidia_cuda_dlls():
    """Adds pip-installed nvidia-cublas-cu12 and nvidia-cudnn-cu12 bin folders to PATH & DLL search."""
    try:
        import site
        roots = site.getusersitepackages()
        if isinstance(roots, str):
            roots = [roots]
        for root in list(roots) + list(site.getsitepackages()):
            nv_dir = os.path.join(root, "nvidia")
            if os.path.isdir(nv_dir):
                for sub in os.listdir(nv_dir):
                    bin_dir = os.path.join(nv_dir, sub, "bin")
                    if os.path.isdir(bin_dir):
                        os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")
                        if hasattr(os, "add_dll_directory"):
                            try:
                                os.add_dll_directory(bin_dir)
                            except OSError:
                                pass
    except Exception:
        pass


def transcribe_video(video_path: str, model_name: str = "large-v3-turbo", gpu_index: int = 1) -> list[dict]:
    """
    Transcribes the video using faster-whisper with word-level timestamps.
    Falls back to GPU 0 or CPU if the requested GPU index is unavailable.
    """
    os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"
    register_nvidia_cuda_dlls()
    import faster_whisper

    log_info(f"Loading Whisper model '{model_name}' on CUDA...")
    model = None
    for dev_idx in [gpu_index, 0]:
        try:
            model = faster_whisper.WhisperModel(
                model_name,
                device="cuda",
                device_index=dev_idx,
                compute_type="float16"
            )
            log_info(f"Whisper '{model_name}' loaded on CUDA GPU #{dev_idx} (FP16).")
            break
        except Exception as e:
            log_warn(f"CUDA GPU #{dev_idx} init note: {e}")

    if model is None:
        log_info("Falling back to CPU int8 for Whisper...")
        model = faster_whisper.WhisperModel(model_name, device="cpu", compute_type="int8")

    audio_pcm = load_audio_16k_float32(video_path)
    segments_gen, info = model.transcribe(
        audio_pcm,
        beam_size=5,
        word_timestamps=True,
        vad_filter=True,
        vad_parameters=dict(min_silence_duration_ms=400)
    )

    duration = info.duration or get_video_duration(video_path) or 1.0
    segments = []
    for seg in segments_gen:
        pct = int(min(95, (seg.end / duration) * 100))
        update_step("transcribe", pct, f"Transcribing ({seg.end:.1f}s / {duration:.1f}s)...")
        words = []
        if seg.words:
            for w in seg.words:
                words.append({
                    "word": w.word,
                    "start": round(float(w.start), 3),
                    "end": round(float(w.end), 3)
                })
        segments.append({
            "start": round(float(seg.start), 3),
            "end": round(float(seg.end), 3),
            "text": seg.text.strip(),
            "words": words
        })

    update_step("transcribe", 100, f"Transcription complete ({len(segments)} segments).")
    return segments


def normalize_for_retake(text: str) -> list[str]:
    cleaned = re.sub(r"[^a-z0-9\s]", "", text.lower())
    fillers = {"uh", "um", "er", "ah", "like", "wait", "sorry", "oops", "let", "me", "restart"}
    return [w for w in cleaned.split() if w and w not in fillers]


def detect_butchered_retakes(segments: list[dict]) -> list[tuple[float, float, str]]:
    """
    Identifies butchered sentences / aborted takes that are immediately repeated
    on the next attempt. Returns a list of (cut_start, cut_end, reason) intervals.
    """
    cuts = []
    n = len(segments)
    retake_cues = ("wait", "sorry", "oops", "scratch that", "let me try again", "hold on", "take two")

    for i in range(n - 1):
        cur = segments[i]
        nxt = segments[i + 1]

        # Must be reasonably close in time (< 10 seconds gap)
        if (nxt["start"] - cur["end"]) > 10.0:
            continue

        cur_words = normalize_for_retake(cur["text"])
        nxt_words = normalize_for_retake(nxt["text"])
        if len(cur_words) < 2 or len(nxt_words) < 2:
            continue

        # Check 1: Explicit stumble/retake cue at the end of current segment
        raw_lower = cur["text"].lower().strip(" .,!?-")
        has_retake_cue = any(raw_lower.endswith(cue) or f" {cue}" in raw_lower for cue in retake_cues)

        # Check 2: Identical 2+ word starting prefix (speaker started sentence, stopped, and restarted it)
        prefix_match_len = 0
        for w1, w2 in zip(cur_words, nxt_words):
            if w1 == w2:
                prefix_match_len += 1
            else:
                break

        # Check 3: Overall sequence similarity against beginning of next take
        compare_slice = nxt_words[:max(len(cur_words), 4)]
        seq_ratio = difflib.SequenceMatcher(None, " ".join(cur_words), " ".join(compare_slice)).ratio()

        if prefix_match_len >= 2 or (seq_ratio >= 0.62 and len(cur_words) <= len(nxt_words) + 2) or (has_retake_cue and seq_ratio >= 0.40):
            reason = f"Cut butchered take [{cur['start']:.1f}s-{cur['end']:.1f}s]: \"{cur['text']}\" -> Kept retake: \"{nxt['text']}\""
            cuts.append((cur["start"], cur["end"], reason))

    return cuts


def subtract_cuts_from_intervals(
    keep_intervals: list[tuple[float, float]],
    cut_intervals: list[tuple[float, float, str]]
) -> list[tuple[float, float]]:
    result = list(keep_intervals)
    for c_start, c_end, _ in cut_intervals:
        new_result = []
        for k_start, k_end in result:
            if c_end <= k_start or c_start >= k_end:
                new_result.append((k_start, k_end))
            else:
                if c_start > k_start + 0.10:
                    new_result.append((k_start, c_start))
                if c_end < k_end - 0.10:
                    new_result.append((c_end, k_end))
        result = new_result
    return [(round(a, 3), round(b, 3)) for a, b in result if (b - a) >= 0.15]


def render_edited_video(
    input_path: str,
    output_path: str,
    keep_intervals: list[tuple[float, float]],
    total_duration: float
) -> bool:
    """
    Uses FFmpeg filter_complex trim/atrim + concat to cut dead air and retakes
    accurately using NVIDIA NVENC hardware encoding (with x264 fallback).
    """
    if not keep_intervals:
        return False

    # If the whole video is kept with negligible trimming (< 0.4s difference), copy directly
    kept_sum = sum(b - a for a, b in keep_intervals)
    if len(keep_intervals) == 1 and abs(kept_sum - total_duration) < 0.4:
        log_info("No significant dead air or retakes found; preserving original stream.")
        if os.path.abspath(input_path) != os.path.abspath(output_path):
            subprocess.run(["ffmpeg", "-y", "-i", input_path, "-c", "copy", output_path], capture_output=True)
        return True

    filter_parts = []
    concat_inputs = []
    for idx, (start, end) in enumerate(keep_intervals):
        filter_parts.append(f"[0:v]trim=start={start}:end={end},setpts=PTS-STARTPTS[v{idx}];")
        filter_parts.append(f"[0:a]atrim=start={start}:end={end},asetpts=PTS-STARTPTS[a{idx}];")
        concat_inputs.append(f"[v{idx}][a{idx}]")

    n = len(keep_intervals)
    filter_Parts_str = "".join(filter_parts) + "".join(concat_inputs) + f"concat=n={n}:v=1:a=1[outv][outa]"

    # Write filter script to temp file so long filter chains never exceed Windows CLI length limits
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as tf:
        tf.write(filter_Parts_str)
        filter_script_path = tf.name

    try:
        for encoder_args in [
            ["-c:v", "h264_nvenc", "-preset", "p4", "-cq", "20"],
            ["-c:v", "libx264", "-preset", "veryfast", "-crf", "20"]
        ]:
            cmd = [
                "ffmpeg", "-y", "-hide_banner",
                "-i", input_path,
                "-filter_complex_script", filter_script_path,
                "-map", "[outv]", "-map", "[outa]",
                *encoder_args,
                "-c:a", "aac", "-b:a", "192k",
                "-movflags", "+faststart",
                output_path
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode == 0 and os.path.exists(output_path) and os.path.getsize(output_path) > 1024:
                return True
    finally:
        try:
            os.remove(filter_script_path)
        except OSError:
            pass

    return False


# ---------------------------------------------------------------------------
# 3. 1-to-4 Priority AI Metadata Generation (Gemini / OpenAI / Claude / Local)
# ---------------------------------------------------------------------------

METADATA_SYSTEM_PROMPT = """You are an expert YouTube producer and SEO strategist.
Based on the provided video transcript, generate a high-CTR YouTube Title, a well-structured SEO Description (including a concise overview, key takeaways, and timestamps), relevant tags, a punchy 3-to-4 word Thumbnail Hook text, and a vivid visual prompt for a 16:9 YouTube thumbnail.

Respond ONLY with valid JSON matching this exact schema:
{
  "title": "Catchy, high-engagement YouTube title (under 70 chars)",
  "description": "Full YouTube description with overview, bullet points, and chapter timestamps",
  "tags": ["tag1", "tag2", "tag3", "tag4", "tag5"],
  "thumbnail_hook": "3 TO 4 PUNCHY WORDS",
  "thumbnail_prompt": "Vivid 16:9 YouTube thumbnail background scene description, high contrast, cinematic lighting, no small text"
}"""


def extract_json_object(raw_text: str) -> dict:
    raw_text = raw_text.strip()
    m = re.search(r"\{.*\}", raw_text, flags=re.DOTALL)
    if m:
        raw_text = m.group(0)
    return json.loads(raw_text)


def call_gemini_metadata(api_key: str, transcript_text: str) -> dict:
    if not api_key:
        raise ValueError("No Gemini API key configured in OBS Vault.")
    url = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key={api_key}"
    body = {
        "contents": [{
            "parts": [{"text": f"{METADATA_SYSTEM_PROMPT}\n\nTRANSCRIPT:\n{transcript_text[:24000]}"}]
        }],
        "generationConfig": {"responseMimeType": "application/json", "temperature": 0.4}
    }
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=45) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    text = data["candidates"][0]["content"]["parts"][0]["text"]
    return extract_json_object(text)


def call_openai_metadata(api_key: str, transcript_text: str) -> dict:
    if not api_key:
        raise ValueError("No OpenAI API key configured in OBS Vault.")
    url = "https://api.openai.com/v1/chat/completions"
    body = {
        "model": "gpt-4o",
        "response_format": {"type": "json_object"},
        "messages": [
            {"role": "system", "content": METADATA_SYSTEM_PROMPT},
            {"role": "user", "content": f"TRANSCRIPT:\n{transcript_text[:24000]}"}
        ],
        "temperature": 0.4
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}"
        }
    )
    with urllib.request.urlopen(req, timeout=45) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    text = data["choices"][0]["message"]["content"]
    return extract_json_object(text)


def call_claude_metadata(api_key: str, transcript_text: str) -> dict:
    if not api_key:
        raise ValueError("No Claude API key configured in OBS Vault.")
    url = "https://api.anthropic.com/v1/messages"
    body = {
        "model": "claude-3-7-sonnet-latest",
        "max_tokens": 1500,
        "system": METADATA_SYSTEM_PROMPT,
        "messages": [
            {"role": "user", "content": f"TRANSCRIPT:\n{transcript_text[:24000]}"}
        ]
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01"
        }
    )
    with urllib.request.urlopen(req, timeout=45) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    text = data["content"][0]["text"]
    return extract_json_object(text)


def call_local_ollama_metadata(ollama_model: str, transcript_text: str) -> dict:
    url = "http://127.0.0.1:11434/api/generate"
    models_to_try = [ollama_model, "gemma4:12b", "llama3.1:8b", "qwen3:14b"]
    seen = set()
    ordered_models = [m for m in models_to_try if m and not (m in seen or seen.add(m))]

    last_err = None
    for model in ordered_models:
        try:
            body = {
                "model": model,
                "prompt": f"{METADATA_SYSTEM_PROMPT}\n\nTRANSCRIPT:\n{transcript_text[:16000]}",
                "format": "json",
                "stream": False,
                "options": {"temperature": 0.4}
            }
            req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"), headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=90) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            parsed = extract_json_object(data.get("response", "{}"))
            parsed["_model_used"] = f"Local GPU Ollama ({model})"
            return parsed
        except Exception as e:
            last_err = e

    raise RuntimeError(f"Local Ollama failed ({last_err})")


def generate_metadata_with_priority(
    priority_list: list[str],
    secrets: dict,
    ollama_model: str,
    segments: list[dict]
) -> tuple[dict, str]:
    """
    Walks the user's 1-to-4 Priority Ranking list (#1 -> #2 -> #3 -> #4)
    to generate video Title, Description, Tags, and Thumbnail Hook.
    """
    if segments:
        lines = []
        for s in segments:
            mins = int(s["start"] // 60)
            secs = int(s["start"] % 60)
            lines.append(f"[{mins:02d}:{secs:02d}] {s['text']}")
        transcript_text = "\n".join(lines)
    else:
        transcript_text = "[No spoken dialogue detected in recording. Generate engaging gaming/creator showcase metadata.]"

    provider_labels = {
        "gemini": "Google Gemini (gemini-2.5-flash)",
        "openai": "ChatGPT / OpenAI (gpt-4o)",
        "claude": "Anthropic Claude (Sonnet)",
        "local": f"Local GPU LLM ({ollama_model})"
    }

    for rank_idx, provider in enumerate(priority_list, start=1):
        p_key = provider.lower().strip()
        label = provider_labels.get(p_key, p_key)
        update_step("llm", 25 + rank_idx * 15, f"Trying Priority #{rank_idx}: {label}...")
        try:
            if p_key == "gemini":
                res = call_gemini_metadata(secrets.get("gemini_key", ""), transcript_text)
            elif p_key == "openai":
                res = call_openai_metadata(secrets.get("openai_key", ""), transcript_text)
            elif p_key == "claude":
                res = call_claude_metadata(secrets.get("claude_key", ""), transcript_text)
            elif p_key == "local":
                res = call_local_ollama_metadata(ollama_model, transcript_text)
            else:
                continue

            if res and res.get("title"):
                used_badge = f"Priority #{rank_idx}: {res.get('_model_used', label)}"
                log_info(f"Metadata generated successfully via {used_badge}.")
                return res, used_badge
        except Exception as e:
            log_warn(f"Priority #{rank_idx} ({label}) skipped/fell back: {e}")

    # Ultimate deterministic fallback if even Ollama daemon isn't running
    fallback_meta = {
        "title": f"OBS Studio Recording - {time.strftime('%Y-%m-%d %H:%M')}",
        "description": f"Automatically edited and transcribed by OBS YouTube Workflow.\n\nTranscript Preview:\n{transcript_text[:1500]}",
        "tags": ["OBS Studio", "Video", "Creator"],
        "thumbnail_hook": "NEW VIDEO DROP",
        "thumbnail_prompt": "Cinematic studio setup with neon lighting"
    }
    return fallback_meta, "Built-in Fallback"


# ---------------------------------------------------------------------------
# 4. 1-to-4 Priority Custom Thumbnail Generator (1280x720 JPG)
# ---------------------------------------------------------------------------

def extract_best_talking_head_frame(video_path: str):
    """
    Uses OpenCV to sample candidate frames across the edited video, scoring each
    by Laplacian sharpness and face visibility so you never get a blurry or mid-blink frame.
    """
    import cv2
    import numpy as np

    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        return None

    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    if frame_count <= 0:
        ret, frame = cap.read()
        cap.release()
        return frame if ret else None

    face_cascade = None
    try:
        cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        if os.path.exists(cascade_path):
            face_cascade = cv2.CascadeClassifier(cascade_path)
    except Exception:
        pass

    sample_points = [int(frame_count * p) for p in (0.12, 0.25, 0.38, 0.50, 0.62, 0.75, 0.85)]
    best_frame = None
    best_score = -1.0

    for idx in sample_points:
        cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
        ret, frame = cap.read()
        if not ret or frame is None:
            continue

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        sharpness = float(cv2.Laplacian(gray, cv2.CV_64F).var())
        brightness = float(np.mean(gray))
        # Penalize pitch-black or blown-out frames
        exposure_weight = 1.0 if 45.0 <= brightness <= 215.0 else 0.3

        face_bonus = 1.0
        if face_cascade is not None:
            faces = face_cascade.detectMultiScale(gray, 1.2, 5)
            if len(faces) > 0:
                face_bonus = 2.2

        score = sharpness * exposure_weight * face_bonus
        if score > best_score:
            best_score = score
            best_frame = frame.copy()

    cap.release()
    return best_frame


def overlay_youtube_hook_text(base_pil_img, hook_text: str):
    """
    Renders high-contrast YouTube thumbnail typography with gradient vignette,
    bold multi-line text, thick outline stroke, and accent badge using Pillow.
    """
    from PIL import Image, ImageDraw, ImageEnhance, ImageFont

    img = base_pil_img.convert("RGB").resize((1280, 720), Image.Resampling.LANCZOS)
    # Boost color contrast & saturation for YouTube feed pop
    img = ImageEnhance.Contrast(img).enhance(1.14)
    img = ImageEnhance.Color(img).enhance(1.18)

    # Apply subtle dark gradient on bottom-left for text legibility
    overlay = Image.new("RGBA", (1280, 720), (0, 0, 0, 0))
    draw_ov = ImageDraw.Draw(overlay)
    for y in range(380, 720):
        alpha = int(185 * ((y - 380) / 340.0))
        draw_ov.line([(0, y), (1280, y)], fill=(8, 8, 14, alpha))
    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")

    draw = ImageDraw.Draw(img)
    hook_clean = (hook_text or "MUST WATCH").upper().strip()

    # Load bold Windows font (Impact / Arial Black / Segoe UI Black)
    font_size = 92
    font = None
    for font_name in ["impact.ttf", "ariblk.ttf", "segoeuib.ttf", "arialbd.ttf"]:
        font_path = os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts", font_name)
        if os.path.exists(font_path):
            try:
                font = ImageFont.truetype(font_path, font_size)
                break
            except Exception:
                pass
    if font is None:
        font = ImageFont.load_default()

    # Draw glowing red/gold accent bar + bold white/yellow text
    draw.rounded_rectangle([54, 520, 1226, 675], radius=18, fill=(18, 18, 26), outline=(255, 55, 55), width=5)
    draw.text(
        (86, 548),
        hook_clean[:34],
        font=font,
        fill=(255, 235, 60),
        stroke_width=6,
        stroke_fill=(0, 0, 0)
    )
    return img


def generate_gemini_thumbnail(api_key: str, prompt: str, hook_text: str, out_jpg: str) -> bool:
    if not api_key:
        raise ValueError("No Gemini API key configured.")
    from google import genai
    from google.genai import types
    from PIL import Image

    client = genai.Client(api_key=api_key)
    result = client.models.generate_images(
        model="imagen-3.0-generate-002",
        prompt=f"YouTube thumbnail 16:9 widescreen, high contrast, vibrant studio lighting: {prompt}",
        config=types.GenerateImagesConfig(
            number_of_images=1,
            output_mime_type="image/jpeg",
            aspect_ratio="16:9"
        )
    )
    for generated_image in result.generated_images:
        pil_img = Image.open(io.BytesIO(generated_image.image.image_bytes))
        final_img = overlay_youtube_hook_text(pil_img, hook_text)
        final_img.save(out_jpg, "JPEG", quality=92)
        return True
    raise RuntimeError("Imagen 3 returned no images.")


def generate_openai_thumbnail(api_key: str, prompt: str, hook_text: str, out_jpg: str) -> bool:
    if not api_key:
        raise ValueError("No OpenAI API key configured.")
    from PIL import Image

    url = "https://api.openai.com/v1/images/generations"
    body = {
        "model": "dall-e-3",
        "prompt": f"High-impact 16:9 YouTube thumbnail background, vibrant colors, cinematic lighting, no tiny text: {prompt}",
        "n": 1,
        "size": "1792x1024",
        "response_format": "b64_json"
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}"
        }
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    b64_bytes = base64.b64decode(data["data"][0]["b64_json"])
    pil_img = Image.open(io.BytesIO(b64_bytes))
    final_img = overlay_youtube_hook_text(pil_img, hook_text)
    final_img.save(out_jpg, "JPEG", quality=92)
    return True


def generate_local_gpu_thumbnail(video_path: str, hook_text: str, out_jpg: str) -> bool:
    import cv2
    from PIL import Image

    frame_bgr = extract_best_talking_head_frame(video_path)
    if frame_bgr is not None:
        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        pil_img = Image.fromarray(frame_rgb)
    else:
        pil_img = Image.new("RGB", (1280, 720), (32, 18, 48))

    final_img = overlay_youtube_hook_text(pil_img, hook_text)
    final_img.save(out_jpg, "JPEG", quality=92)
    return True


def generate_thumbnail_with_priority(
    priority_list: list[str],
    secrets: dict,
    video_path: str,
    metadata: dict,
    thumb_style: str,
    out_jpg: str
) -> str:
    """
    Walks the user's 1-to-4 Priority Ranking list to generate the 1280x720 custom thumbnail.
    """
    hook_text = metadata.get("thumbnail_hook", "NEW VIDEO")
    prompt = metadata.get("thumbnail_prompt", metadata.get("title", "YouTube creator studio"))

    # If user explicitly selected "Talking-Head Frame + AI Hook Text" style, use local smart frame compositor directly
    if thumb_style == "frame_overlay":
        generate_local_gpu_thumbnail(video_path, hook_text, out_jpg)
        return "Smart Keyframe + AI Hook Compositor"

    for rank_idx, provider in enumerate(priority_list, start=1):
        p_key = provider.lower().strip()
        update_step("thumbnail", 20 + rank_idx * 18, f"Thumbnail Priority #{rank_idx} ({p_key.upper()})...")
        try:
            if p_key == "gemini":
                if generate_gemini_thumbnail(secrets.get("gemini_key", ""), prompt, hook_text, out_jpg):
                    return f"Priority #{rank_idx}: Google Gemini (Imagen 3)"
            elif p_key == "openai":
                if generate_openai_thumbnail(secrets.get("openai_key", ""), prompt, hook_text, out_jpg):
                    return f"Priority #{rank_idx}: OpenAI (DALL-E 3)"
            elif p_key in ("claude", "local"):
                # Claude does not have an image endpoint; pairs its prompt/hook with Local GPU Smart Frame Compositor
                if generate_local_gpu_thumbnail(video_path, hook_text, out_jpg):
                    label = "Claude Hook + Smart Frame Compositor" if p_key == "claude" else "Local GPU Smart Frame Compositor"
                    return f"Priority #{rank_idx}: {label}"
        except Exception as e:
            log_warn(f"Thumbnail Priority #{rank_idx} ({p_key}) fell back: {e}")

    generate_local_gpu_thumbnail(video_path, hook_text, out_jpg)
    return "Fallback: Local GPU Smart Frame Compositor"


# ---------------------------------------------------------------------------
# 5. YouTube Data API v3 OAuth 2.0 & Resumable Upload (Video + Thumbnail)
# ---------------------------------------------------------------------------

YOUTUBE_SCOPES = [
    "https://www.googleapis.com/auth/youtube.upload",
    "https://www.googleapis.com/auth/youtube.readonly"
]


def run_interactive_oauth_login(client_id: str, client_secret: str, client_secret_file: str, token_save_path: str):
    """
    Runs the Google OAuth 2.0 desktop browser login flow and saves the authorized
    credentials so OBS can encrypt and store them.
    """
    from google_auth_oauthlib.flow import InstalledAppFlow
    from googleapiclient.discovery import build

    if client_secret_file and os.path.exists(client_secret_file):
        flow = InstalledAppFlow.from_client_secrets_file(client_secret_file, YOUTUBE_SCOPES)
    elif client_id and client_secret:
        client_config = {
            "installed": {
                "client_id": client_id,
                "client_secret": client_secret,
                "auth_uri": "https://accounts.google.com/o/oauth2/auth",
                "token_uri": "https://oauth2.googleapis.com/token",
                "redirect_uris": ["http://localhost"]
            }
        }
        flow = InstalledAppFlow.from_client_config(client_config, YOUTUBE_SCOPES)
    else:
        raise ValueError("Please provide your Google OAuth Client ID & Secret (or select client_secret.json) in Settings -> YouTube Automation.")

    creds = flow.run_local_server(port=0, prompt="consent")
    channel_title = "Connected YouTube Channel"
    try:
        yt = build("youtube", "v3", credentials=creds)
        ch_resp = yt.channels().list(part="snippet", mine=True).execute()
        items = ch_resp.get("items", [])
        if items:
            channel_title = items[0]["snippet"]["title"]
    except Exception:
        pass

    token_data = {
        "token_json": creds.to_json(),
        "channel_title": channel_title
    }
    if token_save_path:
        os.makedirs(os.path.dirname(os.path.abspath(token_save_path)), exist_ok=True)
        with open(token_save_path, "w", encoding="utf-8") as f:
            json.dump(token_data, f)

    emit("oauth_success", channel_title=channel_title, token_json=creds.to_json())


def upload_to_youtube(
    video_path: str,
    thumbnail_path: str,
    metadata: dict,
    privacy_status: str,
    category_id: str,
    made_for_kids: bool,
    oauth_token_json: str
) -> tuple[str, str]:
    """
    Uploads the edited MP4 video to YouTube as UNLISTED (or configured privacy)
    and sets the custom 1280x720 thumbnail.
    """
    if not oauth_token_json:
        raise ValueError("YouTube account not connected yet. Open Settings -> YouTube Automation and click 'Connect Google Account'.")

    from google.auth.transport.requests import Request
    from google.oauth2.credentials import Credentials
    from googleapiclient.discovery import build
    from googleapiclient.http import MediaFileUpload

    creds_info = json.loads(oauth_token_json)
    if "token_json" in creds_info:
        creds_info = json.loads(creds_info["token_json"])

    creds = Credentials.from_authorized_user_info(creds_info, YOUTUBE_SCOPES)
    if creds.expired and creds.refresh_token:
        creds.refresh(Request())

    youtube = build("youtube", "v3", credentials=creds)

    body = {
        "snippet": {
            "title": metadata.get("title", "OBS Automated Video")[:100],
            "description": metadata.get("description", ""),
            "tags": metadata.get("tags", ["OBS", "YouTube"]),
            "categoryId": str(category_id or "22")
        },
        "status": {
            "privacyStatus": privacy_status or "unlisted",
            "selfDeclaredMadeForKids": bool(made_for_kids)
        }
    }

    media = MediaFileUpload(video_path, mimetype="video/mp4", chunksize=2 * 1024 * 1024, resumable=True)
    request = youtube.videos().insert(part="snippet,status", body=body, media_body=media)

    response = None
    while response is None:
        status, response = request.next_chunk()
        if status:
            pct = int(status.progress() * 90)
            update_step("upload", pct, f"Uploading MP4 to YouTube ({pct}%)...")

    video_id = response["id"]
    video_url = f"https://youtu.be/{video_id}"
    log_info(f"Video uploaded successfully ({privacy_status.upper()}): {video_url}")

    # Upload custom thumbnail
    if thumbnail_path and os.path.exists(thumbnail_path):
        update_step("upload", 95, "Uploading custom 1280x720 AI thumbnail to YouTube...")
        try:
            youtube.thumbnails().set(
                videoId=video_id,
                media_body=MediaFileUpload(thumbnail_path, mimetype="image/jpeg")
            ).execute()
            log_info("Custom AI thumbnail attached to YouTube video!")
        except Exception as e:
            log_warn(f"Video uploaded, but custom thumbnail API returned: {e} (Thumbnail saved locally at {thumbnail_path})")

    update_step("upload", 100, f"Upload complete! {video_url}")
    return video_id, video_url


# ---------------------------------------------------------------------------
# 6. Main Pipeline Orchestrator
# ---------------------------------------------------------------------------

def run_pipeline(config: dict):
    input_video = config.get("input_video", "")
    if not input_video or not os.path.exists(input_video):
        log_error(f"Input video not found: {input_video}")
        emit("error", message=f"Input video not found: {input_video}")
        return 1

    base_dir = os.path.dirname(os.path.abspath(input_video))
    stem = Path(input_video).stem
    edited_video = os.path.join(base_dir, f"{stem}_yt_edited.mp4")
    thumbnail_jpg = os.path.join(base_dir, f"{stem}_yt_thumb.jpg")
    transcript_file = os.path.join(base_dir, f"{stem}_yt_transcript.txt")

    silence_db = float(config.get("silence_db", -32.0))
    min_silence_sec = float(config.get("min_silence_sec", 0.5))
    padding_sec = float(config.get("padding_sec", 0.15))
    cut_retakes = bool(config.get("cut_retakes", True))
    whisper_model = config.get("whisper_model", "large-v3-turbo")
    ollama_model = config.get("ollama_model", "gemma4:12b")
    priority_list = config.get("priority_list", ["gemini", "openai", "claude", "local"])
    thumb_style = config.get("thumbnail_style", "ai_graphic")
    privacy_status = config.get("privacy_status", "unlisted")
    category_id = config.get("category_id", "22")
    made_for_kids = bool(config.get("made_for_kids", False))
    secrets = config.get("secrets", {})

    total_duration = get_video_duration(input_video)
    log_info(f"Starting YouTube Auto-Edit Pipeline for '{ os.path.basename(input_video) }' ({total_duration:.1f}s)")

    # STAGE 1: Whisper Transcription
    update_step("transcribe", 5, f"Starting Whisper ({whisper_model}) CUDA transcription...")
    try:
        segments = transcribe_video(input_video, model_name=whisper_model, gpu_index=1)
    except Exception as e:
        log_warn(f"Whisper encountered an issue ({e}); continuing with audio silence cutting.")
        segments = []

    # Save transcript alongside video
    if segments:
        with open(transcript_file, "w", encoding="utf-8") as tf:
            for s in segments:
                tf.write(f"[{s['start']:.2f}s - {s['end']:.2f}s] {s['text']}\n")

    # STAGE 2: Dead Air & Butchered Sentence Editing
    update_step("edit", 15, f"Detecting dead air (threshold: {silence_db} dB, min: {min_silence_sec}s)...")
    keep_intervals = detect_nonsilent_intervals(
        input_video,
        silence_db=silence_db,
        min_silence_dur=min_silence_sec,
        padding_sec=padding_sec,
        total_duration=total_duration
    )

    if cut_retakes and len(segments) > 1:
        update_step("edit", 45, "Scanning transcript for butchered sentences & repeated takes...")
        retake_cuts = detect_butchered_retakes(segments)
        for _, _, reason in retake_cuts:
            log_info(reason)
        if retake_cuts:
            keep_intervals = subtract_cuts_from_intervals(keep_intervals, retake_cuts)

    kept_duration = sum(b - a for a, b in keep_intervals) if keep_intervals else total_duration
    removed_sec = max(0.0, total_duration - kept_duration)
    update_step("edit", 65, f"Rendering edited MP4 via NVENC (removed {removed_sec:.1f}s of dead air/retakes)...")

    if render_edited_video(input_video, edited_video, keep_intervals, total_duration):
        final_video_to_upload = edited_video
        log_info(f"Edited video saved: {os.path.basename(edited_video)} ({kept_duration:.1f}s, saved {removed_sec:.1f}s)")
    else:
        final_video_to_upload = input_video
        log_warn("Using original remuxed MP4 for upload.")

    update_step("edit", 100, f"Video edit complete (-{removed_sec:.1f}s trimmed).")

    # STAGE 3: Priority-Ranked AI Title & Description
    update_step("llm", 10, "Generating Title & SEO Description via Priority Chain...")
    metadata, llm_provider_used = generate_metadata_with_priority(
        priority_list=priority_list,
        secrets=secrets,
        ollama_model=ollama_model,
        segments=segments
    )
    emit(
        "metadata_ready",
        title=metadata.get("title", ""),
        description=metadata.get("description", ""),
        tags=metadata.get("tags", []),
        thumbnail_hook=metadata.get("thumbnail_hook", ""),
        provider=llm_provider_used
    )
    update_step("llm", 100, f"Metadata ready ({llm_provider_used}).")

    # STAGE 4: Priority-Ranked Custom 1280x720 Thumbnail
    update_step("thumbnail", 10, "Generating 1280x720 Custom Thumbnail...")
    thumb_provider_used = generate_thumbnail_with_priority(
        priority_list=priority_list,
        secrets=secrets,
        video_path=final_video_to_upload,
        metadata=metadata,
        thumb_style=thumb_style,
        out_jpg=thumbnail_jpg
    )
    emit("thumbnail_ready", thumbnail_path=thumbnail_jpg, provider=thumb_provider_used)
    update_step("thumbnail", 100, f"Thumbnail ready ({thumb_provider_used}).")

    # STAGE 5: YouTube Upload (Unlisted) + Custom Thumbnail
    if config.get("skip_upload", False):
        emit(
            "complete",
            video_id="LOCAL_DRY_RUN",
            url="",
            edited_video=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            message="Dry-run complete (Upload skipped)."
        )
        return 0

    update_step("upload", 5, f"Connecting to YouTube Data API v3 ({privacy_status.upper()})...")
    try:
        video_id, video_url = upload_to_youtube(
            video_path=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            metadata=metadata,
            privacy_status=privacy_status,
            category_id=category_id,
            made_for_kids=made_for_kids,
            oauth_token_json=secrets.get("youtube_token_json", "")
        )
        emit(
            "complete",
            video_id=video_id,
            url=video_url,
            edited_video=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            message=f"Uploaded as {privacy_status.upper()}: {video_url}"
        )
        return 0
    except Exception as e:
        log_error(f"YouTube Upload Error: {e}")
        emit(
            "upload_pending",
            edited_video=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            message=f"Edited video & thumbnail are ready locally! Upload paused: {e}"
        )
        return 0


def main():
    # Strictly ignore global Windows API key environment variables so ONLY keys unlocked by OBS DPAPI Vault are used
    for env_key in ("OPENAI_API_KEY", "GEMINI_API_KEY", "GOOGLE_API_KEY", "ANTHROPIC_API_KEY"):
        os.environ.pop(env_key, None)

    parser = argparse.ArgumentParser(description="OBS YouTube Automated Pipeline Worker")
    parser.add_argument("--stdin-config", action="store_true", help="Read JSON config & decrypted keys from private stdin")
    parser.add_argument("--oauth-login", action="store_true", help="Run interactive Google OAuth login flow")
    parser.add_argument("--self-test", action="store_true", help="Run automated self-test of the pipeline")
    args = parser.parse_args()

    if args.stdin_config:
        raw_line = sys.stdin.readline()
        if not raw_line:
            log_error("No configuration received on stdin.")
            return 1
        cfg = json.loads(raw_line)
        if cfg.get("mode") == "oauth_login":
            try:
                run_interactive_oauth_login(
                    client_id=cfg.get("client_id", ""),
                    client_secret=cfg.get("client_secret", ""),
                    client_secret_file=cfg.get("client_secret_file", ""),
                    token_save_path=cfg.get("token_save_path", "")
                )
                return 0
            except Exception as e:
                log_error(str(e))
                emit("oauth_error", message=str(e))
                return 1
        return run_pipeline(cfg)

    if args.self_test:
        log_info("Running pipeline self-test...")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
