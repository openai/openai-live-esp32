"""Generate a tiny, original black-frame pack for firmware CI (no API/media downloads)."""

import base64
import importlib.util
import sys
from pathlib import Path


def create(path):
    spec = importlib.util.spec_from_file_location(
        "cinema",
        Path(__file__).resolve().parents[1] / "tools/prepare_stackchan_cinema.py",
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    jpeg = base64.b64decode(
        "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYwLjMxLjEwMgD/2wBDAAgSEhUSFRgYGBgYGB0bHR4eHh0dHR0eHh4gICAmJiYgICAeHiAgJCQmJikqKScnJicqKi0tLTY2MzM/P0FNTV3/xABLAAEBAAAAAAAAAAAAAAAAAAAACAEBAAAAAAAAAAAAAAAAAAAAABABAAAAAAAAAAAAAAAAAAAAABEBAAAAAAAAAAAAAAAAAAAAAP/AABEIAPABQAMBIgACEQADEQD/2gAMAwEAAhEDEQA/AJ/"
        + "A" * 599
        + "B//9k="
    )
    film = module.make_film([jpeg] * 12)
    path.mkdir(parents=True, exist_ok=True)
    (path / "fallback.bin").write_bytes(film)
    (path / "CinemaAssets.hpp").write_text(
        """#pragma once
#include <cstdint>
namespace cinema::assets {
inline constexpr char MODEL[] = "text-embedding-3-small";
inline constexpr unsigned DIMENSIONS = 256, COUNT = 1;
inline constexpr char DIGEST[] = "0000000000000000000000000000000000000000000000000000000000000000";
struct Clip { uint32_t source, bytes, offset, length; float norm; uint16_t neighbors[8]; };
inline constexpr Clip CLIPS[] = {{1, %d, 0, %d, 127.f, {0,0,0,0,0,0,0,0}}};
inline constexpr int8_t VECTORS[][DIMENSIONS] = {{127}};
}
"""
        % (len(film), len(film))
    )


if __name__ == "__main__":
    create(Path(sys.argv[1]))
