"""Writes the four models of the morph landing test (ADR 0196).

A square pillar, half a unit wide and one tall, standing on the origin, in two
kinds and two makings:

- `content/models/plain.gltf` and `content/models/skinned.gltf` are the pillar
  at REST with three morph targets, and the mesh's own weights for them:
    `Tall`  lifts the top by 2;      weight  0.5  -- a pillar two tall.
    `Lean`  pushes the top by 1 in x and tips the side faces' normals;
                                     weight -0.25 -- the top a quarter back.
    `Burst` throws every vertex outwards by 3;
                                     weight  0    -- and nothing happens.
- `made/plain.gltf` and `made/skinned.gltf` are the same pillar with NO
  targets, its vertices already where those weights put them and its normals
  already turned.

Drawn in the same scene, the two makings are the same picture -- which says a
morphed vertex lands where the file says, in the lit pass, in the depth the
camera's prepass writes and in the shadow it casts.

The skinned kind hangs every vertex on a joint, `Tip`, which a clip holds
moved and turned: a target is applied before the joints (as glTF has it), and
a turn is what tells the two orders apart.

Run it from this folder (`python tools/make_models.py`) after changing it; the
files it writes are checked in.
"""

import base64
import json
import math
import struct
from pathlib import Path

HALF = 0.25
TARGETS = (("Tall", 0.5), ("Lean", -0.25), ("Burst", 0.0))

# Six faces of four vertices each, flat-shaded: (normal, four corners).
FACES = []
for axis, sign in ((0, 1), (0, -1), (2, 1), (2, -1), (1, 1), (1, -1)):
    normal = [0.0, 0.0, 0.0]
    normal[axis] = float(sign)
    if axis == 1:
        y = 1.0 if sign > 0 else 0.0
        corners = [(-HALF, y, -HALF), (-HALF, y, HALF), (HALF, y, HALF), (HALF, y, -HALF)]
        if sign < 0:
            corners.reverse()
    else:
        u = HALF * sign
        if axis == 0:
            corners = [(u, 0.0, HALF), (u, 0.0, -HALF), (u, 1.0, -HALF), (u, 1.0, HALF)]
        else:
            corners = [(-HALF, 0.0, u), (HALF, 0.0, u), (HALF, 1.0, u), (-HALF, 1.0, u)]
        if sign < 0:
            corners.reverse()
    FACES.append((tuple(normal), corners))

positions, normals, indices = [], [], []
for normal, corners in FACES:
    base = len(positions)
    positions.extend(corners)
    normals.extend([normal] * 4)
    indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def deltas(name):
    """A target's position and normal deltas, a vertex each."""
    places, turns = [], []
    for position, normal in zip(positions, normals):
        top = position[1] > 0.5
        if name == "Tall":
            places.append((0.0, 2.0, 0.0) if top else (0.0, 0.0, 0.0))
            turns.append((0.0, 0.0, 0.0))
        elif name == "Lean":
            places.append((1.0, 0.0, 0.0) if top else (0.0, 0.0, 0.0))
            # The faces that look along x tip as the pillar leans; a number
            # chosen to be seen, not derived -- the test is that it arrives.
            turns.append((0.0, -1.2 * normal[0], 0.0))
        else:
            places.append((position[0] * 12.0, 3.0, position[2] * 12.0))
            turns.append((0.0, 0.0, 0.0))
    return places, turns


def made():
    """The pillar with the weights applied, as the vertex stage applies them."""
    places = [list(position) for position in positions]
    turned = [list(normal) for normal in normals]
    for name, weight in TARGETS:
        target_places, target_turns = deltas(name)
        for index in range(len(places)):
            for axis in range(3):
                places[index][axis] += target_places[index][axis] * weight
                turned[index][axis] += target_turns[index][axis] * weight
    for normal in turned:
        size = math.sqrt(sum(c * c for c in normal))
        for axis in range(3):
            normal[axis] /= size
    return [tuple(p) for p in places], [tuple(n) for n in turned]


def write(path, with_targets, skinned):
    blob = bytearray()
    views, accessors = [], []

    def add(data, component, kind, count, bounds=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        entry = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        if bounds is not None:
            entry["min"], entry["max"] = bounds
        accessors.append(entry)
        return len(accessors) - 1

    def floats(rows):
        return b"".join(struct.pack("<%df" % len(row), *row) for row in rows)

    def bounds(rows):
        return [min(r[a] for r in rows) for a in range(3)], [max(r[a] for r in rows) for a in range(3)]

    places, turned = (positions, normals) if with_targets else made()
    count = len(places)
    attributes = {
        "POSITION": add(floats(places), 5126, "VEC3", count, bounds(places)),
        "NORMAL": add(floats(turned), 5126, "VEC3", count),
    }
    if skinned:
        # Every vertex wholly on joint 1, `Tip`.
        attributes["JOINTS_0"] = add(struct.pack("<%dH" % (count * 4), *([1, 0, 0, 0] * count)), 5123, "VEC4", count)
        attributes["WEIGHTS_0"] = add(floats([(1.0, 0.0, 0.0, 0.0)] * count), 5126, "VEC4", count)
    primitive = {
        "attributes": attributes,
        "indices": add(struct.pack("<%dH" % len(indices), *indices), 5123, "SCALAR", len(indices)),
        "material": 0,
    }
    mesh = {"name": "Pillar", "primitives": [primitive]}
    if with_targets:
        primitive["targets"] = []
        for name, _ in TARGETS:
            target_places, target_turns = deltas(name)
            primitive["targets"].append(
                {
                    "POSITION": add(floats(target_places), 5126, "VEC3", count, bounds(target_places)),
                    "NORMAL": add(floats(target_turns), 5126, "VEC3", count),
                }
            )
        mesh["weights"] = [weight for _, weight in TARGETS]
        mesh["extras"] = {"targetNames": [name for name, _ in TARGETS]}

    document = {
        "asset": {"version": "2.0", "generator": "tests/screenshots/morphlanding/tools/make_models.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "meshes": [mesh],
        "materials": [
            {
                "name": "Clay",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.85, 0.42, 0.2, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.8,
                }
            }
        ],
    }
    if skinned:
        identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        inverse = add(floats([identity, identity]), 5126, "MAT4", 2)
        # `Tip` held half a unit to the side and turned a twelfth of a turn
        # about z: two keys of the same pose.
        half = math.radians(30.0) / 2.0
        turn = (0.0, 0.0, math.sin(half), math.cos(half))
        times = add(floats([(0.0,), (1.0,)]), 5126, "SCALAR", 2, ([0.0], [1.0]))
        moved = add(floats([(0.5, 0.0, 0.0)] * 2), 5126, "VEC3", 2)
        turned_keys = add(floats([turn] * 2), 5126, "VEC4", 2)
        document["nodes"] = [
            {"name": "Pillar", "mesh": 0, "skin": 0},
            {"name": "Base", "children": [2]},
            {"name": "Tip"},
        ]
        document["scenes"] = [{"nodes": [0, 1]}]
        document["skins"] = [{"joints": [1, 2], "inverseBindMatrices": inverse, "skeleton": 1}]
        document["animations"] = [
            {
                "name": "Hold",
                "samplers": [
                    {"input": times, "output": moved, "interpolation": "LINEAR"},
                    {"input": times, "output": turned_keys, "interpolation": "LINEAR"},
                ],
                "channels": [
                    {"sampler": 0, "target": {"node": 2, "path": "translation"}},
                    {"sampler": 1, "target": {"node": 2, "path": "rotation"}},
                ],
            }
        ]
    else:
        document["nodes"] = [{"name": "Pillar", "mesh": 0}]
    document["buffers"] = [
        {
            "byteLength": len(blob),
            "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
        }
    ]
    document["bufferViews"] = views
    document["accessors"] = accessors
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print("wrote", path)


root = Path(__file__).resolve().parent.parent
for kind, skinned in (("plain", False), ("skinned", True)):
    write(root / "content" / "models" / f"{kind}.gltf", True, skinned)
    write(root / "made" / f"{kind}.gltf", False, skinned)
