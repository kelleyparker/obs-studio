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


def spot(id_, at, confidence, length=4.0):
    return {"id": id_, "start": at, "end": at + length, "edited_start": at, "edited_end": at + length,
            "confidence": confidence, "kind": "image", "prompt": "a picture"}


def fake_sd(colour="green"):
    """Stands in for generate_sd_images: writes a plain picture per prompt, as youtube_imagegen.py names them."""
    from PIL import Image

    def make(config, prompts, out_dir, label, **kwargs):
        made = []
        for n, _ in enumerate(prompts, start=1):
            image = Image.new("RGB", (1344, 768), colour)
            image.save(os.path.join(out_dir, f"sd_{n}.png"))
            made.append(image)
        return made
    return make


class PictureTests(unittest.TestCase):
    def test_dial_picks_the_best_spots_spread_out(self):
        spots = [spot("b01", 10, 0.9), spot("b02", 15, 0.95), spot("b03", 100, 0.5), spot("b04", 150, 0.7)]
        # "Few" on a 3-minute edit is 2 pictures, at least 45 s apart: b01 sits too close to b02
        self.assertEqual(yp.pick_broll_spots(spots, 2, 180.0), ["b02", "b04"])
        self.assertEqual(yp.pick_broll_spots(spots, 0, 180.0), [])
        self.assertEqual(yp.pick_broll_spots(spots, 5, 180.0), ["b01", "b02", "b03", "b04"])
        # Within the B-roll budget: room for one 4-second picture
        self.assertEqual(yp.pick_broll_spots(spots, 5, 180.0, budget_sec=5.0), ["b02"])

    def test_more_on_the_dial_is_never_fewer(self):
        spots = [spot(f"b{n:02d}", 6 + n * 9, 0.3 + (n % 5) / 10) for n in range(20)]
        counts = [len(yp.pick_broll_spots(spots, a, 200.0)) for a in range(6)]
        self.assertEqual(counts, sorted(counts))
        self.assertEqual(counts[0], 0)

    def test_short_lines_stretch_and_long_ones_stop(self):
        segs = [segment(10.0, 11.5, "Look at this"), segment(20.0, 32.0, "A long explanation " * 4)]
        cands = [{"start": 10.0, "end": 11.5, "prompt": "p1", "confidence": 0.8},
                 {"start": 20.0, "end": 32.0, "prompt": "p2", "confidence": 0.8}]
        out = yp.apply_broll_guardrails(cands, [(0.0, 60.0)], words_for(segs), (0.0, 60.0), 1.0,
                                        min_sec=yp.BROLL_IMAGE_MIN_SEC, max_sec=yp.BROLL_IMAGE_MAX_SEC, merge=False)
        self.assertEqual([(c["start"], c["end"]) for c in out], [(10.0, 13.0), (20.0, 26.0)])
        self.assertEqual([c["prompt"] for c in out], ["p1", "p2"])

    def test_plan_without_a_screen_recording_is_all_pictures(self):
        segs = [segment(8.0, 12.0, "Compound interest is like a snowball rolling downhill"),
                segment(30.0, 35.0, "The second idea is that habits beat motivation every time"),
                segment(60.0, 64.0, "Finally think of your budget as a map for your money")]
        tmp = tempfile.mkdtemp(prefix="broll_pictures_")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        cfg = {"enabled": True, "amount": 5, "image_source": "gemini", "max_coverage": 0.5}
        with quiet():
            plan = yp.plan_broll(cfg, os.path.join(tmp, "main.mp4"), segs, [(0.0, 70.0)], [], {}, "none",
                                 os.path.join(tmp, "plan.json"), os.path.join(tmp, "thumbs"))
        self.assertEqual(plan["screen_video"], "")
        self.assertEqual({i["kind"] for i in plan["inserts"]}, {"image"})
        self.assertEqual(len(plan["inserts"]), 3)
        self.assertTrue(all(i["prompt"] and i["source"] == "gemini" and i["picked"] for i in plan["inserts"]))

    def test_nothing_to_plan_when_pictures_are_off(self):
        with quiet():
            self.assertIsNone(yp.plan_broll({"enabled": True, "amount": 0}, "main.mp4",
                                            [segment(8.0, 12.0, "one two three four five")], [(0.0, 20.0)],
                                            [], {}, "none", "plan.json", "thumbs"))

    def test_cloud_sends_only_the_prompt_and_falls_back_to_this_pc(self):
        from unittest import mock
        from PIL import Image

        tmp = tempfile.mkdtemp(prefix="broll_make_")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        plan = {"inserts": [dict(spot("b01", 10, 0.9), source="gemini"), dict(spot("b02", 30, 0.9), source="gemini"),
                            dict(spot("b03", 50, 0.9), source="sd")]}
        calls = []

        def cloud(source, secrets, prompt, *args, **kwargs):
            calls.append((args, kwargs))
            if len(calls) == 2:
                raise RuntimeError("HTTP 429 RESOURCE_EXHAUSTED: quota")
            return Image.new("RGB", (1376, 768), "blue"), source, "gemini-3.1-flash-image-preview"

        notes = []
        with quiet(), mock.patch.object(yp, "generate_cloud_image", side_effect=cloud), \
                mock.patch.object(yp, "generate_sd_images", side_effect=fake_sd()) as sd:
            made = yp.make_broll_images({"secrets": {"gemini_key": "K"}}, plan, ["b01", "b02", "b03"], tmp, notes)
        self.assertEqual(made, 3)
        self.assertEqual(calls[0], ((), {}))  # text only: no picture of you goes to the cloud
        by_id = {i["id"]: i for i in plan["inserts"]}
        self.assertEqual(by_id["b01"]["made_by"], "Google Nano Banana 2")
        self.assertEqual(by_id["b02"]["made_by"], yp.BROLL_IMAGE_SOURCES["sd"])
        self.assertEqual(by_id["b03"]["made_by"], yp.BROLL_IMAGE_SOURCES["sd"])
        self.assertEqual(len(sd.call_args.args[1]), 2)  # one Stable Diffusion batch for both
        self.assertTrue(all(os.path.exists(i["image"]) for i in plan["inserts"]))
        self.assertTrue(any("b02" in n for n in notes))


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


def mean_volume(video, start, length):
    """Mean loudness in dB of `length` seconds of the audio from `start`."""
    err = subprocess.run(
        ["ffmpeg", "-ss", str(start), "-t", str(length), "-i", video, "-vn", "-af", "volumedetect", "-f", "null", "-"],
        capture_output=True, text=True
    ).stderr
    return float(err.split("mean_volume:")[1].split("dB")[0])


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

    def test_picture_fades_in_and_out_even_without_cuts(self):
        for name, keep in (("fades.mp4", [(0.0, 6.0), (8.0, 20.0)]), ("uncut.mp4", [(0.0, 20.0)])):
            out = os.path.join(self.tmp, name)
            with quiet():
                self.assertTrue(yp.render_edited_video(self.main, out, keep, 20.0))
            end = yp.get_video_duration(out)
            self.assertLess(center_rgb(out, 0.0)[0], 40)          # starts black
            self.assertGreater(center_rgb(out, 3.0)[0], 180)      # red in the middle
            self.assertLess(center_rgb(out, end - 0.1)[0], 60)    # ends black
            # The sound keeps its level to the end (tapered by hand, if at all)
            self.assertGreater(mean_volume(out, end - 0.25, 0.2), mean_volume(out, 3.0, 0.5) - 6)

    def test_a_quiet_voice_is_brought_to_youtube_loudness(self):
        quiet_main = os.path.join(self.tmp, "quiet.mp4")
        ffmpeg("-i", self.main, "-c:v", "copy", "-af", "volume=-25dB", "-c:a", "aac", quiet_main)
        gain = yp.voice_gain_db(yp.measure_lufs(quiet_main))
        self.assertGreater(gain, 20)
        out = os.path.join(self.tmp, "loud.mp4")
        with quiet():
            self.assertTrue(yp.render_edited_video(quiet_main, out, [(0.0, 6.0), (8.0, 20.0)], 20.0, gain_db=gain))
        self.assertAlmostEqual(yp.measure_lufs(out), yp.VOICE_TARGET_LUFS, delta=1.0)
        err = subprocess.run(["ffmpeg", "-i", out, "-vn", "-af", "volumedetect", "-f", "null", "-"],
                             capture_output=True, text=True).stderr
        self.assertLess(float(err.split("max_volume:")[1].split("dB")[0]), -0.5)  # the limiter caught the peaks
        self.assertAlmostEqual(yp.get_video_duration(out), 18.0, delta=0.15)

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
            "secrets": {"gemini_key": "SECRET"}, "music_enabled": False,
            "broll": {"enabled": True, "screen_video": self.screen, "start_delta_sec": 1.5,
                      "markers": [], "style": "cutaway", "max_coverage": 0.5, "review": True},
        }
        patches = (
            mock.patch.object(yp, "transcribe_video", return_value=segs),
            mock.patch.object(yp, "generate_ai_thumbnails", return_value=([], "test")),
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

    def test_still_clip_zooms_at_the_edit_size(self):
        from PIL import Image
        picture = os.path.join(self.tmp, "still.png")
        Image.new("RGB", (800, 600), "green").save(picture)
        clip = os.path.join(self.tmp, "still.mp4")
        self.assertTrue(yp.make_still_clip(picture, 2.0, 640, 360, "30/1", clip, n=1))
        self.assertAlmostEqual(yp.get_video_duration(clip), 2.0, delta=0.1)
        self.assertEqual(yp.probe_video_geometry(clip)[:2], (640, 360))

    def test_picture_inserts_render_over_the_webcam(self):
        from PIL import Image
        picture = os.path.join(self.tmp, "picture.png")
        Image.new("RGB", (1344, 768), "lime").save(picture)
        clip = os.path.join(self.tmp, "picture.mp4")
        self.assertTrue(yp.make_still_clip(picture, 4.2, 640, 360, "30/1", clip))
        out = os.path.join(self.tmp, "pictures.mp4")
        broll = {"offset": 0.0, "inserts": [], "images": [(10.0, 14.0, clip)], "style": "cutaway"}
        with quiet():
            self.assertTrue(yp.render_edited_video(self.main, out, [(0.0, 6.0), (8.0, 20.0)], 20.0, broll))
        self.assertGreater(center_rgb(out, 9.0)[1], 180)   # raw 11 s is 9 s into the edit: the picture
        self.assertGreater(center_rgb(out, 3.0)[0], 180)   # the webcam elsewhere
        self.assertAlmostEqual(yp.get_video_duration(out), 18.0, delta=0.15)

    def test_screen_and_picture_in_one_edit(self):
        from PIL import Image
        picture = os.path.join(self.tmp, "picture2.png")
        Image.new("RGB", (1344, 768), "lime").save(picture)
        clip = os.path.join(self.tmp, "picture2.mp4")
        self.assertTrue(yp.make_still_clip(picture, 3.2, 640, 360, "30/1", clip))
        out = os.path.join(self.tmp, "both.mp4")
        broll = {"screen_path": self.screen, "offset": -1.5, "inserts": [(15.0, 18.0)],
                 "images": [(9.0, 12.0, clip)], "style": "cutaway"}
        with quiet():
            self.assertTrue(yp.render_edited_video(self.main, out, [(0.0, 20.0)], 20.0, broll))
        self.assertGreater(center_rgb(out, 10.5)[1], 180)  # picture
        self.assertGreater(center_rgb(out, 16.5)[2], 180)  # screen
        self.assertGreater(center_rgb(out, 5.0)[0], 180)   # webcam

    def test_pictures_in_step_mode_then_auto(self):
        import json
        from unittest import mock

        segs = [segment(9.0, 14.0, "Think of compound interest as a snowball rolling down a hill")]
        base = {
            "mode": "pipeline", "input_video": self.main, "priority_list": [], "skip_upload": True,
            "secrets": {}, "music_enabled": False,
        }
        patches = [
            mock.patch.object(yp, "transcribe_video", return_value=segs),
            mock.patch.object(yp, "generate_ai_thumbnails", return_value=([], "test")),
            mock.patch.object(yp, "generate_sd_images", side_effect=fake_sd("lime")),
        ]

        def events(fn, *args):
            buf = io.StringIO()
            with contextlib.ExitStack() as stack:
                for p in patches:
                    stack.enter_context(p)
                stack.enter_context(contextlib.redirect_stdout(buf))
                rc = fn(*args)
            return rc, [json.loads(line) for line in buf.getvalue().splitlines()]

        # Step mode: pauses with the picked spot, no picture yet
        cfg = dict(base, broll={"enabled": True, "amount": 3, "image_source": "sd", "style": "cutaway",
                                "max_coverage": 0.5, "review": True})
        rc, evs = events(yp.run_pipeline, cfg)
        ready = [e for e in evs if e["type"] == "broll_plan_ready"]
        self.assertEqual((rc, len(ready)), (0, 1))
        plan = yp.read_json_file(ready[0]["plan_path"])
        ins = plan["inserts"][0]
        self.assertEqual((ins["kind"], ins["picked"], ins["image"]), ("image", True, ""))

        # "Make images" in the panel, with an edited prompt
        rc, evs = events(yp.broll_images, ready[0]["state_path"], {},
                         {"ids": [ins["id"]], "prompts": {ins["id"]: "a snowball on a hill"}, "sources": {ins["id"]: "sd"}})
        self.assertEqual(rc, 0)
        self.assertTrue(any(e["type"] == "broll_plan_ready" and e.get("made") == 1 for e in evs))
        plan = yp.read_json_file(ready[0]["plan_path"])
        self.assertEqual(plan["inserts"][0]["prompt"], "a snowball on a hill")
        self.assertTrue(os.path.exists(plan["inserts"][0]["image"]))

        plan["inserts"][0]["status"] = "accepted"
        yp.write_json_file(ready[0]["plan_path"], plan)
        rc, evs = events(yp.resume_pipeline, ready[0]["state_path"], {})
        done = [e for e in evs if e["type"] == "complete"]
        self.assertEqual((rc, len(done)), (0, 1))
        self.assertGreater(center_rgb(done[0]["edited_video"], 11.5)[1], 180)

        # Auto mode: makes the picked pictures itself and renders them
        cfg = dict(base, broll={"enabled": True, "amount": 3, "image_source": "sd", "style": "cutaway",
                                "max_coverage": 0.5, "review": False})
        rc, evs = events(yp.run_pipeline, cfg)
        done = [e for e in evs if e["type"] == "complete"]
        self.assertEqual((rc, len(done)), (0, 1))
        self.assertGreater(center_rgb(done[0]["edited_video"], 11.5)[1], 180)


if __name__ == "__main__":
    unittest.main(verbosity=2)
