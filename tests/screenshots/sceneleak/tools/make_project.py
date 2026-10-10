"""Writes the content of the scene-change test (D610): a lobby and two maps,
each map with two materials of its own (a picture each) and a model of its
own, so that leaving a map has something to let go.

Run it from this folder (`python tools/make_project.py`) after changing it;
the files it writes are checked in.
"""

import base64
import json
import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIDE = 128
MAPS = ("a", "b")


def png(colour):
    row = b"\x00" + bytes(colour + (255,)) * SIDE

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", SIDE, SIDE, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(row * SIDE, 9))
        + chunk(b"IEND", b"")
    )


def ball(radius, rings=8, segments=12):
    positions, normals, indices = [], [], []
    for ring in range(rings + 1):
        polar = math.pi * ring / rings
        for segment in range(segments + 1):
            around = 2 * math.pi * segment / segments
            n = (math.sin(polar) * math.sin(around), math.cos(polar), math.sin(polar) * math.cos(around))
            normals.append(n)
            positions.append(tuple(c * radius for c in n))
    for ring in range(rings):
        for segment in range(segments):
            a = ring * (segments + 1) + segment
            b = a + segments + 1
            indices.extend([b, b + 1, a + 1, b, a + 1, a])
    blob = bytearray()
    views, accessors = [], []

    def add(data, component, kind, count, bounds=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        entry = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        if bounds:
            entry["min"], entry["max"] = bounds
        accessors.append(entry)
        return len(accessors) - 1

    def flat(rows):
        return b"".join(struct.pack("<3f", *row) for row in rows)

    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    return {
        "asset": {"version": "2.0", "generator": "tests/screenshots/sceneleak/tools/make_project.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": add(flat(positions), 5126, "VEC3", len(positions), (lo, hi)),
                            "NORMAL": add(flat(normals), 5126, "VEC3", len(normals)),
                        },
                        "indices": add(struct.pack("<%dH" % len(indices), *indices), 5123, "SCALAR", len(indices)),
                    }
                ]
            }
        ],
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
            }
        ],
        "bufferViews": views,
        "accessors": accessors,
    }


for folder in ("content/scenes", "content/textures", "content/materials", "content/models"):
    (ROOT / folder).mkdir(parents=True, exist_ok=True)

for name in ("lobby",) + MAPS:
    (ROOT / "content" / "scenes" / f"{name}.scene.json").write_text(
        json.dumps({"format": "scene", "version": 2, "root": {}}, indent=1) + "\n", encoding="utf-8"
    )
for which, name in enumerate(MAPS):
    for index in range(2):
        shade = 90 + index * 80
        colour = (shade, 70, 60) if which == 0 else (60, 70, shade)
        (ROOT / "content" / "textures" / f"{name}_{index}.png").write_bytes(png(colour))
        (ROOT / "content" / "materials" / f"{name}_{index}.material.json").write_text(
            json.dumps(
                {
                    "format": "material",
                    "version": 1,
                    "parent": "",
                    "properties": {"Color": [1, 1, 1], "ColorMap": f"asset://textures/{name}_{index}.png"},
                },
                indent=1,
            )
            + "\n",
            encoding="utf-8",
        )
    (ROOT / "content" / "models" / f"{name}.gltf").write_text(
        json.dumps(ball(1.0 + 0.2 * which), indent=1) + "\n", encoding="utf-8"
    )
print("wrote", ROOT / "content")
