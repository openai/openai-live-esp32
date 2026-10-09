"""Frame/status semantics without requiring USB hardware or pyserial."""

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from screen_mirror import Mirror, handler


class FrameTest(unittest.TestCase):
    def setUp(self):
        self.mirror = Mirror(SimpleNamespace())
        self.mirror.connected = True
        d = self.mirror.decoder
        d.width, d.height = 2, 1
        d.pixels = bytearray(b"\x00\xf8\xe0\x07")
        d.rows = {0}
        d.revision = 5
        self.handler = object.__new__(handler(self.mirror))
        self.handler.server = SimpleNamespace(server_port=8765)
        self.handler.reply = self.reply

    def reply(self, code, body=b"", content_type=None, headers=None):
        self.result = (code, body, headers)

    def get(self, tag="", host="127.0.0.1:8765"):
        self.handler.path = "/frame"
        self.handler.headers = {"Host": host, "If-None-Match": tag}
        self.handler.do_GET()
        return self.result

    def test_conditional_frames_and_reconnect(self):
        code, pixels, headers = self.get()
        self.assertEqual(code, 200)
        self.assertEqual(pixels, self.mirror.decoder.pixels)
        self.assertEqual(headers["X-Ready"], 1)
        tag = headers["ETag"]
        self.assertEqual(self.get(tag)[:2], (204, b""))
        self.mirror.decoder.revision += 1
        self.assertEqual(self.get(tag)[0], 200)
        self.mirror.decoder.revision -= 1
        self.mirror.generation += 1
        # Same revision after reconnect must not suppress a different frame.
        self.assertEqual(self.get(tag)[0], 200)

    def test_status_survives_empty_responses(self):
        self.mirror.enabled = False
        code, pixels, headers = self.get()
        self.assertEqual((code, pixels), (204, b""))
        self.assertEqual(headers["X-Enabled"], 0)
        self.mirror.enabled = True
        self.mirror.connected = False
        code, pixels, headers = self.get()
        self.assertEqual((code, pixels), (204, b""))
        self.assertEqual(headers["X-Connected"], 0)
        self.mirror.connected = True
        self.mirror.decoder.rows.clear()
        self.assertEqual(self.get()[0], 204)

    def test_loopback_host_and_origin_guards(self):
        self.assertEqual(self.get(host="example.com")[0], 403)
        self.handler.path = "/mirror/off"
        self.handler.headers = {
            "Host": "127.0.0.1:8765",
            "Origin": "https://example.com",
        }
        self.handler.do_POST()
        self.assertEqual(self.result[0], 403)
        self.assertTrue(self.mirror.enabled)
        self.handler.headers["Origin"] = "http://127.0.0.1:8765"
        self.handler.do_POST()
        self.assertEqual(self.result[0], 200)
        self.assertFalse(self.mirror.enabled)


if __name__ == "__main__":
    unittest.main()
