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
"""

import json
import math
from pathlib import Path

from make_heroes import HEROES
from rig import X, Y, Z, arc, conj, cross, mul, rotate, turn, unit

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
                    "MeshContent": f"asset://models/{model}.gltf",
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
                        "properties": {"JointName": hero["head"], "CFrame": frame((0.0, (high - hero["rig"].at[hero["rig"].index(hero["head"])][1]) * 0.6, 0.0))},
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


# --- the staff -------------------------------------------------------------------
# A staff in `Stocky`'s right hand, with a place on it for the left.
#
# The RIGHT hand is the clip's: the staff is welded to a `Bone` on it and goes
# wherever the arm swings. The LEFT hand is the staff's: an `IKControl` puts
# it on the `Grip` attachment, further up, whatever the clip had that arm
# doing. So the two numbers that matter are which way the staff lies in the
# right hand and how the grip is turned -- the left hand takes the grip's turn
# as its own (`AlignRotation`).
STAFF_LONG = 1.5
HELD_AT = -0.45  # where along the staff the right hand is, from its middle
HANDS_APART = 0.55
PALM = 0.07  # from a wrist joint to the middle of its hand

# Worked out for the arm as it hangs in a walk: the hand's joint turned from
# its rest by the arm's being lowered.
hanging = turn(Z, 78.0)
# Up, across the body and forwards, in the model's space...
lie = unit((0.55, 0.70, 0.45))
# ...which the weld wants in the hand joint's own. A part is long along its Y.
in_hand = arc(Y, rotate(conj(hanging), lie))
# The left hand lies across the staff -- the staff along the hand's own Z --
# with its fingers pointing on from the forearm that brings it.
forearm = unit((-0.75, 0.25, 0.35))
across = sum(forearm[i] * lie[i] for i in range(3))
fingers = unit(tuple(forearm[i] - across * lie[i] for i in range(3)))
palm = cross(lie, fingers)
staff_turned = mul(hanging, in_hand)
back = conj(staff_turned)
grip_frame = [0.0, HELD_AT + HANDS_APART, 0.0, *rotate(back, fingers), *rotate(back, palm), *rotate(back, lie)]

stocky_hands = HEROES["stocky"]["hands"]
staff_on_mesh = [
    {
        # The middle of the right hand, in the hand joint's own space: its
        # fingers run along -X.
        "class": "Bone",
        "name": "RightGrip",
        "properties": {"JointName": stocky_hands[1], "CFrame": frame((-PALM, 0.0, 0.0))},
    },
    {
        "class": "IKControl",
        "name": "LeftHandOnStaff",
        "properties": {
            "Type": "TwoBone",
            "EndJoint": stocky_hands[0],
            "Target": "Workspace.Stocky.Staff.Grip",
            # Which way the elbow goes: out to the side. With none, a limb
            # bends the way its clip has it bent, and this arm's clip never
            # meant it to reach across the chest.
            "Pole": "Workspace.Stocky.LeftElbow",
            # The wrist stops short of the grip by half a hand, so the hand's
            # middle is what lies on the staff.
            "TargetOffset": frame((-PALM, 0.0, 0.0)),
            "AlignRotation": True,
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
        "children": [{"class": "Attachment", "name": "Grip", "properties": {"CFrame": grip_frame}}],
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
            "Part0": "Workspace.Stocky.Hero.RightGrip",
            "Part1": "Workspace.Stocky.Staff",
            "C0": frame((0.0, 0.0, 0.0), in_hand),
            "C1": frame((0.0, HELD_AT, 0.0)),
        },
    },
]

# --- the scene -------------------------------------------------------------------
PLAYER_AT = (0.0, 4.0)
SMALL_AT = (-6.6, -1.4)
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
            # The player's: walked by input. The server points its look at
            # whichever of the others is nearer.
            character("Tall", "tall", PLAYER_AT, (0.0, -1.0), 0.5, 3.6, "Workspace.Stocky.Hero.Eyes", 0.3, 1),
            # Walks the stairs, the platform and the ramp for ever, a staff
            # in its hands, watching the player.
            character(
                "Stocky",
                "stocky",
                (-7.5, 0.5),
                (0.0, -1.0),
                0.7,
                1.1,
                "Workspace.Tall.Hero.Eyes",
                0.3,
                1,
                more_on_mesh=staff_on_mesh,
                more=staff,
            ),
            # Stands by the foot of the stairs and watches the player, its
            # back and its head sharing the turn.
            character(
                "Small",
                "small",
                SMALL_AT,
                (PLAYER_AT[0] - SMALL_AT[0], PLAYER_AT[1] - SMALL_AT[1]),
                0.4,
                1.0,
                "Workspace.Tall.Hero.Eyes",
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
