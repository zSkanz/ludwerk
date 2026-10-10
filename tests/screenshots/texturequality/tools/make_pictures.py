"""Writes the three pictures of the texture quality test: 256 by 256, each a
flat colour with a darker frame -- nothing to look at, and small on disk.

- `wall.png` is a material's and nothing else's.
- `sprite.png` is a sprite's and nothing else's.
- `both.png` is a material's AND a sprite's.

Run it from this folder (`python tools/make_pictures.py`) after changing it;
the files it writes are checked in.
"""

import struct
import zlib
from pathlib import Path

SIDE = 256


def png(colour):
    rows = bytearray()
    for y in range(SIDE):
        rows.append(0)
        for x in range(SIDE):
            edge = x < 8 or y < 8 or x >= SIDE - 8 or y >= SIDE - 8
            rows.extend(bytes(c // 3 if edge else c for c in colour) + b"\xff")

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", SIDE, SIDE, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )


out = Path(__file__).resolve().parent.parent / "content" / "textures"
out.mkdir(parents=True, exist_ok=True)
for name, colour in (("wall", (200, 120, 80)), ("sprite", (80, 160, 220)), ("both", (120, 200, 120))):
    (out / f"{name}.png").write_bytes(png(colour))
    print("wrote", out / f"{name}.png")
