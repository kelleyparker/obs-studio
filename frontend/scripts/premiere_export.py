#!/usr/bin/env python3
"""
Premiere Pro export for the YouTube pipeline: the AI edit as a Final Cut Pro 7 XML (xmeml v4)
sequence, which Premiere opens as a new project (OBS starts it on the file) or with File > Import.

The sequence uses the original recording, so every cut is a real edit point with handles and can be
widened or tightened in Premiere. Tracks:
  V1  the kept parts of the recording, back to back
  V2  screen B-roll at each accepted insert (if any)
  V3  the webcam bubble over the screen (picture-in-picture B-roll only)
  A1/A2  the recording's first audio track (OBS's main mix), linked to V1. Stereo is written the way
         Premiere's own exports write it: one track per channel, which Premiere joins into a stereo clip.
  A3/A4  the background music (if any) on its own, as it sits in the upload, so its level can be changed
Markers show the chapters and every place the AI cut. OBS's other audio tracks are left out; they
usually hold the same sound as the main mix, and stacking them would play it several times over.
"""

import json
import os
import subprocess
import urllib.parse
import xml.etree.ElementTree as ET


def probe_media(path: str) -> dict:
    """Frame rate, size, length and audio streams of a media file (ffprobe)."""
    res = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "format=duration:stream=codec_type,width,height,avg_frame_rate,r_frame_rate,channels,sample_rate",
                          "-of", "json", path], capture_output=True, text=True, timeout=60)
    data = json.loads(res.stdout or "{}")
    streams = data.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), {})
    fps = 30.0
    for key in ("avg_frame_rate", "r_frame_rate"):
        num, _, den = str(video.get(key, "0/1")).partition("/")
        try:
            value = float(num) / float(den or 1)
        except (ValueError, ZeroDivisionError):
            value = 0.0
        if value > 0:
            fps = value
            break
    return {
        "has_video": bool(video),
        "duration": float(data.get("format", {}).get("duration") or 0.0),
        "fps": fps,
        "width": int(video.get("width") or 1920),
        "height": int(video.get("height") or 1080),
        "audio": [{"channels": int(s.get("channels") or 2), "sample_rate": int(s.get("sample_rate") or 48000)}
                  for s in streams if s.get("codec_type") == "audio"],
    }


def timebase(fps: float) -> tuple[int, bool]:
    """FCP timebase and NTSC flag: 29.97 -> (30, True), 25 -> (25, False)."""
    rounded = max(1, round(fps))
    return rounded, abs(fps - rounded * 1000 / 1001) < 0.01 and abs(fps - rounded) > 0.001


def path_url(path: str) -> str:
    """file://localhost/G%3a/obs_recordings/My%20Video.mp4, the way Premiere writes it."""
    return "file://localhost/" + urllib.parse.quote(os.path.abspath(path).replace("\\", "/"), safe="/")


def _sub(parent, tag: str, text=None, **attrs):
    el = ET.SubElement(parent, tag, {k: str(v) for k, v in attrs.items()})
    if text is not None:
        el.text = str(text)
    return el


def _rate(parent, fps: float):
    base, ntsc = timebase(fps)
    rate = _sub(parent, "rate")
    _sub(rate, "timebase", base)
    _sub(rate, "ntsc", "TRUE" if ntsc else "FALSE")


def _video_characteristics(parent, probe: dict):
    sc = _sub(parent, "samplecharacteristics")
    _rate(sc, probe["fps"])
    _sub(sc, "width", probe["width"])
    _sub(sc, "height", probe["height"])
    _sub(sc, "anamorphic", "FALSE")
    _sub(sc, "pixelaspectratio", "square")
    _sub(sc, "fielddominance", "none")


def _file(parent, file_id: str, path: str, probe: dict, fps: float, defined: set):
    """The <file> of a clip: in full the first time, then only as a reference."""
    if file_id in defined:
        _sub(parent, "file", id=file_id)
        return
    defined.add(file_id)
    f = _sub(parent, "file", id=file_id)
    _sub(f, "name", os.path.basename(path))
    _sub(f, "pathurl", path_url(path))
    _rate(f, fps)
    _sub(f, "duration", round(probe["duration"] * fps))
    media = _sub(f, "media")
    if probe.get("has_video", True):
        _video_characteristics(_sub(media, "video"), probe)
    if probe["audio"]:
        audio = _sub(media, "audio")
        sc = _sub(audio, "samplecharacteristics")
        _sub(sc, "depth", 16)
        _sub(sc, "samplerate", probe["audio"][0]["sample_rate"])
        _sub(audio, "channelcount", probe["audio"][0]["channels"])


def _motion(parent, scale: float, horiz: float = 0.0, vert: float = 0.0):
    """Basic Motion: scale in percent, center offset as a fraction of the frame (0, 0 = middle)."""
    effect = _sub(_sub(parent, "filter"), "effect")
    _sub(effect, "name", "Basic Motion")
    _sub(effect, "effectid", "basic")
    _sub(effect, "effectcategory", "motion")
    _sub(effect, "effecttype", "motion")
    _sub(effect, "mediatype", "video")
    p = _sub(effect, "parameter", authoringApp="PremierePro")
    _sub(p, "parameterid", "scale")
    _sub(p, "name", "Scale")
    _sub(p, "valuemin", 0)
    _sub(p, "valuemax", 1000)
    _sub(p, "value", round(scale, 3))
    p = _sub(effect, "parameter", authoringApp="PremierePro")
    _sub(p, "parameterid", "center")
    _sub(p, "name", "Center")
    value = _sub(p, "value")
    _sub(value, "horiz", round(horiz, 5))
    _sub(value, "vert", round(vert, 5))


PPRO_TICKS_PER_SEC = 254016000000


def _transition(track, name: str, mediatype: str, start: int, end: int, alignment: str, fps: float,
                effectid: str = ""):
    """A transition at an end of the sequence: alignment "start-black" (fade in) or "end-black" (fade out)."""
    item = _sub(track, "transitionitem")
    _sub(item, "start", start)
    _sub(item, "end", end)
    _sub(item, "alignment", alignment)
    cut_frames = 0 if alignment == "start-black" else end - start
    _sub(item, "cutPointTicks", round(cut_frames / fps * PPRO_TICKS_PER_SEC))
    _rate(item, fps)
    effect = _sub(item, "effect")
    _sub(effect, "name", name)
    _sub(effect, "effectid", effectid or name)
    if mediatype == "video":
        _sub(effect, "effectcategory", "Dissolve")
    _sub(effect, "effecttype", "transition")
    _sub(effect, "mediatype", mediatype)
    if mediatype == "video":
        _sub(effect, "wipecode", 0)
        _sub(effect, "wipeaccuracy", 100)
        _sub(effect, "startratio", 0)
        _sub(effect, "endratio", 1)
        _sub(effect, "reverse", "FALSE")


def _clipitem(track, clip_id: str, name: str, fps: float, start: int, end: int, src_in: int, src_out: int,
              file_id: str, path: str, probe: dict, defined: set, **attrs):
    clip = _sub(track, "clipitem", id=clip_id, **attrs)
    _sub(clip, "masterclipid", file_id.replace("file", "masterclip"))
    _sub(clip, "name", name)
    _sub(clip, "enabled", "TRUE")
    _sub(clip, "duration", round(probe["duration"] * fps))
    _rate(clip, fps)
    _sub(clip, "start", start)
    _sub(clip, "end", end)
    _sub(clip, "in", src_in)
    _sub(clip, "out", src_out)
    _file(clip, file_id, path, probe, fps, defined)
    return clip


def build_premiere_xml(recording: str, probe: dict, keep_intervals: list, broll: dict | None,
                       chapters: list, cut_marks: list, name: str = "AI edit", fade_sec: float = 1.0,
                       music: dict | None = None) -> str:
    """
    The edit as FCP 7 XML. keep_intervals are (start, end) seconds of the recording; broll is None or
    {path, probe, offset, inserts: [(start, end) in recording seconds], style: "pip"|"cutaway", pip_width};
    chapters are (seconds, title) and cut_marks (seconds, name, comment), both on the edited timeline.
    V1 gets a Cross Dissolve from black at the start and to black at the end, the audio a Cross Fade
    (+3dB) at the end, each fade_sec long (0 for none) or as long as the first or last clip if that is
    shorter. music is None or {path, probe}: a song as long as the edit, put on its own tracks.
    """
    fps = probe["fps"]

    def frames(seconds: float) -> int:
        return int(round(seconds * fps))

    # Where each kept part lands on the timeline; lengths come from the rounded in/out so there are no gaps
    parts, t = [], 0
    for a, b in keep_intervals:
        src_in, src_out = frames(a), frames(b)
        if src_out <= src_in:
            continue
        parts.append({"a": a, "b": b, "in": src_in, "out": src_out, "start": t, "end": t + src_out - src_in})
        t += src_out - src_in
    total = t
    # A transition can't be longer than the clip it sits on
    fade = min(frames(fade_sec), total // 3)
    fade_in = min(fade, parts[0]["end"] - parts[0]["start"]) if parts else 0
    fade_out = min(fade, parts[-1]["end"] - parts[-1]["start"]) if parts else 0

    root = ET.Element("xmeml", version="4")
    seq = _sub(root, "sequence", id="sequence-1")
    _sub(seq, "name", name)
    _sub(seq, "duration", total)
    _rate(seq, fps)
    tc = _sub(seq, "timecode")
    _rate(tc, fps)
    _sub(tc, "string", "00:00:00:00")
    _sub(tc, "frame", 0)
    _sub(tc, "displayformat", "NDF")

    media = _sub(seq, "media")
    video = _sub(media, "video")
    _video_characteristics(_sub(video, "format"), probe)
    defined = set()
    # Audio tracks: one per channel of the main mix (stereo = left and right, mono = one)
    stereo = bool(probe["audio"]) and probe["audio"][0]["channels"] >= 2
    audio_count = (2 if stereo else 1) if probe["audio"] else 0

    def link_part(clip, i: int):
        """Link a part's picture and sound so they move together in Premiere."""
        for kind, clip_id, track_index in [("video", f"clipitem-v1-{i + 1}", 1)] + [
                ("audio", f"clipitem-a{n + 1}-{i + 1}", n + 1) for n in range(audio_count)]:
            link = _sub(clip, "link")
            _sub(link, "linkclipref", clip_id)
            _sub(link, "mediatype", kind)
            _sub(link, "trackindex", track_index)
            # Items are counted with the transitions, so V1's clips come after its opening dissolve
            _sub(link, "clipindex", i + 1 + (1 if kind == "video" and fade_in else 0))
            if kind == "audio":
                _sub(link, "groupindex", 1)

    v1 = _sub(video, "track")
    if fade_in:
        _transition(v1, "Cross Dissolve", "video", 0, fade_in, "start-black", fps)
    for i, part in enumerate(parts):
        link_part(_clipitem(v1, f"clipitem-v1-{i + 1}", f"Part {i + 1}", fps, part["start"], part["end"],
                            part["in"], part["out"], "file-1", recording, probe, defined), i)
    if fade_out:
        _transition(v1, "Cross Dissolve", "video", total - fade_out, total, "end-black", fps)

    if broll and broll.get("inserts"):
        screen_probe = broll["probe"]
        fit = min(probe["width"] / screen_probe["width"], probe["height"] / screen_probe["height"]) * 100
        v2 = _sub(video, "track")
        v3 = _sub(video, "track") if broll.get("style", "pip") == "pip" else None
        n = 0
        for part in parts:
            for s, e in broll["inserts"]:
                s, e = max(s, part["a"]), min(e, part["b"])
                if e - s < 1.0 / fps:
                    continue
                n += 1
                start = part["start"] + frames(s) - part["in"]
                end = start + frames(e) - frames(s)
                screen = _clipitem(v2, f"clipitem-v2-{n}", f"Screen {n}", fps, start, end,
                                   frames(s + broll["offset"]), frames(s + broll["offset"]) + end - start,
                                   "file-2", broll["path"], screen_probe, defined)
                if abs(fit - 100) > 0.5:
                    _motion(screen, fit)
                if v3 is not None:
                    # Webcam bubble in the bottom-right corner, as in the rendered PiP
                    width = float(broll.get("pip_width", 0.22))
                    margin = 0.025
                    bubble = _clipitem(v3, f"clipitem-v3-{n}", f"Webcam {n}", fps, start, end, frames(s), frames(e),
                                       "file-1", recording, probe, defined)
                    _motion(bubble, width * 100, 0.5 - margin - width / 2,
                            0.5 - margin * probe["width"] / probe["height"] - width / 2)

    for track in video.findall("track"):
        _sub(track, "enabled", "TRUE")
        _sub(track, "locked", "FALSE")

    audio = _sub(media, "audio")
    _sub(audio, "numOutputChannels", 2)
    fmt = _sub(_sub(audio, "format"), "samplecharacteristics")
    _sub(fmt, "depth", 16)
    _sub(fmt, "samplerate", probe["audio"][0]["sample_rate"] if probe["audio"] else 48000)
    outputs = _sub(audio, "outputs")
    for n in (1, 2):
        group = _sub(outputs, "group")
        _sub(group, "index", n)
        _sub(group, "numchannels", 1)
        _sub(group, "downmix", 0)
        _sub(_sub(group, "channel"), "index", n)
    for n in range(audio_count):
        # A stereo clip is one track per channel ("exploded"), each reading its channel of the file
        attrs = {"currentExplodedTrackIndex": n, "totalExplodedTrackCount": 2} if stereo else {}
        track = _sub(audio, "track", **attrs, premiereTrackType="Stereo" if stereo else "Mono")
        for i, part in enumerate(parts):
            clip = _clipitem(track, f"clipitem-a{n + 1}-{i + 1}", f"Part {i + 1}", fps, part["start"], part["end"],
                             part["in"], part["out"], "file-1", recording, probe, defined,
                             premiereChannelType="stereo" if stereo else "mono")
            source = _sub(clip, "sourcetrack")
            _sub(source, "mediatype", "audio")
            _sub(source, "trackindex", n + 1)
            link_part(clip, i)
        if fade_out:
            # FCP 7's equal-power fade, which Premiere reads as Constant Power; a name it doesn't know
            # (like its own Exponential Fade) gets swapped for that anyway, with a Translation Report popup
            _transition(track, "Cross Fade (+3dB)", "audio", total - fade_out, total, "end-black", fps,
                        effectid="KGAudioTransCrossFade3dB")
        _sub(track, "enabled", "TRUE")
        _sub(track, "locked", "FALSE")
        _sub(track, "outputchannelindex", n + 1)

    if music and music["probe"]["audio"]:
        # The music's own tracks, after the voice; its left and right are linked into one stereo clip
        music_stereo = music["probe"]["audio"][0]["channels"] >= 2
        channels = 2 if music_stereo else 1
        end = min(total, frames(music["probe"]["duration"]))
        first = audio_count + 1
        for n in range(channels):
            attrs = {"currentExplodedTrackIndex": n, "totalExplodedTrackCount": 2} if music_stereo else {}
            track = _sub(audio, "track", **attrs, premiereTrackType="Stereo" if music_stereo else "Mono")
            clip = _clipitem(track, f"clipitem-m{n + 1}-1", "Music", fps, 0, end, 0, end, "file-3", music["path"],
                             music["probe"], defined, premiereChannelType="stereo" if music_stereo else "mono")
            source = _sub(clip, "sourcetrack")
            _sub(source, "mediatype", "audio")
            _sub(source, "trackindex", n + 1)
            for m in range(channels):
                link = _sub(clip, "link")
                _sub(link, "linkclipref", f"clipitem-m{m + 1}-1")
                _sub(link, "mediatype", "audio")
                _sub(link, "trackindex", first + m)
                _sub(link, "clipindex", 1)
                _sub(link, "groupindex", 1)
            _sub(track, "enabled", "TRUE")
            _sub(track, "locked", "FALSE")
            _sub(track, "outputchannelindex", n + 1)

    for seconds, title in chapters:
        marker = _sub(seq, "marker")
        _sub(marker, "name", title)
        _sub(marker, "comment", "Chapter")
        _sub(marker, "in", min(frames(seconds), max(0, total - 1)))
        _sub(marker, "out", -1)
    for seconds, title, comment in cut_marks:
        marker = _sub(seq, "marker")
        _sub(marker, "name", title)
        _sub(marker, "comment", comment)
        _sub(marker, "in", min(frames(seconds), max(0, total - 1)))
        _sub(marker, "out", -1)

    ET.indent(root, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE xmeml>\n' + ET.tostring(root, encoding="unicode") + "\n"
