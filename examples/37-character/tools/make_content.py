"""Makes this example's models and its clip library out of three free packs,
and the crossbow, which is drawn here.

    python tools/make_content.py <a folder the packs were unpacked in>

**Nothing is fetched**, here or when the engine is built: the packs are
somebody's download, this cuts what the example uses out of them, and what it
writes is checked in. `content/THIRD_PARTY.md` says which pack each file is
from, the version, where it was had and under what terms -- all three are
CC0 -- and the packs' own licence files are beside it, copied as they came.

What it writes, under `content/`:

- `clips/adventurer.glb` -- **the library**: one rig and twelve clips, none of
  them anybody's body here. Ten are the pack's, under the names the graph
  uses. `AimUp` is written below, as a lean is. And the pack's rig is given a
  joint of its own, `String`, under the hand that holds a crossbow, with a
  track in `Shoot` and in `Reload`: a prop joint, moved by the clip that
  moves the arms, as rigs are commonly made.
- `models/rogue.glb` -- the player's body, without its head.
- `models/rogue_hood.glb`, `models/rogue_head.glb` -- the same head with a
  hood up and with none: two pieces on the body's skeleton, worn through
  `MeshPart.PoseFrom` and swapped while the game runs.
- `models/skeleton.glb` -- another pack's skeleton on the same rig, made
  tall and thin.
- `models/mannequin.glb` -- another pack's body: other joint names, another
  rest, legs twice as long.
- `models/crossbow.glb` -- drawn here: a stock, a bow and a string, with two
  joints, `Crossbow` and `String`.

And `tools/heroes.json`, which `make_scene.py` reads: what each body calls
its hips, feet, head and hands, and how tall it stands.
"""

import json
import math
import shutil
import struct
import sys
from pathlib import Path

from gltf_edit import Gltf, _conj, _mul, _rotate, world_rest

HERE = Path(__file__).resolve().parent
CONTENT = HERE.parent / "content"

# The first pack's characters stand 2.2 metres as they come. Everything of
# that pack -- the library, the bodies on its rig, the crossbow made for its
# hand -- is made this much smaller, together.
SMALLER = 0.8

# The clips kept, and the names the graph knows them by.
CLIPS = {
    "Idle": "Idle",
    "Walking_A": "Walk",
    "Running_A": "Run",
    "Walking_Backwards": "WalkBack",
    "Running_Strafe_Left": "StrafeLeft",
    "Running_Strafe_Right": "StrafeRight",
    "Jump_Start": "Jump",
    "Jump_Idle": "Fall",
    "1H_Melee_Attack_Slice_Horizontal": "Slash",
    "2H_Ranged_Shoot": "Shoot",
    "2H_Ranged_Reload": "Reload",
}
# What the pack puts in its characters' hands.
HELD = {"Knife_Offhand", "1H_Crossbow", "2H_Crossbow", "Knife", "Throwable"}


def found(root, pattern):
    matches = sorted(Path(root).rglob(pattern))
    if not matches:
        raise SystemExit(f"nothing called {pattern} under {root}")
    return matches[0]


def turn(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def is_mesh(node):
    return "mesh" in node


def top_of(doc):
    """How high the meshes that are kept reach, at rest."""
    gone = getattr(doc, "_gone", set())
    high = 0.0
    for index, node in enumerate(doc.doc["nodes"]):
        if "mesh" not in node or "skin" not in node or index in gone:
            continue
        for primitive in doc.doc["meshes"][node["mesh"]]["primitives"]:
            high = max(high, max(value[1] for value in doc.values(primitive["attributes"]["POSITION"])))
    return high


def facts(doc, names, high):
    """What `make_scene.py` wants of a body: its joints by what they are, and
    a few places on it."""
    rest = world_rest(doc)
    head_at, head_turned = rest[doc.node(names["head"])]
    ankle = rest[doc.node(names["feet"][0])][0][1]
    # Between the eyes: two fifths of the way up the head and in front of it,
    # said in the head joint's own space.
    tall = high - head_at[1]
    eyes = _rotate(_conj(head_turned), (0.0, tall * 0.4, tall * 0.45))
    return {**names, "height": round(high, 4), "ankle": round(ankle, 4), "eyes": [round(value, 4) for value in eyes]}


# --- the crossbow ----------------------------------------------------------------
# Drawn in the space the pack draws its own in -- the stock along +Z, the bow
# across X, up along Y -- and hung in the hand where the pack hangs its own:
# a quarter turn about the hand slot's Y, a little way along it.
STOCK_FROM, STOCK_TO = -0.25, 1.0
TIP = (0.60, 0.03, 0.78)
LOOSE = (0.0, 0.03, 0.78)
COCKED = (0.0, 0.03, 0.30)
IN_HAND = (-0.105, -0.010, 0.0)


def in_slot(point):
    """A point of the crossbow, in the hand slot's own space, at the size the
    bodies are made."""
    x, y, z = point
    return tuple((value + offset) * SMALLER for value, offset in zip((z, y, -x), IN_HAND))


def crossbow(path):
    positions, normals, joints, weights = [], [], [], []
    indices = {"Wood": [], "Steel": [], "String": []}

    def box(material, start, end, wide, high, end_joint=0):
        """A bar from `start` to `end`, `wide` across and `high` up. Its far
        end is `end_joint`'s: nought is the crossbow itself."""
        along = tuple(end[i] - start[i] for i in range(3))
        length = math.sqrt(sum(c * c for c in along))
        along = tuple(c / length for c in along)
        up = (0.0, 1.0, 0.0)
        across = (along[2], 0.0, -along[0])
        size = math.sqrt(sum(c * c for c in across)) or 1.0
        across = tuple(c / size for c in across)
        corners = []
        for at, joint in ((start, 0), (end, end_joint)):
            for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                corners.append((tuple(at[i] + across[i] * sx * wide / 2 + up[i] * sy * high / 2 for i in range(3)), joint))
        faces = (
            ((0, 3, 2, 1), tuple(-c for c in along)),
            ((4, 5, 6, 7), along),
            ((0, 1, 5, 4), (0.0, -1.0, 0.0)),
            ((3, 7, 6, 2), up),
            ((0, 4, 7, 3), tuple(-c for c in across)),
            ((1, 2, 6, 5), across),
        )
        for corner, normal in faces:
            first = len(positions)
            for index in corner:
                place, joint = corners[index]
                positions.append(in_slot(place))
                # Turned as the points are: the quarter turn about Y.
                normals.append((normal[2], normal[1], -normal[0]))
                joints.append((joint, 0, 0, 0))
                weights.append((1.0, 0.0, 0.0, 0.0))
            indices[material].extend([first, first + 1, first + 2, first, first + 2, first + 3])

    box("Wood", (0.0, -0.02, STOCK_FROM), (0.0, -0.02, STOCK_TO), 0.08, 0.08)
    box("Wood", (0.0, -0.09, STOCK_FROM), (0.0, -0.09, 0.02), 0.09, 0.16)
    for side in (-1.0, 1.0):
        box("Steel", (0.0, 0.03, STOCK_TO - 0.03), (TIP[0] * side, TIP[1], TIP[2]), 0.05, 0.035)
        # The string is the crossbow's at the bow's tip and the string's own
        # joint where the two halves meet.
        box("String", (TIP[0] * side, TIP[1], TIP[2]), COCKED, 0.012, 0.012, end_joint=1)

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

    count = len(positions)
    attributes = {
        "POSITION": add(
            floats(positions),
            5126,
            "VEC3",
            count,
            ([min(p[i] for p in positions) for i in range(3)], [max(p[i] for p in positions) for i in range(3)]),
        ),
        "NORMAL": add(floats(normals), 5126, "VEC3", count),
        "JOINTS_0": add(b"".join(struct.pack("<4H", *row) for row in joints), 5123, "VEC4", count),
        "WEIGHTS_0": add(floats(weights), 5126, "VEC4", count),
    }
    materials = [("Wood", (0.42, 0.29, 0.16), 0.8), ("Steel", (0.30, 0.31, 0.34), 0.45), ("String", (0.90, 0.88, 0.80), 0.7)]
    primitives = []
    for slot, (name, _, _) in enumerate(materials):
        drawn = indices[name]
        primitives.append(
            {"attributes": attributes, "indices": add(struct.pack("<%dH" % len(drawn), *drawn), 5123, "SCALAR", len(drawn)), "material": slot}
        )
    string_at = in_slot(COCKED)
    binds = [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *(-c for c in string_at), 1)]
    doc = {
        "asset": {"version": "2.0", "generator": "examples/37-character/tools/make_content.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "Crossbow", "children": [1]},
            {"name": "String", "translation": list(string_at)},
            {"name": "Mesh", "mesh": 0, "skin": 0},
        ],
        "skins": [{"joints": [0, 1], "inverseBindMatrices": add(floats(binds), 5126, "MAT4", 2), "skeleton": 0}],
        "materials": [
            {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0], "metallicFactor": 0.0, "roughnessFactor": rough}}
            for name, colour, rough in materials
        ],
        "meshes": [{"name": "Crossbow", "primitives": primitives}],
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(blob)}],
    }
    return Gltf(doc, [blob]).save(path)


# --- the files --------------------------------------------------------------------
def main(packs):
    hooded = found(packs, "Rogue_Hooded.glb")
    bare = found(packs, "Rogue.glb")
    minion = found(packs, "Skeleton_Minion.glb")
    mannequin = found(packs, "AnimationLibrary_*_Standard.gltf")
    sizes = {}

    # **The library.** The rig, one of the pack's meshes -- a model file is
    # asked for one -- and the clips, with what a clip says of a joint that
    # never leaves its rest taken out.
    library = Gltf.load(hooded)
    library.drop_nodes(lambda node: is_mesh(node) and node.get("name") != "Rogue_Body")
    library.keep_clips(CLIPS)
    library.drop_rest_channels()
    library.stretch(SMALLER, SMALLER, SMALLER)
    # The crossbow's string, as a joint of the hand that holds it: at rest it
    # is drawn back and latched. Loosed, it is straight between the bow's tips.
    cocked, loose = in_slot(COCKED), in_slot(LOOSE)
    library.add_joint("String", "handslot.r", cocked)
    shot = library.length("Shoot")
    library.add_track("Shoot", "String", [(0.0, cocked), (0.10 * shot, cocked), (0.10 * shot + 0.04, loose), (shot, loose)])
    # The pack's reload has the off hand at the string from about an eighth of
    # a second to half a second in; the string comes back with it.
    library.add_track("Reload", "String", [(0.0, loose), (0.13, loose), (0.48, cocked), (library.length("Reload"), cocked)])
    # A lean back, to be added to whatever else plays: the two joints of the
    # back each turned twelve degrees about the model's X, from where they
    # rest. A joint's turn is said in its parent's space, so the model's X is
    # taken into the joint's own first.
    rest = world_rest(library)
    lean = {}
    for joint in ("spine", "chest"):
        index = library.node(joint)
        own = tuple(library.doc["nodes"][index].get("rotation", (0.0, 0.0, 0.0, 1.0)))
        in_world = rest[index][1]
        leaned = _mul(own, _mul(_conj(in_world), _mul(turn((1.0, 0.0, 0.0), -12.0), in_world)))
        lean[joint] = [(0.0, own), (1.0, leaned)]
    library.add_clip("AimUp", lean)
    sizes["clips/adventurer.glb"] = library.save(CONTENT / "clips" / "adventurer.glb")

    heroes = {}

    # **The player's body**: everything but the head and what was in its
    # hands. Its cape is a mesh with no skin, hung from the chest joint.
    body = Gltf.load(hooded)
    body.drop_nodes(lambda node: is_mesh(node) and (node.get("name") in HELD or node.get("name") == "Rogue_Head_Hooded"))
    body.keep_clips({})
    body.stretch(SMALLER, SMALLER, SMALLER)
    sizes["models/rogue.glb"] = body.save(CONTENT / "models" / "rogue.glb")

    # **Its two heads**, each a file with the skeleton and one mesh.
    hood = Gltf.load(hooded)
    hood.drop_nodes(lambda node: is_mesh(node) and node.get("name") != "Rogue_Head_Hooded")
    hood.keep_clips({})
    hood.stretch(SMALLER, SMALLER, SMALLER)
    sizes["models/rogue_hood.glb"] = hood.save(CONTENT / "models" / "rogue_hood.glb")
    head = Gltf.load(bare)
    head.drop_nodes(lambda node: is_mesh(node) and node.get("name") != "Rogue_Head")
    head.keep_clips({})
    head.stretch(SMALLER, SMALLER, SMALLER)
    sizes["models/rogue_head.glb"] = head.save(CONTENT / "models" / "rogue_head.glb")
    # A hand, for a limb that reaches, is the joint the forearm ends at.
    kaykit = {"hips": "hips", "feet": ["foot.l", "foot.r"], "head": "head", "hands": ["wrist.l", "wrist.r"], "slots": ["handslot.l", "handslot.r"]}
    heroes["rogue"] = facts(body, kaykit, top_of(hood))

    # **The skeleton**: the same rig, taller than the library's and thinner --
    # another build under the same names, with longer legs.
    thin = Gltf.load(minion)
    thin.keep_clips({})
    thin.stretch(0.78, 0.92, 0.78)
    sizes["models/skeleton.glb"] = thin.save(CONTENT / "models" / "skeleton.glb")
    heroes["skeleton"] = facts(thin, kaykit, top_of(thin))

    # **The mannequin**: another pack's body, as it comes, without its clips.
    other = Gltf.load(mannequin)
    other.keep_clips({})
    sizes["models/mannequin.glb"] = other.save(CONTENT / "models" / "mannequin.glb")
    heroes["mannequin"] = facts(
        other,
        {"hips": "DEF-hips", "feet": ["DEF-foot.L", "DEF-foot.R"], "head": "DEF-head", "hands": ["DEF-hand.L", "DEF-hand.R"], "slots": []},
        top_of(other),
    )

    sizes["models/crossbow.glb"] = crossbow(CONTENT / "models" / "crossbow.glb")

    (HERE / "heroes.json").write_text(json.dumps(heroes, indent=1) + "\n", encoding="utf-8", newline="\n")

    # The packs' own words about themselves, as they came.
    licences = CONTENT / "licences"
    licences.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(hooded.parents[2] / "LICENSE.txt", licences / "KayKit-Adventurers-1.0-LICENSE.txt")
    shutil.copyfile(minion.parents[2] / "LICENSE.txt", licences / "KayKit-Skeletons-1.0-LICENSE.txt")
    shutil.copyfile(mannequin.parents[1] / "LICENSE", licences / "Universal-Animation-Library-LICENSE.txt")

    for name, size in sizes.items():
        print(f"{name}: {size / 1024:.0f} KB")
    for name, hero in heroes.items():
        print(name, hero)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
