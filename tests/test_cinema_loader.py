import hashlib
import json
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import load_stackchan_cinema as loader
from cinema_layout import media_path

DIGEST = "a" * 64


class Port:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def readline(self):
        return (next(self.lines) + "\n").encode()


class LoaderTests(unittest.TestCase):
    def test_canonical_flat_and_grouped_paths(self):
        for index in range(30000):
            self.assertEqual(media_path(index, 2), f"CINEMA/{index:04d}.SCF")
            self.assertEqual(
                media_path(index), f"CINEMA3/{index // 256:02X}/{index:04d}.SCF"
            )
        for invalid in (-1, 30000, "../0000", 0.5, True):
            with self.assertRaises(ValueError):
                media_path(invalid)
        with self.assertRaises(ValueError):
            media_path(0, 999)

    def pack(self, root, data, version=2):
        movie = root / media_path(0, version)
        movie.parent.mkdir(parents=True)
        movie.write_bytes(data)
        (root / "catalog.json").write_text(
            json.dumps(
                {
                    "version": version,
                    "pack_digest": DIGEST,
                    "clips": [
                        {
                            "index": 0,
                            "file": media_path(0, version),
                            "bytes": len(data),
                            "sha256": hashlib.sha256(data).hexdigest(),
                        }
                    ],
                }
            )
        )

    def card(self):
        return "CINEMA CARD total=16000000 free=15999999 digest=" + DIGEST + " count=1"

    @patch.object(loader.time, "sleep")
    def test_verified_sharded_transfer(self, _):
        data = b"x" * 32
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.pack(root, data, version=3)
            port = Port(
                [
                    self.card(),
                    "CINEMA READY 0 4096",
                    "CINEMA ACK 32",
                    f"CINEMA DONE 0 {zlib.crc32(data):08x}",
                    "CINEMA COMMITTED " + DIGEST,
                ]
            )
            loader.upload(port, root)
            self.assertIn(data, port.writes)
            self.assertEqual(port.writes[-1], b"cinema sd commit\n")

    @patch.object(loader.time, "sleep")
    def test_verified_transfer_and_resume(self, _):
        data = b"x" * 5000
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.pack(root, data)
            port = Port(
                [
                    self.card(),
                    "CINEMA READY 0 4096",
                    "CINEMA ACK 4096",
                    "CINEMA ACK 5000",
                    f"CINEMA DONE 0 {zlib.crc32(data):08x}",
                    "CINEMA COMMITTED " + DIGEST,
                ]
            )
            loader.upload(port, root)
            self.assertEqual(
                port.writes[-3:], [data[:4096], data[4096:], b"cinema sd commit\n"]
            )
            resumed = Port(
                [
                    "CINEMA ERROR stop-live-and-cinema-first",
                    self.card(),
                    "CINEMA SKIP 0",
                    "CINEMA COMMITTED " + DIGEST,
                ]
            )
            loader.upload(resumed, root)
            self.assertNotIn(data[:4096], resumed.writes)
            self.assertEqual(resumed.writes[-1], b"cinema sd commit\n")

    @patch.object(loader.time, "sleep")
    def test_movie_symlink_cannot_read_outside_pack(self, _):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "pack"
            root.mkdir()
            data = b"x" * 32
            self.pack(root, data)
            outside = Path(folder) / "outside"
            outside.write_bytes(data)
            movie = root / "CINEMA/0000.SCF"
            movie.unlink()
            movie.symlink_to(outside)
            port = Port([self.card()])
            with self.assertRaises(ValueError):
                loader.upload(port, root)
            self.assertFalse(any(b"cinema sd put" in p for p in port.writes))

    @patch.object(loader.time, "sleep")
    def test_completed_device_failure_can_retry_but_is_bounded(self, _):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = b"x" * 32
            self.pack(root, data)
            failure = [
                "CINEMA READY 0 4096",
                "CINEMA ERROR write-or-checksum 0 00000000",
            ]
            port = Port(
                [self.card()]
                + failure
                + [
                    "CINEMA READY 0 4096",
                    "CINEMA ACK 32",
                    f"CINEMA DONE 0 {zlib.crc32(data):08x}",
                    "CINEMA COMMITTED " + DIGEST,
                ]
            )
            loader.upload(port, root)
            self.assertEqual(
                sum(p.startswith(b"cinema sd put") for p in port.writes), 2
            )
            port = Port([self.card()] + failure * 3)
            with self.assertRaises(RuntimeError):
                loader.upload(port, root)
            self.assertEqual(
                sum(p.startswith(b"cinema sd put") for p in port.writes), 3
            )
            self.assertNotIn(b"cinema sd commit\n", port.writes)

    @patch.object(loader.time, "sleep")
    def test_wrong_firmware_never_writes_card(self, _):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.pack(root, b"x" * 32)
            port = Port([self.card().replace(DIGEST, "b" * 64)])
            with self.assertRaises(ValueError):
                loader.upload(port, root)
            self.assertFalse(any(b"cinema sd put" in p for p in port.writes))

    @patch.object(loader.time, "sleep")
    def test_corruption_and_bad_ack_never_commit(self, _):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = b"x" * 32
            self.pack(root, data)
            for messages in [
                ["CINEMA READY 1 4096"],
                ["CINEMA READY 0 4096", "CINEMA ACK 33"],
                ["CINEMA READY 0 4096", "CINEMA ACK 32", "CINEMA DONE 0 00000000"],
                ["CINEMA ERROR card-not-mounted"],
            ]:
                port = Port([self.card()] + messages)
                with self.assertRaises(RuntimeError):
                    loader.upload(port, root)
                self.assertNotIn(b"cinema sd commit\n", port.writes)
            (root / "CINEMA/0000.SCF").write_bytes(b"y" * 32)
            port = Port([self.card()])
            with self.assertRaises(ValueError):
                loader.upload(port, root)
            self.assertFalse(any(b"cinema sd put" in p for p in port.writes))


if __name__ == "__main__":
    unittest.main()
