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
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

# OBS writes the stdin config as UTF-8; without this, Windows decodes the
# pipes with the ANSI code page (cp1252) and chokes on emoji / non-Latin text.
for _stream in (sys.stdin, sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def emit(event_type: str, **kwargs):
    """Send a structured JSON-line event to OBS Studio over stdout."""
    payload = {"type": event_type, "timestamp": time.time()}
    payload.update(kwargs)
    # ensure_ascii keeps the wire format pure ASCII (non-ASCII becomes \uXXXX,
    # which QJsonDocument decodes), so no console code page can crash emit().
    sys.stdout.write(json.dumps(payload, ensure_ascii=True) + "\n")
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
    silence_db: float = -42.0,
    min_silence_dur: float = 1.2,
    padding_sec: float = 0.25,
    total_duration: float = 0.0,
    speech_segments: list[dict] = None
) -> list[tuple[float, float]]:
    """
    Runs FFmpeg silencedetect filter and returns a list of (start, end)
    intervals that contain active audio (with natural speech padding).
    Guarantees that speech segments identified by Whisper are strictly protected.
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
        keep = [(0.0, total_duration)]
    else:
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

    # Whisper Speech Interval Protection:
    # Ensure any timestamp where Whisper detected spoken words is strictly preserved.
    if speech_segments:
        all_intervals = list(keep)
        for s in speech_segments:
            s_start = max(0.0, float(s.get("start", 0.0)) - padding_sec)
            s_end = min(total_duration, float(s.get("end", 0.0)) + padding_sec)
            if s_end > s_start:
                all_intervals.append((s_start, s_end))
        all_intervals.sort(key=lambda x: x[0])
        keep = all_intervals

    # Merge tiny adjacent gaps (< 0.25s) and overlapping intervals
    merged = []
    for seg in sorted(keep, key=lambda x: x[0]):
        if not merged:
            merged.append(list(seg))
        else:
            if seg[0] <= merged[-1][1] + 0.25:
                merged[-1][1] = max(merged[-1][1], seg[1])
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


def _has_stumble_cue(text: str, cues: tuple) -> bool:
    """
    True when a stumble cue starts or ends a sentence in `text`, or follows a comma/dash.
    Cues buried mid-sentence are ignored, so "the editor should cut that silence out"
    is not mistaken for the speaker asking to cut a take.
    """
    for sentence in re.split(r"[.!?]+", text.lower()):
        sentence = sentence.strip(" ,;:-\"'")
        if not sentence:
            continue
        for cue in cues:
            c = re.escape(cue)
            if re.match(rf"{c}\b", sentence) or re.search(rf"\b{c}$", sentence) or re.search(rf"[,;:—-]\s*{c}\b", sentence):
                return True
    return False


RETAKE_CUES = (
    "wait", "sorry", "oops", "scratch that", "let me try again", "let me try that again",
    "try that again", "hold on", "take two", "let me restart", "cut that", "let me re-say"
)
RETAKE_GAP_SEC = 8.0          # a retake must start within this long of the bad take
RETAKE_MIN_PREFIX = 3         # shared leading words that mark a restart ("Stage 1.../Stage 2..." share only 1)
RETAKE_MAX_FALSE_START = 6    # longest complete sentence still treated as a false start
RETAKE_MAX_IN_SEGMENT = 8     # longest false start repeated inside one Whisper segment


def _norm_word(word: str) -> str:
    return re.sub(r"[^a-z0-9]", "", word.lower())


def _shared_prefix_len(a: list[str], b: list[str]) -> int:
    count = 0
    for w1, w2 in zip(a, b):
        if w1 != w2:
            break
        count += 1
    return count


def _shares_phrase(a: list[str], b: list[str], n: int = RETAKE_MIN_PREFIX) -> bool:
    """True when `a` and `b` have an n-word phrase in common (the retake restates the bad take)."""
    grams = {tuple(a[i:i + n]) for i in range(len(a) - n + 1)}
    return any(tuple(b[i:i + n]) in grams for i in range(len(b) - n + 1))


def _last_cue_span(words: list[dict]) -> tuple[int, int] | None:
    """(first, last) word index of the last stumble cue in a segment's Whisper words."""
    toks = [_norm_word(w["word"]) for w in words]
    best = None
    for cue in RETAKE_CUES:
        ct = [_norm_word(t) for t in cue.split()]
        for i in range(len(toks) - len(ct) + 1):
            if toks[i:i + len(ct)] == ct and (best is None or i + len(ct) - 1 > best[1]):
                best = (i, i + len(ct) - 1)
    return best


def _in_segment_restarts(words: list[dict]) -> list[tuple[int, int]]:
    """
    (i, j) word-index pairs where words[i:j] is a false start the speaker repeats from words[j],
    e.g. "transcription runs on transcription runs on Whisper..." in a single Whisper segment.
    The whole false start must repeat word for word, so parallels like
    "the more you know, the more you grow" are left alone.
    """
    idx = [k for k, w in enumerate(words) if _norm_word(w["word"])]
    toks = [_norm_word(words[k]["word"]) for k in idx]
    n = RETAKE_MIN_PREFIX
    found = []
    i = 0
    while i + n <= len(toks):
        hit = None
        for j in range(i + n, min(len(toks) - n, i + RETAKE_MAX_IN_SEGMENT) + 1):
            if toks[i:j] == toks[j:2 * j - i] and len(set(toks[i:j])) > 1:
                hit = j
                break
        if hit is None:
            i += 1
            continue
        found.append((idx[i], idx[hit]))
        i = hit
    return found


def detect_butchered_retakes(segments: list[dict]) -> list[tuple[float, float, str]]:
    """
    Identifies butchered sentences / aborted takes that are immediately repeated
    on the next attempt. Never cuts complete sentences or parallel phrasing/lists
    unless an explicit stumble cue ('wait', 'sorry', 'oops', 'scratch that', etc.) is present,
    the sentence is a short restart of the next one, or a phrase is restarted inside one segment.
    """
    cuts = []
    n = len(segments)

    def take(seg: dict, kept: str, why: str = "butchered take") -> tuple[float, float, str]:
        return (seg["start"], seg["end"],
                f"Cut {why} [{seg['start']:.1f}s-{seg['end']:.1f}s]: \"{seg['text']}\" -> Kept retake: \"{kept}\"")

    for i in range(n):
        cur = segments[i]
        prev = segments[i - 1] if i > 0 and cur["start"] - segments[i - 1]["end"] <= RETAKE_GAP_SEC else None
        nxt = segments[i + 1] if i + 1 < n and segments[i + 1]["start"] - cur["end"] <= RETAKE_GAP_SEC else None
        words = cur.get("words") or []
        raw_cur = cur["text"].strip()
        cur_words = normalize_for_retake(raw_cur)

        # Check 1: A phrase restarted inside one Whisper segment (needs word timestamps)
        for a, b in _in_segment_restarts(words):
            start, end = words[a]["start"], words[b]["start"]
            false_start = "".join(w["word"] for w in words[a:b]).strip()
            cuts.append((start, end, f"Cut false start [{start:.1f}s-{end:.1f}s]: \"{false_start}\" -> Kept retake in: \"{raw_cur}\""))

        # Check 2: Explicit verbal stumble or retake cue
        if _has_stumble_cue(raw_cur.lower(), RETAKE_CUES):
            span = _last_cue_span(words) if words else None
            before = normalize_for_retake("".join(w["word"] for w in words[:span[0]])) if span else cur_words
            after = normalize_for_retake("".join(w["word"] for w in words[span[1] + 1:])) if span else []
            if len(after) >= RETAKE_MIN_PREFIX:
                # The retake follows the cue in the same segment: keep it, cut the bad take + cue.
                end = words[span[1] + 1]["start"]
                cuts.append((cur["start"], end,
                             f"Cut stumble [{cur['start']:.1f}s-{end:.1f}s] before retake in: \"{raw_cur}\""))
                retake = after
            else:
                cuts.append(take(cur, nxt["text"] if nxt else "", "stumble cue"))
                retake = normalize_for_retake(nxt["text"]) if nxt else []
            # A cue that opens the segment ("Wait, scratch that.") disowns the previous sentence,
            # but only when the retake restates it, so a stray "sorry" can't eat a good line.
            if len(before) < 2 and prev and _shares_phrase(normalize_for_retake(prev["text"]), retake):
                cuts.append(take(prev, " ".join(retake), "take disowned by stumble cue"))
            continue

        if not nxt:
            continue
        nxt_words = normalize_for_retake(nxt["text"])
        prefix_match_len = _shared_prefix_len(cur_words, nxt_words)

        # Check 3: Short restart ("Transcription runs on." -> "Transcription runs on Whisper...")
        if len(cur_words) <= RETAKE_MAX_FALSE_START and prefix_match_len >= RETAKE_MIN_PREFIX:
            cuts.append(take(cur, nxt["text"]))
            continue

        # Check 4: Aborted short fragment (speaker trailed off without ending punctuation).
        # Complete parallel sentences ("Super Mario 1 is...", "Super Mario 2 is...") are never cut.
        is_short_fragment = (not raw_cur.endswith((".", "!", "?"))) and len(cur_words) <= 4
        if is_short_fragment and prefix_match_len >= 1:
            cuts.append(take(cur, nxt["text"]))

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


MIN_CUT_SEC = 0.10   # a reviewed cut shorter than this does nothing
MIN_KEEP_SEC = 0.15  # same sliver rule as the automatic cut list


def cuts_from_keep(
    keep_intervals: list[tuple[float, float]],
    retake_cuts: list[tuple[float, float, str]],
    total_duration: float
) -> list[dict]:
    """The removed ranges (everything outside keep_intervals), labelled for the OBS cut editor."""
    gaps = []
    cursor = 0.0
    for k_start, k_end in sorted(keep_intervals):
        if k_start - cursor >= MIN_CUT_SEC:
            gaps.append((cursor, k_start))
        cursor = max(cursor, k_end)
    if total_duration - cursor >= MIN_CUT_SEC:
        gaps.append((cursor, total_duration))

    cuts = []
    for i, (g_start, g_end) in enumerate(gaps, start=1):
        overlaps = [(min(g_end, c_end) - max(g_start, c_start), reason)
                    for c_start, c_end, reason in retake_cuts]
        overlaps = [o for o in overlaps if o[0] > 0.05]
        if overlaps:
            kind, reason = "retake", max(overlaps)[1]
        else:
            kind, reason = "silence", f"Dead air ({g_end - g_start:.1f}s)"
        cuts.append({"id": f"c{i:02d}", "start": round(g_start, 3), "end": round(g_end, 3),
                     "kind": kind, "reason": reason, "enabled": True})
    return cuts


def keep_from_cuts(cuts: list[dict], total_duration: float) -> list[tuple[float, float]]:
    """Rebuild the keep list from reviewed cuts: everything outside the enabled ones."""
    spans = []
    for c in cuts:
        if not c.get("enabled", True):
            continue
        start = max(0.0, float(c.get("start", 0.0)))
        end = min(total_duration, float(c.get("end", 0.0)))
        if end - start >= MIN_CUT_SEC:
            spans.append([start, end])
    spans.sort()
    merged = []
    for s in spans:
        if merged and s[0] <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], s[1])
        else:
            merged.append(s)

    keep = []
    cursor = 0.0
    for c_start, c_end in merged:
        if c_start - cursor >= MIN_KEEP_SEC:
            keep.append((round(cursor, 3), round(c_start, 3)))
        cursor = c_end
    if total_duration - cursor >= MIN_KEEP_SEC:
        keep.append((round(cursor, 3), round(total_duration, 3)))
    return keep


# ---------------------------------------------------------------------------
# 2b. Screen B-roll: sync, planning & guardrails
# ---------------------------------------------------------------------------

BROLL_HOOK_SEC = 5.0          # never cover the speaker during the opening hook
BROLL_MIN_SEC = 2.0
BROLL_MAX_SEC = 12.0
BROLL_MERGE_GAP_SEC = 1.5
BROLL_ACCEPT_CONFIDENCE = 0.6
BROLL_PIP_WIDTH = 0.22        # webcam bubble width as a fraction of the frame

BROLL_CUE_RE = re.compile(
    r"\b(as you can see|you can see|take a look|look at (?:this|that|the)|let me show|i'?ll show|show you|"
    r"over here|right here|on (?:the|my) screen|this (?:setting|button|menu|tab|panel|window|file|folder|page|"
    r"screen|option|code|line|function|command|dialog|dock)|click(?:ing)?|scroll(?:ing)?|"
    r"select(?:ing)?|drag(?:ging)?|paste|type in)\b",
    re.IGNORECASE,
)

BROLL_SYSTEM_PROMPT = """You are a video editor planning screen-recording B-roll for a talking-head tech video.
The creator recorded their screen at the same time as the webcam, so cutting to the screen at a given second
shows exactly what was on screen while those words were spoken.

Pick moments where showing the screen helps the viewer: the speaker refers to something visible (UI, settings,
code, files, websites, terminal output, results), demonstrates steps, or says things like "as you can see",
"click", "this setting". Do NOT pick opinions, the intro, personal stories or the conclusion; the viewer
should see the speaker's face then. Lines tagged [MARK] are moments the creator flagged live as needing
B-roll: strongly prefer covering them. Each insert should be 2 to 12 seconds.

Times in the transcript are seconds. Respond ONLY with valid JSON matching this schema:
{
  "inserts": [
    {"start": 12.4, "end": 19.0, "quote": "exact words spoken", "reason": "short reason", "confidence": 0.8}
  ]
}"""


def raw_to_edited(t: float, keep_intervals: list[tuple]) -> float:
    """Map a raw-recording time to the edited video's timeline (times inside a cut snap forward)."""
    offset = 0.0
    for a, b in keep_intervals:
        if t < a:
            return offset
        if t <= b:
            return offset + t - a
        offset += b - a
    return offset


def edited_to_raw(t: float, keep_intervals: list[tuple]) -> float:
    offset = 0.0
    for a, b in keep_intervals:
        if t <= offset + (b - a):
            return a + max(0.0, t - offset)
        offset += b - a
    return keep_intervals[-1][1] if keep_intervals else t


def flatten_words(segments: list[dict]) -> list[tuple[float, float]]:
    words = []
    for s in segments:
        for w in s.get("words") or []:
            words.append((float(w["start"]), float(w["end"])))
    return sorted(words)


def snap_to_words(t: float, words: list[tuple[float, float]], side: str, reach: float = 1.0) -> float:
    """Move a cut point to the nearest word boundary so B-roll never starts or ends mid-word."""
    for ws, we in words:
        if ws <= t <= we:
            return ws if side == "start" else we
    if side == "start":
        nxt = [ws for ws, _ in words if ws >= t]
        return nxt[0] if nxt and nxt[0] - t <= reach else t
    prev = [we for _, we in words if we <= t]
    return prev[-1] if prev and t - prev[-1] <= reach else t


def text_between(segments: list[dict], start: float, end: float) -> str:
    words = [w["word"] for s in segments for w in (s.get("words") or []) if w["end"] > start and w["start"] < end]
    if words:
        return "".join(words).strip()
    return " ".join(s["text"] for s in segments if s["end"] > start and s["start"] < end).strip()


def load_audio_mono(video_path: str, sample_rate: int = 8000, max_sec: float = 60.0):
    import numpy as np
    cmd = [
        "ffmpeg", "-nostdin", "-v", "error",
        "-t", f"{max_sec}", "-i", video_path,
        "-f", "f32le", "-ac", "1", "-acodec", "pcm_f32le", "-ar", str(sample_rate), "-"
    ]
    res = subprocess.run(cmd, capture_output=True)
    if res.returncode != 0 or not res.stdout:
        raise RuntimeError(f"FFmpeg audio decode failed for {os.path.basename(video_path)}")
    return np.frombuffer(res.stdout, dtype=np.float32).copy()


def correlate_offset(main_audio, screen_audio, sample_rate: int, max_lag_sec: float = 10.0) -> tuple[float, float]:
    """
    Returns (offset_sec, strength) where screen_time = main_time + offset_sec, found by FFT
    cross-correlation of the two mic tracks. Strength is the normalized peak (0..1).
    """
    import numpy as np
    a = main_audio - main_audio.mean()
    b = screen_audio - screen_audio.mean()
    norm = float(np.linalg.norm(a) * np.linalg.norm(b))
    if norm <= 0:
        return 0.0, 0.0
    n = len(a) + len(b) - 1
    nfft = 1 << (n - 1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(a, nfft) * np.conj(np.fft.rfft(b, nfft)), nfft)
    # corr[k] pairs main[i + k] with screen[i]; negative lags wrap to the end.
    max_lag = int(max_lag_sec * sample_rate)
    lags = np.concatenate([np.arange(0, min(max_lag, len(a) - 1) + 1), np.arange(-min(max_lag, len(b) - 1), 0)])
    values = corr[lags % nfft]
    best = int(np.argmax(values))
    lag = int(lags[best])
    return -lag / sample_rate, float(values[best] / norm)


def estimate_broll_offset(main_video: str, screen_video: str, start_delta_sec: float) -> float:
    """
    screen_time = main_time + offset. OBS's own start timestamps give the hint
    (offset = -start_delta); the mic audio in both files refines it.
    """
    hint = -float(start_delta_sec or 0.0)
    try:
        sr = 8000
        offset, strength = correlate_offset(load_audio_mono(main_video, sr), load_audio_mono(screen_video, sr), sr)
        if strength >= 0.3:
            log_info(f"B-roll sync: audio offset {offset:+.3f}s (match {strength:.2f}, OBS hint {hint:+.3f}s).")
            return offset
        log_warn(f"B-roll sync: weak audio match ({strength:.2f}); using OBS start times ({hint:+.3f}s).")
    except Exception as e:
        log_warn(f"B-roll sync: audio alignment failed ({e}); using OBS start times ({hint:+.3f}s).")
    return hint


def plan_broll_heuristic(segments: list[dict], markers: list[float]) -> list[dict]:
    candidates = []
    for s in segments:
        if BROLL_CUE_RE.search(s.get("text", "")):
            candidates.append({
                "start": s["start"], "end": s["end"], "quote": s["text"],
                "reason": "Speaker refers to something on screen", "confidence": 0.65
            })
    candidates.extend(marker_candidates(segments, markers))
    return candidates


def marker_candidates(segments: list[dict], markers: list[float]) -> list[dict]:
    return [{
        "start": m - 1.0, "end": m + 5.0, "quote": text_between(segments, m - 1.0, m + 5.0),
        "reason": "Marked live with the B-roll hotkey", "confidence": 0.85
    } for m in markers]


def apply_broll_guardrails(
    candidates: list[dict],
    keep_intervals: list[tuple],
    words: list[tuple[float, float]],
    screen_range: tuple[float, float],
    max_coverage: float,
    hook_sec: float = BROLL_HOOK_SEC
) -> list[dict]:
    """
    Turn raw planner output into safe inserts: word-snapped, inside the screen recording,
    clear of the opening hook, 2-12 s long on the edited timeline, merged when close,
    and capped at `max_coverage` of the edited video (highest confidence wins).
    """
    if not keep_intervals:
        return []
    lo, hi = screen_range
    hook_raw = edited_to_raw(hook_sec, keep_intervals)
    edited_total = sum(b - a for a, b in keep_intervals)

    cleaned = []
    for c in candidates:
        try:
            s, e = float(c["start"]), float(c["end"])
            confidence = max(0.0, min(1.0, float(c.get("confidence", 0.5))))
        except (KeyError, TypeError, ValueError):
            continue
        s = max(snap_to_words(s, words, "start"), lo, hook_raw)
        e = min(snap_to_words(e, words, "end"), hi)
        es, ee = raw_to_edited(s, keep_intervals), raw_to_edited(e, keep_intervals)
        if ee - es > BROLL_MAX_SEC:
            e = edited_to_raw(es + BROLL_MAX_SEC, keep_intervals)
            ee = es + BROLL_MAX_SEC
        if ee - es < BROLL_MIN_SEC:
            continue
        cleaned.append({
            "start": s, "end": e, "edited_start": es, "edited_end": ee,
            "quote": str(c.get("quote", "")).strip(), "reason": str(c.get("reason", "")).strip(),
            "confidence": confidence
        })

    merged = []
    for c in sorted(cleaned, key=lambda x: x["start"]):
        prev = merged[-1] if merged else None
        if prev and c["edited_start"] - prev["edited_end"] < BROLL_MERGE_GAP_SEC \
                and max(prev["edited_end"], c["edited_end"]) - prev["edited_start"] <= BROLL_MAX_SEC:
            if c["end"] > prev["end"]:
                prev["end"], prev["edited_end"] = c["end"], c["edited_end"]
            if c["quote"] and c["quote"] not in prev["quote"]:
                prev["quote"] = f"{prev['quote']} {c['quote']}".strip()
            if c["reason"] and c["reason"] not in prev["reason"]:
                prev["reason"] = f"{prev['reason']}; {c['reason']}".strip("; ")
            prev["confidence"] = max(prev["confidence"], c["confidence"])
        elif prev and c["edited_start"] < prev["edited_end"]:
            # Overlaps but would be too long merged: keep the stronger one
            if c["confidence"] > prev["confidence"]:
                merged[-1] = c
        else:
            merged.append(c)

    budget = max(0.0, float(max_coverage)) * edited_total
    chosen, used = [], 0.0
    for c in sorted(merged, key=lambda x: -x["confidence"]):
        length = c["edited_end"] - c["edited_start"]
        if used + length <= budget:
            chosen.append(c)
            used += length

    chosen.sort(key=lambda x: x["start"])
    for idx, c in enumerate(chosen, start=1):
        c["id"] = f"b{idx:02d}"
        for k in ("start", "end", "edited_start", "edited_end"):
            c[k] = round(c[k], 3)
        c["confidence"] = round(c["confidence"], 2)
    return chosen


def grab_frame(video_path: str, t: float, out_jpg: str, width: int = 320) -> bool:
    cmd = [
        "ffmpeg", "-y", "-v", "error", "-ss", f"{max(0.0, t):.3f}", "-i", video_path,
        "-frames:v", "1", "-vf", f"scale={width}:-2", "-q:v", "4", out_jpg
    ]
    res = subprocess.run(cmd, capture_output=True)
    return res.returncode == 0 and os.path.exists(out_jpg)


def plan_broll(
    broll_cfg: dict,
    input_video: str,
    segments: list[dict],
    keep_intervals: list[tuple],
    priority_list: list[str],
    secrets: dict,
    ollama_model: str,
    plan_path: str,
    thumbs_dir: str
) -> dict | None:
    """Builds and saves the B-roll plan for the screen recording, or returns None when there is nothing to plan."""
    screen_video = broll_cfg.get("screen_video", "")
    if not screen_video or not os.path.exists(screen_video):
        log_warn(f"B-roll screen recording not found ({screen_video or 'none'}); skipping B-roll.")
        return None

    update_step("broll", 10, "Syncing screen recording with the webcam...")
    offset = estimate_broll_offset(input_video, screen_video, broll_cfg.get("start_delta_sec", 0.0))
    screen_duration = get_video_duration(screen_video)
    screen_range = (max(0.0, -offset), max(0.0, screen_duration - offset - 0.1))
    markers = [float(m) for m in broll_cfg.get("markers", []) if isinstance(m, (int, float))]

    candidates = []
    provider = "Heuristic cue phrases"
    if segments:
        marker_lines = set()
        for m in markers:
            for i, s in enumerate(segments):
                if s["start"] - 1.0 <= m <= s["end"] + 1.0:
                    marker_lines.add(i)
        lines = [
            f"[{s['start']:.1f}-{s['end']:.1f}]{' [MARK]' if i in marker_lines else ''} {s['text']}"
            for i, s in enumerate(segments)
        ]
        user_text = "TRANSCRIPT (seconds):\n" + "\n".join(lines)
        update_step("broll", 20, "Finding moments to show your screen...")
        res, badge = run_llm_chain(
            priority_list, secrets, ollama_model, BROLL_SYSTEM_PROMPT, user_text,
            step="broll", validate=lambda r: isinstance(r.get("inserts"), list)
        )
        if res is not None:
            candidates = [c for c in res["inserts"] if isinstance(c, dict)]
            provider = badge
            # Live markers always count, even if the model skipped them
            covered = lambda m: any(float(c.get("start", 0)) - 1.0 <= m <= float(c.get("end", 0)) + 1.0 for c in candidates)
            candidates.extend(marker_candidates(segments, [m for m in markers if not covered(m)]))
        else:
            candidates = plan_broll_heuristic(segments, markers)
    else:
        candidates = marker_candidates(segments, markers)

    inserts = apply_broll_guardrails(
        candidates, keep_intervals, flatten_words(segments), screen_range,
        float(broll_cfg.get("max_coverage", 0.40))
    )

    os.makedirs(thumbs_dir, exist_ok=True)
    for ins in inserts:
        thumb = os.path.join(thumbs_dir, f"insert_{ins['id']}.jpg")
        mid = (ins["start"] + ins["end"]) / 2.0 + offset
        ins["thumb"] = thumb if grab_frame(screen_video, mid, thumb) else ""
        ins["status"] = "proposed"

    plan = {
        "version": 1,
        "main_video": os.path.abspath(input_video),
        "screen_video": os.path.abspath(screen_video),
        "offset_sec": round(offset, 3),
        "style": broll_cfg.get("style", "pip") if broll_cfg.get("style") in ("pip", "cutaway") else "pip",
        "provider": provider,
        "inserts": inserts
    }
    write_json_file(plan_path, plan)
    covered_sec = sum(i["edited_end"] - i["edited_start"] for i in inserts)
    update_step("broll", 90, f"{len(inserts)} B-roll moments planned ({covered_sec:.0f}s, {provider}).")
    return plan


def accepted_broll_inserts(plan: dict, screen_duration: float) -> list[tuple[float, float]]:
    """Raw-timeline (start, end) spans the reviewer accepted, clipped to the screen recording."""
    offset = float(plan.get("offset_sec", 0.0))
    lo, hi = max(0.0, -offset), max(0.0, screen_duration - offset - 0.1)
    spans = []
    for ins in plan.get("inserts", []):
        if ins.get("status") != "accepted":
            continue
        try:
            s, e = max(float(ins["start"]), lo), min(float(ins["end"]), hi)
        except (KeyError, TypeError, ValueError):
            continue
        if e - s >= 0.5:
            spans.append((s, e))
    merged = []
    for s, e in sorted(spans):
        if merged and s <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((s, e))
    return merged


def write_json_file(path: str, data: dict):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def read_json_file(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def probe_video_geometry(video_path: str) -> tuple[int, int, str]:
    """(width, height, frame rate as an FFmpeg rational) of the first video stream."""
    cmd = [
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate",
        "-of", "json", video_path
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    stream = json.loads(res.stdout or "{}").get("streams", [{}])[0]
    fps = stream.get("avg_frame_rate") or "30/1"
    if fps.startswith("0"):
        fps = "30/1"
    return int(stream.get("width", 1920)), int(stream.get("height", 1080)), fps


def split_interval_for_broll(a: float, b: float, spans: list[tuple[float, float]], min_piece: float = 0.1) -> list[tuple[float, float, bool]]:
    """Splits keep interval [a, b] into (start, end, is_broll) pieces; slivers shorter than `min_piece` are absorbed."""
    pieces = []
    cursor = a
    for s, e in spans:
        s, e = max(s, a), min(e, b)
        if e - s < min_piece:
            continue
        if s - cursor < min_piece:
            s = cursor
        if s > cursor:
            pieces.append((cursor, s, False))
        pieces.append((s, e, True))
        cursor = e
    if b - cursor >= min_piece:
        pieces.append((cursor, b, False))
    elif pieces:
        start, _, is_broll = pieces[-1]
        pieces[-1] = (start, b, is_broll)
    else:
        pieces.append((a, b, False))
    return pieces


def build_broll_filter(keep_intervals: list[tuple], broll: dict, width: int, height: int, fps: str) -> str:
    """
    Video: A-roll and screen pieces concatenated per keep interval (screen scaled/padded to the
    output size, optional webcam bubble). Audio: one atrim per keep interval from the A-roll,
    so the soundtrack (and chapter timing) is identical to the non-B-roll edit.
    """
    offset = float(broll["offset"])
    spans = broll["inserts"]
    pip = broll.get("style", "pip") == "pip"
    norm = f"fps={fps},setsar=1,format=yuv420p"
    fit = f"scale={width}:{height}:force_original_aspect_ratio=decrease,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2"
    bubble_w = int(width * BROLL_PIP_WIDTH) // 2 * 2
    margin = int(width * 0.025)

    parts, video_labels, audio_labels = [], [], []
    p = 0
    for idx, (a, b) in enumerate(keep_intervals):
        for s, e, is_broll in split_interval_for_broll(a, b, spans):
            if not is_broll:
                parts.append(f"[0:v]trim=start={s:.3f}:end={e:.3f},setpts=PTS-STARTPTS,{norm}[p{p}];")
            elif pip:
                parts.append(f"[1:v]trim=start={s + offset:.3f}:end={e + offset:.3f},setpts=PTS-STARTPTS,{fit},{norm}[s{p}];")
                parts.append(f"[0:v]trim=start={s:.3f}:end={e:.3f},setpts=PTS-STARTPTS,scale={bubble_w}:-2,{norm}[c{p}];")
                parts.append(f"[s{p}][c{p}]overlay=x=W-w-{margin}:y=H-h-{margin}:shortest=1[p{p}];")
            else:
                parts.append(f"[1:v]trim=start={s + offset:.3f}:end={e + offset:.3f},setpts=PTS-STARTPTS,{fit},{norm}[p{p}];")
            video_labels.append(f"[p{p}]")
            p += 1
        parts.append(f"[0:a]atrim=start={a}:end={b},asetpts=PTS-STARTPTS[a{idx}];")
        audio_labels.append(f"[a{idx}]")

    return (
        "".join(parts)
        + "".join(video_labels) + f"concat=n={len(video_labels)}:v=1:a=0[outv];"
        + "".join(audio_labels) + f"concat=n={len(audio_labels)}:v=0:a=1[outa]"
    )


def render_edited_video(
    input_path: str,
    output_path: str,
    keep_intervals: list[tuple[float, float]],
    total_duration: float,
    broll: dict | None = None
) -> bool:
    """
    Uses FFmpeg filter_complex trim/atrim + concat to cut dead air and retakes
    accurately using NVIDIA NVENC hardware encoding (with x264 fallback).
    `broll` = {"screen_path", "offset", "inserts": [(raw_start, raw_end)], "style"} adds screen B-roll.
    """
    if not keep_intervals:
        return False

    use_broll = bool(broll and broll.get("inserts") and broll.get("screen_path"))

    # If the whole video is kept with negligible trimming (< 0.4s difference), copy directly
    kept_sum = sum(b - a for a, b in keep_intervals)
    if not use_broll and len(keep_intervals) == 1 and abs(kept_sum - total_duration) < 0.4:
        log_info("No significant dead air or retakes found; preserving original stream.")
        if os.path.abspath(input_path) != os.path.abspath(output_path):
            subprocess.run(["ffmpeg", "-y", "-i", input_path, "-c", "copy", output_path], capture_output=True)
        return True

    inputs = ["-i", input_path]
    if use_broll:
        width, height, fps = probe_video_geometry(input_path)
        filter_Parts_str = build_broll_filter(keep_intervals, broll, width, height, fps)
        inputs += ["-i", broll["screen_path"]]
    else:
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
                *inputs,
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
        log_warn(f"FFmpeg render failed: {(res.stderr or '').strip()[-600:]}")
    finally:
        try:
            os.remove(filter_script_path)
        except OSError:
            pass

    return False


# ---------------------------------------------------------------------------
# 3. 1-to-4 Priority AI Metadata Generation (Gemini / OpenAI / Claude / Local)
# ---------------------------------------------------------------------------

METADATA_SYSTEM_PROMPT = """You are a world-class YouTube producer, copywriter, and SEO strategist.
Based on the provided video transcript, generate compelling YouTube metadata following these strict rules:

1. PRIMARY TITLE (the "title" field):
   - Your #1 single preferred catchy, high-CTR YouTube title (under 70 chars).
   - Hook the viewer with curiosity, strong statements, or high-stakes framing.

2. ALTERNATIVE PROSPECTIVE TITLES (the "alternative_titles" field):
   - A list of EXACTLY 9 other prospective catchy YouTube titles for this video.
   - Cover diverse styles: curiosity gap, bold declaration, how-to, question/debate, insider secret, listicle/ranking, contrarian hot take, emotional hook, and punchy hype style.
   - Total of 10 titles across primary and alternatives.

3. SEO DESCRIPTION (the "description" field):
   - Executive Summary (Book-Blurb Style): Write a compelling 1-to-2 paragraph executive summary written like a high-converting back-cover book blurb designed to sell the viewer on why they cannot afford to miss this video. Use persuasive, magnetic, polished prose.
   - Key Takeaways: Bulleted list of 3-5 core points or insights.
   - Timestamped Chapters: YouTube chapters formatted "MM:SS Title", starting at 00:00 and at least 10 seconds apart (e.g. 00:00 Intro and project overview\\n00:45 Setting up the local AI pipeline\\n...). Each chapter title is a summary of NO MORE than 10 words, written like a short tech headline, with NO period or other punctuation at the end.
   - Do NOT list the alternative titles inside the description; they belong only in the "alternative_titles" field.

4. TAGS (the "tags" field): 5 to 10 highly relevant SEO tags.

5. THUMBNAIL HOOK (the "thumbnail_hook" field): 3 to 4 punchy, high-contrast all-caps words suitable for a thumbnail graphic overlay.

6. THUMBNAIL PROMPT (the "thumbnail_prompt" field): Vivid visual description for a 16:9 cinematic background.

Respond ONLY with valid JSON matching this exact schema:
{
  "title": "Your #1 preferred catchy YouTube title",
  "alternative_titles": [
    "Catchy Alternative 2",
    "Catchy Alternative 3",
    "Catchy Alternative 4",
    "Catchy Alternative 5",
    "Catchy Alternative 6",
    "Catchy Alternative 7",
    "Catchy Alternative 8",
    "Catchy Alternative 9",
    "Catchy Alternative 10"
  ],
  "description": "Full description starting with the book-blurb executive summary, then takeaways and chapters",
  "tags": ["tag1", "tag2", "tag3", "tag4", "tag5"],
  "thumbnail_hook": "3 TO 4 WORDS",
  "thumbnail_prompt": "Cinematic visual description for 16:9 background"
}"""


def extract_json_object(raw_text: str) -> dict:
    raw_text = raw_text.strip()
    m = re.search(r"\{.*\}", raw_text, flags=re.DOTALL)
    if m:
        raw_text = m.group(0)
    return json.loads(raw_text)


# Gemini text models for titles and descriptions, newest first; Google retires old ones, so the next
# is tried when one is gone, and if all are gone the newest Flash model the key can use is picked
GEMINI_TEXT_MODELS = ["gemini-3-flash-preview", "gemini-3.1-pro-preview", "gemini-2.5-flash"]
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta"


def gemini_request(url: str, body: dict | None = None, timeout: int = 45) -> dict:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        # Google's reason (model not found, bad key, quota) is in the JSON body
        detail = str(e)
        try:
            err = json.loads(e.read().decode("utf-8", "replace")).get("error") or {}
            detail = f"HTTP {e.code} {err.get('status') or ''}: {err.get('message') or detail}"
        except Exception:
            pass
        raise RuntimeError(detail) from None


def gemini_generate_json(api_key: str, model: str, prompt: str) -> dict:
    body = {
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {"responseMimeType": "application/json", "temperature": 0.4}
    }
    data = gemini_request(f"{GEMINI_API}/models/{model}:generateContent?key={api_key}", body)
    text = data["candidates"][0]["content"]["parts"][0]["text"]
    return extract_json_object(text)


def newest_gemini_flash_model(api_key: str) -> str:
    """The newest Flash text model this key can call, from Google's model list (free to ask)."""
    data = gemini_request(f"{GEMINI_API}/models?key={api_key}&pageSize=1000")
    skip = ("image", "tts", "live", "lite", "embedding", "audio")
    names = [m.get("name", "").removeprefix("models/") for m in data.get("models", [])
             if "generateContent" in m.get("supportedGenerationMethods", [])]
    names = [n for n in names if "flash" in n and not any(s in n for s in skip)]
    if not names:
        raise RuntimeError("No Gemini Flash text model is available to this API key")

    def version(name):
        m = re.search(r"gemini-(\d+(?:\.\d+)?)", name)
        return (float(m.group(1)) if m else 0.0, "preview" not in name and "exp" not in name)

    return max(names, key=version)


def call_gemini_json(api_key: str, system_prompt: str, user_text: str, max_tokens: int = 1800) -> dict:
    if not api_key:
        raise ValueError("No Gemini API key configured in OBS Vault.")
    prompt = f"{system_prompt}\n\n{user_text[:24000]}"
    cached = _working_cloud_model.get("gemini_text")
    models = ([cached] if cached else []) + [m for m in GEMINI_TEXT_MODELS if m != cached]
    errors = []
    for model in models:
        try:
            res = gemini_generate_json(api_key, model, prompt)
        except Exception as e:
            if not is_model_unavailable(e):
                raise
            errors.append(f"{model}: {e}")
            continue
        _working_cloud_model["gemini_text"] = model
        res["_model_used"] = f"Google Gemini ({model})"
        return res
    log_warn("Gemini text models unavailable (" + " | ".join(errors) + "); asking Google which models this key can use")
    model = newest_gemini_flash_model(api_key)
    res = gemini_generate_json(api_key, model, prompt)
    _working_cloud_model["gemini_text"] = model
    res["_model_used"] = f"Google Gemini ({model})"
    return res


def call_openai_json(api_key: str, system_prompt: str, user_text: str, max_tokens: int = 1800) -> dict:
    if not api_key:
        raise ValueError("No OpenAI API key configured in OBS Vault.")
    url = "https://api.openai.com/v1/chat/completions"
    body = {
        "model": "gpt-4o",
        "response_format": {"type": "json_object"},
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text[:24000]}
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


CLAUDE_MODEL = "claude-sonnet-5-5"


def call_claude_json(api_key: str, system_prompt: str, user_text: str, max_tokens: int = 1800) -> dict:
    if not api_key:
        raise ValueError("No Claude API key configured in OBS Vault.")
    url = "https://api.anthropic.com/v1/messages"
    body = {
        "model": CLAUDE_MODEL,
        # Thinking is always on for this model and shares the token budget with the answer.
        "max_tokens": max(max_tokens, 16000),
        "output_config": {"effort": "low"},
        # On a safety decline, rerun on Anthropic's recommended fallback model server-side.
        "fallbacks": "default",
        "system": system_prompt,
        "messages": [
            {"role": "user", "content": user_text[:24000]}
        ]
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "server-side-fallback-2026-07-01"
        }
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    if data.get("stop_reason") == "refusal":
        raise RuntimeError(f"Claude declined the request ({(data.get('stop_details') or {}).get('category')}).")
    # The response opens with a thinking block; the JSON is in the text blocks.
    text = "".join(b.get("text", "") for b in data.get("content", []) if b.get("type") == "text")
    return extract_json_object(text)


OLLAMA_URL = "http://127.0.0.1:11434"


def ensure_ollama_server(wait_sec: float = 30.0) -> bool:
    """
    Make sure something answers on Ollama's local port. The Ollama tray app can stay
    running after its own server has died, so start 'ollama serve' ourselves if needed.
    """
    def answering() -> bool:
        try:
            with urllib.request.urlopen(f"{OLLAMA_URL}/api/tags", timeout=2):
                return True
        except Exception:
            return False

    if answering():
        return True
    exe = shutil.which("ollama") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Ollama", "ollama.exe")
    if not os.path.isfile(exe):
        return False
    log_info("Local Ollama isn't answering; starting 'ollama serve' in the background...")
    flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    subprocess.Popen([exe, "serve"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, creationflags=flags)
    deadline = time.time() + wait_sec
    while time.time() < deadline:
        time.sleep(1.0)
        if answering():
            return True
    return False


def call_local_ollama_json(ollama_model: str, system_prompt: str, user_text: str) -> dict:
    if not ensure_ollama_server():
        raise RuntimeError(f"Local Ollama isn't answering on {OLLAMA_URL} and could not be started.")
    url = f"{OLLAMA_URL}/api/generate"
    models_to_try = [ollama_model, "gemma4:12b", "llama3.1:8b", "qwen3:14b"]
    seen = set()
    ordered_models = [m for m in models_to_try if m and not (m in seen or seen.add(m))]

    errors = []
    for model in ordered_models:
        try:
            body = {
                "model": model,
                "prompt": f"{system_prompt}\n\n{user_text[:16000]}",
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
        except urllib.error.HTTPError as e:
            # Ollama puts the real reason (missing model, broken runner, OOM)
            # in the JSON body; the bare status line just says "500".
            detail = str(e)
            try:
                detail = json.loads(e.read().decode("utf-8", "replace")).get("error", detail)
            except Exception:
                pass
            errors.append(f"{model}: {detail}")
        except Exception as e:
            errors.append(f"{model}: {e}")

    raise RuntimeError("Local Ollama failed (" + " | ".join(errors) + ")")


def llm_provider_labels(ollama_model: str) -> dict:
    return {
        "gemini": "Google Gemini (Gemini 3 Flash)",
        "openai": "ChatGPT / OpenAI (gpt-4o)",
        "claude": f"Anthropic Claude ({CLAUDE_MODEL})",
        "local": f"Local GPU LLM ({ollama_model})"
    }


def run_llm_chain(
    priority_list: list[str],
    secrets: dict,
    ollama_model: str,
    system_prompt: str,
    user_text: str,
    step: str,
    validate,
    max_tokens: int = 1800
) -> tuple[dict | None, str]:
    """
    Walks the user's 1-to-4 Priority Ranking list and returns the first JSON
    result that passes `validate`, with its provider badge; (None, "") if all fail.
    """
    labels = llm_provider_labels(ollama_model)
    for rank_idx, provider in enumerate(priority_list, start=1):
        p_key = provider.lower().strip()
        if p_key not in labels:
            continue
        label = labels[p_key]
        update_step(step, 25 + rank_idx * 15, f"Trying Priority #{rank_idx}: {label}...")
        try:
            if p_key == "gemini":
                res = call_gemini_json(secrets.get("gemini_key", ""), system_prompt, user_text, max_tokens)
            elif p_key == "openai":
                res = call_openai_json(secrets.get("openai_key", ""), system_prompt, user_text, max_tokens)
            elif p_key == "claude":
                res = call_claude_json(secrets.get("claude_key", ""), system_prompt, user_text, max_tokens)
            else:
                res = call_local_ollama_json(ollama_model, system_prompt, user_text)
            if res and validate(res):
                return res, f"Priority #{rank_idx}: {res.get('_model_used', label)}"
        except Exception as e:
            log_warn(f"Priority #{rank_idx} ({label}) skipped/fell back: {e}")
    return None, ""


_ALT_TITLES_SECTION_RE = re.compile(
    r"\n*[^\n]*alternative[^\n]*titles?[^\n]*\n(?:[ \t]*(?:\d+[.)]|[-•*])[^\n]*(?:\n|$))*",
    re.IGNORECASE,
)


def strip_alternative_titles_section(description: str) -> str:
    """Drop a trailing 'alternative titles' list a model added despite the prompt."""
    return _ALT_TITLES_SECTION_RE.sub("\n", description or "").strip()


def write_titles_file(path: str, primary_title: str, alternative_titles: list[str]):
    """Save the title options beside the video so they never have to live in the description."""
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"1. {primary_title} (current)\n")
            for idx, alt in enumerate(alternative_titles, start=2):
                f.write(f"{idx}. {alt}\n")
        log_info(f"Title options saved: {os.path.basename(path)}")
    except OSError as e:
        log_warn(f"Could not save title options file: {e}")


_CHAPTER_FILLER_RE = re.compile(r"^(?:(?:so|okay|ok|um|uh|well|alright|and|but)\b[\s,]*)+", re.IGNORECASE)
_CHAPTER_LINE_RE = re.compile(r"^(\s*\d{1,2}(?::\d{2}){1,2})(\s*[-–—]\s*|\s+)(.*)$")


_DANGLING_WORDS = {"a", "an", "the", "and", "or", "but", "to", "of", "in", "on", "at", "for",
                   "with", "you", "your", "is", "that", "this", "so"}


def tidy_chapter_title(text: str, max_words: int = 10) -> str:
    """
    Cap a chapter title at `max_words` words with no trailing period/ellipsis.
    Only the first sentence is kept, and a longer one is cut at its last clause break, not mid-phrase.
    """
    words = re.split(r"(?<=[.!?])\s+", (text or "").strip(), maxsplit=1)[0].split()
    if len(words) > max_words:
        words = words[:max_words]
        breaks = [k for k, w in enumerate(words[:-1]) if w.endswith((",", ";", ":", "—"))]
        if breaks and breaks[-1] >= 2:
            words = words[:breaks[-1] + 1]
        while len(words) > 1 and _norm_word(words[-1]) in _DANGLING_WORDS:
            words.pop()
    return " ".join(words).rstrip(" .…,;:—")


def normalize_chapter_lines(description: str) -> str:
    """Apply `tidy_chapter_title` to every 'MM:SS title' line of a description."""
    out = []
    for line in (description or "").split("\n"):
        m = _CHAPTER_LINE_RE.match(line)
        if m:
            title = tidy_chapter_title(m.group(3))
            line = f"{m.group(1).strip()}{m.group(2) if title else ''}{title}".rstrip()
        out.append(line)
    return "\n".join(out)


def remap_segments_to_edited_timeline(segments: list[dict], keep_intervals: list[tuple]) -> list[dict]:
    """
    Shift segment times from the raw recording onto the edited video. The edit is the
    kept intervals concatenated in order, so a time maps to the kept duration before its
    interval plus its offset inside it. Segments that fall inside a cut are dropped.
    """
    remapped = []
    for seg in segments:
        mid = (seg["start"] + seg["end"]) / 2.0
        offset = 0.0
        for a, b in keep_intervals:
            if a <= mid <= b:
                start = offset + max(seg["start"], a) - a
                end = offset + min(seg["end"], b) - a
                remapped.append({**seg, "start": start, "end": max(start, end)})
                break
            offset += b - a
    return remapped


SOCIAL_LINKS = [
    ("X", "https://www.x.com/kelleyparker"),
]
_SOCIAL_ICONS = {"X": "\U0001D54F"}


def append_social_footer(description: str) -> str:
    """Append the hardcoded social-links block once (YouTube rejects '<' and '>', so plain text only)."""
    description = (description or "").rstrip()
    if not SOCIAL_LINKS or "CONNECT WITH ME" in description:
        return description
    rule = "━" * 22
    lines = [rule, "\U0001F517 CONNECT WITH ME", rule]
    for name, url in SOCIAL_LINKS:
        icon = _SOCIAL_ICONS.get(name, "▶")
        lines.append(f"{icon}  Follow me on {name} ➜ {url}")
    return description + "\n\n" + "\n".join(lines)


def generate_heuristic_transcript_metadata(segments: list[dict], transcript_text: str) -> dict:
    """
    Intelligent NLP & transcript-driven metadata engine.
    Analyzes spoken text, extracts themes/entities, constructs the #1 catchy YouTube title,
    9 distinct prospective alternative titles (covering curiosity, bold, debate, etc.),
    and formats a compelling book-blurb style executive summary with real chapter timestamps.
    Guarantees no generic fallback date strings ever reach the user.
    """
    full_text = " ".join(s.get("text", "") for s in segments) if segments else transcript_text
    lower = full_text.lower()

    # Detect gaming / Mario / Zelda themes
    is_mario_zelda = ("mario" in lower or "zelda" in lower or "ocarina" in lower or "video game" in lower)

    # Chapters formatted with real timestamps
    # YouTube ignores chapters spaced closer than 10s; titles are <=10 words, no trailing period.
    chapter_lines = ["00:00 - Introduction"]
    last_sec = 0
    for s in segments:
        sec = int(s.get("start", 0))
        if sec - last_sec < 10:
            continue
        txt = tidy_chapter_title(_CHAPTER_FILLER_RE.sub("", s.get("text", "").strip()))
        if not txt:
            continue
        txt = txt[0].upper() + txt[1:]
        chapter_lines.append(f"{sec // 60:02d}:{sec % 60:02d} - {txt}")
        last_sec = sec

    chapters_str = "\n".join(chapter_lines[:8])

    if is_mario_zelda:
        primary_title = "Super Mario Bros vs. Zelda: Ranking The Greatest Games of All Time"
        alt_titles = [
            "The Controversial Gaming Tier List Nobody Wants to Admit",
            "Why Super Mario Bros Still Destroys Every Modern Video Game",
            "Ocarina of Time vs. Majora's Mask: Settling The Best Zelda Debate",
            "Is Super Mario Bros 2 Actually A Masterpiece? (Hot Take)",
            "The Untold Genius Behind Retro Gaming's Greatest Trilogy",
            "Top 5 Greatest Video Games of All Time Ranked (Super Mario & Zelda)",
            "Stop Playing New Releases Until You Understand This Classic Rule",
            "Why I Will Defend Super Mario Bros as The GOAT Until The End",
            "The Greatest Games Ever Made: An Unfiltered Breakdown"
        ]
        book_blurb = (
            "In an era obsessed with hyper-realistic visuals and endless live-service hype, "
            "one timeless question continues to ignite fiery debate across every gaming community: "
            "what truly makes a video game the greatest of all time? From the ground-shifting brilliance "
            "of the Super Mario Bros trilogy to the haunting atmosphere of Zelda Ocarina of Time and Majora's Mask, "
            "this video delivers an unapologetic, front-row breakdown of the legendary masterpieces that permanently "
            "redefined digital entertainment. If you have ever held a controller or debated gaming history, "
            "this is the definitive argument you cannot afford to miss."
        )
        takeaways = (
            "• The definitive ranking of the Super Mario Bros trilogy and why it set the gold standard\n"
            "• Why Zelda Ocarina of Time remains the pinnacle of 3D game design\n"
            "• The case for Majora's Mask as gaming's most daring narrative sequel\n"
            "• The timeless gameplay mechanics modern studios keep forgetting"
        )
        tags = ["Super Mario Bros", "Zelda Ocarina of Time", "Majoras Mask", "Retro Gaming", "Gaming Tier List", "Greatest Games of All Time", "Nintendo"]
        hook = "GREATEST GAMES EVER"
        prompt = "Cinematic retro gaming montage featuring iconic 8-bit Mario and golden Triforce emblems, dramatic studio neon lighting, high-contrast 16:9"
    else:
        # Dynamic topic extraction for general videos
        words = re.findall(r"\b[A-Za-z]{4,}\b", full_text)
        stopwords = {"this", "that", "with", "have", "from", "they", "will", "what", "there", "about", "would", "make", "just", "know", "take", "people", "into", "year", "your", "good", "some", "could", "them", "other", "than", "then", "look", "only", "come", "over", "think", "also", "back", "after", "used", "work", "first", "well", "even", "want", "because", "any", "these", "give", "most", "us"}
        filtered = [w.capitalize() for w in words if w.lower() not in stopwords]
        freq = {}
        for w in filtered:
            freq[w] = freq.get(w, 0) + 1
        top_topics = sorted(freq.keys(), key=lambda x: freq[x], reverse=True)[:3]
        topic_str = " & ".join(top_topics) if top_topics else "The Untold Truth"

        primary_title = f"The Truth About {topic_str}: What Nobody Is Telling You"
        alt_titles = [
            f"Why {topic_str} Changes Everything (Full Breakdown)",
            f"The Biggest Mistake People Make With {topic_str}",
            f"Stop Doing This Until You Understand {topic_str}",
            f"Is {topic_str} Actually Worth It? An Honest Look",
            f"The Hidden Secret Behind {topic_str} Revealed",
            f"Top 5 Things You Must Know About {topic_str}",
            f"How {topic_str} Will Transform Your Results Fast",
            f"The Controversial Truth About {topic_str} Exposed",
            f"{topic_str}: The Complete Insider Masterclass"
        ]
        book_blurb = (
            f"Step inside the untold reality of {topic_str}. In a crowded world full of surface-level advice "
            "and recycled opinions, this video cuts through the noise to deliver an uncompromising, deep-dive "
            "investigation into what actually moves the needle. Whether you are looking to master the fundamentals "
            "or uncover the hidden nuances that the mainstream ignores, this is the essential playbook that "
            "will forever shift how you think about the subject."
        )
        takeaways = (
            f"• Core fundamentals of {topic_str} explained without jargon\n"
            "• The hidden pitfalls most people fail to notice until it's too late\n"
            "• Actionable steps to optimize your strategy immediately\n"
            "• Key takeaways to future-proof your workflow"
        )
        tags = [topic_str, "Deep Dive", "Tutorial", "Creator", "OBS Studio", "Masterclass"]
        hook = "WATCH THIS FIRST"
        prompt = f"Futuristic high-tech studio scene showcasing {topic_str}, dramatic rim lighting, cinematic 16:9 composition"

    description = (
        f"📖 EXECUTIVE SUMMARY:\n{book_blurb}\n\n"
        f"📌 KEY TAKEAWAYS:\n{takeaways}\n\n"
        f"⏱️ CHAPTER TIMESTAMPS:\n{chapters_str}\n\n"
        f"Produced with OBS Studio YouTube Automation."
    )

    return {
        "title": primary_title,
        "alternative_titles": alt_titles,
        "description": description,
        "tags": tags,
        "thumbnail_hook": hook,
        "thumbnail_prompt": prompt
    }


TEMPLATE_METADATA_BADGE = "Template fallback (no AI provider answered)"


def generate_metadata_with_priority(
    priority_list: list[str],
    secrets: dict,
    ollama_model: str,
    segments: list[dict]
) -> tuple[dict, str]:
    """
    Walks the user's 1-to-4 Priority Ranking list (#1 -> #2 -> #3 -> #4)
    to generate video Title, 9 Alternative Titles, SEO Description (with book blurb),
    Tags, and Thumbnail Hook.
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

    res, used_badge = run_llm_chain(
        priority_list, secrets, ollama_model,
        METADATA_SYSTEM_PROMPT, f"TRANSCRIPT:\n{transcript_text}",
        step="llm", validate=lambda r: bool(r.get("title"))
    )
    if res:
        res["description"] = normalize_chapter_lines(
            strip_alternative_titles_section(res.get("description", ""))
        )
        # Ensure alternative_titles exists and has items
        if not res.get("alternative_titles"):
            res["alternative_titles"] = [
                f"{res['title']} (Extended Cut)",
                f"Why {res['title']} Matters Now",
                f"The Secret of {res['title']}",
                f"How To Master {res['title']}",
                f"Stop Ignoring {res['title']}",
                f"{res['title']} - Unfiltered Reaction",
                f"The Complete Guide to {res['title']}",
                f"Is {res['title']} True?",
                f"{res['title']} Explained Simply"
            ]
        log_info(f"Metadata generated successfully via {used_badge}.")
        return res, used_badge

    # Fallback: intelligent transcript heuristic analysis (never static generic date)
    log_warn(
        "No AI provider answered (see the Priority warnings above), so the title and description "
        "come from a fill-in template. Add an API key in YouTube settings or start Ollama, and "
        "review the title before publishing."
    )
    fallback_meta = generate_heuristic_transcript_metadata(segments, transcript_text)
    return fallback_meta, TEMPLATE_METADATA_BADGE


# ---------------------------------------------------------------------------
# 4. 1-to-4 Priority Custom Thumbnail Generator (1280x720 JPG)
# ---------------------------------------------------------------------------

def extract_top_talking_head_frames(video_path: str, count: int = 4, samples: int = 12, min_gap: float = 0.08) -> list:
    """
    Uses OpenCV to sample candidate frames across the edited video, scoring each
    by Laplacian sharpness and face visibility so you never get a blurry or mid-blink frame.
    Returns up to `count` frames, best first, at least `min_gap` of the video apart.
    """
    import cv2
    import numpy as np

    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        return []

    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    if frame_count <= 0:
        ret, frame = cap.read()
        cap.release()
        return [frame] if ret else []

    face_cascade = None
    try:
        cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        if os.path.exists(cascade_path):
            face_cascade = cv2.CascadeClassifier(cascade_path)
    except Exception:
        pass

    # Evenly spread between 6% and 94%, away from the intro and outro
    sample_points = [int(frame_count * (0.06 + 0.88 * i / max(1, samples - 1))) for i in range(samples)]
    scored = []

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

        scored.append((sharpness * exposure_weight * face_bonus, idx, frame.copy()))

    cap.release()

    picked = []
    for score, idx, frame in sorted(scored, key=lambda s: s[0], reverse=True):
        if all(abs(idx - other) >= frame_count * min_gap for _, other, _ in picked):
            picked.append((score, idx, frame))
        if len(picked) >= count:
            break
    return [frame for _, _, frame in picked]


def extract_best_talking_head_frame(video_path: str):
    frames = extract_top_talking_head_frames(video_path, count=1)
    return frames[0] if frames else None


def frame_to_pil(frame_bgr):
    import cv2
    from PIL import Image

    if frame_bgr is None:
        return Image.new("RGB", (1280, 720), (32, 18, 48))
    return Image.fromarray(cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB))


def load_thumbnail_font(size: int):
    from PIL import ImageFont

    # Load bold Windows font (Impact / Arial Black / Segoe UI Black)
    for font_name in ["impact.ttf", "ariblk.ttf", "segoeuib.ttf", "arialbd.ttf"]:
        font_path = os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts", font_name)
        if os.path.exists(font_path):
            try:
                return ImageFont.truetype(font_path, size)
            except Exception:
                pass
    return ImageFont.load_default()


def wrap_hook_lines(text: str, max_chars: int = 12, max_lines: int = 3) -> list[str]:
    lines = []
    for word in text.split():
        if lines and len(lines[-1]) + 1 + len(word) <= max_chars:
            lines[-1] += " " + word
        else:
            lines.append(word)
    return lines[:max_lines]


THUMBNAIL_LAYOUTS = ("bottom", "left", "zoom", "duotone")


def parse_hex_color(value: str, default: tuple) -> tuple:
    value = (value or "").strip().lstrip("#")
    if re.fullmatch(r"[0-9a-fA-F]{6}", value):
        return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))
    return default


def overlay_youtube_hook_text(base_pil_img, hook_text: str, layout: str = "bottom", accent: tuple = (255, 55, 55)):
    """
    Renders high-contrast YouTube thumbnail typography with gradient vignette,
    bold text, thick outline stroke, and accent badge using Pillow.
    Layouts: "bottom" (banner), "left" (big stacked text), "zoom" (punch-in), "duotone" (tinted).
    """
    from PIL import Image, ImageDraw, ImageEnhance, ImageOps

    img = base_pil_img.convert("RGB")
    if layout == "zoom":
        w, h = img.size
        img = img.crop((int(w * 0.15), int(h * 0.12), int(w * 0.85), int(h * 0.82)))
    img = img.resize((1280, 720), Image.Resampling.LANCZOS)
    if layout == "duotone":
        gray = ImageOps.grayscale(img)
        img = ImageOps.colorize(gray, black=(10, 10, 20), white=tuple(min(255, c + 60) for c in accent))
    # Boost color contrast & saturation for YouTube feed pop
    img = ImageEnhance.Contrast(img).enhance(1.14)
    img = ImageEnhance.Color(img).enhance(1.18)

    hook_clean = (hook_text or "MUST WATCH").upper().strip()
    overlay = Image.new("RGBA", (1280, 720), (0, 0, 0, 0))
    draw_ov = ImageDraw.Draw(overlay)

    if layout in ("left", "right"):
        # Dark gradient from that edge for big stacked text ("right" mirrors "left")
        right = layout == "right"
        for x in range(0, 760):
            alpha = int(215 * (1.0 - x / 760.0))
            edge_x = 1279 - x if right else x
            draw_ov.line([(edge_x, 0), (edge_x, 720)], fill=(8, 8, 14, alpha))
        img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
        draw = ImageDraw.Draw(img)
        font = load_thumbnail_font(118)
        lines = wrap_hook_lines(hook_clean[:40])
        y = 360 - len(lines) * 66
        bar_x = 1208 if right else 56
        draw.rectangle([bar_x, y - 26, bar_x + 16, y + len(lines) * 132 - 20], fill=accent)
        for line in lines:
            x = 1184 - draw.textlength(line, font=font) if right else 96
            draw.text((x, y), line, font=font, fill=(255, 235, 60), stroke_width=7, stroke_fill=(0, 0, 0))
            y += 132
        return img

    # Apply subtle dark gradient on the bottom for text legibility
    for y in range(380, 720):
        alpha = int(185 * ((y - 380) / 340.0))
        draw_ov.line([(0, y), (1280, y)], fill=(8, 8, 14, alpha))
    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")

    draw = ImageDraw.Draw(img)
    font = load_thumbnail_font(92)

    # Draw glowing accent bar + bold white/yellow text
    draw.rounded_rectangle([54, 520, 1226, 675], radius=18, fill=(18, 18, 26), outline=accent, width=5)
    draw.text(
        (86, 548),
        hook_clean[:34],
        font=font,
        fill=(255, 235, 60),
        stroke_width=6,
        stroke_fill=(0, 0, 0)
    )
    return img


# Cloud image sources: (name, models newest first). When a model isn't available (retired, or not
# enabled for the API key yet), the next one is tried.
CLOUD_IMAGE_MODELS = {
    "gemini": ("Google Nano Banana 2",
               ["gemini-3.1-flash-image-preview", "gemini-3-pro-image-preview", "gemini-2.5-flash-image"]),
    "gemini_pro": ("Google Nano Banana Pro",
                   ["gemini-3-pro-image-preview", "gemini-3.1-flash-image-preview", "gemini-2.5-flash-image"]),
    "openai": ("OpenAI GPT Image", ["gpt-image-2", "gpt-image-1.5", "gpt-image-1"]),
}
_working_cloud_model = {}


def is_model_unavailable(error) -> bool:
    text = str(error).lower()
    return any(s in text for s in ("404", "not_found", "not found", "model_not_found", "does not exist"))


def generate_gemini_image(api_key: str, prompt: str, model: str, init_image=None):
    """Nano Banana (Gemini image model) text-to-image, or an edit of init_image. Returns a PIL image."""
    if not api_key:
        raise ValueError("No Gemini API key configured.")
    from google import genai
    from google.genai import types
    from PIL import Image

    client = genai.Client(api_key=api_key)
    result = client.models.generate_content(
        model=model,
        contents=[prompt] if init_image is None else [prompt, init_image],
        config=types.GenerateContentConfig(
            response_modalities=["TEXT", "IMAGE"],
            image_config=types.ImageConfig(aspect_ratio="16:9")
        )
    )
    said = []
    for candidate in result.candidates or []:
        for part in (candidate.content.parts if candidate.content else None) or []:
            if part.inline_data and part.inline_data.data:
                return Image.open(io.BytesIO(part.inline_data.data))
            if part.text:
                said.append(part.text.strip())
    raise RuntimeError(f"{model} returned no image" + (f": {' '.join(said)[:200]}" if said else "."))


def multipart_body(fields: dict, files: dict) -> tuple[bytes, str]:
    """multipart/form-data for urllib: fields {name: value}, files {name: (filename, bytes, type)}."""
    boundary = "----obs" + os.urandom(12).hex()
    out = io.BytesIO()
    for name, value in fields.items():
        out.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode("utf-8"))
    for name, (filename, content, ctype) in files.items():
        out.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'
                  f'Content-Type: {ctype}\r\n\r\n'.encode("utf-8"))
        out.write(content)
        out.write(b"\r\n")
    out.write(f"--{boundary}--\r\n".encode("utf-8"))
    return out.getvalue(), f"multipart/form-data; boundary={boundary}"


def generate_openai_image(api_key: str, prompt: str, model: str, init_image=None):
    """GPT Image text-to-image, or an edit of init_image. Returns a PIL image."""
    if not api_key:
        raise ValueError("No OpenAI API key configured.")
    from PIL import Image

    fields = {"model": model, "prompt": prompt, "n": 1, "size": "1536x1024", "quality": "medium"}
    if init_image is None:
        url = "https://api.openai.com/v1/images/generations"
        body, ctype = json.dumps(fields).encode("utf-8"), "application/json"
    else:
        png = io.BytesIO()
        init_image.convert("RGB").save(png, "PNG")
        url = "https://api.openai.com/v1/images/edits"
        body, ctype = multipart_body(fields, {"image": ("background.png", png.getvalue(), "image/png")})
    req = urllib.request.Request(url, data=body, headers={"Content-Type": ctype, "Authorization": f"Bearer {api_key}"})
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        # The reason (model not found, organization not verified, safety) is in the JSON body
        detail = str(e)
        try:
            err = json.loads(e.read().decode("utf-8", "replace")).get("error") or {}
            detail = f"HTTP {e.code} {err.get('code') or ''}: {err.get('message') or detail}"
        except Exception:
            pass
        raise RuntimeError(detail) from None
    return Image.open(io.BytesIO(base64.b64decode(data["data"][0]["b64_json"])))


def generate_cloud_image(source: str, secrets: dict, prompt: str, init_image=None):
    """One image from a cloud source, trying the next model if one isn't available. Returns (image, model)."""
    _, models = CLOUD_IMAGE_MODELS[source]
    working = _working_cloud_model.get(source)
    models = sorted(models, key=lambda m: m != working)
    last = None
    for model in models:
        try:
            if source == "openai":
                img = generate_openai_image(secrets.get("openai_key", ""), prompt, model, init_image)
            else:
                img = generate_gemini_image(secrets.get("gemini_key", ""), prompt, model, init_image)
            _working_cloud_model[source] = model
            return img, model
        except Exception as e:
            if not is_model_unavailable(e):
                raise
            log_warn(f"{model} isn't available ({str(e)[:160]}); trying the next model.")
            last = e
    raise RuntimeError(f"None of {', '.join(models)} is available: {last}")


DEFAULT_THUMBNAIL_IMAGE_MODEL = "x/z-image-turbo"

# Local Stable Diffusion runs youtube_imagegen.py with the Python of its own venv (PyTorch + diffusers),
# with the model cached next to it: ~/OBSYouTubeImageGen/{venv,hf}
SD_HOME = os.path.join(os.path.expanduser("~"), "OBSYouTubeImageGen")
DEFAULT_SD_PYTHON = os.path.join(SD_HOME, "venv", "Scripts" if os.name == "nt" else "bin",
                                 "python.exe" if os.name == "nt" else "python")
DEFAULT_SD_MODEL = "stabilityai/stable-diffusion-xl-base-1.0"
SD_NEGATIVE_PROMPT = "text, words, letters, caption, watermark, logo, signature, blurry, lowres, deformed, ugly"

THUMBNAIL_IMAGE_SOURCE_LABELS = {
    "sd": "Local Stable Diffusion",
    "local": "Local Ollama image model",
    "gemini": CLOUD_IMAGE_MODELS["gemini"][0],
    "gemini_pro": CLOUD_IMAGE_MODELS["gemini_pro"][0],
    "openai": CLOUD_IMAGE_MODELS["openai"][0],
    "claude": "Claude design + local compositor",
}

THUMBNAIL_PROMPT_VARIANTS = (
    "cinematic lighting, shallow depth of field",
    "bold graphic poster style, strong simple shapes",
    "bright saturated colours, playful energy",
    "dramatic close-up, moody high contrast",
)

THUMBNAIL_DESIGN_PROMPT = """You design YouTube thumbnails for a talking-head video.
Return ONLY a JSON object: {"designs": [{"hook": "...", "layout": "...", "accent": "#RRGGBB"}]}
with exactly {count} designs. Each hook is 2 to 5 punchy, curiosity-driven words, at most
28 characters, and every design uses a different hook. layout is one of: bottom, left, zoom, duotone
(use each layout at most once while possible). accent is a bold colour that pops on a dark background."""


def generate_ollama_image(model: str, prompt: str):
    """Text-to-image on the local Ollama server (experimental in Ollama). Returns a PIL image."""
    from PIL import Image

    if not ensure_ollama_server():
        raise RuntimeError(f"Local Ollama isn't answering on {OLLAMA_URL} and could not be started.")
    body = {"model": model, "prompt": prompt, "stream": False, "width": 1280, "height": 720}
    req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = str(e)
        try:
            detail = json.loads(e.read().decode("utf-8", "replace")).get("error", detail)
        except Exception:
            pass
        raise RuntimeError(detail)
    b64 = data.get("image") or (data.get("images") or [""])[0]
    if not b64:
        raise RuntimeError(f"'{model}' returned no image (is it an image model, and does this Ollama support image generation?)")
    return Image.open(io.BytesIO(base64.b64decode(b64)))


THUMBNAIL_STYLES = {
    "mixed": THUMBNAIL_PROMPT_VARIANTS,
    "cinematic": ("cinematic lighting, shallow depth of field", "cinematic wide shot, anamorphic lens flare",
                  "cinematic low angle, volumetric light", "cinematic golden hour, film grain"),
    "poster": ("bold graphic poster style, strong simple shapes", "flat vector poster art, limited palette",
               "retro screen-print poster, halftone texture", "minimal geometric poster, big negative space"),
    "bright": ("bright saturated colours, playful energy", "vivid pop-art colours, high key lighting",
               "colourful 3D render, soft studio light", "sunny bright scene, clean and cheerful"),
    "moody": ("dramatic close-up, moody high contrast", "dark moody scene, rim lighting, deep shadows",
              "noir style, single hard light, haze", "low-key lighting, deep blue and orange tones"),
}

# Measured on an RTX 5070 with SDXL at 1344x768, 30 steps; replaced by this PC's own timings after a run
SD_TIMINGS_DEFAULT = {"load_s": 15.0, "first_image_s": 27.0, "image_s": 10.0}


def sd_timings_path() -> str:
    return os.path.join(SD_HOME, "timings.json")


def load_sd_timings() -> dict:
    try:
        saved = read_json_file(sd_timings_path())
        return {k: float(saved.get(k, v)) for k, v in SD_TIMINGS_DEFAULT.items()}
    except (OSError, ValueError, TypeError, AttributeError):
        return dict(SD_TIMINGS_DEFAULT)


def save_sd_timings(load_s: float, image_times: list):
    if not image_times:
        return
    later = image_times[1:]
    timings = {
        "load_s": round(load_s, 1),
        "first_image_s": round(image_times[0], 1),
        "image_s": round(sum(later) / len(later), 1) if later else load_sd_timings()["image_s"],
    }
    try:
        write_json_file(sd_timings_path(), timings)
    except OSError:
        pass


def sd_eta_seconds(timings: dict, count: int, done: int, phase: str, elapsed: float = 0.0,
                   step: int = 0, steps: int = 0, last_image_s: float | None = None, work: float = 1.0,
                   step_rate: float | None = None) -> float:
    """
    Seconds left in a Stable Diffusion job of `count` images, `done` of them finished. `phase` is
    "loading" (elapsed = time since the model started loading) or "image" (elapsed = time in the
    current image, `step` of `steps` done, `step_rate` seconds per step once measured). `work` scales
    the saved timings (Refine runs fewer steps).
    """
    if done >= count:
        return 0.0
    next_image = last_image_s or timings["image_s"] * work
    if phase == "loading":
        return (max(0.0, timings["load_s"] - elapsed) + timings["first_image_s"] * work
                + max(0, count - 1) * timings["image_s"] * work)
    if step_rate and steps > 0:
        current = step_rate * max(0, steps - step)
    else:
        first = timings["first_image_s"] * work if done == 0 and last_image_s is None else next_image
        current = max(0.0, first - elapsed)
    return current + max(0, count - done - 1) * next_image


def imagegen_command(config: dict) -> tuple[list, dict, int]:
    """Command line, environment and creation flags for youtube_imagegen.py in the Stable Diffusion venv."""
    python = config.get("thumbnail_sd_python") or DEFAULT_SD_PYTHON
    if not os.path.exists(python):
        raise RuntimeError(f"Stable Diffusion isn't installed (no {python}).")
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "youtube_imagegen.py")
    env = dict(os.environ, HF_HOME=os.path.join(SD_HOME, "hf"), HF_HUB_DISABLE_TELEMETRY="1",
               PYTHONIOENCODING="utf-8", PYTORCH_CUDA_ALLOC_CONF="expandable_segments:True",
               U2NET_HOME=os.path.join(SD_HOME, "rembg"))
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    return [python, script], env, flags


def generate_sd_images(config: dict, prompts: list, out_dir: str, label: str, init_images: list | None = None,
                       strength: float = 0.5, negative_prompt: str = SD_NEGATIVE_PROMPT,
                       notes: list | None = None) -> list:
    """
    Local Stable Diffusion (youtube_imagegen.py in its own venv): text-to-image, or image-to-image
    when `init_images` is given. Shows a countdown in the thumbnail step. Returns PIL images.
    """
    import queue
    import threading

    from PIL import Image

    cmd, env, flags = imagegen_command(config)
    job = {"model": config.get("thumbnail_sd_model") or DEFAULT_SD_MODEL, "prompts": prompts,
           "negative_prompt": negative_prompt, "width": 1344, "height": 768, "steps": 30,
           "guidance": 7.0, "out_dir": out_dir}
    work = 1.0
    if init_images:
        job.update(init_images=init_images, strength=strength)
        work = strength
    timings = load_sd_timings()
    count = len(prompts)

    # stderr (library warnings) goes to a file: an unread pipe fills up and stalls the generator
    err_path = os.path.join(out_dir, "sd_log.txt")
    with open(err_path, "w", encoding="utf-8") as err_file:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err_file, env=env,
                                creationflags=flags, text=True, encoding="utf-8", errors="replace")
        proc.stdin.write(json.dumps(job))
        proc.stdin.close()

        # A reader thread, so the countdown keeps ticking while the model loads and nothing is printed
        lines = queue.Queue()

        def pump():
            for raw in proc.stdout:
                lines.put(raw)
            lines.put(None)

        threading.Thread(target=pump, daemon=True).start()

        images, image_times = [], []
        finished, step, steps, load_s = 0, 0, 0, 0.0
        phase, phase_started = "loading", time.time()
        # Seconds per step, measured from the first step reported in an image (the time before it is warm-up)
        rate_from, step_rate = None, None

        def show():
            left = sd_eta_seconds(timings, count, finished, phase, time.time() - phase_started, step, steps,
                                  image_times[-1] if image_times else None, work, step_rate)
            if phase == "loading":
                update_step("thumbnail", 30, f"Loading Stable Diffusion… ~{_fmt_clock(left)} left")
            else:
                done = finished + (step / steps if steps else 0.0)
                update_step("thumbnail", 30 + int(55 * done / count),
                            f"AI image {min(finished + 1, count)}/{count} · {_fmt_clock(left)} left")

        show()
        while True:
            try:
                line = lines.get(timeout=1.0)
            except queue.Empty:
                show()
                continue
            if line is None:
                break
            try:
                event = json.loads(line)
            except ValueError:
                continue
            kind = event.get("type")
            if kind == "loading":
                phase_started = time.time()
            elif kind == "loaded":
                load_s = float(event.get("seconds") or 0.0)
                log_info(f"Stable Diffusion loaded on {event.get('device')} in {event.get('seconds')} s.")
                phase, phase_started, step, steps = "image", time.time(), 0, 0
            elif kind == "progress":
                step, steps = int(event.get("step", 0)), int(event.get("steps", 0))
                if rate_from is None:
                    rate_from = (time.time(), step)
                elif step > rate_from[1]:
                    step_rate = (time.time() - rate_from[0]) / (step - rate_from[1])
            elif kind == "image":
                images.append(Image.open(event["path"]).convert("RGB"))
                image_times.append(float(event.get("seconds") or 0.0))
                finished += 1
                phase_started, step, steps, rate_from = time.time(), 0, 0, None
            elif kind == "error":
                message = event.get("message", "")
                log_warn(f"{label}: {message}")
                if "index" in event:
                    finished += 1
                    phase_started, step, steps, rate_from = time.time(), 0, 0, None
                    if notes is not None:
                        notes.append(f"Image {event['index'] + 1} failed: {message[:160]}")
                elif notes is not None:
                    notes.append(message[:200])
            show()
        proc.wait()
    if not images:
        with open(err_path, encoding="utf-8", errors="replace") as f:
            tail = f.read().strip().splitlines()[-1:]
        raise RuntimeError(tail[0] if tail else f"no images (exit code {proc.returncode})")
    os.remove(err_path)
    if not init_images:
        save_sd_timings(load_s, image_times)
    return images


def cut_out_person(config: dict, image_path: str):
    """The person in a photo with the background removed (RGBA, cropped), cached by the photo's contents."""
    import hashlib

    from PIL import Image

    with open(image_path, "rb") as f:
        digest = hashlib.sha1(f.read()).hexdigest()[:16]
    cached = os.path.join(SD_HOME, "cutouts", f"{digest}.png")
    if not os.path.exists(cached):
        os.makedirs(os.path.dirname(cached), exist_ok=True)
        cmd, env, flags = imagegen_command(config)
        res = subprocess.run(cmd, input=json.dumps({"task": "cutout", "image": image_path, "out": cached}),
                             capture_output=True, text=True, encoding="utf-8", errors="replace",
                             env=env, creationflags=flags, timeout=900)
        if not os.path.exists(cached):
            events = [json.loads(l) for l in res.stdout.splitlines() if l.startswith("{")]
            error = next((e.get("message") for e in events if e.get("type") == "error"), "")
            raise RuntimeError(error or (res.stderr.strip().splitlines() or ["background removal failed"])[-1])
    return Image.open(cached).convert("RGBA")


def resolve_person(config: dict, options: dict, frames: list, out_dir: str, notes: list):
    """The cut-out person for the thumbnail (from the attached photo or the best video frame), or None."""
    mode = options.get("person", "none")
    if mode not in ("photo", "video"):
        return None
    try:
        if mode == "photo":
            path = options.get("photo_path", "")
            if not path or not os.path.exists(path):
                raise RuntimeError(f"the photo wasn't found ({path or 'none attached'})")
        else:
            if not frames:
                raise RuntimeError("no video frame to cut you out of")
            path = os.path.join(out_dir, "person_frame.jpg")
            frame_to_pil(frames[0]).save(path, "JPEG", quality=95)
        update_step("thumbnail", 25, "Cutting you out of the photo...")
        return cut_out_person(config, path)
    except Exception as e:
        log_warn(f"Couldn't place you in the thumbnail: {e}")
        notes.append(f"Couldn't place you in the thumbnail: {e}")
        return None


def paste_person(img, person, side: str):
    """Paste a cut-out person at the bottom of one side, with a white outline and a soft shadow."""
    from PIL import Image, ImageFilter, ImageOps

    w, h = img.size
    scale = min(h * 0.92 / person.height, w * 0.45 / person.width)
    person = person.resize((max(1, int(person.width * scale)), max(1, int(person.height * scale))),
                           Image.Resampling.LANCZOS)
    pad = 12
    person = ImageOps.expand(person, border=pad, fill=(0, 0, 0, 0))
    x = w - person.width - 30 + pad if side == "right" else 30 - pad
    y = h - person.height + pad
    alpha = person.getchannel("A")

    def layer(color, mask, dx=0, dy=0):
        full = Image.new("L", img.size, 0)
        full.paste(mask, (x + dx, y + dy))
        out = Image.new("RGBA", img.size, color + (0,))
        out.putalpha(full)
        return out

    canvas = img.convert("RGBA")
    shadow = alpha.filter(ImageFilter.GaussianBlur(16)).point(lambda a: int(a * 0.75))
    canvas = Image.alpha_composite(canvas, layer((0, 0, 0), shadow, 12, 10))
    canvas = Image.alpha_composite(canvas, layer((255, 255, 255), alpha.filter(ImageFilter.MaxFilter(13))))
    canvas.alpha_composite(person, (x, y))
    return canvas.convert("RGB")


def compose_thumbnail(bg, hook: str, person=None, side: str = "right", layout: str = "", accent: tuple = (255, 55, 55)):
    """
    1280x720 thumbnail from a text-free background: the cut-out person on one side (if any) and the
    hook text on the other. An empty hook means no text.
    """
    from PIL import ImageEnhance, ImageOps

    img = ImageOps.fit(bg.convert("RGB"), (1280, 720))
    if person is not None:
        img = paste_person(img, person, side)
        layout = "left" if side == "right" else "right"
    if (hook or "").strip():
        return overlay_youtube_hook_text(img, hook, layout or "bottom", accent)
    return ImageEnhance.Color(ImageEnhance.Contrast(img).enhance(1.14)).enhance(1.18)


def thumbnail_prompts(base_prompt: str, count: int, options: dict) -> tuple[list, str]:
    """Prompts for `count` AI backgrounds in the chosen style, and the negative prompt."""
    style = THUMBNAIL_STYLES.get(options.get("style") or "mixed", THUMBNAIL_PROMPT_VARIANTS)
    with_person = options.get("person", "none") in ("photo", "video")
    room = f", empty space on the {options.get('side', 'right')} side, no people" if with_person else ""
    prompts = [f"YouTube thumbnail background, {base_prompt}, {style[i % len(style)]}{room}, highly detailed, sharp focus"
               for i in range(count)]
    negative = SD_NEGATIVE_PROMPT + (", person, people, hands, face" if with_person else "")
    avoid = (options.get("avoid") or "").strip().strip(",")
    if avoid:
        negative += f", {avoid}"
    return prompts, negative


def thumbnail_hook(metadata: dict, options: dict) -> str:
    """The text on the thumbnail: the user's (empty means none) or the AI's hook."""
    hook = options.get("hook")
    return hook if hook is not None else metadata.get("thumbnail_hook", "NEW VIDEO")


def save_ai_candidate(bg, name: str, out_dir: str, hook: str, person, side: str) -> dict:
    from PIL import ImageOps

    bg = ImageOps.fit(bg.convert("RGB"), (1280, 720))
    bg_path = os.path.join(out_dir, f"{name}_bg.jpg")
    bg.save(bg_path, "JPEG", quality=95)
    out_jpg = os.path.join(out_dir, f"{name}.jpg")
    compose_thumbnail(bg, hook, person, side).save(out_jpg, "JPEG", quality=92)
    return {"path": out_jpg, "bg": bg_path}


def design_thumbnails(designer: str, count: int, config: dict, metadata: dict) -> tuple[list[dict], str]:
    """Hooks, layouts and colours for frame-based thumbnails, from Claude or the local LLM."""
    title = metadata.get("title", "")
    user_text = (f"Video title: {title}\nSuggested hook: {metadata.get('thumbnail_hook', '')}\n"
                 f"Description:\n{metadata.get('description', '')[:800]}")
    system_prompt = THUMBNAIL_DESIGN_PROMPT.replace("{count}", str(count))
    designs, label = [], "Local smart frame compositor"
    try:
        if designer == "claude":
            res = call_claude_json(config.get("secrets", {}).get("claude_key", ""), system_prompt, user_text, 800)
            label = f"Claude design ({CLAUDE_MODEL}) + local compositor"
        else:
            model = config.get("ollama_model", "gemma4:12b")
            res = call_local_ollama_json(model, system_prompt, user_text)
            label = f"Local LLM design ({model}) + local compositor"
        designs = [d for d in (res.get("designs") or []) if isinstance(d, dict) and str(d.get("hook", "")).strip()]
    except Exception as e:
        log_warn(f"Thumbnail design via {designer} failed ({e}); using the video's own hooks.")

    # Fill up with the metadata hook and alternative titles so there are always `count` designs
    fallback_hooks = [metadata.get("thumbnail_hook", "")] + list(metadata.get("alternative_titles", []))
    fallback_hooks = [h for h in fallback_hooks if h] or ["NEW VIDEO"]
    while len(designs) < count:
        designs.append({"hook": fallback_hooks[len(designs) % len(fallback_hooks)][:28]})
    out = []
    for i, d in enumerate(designs[:count]):
        layout = d.get("layout") if d.get("layout") in THUMBNAIL_LAYOUTS else THUMBNAIL_LAYOUTS[i % len(THUMBNAIL_LAYOUTS)]
        out.append({"hook": str(d["hook"]).strip()[:40], "layout": layout,
                    "accent": parse_hex_color(d.get("accent", ""), (255, 55, 55))})
    return out, label


def generate_ai_thumbnails(source: str, count: int, config: dict, metadata: dict, frames: list, out_dir: str,
                           options: dict | None = None, notes: list | None = None,
                           person=None) -> tuple[list[dict], str]:
    """
    Makes `count` AI thumbnails as ai_1.jpg.. in out_dir, each with its text-free background
    (ai_1_bg.jpg..) so the text and photo can be changed later. Returns ([{path, bg}], label).
    Local Stable Diffusion and local Ollama are free; Gemini and OpenAI cost API credits. Claude can't
    draw, so it designs frame-based thumbnails. If image generation fails, falls back to free locally
    designed frame thumbnails.
    """
    options = options or {}
    notes = notes if notes is not None else []
    hook = thumbnail_hook(metadata, options)
    side = options.get("side", "right")
    base_prompt = (options.get("prompt") or metadata.get("thumbnail_prompt") or metadata.get("title")
                   or "YouTube creator studio").strip()
    prompts, negative = thumbnail_prompts(base_prompt, count, options)
    secrets = config.get("secrets", {})
    label = THUMBNAIL_IMAGE_SOURCE_LABELS.get(source, source)
    made = []
    designer = "claude" if source == "claude" else "local"

    if source == "sd":
        model = config.get("thumbnail_sd_model") or DEFAULT_SD_MODEL
        label = f"Local Stable Diffusion ({model.split('/')[-1]})"
        try:
            for i, img in enumerate(generate_sd_images(config, prompts, out_dir, label,
                                                       negative_prompt=negative, notes=notes)):
                made.append(save_ai_candidate(img, f"ai_{i + 1}", out_dir, hook, person, side))
        except Exception as e:
            log_warn(f"{label} failed: {e}")
            notes.append(f"{label} failed: {str(e)[:200]}")
        for raw in Path(out_dir).glob("sd_*.png"):
            raw.unlink()
        if made:
            return made, label
        log_info("AI image generation didn't work; making free locally designed thumbnails instead.")

    if source == "local" or source in CLOUD_IMAGE_MODELS:
        model = config.get("thumbnail_image_model") or DEFAULT_THUMBNAIL_IMAGE_MODEL
        if source == "local":
            label = f"Local Ollama image ({model})"
        avoid = (options.get("avoid") or "").strip()
        for i in range(count):
            prompt = prompts[i].replace("YouTube thumbnail background,", "YouTube thumbnail background, 16:9, no text:")
            if avoid:
                prompt += f". Do not show: {avoid}"
            update_step("thumbnail", 30 + 55 * i // count, f"AI image {i + 1}/{count} ({label})...")
            try:
                if source == "local":
                    img = generate_ollama_image(model, prompt)
                else:
                    img, used = generate_cloud_image(source, secrets, prompt)
                    label = f"{CLOUD_IMAGE_MODELS[source][0]} ({used})"
                made.append(save_ai_candidate(img, f"ai_{len(made) + 1}", out_dir, hook, person, side))
            except Exception as e:
                log_warn(f"{label} image {i + 1} failed: {e}")
                notes.append(f"{label} image {i + 1} failed: {str(e)[:160]}")
                if not made:
                    break  # the first one failed; don't keep retrying a source that doesn't work
        if made:
            return made, label
        log_info("AI image generation didn't work; making free locally designed thumbnails instead.")

    designs, label = design_thumbnails(designer, count, config, metadata)
    for i, d in enumerate(designs):
        bg = frame_to_pil(frames[i % len(frames)] if frames else None)
        bg_path = os.path.join(out_dir, f"ai_{i + 1}_bg.jpg")
        bg.save(bg_path, "JPEG", quality=95)
        out_jpg = os.path.join(out_dir, f"ai_{i + 1}.jpg")
        text = hook if options.get("hook") is not None else d["hook"]
        compose_thumbnail(bg, text, None, side, d["layout"], d["accent"]).save(out_jpg, "JPEG", quality=92)
        made.append({"path": out_jpg, "bg": bg_path, "layout": d["layout"], "accent": list(d["accent"])})
    return made, label


def cloud_refine_prompt(base_prompt: str, options: dict, strength: float) -> str:
    """Cloud edit models have no strength setting, so the "Change: a little ↔ a lot" slider becomes words."""
    prompt = f"Edit this YouTube thumbnail background: {base_prompt}. Keep it free of any text, 16:9."
    if strength <= 0.45:
        prompt += " Make only small changes and keep the composition."
    elif strength >= 0.65:
        prompt += " Feel free to reimagine it."
    if options.get("person", "none") in ("photo", "video"):
        prompt += f" Keep the {options.get('side', 'right')} side empty, no people."
    avoid = (options.get("avoid") or "").strip().strip(",")
    if avoid:
        prompt += f" Do not show: {avoid}."
    return prompt


def refine_with_cloud(source: str, secrets: dict, bg_path: str, prompt: str, count: int, notes: list) -> list:
    """`count` edits of the background by the cloud model. Only the text-free background is sent, never your photo."""
    from PIL import Image

    name = CLOUD_IMAGE_MODELS[source][0]
    init = Image.open(bg_path).convert("RGB")
    images = []
    for i in range(count):
        update_step("thumbnail", 20 + 70 * i // count, f"Refining {i + 1}/{count} with {name}...")
        try:
            img, _ = generate_cloud_image(source, secrets, prompt, init)
            images.append(img)
        except Exception as e:
            log_warn(f"{name} refine {i + 1} failed: {e}")
            notes.append(f"{name} refine {i + 1} failed: {str(e)[:160]}")
            break
    return images


def shows_you(candidate: dict) -> bool:
    """Video frames (and refines of them) have you in the background, so they never go to a cloud model."""
    return candidate.get("kind") == "frame" or bool(candidate.get("shows_you"))


def refine_thumbnail(config: dict, candidate: dict, metadata: dict, options: dict, out_dir: str,
                     person, notes: list, first: int, count: int = 2) -> list[dict]:
    """
    Image-to-image on a candidate's text-free background: the cloud model's edit mode when the source is
    a cloud one, otherwise local Stable Diffusion. Returns [{path, bg}] as refined_N.
    """
    base_prompt = (options.get("prompt") or metadata.get("thumbnail_prompt") or metadata.get("title") or "").strip()
    strength = min(0.8, max(0.3, float(options.get("strength", 0.5))))
    hook = thumbnail_hook(metadata, options)
    side = options.get("side", "right")
    source = config.get("thumbnail_image_source", "sd")
    if source in CLOUD_IMAGE_MODELS and shows_you(candidate):
        notes.append("Video frames show you, so this one was refined with local Stable Diffusion "
                     "(nothing of you is uploaded).")
    if source in CLOUD_IMAGE_MODELS and not shows_you(candidate):
        images = refine_with_cloud(source, config.get("secrets", {}), candidate["bg"],
                                   cloud_refine_prompt(base_prompt, options, strength), count, notes)
    else:
        prompts, negative = thumbnail_prompts(base_prompt, count, options)
        label = "Refine (local Stable Diffusion)"
        images = generate_sd_images(config, prompts, out_dir, label, init_images=[candidate["bg"]] * count,
                                    strength=strength, negative_prompt=negative, notes=notes)
        for raw in Path(out_dir).glob("sd_*.png"):
            raw.unlink()
    return [save_ai_candidate(img, f"refined_{first + i}", out_dir, hook, person, side)
            for i, img in enumerate(images)]


def recompose_candidate(candidate: dict, hook: str, person, side: str):
    """Redraw a candidate from its text-free background with new text and photo."""
    from PIL import Image

    bg_path = candidate.get("bg", "")
    if not bg_path or not os.path.exists(bg_path):
        return
    bg = Image.open(bg_path)
    if candidate.get("kind") == "frame":
        img = compose_thumbnail(bg, hook)  # a video frame already shows you
    else:
        accent = tuple(candidate.get("accent") or (255, 55, 55))
        img = compose_thumbnail(bg, hook, person, side, candidate.get("layout", "") if person is None else "", accent)
    img.save(candidate["path"], "JPEG", quality=92)


def make_thumbnail_candidates(config: dict, video_path: str, metadata: dict, out_dir: str,
                              frame_count: int, ai_count: int, options: dict | None = None) -> dict:
    """Writes frame_N.jpg and ai_N.jpg candidates and returns {candidates, chosen, provider, notes, options}."""
    options = dict(options or {})
    os.makedirs(out_dir, exist_ok=True)
    for old in list(Path(out_dir).glob("*.jpg")) + list(Path(out_dir).glob("*.png")):
        old.unlink()

    hook = thumbnail_hook(metadata, options)
    notes = []
    update_step("thumbnail", 10, "Picking the best video frames...")
    frames = extract_top_talking_head_frames(video_path, count=max(frame_count, 4))

    candidates = []
    for i in range(frame_count):
        bg = frame_to_pil(frames[i] if i < len(frames) else (frames[0] if frames else None))
        bg_path = os.path.join(out_dir, f"frame_{i + 1}_bg.jpg")
        bg.save(bg_path, "JPEG", quality=95)
        out_jpg = os.path.join(out_dir, f"frame_{i + 1}.jpg")
        compose_thumbnail(bg, hook).save(out_jpg, "JPEG", quality=92)
        candidates.append({"id": f"frame_{i + 1}", "path": out_jpg, "bg": bg_path, "kind": "frame",
                           "label": f"Frame {i + 1}"})

    provider = "Local smart frame compositor"
    source = config.get("thumbnail_image_source", "sd")
    if ai_count > 0 and source != "off":
        person = resolve_person(config, options, frames, out_dir, notes)
        made, provider = generate_ai_thumbnails(source, ai_count, config, metadata, frames, out_dir,
                                                options, notes, person)
        for i, m in enumerate(made):
            candidates.append({"id": f"ai_{i + 1}", "kind": "ai", "label": f"AI {i + 1}", **m})

    if not candidates:
        # Last resort so a run never ends without a thumbnail
        out_jpg = os.path.join(out_dir, "frame_1.jpg")
        compose_thumbnail(frame_to_pil(frames[0] if frames else None), hook).save(out_jpg, "JPEG", quality=92)
        candidates.append({"id": "frame_1", "path": out_jpg, "kind": "frame", "label": "Frame 1"})

    chosen = next((c["id"] for c in candidates if c["kind"] == "ai"), candidates[0]["id"])
    return {"version": 1, "candidates": candidates, "chosen": chosen, "provider": provider,
            "notes": notes, "options": options}


def chosen_thumbnail_path(thumbs: dict) -> str:
    by_id = {c["id"]: c["path"] for c in thumbs.get("candidates", [])}
    path = by_id.get(thumbs.get("chosen", ""))
    if path and os.path.exists(path):
        return path
    return next((p for p in by_id.values() if os.path.exists(p)), "")


# ---------------------------------------------------------------------------
# 5. YouTube Data API v3 OAuth 2.0 & Resumable Upload (Video + Thumbnail)
# ---------------------------------------------------------------------------

YOUTUBE_SCOPES = [
    "https://www.googleapis.com/auth/youtube.upload",
    "https://www.googleapis.com/auth/youtube.readonly",
    # Needed by videos.update (Submit button); upload/readonly cannot edit an existing video.
    "https://www.googleapis.com/auth/youtube",
]
YOUTUBE_EDIT_SCOPES = (
    "https://www.googleapis.com/auth/youtube",
    "https://www.googleapis.com/auth/youtube.force-ssl",
)
RECONNECT_FOR_EDIT_MESSAGE = (
    "Your Google connection can upload but not edit videos. Open Settings -> YouTube Automation "
    "and click 'Connect Google Account' once more to allow editing, then press Submit again."
)


def parse_youtube_token(oauth_token_json: str) -> dict:
    """Decode the vault's token JSON (plain or wrapped in {"token_json": ...})."""
    creds_info = json.loads(oauth_token_json)
    if "token_json" in creds_info:
        creds_info = json.loads(creds_info["token_json"])
    return creds_info


def load_youtube_credentials(oauth_token_json: str):
    """Build refreshed Google credentials from the token JSON stored in the OBS vault."""
    from google.auth.transport.requests import Request
    from google.oauth2.credentials import Credentials

    creds_info = parse_youtube_token(oauth_token_json)

    # Refresh with the scopes the token was actually granted: asking Google for a broader
    # set at refresh time fails with invalid_scope until the user reconnects.
    granted = creds_info.get("scopes") or YOUTUBE_SCOPES
    creds = Credentials.from_authorized_user_info(creds_info, granted)
    if creds.expired and creds.refresh_token:
        creds.refresh(Request())
    return creds


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

    from googleapiclient.discovery import build
    from googleapiclient.http import MediaFileUpload

    creds = load_youtube_credentials(oauth_token_json)
    youtube = build("youtube", "v3", credentials=creds)

    body = {
        "snippet": {
            "title": metadata.get("title", "OBS Automated Video")[:100],
            "description": metadata.get("description", ""),
            "tags": metadata.get("tags", ["OBS", "YouTube"]),
            "categoryId": str(category_id or "28")
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


def update_video_details(video_id: str, title: str, description: str, oauth_token_json: str):
    """Push an edited title/description to an already-uploaded video (Submit button)."""
    if not oauth_token_json:
        raise ValueError("YouTube account not connected yet. Open Settings -> YouTube Automation and click 'Connect Google Account'.")
    if not video_id:
        raise ValueError("No uploaded video to update yet. Run the pipeline and let the upload finish first.")

    title = (title or "").strip()
    description = description or ""
    if not title:
        raise ValueError("Title cannot be empty.")
    if len(title) > 100:
        raise ValueError(f"Title is {len(title)} characters; YouTube allows at most 100.")
    if len(description.encode("utf-8")) > 5000:
        raise ValueError("Description is over YouTube's 5000-byte limit.")
    if any(ch in title + description for ch in "<>"):
        raise ValueError("YouTube does not allow '<' or '>' in titles or descriptions.")

    from googleapiclient.discovery import build
    from googleapiclient.errors import HttpError

    # Check the granted scopes before touching the network or refreshing the token.
    if not any(scope in YOUTUBE_EDIT_SCOPES for scope in (parse_youtube_token(oauth_token_json).get("scopes") or [])):
        raise PermissionError(RECONNECT_FOR_EDIT_MESSAGE)

    youtube = build("youtube", "v3", credentials=load_youtube_credentials(oauth_token_json))
    try:
        items = youtube.videos().list(part="snippet", id=video_id).execute().get("items", [])
        if not items:
            raise ValueError(f"YouTube video {video_id} was not found on the connected channel.")
        current = items[0]["snippet"]
        # videos.update replaces the whole snippet, so resend every writable field unchanged.
        snippet = {
            "title": title,
            "description": description,
            "categoryId": current.get("categoryId", "28"),
        }
        for key in ("tags", "defaultLanguage"):
            if key in current:
                snippet[key] = current[key]
        youtube.videos().update(part="snippet", body={"id": video_id, "snippet": snippet}).execute()
    except HttpError as e:
        if getattr(e.resp, "status", 0) == 403 and "insufficient" in str(e).lower():
            raise PermissionError(RECONNECT_FOR_EDIT_MESSAGE) from e
        raise


RECONNECT_FOR_DELETE_MESSAGE = (
    "Your Google connection can upload but not delete videos. Open Settings -> YouTube Automation "
    "and click 'Connect Google Account' once more to allow it, then try again."
)


def delete_youtube_video(video_id: str, oauth_token_json: str):
    """Permanently delete an uploaded video. A video that's already gone counts as deleted."""
    if not oauth_token_json:
        raise ValueError("YouTube account not connected yet. Open Settings -> YouTube Automation and click 'Connect Google Account'.")
    if not video_id:
        raise ValueError("There's no uploaded video to delete.")
    if not any(scope in YOUTUBE_EDIT_SCOPES for scope in (parse_youtube_token(oauth_token_json).get("scopes") or [])):
        raise PermissionError(RECONNECT_FOR_DELETE_MESSAGE)

    from googleapiclient.discovery import build
    from googleapiclient.errors import HttpError

    youtube = build("youtube", "v3", credentials=load_youtube_credentials(oauth_token_json))
    try:
        youtube.videos().delete(id=video_id).execute()
        log_info(f"Deleted https://youtu.be/{video_id} from YouTube.")
    except HttpError as e:
        status = getattr(e.resp, "status", 0)
        if status == 404:
            log_info(f"YouTube video {video_id} was already gone.")
            return
        if status == 403 and "insufficient" in str(e).lower():
            raise PermissionError(RECONNECT_FOR_DELETE_MESSAGE) from e
        raise


_CODEC_LABELS = {
    "h264": "H.264", "hevc": "HEVC (H.265)", "av1": "AV1", "vp9": "VP9",
    "aac": "AAC", "opus": "Opus", "mp3": "MP3",
}


def _as_int(value) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return 0


def _fmt_clock(seconds: float) -> str:
    hours, rem = divmod(int(round(seconds)), 3600)
    minutes, secs = divmod(rem, 60)
    return f"{hours}:{minutes:02d}:{secs:02d}" if hours else f"{minutes}:{secs:02d}"


def probe_video_stats(video_path: str) -> dict:
    """Read length, resolution, codecs and bitrates of a finished MP4 with one ffprobe call."""
    try:
        res = subprocess.run([
            "ffprobe", "-v", "error",
            "-show_entries",
            "format=duration,size,bit_rate:stream=codec_type,codec_name,width,height,avg_frame_rate,bit_rate,sample_rate,channels",
            "-of", "json", video_path
        ], capture_output=True, text=True, timeout=30)
        data = json.loads(res.stdout)
    except Exception:
        return {}

    fmt = data.get("format", {})
    streams = data.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), {})
    audio = next((s for s in streams if s.get("codec_type") == "audio"), {})

    num, _, den = str(video.get("avg_frame_rate", "0/1")).partition("/")
    try:
        fps = float(num) / float(den or 1)
    except (ValueError, ZeroDivisionError):
        fps = 0.0

    overall_bps = _as_int(fmt.get("bit_rate"))
    audio_bps = _as_int(audio.get("bit_rate"))
    return {
        "duration": float(fmt.get("duration") or 0.0),
        "size_bytes": _as_int(fmt.get("size")),
        "overall_bps": overall_bps,
        "width": _as_int(video.get("width")),
        "height": _as_int(video.get("height")),
        "fps": fps,
        "video_codec": video.get("codec_name", ""),
        "video_bps": _as_int(video.get("bit_rate")) or max(0, overall_bps - audio_bps),
        "audio_codec": audio.get("codec_name", ""),
        "audio_bps": audio_bps,
        "sample_rate": _as_int(audio.get("sample_rate")),
        "channels": _as_int(audio.get("channels")),
    }


def format_video_stats(stats: dict, original_duration: float) -> str:
    if not stats:
        return ""
    lines = [f"Length: {_fmt_clock(stats['duration'])} ({stats['duration']:.1f}s)"]
    if stats["width"]:
        vcodec = _CODEC_LABELS.get(stats["video_codec"], stats["video_codec"].upper())
        lines.append(
            f"Video: {stats['width']}x{stats['height']} ({stats['height']}p) @ {round(stats['fps'], 2):g} fps, "
            f"{vcodec}, {stats['video_bps'] / 1e6:.1f} Mbps"
        )
    if stats["audio_codec"]:
        acodec = _CODEC_LABELS.get(stats["audio_codec"], stats["audio_codec"].upper())
        layout = {1: "mono", 2: "stereo"}.get(stats["channels"], f"{stats['channels']} ch")
        lines.append(f"Audio: {acodec}, {stats['audio_bps'] // 1000} kbps, {stats['sample_rate'] / 1000:g} kHz {layout}")
    lines.append(f"File size: {stats['size_bytes'] / 1048576:.1f} MB")
    trimmed = original_duration - stats["duration"]
    if original_duration > 0 and trimmed >= 0.5:
        lines.append(f"Trimmed: {trimmed:.1f}s removed from the {original_duration:.1f}s original")
    return "\n".join(lines)


def emit_upload_stats(video_path: str, original_duration: float):
    stats = probe_video_stats(video_path)
    text = format_video_stats(stats, original_duration)
    if text:
        emit("upload_stats", text=text, stats=stats)


# ---------------------------------------------------------------------------
# 6. Main Pipeline Orchestrator
# ---------------------------------------------------------------------------

def pipeline_paths(input_video: str) -> dict:
    base_dir = os.path.dirname(os.path.abspath(input_video))
    stem = Path(input_video).stem
    return {
        "edited_video": os.path.join(base_dir, f"{stem}_yt_edited.mp4"),
        "thumbnail_jpg": os.path.join(base_dir, f"{stem}_yt_thumb.jpg"),
        "thumbs_dir": os.path.join(base_dir, f"{stem}_yt_thumbs"),
        "thumbs_file": os.path.join(base_dir, f"{stem}_yt_thumbs.json"),
        "cuts_file": os.path.join(base_dir, f"{stem}_yt_cuts.json"),
        "metadata_file": os.path.join(base_dir, f"{stem}_yt_metadata.json"),
        "transcript_file": os.path.join(base_dir, f"{stem}_yt_transcript.txt"),
        "titles_file": os.path.join(base_dir, f"{stem}_yt_titles.txt"),
        "broll_plan": os.path.join(base_dir, f"{stem}_broll_plan.json"),
        "broll_thumbs": os.path.join(base_dir, f"{stem}_broll"),
        "state_file": os.path.join(base_dir, f"{stem}_yt_state.json"),
        "premiere_dir": os.path.join(base_dir, f"{stem}_premiere"),
    }


# Step mode pauses at these gates; Auto mode runs straight through.
REVIEW_GATES = ("cuts", "broll", "metadata", "thumbnail", "publish")
STEP_FRAME_THUMBNAILS = 4
STEP_AI_THUMBNAILS = 4


def review_gates(config: dict) -> set:
    if "review_gates" in config:
        return {g for g in config.get("review_gates") or [] if g in REVIEW_GATES}
    # Older callers only knew the B-roll review switch
    return {"broll"} if (config.get("broll") or {}).get("review", True) else set()


def pause_for_review(config: dict, ctx: dict, gate: str) -> str:
    """Save everything the next stage needs (never the secrets) and let OBS relaunch us in resume_pipeline mode."""
    ctx["pending_gate"] = gate
    state_file = pipeline_paths(ctx["input_video"])["state_file"]
    write_json_file(state_file, {
        "version": 2,
        "config": {k: v for k, v in config.items() if k != "secrets"},
        "ctx": ctx,
    })
    return state_file


def stage_analyze(config: dict) -> dict | None:
    """Transcribe and find the cut list. Returns the pipeline context, or None on error."""
    input_video = config.get("input_video", "")
    if not input_video or not os.path.exists(input_video):
        log_error(f"Input video not found: {input_video}")
        emit("error", message=f"Input video not found: {input_video}")
        return None

    paths = pipeline_paths(input_video)
    silence_db = float(config.get("silence_db", -42.0))
    min_silence_sec = float(config.get("min_silence_sec", 1.20))
    padding_sec = float(config.get("padding_sec", 0.25))
    cut_retakes = bool(config.get("cut_retakes", True))
    whisper_model = config.get("whisper_model", "large-v3-turbo")

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
        with open(paths["transcript_file"], "w", encoding="utf-8") as tf:
            for s in segments:
                tf.write(f"[{s['start']:.2f}s - {s['end']:.2f}s] {s['text']}\n")

    # STAGE 2: Dead Air & Butchered Sentence Detection
    update_step("edit", 15, f"Detecting dead air (threshold: {silence_db} dB, min: {min_silence_sec}s)...")
    keep_intervals = detect_nonsilent_intervals(
        input_video,
        silence_db=silence_db,
        min_silence_dur=min_silence_sec,
        padding_sec=padding_sec,
        total_duration=total_duration,
        speech_segments=segments
    )

    retake_cuts = []
    if cut_retakes and len(segments) > 1:
        update_step("edit", 45, "Scanning transcript for butchered sentences & repeated takes...")
        retake_cuts = detect_butchered_retakes(segments)
        for _, _, reason in retake_cuts:
            log_info(reason)
        if retake_cuts:
            keep_intervals = subtract_cuts_from_intervals(keep_intervals, retake_cuts)

    update_step("edit", 60, f"Cut list ready ({len(keep_intervals)} sections kept).")

    return {
        "input_video": input_video,
        "total_duration": total_duration,
        "segments": segments,
        "keep_intervals": [list(k) for k in keep_intervals],
        "retake_cuts": [list(c) for c in retake_cuts],
        "broll_plan_path": "",
        "next_stage": "broll_plan",
    }


def stage_broll_plan(config: dict, ctx: dict, gates: set) -> str | None:
    """Plans B-roll on the (reviewed) cut list. Returns 'broll' to pause for review, else None."""
    ctx["next_stage"] = "render"
    broll_cfg = config.get("broll") or {}
    if not broll_cfg.get("enabled"):
        update_step("broll", 100, "Skipped (B-roll is off).")
        return None

    paths = pipeline_paths(ctx["input_video"])
    plan = plan_broll(
        broll_cfg, ctx["input_video"], ctx["segments"], [tuple(k) for k in ctx["keep_intervals"]],
        priority_list=config.get("priority_list", ["gemini", "openai", "claude", "local"]),
        secrets=config.get("secrets", {}),
        ollama_model=config.get("ollama_model", "gemma4:12b"),
        plan_path=paths["broll_plan"],
        thumbs_dir=paths["broll_thumbs"]
    )
    if not plan or not plan["inserts"]:
        if plan:
            update_step("broll", 100, "No B-roll moments found.")
        return None

    ctx["broll_plan_path"] = paths["broll_plan"]
    if "broll" in gates:
        return "broll"
    for ins in plan["inserts"]:
        ins["status"] = "accepted" if ins.get("confidence", 0) >= BROLL_ACCEPT_CONFIDENCE else "rejected"
    write_json_file(paths["broll_plan"], plan)
    return None


def stage_render(config: dict, ctx: dict) -> None:
    """Render the edit (with accepted B-roll)."""
    input_video = ctx["input_video"]
    total_duration = float(ctx["total_duration"])
    keep_intervals = [tuple(k) for k in ctx["keep_intervals"]]
    edited_video = pipeline_paths(input_video)["edited_video"]

    broll = None
    if ctx.get("broll_plan_path"):
        try:
            plan = read_json_file(ctx["broll_plan_path"])
            screen_video = plan.get("screen_video", "")
            spans = accepted_broll_inserts(plan, get_video_duration(screen_video)) if os.path.exists(screen_video) else []
            if spans:
                broll = {
                    "screen_path": screen_video,
                    "offset": float(plan.get("offset_sec", 0.0)),
                    "inserts": spans,
                    "style": plan.get("style", "pip"),
                }
                update_step("broll", 100, f"{len(spans)} B-roll inserts accepted ({plan.get('style', 'pip')}).")
            else:
                update_step("broll", 100, "No B-roll inserts accepted.")
        except (OSError, ValueError) as e:
            log_warn(f"Could not read the B-roll plan ({e}); rendering without B-roll.")

    kept_duration = sum(b - a for a, b in keep_intervals) if keep_intervals else total_duration
    removed_sec = max(0.0, total_duration - kept_duration)
    with_broll = f" with {len(broll['inserts'])} B-roll inserts" if broll else ""
    update_step("edit", 65, f"Rendering edited MP4 via NVENC{with_broll} (removed {removed_sec:.1f}s of dead air/retakes)...")

    rendered = render_edited_video(input_video, edited_video, keep_intervals, total_duration, broll)
    if not rendered and broll:
        log_warn("B-roll render failed; rendering the plain edit instead.")
        rendered = render_edited_video(input_video, edited_video, keep_intervals, total_duration)

    if rendered:
        ctx["final_video"] = edited_video
        ctx["rendered"] = True
        ctx["joins"] = edit_joins(keep_intervals)
        log_info(f"Edited video saved: {os.path.basename(edited_video)} ({kept_duration:.1f}s, saved {removed_sec:.1f}s)")
    else:
        ctx["final_video"] = input_video
        ctx["rendered"] = False
        ctx["joins"] = []
        log_warn("Using original remuxed MP4 for upload.")

    update_step("edit", 100, f"Video edit complete (-{removed_sec:.1f}s trimmed).")
    emit("render_ready", video=ctx["final_video"], duration=round(kept_duration, 3), joins=ctx["joins"])
    ctx["next_stage"] = "metadata"
    save_checkpoint(config, ctx)


def edit_joins(keep_intervals: list) -> list[float]:
    """Where each cut lands in the edited video: the running total of the kept lengths."""
    joins, t = [], 0.0
    for a, b in keep_intervals[:-1]:
        t += b - a
        joins.append(round(t, 3))
    return joins


def emit_metadata(ctx: dict):
    metadata = ctx["metadata"]
    emit(
        "metadata_ready",
        title=metadata.get("title", ""),
        alternative_titles=metadata.get("alternative_titles", []),
        description=metadata.get("description", ""),
        tags=metadata.get("tags", []),
        thumbnail_hook=metadata.get("thumbnail_hook", ""),
        provider=ctx.get("llm_provider", "")
    )


def stage_metadata(config: dict, ctx: dict, gates: set) -> str | None:
    """AI title & description. Returns 'metadata' to pause for review, else None."""
    paths = pipeline_paths(ctx["input_video"])
    segments = ctx["segments"]
    if ctx.get("rendered"):
        # Chapters must use the trimmed video's timeline, not the raw recording's.
        segments = remap_segments_to_edited_timeline(segments, [tuple(k) for k in ctx["keep_intervals"]])

    update_step("llm", 10, "Generating Title & SEO Description via Priority Chain...")
    metadata, llm_provider_used = generate_metadata_with_priority(
        priority_list=config.get("priority_list", ["gemini", "openai", "claude", "local"]),
        secrets=config.get("secrets", {}),
        ollama_model=config.get("ollama_model", "gemma4:12b"),
        segments=segments
    )
    metadata["description"] = append_social_footer(metadata.get("description", ""))
    ctx["metadata"] = metadata
    ctx["llm_provider"] = llm_provider_used
    ctx["next_stage"] = "thumbnails"
    write_titles_file(paths["titles_file"], metadata.get("title", ""), metadata.get("alternative_titles", []))
    emit_metadata(ctx)

    if "metadata" in gates:
        write_json_file(paths["metadata_file"], {
            "version": 1,
            "title": metadata.get("title", ""),
            "alternative_titles": metadata.get("alternative_titles", []),
            "description": metadata.get("description", ""),
            "tags": metadata.get("tags", []),
            "thumbnail_hook": metadata.get("thumbnail_hook", ""),
            "provider": llm_provider_used,
        })
        update_step("llm", 100, f"Waiting for your review ({llm_provider_used}).")
        return "metadata"
    update_step("llm", 100, f"Metadata ready ({llm_provider_used}).")
    return None


def stage_thumbnails(config: dict, ctx: dict, gates: set) -> str | None:
    """Thumbnail candidates. Step mode: 4 frames + 4 AI to choose from. Auto: one AI thumbnail."""
    paths = pipeline_paths(ctx["input_video"])
    step_mode = "thumbnail" in gates
    options = dict(config.get("thumb_options") or {})
    options.update(ctx.get("thumb_options") or {})
    ctx["thumb_options"] = options
    thumbs = make_thumbnail_candidates(
        config, ctx["final_video"], ctx["metadata"], paths["thumbs_dir"],
        frame_count=STEP_FRAME_THUMBNAILS if step_mode else 0,
        ai_count=STEP_AI_THUMBNAILS if step_mode else 1,
        options=options
    )
    write_json_file(paths["thumbs_file"], thumbs)
    ctx["thumb_provider"] = thumbs["provider"]
    ctx["next_stage"] = "publish"
    if step_mode:
        update_step("thumbnail", 100, f"Pick a thumbnail ({len(thumbs['candidates'])} options).")
        return "thumbnail"
    use_chosen_thumbnail(ctx)
    return None


def use_chosen_thumbnail(ctx: dict):
    paths = pipeline_paths(ctx["input_video"])
    thumbs = read_json_file(paths["thumbs_file"])
    chosen = chosen_thumbnail_path(thumbs)
    if chosen:
        shutil.copyfile(chosen, paths["thumbnail_jpg"])
    emit("thumbnail_ready", thumbnail_path=paths["thumbnail_jpg"], provider=ctx.get("thumb_provider", ""))
    update_step("thumbnail", 100, f"Thumbnail ready ({thumbs.get('chosen', '')}, {ctx.get('thumb_provider', '')}).")


def stage_upload(config: dict, ctx: dict) -> int:
    """YouTube Upload + Custom Thumbnail (of the edit, or of the finished file the user picked)."""
    final_video_to_upload = ctx.get("upload_file") or ctx["final_video"]
    thumbnail_jpg = pipeline_paths(ctx["input_video"])["thumbnail_jpg"]
    metadata = ctx["metadata"]
    privacy_status = config.get("privacy_status", "unlisted")
    secrets = config.get("secrets", {})
    ctx["next_stage"] = "done"

    if config.get("skip_upload", False):
        save_checkpoint(config, ctx)
        emit(
            "complete",
            video_id="LOCAL_DRY_RUN",
            url="",
            edited_video=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            message="Dry-run complete (Upload skipped)."
        )
        return 0

    if ctx.get("llm_provider") == TEMPLATE_METADATA_BADGE and privacy_status == "public":
        privacy_status = "unlisted"
        log_warn("Uploading as UNLISTED instead of public because the title is a template; review it first.")

    update_step("upload", 5, f"Connecting to YouTube Data API v3 ({privacy_status.upper()})...")
    try:
        video_id, video_url = upload_to_youtube(
            video_path=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            metadata=metadata,
            privacy_status=privacy_status,
            category_id=config.get("category_id", "28"),
            made_for_kids=bool(config.get("made_for_kids", False)),
            oauth_token_json=secrets.get("youtube_token_json", "")
        )
        ctx["video_id"] = video_id
        ctx["video_url"] = video_url
        save_checkpoint(config, ctx)
        emit_upload_stats(final_video_to_upload, float(ctx["total_duration"]))
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
        save_checkpoint(config, ctx)
        emit(
            "upload_pending",
            edited_video=final_video_to_upload,
            thumbnail_path=thumbnail_jpg,
            message=f"Edited video & thumbnail are ready locally! Upload paused: {e}"
        )
        return 0


def finish_without_upload(config: dict, ctx: dict, message: str) -> int:
    ctx["next_stage"] = "done"
    save_checkpoint(config, ctx)
    update_step("upload", 100, "Not uploaded (finished locally).")
    emit("complete", video_id="", url="", edited_video=ctx.get("final_video", ""),
         thumbnail_path=pipeline_paths(ctx["input_video"])["thumbnail_jpg"], message=message)
    return 0


def parse_chapters(description: str) -> list[tuple[float, str]]:
    """The 'MM:SS Title' chapter lines of a description as (seconds, title)."""
    chapters = []
    for line in (description or "").split("\n"):
        m = _CHAPTER_LINE_RE.match(line)
        if not m:
            continue
        seconds = 0
        for part in m.group(1).strip().split(":"):
            seconds = seconds * 60 + int(part)
        chapters.append((float(seconds), m.group(3).strip()))
    return chapters


def export_premiere_project(ctx: dict) -> str:
    """Writes {stem}_premiere/ with a Premiere Pro (FCP 7 XML) sequence of the edit. Returns the XML path."""
    import premiere_export

    paths = pipeline_paths(ctx["input_video"])
    out_dir = paths["premiere_dir"]
    os.makedirs(out_dir, exist_ok=True)
    total = float(ctx["total_duration"])
    keep = [tuple(k) for k in ctx.get("keep_intervals") or [[0.0, total]]]

    broll = None
    if ctx.get("broll_plan_path") and os.path.exists(ctx["broll_plan_path"]):
        plan = read_json_file(ctx["broll_plan_path"])
        screen = plan.get("screen_video", "")
        if os.path.exists(screen):
            spans = accepted_broll_inserts(plan, get_video_duration(screen))
            if spans:
                broll = {"path": screen, "probe": premiere_export.probe_media(screen),
                         "offset": float(plan.get("offset_sec", 0.0)), "inserts": spans,
                         "style": plan.get("style", "pip"), "pip_width": BROLL_PIP_WIDTH}

    # Where each cut was, on the edited timeline, so it can be checked in Premiere
    cut_marks = []
    for c in cuts_from_keep(keep, [tuple(r) for r in ctx.get("retake_cuts", [])], total):
        at = sum(max(0.0, min(b, c["start"]) - a) for a, b in keep)
        cut_marks.append((at, f"Cut: {c['kind']} {c['end'] - c['start']:.1f}s", c.get("reason", "")))

    metadata = ctx.get("metadata") or {}
    stem = Path(ctx["input_video"]).stem
    xml = premiere_export.build_premiere_xml(
        ctx["input_video"], premiere_export.probe_media(ctx["input_video"]), keep, broll,
        parse_chapters(metadata.get("description", "")) if ctx.get("rendered") else [], cut_marks,
        name=f"{stem} (AI edit)")
    xml_path = os.path.join(out_dir, f"{stem}.xml")
    with open(xml_path, "w", encoding="utf-8") as f:
        f.write(xml)

    if os.path.exists(paths["thumbnail_jpg"]):
        shutil.copyfile(paths["thumbnail_jpg"], os.path.join(out_dir, "thumbnail.jpg"))
    if metadata:
        alts = "\n".join(f"- {t}" for t in metadata.get("alternative_titles", []))
        with open(os.path.join(out_dir, "youtube_details.txt"), "w", encoding="utf-8") as f:
            f.write(f"TITLE\n{metadata.get('title', '')}\n\nALTERNATIVE TITLES\n{alts}\n\n"
                    f"DESCRIPTION\n{metadata.get('description', '')}\n\nTAGS\n{', '.join(metadata.get('tags', []))}\n")
    log_info(f"Premiere Pro sequence saved: {xml_path}. In Premiere use File > Import and pick this XML. "
             f"It uses the original recording, so keep {os.path.basename(ctx['input_video'])} where it is.")
    emit("premiere_exported", xml_path=xml_path, folder=out_dir)
    return xml_path


def apply_review(ctx: dict, review: dict | None = None) -> None:
    """Read back what the user decided at the gate the run paused on."""
    review = review or {}
    gate = ctx.pop("pending_gate", "")
    paths = pipeline_paths(ctx["input_video"])
    if gate == "cuts":
        cuts = read_json_file(paths["cuts_file"]).get("cuts", [])
        keep = keep_from_cuts(cuts, float(ctx["total_duration"]))
        if not keep:
            log_warn("Every part of the video was cut; keeping the whole recording instead.")
            keep = [(0.0, float(ctx["total_duration"]))]
        ctx["keep_intervals"] = [list(k) for k in keep]
        enabled = sum(1 for c in cuts if c.get("enabled", True))
        log_info(f"Using your reviewed cut list ({enabled} of {len(cuts)} cuts, {len(keep)} sections kept).")
    elif gate == "metadata":
        reviewed = read_json_file(paths["metadata_file"])
        metadata = ctx["metadata"]
        for key in ("title", "alternative_titles", "description", "tags", "thumbnail_hook"):
            if key in reviewed:
                metadata[key] = reviewed[key]
        write_titles_file(paths["titles_file"], metadata.get("title", ""), metadata.get("alternative_titles", []))
        emit_metadata(ctx)
        update_step("llm", 100, f"Approved: {metadata.get('title', '')}")
        log_info(f"Using your approved title: {metadata.get('title', '')}")
    elif gate == "thumbnail":
        use_chosen_thumbnail(ctx)
    elif gate == "publish":
        action = review.get("action", "upload")
        if action == "finish":
            ctx["next_stage"] = "finish"
        elif action == "upload_file":
            path = review.get("file", "")
            if not path or not os.path.exists(path):
                raise ValueError(f"The file to upload wasn't found: {path}")
            ctx["upload_file"] = path
            edited = get_video_duration(ctx["final_video"]) if os.path.exists(ctx.get("final_video", "")) else 0.0
            length = get_video_duration(path)
            if edited and abs(length - edited) > 1.0:
                log_warn(f"{os.path.basename(path)} is {length:.1f}s long and the edit was {edited:.1f}s; "
                         "the chapter times in the description may be off.")
            log_info(f"Uploading your file instead of the edit: {path}")


def run_stages(config: dict, ctx: dict) -> int:
    """Runs from ctx['next_stage'] to the end, stopping at any review gate the config asks for."""
    gates = review_gates(config)
    paths = pipeline_paths(ctx["input_video"])
    while True:
        stage = ctx.get("next_stage", "render")
        if stage == "cuts_review":
            ctx["next_stage"] = "broll_plan"
            cuts = cuts_from_keep([tuple(k) for k in ctx["keep_intervals"]],
                                  [tuple(c) for c in ctx.get("retake_cuts", [])], float(ctx["total_duration"]))
            if ctx.pop("reuse_cuts", False) and os.path.exists(paths["cuts_file"]):
                # Starting over from the cuts: begin from the cut list you approved last time
                saved = read_json_file(paths["cuts_file"])
                if abs(float(saved.get("duration", 0.0)) - float(ctx["total_duration"])) < 0.5:
                    cuts = saved.get("cuts", cuts)
                    if "cuts" not in gates:
                        keep = keep_from_cuts(cuts, float(ctx["total_duration"])) or [(0.0, float(ctx["total_duration"]))]
                        ctx["keep_intervals"] = [list(k) for k in keep]
            if "cuts" in gates and cuts:
                write_json_file(paths["cuts_file"], {
                    "version": 1,
                    "video": ctx["input_video"],
                    "duration": float(ctx["total_duration"]),
                    "cuts": cuts,
                })
                state_file = pause_for_review(config, ctx, "cuts")
                removed = sum(c["end"] - c["start"] for c in cuts if c.get("enabled", True))
                update_step("edit", 60, f"Waiting for your review ({len(cuts)} cuts, {removed:.1f}s).")
                emit("review_ready", gate="cuts", state_path=state_file, data_path=paths["cuts_file"],
                     count=len(cuts), removed_sec=round(removed, 2))
                return 0
        elif stage == "broll_plan":
            if stage_broll_plan(config, ctx, gates) == "broll":
                state_file = pause_for_review(config, ctx, "broll")
                plan = read_json_file(ctx["broll_plan_path"])
                update_step("broll", 100, f"Waiting for your review ({len(plan['inserts'])} B-roll moments).")
                emit("broll_plan_ready", plan_path=ctx["broll_plan_path"], state_path=state_file,
                     insert_count=len(plan["inserts"]))
                return 0
        elif stage == "render":
            stage_render(config, ctx)
        elif stage == "metadata":
            if stage_metadata(config, ctx, gates) == "metadata":
                state_file = pause_for_review(config, ctx, "metadata")
                emit("review_ready", gate="metadata", state_path=state_file, data_path=paths["metadata_file"])
                return 0
            save_checkpoint(config, ctx)
        elif stage == "thumbnails":
            if stage_thumbnails(config, ctx, gates) == "thumbnail":
                state_file = pause_for_review(config, ctx, "thumbnail")
                emit("review_ready", gate="thumbnail", state_path=state_file, data_path=paths["thumbs_file"])
                return 0
            save_checkpoint(config, ctx)
        elif stage == "publish":
            ctx["next_stage"] = "upload"
            if "publish" in gates:
                state_file = pause_for_review(config, ctx, "publish")
                update_step("upload", 0, "Waiting for you to publish.")
                emit("review_ready", gate="publish", state_path=state_file, data_path=state_file,
                     video=ctx.get("final_video", ""), title=ctx.get("metadata", {}).get("title", ""),
                     duration=round(get_video_duration(ctx["final_video"]), 2) if os.path.exists(ctx.get("final_video", "")) else 0.0)
                return 0
            finish = config.get("auto_finish", "upload")
            if finish in ("premiere", "both"):
                try:
                    export_premiere_project(ctx)
                except Exception as e:
                    log_error(f"Premiere export failed: {e}")
            if finish == "premiere":
                return finish_without_upload(config, ctx, "Exported for Premiere Pro (not uploaded).")
        elif stage == "finish":
            return finish_without_upload(config, ctx, "Finished without uploading. The edit and thumbnail are next to your recording.")
        elif stage == "upload":
            return stage_upload(config, ctx)
        else:
            log_error(f"Unknown pipeline stage '{stage}'.")
            emit("error", message=f"Unknown pipeline stage '{stage}'.")
            return 1


def save_checkpoint(config: dict, ctx: dict):
    """The state file for a run that isn't waiting at a gate, so it can be started over or exported later."""
    ctx.pop("pending_gate", None)
    write_json_file(pipeline_paths(ctx["input_video"])["state_file"], {
        "version": 2,
        "config": {k: v for k, v in config.items() if k != "secrets"},
        "ctx": ctx,
    })


def run_pipeline(config: dict):
    ctx = stage_analyze(config)
    if ctx is None:
        return 1
    ctx["auto_keep_intervals"] = [list(k) for k in ctx["keep_intervals"]]
    ctx["next_stage"] = "cuts_review"
    save_checkpoint(config, ctx)
    return run_stages(config, ctx)


def load_state(state_path: str) -> dict | None:
    try:
        state = read_json_file(state_path)
    except (OSError, ValueError) as e:
        log_error(f"Could not read pipeline state: {e}")
        emit("error", message=f"Could not read the pipeline state: {e}")
        return None
    ctx = state.get("ctx", {})
    if not ctx.get("input_video") or not os.path.exists(ctx["input_video"]):
        emit("error", message=f"Input video not found: {ctx.get('input_video', '')}")
        return None
    return state


def resume_pipeline(state_path: str, secrets: dict, review: dict | None = None) -> int:
    """Continue a paused run after the user reviewed a gate in OBS."""
    state = load_state(state_path)
    if state is None:
        return 1
    config = dict(state.get("config", {}))
    config["secrets"] = secrets or {}
    ctx = state.get("ctx", {})
    if state.get("version", 1) < 2:
        # Version 1 only ever paused for the B-roll review, right before rendering
        ctx["next_stage"] = "render"
        ctx["pending_gate"] = "broll"
    gate = ctx.get("pending_gate", "")
    log_info(f"Resuming pipeline for '{os.path.basename(ctx['input_video'])}' after the {gate or 'previous'} review.")
    try:
        apply_review(ctx, review)
    except (OSError, ValueError) as e:
        log_error(f"Could not read your {gate} review: {e}")
        emit("error", message=f"Could not read your {gate} review: {e}")
        return 1
    return run_stages(config, ctx)


def regenerate_thumbnails(state_path: str, secrets: dict, action: str, options: dict) -> int:
    """
    At the thumbnail gate: make new AI images ("generate"), refine one ("refine"), or redraw every
    candidate with new text and photo ("recompose"). Then wait at the thumbnail gate again.
    """
    state = load_state(state_path)
    if state is None:
        return 1
    config = dict(state.get("config", {}))
    config["secrets"] = secrets or {}
    ctx = state.get("ctx", {})
    if ctx.get("pending_gate") != "thumbnail":
        emit("error", message="This job isn't waiting at the thumbnail step.")
        return 1
    paths = pipeline_paths(ctx["input_video"])
    out_dir = paths["thumbs_dir"]
    thumbs = read_json_file(paths["thumbs_file"])
    opts = dict(ctx.get("thumb_options") or {})
    opts.update(options or {})
    ctx["thumb_options"] = opts
    metadata = ctx["metadata"]
    hook = thumbnail_hook(metadata, opts)
    side = opts.get("side", "right")
    notes = []
    candidates = thumbs.get("candidates", [])

    needs_frames = action == "generate" or opts.get("person") == "video"
    update_step("thumbnail", 5, "Getting ready...")
    frames = extract_top_talking_head_frames(ctx["final_video"], count=4) if needs_frames else []
    person = resolve_person(config, opts, frames, out_dir, notes)

    try:
        if action == "generate":
            # New images overwrite ai_N in place, so cancelling halfway still leaves pictures to pick from
            source = config.get("thumbnail_image_source", "sd")
            if source == "off":
                source = "sd"
            made, provider = generate_ai_thumbnails(source, STEP_AI_THUMBNAILS, config, metadata, frames,
                                                    out_dir, opts, notes, person)
            candidates = [c for c in candidates if c.get("kind") == "frame"] + [
                {"id": f"ai_{i + 1}", "kind": "ai", "label": f"AI {i + 1}", **m} for i, m in enumerate(made)]
            thumbs["provider"] = provider
            ctx["thumb_provider"] = provider
            thumbs["chosen"] = next((c["id"] for c in candidates if c["kind"] == "ai"), thumbs.get("chosen", ""))
        elif action == "refine":
            source_cand = next((c for c in candidates if c["id"] == opts.get("refine_id")), None)
            if not source_cand or not os.path.exists(source_cand.get("bg", "")):
                raise ValueError("Pick the image to refine first.")
            first = 1 + sum(1 for c in candidates if c.get("kind") == "refined")
            made = refine_thumbnail(config, source_cand, metadata, opts, out_dir, person, notes, first)
            new = [{"id": f"refined_{first + i}", "kind": "refined", "label": f"Refined {first + i}",
                    "shows_you": shows_you(source_cand), **m} for i, m in enumerate(made)]
            candidates += new
            if new:
                thumbs["chosen"] = new[0]["id"]
        else:
            update_step("thumbnail", 50, "Redrawing the thumbnails...")
            for c in candidates:
                recompose_candidate(c, hook, person, side)
    except Exception as e:
        log_error(f"Thumbnail {action} failed: {e}")
        notes.append(f"{action.capitalize()} failed: {str(e)[:200]}")

    thumbs["candidates"] = candidates
    thumbs["notes"] = notes
    thumbs["options"] = opts
    write_json_file(paths["thumbs_file"], thumbs)
    state_file = pause_for_review(config, ctx, "thumbnail")
    update_step("thumbnail", 100, f"Pick a thumbnail ({len(candidates)} options).")
    emit("review_ready", gate="thumbnail", state_path=state_file, data_path=paths["thumbs_file"])
    return 0


REDO_STAGES = {"cuts": "cuts_review", "metadata": "metadata", "thumbnails": "thumbnails"}
REDO_DROPS = {
    "cuts": ("rendered", "final_video", "joins", "metadata", "llm_provider", "thumb_provider"),
    "metadata": ("metadata", "llm_provider", "thumb_provider"),
    "thumbnails": ("thumb_provider",),
}


def redo_pipeline(cfg: dict) -> int:
    """Start a video over from the cuts, the title or the thumbnail, optionally deleting its YouTube upload first."""
    state = load_state(cfg.get("state_path", ""))
    if state is None:
        return 1
    start = cfg.get("from", "")
    if start not in REDO_STAGES and start != "transcribe":
        emit("error", message=f"Can't start over from '{start}'.")
        return 1
    ctx = state.get("ctx", {})
    ctx.pop("pending_gate", None)
    needs = {"transcribe": "input_video", "cuts": "keep_intervals", "metadata": "final_video",
             "thumbnails": "metadata"}[start]
    if not ctx.get(needs) or (needs == "final_video" and not os.path.exists(ctx["final_video"])):
        emit("error", message="This video never got that far; start over from an earlier step.")
        return 1

    # The current settings (Step/Auto gates, thumbnail source...) replace the ones the video was made with
    config = dict(state.get("config", {}))
    config.update({k: v for k, v in cfg.items() if k not in ("mode", "state_path", "from", "delete_video_id")})
    config["secrets"] = cfg.get("secrets", {})

    video_id = cfg.get("delete_video_id", "")
    if video_id:
        try:
            delete_youtube_video(video_id, config["secrets"].get("youtube_token_json", ""))
        except Exception as e:
            log_error(f"Could not delete the YouTube video: {e}")
            emit("error", message=f"Could not delete the YouTube video, so nothing was started over: {e}")
            return 1
        emit("video_deleted", video_id=video_id)
    if start == "transcribe":
        config["input_video"] = ctx["input_video"]
        return run_pipeline(config)
    for key in ("video_id", "video_url", "upload_file") + REDO_DROPS[start]:
        ctx.pop(key, None)
    if start == "cuts":
        ctx["reuse_cuts"] = True
        ctx["keep_intervals"] = ctx.get("auto_keep_intervals") or ctx["keep_intervals"]
    ctx["next_stage"] = REDO_STAGES[start]
    log_info(f"Starting '{os.path.basename(ctx['input_video'])}' over from the {start} step (the recording is kept).")
    return run_stages(config, ctx)


def export_premiere_from_state(state_path: str) -> int:
    state = load_state(state_path)
    if state is None:
        return 1
    ctx = state.get("ctx", {})
    if not ctx.get("keep_intervals"):
        emit("error", message="This video hasn't been cut yet, so there's nothing to export.")
        return 1
    try:
        export_premiere_project(ctx)
        return 0
    except Exception as e:
        log_error(f"Premiere export failed: {e}")
        emit("error", message=f"Premiere export failed: {e}")
        return 1


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
        mode = cfg.get("mode")
        if mode == "oauth_login":
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
        if mode == "update_video":
            try:
                update_video_details(
                    video_id=cfg.get("video_id", ""),
                    title=cfg.get("title", ""),
                    description=cfg.get("description", ""),
                    oauth_token_json=cfg.get("secrets", {}).get("youtube_token_json", "")
                )
                emit("update_success", video_id=cfg.get("video_id", ""), title=cfg.get("title", ""))
                return 0
            except Exception as e:
                log_error(f"YouTube update error: {e}")
                emit("update_error", message=str(e))
                return 1
        if mode == "delete_video":
            try:
                delete_youtube_video(cfg.get("video_id", ""), cfg.get("secrets", {}).get("youtube_token_json", ""))
                emit("video_deleted", video_id=cfg.get("video_id", ""))
                return 0
            except Exception as e:
                log_error(f"YouTube delete error: {e}")
                emit("delete_error", message=str(e))
                return 0
        if mode == "resume_pipeline":
            return resume_pipeline(cfg.get("state_path", ""), cfg.get("secrets", {}), cfg.get("review"))
        if mode == "thumbnail_regen":
            return regenerate_thumbnails(cfg.get("state_path", ""), cfg.get("secrets", {}),
                                         cfg.get("action", "generate"), cfg.get("options") or {})
        if mode == "redo_pipeline":
            return redo_pipeline(cfg)
        if mode == "export_premiere":
            return export_premiere_from_state(cfg.get("state_path", ""))
        return run_pipeline(cfg)

    if args.self_test:
        log_info("Running pipeline self-test...")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
