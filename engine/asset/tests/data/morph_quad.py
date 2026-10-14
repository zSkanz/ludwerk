"""Writes `morph_quad.gltf` beside itself: the smallest file with morph targets
that still says everything the importer has to get right (ADR 0196).

A unit quad under a node that doubles it along x, with two named targets:

- `Raise` lifts the two top corners by 0.5 and tips one corner's normal. It
  is a SPARSE accessor naming only those two vertices, as an exporter writes
  a target that moves a patch of a face.
- `Wide` pushes the two right-hand corners out by 0.25 along x -- 0.5 once the
  node's scale has been baked in.

The mesh's own weights are `[0, 0.25]`, and one clip drives them: `Raise`
from 0 to 1 over a second while `Wide` falls from 0.25 to 0.

Run `python morph_quad.py` after changing it; the file is checked in.
"""

import base64
import json
import struct
from pathlib import Path

blob = bytearray()
views, accessors = [], []


def view(data):
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
    blob.extend(data)
    return len(views) - 1


def accessor(data, component, kind, count, bounds=None):
    entry = {"bufferView": view(data), "componentType": component, "count": count, "type": kind}
    if bounds is not None:
        entry["min"], entry["max"] = bounds
    accessors.append(entry)
    return len(accessors) - 1


def floats(rows):
    return b"".join(struct.pack("<%df" % len(row), *row) for row in rows)


positions = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
normals = [(0, 0, 1)] * 4
indices = [0, 1, 2, 0, 2, 3]

a_position = accessor(floats(positions), 5126, "VEC3", 4, ([0, 0, 0], [1, 1, 0]))
a_normal = accessor(floats(normals), 5126, "VEC3", 4)
a_indices = accessor(struct.pack("<6H", *indices), 5123, "SCALAR", 6)

# `Raise`, sparse: two of the four vertices, and nothing said of the others.
sparse_indices = view(struct.pack("<2H", 2, 3))
raise_places = view(floats([(0, 0.5, 0), (0, 0.5, 0)]))
accessors.append(
    {
        "componentType": 5126,
        "count": 4,
        "type": "VEC3",
        "min": [0, 0, 0],
        "max": [0, 0.5, 0],
        "sparse": {
            "count": 2,
            "indices": {"bufferView": sparse_indices, "componentType": 5123},
            "values": {"bufferView": raise_places},
        },
    }
)
a_raise_position = len(accessors) - 1
a_raise_normal = accessor(floats([(0, 0, 0), (0, 0, 0), (0, 0.2, 0), (0, 0, 0)]), 5126, "VEC3", 4)

# `Wide`, dense: every vertex has an entry and two of them are nought.
a_wide_position = accessor(
    floats([(0, 0, 0), (0.25, 0, 0), (0.25, 0, 0), (0, 0, 0)]), 5126, "VEC3", 4, ([0, 0, 0], [0.25, 0, 0])
)

# The clip: two keys, each with a weight a target.
a_times = accessor(floats([(0.0,), (1.0,)]), 5126, "SCALAR", 2, ([0.0], [1.0]))
a_weights = accessor(floats([(0.0,), (0.25,), (1.0,), (0.0,)]), 5126, "SCALAR", 4)

document = {
    "asset": {"version": "2.0", "generator": "engine/asset/tests/data/morph_quad.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "Face", "mesh": 0, "scale": [2.0, 1.0, 1.0]}],
    "meshes": [
        {
            "name": "Face",
            "weights": [0.0, 0.25],
            "extras": {"targetNames": ["Raise", "Wide"]},
            "primitives": [
                {
                    "attributes": {"POSITION": a_position, "NORMAL": a_normal},
                    "indices": a_indices,
                    "targets": [
                        {"POSITION": a_raise_position, "NORMAL": a_raise_normal},
                        {"POSITION": a_wide_position},
                    ],
                }
            ],
        }
    ],
    "animations": [
        {
            "name": "Speak",
            "samplers": [{"input": a_times, "output": a_weights, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}],
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

out = Path(__file__).resolve().parent / "morph_quad.gltf"
out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
print("wrote", out)
