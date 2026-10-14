"""Draws `content/models/figure.gltf` and `content/models/cape.gltf`: a block of
a body, and a cape that is a mesh of its own -- what a `Cloth` wants of a
model (ADR 0214).

Run it from this folder (`python tools/make_models.py`) after changing it. The
files it writes are checked in, so nothing needs Python to run the example.

The cape is the example of how an artist lays one out for cloth: modelled
hanging straight down behind the body, its top row weighted to a joint on the
shoulders (`CapePin`, the one the `Cloth`'s `PinJoints` names) and the rest
weighted as the cape would be with no cloth -- here, to the body. Both files
have the same two joints, so the cape wears the body's pose (`PoseFrom`).
"""

import base64
import json
import struct
from pathlib import Path

# The cape: nine columns across the back, fourteen rows from the shoulders
# down, a little behind the body's back.
COLUMNS = 9
ROWS = 14
WIDTH = 0.62
TOP = 1.62
BOTTOM = 0.32
BACK = 0.2

JOINTS = [
    {"name": "Body", "parent": -1, "at": (0.0, 0.0, 0.0)},
    {"name": "CapePin", "parent": 0, "at": (0.0, TOP, BACK)},
]


def write(name, positions, normals, bones, indices, color, generator):
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
    a_joints = add(b"".join(struct.pack("<4H", j, 0, 0, 0) for j in bones), 5123, "VEC4", len(bones), 34962)
    a_weights = add(floats([(1.0, 0.0, 0.0, 0.0)] * len(bones)), 5126, "VEC4", len(bones), 34962)
    a_indices = add(struct.pack("<%dH" % len(indices), *indices), 5123, "SCALAR", len(indices), 34963)
    # Each joint's inverse bind: where it stands in the model, undone.
    inverse = [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -j["at"][0], -j["at"][1], -j["at"][2], 1) for j in JOINTS]
    a_inverse = add(floats(inverse), 5126, "MAT4", len(inverse))

    nodes = []
    for index, joint in enumerate(JOINTS):
        parent = JOINTS[joint["parent"]]["at"] if joint["parent"] >= 0 else (0.0, 0.0, 0.0)
        node = {"name": joint["name"], "translation": [joint["at"][i] - parent[i] for i in range(3)]}
        children = [child for child, other in enumerate(JOINTS) if other["parent"] == index]
        if children:
            node["children"] = children
        nodes.append(node)
    nodes.append({"name": name, "mesh": 0, "skin": 0})

    document = {
        "asset": {"version": "2.0", "generator": generator},
        "scene": 0,
        "scenes": [{"nodes": [0, len(JOINTS)]}],
        "nodes": nodes,
        "skins": [{"joints": list(range(len(JOINTS))), "inverseBindMatrices": a_inverse, "skeleton": 0}],
        "materials": [
            {"name": name, "pbrMetallicRoughness": {"baseColorFactor": color, "roughnessFactor": 0.85}},
        ],
        "meshes": [
            {
                "name": name,
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
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
            }
        ],
        "bufferViews": views,
        "accessors": accessors,
    }
    out = Path(__file__).resolve().parent.parent / "content" / "models" / f"{name.lower()}.gltf"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print("wrote", out, "-", len(positions), "vertices")


def quad(indices, a, b, c, d):
    indices.extend([a, b, c, a, c, d])


# --- The body: a box from the feet to the shoulders, each face its own
# corners so each is lit flat, all of it on the body's joint.
positions, normals, bones, indices = [], [], [], []


def vertex(at, normal, joint):
    positions.append(at)
    normals.append(normal)
    bones.append(joint)
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
    quad(indices, *[vertex(corner, normal, 0) for corner in corners])
write("Figure", positions, normals, bones, indices, [0.42, 0.45, 0.5, 1.0], "examples/46-cloth-cape/tools/make_models.py")

# --- The cape: one sheet of COLUMNS by ROWS corners, drawn on both of its
# sides -- the back a sheet of its own at the same corners, which the cloth
# takes as the same points. Its top row is on `CapePin`; the rest on the body.
positions, normals, bones, indices = [], [], [], []
for facing in (1.0, -1.0):
    grid = []
    for row in range(ROWS):
        y = TOP - (TOP - BOTTOM) * row / (ROWS - 1)
        grid.append(
            [
                vertex((WIDTH * (column / (COLUMNS - 1) - 0.5), y, BACK), (0.0, 0.0, facing), 1 if row == 0 else 0)
                for column in range(COLUMNS)
            ]
        )
    for row in range(ROWS - 1):
        for column in range(COLUMNS - 1):
            a, b = grid[row][column], grid[row][column + 1]
            c, d = grid[row + 1][column + 1], grid[row + 1][column]
            quad(indices, *((a, d, c, b) if facing > 0 else (a, b, c, d)))
write("Cape", positions, normals, bones, indices, [0.72, 0.1, 0.12, 1.0], "examples/46-cloth-cape/tools/make_models.py")
