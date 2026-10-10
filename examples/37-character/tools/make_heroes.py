"""Draws the three heroes: `content/models/tall.gltf`, `stocky.gltf` and
`small.gltf` -- three bodies of three builds from three "tools", none of them
carrying a clip (ADR 0199).

Run it from this folder (`python tools/make_heroes.py`) after changing it. The
files it writes are checked in, so nothing needs Python to run the example.

Each is a figure of boxes, every box moved by one joint. What differs is what
differs between heroes bought or made apart, and each difference is one the
engine has to carry a clip across:

- **`tall`** is long and thin, and names its joints as the clip library does.
  It is still another skeleton: its bones are other lengths, so a clip's turns
  are taken and its own lengths are kept.
- **`stocky`** has short legs, wide shoulders and arms that reach its knees,
  and names its joints as one of the two big engines' exported humanoids do
  (`pelvis`, `spine_01`, `upperarm_l`, `calf_r`).
- **`small`** is the size of a child with a big head, names its joints as a
  common Blender rig does (`upper_arm.L`, `shin.R`), rests with its arms
  lowered forty degrees where the library's are straight out, and -- as that
  tool has it -- every joint of a limb is turned so that its own +Y runs along
  its bone. A turn from a clip means something else in every one of those
  joints' own spaces, and the stance has to be taken out.

`HEROES` is also read by `make_scene.py`: which joint is a hero's hips, its
ankles, its head and its hands, and how tall it stands.
"""

import math
from pathlib import Path

from rig import IDENTITY, X, Y, Blocks, Rig, add, arc, turn, write

SKIN = ((0.87, 0.72, 0.58), 0.7)
DARK = ((0.11, 0.11, 0.13), 0.6)


def hero(colour, names, build, lowered=0.0, along_bones=False):
    """A rig and its blocks from a build: heights up the back, how far out the
    shoulders and the hips are, how long each part of a limb is, and how thick
    the boxes are. `names` says what this rig calls each joint; `lowered` is
    how far below straight out its arms rest, and `along_bones` whether a
    limb's joints are turned so their own +Y runs along the bone."""
    b = build
    joints = [
        (names["hips"], None, (0.0, b["hips"], 0.0)),
        (names["spine"], names["hips"], (0.0, b["spine"], 0.0)),
        (names["chest"], names["spine"], (0.0, b["chest"], 0.0)),
        (names["neck"], names["chest"], (0.0, b["neck"], 0.0)),
        (names["head"], names["neck"], (0.0, b["head"], 0.0)),
    ]
    arms, legs = {}, {}
    for side, x in (("left", 1.0), ("right", -1.0)):
        out = (x * math.cos(math.radians(lowered)), -math.sin(math.radians(lowered)), 0.0)
        shoulder = (x * b["collar"], b["shoulders"], 0.0)
        upper = (x * b["shoulder"], b["shoulders"], 0.0)
        lower = add(upper, tuple(c * b["upper_arm"] for c in out))
        hand = add(lower, tuple(c * b["lower_arm"] for c in out))
        arms[side] = (upper, lower, hand, add(hand, tuple(c * b["hand"] for c in out)))
        # A limb's joints as that tool turns them: +Y along the bone.
        across = arc(Y, (x, 0.0, 0.0)) if along_bones else IDENTITY
        along = arc(Y, out) if along_bones else IDENTITY
        down = turn(X, 180.0) if along_bones else IDENTITY
        toes = turn(X, 90.0) if along_bones else IDENTITY
        hip = (x * b["hip"], b["legs"], 0.0)
        knee = (x * b["hip"], b["knee"], 0.0)
        ankle = (x * b["hip"], b["ankle"], 0.0)
        legs[side] = (hip, knee, ankle)
        joints += [
            (names[f"{side}_shoulder"], names["chest"], shoulder, across),
            (names[f"{side}_upper_arm"], names[f"{side}_shoulder"], upper, along),
            (names[f"{side}_lower_arm"], names[f"{side}_upper_arm"], lower, along),
            (names[f"{side}_hand"], names[f"{side}_lower_arm"], hand, along),
            (names[f"{side}_upper_leg"], names["hips"], hip, down),
            (names[f"{side}_lower_leg"], names[f"{side}_upper_leg"], knee, down),
            (names[f"{side}_foot"], names[f"{side}_lower_leg"], ankle, toes),
        ]
    rig = Rig(joints)

    materials = {"Body": (colour, 0.75), "Skin": SKIN, "Dark": DARK}
    blocks = Blocks(rig, materials)
    waist, chest, deep = b["waist"], b["chest_wide"], b["deep"]
    blocks.block(names["hips"], (-waist, b["legs"] - 0.05, -deep), (waist, b["spine"], deep), "Dark")
    blocks.block(names["spine"], (-waist * 0.9, b["spine"], -deep * 0.9), (waist * 0.9, b["chest"], deep * 0.9), "Body")
    blocks.block(names["chest"], (-chest, b["chest"], -deep), (chest, b["neck"], deep), "Body")
    neck = b["head_wide"] * 0.45
    blocks.block(names["neck"], (-neck, b["neck"], -neck), (neck, b["head"], neck), "Skin")
    wide, top = b["head_wide"], b["top"]
    blocks.block(names["head"], (-wide, b["head"], -wide), (wide, top, wide), "Skin")
    # A visor, so that which way a head is turned can be seen.
    eyes = b["head"] + (top - b["head"]) * 0.55
    blocks.block(names["head"], (-wide * 0.8, eyes, wide), (wide * 0.8, eyes + (top - b["head"]) * 0.22, wide * 1.15), "Dark")
    for side in ("left", "right"):
        upper, lower, hand, tips = arms[side]
        thick = b["arm"]
        blocks.limb(names[f"{side}_upper_arm"], upper, lower, thick, thick, "Body")
        blocks.limb(names[f"{side}_lower_arm"], lower, hand, thick * 0.85, thick * 0.85, "Skin")
        blocks.limb(names[f"{side}_hand"], hand, tips, thick * 0.6, thick * 1.1, "Skin")
        hip, knee, ankle = legs[side]
        thick = b["leg"]
        blocks.limb(names[f"{side}_upper_leg"], hip, knee, thick, thick, "Body")
        blocks.limb(names[f"{side}_lower_leg"], knee, ankle, thick * 0.85, thick * 0.85, "Body")
        blocks.block(
            names[f"{side}_foot"],
            (ankle[0] - thick * 0.5, 0.0, -thick * 0.6),
            (ankle[0] + thick * 0.5, b["ankle"], thick * 1.5),
            "Dark",
        )
    return {
        "rig": rig,
        "blocks": blocks,
        "materials": materials,
        "height": top,
        "ankle": b["ankle"],
        "hips": names["hips"],
        "head": names["head"],
        "feet": (names["left_foot"], names["right_foot"]),
        "hands": (names["left_hand"], names["right_hand"]),
    }


def named(pattern, back):
    """What a rig calls its joints: the back's five, and a limb's seven a
    side, `pattern` saying where the side goes in a name."""
    limbs = ("shoulder", "upper_arm", "lower_arm", "hand", "upper_leg", "lower_leg", "foot")
    names = dict(zip(("hips", "spine", "chest", "neck", "head"), back))
    for side, word in (("left", pattern[0]), ("right", pattern[1])):
        for limb, called in zip(limbs, pattern[2]):
            names[f"{side}_{limb}"] = word.format(called)
    return names


HEROES = {
    # The library's own names, and nothing of its lengths.
    "tall": hero(
        (0.20, 0.42, 0.80),
        named(
            ("Left{}", "Right{}", ("Shoulder", "UpperArm", "LowerArm", "Hand", "UpperLeg", "LowerLeg", "Foot")),
            ("Hips", "Spine", "Chest", "Neck", "Head"),
        ),
        {
            "hips": 1.16, "spine": 1.33, "chest": 1.54, "neck": 1.78, "head": 1.89, "top": 2.12,
            "shoulders": 1.73, "collar": 0.06, "shoulder": 0.17, "upper_arm": 0.35, "lower_arm": 0.34, "hand": 0.12,
            "hip": 0.085, "legs": 1.13, "knee": 0.61, "ankle": 0.09,
            "waist": 0.13, "chest_wide": 0.17, "deep": 0.08, "head_wide": 0.10, "arm": 0.08, "leg": 0.11,
        },
    ),
    "stocky": hero(
        (0.80, 0.33, 0.16),
        named(
            ("{}_l", "{}_r", ("clavicle", "upperarm", "lowerarm", "hand", "thigh", "calf", "foot")),
            ("pelvis", "spine_01", "spine_02", "neck_01", "head"),
        ),
        {
            "hips": 0.70, "spine": 0.82, "chest": 0.98, "neck": 1.19, "head": 1.27, "top": 1.50,
            "shoulders": 1.14, "collar": 0.10, "shoulder": 0.31, "upper_arm": 0.32, "lower_arm": 0.32, "hand": 0.14,
            "hip": 0.15, "legs": 0.66, "knee": 0.37, "ankle": 0.08,
            "waist": 0.24, "chest_wide": 0.31, "deep": 0.15, "head_wide": 0.13, "arm": 0.13, "leg": 0.16,
        },
    ),
    "small": hero(
        (0.25, 0.62, 0.30),
        named(
            ("{}.L", "{}.R", ("shoulder", "upper_arm", "forearm", "hand", "thigh", "shin", "foot")),
            ("hips", "spine", "chest", "neck", "head"),
        ),
        {
            "hips": 0.52, "spine": 0.60, "chest": 0.70, "neck": 0.83, "head": 0.88, "top": 1.18,
            "shoulders": 0.80, "collar": 0.04, "shoulder": 0.11, "upper_arm": 0.17, "lower_arm": 0.16, "hand": 0.07,
            "hip": 0.06, "legs": 0.50, "knee": 0.28, "ankle": 0.06,
            "waist": 0.09, "chest_wide": 0.11, "deep": 0.07, "head_wide": 0.15, "arm": 0.06, "leg": 0.08,
        },
        lowered=40.0,
        along_bones=True,
    ),
}

if __name__ == "__main__":
    for name, made in HEROES.items():
        out = Path(__file__).resolve().parent.parent / "content" / "models" / f"{name}.gltf"
        write(out, made["rig"], made["blocks"], made["materials"], [], "examples/37-character/tools/make_heroes.py")
        print("wrote", out, "-", len(made["rig"].names), "joints,", len(made["blocks"].positions), "vertices")
