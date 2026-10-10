"""Writes `content/scenes/character.scene.json`: the floor, the stairs up to a
platform and the ramp down from it, the camera, and the three heroes -- each a
`CharacterBody` with its model, its `AnimationPlayer`, its feet and its look --
as a scene on disk, so the example opens in the editor with all of it there.

Run it from this folder (`python tools/make_scene.py`) after changing it; the
file it writes is checked in.

**How a hero is put together** is the thing to read here (`character` below):

    CharacterBody          what walks: a capsule, not drawn
      Hero (MeshPart)      what is seen, held to the capsule by `HeroWeld`
        Feet               FootPlacement: the two ankles and the hips, by name
        Look               IKControl, LookAt: the head
        Eyes               Bone on the head: what OTHER heads look at
      HeroWeld
      Animation            AnimationPlayer with the graph

The player is the capsule's child, so the capsule is what its graph reads
`CharacterBody.*` from and whose attributes it reads `Attribute.*` from, and
the model under the capsule is what it moves.

**And what a hero wears and holds** (`rogue_wears` below): a mesh that names
the body in `PoseFrom` is posed from the body's pose and from nothing else.
The head is such a piece -- a file with the body's skeleton and one mesh --
and so is the crossbow, which has none of the body's joints and hangs from a
`Bone` on the body's hand.
"""

import json
import math
from pathlib import Path

HEROES = json.loads((Path(__file__).resolve().parent / "heroes.json").read_text(encoding="utf-8"))

X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


# --- quaternions (x, y, z, w) ------------------------------------------------
def turn(axis, degrees):
    """A turn of `degrees` about `axis`, right-handed."""
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def mul(a, b):
    """`b`, and then `a`."""
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


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    size = math.sqrt(sum(c * c for c in v))
    return (v[0] / size, v[1] / size, v[2] / size)


def arc(start, end):
    """The shortest turn that takes the direction `start` to `end`."""
    a, b = unit(start), unit(end)
    axis = cross(a, b)
    q = (axis[0], axis[1], axis[2], 1.0 + sum(a[i] * b[i] for i in range(3)))
    size = math.sqrt(sum(c * c for c in q))
    return tuple(c / size for c in q)


GRAPH = "asset://anim/hero.animgraph.json"

# --- the level ---------------------------------------------------------------
# Five steps of fifteen centimetres up to a platform at ninety, and a ramp down
# its far end. `src/server/init.luau` walks the route along it.
RISE, TREAD, STEPS = 0.15, 0.45, 5
LANE, WIDTH = -4.0, 3.0
STAIRS_FROM = -6.0
PLATFORM_FROM = STAIRS_FROM + STEPS * TREAD
PLATFORM_TO = PLATFORM_FROM + 4.5
PLATFORM_HIGH = RISE * (STEPS + 1)
RAMP_TO = PLATFORM_TO + 4.8

STONE = [0.55, 0.56, 0.60]
BOARD = [0.60, 0.47, 0.33]


def frame(at, turned=(0.0, 0.0, 0.0, 1.0)):
    """A CFrame as a scene writes one: where, then its right, up and back."""
    return [*at, *rotate(turned, X), *rotate(turned, Y), *rotate(turned, Z)]


def facing(towards):
    """Turned about the vertical so that its front -- a part's -Z -- is
    towards (x, z)."""
    return turn(Y, math.degrees(math.atan2(-towards[0], -towards[1])))


def block(name, low, high, colour):
    return {
        "class": "Part",
        "name": name,
        "properties": {
            "CFrame": frame([(low[i] + high[i]) / 2 for i in range(3)]),
            "Size": [high[i] - low[i] for i in range(3)],
            "Anchored": True,
            "MaterialParameters": {"Color": colour},
        },
    }


level = [block("Floor", (-30.0, -1.0, -30.0), (30.0, 0.0, 30.0), [0.36, 0.42, 0.36])]
for step in range(STEPS):
    x = STAIRS_FROM + step * TREAD
    level.append(block(f"Step{step + 1}", (x, 0.0, LANE - WIDTH / 2), (x + TREAD, RISE * (step + 1), LANE + WIDTH / 2), STONE))
level.append(block("Platform", (PLATFORM_FROM, 0.0, LANE - WIDTH / 2), (PLATFORM_TO, PLATFORM_HIGH, LANE + WIDTH / 2), STONE))
# The ramp is a board laid from the platform's edge to the floor.
run = RAMP_TO - PLATFORM_TO
slope = math.atan2(PLATFORM_HIGH, run)
thick = 0.24
level.append(
    {
        "class": "Part",
        "name": "Ramp",
        "properties": {
            "CFrame": frame(
                (PLATFORM_TO + run / 2 - math.sin(slope) * thick / 2, PLATFORM_HIGH / 2 - math.cos(slope) * thick / 2, LANE),
                turn(Z, -math.degrees(slope)),
            ),
            "Size": [math.hypot(run, PLATFORM_HIGH), thick, WIDTH],
            "Anchored": True,
            "MaterialParameters": {"Color": BOARD},
        },
    }
)


# --- a hero --------------------------------------------------------------------
def character(name, model, at, towards, wide, speed, look_at, step_height, chain, more_on_mesh=(), more=()):
    hero = HEROES[model]
    high = hero["height"]
    turned = facing(towards)
    centre = (at[0], high / 2, at[1])
    # **A glTF character faces +Z and a part's front is -Z**, so the model is
    # held to the capsule turned half round, with its soles at the capsule's
    # foot. `FootPlacement` counts the ground from the model's own nought.
    seat = turn(Y, 180.0)
    return {
        "class": "CharacterBody",
        "name": name,
        "properties": {
            "CFrame": frame(centre, turned),
            "Size": [wide, high, wide],
            "WalkSpeed": speed,
            "JumpSpeed": 4.6,
            # A step of the stairs is walked over; anything taller is a wall.
            "AutoStepHeight": 0.3,
            # The capsule is not drawn: see-through altogether, which casts no
            # shadow either. How see-through a part is, is its material's.
            "MaterialParameters": {"Transparency": 1.0},
        },
        "children": [
            {
                "class": "MeshPart",
                "name": "Hero",
                "properties": {
                    "MeshContent": f"asset://models/{model}.glb",
                    "CFrame": frame((at[0], 0.0, at[1]), mul(turned, seat)),
                    "CanCollide": False,
                },
                "children": [
                    {
                        "class": "FootPlacement",
                        "name": "Feet",
                        "properties": {
                            "LeftFoot": hero["feet"][0],
                            "RightFoot": hero["feet"][1],
                            "Hips": hero["hips"],
                            # From this model's ankle joint to its sole.
                            "FootHeight": hero["ankle"],
                            "StepHeight": step_height,
                        },
                    },
                    {
                        "class": "IKControl",
                        "name": "Look",
                        "properties": {
                            "Type": "LookAt",
                            "EndJoint": hero["head"],
                            "Target": look_at,
                            "ChainLength": chain,
                            "MaxAngle": 75.0,
                            "Smoothing": 0.25,
                        },
                    },
                    {
                        # Between the eyes: a little up the head and in front
                        # of it, in the head joint's own space.
                        "class": "Bone",
                        "name": "Eyes",
                        "properties": {"JointName": hero["head"], "CFrame": frame(hero["eyes"])},
                    },
                    *more_on_mesh,
                ],
            },
            {
                "class": "Weld",
                "name": "HeroWeld",
                "properties": {
                    "Part0": f"Workspace.{name}",
                    "Part1": f"Workspace.{name}.Hero",
                    "C0": frame((0.0, -high / 2, 0.0), seat),
                },
            },
            {"class": "AnimationPlayer", "name": "Animation", "properties": {"Graph": GRAPH}},
            *more,
        ],
    }


# --- what the rogue wears and holds ----------------------------------------------
# **A piece worn**: a mesh whose `PoseFrom` is the body. It is a file with the
# body's skeleton and one mesh -- a head with its hood up -- and it is on the
# body joint for joint, whatever moved the joint: a clip, the feet finding a
# stair, the look turning the head. `src/server/init.luau` takes it off and
# puts the other head on while the game runs.
#
# **A thing held**: the crossbow has two joints, `Crossbow` and `String`, and
# none of the body's. Under a `Bone` of the body it hangs from that joint --
# the pack's own hand slot, where its own crossbow hangs -- and `String` is
# moved by the track of that name in whatever clip the body is playing: the
# shot lets it go and the reload draws it back. Handing it to the other hand
# is parenting it under the other hand's bone, which is turned half round
# about the slot's own Z so that it points away from the body on that side too.
rogue = HEROES["rogue"]
rogue_on_mesh = [
    {
        "class": "Bone",
        "name": "RightHold",
        "properties": {"JointName": rogue["slots"][1]},
        "children": [
            {
                "class": "MeshPart",
                "name": "Crossbow",
                "properties": {
                    "MeshContent": "asset://models/crossbow.glb",
                    "PoseFrom": "Workspace.Rogue.Hero",
                    "CanCollide": False,
                },
            }
        ],
    },
    {
        "class": "Bone",
        "name": "LeftHold",
        "properties": {"JointName": rogue["slots"][0], "CFrame": frame((0.0, 0.0, 0.0), turn(Z, 180.0))},
    },
]
rogue_wears = [
    {
        "class": "MeshPart",
        "name": "Head",
        "properties": {
            "MeshContent": "asset://models/rogue_hood.glb",
            "PoseFrom": "Workspace.Rogue.Hero",
            "CanCollide": False,
        },
    },
]

# --- the staff -------------------------------------------------------------------
# A staff in the skeleton's right hand, with a place on it for the left.
#
# The RIGHT hand is the clip's: the staff is welded to a `Bone` on the pack's
# hand slot -- the joint its characters hold things by, with its Y along
# whatever is held -- and goes wherever the arm swings. The LEFT hand is the
# staff's: an `IKControl` puts it on the `Grip` attachment, further up,
# whatever the clip had that arm doing. A part is long along its Y, so the
# staff is in the hand as the pack's own staff would be, with nothing turned.
STAFF_LONG = 1.6
HELD_AT = -0.25  # where along the staff the right hand is, from its middle
HANDS_APART = 0.45

skeleton = HEROES["skeleton"]
staff_on_mesh = [
    {
        "class": "Bone",
        "name": "RightGrip",
        "properties": {"JointName": skeleton["slots"][1]},
    },
    {
        "class": "IKControl",
        "name": "LeftHandOnStaff",
        "properties": {
            "Type": "TwoBone",
            "EndJoint": skeleton["hands"][0],
            "Target": "Workspace.Skeleton.Staff.Grip",
            # Which way the elbow goes: out to the side. With none, a limb
            # bends the way its clip has it bent, and this arm's clip never
            # meant it to reach across the chest.
            "Pole": "Workspace.Skeleton.LeftElbow",
            # A hand on something the body carries follows it at once: eased,
            # it would trail the staff by however far the body walks meanwhile.
            "Smoothing": 0.0,
        },
    },
]
staff = [
    {
        "class": "Part",
        "name": "Staff",
        "properties": {
            "CFrame": frame((-7.5, 1.0, 0.5)),
            "Size": [0.05, STAFF_LONG, 0.05],
            "CanCollide": False,
            "MaterialParameters": {"Color": [0.42, 0.29, 0.16]},
        },
        "children": [
            {"class": "Attachment", "name": "Grip", "properties": {"CFrame": frame((0.0, HELD_AT + HANDS_APART, 0.0))}}
        ],
    },
    {
        # Out to the body's left and a little low, in the capsule's own
        # space: its left is -X.
        "class": "Attachment",
        "name": "LeftElbow",
        "properties": {"CFrame": frame((-1.2, -0.3, 0.0))},
    },
    {
        "class": "Weld",
        "name": "StaffWeld",
        "properties": {
            "Part0": "Workspace.Skeleton.Hero.RightGrip",
            "Part1": "Workspace.Skeleton.Staff",
            "C1": frame((0.0, HELD_AT, 0.0)),
        },
    },
]

# --- the scene -------------------------------------------------------------------
PLAYER_AT = (0.0, 4.0)
MANNEQUIN_AT = (-6.6, -1.4)
EYE, TARGET = (0.0, 3.26, 9.0), (0.0, 1.56, 4.0)
looking = unit(tuple(EYE[i] - TARGET[i] for i in range(3)))
right = unit(cross(Y, looking))

scene = {
    "format": "scene",
    "version": 2,
    "root": {
        "class": "Workspace",
        "name": "Workspace",
        "properties": {"CurrentCamera": "Workspace.MainCamera"},
        "children": [
            {
                # Where the client's script first has it; it follows the
                # player from then on.
                "class": "Camera",
                "name": "MainCamera",
                "properties": {"CFrame": [*EYE, *right, *cross(looking, right), *looking], "FieldOfView": 60.0},
            },
            *level,
            # The player's: walked by input, a crossbow in its hand and a
            # hood on its head. The server points its look at whichever of
            # the others is nearer. As fast as the library's run.
            character(
                "Rogue",
                "rogue",
                PLAYER_AT,
                (0.0, -1.0),
                0.6,
                2.38,
                "Workspace.Skeleton.Hero.Eyes",
                0.3,
                1,
                more_on_mesh=rogue_on_mesh,
                more=rogue_wears,
            ),
            # Walks the stairs, the platform and the ramp for ever, a staff
            # in its hands, watching the player. Its legs are longer than the
            # library's, and its walk is the library's walk at that much more
            # of a pace.
            character(
                "Skeleton",
                "skeleton",
                (-7.5, 0.5),
                (0.0, -1.0),
                0.55,
                0.62,
                "Workspace.Rogue.Hero.Eyes",
                0.3,
                1,
                more_on_mesh=staff_on_mesh,
                more=staff,
            ),
            # Stands by the foot of the stairs and watches the player, its
            # neck and its head sharing the turn: another pack's body, with
            # other names for its joints and another rest.
            character(
                "Mannequin",
                "mannequin",
                MANNEQUIN_AT,
                (PLAYER_AT[0] - MANNEQUIN_AT[0], PLAYER_AT[1] - MANNEQUIN_AT[1]),
                0.5,
                1.0,
                "Workspace.Rogue.Hero.Eyes",
                0.15,
                2,
            ),
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "character.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8", newline="\n")
print("wrote", out)
