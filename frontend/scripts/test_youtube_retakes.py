"""
Checks for retake detection, chapter titles and the LLM calls in youtube_pipeline.py.

    python frontend/scripts/test_youtube_retakes.py
"""

import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import youtube_pipeline as yp  # noqa: E402


def quiet():
    """The pipeline reports progress as JSON lines on stdout; keep test output readable."""
    return contextlib.redirect_stdout(io.StringIO())


def segment(start, end, text):
    tokens = text.split()
    step = (end - start) / len(tokens)
    return {
        "start": start, "end": end, "text": text,
        "words": [{"word": f" {w}", "start": start + i * step, "end": start + (i + 1) * step} for i, w in enumerate(tokens)],
    }


def cut_texts(segments):
    """Text of each segment that a retake cut removes completely."""
    cuts = yp.detect_butchered_retakes(segments)
    return [s["text"] for s in segments if any(a <= s["start"] and s["end"] <= b for a, b, _ in cuts)]


# Whisper's segments for the 2026-10-27 test recording (the source of https://youtu.be/ZHh4whRAdEE).
TEST_RECORDING = [
    (2.13, 7.11, "Today I'm going to show you how I turned OBS into a one-click YouTube editing machine."),
    (7.41, 10.29, "And by the end of this video, you'll have it running on your own machine."),
    (12.11, 15.77, "Here's the problem. Recording is easy, but editing takes forever."),
    (16.05, 22.73, "You cut the dead air, you cut the mistakes, you write a title, you write a description, and you make chapters."),
    (23.01, 25.49, "That's an hour enough work for a 10-minute video."),
    (26.77, 29.39, "So I built a pipeline that does all that for me."),
    (29.77, 30.99, "Let me show you the doc."),
    (31.68, 36.74, "Over here on the right side of the screen is the YouTube Workflow Doc."),
    (38.56, 42.76, "If you look at this panel, you can see the Console Log Status and the Submit to YouTube button."),
    (48.69, 50.39, "I'm clicking into Settings now."),
    (50.71, 57.60, "The setting right here controls the Silence Threshold, and this option turns Screen B-Roll on or off."),
    (57.78, 62.70, "The pipeline has three stages."),
    (63.04, 64.86, "Stage 1 cuts the silence."),
    (65.65, 67.99, "Stage 2 removes the retakes."),
    (67.99, 71.45, "Stage 3 writes the title, the description, and the chapters."),
    (73.97, 75.37, "Transcription runs on."),
    (76.66, 79.78, "Transcription runs on Whisper Large V3 Turbo with CUDA."),
    (80.16, 82.68, "So a 10-minute video finishes at under a minute."),
    (85.10, 87.44, "And it uses about 40 gigabytes of VRAM."),
    (87.56, 88.28, "Wait, scratch that."),
    (88.44, 91.18, "It uses about 4 gigabytes of VRAM on my card."),
    (92.53, 93.93, "Now let's talk about B-Roll."),
    (94.17, 97.37, "While I'm talking, OBS records my screen at the same time."),
    (97.55, 99.71, "Take a look at this file in the Recordings folder."),
    (115.86, 123.96, "When I say something like Click This Button, the editor cuts to the screen recording at that exact moment and puts my webcam in a little bubble in the corner."),
    (125.13, 126.47, "Sorry, let me try that again."),
    (126.79, 128.93, "The B-Roll only shows up where it actually helps."),
    (128.93, 130.77, "It never covers the opening hook."),
    (130.95, 133.67, "And each clip runs between 2 and 12 seconds."),
    (135.34, 138.70, "Here's a quick test for the metadata."),
    (139.89, 142.79, "This video was filmed at a cafe in Sao Paulo."),
    (143.13, 146.13, "It costs $0 and it works on Windows 11."),
    (146.13, 147.45, "No subscription needed."),
    (148.25, 154.18, "If I scroll down in the console log, you can see every cut it made and why it made it."),
    (154.73, 156.03, "So that's the whole workflow."),
    (156.25, 159.85, "Record, click one button, review the B-Roll, and submit."),
    (159.85, 163.57, "If this saved you an hour, subscribe, and I'll see you in the next one."),
]


class RetakeTests(unittest.TestCase):
    def test_test_recording_end_to_end(self):
        segments = [segment(*s) for s in TEST_RECORDING]
        self.assertEqual(cut_texts(segments), [
            "Transcription runs on.",
            "And it uses about 40 gigabytes of VRAM.",
            "Wait, scratch that.",
            "Sorry, let me try that again.",
        ])

    def test_cue_only_segment_also_cuts_the_take_it_disowns(self):
        segments = [
            segment(0.0, 2.3, "And it uses about 40 gigabytes of VRAM."),
            segment(2.5, 3.2, "Wait, scratch that."),
            segment(3.4, 6.1, "It uses about 4 gigabytes of VRAM on my card."),
        ]
        self.assertEqual(cut_texts(segments), [segments[0]["text"], segments[1]["text"]])

    def test_cue_keeps_an_unrelated_previous_line(self):
        segments = [
            segment(0.0, 8.0, "When I say click this button, the editor cuts to the screen recording."),
            segment(9.0, 10.3, "Sorry, let me try that again."),
            segment(10.6, 12.8, "The B-Roll only shows up where it actually helps."),
        ]
        self.assertEqual(cut_texts(segments), [segments[1]["text"]])

    def test_cue_with_retake_in_the_same_segment_keeps_the_retake(self):
        seg = segment(0.0, 6.0, "It uses 40 gigabytes, wait, scratch that, it uses 4 gigabytes of VRAM.")
        cuts = yp.detect_butchered_retakes([seg])
        self.assertEqual(len(cuts), 1)
        start, end, _ = cuts[0]
        retake_start = next(w["start"] for w in seg["words"] if w["word"] == " it")
        self.assertEqual(start, 0.0)
        self.assertAlmostEqual(end, retake_start)

    def test_short_complete_restart_is_cut(self):
        segments = [
            segment(0.0, 1.4, "Transcription runs on."),
            segment(2.7, 5.8, "Transcription runs on Whisper Large V3 Turbo with CUDA."),
        ]
        self.assertEqual(cut_texts(segments), [segments[0]["text"]])

    def test_short_restart_with_a_wrong_last_word_is_cut(self):
        segments = [
            segment(0.0, 1.0, "Here's a quick desk."),
            segment(1.2, 3.0, "Here's a quick test for the metadata."),
        ]
        self.assertEqual(cut_texts(segments), [segments[0]["text"]])

    def test_parallel_sentences_are_kept(self):
        segments = [
            segment(0.0, 1.8, "Stage 1 cuts the silence."),
            segment(2.6, 4.9, "Stage 2 removes the retakes."),
            segment(4.9, 8.4, "Stage 3 writes the title, the description, and the chapters."),
        ]
        self.assertEqual(yp.detect_butchered_retakes(segments), [])

    def test_restart_inside_one_segment_cuts_only_the_false_start(self):
        seg = segment(0.0, 9.0, "Transcription runs on transcription runs on Whisper Large V3 Turbo with CUDA.")
        cuts = yp.detect_butchered_retakes([seg])
        self.assertEqual(len(cuts), 1)
        start, end, _ = cuts[0]
        self.assertEqual(start, seg["words"][0]["start"])
        self.assertEqual(end, seg["words"][3]["start"])

    def test_rhetorical_parallel_inside_one_segment_is_kept(self):
        seg = segment(0.0, 4.0, "The more you know, the more you grow.")
        self.assertEqual(yp.detect_butchered_retakes([seg]), [])

    def test_retakes_too_far_apart_are_kept(self):
        segments = [
            segment(0.0, 1.4, "Transcription runs on."),
            segment(20.0, 23.0, "Transcription runs on Whisper Large V3 Turbo with CUDA."),
        ]
        self.assertEqual(yp.detect_butchered_retakes(segments), [])


class ChapterTitleTests(unittest.TestCase):
    def test_long_title_stops_at_a_clause_break(self):
        self.assertEqual(
            yp.tidy_chapter_title("You cut the dead air, you cut the mistakes, you write a title, you write a description"),
            "You cut the dead air, you cut the mistakes",
        )
        self.assertEqual(
            yp.tidy_chapter_title("If you look at this panel, you can see the Console Log Status and the button"),
            "If you look at this panel",
        )

    def test_long_title_stops_at_the_first_sentence(self):
        self.assertEqual(yp.tidy_chapter_title("Here's the problem. Recording is easy, but editing takes forever."), "Here's the problem")

    def test_short_title_is_unchanged(self):
        self.assertEqual(yp.tidy_chapter_title("Setting up the local AI pipeline."), "Setting up the local AI pipeline")


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class ClaudeCallTests(unittest.TestCase):
    def call(self, reply):
        sent = {}

        def fake_urlopen(req, timeout=None):
            sent["body"] = json.loads(req.data.decode("utf-8"))
            sent["headers"] = {k.lower(): v for k, v in req.header_items()}
            return FakeResponse(json.dumps(reply).encode("utf-8"))

        with mock.patch.object(yp.urllib.request, "urlopen", fake_urlopen):
            result = yp.call_claude_json("test-key", "system", "transcript")
        return result, sent

    def test_uses_current_model_and_reads_json_after_thinking(self):
        result, sent = self.call({
            "stop_reason": "end_turn",
            "content": [
                {"type": "thinking", "thinking": "", "signature": "x"},
                {"type": "text", "text": "{\"title\": \"OBS one-click YouTube editing\"}"},
            ],
        })
        self.assertEqual(result["title"], "OBS one-click YouTube editing")
        self.assertEqual(sent["body"]["model"], "claude-sonnet-5-5")
        self.assertNotIn("temperature", sent["body"])
        self.assertGreaterEqual(sent["body"]["max_tokens"], 16000)
        self.assertEqual(sent["body"]["fallbacks"], "default")
        self.assertEqual(sent["headers"]["anthropic-beta"], "server-side-fallback-2026-07-01")

    def test_refusal_raises(self):
        with self.assertRaises(RuntimeError):
            self.call({"stop_reason": "refusal", "stop_details": {"category": "cyber"}, "content": []})

    def test_label_names_the_model(self):
        self.assertIn("claude-sonnet-5-5", yp.llm_provider_labels("gemma4:12b")["claude"])


class FallbackTests(unittest.TestCase):
    def test_missing_keys_and_no_ollama_fall_back_to_the_template(self):
        segments = [segment(*s) for s in TEST_RECORDING[:6]]
        with quiet() as out, mock.patch.object(yp, "ensure_ollama_server", return_value=False):
            meta, badge = yp.generate_metadata_with_priority(["gemini", "openai", "claude", "local"], {}, "gemma4:12b", segments)
        self.assertEqual(badge, yp.TEMPLATE_METADATA_BADGE)
        self.assertTrue(meta["title"])
        log = out.getvalue()
        self.assertIn("No Claude API key configured", log)
        self.assertIn("could not be started", log)
        self.assertIn("fill-in template", log)


if __name__ == "__main__":
    unittest.main()
