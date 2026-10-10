"""Draws `content/models/walker.gltf`: the least a character is, for the
two-process animation gate -- a block of a body, one arm that every clip
swings by its own amount, and a cape of three joints for a `SpringBone`.

Run it from this folder (`python tools/make_walker.py`) after changing it. The
file it writes is checked in, so nothing needs Python to run the gate.

The clips are told apart by their length and by how far the arm swings; what
the gate reads is the graph's states and events, not the picture.
"""

import base64
import json
import math
import struct
from pathlib import Path

# Every parent before its children. `Body` is the rig's root.
JOINTS = [
    {"name": "Body", "parent": -1, "at": (0.0, 0.0, 0.0)},
    {"name": "Arm", "parent": 0, "at": (0.35, 1.4, 0.0)},
    {"name": "Cape_M0", "parent": 0, "at": (0.0, 1.6, 0.2)},
    {"name": "Cape_M1", "parent": 2, "at": (0.0, 1.2, 0.2)},
    {"name": "Cape_M2", "parent": 3, "at": (0.0, 0.8, 0.2)},
]
ARM = 1

# name, seconds, how far the arm swings at its middle, in radians.
CLIPS = [
    ("Idle", 1.0, 0.1),
    ("Walk", 1.0, 0.5),
    ("Run", 0.5, 0.9),
    ("Jump", 0.3, 1.2),
    ("Fall", 0.6, 0.7),
    ("Slash", 0.4, 1.5),
]

positions, normals, bones, weights, indices = [], [], [], [], []


def vertex(at, normal):
    positions.append(at)
    normals.append(normal)
    bones.append((0, 0, 0, 0))
    weights.append((1.0, 0.0, 0.0, 0.0))
    return len(positions) - 1


lo, hi = (-0.3, 0.0, -0.17), (0.3, 1.72, 0.17)
faces = [
    ((0, 0, 1), [(lo[0], lo[1], hi[2]), (hi[0], lo[1], hi[2]), (hi[0], hi[1], hi[2]), (lo[0], hi[1], hi[2])]),
    ((0, 0, -1), [(hi[0], lo[1], lo[2]), (lo[0], lo[1], lo[2]), (lo[0], hi[1], lo[2]), (hi[0], hi[1], lo[2])]),
    ((1, 0, 0), [(hi[0], lo[1], hi[2]), (hi[0], lo[1], lo[2]), (hi[0], hi[1], lo[2]), (hi[0], hi[1], hi[2])]),
    ((-1, 0, 0), [(lo[0], lo[1], lo[2]), (lo[0], lo[1], hi[2]), (lo[0], hi[1], hi[2]), (lo[0], hi[1], lo[2])]),
    ((0, 1, 0), [(lo[0], hi[1], hi[2]), (hi[0], hi[1], hi[2]), (hi[0], hi[1], lo[2]), (lo[0], hi[1], lo[2])]),
    ((0, -1, 0), [(lo[0], lo[1], lo[2]), (hi[0], lo[1], lo[2]), (hi[0], lo[1], hi[2]), (lo[0], lo[1], hi[2])]),
]
for normal, corners in faces:
    a, b, c, d = [vertex(corner, normal) for corner in corners]
    indices.extend([a, b, c, a, c, d])

blob = bytearray()
views, accessors = [], []


def add(data, component, kind, count, target=None, bounds=None):
    while len(blob) % 4:
        blob.append(0)
    view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
    if target is not None:
        view["target"] = target
    blob.extend(data)
    views.append(view)
    accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
    if bounds is not None:
        accessor["min"], accessor["max"] = bounds
    accessors.append(accessor)
    return len(accessors) - 1


def floats(rows):
    return b"".join(struct.pack("<%df" % len(row), *row) for row in rows)


mins = [min(p[i] for p in positions) for i in range(3)]
maxs = [max(p[i] for p in positions) for i in range(3)]
a_position = add(floats(positions), 5126, "VEC3", len(positions), 34962, (mins, maxs))
a_normal = add(floats(normals), 5126, "VEC3", len(normals), 34962)
a_joints = add(b"".join(struct.pack("<4H", *row) for row in bones), 5123, "VEC4", len(bones), 34962)
a_weights = add(floats(weights), 5126, "VEC4", len(weights), 34962)
a_indices = add(struct.pack("<%dH" % len(indices), *indices), 5123, "SCALAR", len(indices), 34963)
inverse = [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -j["at"][0], -j["at"][1], -j["at"][2], 1) for j in JOINTS]
a_inverse = add(floats(inverse), 5126, "MAT4", len(inverse))

animations = []
for name, seconds, swing in CLIPS:
    times = [0.0, seconds / 2.0, seconds]
    # About Z: the arm from straight out towards up, and back -- the first
    # key and the last alike, so a loop has no pop.
    turns = [(0.0, 0.0, 0.0, 1.0), (0.0, 0.0, math.sin(swing / 2.0), math.cos(swing / 2.0)), (0.0, 0.0, 0.0, 1.0)]
    a_times = add(floats([(t,) for t in times]), 5126, "SCALAR", len(times), None, ([0.0], [seconds]))
    a_turns = add(floats(turns), 5126, "VEC4", len(turns))
    animations.append(
        {
            "name": name,
            "samplers": [{"input": a_times, "output": a_turns, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 0, "target": {"node": ARM, "path": "rotation"}}],
        }
    )

nodes = []
for index, joint in enumerate(JOINTS):
    parent = JOINTS[joint["parent"]]["at"] if joint["parent"] >= 0 else (0.0, 0.0, 0.0)
    node = {"name": joint["name"], "translation": [joint["at"][i] - parent[i] for i in range(3)]}
    children = [child for child, other in enumerate(JOINTS) if other["parent"] == index]
    if children:
        node["children"] = children
    nodes.append(node)
nodes.append({"name": "Walker", "mesh": 0, "skin": 0})

document = {
    "asset": {"version": "2.0", "generator": "tests/netcode/animated/tools/make_walker.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, len(JOINTS)]}],
    "nodes": nodes,
    "skins": [{"joints": list(range(len(JOINTS))), "inverseBindMatrices": a_inverse, "skeleton": 0}],
    "materials": [
        {"name": "Body", "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.45, 0.5, 1.0], "roughnessFactor": 0.8}}
    ],
    "meshes": [
        {
            "name": "Walker",
            "primitives": [
                {
                    "attributes": {
                        "POSITION": a_position,
                        "NORMAL": a_normal,
                        "JOINTS_0": a_joints,
                        "WEIGHTS_0": a_weights,
                    },
                    "indices": a_indices,
                    "material": 0,
                }
            ],
        }
    ],
    "animations": animations,
    "buffers": [
        {
            "byteLength": len(blob),
            "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
        }
    ],
    "bufferViews": views,
    "accessors": accessors,
}

out = Path(__file__).resolve().parent.parent / "content" / "models" / "walker.gltf"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
print("wrote", out, "-", len(JOINTS), "joints,", len(CLIPS), "clips")
