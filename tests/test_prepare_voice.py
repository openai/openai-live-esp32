"""Credential schema checks using ESP-IDF's existing Python environment."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "prepare_voice", Path(__file__).resolve().parents[1] / "tools/prepare_voice.py"
)
assert SPEC and SPEC.loader
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class WifiProfilesTest(unittest.TestCase):
    def load(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wifi.json"
            path.write_text(json.dumps(value))
            return prepare.load_wifi(path)

    def test_legacy_and_multiple(self):
        one = {"ssid": "fixture", "password": "testpass"}
        two = {"ssid": "fixture-2", "password": ""}
        self.assertEqual(self.load(one), [one])
        self.assertEqual(self.load({"networks": [one, two]}), [one, two])
        self.assertEqual(prepare.load_wifi(None), [])

    def test_invalid_credentials(self):
        for value in [
            {"networks": []},
            {"networks": [None]},
            {"ssid": "", "password": "testpass"},
            {"ssid": "x" * 33, "password": "testpass"},
            {"ssid": "x\0y", "password": "testpass"},
            {"ssid": "x", "password": "tiny"},
            {"ssid": "x", "password": "y" * 64},
            {"ssid": "x", "password": "test\0pass"},
            [{"ssid": "same", "password": ""}] * 2,
            [{"ssid": str(i), "password": ""} for i in range(9)],
        ]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.load(value)

    def test_private_header(self):
        networks = self.load({"ssid": 'test"\\net', "password": "testpass"})
        with tempfile.TemporaryDirectory() as directory:
            private = Path(directory) / "private"
            prepare.write_secrets(private, "fixture-key", networks)
            path = private / "live_voice_secrets.h"
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(private.stat().st_mode & 0o777, 0o700)
            self.assertIn("LIVE_WIFI_PROFILE_COUNT = 1", path.read_text())
            self.assertIn(json.dumps(networks[0]["ssid"]), path.read_text())


if __name__ == "__main__":
    unittest.main()
