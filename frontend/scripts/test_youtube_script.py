"""
Checks for the script writer in youtube_pipeline.py (Claude writes the spoken script before recording).

    python frontend/scripts/test_youtube_script.py

Everything runs offline: urllib is replaced by a fake that replays recorded Messages API replies.
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import youtube_pipeline as yp  # noqa: E402

KEY = "sk-ant-test-key-do-not-log"


def sse(*events):
    """A Messages API stream as the raw lines urlopen yields."""
    lines = []
    for event in events:
        lines.append(f"event: {event['type']}\n".encode())
        lines.append(f"data: {json.dumps(event)}\n".encode())
        lines.append(b"\n")
    return lines


def text_stream(*pieces, stop="end_turn", usage_in=1000, usage_out=3000, fallback_at=None):
    events = [{"type": "message_start", "message": {"model": "claude-opus-5-5", "usage": {"input_tokens": usage_in}}},
              {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}},
              {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": "plan"}},
              {"type": "content_block_stop", "index": 0},
              {"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}]
    for i, piece in enumerate(pieces):
        if fallback_at == i:
            events += [{"type": "content_block_stop", "index": 1},
                       {"type": "content_block_start", "index": 2,
                        "content_block": {"type": "fallback", "from": {"model": "claude-opus-5-5"},
                                          "to": {"model": "claude-opus-5"}}},
                       {"type": "content_block_stop", "index": 2},
                       {"type": "content_block_start", "index": 3, "content_block": {"type": "text", "text": ""}}]
        events.append({"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": piece}})
    events += [{"type": "content_block_stop", "index": 1},
               {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": usage_out}},
               {"type": "message_stop"}]
    return sse(*events)


class FakeResponse:
    def __init__(self, lines=None, body=None):
        self.lines = lines or []
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def __iter__(self):
        return iter(self.lines)

    def read(self):
        return json.dumps(self.body).encode()


class FakeUrlopen:
    """Records each request and answers with the next canned response."""

    def __init__(self, *responses):
        self.responses = list(responses)
        self.requests = []

    def __call__(self, req, timeout=None):
        self.requests.append({"url": req.full_url, "headers": dict(req.header_items()),
                              "body": json.loads(req.data.decode())})
        return self.responses.pop(0)


def run_quiet(fn, *args, **kwargs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = fn(*args, **kwargs)
    events = [json.loads(line) for line in out.getvalue().splitlines() if line.strip()]
    return result, events, out.getvalue()


SCRIPT = ("# Why Your Webcam Looks Bad\n\n## Hook\n\nYour webcam isn't the problem. Your light is.\n\n"
          "[show: a dim face next to a lit one]\n\n## Fix\n\nPut a lamp behind the screen.\n")


class StreamTests(unittest.TestCase):
    def test_text_is_joined_and_thinking_is_skipped(self):
        fake = FakeUrlopen(FakeResponse(text_stream("Hello ", "there.")))
        seen = []
        with mock.patch.object(yp.urllib.request, "urlopen", fake):
            (text, usage), _, _ = run_quiet(yp.claude_stream_text, KEY, "claude-opus-5-5", "high", "sys",
                                            [{"role": "user", "content": "hi"}], seen.append)
        self.assertEqual(text, "Hello there.")
        self.assertEqual(seen, ["Hello ", "there."])
        self.assertEqual(usage["input_tokens"], 1000)
        self.assertEqual(usage["output_tokens"], 3000)

    def test_text_after_a_mid_stream_fallback_carries_on(self):
        fake = FakeUrlopen(FakeResponse(text_stream("Part one. ", "Part two.", fallback_at=1)))
        with mock.patch.object(yp.urllib.request, "urlopen", fake):
            (text, _), events, _ = run_quiet(yp.claude_stream_text, KEY, "claude-opus-5-5", "high", "sys",
                                             [{"role": "user", "content": "hi"}], lambda _: None)
        self.assertEqual(text, "Part one. Part two.")
        self.assertTrue(any(e["type"] == "log" and "claude-opus-5" in e["message"] for e in events))

    def test_a_refusal_raises(self):
        fake = FakeUrlopen(FakeResponse(text_stream("", stop="refusal")))
        with mock.patch.object(yp.urllib.request, "urlopen", fake), self.assertRaises(RuntimeError):
            run_quiet(yp.claude_stream_text, KEY, "claude-opus-5-5", "high", "sys",
                      [{"role": "user", "content": "hi"}], lambda _: None)

    def test_request_body_and_headers(self):
        fake = FakeUrlopen(FakeResponse(text_stream("x")))
        with mock.patch.object(yp.urllib.request, "urlopen", fake):
            run_quiet(yp.claude_stream_text, KEY, "claude-opus-5-5", "high", "sys",
                      [{"role": "user", "content": "hi"}], lambda _: None)
        req = fake.requests[0]
        body = req["body"]
        self.assertEqual(req["url"], "https://api.anthropic.com/v1/messages")
        self.assertEqual(body["model"], "claude-opus-5-5")
        self.assertEqual(body["output_config"], {"effort": "high"})
        self.assertEqual(body["fallbacks"], "default")
        self.assertTrue(body["stream"])
        for banned in ("temperature", "top_p", "top_k", "thinking"):
            self.assertNotIn(banned, body)
        headers = {k.lower(): v for k, v in req["headers"].items()}
        self.assertEqual(headers["x-api-key"], KEY)
        self.assertEqual(headers["anthropic-beta"], "server-side-fallback-2026-07-01")

    def test_unknown_model_and_effort_fall_back_to_the_defaults(self):
        body = yp.script_request_body("gpt-4o", "turbo", "sys", [], 100)
        self.assertEqual(body["model"], "claude-opus-5-5")
        self.assertEqual(body["output_config"]["effort"], "high")

    def test_cost_from_usage(self):
        self.assertAlmostEqual(yp.claude_cost("claude-opus-5-5", {"input_tokens": 1_000_000, "output_tokens": 0}), 4.0)
        self.assertAlmostEqual(yp.claude_cost("claude-opus-5-5", {"input_tokens": 0, "output_tokens": 1_000_000}), 20.0)
        self.assertAlmostEqual(yp.claude_cost("claude-sonnet-5-5", {"input_tokens": 1000, "output_tokens": 2000}),
                               (1000 * 2 + 2000 * 10) / 1e6)


class WriteScriptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, fake, **options):
        options.setdefault("out_dir", self.tmp.name)
        with mock.patch.object(yp.urllib.request, "urlopen", fake):
            return run_quiet(yp.write_script, {"claude_key": KEY}, options)

    def test_writes_saves_and_reports_cost(self):
        pieces = [SCRIPT[i:i + 20] for i in range(0, len(SCRIPT), 20)]
        fake = FakeUrlopen(FakeResponse(text_stream(*pieces, usage_in=2000, usage_out=8000)))
        code, events, raw = self.write(fake, action="write", topic="webcam light", minutes=3, tone="tutorial")
        self.assertEqual(code, 0)
        ready = [e for e in events if e["type"] == "script_ready"][0]
        self.assertEqual(ready["title"], "Why Your Webcam Looks Bad")
        self.assertTrue(ready["path"].endswith("Why Your Webcam Looks Bad.md"))
        with open(ready["path"], encoding="utf-8") as f:
            self.assertEqual(f.read(), SCRIPT)
        self.assertEqual(ready["cost"], f"~${(2000 * 4 + 8000 * 20) / 1e6:.2f}")
        self.assertEqual(ready["words"], yp.spoken_words(SCRIPT))
        self.assertNotIn(KEY, raw)
        ask = fake.requests[0]["body"]["messages"][0]["content"]
        self.assertIn("about 450 spoken words", ask)
        self.assertIn("step-by-step tutorial", ask)

    def test_revision_sends_the_old_script_and_saves_a_new_version(self):
        first = os.path.join(self.tmp.name, "2026-11-01 Why Your Webcam Looks Bad.md")
        with open(first, "w", encoding="utf-8") as f:
            f.write(SCRIPT)
        fake = FakeUrlopen(FakeResponse(text_stream(SCRIPT.replace("lamp", "big lamp"))))
        code, events, _ = self.write(fake, action="revise", topic="webcam light", previous=SCRIPT,
                                     previous_path=first, feedback="Make the fix bigger.")
        self.assertEqual(code, 0)
        messages = fake.requests[0]["body"]["messages"]
        self.assertEqual([m["role"] for m in messages], ["user", "assistant", "user"])
        self.assertEqual(messages[1]["content"], SCRIPT)
        self.assertIn("Make the fix bigger.", messages[2]["content"])
        ready = [e for e in events if e["type"] == "script_ready"][0]
        self.assertTrue(ready["path"].endswith("Why Your Webcam Looks Bad v2.md"))

    def test_an_error_is_reported_without_the_key(self):
        def boom(req, timeout=None):
            raise yp.urllib.error.URLError("no network")
        with mock.patch.object(yp.urllib.request, "urlopen", boom):
            code, events, raw = run_quiet(yp.write_script, {"claude_key": KEY}, {"action": "write", "topic": "x"})
        self.assertEqual(code, 1)
        self.assertTrue(any(e["type"] == "script_error" for e in events))
        self.assertNotIn(KEY, raw)

    def test_no_key_says_where_to_add_it(self):
        code, events, _ = run_quiet(yp.write_script, {}, {"action": "write", "topic": "x"})
        self.assertEqual(code, 1)
        self.assertIn("Vault", [e for e in events if e["type"] == "script_error"][0]["message"])

    def test_ideas_use_structured_output_and_past_titles(self):
        for name, title in (("a_yt_titles.txt", "1. Old Video One (current)\n2. Alt\n"),
                            ("b_yt_titles.txt", "1. Old Video Two (current)\n"),
                            ("b_yt_metadata.json", "{}")):
            with open(os.path.join(self.tmp.name, name), "w", encoding="utf-8") as f:
                f.write(title)
        ideas = [{"title": f"Idea {i}", "angle": "a", "why": "w"} for i in range(5)]
        reply = {"model": "claude-opus-5-5", "stop_reason": "end_turn",
                 "content": [{"type": "thinking", "thinking": ""},
                             {"type": "text", "text": json.dumps({"ideas": ideas})}],
                 "usage": {"input_tokens": 500, "output_tokens": 1500}}
        fake = FakeUrlopen(FakeResponse(body=reply))
        code, events, _ = self.write(fake, action="ideas", notes="tech tips", title_dirs=[self.tmp.name])
        self.assertEqual(code, 0)
        got = [e for e in events if e["type"] == "script_ideas"][0]
        self.assertEqual([i["title"] for i in got["ideas"]], [f"Idea {i}" for i in range(5)])
        body = fake.requests[0]["body"]
        self.assertFalse(body["stream"])
        self.assertEqual(body["output_config"]["format"]["type"], "json_schema")
        ask = body["messages"][0]["content"]
        self.assertIn("Old Video One", ask)
        self.assertIn("Old Video Two", ask)
        self.assertNotIn("Alt", ask)


class HelperTests(unittest.TestCase):
    def test_spoken_words_leave_out_headings_and_cues(self):
        self.assertEqual(yp.spoken_words(SCRIPT), len("Your webcam isn't the problem. Your light is.".split())
                         + len("Put a lamp behind the screen.".split()))

    def test_length_target_is_minutes_times_wpm(self):
        ask = yp.script_messages({"topic": "t", "minutes": 8})[0]["content"]
        self.assertIn(f"about {8 * yp.SCRIPT_WPM} spoken words", ask)

    def test_save_names_are_safe_and_versioned(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = yp.save_script("x", 'What: "AI" / you?', tmp)
            b = yp.save_script("y", 'What: "AI" / you?', tmp)
            self.assertNotRegex(os.path.basename(a), r'[<>:"/\\|?*]')
            self.assertTrue(b.endswith(" v2.md"))


if __name__ == "__main__":
    unittest.main()
