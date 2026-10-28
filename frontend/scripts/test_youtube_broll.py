"""
Checks for the screen B-roll part of youtube_pipeline.py.

    python frontend/scripts/test_youtube_broll.py

The render tests need ffmpeg/ffprobe on PATH and are skipped otherwise.
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import youtube_pipeline as yp  # noqa: E402

HAVE_FFMPEG = bool(shutil.which("ffmpeg") and shutil.which("ffprobe"))


def quiet():
    """The pipeline reports progress as JSON lines on stdout; keep test output readable."""
    return contextlib.redirect_stdout(io.StringIO())


def words_for(segments):
    return yp.flatten_words(segments)


def segment(start, end, text):
    tokens = text.split()
    step = (end - start) / len(tokens)
    return {
        "start": start, "end": end, "text": text,
        "words": [{"word": f" {w}", "start": start + i * step, "end": start + (i + 1) * step} for i, w in enumerate(tokens)],
    }


class TimelineTests(unittest.TestCase):
    keep = [(0.0, 10.0), (20.0, 30.0)]

    def test_raw_to_edited(self):
        self.assertEqual(yp.raw_to_edited(5.0, self.keep), 5.0)
        self.assertEqual(yp.raw_to_edited(15.0, self.keep), 10.0)  # inside the cut snaps forward
        self.assertEqual(yp.raw_to_edited(25.0, self.keep), 15.0)

    def test_round_trip(self):
        for t in (0.0, 3.3, 10.0, 12.5, 19.9):
            self.assertAlmostEqual(yp.raw_to_edited(yp.edited_to_raw(t, self.keep), self.keep), t)


class GuardrailTests(unittest.TestCase):
    keep = [(0.0, 60.0)]
    screen_range = (0.0, 60.0)

    def plan(self, candidates, max_coverage=1.0, keep=None, screen_range=None, segments=()):
        return yp.apply_broll_guardrails(
            candidates, keep or self.keep, words_for(list(segments)),
            screen_range or self.screen_range, max_coverage
        )

    def test_hook_is_never_covered(self):
        out = self.plan([{"start": 1.0, "end": 9.0, "confidence": 0.9}])
        self.assertEqual(len(out), 1)
        self.assertGreaterEqual(out[0]["edited_start"], yp.BROLL_HOOK_SEC)

    def test_too_short_is_dropped(self):
        self.assertEqual(self.plan([{"start": 20.0, "end": 21.0, "confidence": 0.9}]), [])

    def test_too_long_is_trimmed(self):
        out = self.plan([{"start": 10.0, "end": 40.0, "confidence": 0.9}])
        self.assertAlmostEqual(out[0]["edited_end"] - out[0]["edited_start"], yp.BROLL_MAX_SEC)

    def test_close_inserts_merge(self):
        out = self.plan([
            {"start": 10.0, "end": 14.0, "quote": "first", "confidence": 0.7},
            {"start": 15.0, "end": 18.0, "quote": "second", "confidence": 0.9},
        ])
        self.assertEqual(len(out), 1)
        self.assertEqual((out[0]["start"], out[0]["end"]), (10.0, 18.0))
        self.assertEqual(out[0]["confidence"], 0.9)
        self.assertIn("second", out[0]["quote"])

    def test_coverage_cap_keeps_highest_confidence(self):
        out = self.plan([
            {"start": 10.0, "end": 16.0, "confidence": 0.5},
            {"start": 30.0, "end": 36.0, "confidence": 0.9},
        ], max_coverage=0.15)  # 9 s budget of 60 s: only one 6 s insert fits
        self.assertEqual([o["start"] for o in out], [30.0])

    def test_clipped_to_screen_recording(self):
        out = self.plan([{"start": 50.0, "end": 58.0, "confidence": 0.9}], screen_range=(0.0, 55.0))
        self.assertEqual(out[0]["end"], 55.0)

    def test_edited_times_follow_cuts(self):
        out = self.plan([{"start": 22.0, "end": 26.0, "confidence": 0.9}], keep=[(0.0, 10.0), (20.0, 30.0)])
        self.assertEqual((out[0]["edited_start"], out[0]["edited_end"]), (12.0, 16.0))

    def test_snaps_to_word_boundaries(self):
        seg = segment(10.0, 14.0, "click this button right here")  # words every 0.8 s
        out = self.plan([{"start": 10.3, "end": 13.5, "confidence": 0.9}], segments=[seg])
        self.assertEqual((out[0]["start"], out[0]["end"]), (10.0, 14.0))

    def test_bad_candidates_are_ignored(self):
        self.assertEqual(self.plan([{"start": "x", "end": 9}, {"end": 5}]), [])


class HeuristicTests(unittest.TestCase):
    def test_cue_phrases_and_markers(self):
        segs = [
            segment(0.0, 4.0, "Hey everyone welcome back"),
            segment(10.0, 15.0, "As you can see the settings panel is open"),
            segment(20.0, 24.0, "I think this is the best approach"),
        ]
        with quiet():
            cands = yp.plan_broll_heuristic(segs, markers=[21.0])
        starts = sorted(c["start"] for c in cands)
        self.assertEqual(starts, [10.0, 20.0])
        self.assertTrue(any("hotkey" in c["reason"] for c in cands))


class ReviewTests(unittest.TestCase):
    def test_only_accepted_inserts_render(self):
        plan = {"offset_sec": -2.0, "inserts": [
            {"start": 10, "end": 15, "status": "accepted"},
            {"start": 20, "end": 25, "status": "rejected"},
            {"start": 30, "end": 35, "status": "proposed"},
            {"start": 14, "end": 18, "status": "accepted"},
        ]}
        self.assertEqual(yp.accepted_broll_inserts(plan, screen_duration=100.0), [(10.0, 18.0)])

    def test_accepted_clipped_to_screen(self):
        plan = {"offset_sec": 5.0, "inserts": [{"start": 0, "end": 4, "status": "accepted"}]}
        # screen_time = raw + 5, screen is 8 s long -> raw window [0, 2.9]
        self.assertEqual(yp.accepted_broll_inserts(plan, screen_duration=8.0), [(0.0, 2.9)])

    def test_split_interval(self):
        pieces = yp.split_interval_for_broll(0.0, 10.0, [(3.0, 5.0), (5.05, 7.0)])
        self.assertEqual(pieces, [(0.0, 3.0, False), (3.0, 5.0, True), (5.0, 7.0, True), (7.0, 10.0, False)])
        self.assertEqual(yp.split_interval_for_broll(0.0, 4.0, [(3.95, 6.0)]), [(0.0, 4.0, False)])


class OffsetTests(unittest.TestCase):
    def test_correlation_finds_delay(self):
        import numpy as np
        sr = 8000
        rng = np.random.default_rng(7)
        main = rng.standard_normal(sr * 20).astype(np.float32)
        # Screen recording started 1.5 s after the webcam one
        screen = main[int(1.5 * sr):].copy()
        offset, strength = yp.correlate_offset(main, screen, sr)
        self.assertAlmostEqual(offset, -1.5, places=3)
        self.assertGreater(strength, 0.9)
        # ...and 0.75 s before it
        offset, _ = yp.correlate_offset(screen, main, sr)
        self.assertAlmostEqual(offset, 1.5, places=3)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-y", "-v", "error", *args], check=True)


def center_rgb(video, t):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(t), "-i", video, "-frames:v", "1",
         "-vf", "crop=iw/4:ih/4:iw*3/8:ih*3/8,scale=1:1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True
    ).stdout
    return tuple(out[:3])


def corner_rgb(video, t):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(t), "-i", video, "-frames:v", "1",
         "-vf", "crop=iw/10:ih/10:iw*0.85:ih*0.85,scale=1:1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True
    ).stdout
    return tuple(out[:3])


@unittest.skipUnless(HAVE_FFMPEG, "ffmpeg not on PATH")
class RenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="broll_test_")
        cls.main = os.path.join(cls.tmp, "main.mp4")
        cls.screen = os.path.join(cls.tmp, "screen.mp4")
        # Webcam = red, screen = blue; shared mic noise, screen started 1.5 s later
        ffmpeg("-f", "lavfi", "-i", "color=c=red:s=640x360:r=30:d=20",
               "-f", "lavfi", "-i", "anoisesrc=d=20:c=pink:r=48000:seed=3",
               "-shortest", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", cls.main)
        ffmpeg("-f", "lavfi", "-i", "color=c=blue:s=1280x720:r=30:d=18.5",
               "-ss", "1.5", "-i", cls.main, "-map", "0:v", "-map", "1:a",
               "-shortest", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", cls.screen)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def render(self, style, out_name):
        out = os.path.join(self.tmp, out_name)
        keep = [(0.0, 6.0), (8.0, 20.0)]
        broll = {"screen_path": self.screen, "offset": -1.5, "inserts": [(10.0, 14.0)], "style": style}
        with quiet():
            ok = yp.render_edited_video(self.main, out, keep, 20.0, broll)
        self.assertTrue(ok)
        return out

    def test_cutaway(self):
        out = self.render("cutaway", "cutaway.mp4")
        self.assertAlmostEqual(yp.get_video_duration(out), 18.0, delta=0.15)
        r, g, b = center_rgb(out, 3.0)             # A-roll
        self.assertGreater(r, 180)
        self.assertLess(b, 80)
        r, g, b = center_rgb(out, 8.0 + 2.0)       # raw 12 s -> edited 10 s: screen
        self.assertGreater(b, 180)
        self.assertLess(r, 80)
        self.assertEqual(yp.probe_video_geometry(out)[:2], (640, 360))

    def test_pip(self):
        out = self.render("pip", "pip.mp4")
        self.assertGreater(center_rgb(out, 10.0)[2], 180)   # screen fills the frame
        self.assertGreater(corner_rgb(out, 10.0)[0], 180)   # webcam bubble bottom-right

    def test_plan_broll_heuristic_end_to_end(self):
        segs = [
            segment(0.5, 4.0, "Welcome back to the channel"),
            segment(9.0, 14.0, "Now click this button and you can see the result"),
        ]
        plan_path = os.path.join(self.tmp, "plan.json")
        cfg = {"enabled": True, "screen_video": self.screen, "start_delta_sec": 1.5, "markers": [], "max_coverage": 0.5}
        with quiet():
            plan = yp.plan_broll(cfg, self.main, segs, [(0.0, 20.0)], [], {}, "none", plan_path,
                                 os.path.join(self.tmp, "thumbs"))
        self.assertAlmostEqual(plan["offset_sec"], -1.5, delta=0.01)
        self.assertEqual(len(plan["inserts"]), 1)
        ins = plan["inserts"][0]
        self.assertEqual((ins["start"], ins["end"], ins["status"]), (9.0, 14.0, "proposed"))
        self.assertTrue(os.path.exists(ins["thumb"]))
        self.assertEqual(yp.read_json_file(plan_path)["inserts"][0]["id"], "b01")

    def test_pause_for_review_then_resume(self):
        import json
        from unittest import mock

        segs = [segment(9.0, 14.0, "Now click this button and you can see the result")]
        cfg = {
            "mode": "pipeline", "input_video": self.main, "priority_list": [], "skip_upload": True,
            "secrets": {"gemini_key": "SECRET"},
            "broll": {"enabled": True, "screen_video": self.screen, "start_delta_sec": 1.5,
                      "markers": [], "style": "cutaway", "max_coverage": 0.5, "review": True},
        }
        patches = (
            mock.patch.object(yp, "transcribe_video", return_value=segs),
            mock.patch.object(yp, "generate_thumbnail_with_priority", return_value="test"),
        )

        def events(fn, *args):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), patches[0], patches[1]:
                rc = fn(*args)
            return rc, [json.loads(line) for line in buf.getvalue().splitlines()]

        rc, evs = events(yp.run_pipeline, cfg)
        self.assertEqual(rc, 0)
        ready = [e for e in evs if e["type"] == "broll_plan_ready"]
        self.assertEqual(len(ready), 1)
        self.assertEqual(ready[0]["insert_count"], 1)
        self.assertFalse(any(e["type"] in ("metadata_ready", "complete") for e in evs))  # paused before render
        with open(ready[0]["state_path"], encoding="utf-8") as f:
            self.assertNotIn("SECRET", f.read())

        # What the OBS review panel does: mark the insert accepted
        plan = yp.read_json_file(ready[0]["plan_path"])
        plan["inserts"][0]["status"] = "accepted"
        yp.write_json_file(ready[0]["plan_path"], plan)

        rc, evs = events(yp.resume_pipeline, ready[0]["state_path"], {"gemini_key": "SECRET"})
        self.assertEqual(rc, 0)
        done = [e for e in evs if e["type"] == "complete"]
        self.assertEqual(len(done), 1)
        self.assertTrue(any(e["type"] == "metadata_ready" for e in evs))
        edited = done[0]["edited_video"]
        self.assertTrue(edited.endswith("_yt_edited.mp4"))
        self.assertGreater(center_rgb(edited, 11.0)[2], 180)  # B-roll made it into the render


if __name__ == "__main__":
    unittest.main(verbosity=2)
