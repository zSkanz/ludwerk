"""Draws `content/models/figure.gltf`: a block of a body with a cape of three
columns of five joints, which is what a `SpringBone` wants of a model.

Run it from this folder (`python tools/make_figure.py`) after changing it. The
file it writes is checked in, so nothing needs Python to run the example.

The cape is the example of how an artist lays one out: each column is a chain
of its own, hanging from a top joint that sits on the shoulders. The top joint
of a column is the one a `SpringBone` names -- it stays where the body puts it
and turns, and the four under it swing.
"""

import base64
import json
import struct
from pathlib import Path

COLUMNS = [("L", -0.26), ("M", 0.0), ("R", 0.26)]
ROWS = [1.62, 1.34, 1.06, 0.78, 0.50]
TAIL = 0.24
BACK = 0.21

# --- joints ------------------------------------------------------------------
# Every parent before its children. `Body` is the rig's root.
joints = [{"name": "Body", "parent": -1, "at": (0.0, 0.0, 0.0)}]
for name, x in COLUMNS:
    for row, y in enumerate(ROWS):
        joints.append(
            {"name": f"Cape_{name}{row}", "parent": 0 if row == 0 else len(joints) - 1, "at": (x, y, BACK)}
        )


def joint_of(column, row):
    return 1 + column * len(ROWS) + min(row, len(ROWS) - 1)


# --- geometry ----------------------------------------------------------------
positions, normals, bones, weights = [], [], [], []
body_indices, cape_indices = [], []


def vertex(at, normal, joint):
    positions.append(at)
    normals.append(normal)
    bones.append((joint, 0, 0, 0))
    weights.append((1.0, 0.0, 0.0, 0.0))
    return len(positions) - 1


def quad(indices, a, b, c, d):
    indices.extend([a, b, c, a, c, d])


# The body: a box from the feet to the shoulders, six faces with their own
# corners so each is lit flat.
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
    quad(body_indices, *[vertex(corner, normal, 0) for corner in corners])

# The cape: a sheet over the three columns, a row of corners at each joint and
# one more below the last, drawn on both of its sides.
heights = ROWS + [TAIL]
for facing in (1.0, -1.0):
    grid = [
        [vertex((x, y, BACK), (0.0, 0.0, facing), joint_of(column, row)) for column, (_, x) in enumerate(COLUMNS)]
        for row, y in enumerate(heights)
    ]
    for row in range(len(heights) - 1):
        for column in range(len(COLUMNS) - 1):
            a, b = grid[row][column], grid[row][column + 1]
            c, d = grid[row + 1][column + 1], grid[row + 1][column]
            quad(cape_indices, *((a, d, c, b) if facing > 0 else (a, b, c, d)))

# --- the file ----------------------------------------------------------------
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
a_body = add(struct.pack("<%dH" % len(body_indices), *body_indices), 5123, "SCALAR", len(body_indices), 34963)
a_cape = add(struct.pack("<%dH" % len(cape_indices), *cape_indices), 5123, "SCALAR", len(cape_indices), 34963)
# Each joint's inverse bind: where it stands in the model, undone.
inverse = [
    (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -j["at"][0], -j["at"][1], -j["at"][2], 1) for j in joints
]
a_inverse = add(floats(inverse), 5126, "MAT4", len(inverse))

nodes = []
for index, joint in enumerate(joints):
    parent = joints[joint["parent"]]["at"] if joint["parent"] >= 0 else (0.0, 0.0, 0.0)
    node = {"name": joint["name"], "translation": [joint["at"][i] - parent[i] for i in range(3)]}
    children = [child for child, other in enumerate(joints) if other["parent"] == index]
    if children:
        node["children"] = children
    nodes.append(node)
nodes.append({"name": "Figure", "mesh": 0, "skin": 0})

attributes = {"POSITION": a_position, "NORMAL": a_normal, "JOINTS_0": a_joints, "WEIGHTS_0": a_weights}
document = {
    "asset": {"version": "2.0", "generator": "examples/35-cape/tools/make_figure.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, len(joints)]}],
    "nodes": nodes,
    "skins": [{"joints": list(range(len(joints))), "inverseBindMatrices": a_inverse, "skeleton": 0}],
    "materials": [
        {"name": "Body", "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.45, 0.5, 1.0], "roughnessFactor": 0.8}},
        {
            "name": "Cape",
            "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorFactor": [0.72, 0.1, 0.12, 1.0], "roughnessFactor": 0.9},
        },
    ],
    "meshes": [
        {
            "name": "Figure",
            "primitives": [
                {"attributes": attributes, "indices": a_body, "material": 0},
                {"attributes": attributes, "indices": a_cape, "material": 1},
            ],
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

out = Path(__file__).resolve().parent.parent / "content" / "models" / "figure.gltf"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
print("wrote", out, "-", len(joints), "joints,", len(positions), "vertices")
