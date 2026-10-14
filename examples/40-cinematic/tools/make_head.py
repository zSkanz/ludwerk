"""Draws `content/models/head.gltf`: a bust whose mouth has the three shape
keys a voice moves, which is what a `LipSync` wants of a model.

Run it from this folder (`python tools/make_head.py`) after changing it. The
file it writes is checked in, so nothing needs Python to run the example.

- **One mesh with a skeleton** -- `Root`, `Neck`, `Head` -- and a material a
  part: skin, the whites of the eyes, what is dark (pupils, lashes, brows) and
  the mouth.
- **Four shape keys**, named as `content/models/head.face.json` asks for
  them: `JawOpen` drops the chin and the lower lip, `Wide` draws the corners
  of the mouth out, `Round` brings them in and parts the lips, and `Blink`
  shuts the eyes.
- **One clip**, `Idle`, which sways the head and keys no shape key: every
  weight on this face is the voice's.

Everything on the face lies on the head's own surface -- an ellipsoid -- a
hair in front of it, so a key that slides a lip slides it along the skin
rather than through it.
"""

import base64
import json
import math
import struct
from pathlib import Path

# --- the head's surface ------------------------------------------------------
HEAD = (0.0, 0.82, 0.02)
RADII = (0.30, 0.37, 0.33)
EYE_Y, EYE_X = 0.885, 0.115
BROW_Y = 0.985
MOUTH_Y, MOUTH_HALF = 0.665, 0.125
KEYS = ("JawOpen", "Wide", "Round", "Blink")

JOINTS = [
    {"name": "Root", "parent": -1, "at": (0.0, 0.0, 0.0)},
    {"name": "Neck", "parent": 0, "at": (0.0, 0.30, 0.0)},
    {"name": "Head", "parent": 1, "at": (0.0, 0.50, 0.0)},
]


def on_face(x, y, lift):
    """The point of the head's front at (x, y), `lift` in front of the skin,
    and the skin's normal there."""
    u = (x - HEAD[0]) / RADII[0]
    v = (y - HEAD[1]) / RADII[1]
    w = math.sqrt(max(1.0 - u * u - v * v, 0.0004))
    normal = (u / RADII[0], v / RADII[1], w / RADII[2])
    size = math.sqrt(sum(c * c for c in normal))
    normal = tuple(c / size for c in normal)
    at = (x + normal[0] * lift, y + normal[1] * lift, HEAD[2] + w * RADII[2] + normal[2] * lift)
    return at, normal


positions, normals, bones, weights = [], [], [], []
# For each key, a vertex's delta: {vertex: (place, normal)}.
deltas = {key: {} for key in KEYS}
indices = {"Skin": [], "White": [], "Dark": [], "Mouth": []}


def skin_weights(y):
    """Wholly the head's above the neck, the root's below it, and shared in
    between so the neck bends rather than breaks."""
    if y >= 0.50:
        return (2, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)
    if y >= 0.30:
        t = (y - 0.30) / 0.20
        return (2, 1, 0, 0), (t, 1.0 - t, 0.0, 0.0)
    return (0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)


def vertex(at, normal):
    positions.append(at)
    normals.append(normal)
    joints, shares = skin_weights(at[1])
    bones.append(joints)
    weights.append(shares)
    return len(positions) - 1


def face_vertex(x, y, lift, moves=None):
    """A vertex on the face, and where each key in `moves` slides it to:
    `moves` is {key: (x, y)} -- the place on the face, not a delta -- or
    (x, y, lift) for a key that also stands it further off the skin."""
    at, normal = on_face(x, y, lift)
    index = vertex(at, normal)
    for key, (to_x, to_y, *off) in (moves or {}).items():
        to, turned = on_face(to_x, to_y, off[0] if off else lift)
        place = tuple(to[i] - at[i] for i in range(3))
        if max(abs(c) for c in place) > 1e-6:
            deltas[key][index] = (place, tuple(turned[i] - normal[i] for i in range(3)))
    return index


def quad(material, a, b, c, d):
    indices[material].extend([a, b, c, a, c, d])


def ellipsoid(material, centre, radii, segments, rings, chin=False):
    """A ball of `rings` by `segments`, wound to face outwards."""
    grid = []
    for ring in range(rings + 1):
        polar = math.pi * ring / rings
        row = []
        for segment in range(segments + 1):
            around = 2.0 * math.pi * segment / segments
            unit = (math.sin(polar) * math.sin(around), math.cos(polar), math.sin(polar) * math.cos(around))
            at = tuple(centre[i] + unit[i] * radii[i] for i in range(3))
            normal = tuple(unit[i] / radii[i] for i in range(3))
            size = math.sqrt(sum(c * c for c in normal))
            index = vertex(at, tuple(c / size for c in normal))
            # The jaw: what is below the mouth and in front goes down with it,
            # most at the chin and not at all by the ears.
            if chin and at[1] < MOUTH_Y - 0.01 and unit[2] > 0.0:
                drop = min((MOUTH_Y - 0.01 - at[1]) / 0.12, 1.0) * unit[2]
                deltas["JawOpen"][index] = ((0.0, -0.075 * drop, 0.003 * drop), (0.0, 0.0, 0.0))
            row.append(index)
        grid.append(row)
    for ring in range(rings):
        for segment in range(segments):
            quad(
                material,
                grid[ring + 1][segment],
                grid[ring + 1][segment + 1],
                grid[ring][segment + 1],
                grid[ring][segment],
            )


# --- the bust ------------------------------------------------------------------
ellipsoid("Skin", HEAD, RADII, 32, 20, chin=True)
# The neck, and shoulders to stand on.
ellipsoid("Skin", (0.0, 0.38, 0.0), (0.115, 0.22, 0.115), 16, 8)
ellipsoid("Skin", (0.0, 0.12, 0.0), (0.42, 0.17, 0.22), 24, 10)


def disc(material, x, y, rx, ry, lift, segments=18):
    centre = face_vertex(x, y, lift)
    rim = [
        face_vertex(
            x + math.sin(2.0 * math.pi * s / segments) * rx, y + math.cos(2.0 * math.pi * s / segments) * ry, lift
        )
        for s in range(segments)
    ]
    for s in range(segments):
        indices[material].extend([centre, rim[(s + 1) % segments], rim[s]])


for side in (-1.0, 1.0):
    eye_x = EYE_X * side
    disc("White", eye_x, EYE_Y, 0.062, 0.044, 0.004)
    disc("Dark", eye_x, EYE_Y - 0.004, 0.024, 0.026, 0.007)

    # **The lid**: a strip of skin folded up under the brow, whose lower edge
    # `Blink` brings down over the eye -- and a line of lashes along that
    # edge, so that a shut eye is drawn and not merely absent.
    columns = 11
    half = 0.070

    def arc(t, top):
        # The eye's outline at `t` from -1 to 1: its upper edge, or its lower.
        reach = math.sqrt(max(1.0 - t * t, 0.0))
        return EYE_Y + (0.048 if top else -0.048) * reach

    lid, lash_top, lash_low = [], [], []
    for row in range(3):
        strip = []
        for column in range(columns):
            t = -1.0 + 2.0 * column / (columns - 1)
            x = eye_x + t * half
            open_y = arc(t, True) - 0.004 * row
            shut_y = arc(t, True) + (arc(t, False) - arc(t, True)) * (row / 2.0)
            strip.append(face_vertex(x, open_y, 0.010, {"Blink": (x, shut_y)}))
        lid.append(strip)
    for column in range(columns):
        t = -1.0 + 2.0 * column / (columns - 1)
        x = eye_x + t * half
        edge = arc(t, True) - 0.008
        shut = arc(t, False)
        lash_top.append(face_vertex(x, edge, 0.012, {"Blink": (x, shut)}))
        lash_low.append(face_vertex(x, edge - 0.007, 0.012, {"Blink": (x, shut - 0.007)}))
    for column in range(columns - 1):
        for row in range(2):
            quad("Skin", lid[row + 1][column], lid[row + 1][column + 1], lid[row][column + 1], lid[row][column])
        quad("Dark", lash_low[column], lash_low[column + 1], lash_top[column + 1], lash_top[column])

    # **The brow**: a bar over the eye, which nothing here moves.
    columns = 7
    top, low = [], []
    for column in range(columns):
        t = column / (columns - 1)  # nought by the nose, one by the temple
        x = (0.045 + 0.155 * t) * side
        y = BROW_Y + 0.012 * math.sin(math.pi * t) - 0.010 * t
        thick = 0.020 - 0.008 * t
        for collect, edge in ((top, y + thick / 2), (low, y - thick / 2)):
            collect.append(face_vertex(x, edge, 0.011))
    for column in range(columns - 1):
        a, b, c, d = low[column], low[column + 1], top[column + 1], top[column]
        # Wound to face forwards on either side of the face.
        if side > 0:
            quad("Dark", a, b, c, d)
        else:
            quad("Dark", b, a, d, c)

# **The mouth**: a line at rest. `JawOpen` drops its lower lip into an oval,
# `Wide` draws its corners out and flattens it, and `Round` brings them in and
# parts the lips -- the three things a voice does to one.
columns = 15
rows = []
# The chin's skin goes straight down with the jaw while the head curves away
# under it, so an open mouth's inside stands a little further off the skin
# the lower it is: drawn in front of the chin, not through it.
for row, (rest, jaw, pucker, off) in enumerate(
    ((0.008, 0.012, 0.030, 0.008), (0.0, -0.050, 0.0, 0.022), (-0.008, -0.118, -0.034, 0.036))
):
    strip = []
    for column in range(columns):
        t = -1.0 + 2.0 * column / (columns - 1)
        taper = math.sqrt(max(1.0 - t * t, 0.0))
        x = t * MOUTH_HALF
        y = MOUTH_Y + rest * taper
        strip.append(
            face_vertex(
                x,
                y,
                0.008,
                {
                    "JawOpen": (x * 0.92, MOUTH_Y + jaw * taper, 0.008 + (off - 0.008) * taper),
                    "Wide": (x * 1.22, MOUTH_Y + rest * taper * 0.6 + 0.010 * t * t),
                    "Round": (x * 0.56, MOUTH_Y + pucker * taper),
                },
            )
        )
    rows.append(strip)
for column in range(columns - 1):
    for row in range(2):
        quad("Mouth", rows[row + 1][column], rows[row + 1][column + 1], rows[row][column + 1], rows[row][column])

# --- the file ------------------------------------------------------------------
blob = bytearray()
views, accessors = [], []


def add(data, component, kind, count, bounds=None):
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
    blob.extend(data)
    accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
    if bounds is not None:
        accessor["min"], accessor["max"] = bounds
    accessors.append(accessor)
    return len(accessors) - 1


def floats(rows):
    return b"".join(struct.pack("<%df" % len(row), *row) for row in rows)


def bounds_of(rows):
    return [min(r[i] for r in rows) for i in range(3)], [max(r[i] for r in rows) for i in range(3)]


count = len(positions)
attributes = {
    "POSITION": add(floats(positions), 5126, "VEC3", count, bounds_of(positions)),
    "NORMAL": add(floats(normals), 5126, "VEC3", count),
    "JOINTS_0": add(b"".join(struct.pack("<4H", *row) for row in bones), 5123, "VEC4", count),
    "WEIGHTS_0": add(floats(weights), 5126, "VEC4", count),
}
targets = []
for key in KEYS:
    places = [deltas[key].get(index, ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))[0] for index in range(count)]
    turns = [deltas[key].get(index, ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))[1] for index in range(count)]
    targets.append(
        {
            "POSITION": add(floats(places), 5126, "VEC3", count, bounds_of(places)),
            "NORMAL": add(floats(turns), 5126, "VEC3", count),
        }
    )

MATERIALS = [
    ("Skin", (0.86, 0.66, 0.54), 0.62),
    ("White", (0.93, 0.93, 0.90), 0.35),
    ("Dark", (0.10, 0.07, 0.06), 0.55),
    ("Mouth", (0.42, 0.10, 0.12), 0.50),
]
primitives = []
for slot, (name, _, _) in enumerate(MATERIALS):
    drawn = indices[name]
    primitives.append(
        {
            "attributes": attributes,
            "indices": add(struct.pack("<%dH" % len(drawn), *drawn), 5123, "SCALAR", len(drawn)),
            "material": slot,
            "targets": targets,
        }
    )

inverse = add(
    floats([(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -j["at"][0], -j["at"][1], -j["at"][2], 1) for j in JOINTS]),
    5126,
    "MAT4",
    len(JOINTS),
)

nodes = []
for index, joint in enumerate(JOINTS):
    parent = JOINTS[joint["parent"]]["at"] if joint["parent"] >= 0 else (0.0, 0.0, 0.0)
    node = {"name": joint["name"], "translation": [joint["at"][i] - parent[i] for i in range(3)]}
    children = [child for child, other in enumerate(JOINTS) if other["parent"] == index]
    if children:
        node["children"] = children
    nodes.append(node)
MESH_NODE = len(nodes)
nodes.append({"name": "Bust", "mesh": 0, "skin": 0})
HEAD_NODE = 2


def turn(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def times(values):
    return add(floats([(t,) for t in values]), 5126, "SCALAR", len(values), ([values[0]], [values[-1]]))


# `Idle`: the head looks a little left, a little right, four seconds round.
idle_times = [0.0, 1.0, 2.0, 3.0, 4.0]
idle_turns = [turn((0, 1, 0), d) for d in (0.0, 7.0, 0.0, -7.0, 0.0)]


animations = [
    {
        "name": "Idle",
        "samplers": [
            {"input": times(idle_times), "output": add(floats(idle_turns), 5126, "VEC4", len(idle_turns))},
        ],
        "channels": [{"sampler": 0, "target": {"node": HEAD_NODE, "path": "rotation"}}],
    },
]

document = {
    "asset": {"version": "2.0", "generator": "examples/38-dubbing/tools/make_head.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, MESH_NODE]}],
    "nodes": nodes,
    "skins": [{"joints": list(range(len(JOINTS))), "inverseBindMatrices": inverse, "skeleton": 0}],
    "materials": [
        {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0], "metallicFactor": 0.0, "roughnessFactor": rough}}
        for name, colour, rough in MATERIALS
    ],
    "meshes": [
        {
            "name": "Bust",
            "primitives": primitives,
            "weights": [0.0] * len(KEYS),
            "extras": {"targetNames": list(KEYS)},
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

out = Path(__file__).resolve().parent.parent / "content" / "models" / "head.gltf"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
moved = {key: len(deltas[key]) for key in KEYS}
print("wrote", out, "-", count, "vertices,", moved)
