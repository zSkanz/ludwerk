"""Writes `content/scenes/meeting.scene.json`: a room with a lamp and a door,
two speakers on their stands, the game's own camera -- and the
`SequencePlayer` with a binding for each name its sequence expects, so the
example opens in the editor with the player to select and the Timeline to
open on it.

Run it from this folder (`python tools/make_scene.py`) after changing it; the
file it writes is checked in.
"""

import json
import math
from pathlib import Path


def cframe(x, y, z):
    return [x, y, z, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]


def look_at(eye, target):
    back = [eye[i] - target[i] for i in range(3)]
    size = sum(c * c for c in back) ** 0.5
    back = [c / size for c in back]
    right = [back[2], 0.0, -back[0]]
    size = sum(c * c for c in right) ** 0.5
    right = [c / size for c in right]
    up = [
        back[1] * right[2] - back[2] * right[1],
        back[2] * right[0] - back[0] * right[2],
        back[0] * right[1] - back[1] * right[0],
    ]
    return list(eye) + right + up + back


STAND = 1.0
# Where the game's own camera stands: what the picture is before the
# sequence takes the view, and the moment it gives it back.
EYE = (0.0, 2.6, 7.5)
WALL = [0.62, 0.60, 0.56]
FLOOR = [0.36, 0.30, 0.26]


def painted(colour):
    """A part's colour is a parameter of its material."""
    return {"MaterialParameters": {"Color": colour}}


def facing(x, y, z, towards):
    """At (x, y, z), turned about the vertical to face `towards`."""
    yaw = math.atan2(towards[0] - x, towards[1] - z)
    c, s = math.cos(yaw), math.sin(yaw)
    return [x, y, z, c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c]


def speaker(name, x, towards):
    """One speaker: a model holding a stand and a bust. A `LipSync` follows
    the voice said under its mesh's parent, so each speaker is a model of its
    own -- and the model is what the sequence's binding names."""
    return {
        "class": "Model",
        "name": name,
        "properties": {},
        "children": [
            {
                "class": "Part",
                "name": "Stand",
                "properties": {"CFrame": cframe(x, STAND / 2, 0.0), "Size": [0.9, STAND, 0.6], "Anchored": True},
            },
            {
                "class": "MeshPart",
                "name": "Bust",
                "properties": {
                    "MeshContent": "asset://models/head.gltf",
                    "CFrame": facing(x, STAND, 0.0, towards),
                    "Anchored": True,
                },
                "children": [
                    {"class": "AnimationPlayer", "name": "Animation", "properties": {}},
                    {"class": "LipSync", "name": "Mouth", "properties": {"Mode": "Auto"}},
                    # Where a camera that looks at this speaker looks: the
                    # face, and not the middle of a model that is mostly stand.
                    {"class": "Attachment", "name": "Face", "properties": {"CFrame": cframe(0.0, 0.82, 0.1)}},
                ],
            },
        ],
    }


def wall(name, x, y, z, sx, sy, sz):
    return {
        "class": "Part",
        "name": name,
        "properties": {"CFrame": cframe(x, y, z), "Size": [sx, sy, sz], "Anchored": True, **painted(WALL)},
    }


def binding(name, target):
    return {"class": "SequenceBinding", "name": name, "properties": {"Target": target}}


scene = {
    "format": "scene",
    "version": 2,
    "root": {
        "class": "Workspace",
        "name": "Workspace",
        "properties": {"CurrentCamera": "Workspace.MainCamera"},
        "children": [
            {
                "class": "Camera",
                "name": "MainCamera",
                "properties": {"CFrame": look_at(EYE, (0.0, 1.5, 0.0)), "FieldOfView": 50.0},
            },
            {
                "class": "Part",
                "name": "Floor",
                "properties": {
                    "CFrame": cframe(0.0, -0.5, 0.0),
                    "Size": [14.0, 1.0, 14.0],
                    "Anchored": True,
                    **painted(FLOOR),
                },
            },
            # The back wall, in three pieces around the doorway the door shuts.
            wall("WallLeft", -3.8, 1.75, -3.1, 6.4, 3.5, 0.2),
            wall("WallRight", 3.8, 1.75, -3.1, 6.4, 3.5, 0.2),
            wall("WallOver", 0.0, 2.85, -3.1, 1.2, 1.3, 0.2),
            {
                "class": "Part",
                "name": "Door",
                # Shut: in the doorway. The sequence keys its `CFrame` to
                # swing it about its left edge.
                "properties": {
                    "CFrame": cframe(0.0, 1.1, -3.0),
                    "Size": [1.2, 2.2, 0.08],
                    "Anchored": True,
                    **painted([0.42, 0.26, 0.16]),
                },
            },
            {
                "class": "Part",
                "name": "Lamp",
                "properties": {
                    "CFrame": cframe(0.0, 2.9, 0.4),
                    "Size": [0.3, 0.3, 0.3],
                    "Anchored": True,
                    **painted([1.0, 0.86, 0.6]),
                    "Shape": "Ball",
                },
                "children": [
                    {
                        "class": "PointLight",
                        "name": "Light",
                        "properties": {"Brightness": 0.5, "Range": 14.0, "Color": [1.0, 0.72, 0.42]},
                    }
                ],
            },
            speaker("Anna", -1.0, (1.0, 0.6)),
            speaker("Bruno", 1.0, (-1.0, 0.6)),
            {
                "class": "SequencePlayer",
                "name": "Meeting",
                "properties": {"Sequence": "asset://sequences/meeting.sequence.json", "Controls": "Suspend"},
                "children": [
                    binding("Anna", "Workspace.Anna"),
                    binding("Bruno", "Workspace.Bruno"),
                    binding("Lamp", "Workspace.Lamp"),
                    binding("Door", "Workspace.Door"),
                ],
            },
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "meeting.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8", newline="\n")
print("wrote", out)
