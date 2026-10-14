"""Writes `content/scenes/booth.scene.json`: the floor, the camera and three
speakers on their stands, as a scene on disk -- so the example opens in the
editor with them, and each speaker's `LipSync` under Properties.

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
EYE = (0.0, 1.95, 3.1)


def facing(x, y, z):
    """At (x, y, z), turned about the vertical to face the camera: a bust seen
    from the side is a face half hidden by its own cheek."""
    yaw = math.atan2(EYE[0] - x, EYE[2] - z)
    c, s = math.cos(yaw), math.sin(yaw)
    # Columns: right, up, back -- the model's +z, its face, towards the eye.
    return [x, y, z, c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c]


def speaker(name, x, mode):
    """One speaker: a model holding a stand and a bust. A `LipSync` follows the
    voice said under its mesh's PARENT, so each speaker is a model of its own
    -- three busts straight under the workspace would all move to one voice."""
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
                    "CFrame": facing(x, STAND, 0.0),
                    "Anchored": True,
                },
                "children": [
                    {"class": "AnimationPlayer", "name": "Animation", "properties": {}},
                    # The whole of a speaker's mouth: which way it is moved.
                    # `Auto` is the best there is -- the line's track when the
                    # recording has one -- and the other two say which of the
                    # lesser ways to use, as a face with no track would.
                    {"class": "LipSync", "name": "Mouth", "properties": {"Mode": mode}},
                ],
            },
        ],
    }


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
                "properties": {"CFrame": look_at(EYE, (0.0, 1.72, 0.0)), "FieldOfView": 40.0},
            },
            {
                "class": "Part",
                "name": "Floor",
                "properties": {"CFrame": cframe(0.0, -0.5, 0.0), "Size": [40.0, 1.0, 40.0], "Anchored": True},
            },
            speaker("Anna", -1.1, "Auto"),
            speaker("Bruno", 0.0, "Bands"),
            speaker("Clara", 1.1, "Loudness"),
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "booth.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8", newline="\n")
print("wrote", out)
