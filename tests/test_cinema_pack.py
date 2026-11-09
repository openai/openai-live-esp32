import importlib.util
import math
import struct
import unittest
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cinema", Path(__file__).resolve().parents[1] / "tools/prepare_stackchan_cinema.py"
)
cinema = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cinema)


class CinemaPackTests(unittest.TestCase):
    def test_container_offsets_and_crc(self):
        frames = [b"\xff\xd8abc\xff\xd9", b"\xff\xd8defgh\xff\xd9"]
        data = cinema.make_film(frames)
        header = struct.unpack_from("<8sHHHHIIII", data)
        self.assertEqual(header[:6], (b"SCFILM1\0", 320, 240, 12, 2, len(data)))
        self.assertEqual(struct.unpack_from("<IIII", data, 32), (48, 7, 55, 9))
        self.assertEqual(zlib.crc32(data[48:]), header[7])
        for invalid in (
            [],
            [b"not jpeg"],
            [b"\xff\xd8" + b"a" * 32768 + b"\xff\xd9"],
            frames * 73,
        ):
            with self.assertRaises(ValueError):
                cinema.make_film(invalid)
        with self.assertRaises(ValueError):
            cinema.make_film(frames, fps=60)

    def test_quantization_preserves_cosine(self):
        original = cinema.vector([math.sin(i * 0.17) for i in range(256)])
        q, norm = cinema.quantize(original)
        self.assertEqual(max(map(abs, q)), 127)
        self.assertGreater(sum(a * b for a, b in zip(original, q)) / norm, 0.9999)
        for bad in ([0] * 256, [1] * 255, [float("nan")] * 256, [float("inf")] * 256):
            with self.assertRaises(ValueError):
                cinema.vector(bad)

    def test_rsc_text_cannot_inject_records(self):
        text = b'fake\na:{"injected":true}\n'
        data = (
            b'0:{"ok":1}\n1:T'
            + f"{len(text):x}".encode()
            + b","
            + text
            + b'2:["real"]\n'
        )
        self.assertEqual(cinema.read_rsc(data), [{"ok": 1}, ["real"]])
        with self.assertRaises(ValueError):
            cinema.read_rsc(b"1:T100,short")


if __name__ == "__main__":
    unittest.main()
