"""Writes `content/scenes/face.scene.json`: the floor, the camera and three
busts on their stands, as a scene on disk -- so the example opens in the
editor with the busts there, and their shape keys under Properties.

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


def bust(name, x):
    return [
        {
            "class": "Part",
            "name": f"{name}Stand",
            "properties": {"CFrame": cframe(x, STAND / 2, 0.0), "Size": [0.9, STAND, 0.6], "Anchored": True},
        },
        {
            "class": "MeshPart",
            "name": name,
            "properties": {
                "MeshContent": "asset://models/head.gltf",
                "CFrame": facing(x, STAND, 0.0),
                "Anchored": True,
            },
            # One player a bust: the clips are the model's own.
            "children": [{"class": "AnimationPlayer", "name": "Animation", "properties": {}}],
        },
    ]


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
            # A clip's weights, a script's weights, and nobody's.
            *bust("Talker", -1.1),
            *bust("Actor", 0.0),
            *bust("Still", 1.1),
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "face.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8")
print("wrote", out)
