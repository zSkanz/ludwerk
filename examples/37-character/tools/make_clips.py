"""Writes `content/clips/humanoid.gltf`: the one library of clips the three
heroes share (ADR 0199), on a rig of its own that none of them has.

Run it from this folder (`python tools/make_clips.py`) after changing it. The
file it writes is checked in, so nothing needs Python to run the example.

The library is the example of what a pack of bought motion is, cut down to
what can be read here: a humanoid rig of nineteen joints resting with its arms
straight out, a mesh nobody looks at -- a file with a skin has to have one --
and ten clips. **A clip keys turns, and the hips' place**: every joint's turn
from its parent, and where the hips are, which is the one translation a body
of another build takes from a clip (scaled by how long its legs are).

The clips are not drawn by hand. Each is a function of how far through it the
body is, sampled sixteen times a cycle:

- a foot is put where a step would have it -- ahead when it lands, drawn back
  under the body at the pace the body is taken to move, lifted and swung
  forward again -- and the leg is bent to reach it;
- the hips come down by what the further foot on the ground needs, which is
  the bob of a walk;
- a loop's last key is its first, so it comes round without a jump.

**Where a foot lands is a moment the graph names** (`Step`, at a quarter and
at three quarters of `Walk` and `Run`): the left foot lands a quarter of the
way round both, and the right one half a cycle later.
"""

import math
from pathlib import Path

from rig import IDENTITY, X, Y, Z, Blocks, Rig, arc, conj, mul, turn, write

# --- the rig -----------------------------------------------------------------
HIPS, HIP, KNEE, ANKLE = 0.95, 0.92, 0.50, 0.08
THIGH, SHIN = HIP - KNEE, KNEE - ANKLE


RIG = Rig(
    [
        ("Hips", None, (0.0, HIPS, 0.0)),
        ("Spine", "Hips", (0.0, 1.10, 0.0)),
        ("Chest", "Spine", (0.0, 1.28, 0.0)),
        ("Neck", "Chest", (0.0, 1.48, 0.0)),
        ("Head", "Neck", (0.0, 1.58, 0.0)),
        *[
            joint
            for name, x in (("Left", 1.0), ("Right", -1.0))
            for joint in (
                (f"{name}Shoulder", "Chest", (0.07 * x, 1.44, 0.0)),
                (f"{name}UpperArm", f"{name}Shoulder", (0.19 * x, 1.44, 0.0)),
                (f"{name}LowerArm", f"{name}UpperArm", (0.47 * x, 1.44, 0.0)),
                (f"{name}Hand", f"{name}LowerArm", (0.73 * x, 1.44, 0.0)),
                (f"{name}UpperLeg", "Hips", (0.10 * x, HIP, 0.0)),
                (f"{name}LowerLeg", f"{name}UpperLeg", (0.10 * x, KNEE, 0.0)),
                (f"{name}Foot", f"{name}LowerLeg", (0.10 * x, ANKLE, 0.0)),
            )
        ],
    ]
)
LEFT, RIGHT = 1.0, -1.0
SIDES = (("Left", LEFT), ("Right", RIGHT))


# --- a pose ------------------------------------------------------------------
def leg(ankle):
    """The thigh's, the shin's and the foot's turns that put an ankle at
    `ankle` -- from the top of its leg, +x to the body's left, +z ahead -- with
    the knee ahead and the sole level."""
    x, y, z = ankle
    out = math.degrees(math.atan2(x, -y))
    down = math.hypot(x, y)
    reach = min(math.hypot(z, down), (THIGH + SHIN) * 0.999)
    bend = 180.0 - math.degrees(math.acos((THIGH**2 + SHIN**2 - reach**2) / (2.0 * THIGH * SHIN)))
    ahead = math.degrees(math.atan2(z, down) + math.acos((THIGH**2 + reach**2 - SHIN**2) / (2.0 * THIGH * reach)))
    thigh = mul(turn(Z, out), turn(X, -ahead))
    shin = turn(X, bend)
    return thigh, shin, conj(mul(thigh, shin))


def arm(side, down, ahead=0.0, bend=12.0):
    """An arm lowered `down` degrees from straight out, swung `ahead`, and
    bent at the elbow."""
    return mul(turn(X, -ahead), turn(Z, -side * down)), turn(Y, -side * bend)


def aimed(side, towards, bend=12.0):
    """An arm pointed along `towards`, said in the chest's own space."""
    return arc((side, 0.0, 0.0), towards), turn(Y, -side * bend)


def pose(left, right, arms, drop=0.02, sway=0.0, lean=0.0, twist=0.0, look=0.0):
    """Every joint's turn and the hips' place. `left` and `right` are where
    each ankle is from where it stands at rest -- (to the body's left, up,
    ahead) -- `drop` how far the hips are let down and `sway` how far to the
    left they are; `lean` is the back's, forwards, `twist` its turn to the
    left, and `look` the head's own."""
    turns = {name: IDENTITY for name in RIG.names}
    half = mul(turn(X, lean / 2.0), turn(Y, twist / 2.0))
    turns["Spine"], turns["Chest"] = half, half
    # The head keeps looking where the body is going while the back turns.
    turns["Neck"] = turn(Y, (look - twist) / 2.0)
    turns["Head"] = mul(turn(Y, (look - twist) / 2.0), turn(X, -lean * 0.6))
    for (name, side), ankle, (upper, lower) in zip(SIDES, (left, right), arms):
        hang = (ankle[0] - sway, ankle[1] + drop - (HIP - ANKLE), ankle[2])
        turns[f"{name}UpperLeg"], turns[f"{name}LowerLeg"], turns[f"{name}Foot"] = leg(hang)
        turns[f"{name}UpperArm"], turns[f"{name}LowerArm"] = upper, lower
    return turns, (sway, HIPS - drop, 0.0)


def low_enough(*feet, least=0.02):
    """How far the hips come down for every foot that is on the ground to be
    reached with a knee not quite straight: the bob of a walk."""
    drop = least
    for across, lift, ahead in feet:
        if lift < 1e-6:
            far = math.hypot(across, ahead)
            drop = max(drop, (HIP - ANKLE) - math.sqrt(max(((THIGH + SHIN) * 0.985) ** 2 - far**2, 0.0)))
    return drop


def step(u, reach, stance, lift):
    """Where a foot is along the way the body goes, and how high, `u` of the
    way round its cycle from the moment it lands: drawn back at a steady pace
    for `stance` of the cycle, then lifted and swung ahead again."""
    if u < stance:
        return reach * (1.0 - 2.0 * u / stance), 0.0
    swing = (u - stance) / (1.0 - stance)
    return -reach * math.cos(math.pi * swing), lift * math.sin(math.pi * swing)


# --- the clips ---------------------------------------------------------------
def idle(p):
    wave = math.sin(2.0 * math.pi * p)
    arms = [arm(side, 76.0 + 2.0 * wave, ahead=2.0) for _, side in SIDES]
    return pose((0, 0, 0), (0, 0, 0), arms, drop=0.02 + 0.006 * (1.0 - math.cos(2.0 * math.pi * p)), lean=1.5 * wave, look=6.0 * wave)


def gait(p, reach, stance, lift, swing, elbow, lean, twist, soft, back=False):
    """A walk or a run: the left foot lands a quarter of the way round."""
    feet = []
    for landed in (0.25, 0.75):
        ahead, up = step((p - landed) % 1.0, reach, stance, lift)
        feet.append((0.0, up, -ahead if back else ahead))
    wave = math.sin(2.0 * math.pi * p) * (-1.0 if back else 1.0)
    # An arm goes back as its own leg goes forward.
    arms = [arm(side, 78.0, ahead=-side * swing * wave, bend=elbow + side * 8.0 * wave) for _, side in SIDES]
    return pose(feet[0], feet[1], arms, drop=low_enough(*feet, least=soft), lean=lean, twist=twist * wave)


def walk(p):
    return gait(p, reach=0.375, stance=0.5, lift=0.12, swing=24.0, elbow=14.0, lean=3.0, twist=7.0, soft=0.045)


def run(p):
    return gait(p, reach=0.44, stance=0.36, lift=0.24, swing=42.0, elbow=75.0, lean=13.0, twist=12.0, soft=0.07)


def walk_back(p):
    return gait(p, reach=0.30, stance=0.5, lift=0.10, swing=14.0, elbow=14.0, lean=-3.0, twist=4.0, soft=0.04, back=True)


def strafe(p, towards):
    """A side step towards the body's left (`towards` 1) or its right (-1):
    the leading foot reaches out and lands, and the other is brought up to
    it."""
    feet = []
    for _, side in SIDES:
        across, up = step((p - (0.25 if side == towards else 0.75)) % 1.0, 0.17, 0.5, 0.10)
        feet.append((across * towards + 0.13 * side, up, 0.0))
    wave = math.sin(2.0 * math.pi * p)
    arms = [arm(side, 72.0 + 5.0 * wave * side * towards, ahead=4.0, bend=22.0) for _, side in SIDES]
    return pose(feet[0], feet[1], arms, drop=low_enough(*feet, least=0.04), lean=2.0)


def fall(p):
    wave = math.sin(2.0 * math.pi * p)
    arms = [arm(side, -38.0 + 9.0 * wave * side, ahead=-12.0, bend=28.0) for _, side in SIDES]
    return pose((0.03, 0.10 + 0.04 * wave, 0.10), (-0.03, 0.10 - 0.04 * wave, -0.08), arms, drop=0.0, lean=6.0)


def looped(name, seconds, at, steps=16):
    keys = [at((index % steps) / steps) for index in range(steps + 1)]
    return keyed(name, [seconds * index / steps for index in range(steps + 1)], keys)


def keyed(name, times, keys, only=None):
    turns = {}
    for joint in RIG.names:
        if only is not None and joint not in only:
            continue
        track = []
        for pose_turns, _ in keys:
            q = pose_turns[joint]
            # The same turn the short way round from the key before it.
            if track and sum(a * b for a, b in zip(track[-1], q)) < 0.0:
                q = tuple(-c for c in q)
            track.append(q)
        turns[joint] = track
    moves = {"Hips": [hips for _, hips in keys]} if only is None else {}
    return {"name": name, "times": times, "turns": turns, "moves": moves}


STAND = pose((0, 0, 0), (0, 0, 0), [arm(side, 76.0, ahead=2.0) for _, side in SIDES])

# `Jump` starts as the feet leave the ground: the knees come up, the body
# springs straight with the arms thrown up, and it hangs there for `Fall` to
# take over.
JUMP = [
    (0.00, STAND),
    (0.08, pose((0, 0.16, 0.06), (0, 0.16, 0.06), [arm(side, 70.0, ahead=-35.0, bend=30.0) for _, side in SIDES], drop=0.10, lean=14.0)),
    (0.22, pose((0, 0.0, -0.04), (0, 0.0, -0.04), [arm(side, 60.0, ahead=125.0, bend=20.0) for _, side in SIDES], drop=0.0, lean=-6.0)),
    (0.40, pose((0.02, 0.07, 0.10), (-0.02, 0.05, -0.06), [arm(side, 40.0, ahead=70.0, bend=30.0) for _, side in SIDES], drop=0.0, lean=3.0)),
]

# `Slash`: the right arm is drawn up and back, cut across the body and let
# down. It keys the chest and everything on it and nothing below -- it is
# played on a layer masked from the chest, over whatever the legs are doing.
UPPER = ["Chest", "Neck", "Head"] + [f"{name}{part}" for name, _ in SIDES for part in ("Shoulder", "UpperArm", "LowerArm", "Hand")]


def slash(twist, towards, bend):
    left = arm(LEFT, 66.0, ahead=-14.0, bend=24.0)
    turns, hips = pose((0, 0, 0), (0, 0, 0), [left, aimed(RIGHT, towards, bend)])
    turns["Chest"] = turn(Y, twist)
    turns["Neck"] = turns["Head"] = turn(Y, -twist / 2.0)
    return turns, hips


SLASH = [
    (0.00, STAND),
    (0.13, slash(-26.0, (-0.45, 0.80, -0.35), 30.0)),
    (0.20, slash(0.0, (-0.35, 0.45, 0.85), 14.0)),
    (0.27, slash(30.0, (0.50, -0.10, 0.85), 8.0)),
    (0.36, slash(34.0, (0.70, -0.50, 0.40), 14.0)),
    (0.50, STAND),
]

# `AimUp` is played on an additive layer: what it adds is how far it is from
# its own first key, so its first key is the rig at rest and its last is the
# lean. It does not loop -- it gets to its last key and stays, and the layer's
# weight says how much of the lean there is.
LEAN = ({**{name: IDENTITY for name in RIG.names}, "Spine": turn(X, -11.0), "Chest": turn(X, -13.0), "Neck": turn(X, -8.0)}, None)
REST = ({name: IDENTITY for name in RIG.names}, None)

clips = [
    looped("Idle", 2.0, idle),
    looped("Walk", 1.0, walk),
    looped("Run", 0.7, run),
    looped("StrafeLeft", 1.0, lambda p: strafe(p, 1.0)),
    looped("StrafeRight", 1.0, lambda p: strafe(p, -1.0)),
    looped("WalkBack", 1.0, walk_back),
    keyed("Jump", [t for t, _ in JUMP], [key for _, key in JUMP]),
    looped("Fall", 0.6, fall, steps=8),
    keyed("Slash", [t for t, _ in SLASH], [key for _, key in SLASH], only=UPPER),
    keyed("AimUp", [0.0, 0.1], [REST, LEAN], only=["Spine", "Chest", "Neck"]),
]

# The mesh nobody looks at: one small block on the hips.
MATERIALS = {"Placeholder": ((0.5, 0.5, 0.5), 0.9)}
blocks = Blocks(RIG, MATERIALS)
blocks.block("Hips", (-0.02, HIPS - 0.02, -0.02), (0.02, HIPS + 0.02, 0.02), "Placeholder")

if __name__ == "__main__":
    out = Path(__file__).resolve().parent.parent / "content" / "clips" / "humanoid.gltf"
    write(out, RIG, blocks, MATERIALS, clips, "examples/37-character/tools/make_clips.py")
    print("wrote", out, "-", len(RIG.names), "joints,", ", ".join(clip["name"] for clip in clips))
