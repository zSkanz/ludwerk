"""What `make_clips.py` and `make_heroes.py` share: a few lines of quaternions,
a rig at rest, blocks skinned to its joints, and the glTF writer.

No dependency but Python, as the other examples' model scripts have none.

**The space everything here is said in** is the model's own, as glTF has it:
+Y up, the character facing +Z, its LEFT at +X, metres.
"""

import base64
import json
import math
import struct

IDENTITY = (0.0, 0.0, 0.0, 1.0)
X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


# --- quaternions (x, y, z, w) ------------------------------------------------
def turn(axis, degrees):
    """A turn of `degrees` about `axis`, right-handed."""
    half = math.radians(degrees) / 2.0
    size = math.sqrt(sum(c * c for c in axis))
    s = math.sin(half) / size
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def mul(a, b):
    """`b`, and then `a`: both about the axes of whatever the joint hangs from."""
    return (
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    )


def conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def rotate(q, v):
    x, y, z = v
    qx, qy, qz, qw = q
    ox, oy, oz = qy * z - qz * y, qz * x - qx * z, qx * y - qy * x
    tx, ty, tz = qy * oz - qz * oy, qz * ox - qx * oz, qx * oy - qy * ox
    return (x + 2.0 * (qw * ox + tx), y + 2.0 * (qw * oy + ty), z + 2.0 * (qw * oz + tz))


def arc(start, end):
    """The shortest turn that takes the direction `start` to `end`."""
    a, b = unit(start), unit(end)
    axis = cross(a, b)
    q = (axis[0], axis[1], axis[2], 1.0 + sum(a[i] * b[i] for i in range(3)))
    size = math.sqrt(sum(c * c for c in q))
    return tuple(c / size for c in q)


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    size = math.sqrt(sum(c * c for c in v))
    return (v[0] / size, v[1] / size, v[2] / size)


# --- a rig at rest -----------------------------------------------------------
class Rig:
    """Joints, parents first. Each is `(name, parent's name or None, where it
    is, how it is turned)` -- the last two in the MODEL's space, at rest, which
    is how a rig is thought of; what a file wants (each joint from its parent)
    is worked out from them."""

    def __init__(self, joints):
        self.names = [joint[0] for joint in joints]
        self.parents = [self.names.index(joint[1]) if joint[1] is not None else -1 for joint in joints]
        self.at = [tuple(float(c) for c in joint[2]) for joint in joints]
        self.turned = [joint[3] if len(joint) > 3 else IDENTITY for joint in joints]

    def index(self, name):
        return self.names.index(name)

    def local(self, joint):
        """Where a joint is and how it is turned, from its parent."""
        parent = self.parents[joint]
        if parent < 0:
            return self.at[joint], self.turned[joint]
        back = conj(self.turned[parent])
        return rotate(back, sub(self.at[joint], self.at[parent])), mul(back, self.turned[joint])

    def inverse_bind(self, joint):
        """Model space to the joint's own, as glTF writes a matrix: a column
        after a column."""
        back = conj(self.turned[joint])
        columns = [rotate(back, axis) for axis in (X, Y, Z)]
        moved = rotate(back, self.at[joint])
        return (*columns[0], 0.0, *columns[1], 0.0, *columns[2], 0.0, -moved[0], -moved[1], -moved[2], 1.0)

    def posed(self, turns, moves=None):
        """Where every joint is, in the model's space, with `turns[name]` in
        place of a joint's rest turn from its parent and `moves[name]` in place
        of its rest offset -- what a clip's key says, and how the clips find
        the ground under a foot they have just moved."""
        at, turned = [], []
        for joint, name in enumerate(self.names):
            offset, rest = self.local(joint)
            offset = (moves or {}).get(name, offset)
            own = turns.get(name, rest)
            parent = self.parents[joint]
            if parent < 0:
                at.append(offset)
                turned.append(own)
            else:
                at.append(add(at[parent], rotate(turned[parent], offset)))
                turned.append(mul(turned[parent], own))
        return at


# --- blocks skinned to joints ------------------------------------------------
class Blocks:
    """A figure of boxes, each moved by one joint and by no other: six faces
    with their own corners, so each is lit flat."""

    def __init__(self, rig, materials):
        self.rig = rig
        self.positions, self.normals, self.bones = [], [], []
        self.indices = {name: [] for name in materials}

    def _box(self, joint, corner, material):
        # `corner(i, j, k)` is the corner at the low or the high end (0 or 1)
        # of each of the box's three axes.
        faces = [
            ((1, 0, 0), (1, 0, 1), (1, 1, 1), (1, 1, 0)),
            ((0, 0, 1), (0, 0, 0), (0, 1, 0), (0, 1, 1)),
            ((0, 1, 1), (0, 1, 0), (1, 1, 0), (1, 1, 1)),
            ((0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 0, 0)),
            ((0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1)),
            ((1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 0)),
        ]
        centre = tuple(sum(corner(i, j, k)[axis] for i in (0, 1) for j in (0, 1) for k in (0, 1)) / 8 for axis in range(3))
        for face in faces:
            corners = [corner(*which) for which in face]
            normal = unit(cross(sub(corners[1], corners[0]), sub(corners[2], corners[0])))
            # Wound to face out of the box, whichever way its axes were handed.
            middle = tuple(sum(c[axis] for c in corners) / 4 for axis in range(3))
            if sum(normal[axis] * (middle[axis] - centre[axis]) for axis in range(3)) < 0:
                corners.reverse()
                normal = tuple(-c for c in normal)
            first = len(self.positions)
            for at in corners:
                self.positions.append(at)
                self.normals.append(normal)
                self.bones.append(self.rig.index(joint))
            self.indices[material].extend([first, first + 1, first + 2, first, first + 2, first + 3])

    def block(self, joint, low, high, material):
        """A box along the model's own axes, from `low` to `high`."""
        ends = (low, high)
        self._box(joint, lambda i, j, k: (ends[i][0], ends[j][1], ends[k][2]), material)

    def limb(self, joint, start, end, wide, deep, material):
        """A box round the line from `start` to `end`, `deep` front to back and
        `wide` the other way across -- an arm or a leg, whichever way the rig
        rests it."""
        along = sub(end, start)
        across = unit(cross(Z, along))
        front = unit(cross(along, across))

        def corner(i, j, k):
            at = add(start, tuple(c * i for c in along))
            at = add(at, tuple(c * wide * (j - 0.5) for c in across))
            return add(at, tuple(c * deep * (k - 0.5) for c in front))

        self._box(joint, corner, material)


# --- the file ----------------------------------------------------------------
def write(path, rig, blocks, materials, clips, generator):
    """One glTF, its buffer inside it. `materials` is name to (colour,
    roughness); `clips` a list of `{"name", "times", "turns": {joint: [a turn a
    key]}, "moves": {joint: [an offset a key]}}`."""
    blob = bytearray()
    views, accessors = [], []

    def accessor(data, component, kind, count, bounds=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        made = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        if bounds is not None:
            made["min"], made["max"] = bounds
        accessors.append(made)
        return len(accessors) - 1

    def floats(rows):
        return b"".join(struct.pack("<%df" % len(row), *row) for row in rows)

    count = len(blocks.positions)
    bounds = (
        [min(p[i] for p in blocks.positions) for i in range(3)],
        [max(p[i] for p in blocks.positions) for i in range(3)],
    )
    attributes = {
        "POSITION": accessor(floats(blocks.positions), 5126, "VEC3", count, bounds),
        "NORMAL": accessor(floats(blocks.normals), 5126, "VEC3", count),
        "JOINTS_0": accessor(b"".join(struct.pack("<4H", bone, 0, 0, 0) for bone in blocks.bones), 5123, "VEC4", count),
        "WEIGHTS_0": accessor(floats([(1.0, 0.0, 0.0, 0.0)] * count), 5126, "VEC4", count),
    }
    primitives = []
    for slot, name in enumerate(materials):
        drawn = blocks.indices[name]
        if drawn:
            primitives.append(
                {
                    "attributes": attributes,
                    "indices": accessor(struct.pack("<%dH" % len(drawn), *drawn), 5123, "SCALAR", len(drawn)),
                    "material": slot,
                }
            )

    joints = range(len(rig.names))
    inverse = accessor(floats([rig.inverse_bind(joint) for joint in joints]), 5126, "MAT4", len(rig.names))
    nodes = []
    for joint in joints:
        offset, turned = rig.local(joint)
        node = {"name": rig.names[joint], "translation": list(offset)}
        if max(abs(turned[i] - IDENTITY[i]) for i in range(4)) > 1e-9:
            node["rotation"] = list(turned)
        children = [child for child in joints if rig.parents[child] == joint]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({"name": "Mesh", "mesh": 0, "skin": 0})

    animations = []
    for clip in clips:
        times = clip["times"]
        clock = accessor(floats([(t,) for t in times]), 5126, "SCALAR", len(times), ([times[0]], [times[-1]]))
        samplers, channels = [], []
        for path_name, kind, keyed in (("rotation", "VEC4", clip.get("turns", {})), ("translation", "VEC3", clip.get("moves", {}))):
            for name, keys in keyed.items():
                samplers.append({"input": clock, "output": accessor(floats(keys), 5126, kind, len(keys))})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": rig.index(name), "path": path_name}})
        animations.append({"name": clip["name"], "samplers": samplers, "channels": channels})

    document = {
        "asset": {"version": "2.0", "generator": generator},
        "scene": 0,
        "scenes": [{"nodes": [joint for joint in joints if rig.parents[joint] < 0] + [mesh_node]}],
        "nodes": nodes,
        "skins": [{"joints": list(joints), "inverseBindMatrices": inverse, "skeleton": 0}],
        "materials": [
            {
                "name": name,
                "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0], "metallicFactor": 0.0, "roughnessFactor": rough},
            }
            for name, (colour, rough) in materials.items()
        ],
        "meshes": [{"name": "Mesh", "primitives": primitives}],
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii"),
            }
        ],
        "bufferViews": views,
        "accessors": accessors,
    }
    if animations:
        document["animations"] = animations
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
