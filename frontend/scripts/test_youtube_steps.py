"""
Checks for Step / Auto mode in youtube_pipeline.py: the reviewed cut list, pausing at each
review gate and resuming, and the thumbnail candidates.

    python frontend/scripts/test_youtube_steps.py

The pipeline tests need ffmpeg/ffprobe on PATH (plus OpenCV and Pillow) and are skipped otherwise.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import premiere_export as pe  # noqa: E402
import youtube_pipeline as yp  # noqa: E402

HAVE_FFMPEG = bool(shutil.which("ffmpeg") and shutil.which("ffprobe"))


class CutPlanTests(unittest.TestCase):
    keep = [(0.0, 5.0), (8.0, 12.0), (15.0, 18.0)]
    retakes = [(12.2, 14.8, "Retake: restarted 'so the next step'")]

    def test_cuts_are_the_gaps(self):
        cuts = yp.cuts_from_keep(self.keep, self.retakes, 20.0)
        self.assertEqual([(c["start"], c["end"]) for c in cuts], [(5.0, 8.0), (12.0, 15.0), (18.0, 20.0)])
        self.assertEqual([c["kind"] for c in cuts], ["silence", "retake", "silence"])
        self.assertIn("restarted", cuts[1]["reason"])
        self.assertTrue(all(c["enabled"] for c in cuts))
        self.assertEqual(cuts[0]["id"], "c01")

    def test_round_trip(self):
        cuts = yp.cuts_from_keep(self.keep, self.retakes, 20.0)
        self.assertEqual(yp.keep_from_cuts(cuts, 20.0), self.keep)

    def test_disabled_cut_keeps_that_part(self):
        cuts = yp.cuts_from_keep(self.keep, self.retakes, 20.0)
        cuts[1]["enabled"] = False
        self.assertEqual(yp.keep_from_cuts(cuts, 20.0), [(0.0, 5.0), (8.0, 18.0)])

    def test_widened_and_shrunk_cuts(self):
        cuts = yp.cuts_from_keep(self.keep, self.retakes, 20.0)
        cuts[0]["start"] = 4.5   # cut more before
        cuts[0]["end"] = 7.6     # cut less after
        self.assertEqual(yp.keep_from_cuts(cuts, 20.0)[:2], [(0.0, 4.5), (7.6, 12.0)])

    def test_overlapping_cuts_merge_and_slivers_drop(self):
        cuts = [
            {"start": 2.0, "end": 4.0, "enabled": True},
            {"start": 3.5, "end": 6.0, "enabled": True},
            {"start": 6.1, "end": 7.0, "enabled": True},     # leaves a 0.1 s sliver: dropped
            {"start": 9.0, "end": 9.05, "enabled": True},    # too short to count as a cut
        ]
        self.assertEqual(yp.keep_from_cuts(cuts, 10.0), [(0.0, 2.0), (7.0, 10.0)])

    def test_cuts_clamp_to_the_video(self):
        cuts = [{"start": -1.0, "end": 1.0, "enabled": True}, {"start": 9.0, "end": 12.0, "enabled": True}]
        self.assertEqual(yp.keep_from_cuts(cuts, 10.0), [(1.0, 9.0)])


class GateConfigTests(unittest.TestCase):
    def test_explicit_gates(self):
        self.assertEqual(yp.review_gates({"review_gates": ["cuts", "thumbnail", "bogus"]}), {"cuts", "thumbnail"})
        self.assertEqual(yp.review_gates({"review_gates": []}), set())

    def test_older_callers_only_know_broll_review(self):
        self.assertEqual(yp.review_gates({"broll": {"review": True}}), {"broll"})
        self.assertEqual(yp.review_gates({"broll": {"review": False}}), set())


class ThumbnailChoiceTests(unittest.TestCase):
    def test_chosen_path_falls_back_to_an_existing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = os.path.join(tmp, "a.jpg")
            open(a, "wb").close()
            thumbs = {"candidates": [{"id": "frame_1", "path": a}, {"id": "ai_1", "path": os.path.join(tmp, "gone.jpg")}],
                      "chosen": "ai_1"}
            self.assertEqual(yp.chosen_thumbnail_path(thumbs), a)
            thumbs["chosen"] = "frame_1"
            self.assertEqual(yp.chosen_thumbnail_path(thumbs), a)


class StableDiffusionThumbnailTests(unittest.TestCase):
    META = {"title": "My Video", "thumbnail_hook": "BIG HOOK", "thumbnail_prompt": "a desk"}

    def test_sd_images_become_thumbnails(self):
        from PIL import Image

        made = [Image.new("RGB", (1344, 768), (200, 30, 30)) for _ in range(2)]
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(yp, "generate_sd_images", return_value=made) as gen, \
                contextlib.redirect_stdout(io.StringIO()):
            made_thumbs, label = yp.generate_ai_thumbnails("sd", 2, {}, self.META, [], tmp)
            self.assertEqual([os.path.basename(m["path"]) for m in made_thumbs], ["ai_1.jpg", "ai_2.jpg"])
            self.assertEqual([os.path.basename(m["bg"]) for m in made_thumbs], ["ai_1_bg.jpg", "ai_2_bg.jpg"])
            self.assertEqual(Image.open(made_thumbs[0]["path"]).size, (1280, 720))
            self.assertIn("Stable Diffusion", label)
            prompts = gen.call_args.args[1]
            self.assertEqual(len(prompts), 2)
            self.assertIn("a desk", prompts[0])

    def test_leave_out_and_person_shape_the_prompts(self):
        prompts, negative = yp.thumbnail_prompts("a desk", 2, {"avoid": "hands, neon", "person": "photo",
                                                               "side": "left", "style": "moody"})
        self.assertIn("empty space on the left side", prompts[0])
        self.assertIn("rim lighting", prompts[1])
        self.assertTrue(negative.endswith("hands, neon"))
        self.assertIn("people", negative)
        _, plain = yp.thumbnail_prompts("a desk", 1, {})
        self.assertEqual(plain, yp.SD_NEGATIVE_PROMPT)

    def test_missing_install_falls_back_to_designed_frames(self):
        config = {"thumbnail_sd_python": os.path.join(tempfile.gettempdir(), "no_such_venv", "python.exe")}
        notes = []
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(yp, "call_local_ollama_json", return_value={"designs": []}), \
                contextlib.redirect_stdout(io.StringIO()):
            made, label = yp.generate_ai_thumbnails("sd", 2, config, self.META, [], tmp, notes=notes)
            self.assertEqual(len(made), 2)
            self.assertIn("compositor", label)
            self.assertTrue(any("isn't installed" in n for n in notes))


class EtaTests(unittest.TestCase):
    T = {"load_s": 15.0, "first_image_s": 27.0, "image_s": 10.0}

    def test_loading_uses_saved_timings(self):
        self.assertAlmostEqual(yp.sd_eta_seconds(self.T, 4, 0, "loading", elapsed=5.0), 10 + 27 + 30)
        self.assertAlmostEqual(yp.sd_eta_seconds(self.T, 4, 0, "loading", elapsed=99.0), 27 + 30)

    def test_live_steps_win_over_history(self):
        # 0.4 s a step with 20 of 30 steps left, then 2 more images at the last measured 12 s
        self.assertAlmostEqual(yp.sd_eta_seconds(self.T, 4, 1, "image", elapsed=9.0, step=10, steps=30,
                                                 last_image_s=12.0, step_rate=0.4), 8 + 2 * 12)
        # Before a step rate is known (warm-up), count down from the expected image time
        self.assertAlmostEqual(yp.sd_eta_seconds(self.T, 4, 1, "image", elapsed=3.0, step=1, steps=30,
                                                 last_image_s=12.0), 9 + 2 * 12)
        self.assertEqual(yp.sd_eta_seconds(self.T, 2, 2, "image", elapsed=5.0), 0.0)

    def test_refine_scales_by_strength(self):
        self.assertAlmostEqual(yp.sd_eta_seconds(self.T, 2, 0, "image", work=0.5), 13.5 + 5)

    def test_timings_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(yp, "SD_HOME", tmp):
            self.assertEqual(yp.load_sd_timings(), yp.SD_TIMINGS_DEFAULT)
            yp.save_sd_timings(12.0, [20.0, 9.0, 11.0])
            self.assertEqual(yp.load_sd_timings(), {"load_s": 12.0, "first_image_s": 20.0, "image_s": 10.0})


class ComposeTests(unittest.TestCase):
    def person(self):
        from PIL import Image

        img = Image.new("RGBA", (300, 600), (0, 0, 0, 0))
        img.paste((0, 255, 0, 255), (50, 50, 250, 600))
        return img

    def test_person_lands_on_the_chosen_side(self):
        from PIL import Image

        bg = Image.new("RGB", (1344, 768), (40, 40, 200))
        for side in ("left", "right"):
            img = yp.compose_thumbnail(bg, "", self.person(), side)
            self.assertEqual(img.size, (1280, 720))
            green = [x for x in range(0, 1280, 8) if img.getpixel((x, 600))[1] > 200 and img.getpixel((x, 600))[0] < 120]
            self.assertTrue(green)
            if side == "left":
                self.assertLess(max(green), 640)
            else:
                self.assertGreater(min(green), 640)

    def test_text_goes_opposite_the_person(self):
        from PIL import Image

        bg = Image.new("RGB", (1280, 720), (128, 128, 128))
        img = yp.compose_thumbnail(bg, "BIG HOOK", self.person(), "left")
        yellow = [x for x in range(0, 1280, 4) for y in range(250, 470, 10)
                  if img.getpixel((x, y))[0] > 230 and img.getpixel((x, y))[1] > 200 and img.getpixel((x, y))[2] < 90]
        self.assertTrue(yellow)
        self.assertGreater(min(yellow), 640)


class PremiereXmlTests(unittest.TestCase):
    PROBE = {"duration": 20.0, "fps": 30000 / 1001, "width": 1920, "height": 1080,
             "audio": [{"channels": 2, "sample_rate": 48000}, {"channels": 1, "sample_rate": 48000}]}

    def build(self, probe=None, broll=None):
        import xml.etree.ElementTree as ET

        xml = pe.build_premiere_xml(
            r"G:\obs rec\My Video.mp4", probe or self.PROBE, [(0.0, 5.0), (8.0, 12.0), (15.0, 18.0)], broll,
            [(0.0, "Intro"), (6.0, "Setup")], [(5.0, "Cut: silence 3.0s", "")])
        return ET.fromstring(xml.split("\n", 2)[2])

    def test_cuts_are_back_to_back_clips(self):
        root = self.build()
        rate = root.find("sequence/rate")
        self.assertEqual((rate.findtext("timebase"), rate.findtext("ntsc")), ("30", "TRUE"))
        v1 = root.findall("sequence/media/video/track")[0].findall("clipitem")
        spans = [(int(c.findtext("start")), int(c.findtext("end")), int(c.findtext("in")), int(c.findtext("out"))) for c in v1]
        self.assertEqual(spans[0], (0, 150, 0, 150))
        self.assertEqual(spans[1][2:], (240, 360))
        for prev, cur in zip(spans, spans[1:]):
            self.assertEqual(prev[1], cur[0])  # no gaps
        self.assertEqual(int(root.findtext("sequence/duration")), spans[-1][1])

    def test_one_audio_track_per_stream_and_markers(self):
        root = self.build()
        tracks = root.findall("sequence/media/audio/track")
        self.assertEqual([t.get("premiereTrackType") for t in tracks], ["Stereo", "Mono"])
        self.assertEqual(tracks[1].find("clipitem/sourcetrack/trackindex").text, "2")
        self.assertEqual([m.findtext("name") for m in root.findall("sequence/marker")], ["Intro", "Setup", "Cut: silence 3.0s"])
        url = root.find(".//file/pathurl").text
        self.assertTrue(url.startswith("file://localhost/G%3A/obs%20rec/My%20Video.mp4"))

    def test_60fps_is_not_ntsc(self):
        root = self.build(dict(self.PROBE, fps=60.0))
        self.assertEqual(root.findtext("sequence/rate/ntsc"), "FALSE")
        self.assertEqual(root.findall("sequence/media/video/track")[0].find("clipitem").findtext("out"), "300")

    def test_pip_broll_adds_screen_and_bubble_tracks(self):
        broll = {"path": r"G:\obs rec\screen.mkv", "probe": dict(self.PROBE, width=2560, height=1440), "offset": 1.0,
                 "inserts": [(9.0, 11.0)], "style": "pip", "pip_width": 0.22}
        tracks = self.build(broll=broll).findall("sequence/media/video/track")
        self.assertEqual(len(tracks), 3)
        screen = tracks[1].find("clipitem")
        self.assertEqual(int(screen.findtext("start")), 150 + 30)  # 1 s into the second part
        self.assertEqual(int(screen.findtext("in")), round(10.0 * 30000 / 1001))
        self.assertEqual(screen.find(".//parameter/value").text, "75.0")
        self.assertEqual(len(tracks[2].findall("clipitem")), 1)


class ChapterTests(unittest.TestCase):
    def test_parse_chapters(self):
        desc = "Intro text\n00:00 - Intro\n01:05 Setting up\n1:02:03 Long one\nnot 12 a chapter"
        self.assertEqual(yp.parse_chapters(desc), [(0.0, "Intro"), (65.0, "Setting up"), (3723.0, "Long one")])

    def test_joins_are_running_totals(self):
        self.assertEqual(yp.edit_joins([(0.0, 5.0), (8.0, 12.0), (15.0, 18.0)]), [5.0, 9.0])
        self.assertEqual(yp.edit_joins([(0.0, 5.0)]), [])


class DeleteVideoTests(unittest.TestCase):
    def test_needs_the_edit_scope(self):
        token = json.dumps({"scopes": ["https://www.googleapis.com/auth/youtube.upload"]})
        with self.assertRaises(PermissionError):
            yp.delete_youtube_video("abc", token)

    def test_already_gone_counts_as_deleted(self):
        from googleapiclient.errors import HttpError

        token = json.dumps({"scopes": ["https://www.googleapis.com/auth/youtube"]})
        youtube = mock.MagicMock()
        youtube.videos.return_value.delete.return_value.execute.side_effect = HttpError(
            mock.Mock(status=404), b"not found")
        with mock.patch("googleapiclient.discovery.build", return_value=youtube), \
                mock.patch.object(yp, "load_youtube_credentials"), contextlib.redirect_stdout(io.StringIO()):
            yp.delete_youtube_video("abc", token)
        youtube.videos.return_value.delete.assert_called_once_with(id="abc")


def png_bytes(color=(20, 120, 200)):
    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGB", (64, 36), color).save(buf, "PNG")
    return buf.getvalue()


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def openai_ok(*args, **kwargs):
    return FakeResponse(json.dumps({"data": [{"b64_json": yp.base64.b64encode(png_bytes()).decode()}]}).encode())


def openai_error(code, err_code, message):
    import urllib.error

    body = json.dumps({"error": {"code": err_code, "message": message}}).encode()
    return urllib.error.HTTPError("https://api.openai.com", code, "error", {}, io.BytesIO(body))


def gemini_reply(*parts):
    from types import SimpleNamespace as NS

    return NS(candidates=[NS(content=NS(parts=[
        NS(inline_data=NS(data=p), text=None) if isinstance(p, bytes) else NS(inline_data=None, text=p)
        for p in parts]))])


class CloudImageTests(unittest.TestCase):
    SECRETS = {"openai_key": "sk-test", "gemini_key": "g-test"}

    def setUp(self):
        yp._working_cloud_model.clear()
        yp._exhausted_cloud_models.clear()
        yp._refused_cloud_keys.clear()

    def test_openai_generate_uses_gpt_image(self):
        with mock.patch("urllib.request.urlopen", side_effect=openai_ok) as urlopen:
            img, _, model = yp.generate_cloud_image("openai", self.SECRETS, "a desk")
        req = urlopen.call_args.args[0]
        body = json.loads(req.data)
        self.assertEqual(model, "gpt-image-2")
        self.assertTrue(req.full_url.endswith("/images/generations"))
        self.assertEqual((body["model"], body["size"]), ("gpt-image-2", "1536x1024"))
        self.assertNotIn("response_format", body)
        self.assertEqual(img.size, (64, 36))

    def test_openai_edit_sends_the_background(self):
        from PIL import Image

        with mock.patch("urllib.request.urlopen", side_effect=openai_ok) as urlopen:
            yp.generate_cloud_image("openai", self.SECRETS, "make it blue", Image.new("RGB", (32, 18)))
        req = urlopen.call_args.args[0]
        self.assertTrue(req.full_url.endswith("/images/edits"))
        self.assertIn("multipart/form-data; boundary=", req.get_header("Content-type"))
        self.assertIn(b'name="image"; filename="background.png"', req.data)
        self.assertIn(b"\x89PNG", req.data)

    def test_missing_model_falls_back_and_is_remembered(self):
        calls = []

        def urlopen(req, timeout=0):
            model = json.loads(req.data)["model"]
            calls.append(model)
            if model == "gpt-image-2":
                raise openai_error(404, "model_not_found", "The model `gpt-image-2` does not exist")
            return openai_ok()

        with mock.patch("urllib.request.urlopen", side_effect=urlopen), contextlib.redirect_stdout(io.StringIO()):
            _, _, model = yp.generate_cloud_image("openai", self.SECRETS, "a")
            yp.generate_cloud_image("openai", self.SECRETS, "b")
        self.assertEqual(model, "gpt-image-1.5")
        self.assertEqual(calls, ["gpt-image-2", "gpt-image-1.5", "gpt-image-1.5"])

    def test_auth_error_does_not_fall_back(self):
        with mock.patch("urllib.request.urlopen", side_effect=openai_error(401, "invalid_api_key", "Bad key")) as u, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "key was refused.*Bad key"):
                yp.generate_cloud_image("openai", self.SECRETS, "a")
        self.assertEqual(u.call_count, 1)

    QUOTA = ("429 RESOURCE_EXHAUSTED. {'error': {'code': 429, 'message': 'You exceeded your current quota. "
             "Quota exceeded for metric: generate_content_free_tier_requests, limit: 0. "
             "Please retry in 4h43m42.97s.', 'status': 'RESOURCE_EXHAUSTED'}}")

    def gemini_client(self, quota_models):
        client = mock.MagicMock()

        def generate(model, **kwargs):
            if model in quota_models:
                raise RuntimeError(self.QUOTA)
            return gemini_reply(png_bytes())

        client.models.generate_content.side_effect = generate
        return client

    def test_quota_error_is_one_readable_line(self):
        msg = yp.short_cloud_error(RuntimeError(self.QUOTA))
        self.assertEqual(msg, "out of quota (429); free-tier keys have no image quota, so enable billing on "
                              "this key's Google project (resets in 4h 44m)")

    def test_quota_tries_the_next_nano_banana(self):
        client = self.gemini_client({"gemini-3-pro-image-preview"})
        with mock.patch("google.genai.Client", return_value=client), contextlib.redirect_stdout(io.StringIO()):
            _, source, model = yp.generate_cloud_image("gemini_pro", self.SECRETS, "a")
            yp.generate_cloud_image("gemini_pro", self.SECRETS, "b")
        self.assertEqual((source, model), ("gemini_pro", "gemini-3.1-flash-image-preview"))
        called = [c.kwargs["model"] for c in client.models.generate_content.call_args_list]
        self.assertEqual(called, ["gemini-3-pro-image-preview", "gemini-3.1-flash-image-preview",
                                  "gemini-3.1-flash-image-preview"])

    def test_quota_on_every_gemini_falls_back_to_gpt_image(self):
        client = self.gemini_client(set(yp.CLOUD_IMAGE_MODELS["gemini_pro"][1]))
        with mock.patch("google.genai.Client", return_value=client), \
                mock.patch("urllib.request.urlopen", side_effect=openai_ok), \
                contextlib.redirect_stdout(io.StringIO()):
            _, source, model = yp.generate_cloud_image("gemini_pro", self.SECRETS, "a", fallback=True)
            _, source2, _ = yp.generate_cloud_image("gemini_pro", self.SECRETS, "b", fallback=True)
        self.assertEqual((source, model, source2), ("openai", "gpt-image-2", "openai"))
        # Each Gemini model is tried once, not again for the second image
        self.assertEqual(client.models.generate_content.call_count, 3)

    def test_no_fallback_without_the_option_or_a_key(self):
        client = self.gemini_client(set(yp.CLOUD_IMAGE_MODELS["gemini_pro"][1]))
        for fallback, secrets in ((False, self.SECRETS), (True, {"gemini_key": "g-test"})):
            yp._exhausted_cloud_models.clear()
            with mock.patch("google.genai.Client", return_value=client), \
                    mock.patch("urllib.request.urlopen", side_effect=openai_ok) as urlopen, \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "out of quota"):
                    yp.generate_cloud_image("gemini_pro", secrets, "a", fallback=fallback)
            urlopen.assert_not_called()

    def test_refused_key_falls_back_to_the_other_provider(self):
        client = mock.MagicMock()
        client.models.generate_content.side_effect = RuntimeError("400 INVALID_ARGUMENT. API key not valid.")
        with mock.patch("google.genai.Client", return_value=client), \
                mock.patch("urllib.request.urlopen", side_effect=openai_ok), \
                contextlib.redirect_stdout(io.StringIO()):
            _, source, _ = yp.generate_cloud_image("gemini", self.SECRETS, "a", fallback=True)
        self.assertEqual(source, "openai")
        self.assertEqual(client.models.generate_content.call_count, 1)

    def test_thumbnails_say_which_model_stood_in(self):
        from PIL import Image

        img = Image.new("RGB", (1536, 1024), (30, 30, 30))
        notes = []
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(yp, "generate_cloud_image", return_value=(img, "openai", "gpt-image-2")) as cloud, \
                contextlib.redirect_stdout(io.StringIO()):
            made, label = yp.generate_ai_thumbnails("gemini_pro", 2, {"secrets": self.SECRETS},
                                                    StableDiffusionThumbnailTests.META, [], tmp, notes=notes)
        self.assertEqual(label, "OpenAI GPT Image (gpt-image-2)")
        self.assertTrue(cloud.call_args.kwargs["fallback"])
        self.assertEqual(notes, ["Google Nano Banana Pro was out of quota or refused the key, "
                                 "so OpenAI GPT Image (gpt-image-2) made these."])

    def test_gemini_pro_tries_pro_first(self):
        client = mock.MagicMock()
        client.models.generate_content.return_value = gemini_reply("Here you go", png_bytes())
        with mock.patch("google.genai.Client", return_value=client):
            img, _, model = yp.generate_cloud_image("gemini_pro", self.SECRETS, "a desk")
        self.assertEqual(model, "gemini-3-pro-image-preview")
        kwargs = client.models.generate_content.call_args.kwargs
        self.assertEqual(kwargs["model"], "gemini-3-pro-image-preview")
        self.assertEqual(kwargs["config"].image_config.aspect_ratio, "16:9")
        self.assertEqual(img.size, (64, 36))

    def test_gemini_text_only_reply_raises_with_the_text(self):
        client = mock.MagicMock()
        client.models.generate_content.return_value = gemini_reply("I can't make that image.")
        with mock.patch("google.genai.Client", return_value=client):
            with self.assertRaisesRegex(RuntimeError, "can't make that image"):
                yp.generate_cloud_image("gemini", self.SECRETS, "a")

    def test_cloud_thumbnails_name_the_model(self):
        from PIL import Image

        img = Image.new("RGB", (1536, 1024), (30, 30, 30))
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(yp, "generate_cloud_image", return_value=(img, "openai", "gpt-image-2")), \
                contextlib.redirect_stdout(io.StringIO()):
            made, label = yp.generate_ai_thumbnails("openai", 2, {"secrets": self.SECRETS},
                                                    StableDiffusionThumbnailTests.META, [], tmp)
        self.assertEqual(len(made), 2)
        self.assertEqual(label, "OpenAI GPT Image (gpt-image-2)")

    def refine(self, kind, extra=None):
        from PIL import Image

        notes = []
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            bg = os.path.join(tmp, "x_bg.jpg")
            Image.new("RGB", (1280, 720)).save(bg)
            cand = {"id": "x", "kind": kind, "bg": bg, **(extra or {})}
            img = Image.new("RGB", (1280, 720), (90, 90, 90))
            with mock.patch.object(yp, "generate_cloud_image", return_value=(img, "openai", "gpt-image-2")) as cloud, \
                    mock.patch.object(yp, "generate_sd_images", return_value=[img, img]) as sd:
                made = yp.refine_thumbnail({"thumbnail_image_source": "openai", "secrets": self.SECRETS}, cand,
                                           StableDiffusionThumbnailTests.META,
                                           {"strength": 0.3, "avoid": "hands"}, tmp, None, notes, 1)
            return made, cloud, sd, notes

    def test_cloud_refine_uses_the_edit_mode(self):
        made, cloud, sd, _ = self.refine("ai")
        self.assertEqual(len(made), 2)
        sd.assert_not_called()
        prompt, init = cloud.call_args.args[2], cloud.call_args.args[3]
        self.assertIn("small changes", prompt)
        self.assertIn("Do not show: hands", prompt)
        self.assertEqual(init.size, (1280, 720))

    def test_frames_of_you_are_refined_locally(self):
        for kind, extra in (("frame", None), ("refined", {"shows_you": True})):
            made, cloud, sd, notes = self.refine(kind, extra)
            cloud.assert_not_called()
            sd.assert_called_once()
            self.assertTrue(any("nothing of you is uploaded" in n for n in notes))


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-y", "-v", "error", *args], check=True)


def run_events(fn, *args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fn(*args)
    return rc, [json.loads(line) for line in buf.getvalue().splitlines()]


def gemini_text_ok(*args, **kwargs):
    text = json.dumps({"title": "Gemini Title"})
    return FakeResponse(json.dumps({"candidates": [{"content": {"parts": [{"text": text}]}}]}).encode())


def gemini_error(code, status, message):
    import urllib.error

    body = json.dumps({"error": {"code": code, "status": status, "message": message}}).encode()
    return urllib.error.HTTPError("https://generativelanguage.googleapis.com", code, "error", {}, io.BytesIO(body))


def requested_model(req):
    return req.full_url.split("/models/")[1].split(":")[0]


class GeminiTextTests(unittest.TestCase):
    def setUp(self):
        yp._working_cloud_model.clear()

    def test_uses_the_newest_model_first(self):
        with mock.patch("urllib.request.urlopen", side_effect=gemini_text_ok) as urlopen:
            res = yp.call_gemini_json("g-test", "sys", "text")
        self.assertEqual(urlopen.call_count, 1)
        self.assertEqual(requested_model(urlopen.call_args.args[0]), "gemini-3-flash-preview")
        self.assertEqual(res["title"], "Gemini Title")
        self.assertEqual(res["_model_used"], "Google Gemini (gemini-3-flash-preview)")

    def test_falls_back_when_a_model_is_gone_and_remembers(self):
        def urlopen(req, timeout=None):
            if requested_model(req) == "gemini-3-flash-preview":
                raise gemini_error(404, "NOT_FOUND", "models/gemini-3-flash-preview is not found")
            return gemini_text_ok()

        with mock.patch("urllib.request.urlopen", side_effect=urlopen) as fake:
            res = yp.call_gemini_json("g-test", "sys", "text")
            self.assertEqual(res["_model_used"], "Google Gemini (gemini-3.1-pro-preview)")
            fake.reset_mock()
            yp.call_gemini_json("g-test", "sys", "text")
        self.assertEqual(fake.call_count, 1)
        self.assertEqual(requested_model(fake.call_args.args[0]), "gemini-3.1-pro-preview")

    def test_key_error_does_not_fall_back(self):
        err = gemini_error(400, "INVALID_ARGUMENT", "API key not valid. Please pass a valid API key.")
        with mock.patch("urllib.request.urlopen", side_effect=err) as urlopen:
            with self.assertRaisesRegex(RuntimeError, "API key not valid"):
                yp.call_gemini_json("bad", "sys", "text")
        self.assertEqual(urlopen.call_count, 1)

    def test_asks_google_for_a_model_when_all_are_gone(self):
        listing = {"models": [
            {"name": "models/gemini-2.0-flash", "supportedGenerationMethods": ["generateContent"]},
            {"name": "models/gemini-4-flash", "supportedGenerationMethods": ["generateContent"]},
            {"name": "models/gemini-4-flash-image", "supportedGenerationMethods": ["generateContent"]},
            {"name": "models/gemini-4-flash-lite", "supportedGenerationMethods": ["generateContent"]},
            {"name": "models/gemini-5-pro", "supportedGenerationMethods": ["generateContent"]},
        ]}

        def urlopen(req, timeout=None):
            if ":generateContent" not in req.full_url:
                return FakeResponse(json.dumps(listing).encode())
            if requested_model(req) in yp.GEMINI_TEXT_MODELS:
                raise gemini_error(404, "NOT_FOUND", "model is not found")
            return gemini_text_ok()

        with mock.patch("urllib.request.urlopen", side_effect=urlopen) as fake:
            res = yp.call_gemini_json("g-test", "sys", "text")
        self.assertEqual(res["_model_used"], "Google Gemini (gemini-4-flash)")
        self.assertEqual(requested_model(fake.call_args.args[0]), "gemini-4-flash")


FAKE_METADATA = {
    "title": "AI Title", "alternative_titles": ["Alt One", "Alt Two"], "description": "Desc",
    "tags": ["a"], "thumbnail_hook": "BIG HOOK", "thumbnail_prompt": "a desk",
}


@unittest.skipUnless(HAVE_FFMPEG, "ffmpeg not on PATH")
class StepRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yt_steps_test_")
        cls.source = os.path.join(cls.tmp, "source.mp4")
        # 20 s: noise 0-6 s, 4 s of silence, noise 10-20 s, over a moving test pattern
        ffmpeg("-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=20",
               "-f", "lavfi", "-i", "anoisesrc=d=6:c=pink:r=48000:seed=1:a=0.5",
               "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono:d=4",
               "-f", "lavfi", "-i", "anoisesrc=d=10:c=pink:r=48000:seed=2:a=0.5",
               "-filter_complex", "[1:a][2:a][3:a]concat=n=3:v=0:a=1[a]",
               "-map", "0:v", "-map", "[a]", "-shortest",
               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", cls.source)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def video_copy(self, name):
        path = os.path.join(self.tmp, name)
        shutil.copyfile(self.source, path)
        return path

    def config(self, video, gates):
        return {
            "mode": "pipeline", "input_video": video, "priority_list": ["local"], "skip_upload": True,
            "review_gates": gates, "thumbnail_image_source": "local", "secrets": {"gemini_key": "SECRET"},
        }

    def patches(self):
        # No Whisper, no LLM, no Ollama image model: exercises the free designed-thumbnail fallback
        return (
            mock.patch.object(yp, "transcribe_video", return_value=[]),
            mock.patch.object(yp, "generate_metadata_with_priority", side_effect=lambda **kw: (dict(FAKE_METADATA), "test")),
            mock.patch.object(yp, "generate_ollama_image", side_effect=RuntimeError("no image model")),
            mock.patch.object(yp, "call_local_ollama_json", side_effect=RuntimeError("offline")),
        )

    def run_with_patches(self, fn, *args):
        p = self.patches()
        with p[0], p[1], p[2], p[3]:
            return run_events(fn, *args)

    def test_step_mode_pauses_at_each_gate(self):
        video = self.video_copy("step.mp4")
        rc, evs = self.run_with_patches(yp.run_pipeline, self.config(video, list(yp.REVIEW_GATES)))
        self.assertEqual(rc, 0)
        ready = [e for e in evs if e["type"] == "review_ready"]
        self.assertEqual([e["gate"] for e in ready], ["cuts"])
        self.assertFalse(any(e["type"] in ("metadata_ready", "complete") for e in evs))
        state_path = ready[0]["state_path"]
        with open(state_path, encoding="utf-8") as f:
            self.assertNotIn("SECRET", f.read())

        # Cut editor: undo every cut, so the render keeps the whole 20 s
        cuts_file = yp.read_json_file(ready[0]["data_path"])
        self.assertGreaterEqual(len(cuts_file["cuts"]), 1)
        for c in cuts_file["cuts"]:
            c["enabled"] = False
        yp.write_json_file(ready[0]["data_path"], cuts_file)

        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {})
        ready = [e for e in evs if e["type"] == "review_ready"]
        self.assertEqual([e["gate"] for e in ready], ["metadata"])
        edited = yp.pipeline_paths(video)["edited_video"]
        self.assertAlmostEqual(yp.get_video_duration(edited), 20.0, delta=0.3)

        # Metadata review: change the title
        meta = yp.read_json_file(ready[0]["data_path"])
        meta["title"] = "My Own Title"
        yp.write_json_file(ready[0]["data_path"], meta)

        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {})
        ready = [e for e in evs if e["type"] == "review_ready"]
        self.assertEqual([e["gate"] for e in ready], ["thumbnail"])
        self.assertEqual([e["title"] for e in evs if e["type"] == "metadata_ready"], ["My Own Title"])
        thumbs = yp.read_json_file(ready[0]["data_path"])
        ids = [c["id"] for c in thumbs["candidates"]]
        self.assertEqual(ids, ["frame_1", "frame_2", "frame_3", "frame_4", "ai_1", "ai_2", "ai_3", "ai_4"])
        self.assertTrue(all(os.path.exists(c["path"]) for c in thumbs["candidates"]))

        # Thumbnail pick: waits at "Ready to publish", not uploading yet
        thumbs["chosen"] = "frame_3"
        yp.write_json_file(ready[0]["data_path"], thumbs)
        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {})
        self.assertEqual([e["gate"] for e in evs if e["type"] == "review_ready"], ["publish"])
        self.assertFalse(any(e["type"] == "complete" for e in evs))

        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {}, {"action": "upload"})
        self.assertEqual(rc, 0)
        self.assertEqual(len([e for e in evs if e["type"] == "complete"]), 1)
        paths = yp.pipeline_paths(video)
        with open(paths["thumbnail_jpg"], "rb") as a, open(thumbs["candidates"][2]["path"], "rb") as b:
            self.assertEqual(a.read(), b.read())
        with open(paths["titles_file"], encoding="utf-8") as f:
            self.assertIn("My Own Title", f.read())

    def test_auto_mode_runs_straight_through(self):
        video = self.video_copy("auto.mp4")
        rc, evs = self.run_with_patches(yp.run_pipeline, self.config(video, []))
        self.assertEqual(rc, 0)
        self.assertFalse(any(e["type"] in ("review_ready", "broll_plan_ready") for e in evs))
        self.assertEqual(len([e for e in evs if e["type"] == "complete"]), 1)
        paths = yp.pipeline_paths(video)
        self.assertLess(yp.get_video_duration(paths["edited_video"]), 18.0)  # the silence was cut
        thumbs = yp.read_json_file(paths["thumbs_file"])
        self.assertEqual([c["id"] for c in thumbs["candidates"]], ["ai_1"])
        self.assertTrue(os.path.exists(paths["thumbnail_jpg"]))

    def run_to_end(self, name, gates, **extra):
        video = self.video_copy(name)
        rc, evs = self.run_with_patches(yp.run_pipeline, dict(self.config(video, gates), **extra))
        self.assertEqual(rc, 0)
        return video, evs

    def test_render_ready_reports_the_joins(self):
        _, evs = self.run_to_end("joins.mp4", [])
        ready = [e for e in evs if e["type"] == "render_ready"]
        self.assertEqual(len(ready), 1)
        self.assertTrue(os.path.exists(ready[0]["video"]))
        self.assertTrue(all(0 < j < ready[0]["duration"] for j in ready[0]["joins"]))

    def test_publish_gate_can_finish_without_uploading(self):
        video, evs = self.run_to_end("finish.mp4", ["publish"])
        ready = [e for e in evs if e["type"] == "review_ready"]
        self.assertEqual([e["gate"] for e in ready], ["publish"])
        rc, evs = self.run_with_patches(yp.resume_pipeline, ready[0]["state_path"], {}, {"action": "finish"})
        done = [e for e in evs if e["type"] == "complete"]
        self.assertEqual((rc, len(done), done[0]["url"]), (0, 1, ""))
        state = yp.read_json_file(ready[0]["state_path"])
        self.assertNotIn("pending_gate", state["ctx"])
        self.assertEqual(state["ctx"]["next_stage"], "done")

    def test_publish_gate_uploads_a_different_file(self):
        video, evs = self.run_to_end("other.mp4", ["publish"])
        state_path = [e for e in evs if e["type"] == "review_ready"][0]["state_path"]
        mine = self.video_copy("premiere_render.mp4")
        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {}, {"action": "upload_file", "file": mine})
        self.assertEqual([e["edited_video"] for e in evs if e["type"] == "complete"], [mine])
        self.assertTrue(any("chapter times" in e.get("message", "") for e in evs if e["type"] == "log"))

    def test_auto_mode_can_export_for_premiere_instead(self):
        import xml.etree.ElementTree as ET

        video, evs = self.run_to_end("prem.mp4", [], auto_finish="premiere")
        exported = [e for e in evs if e["type"] == "premiere_exported"]
        self.assertEqual(len(exported), 1)
        with open(exported[0]["xml_path"], encoding="utf-8") as f:
            root = ET.fromstring(f.read().split("\n", 2)[2])
        self.assertGreaterEqual(len(root.findall("sequence/media/video/track/clipitem")), 2)  # the silence was cut
        self.assertEqual([e["url"] for e in evs if e["type"] == "complete"], [""])
        folder = yp.pipeline_paths(video)["premiere_dir"]
        self.assertTrue(os.path.exists(os.path.join(folder, "youtube_details.txt")))
        self.assertTrue(os.path.exists(os.path.join(folder, "thumbnail.jpg")))

    def test_export_premiere_from_a_finished_run(self):
        video, _ = self.run_to_end("prem2.mp4", [])
        rc, evs = run_events(yp.export_premiere_from_state, yp.pipeline_paths(video)["state_file"])
        self.assertEqual(rc, 0)
        self.assertEqual(len([e for e in evs if e["type"] == "premiere_exported"]), 1)

    def test_start_over_from_later_steps(self):
        video, _ = self.run_to_end("redo.mp4", [])
        state_path = yp.pipeline_paths(video)["state_file"]

        rc, evs = self.run_with_patches(yp.redo_pipeline, {"state_path": state_path, "from": "thumbnails",
                                                            "review_gates": ["thumbnail"], "secrets": {}})
        self.assertEqual([e["gate"] for e in evs if e["type"] == "review_ready"], ["thumbnail"])
        self.assertFalse(any(e["type"] in ("metadata_ready", "render_ready") for e in evs))

        rc, evs = self.run_with_patches(yp.redo_pipeline, {"state_path": state_path, "from": "metadata",
                                                            "review_gates": [], "secrets": {}})
        types = [e["type"] for e in evs]
        self.assertIn("metadata_ready", types)
        self.assertNotIn("render_ready", types)
        self.assertEqual(types.count("complete"), 1)

        rc, evs = self.run_with_patches(yp.redo_pipeline, {"state_path": state_path, "from": "cuts",
                                                            "review_gates": ["cuts"], "secrets": {}})
        self.assertEqual([e["gate"] for e in evs if e["type"] == "review_ready"], ["cuts"])
        self.assertFalse(any(e["type"] == "log" and "Whisper" in e.get("message", "") for e in evs))

    def test_failed_delete_does_not_start_over(self):
        video, _ = self.run_to_end("redo_del.mp4", [])
        state_path = yp.pipeline_paths(video)["state_file"]
        with mock.patch.object(yp, "delete_youtube_video", side_effect=PermissionError("reconnect")):
            rc, evs = self.run_with_patches(yp.redo_pipeline, {"state_path": state_path, "from": "metadata",
                                                                "delete_video_id": "abc", "secrets": {}})
        self.assertEqual(rc, 1)
        self.assertTrue(any(e["type"] == "error" and "nothing was started over" in e["message"] for e in evs))
        self.assertFalse(any(e["type"] in ("metadata_ready", "complete") for e in evs))

    def test_thumbnail_tweaks_stay_at_the_thumbnail_step(self):
        video, evs = self.run_to_end("tweak.mp4", ["thumbnail"])
        ready = [e for e in evs if e["type"] == "review_ready"][0]
        before = [c["id"] for c in yp.read_json_file(ready["data_path"])["candidates"]]

        rc, evs = self.run_with_patches(yp.regenerate_thumbnails, ready["state_path"], {}, "recompose", {"hook": ""})
        self.assertEqual([e["gate"] for e in evs if e["type"] == "review_ready"], ["thumbnail"])
        thumbs = yp.read_json_file(ready["data_path"])
        self.assertEqual([c["id"] for c in thumbs["candidates"]], before)
        self.assertEqual(thumbs["options"]["hook"], "")

        rc, evs = self.run_with_patches(yp.regenerate_thumbnails, ready["state_path"], {}, "generate", {"avoid": "hands"})
        thumbs = yp.read_json_file(ready["data_path"])
        self.assertEqual([c["id"] for c in thumbs["candidates"] if c["kind"] == "frame"], [i for i in before if i.startswith("frame")])
        self.assertTrue(any(c["kind"] == "ai" for c in thumbs["candidates"]))
        state = yp.read_json_file(ready["state_path"])
        self.assertEqual(state["ctx"]["pending_gate"], "thumbnail")
        self.assertEqual(state["ctx"]["thumb_options"], {"hook": "", "avoid": "hands"})

    def test_version_1_state_still_resumes(self):
        video = self.video_copy("v1.mp4")
        duration = yp.get_video_duration(video)
        state_path = yp.pipeline_paths(video)["state_file"]
        yp.write_json_file(state_path, {
            "version": 1,
            "config": {"input_video": video, "priority_list": [], "skip_upload": True},
            "ctx": {"input_video": video, "total_duration": duration, "segments": [],
                    "keep_intervals": [[0.0, duration]], "broll_plan_path": ""},
        })
        rc, evs = self.run_with_patches(yp.resume_pipeline, state_path, {})
        self.assertEqual(rc, 0)
        self.assertEqual(len([e for e in evs if e["type"] == "complete"]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
