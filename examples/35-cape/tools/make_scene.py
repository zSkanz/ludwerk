"""Writes `content/scenes/cape.scene.json`: the floor, the camera and the two
caped figures, as a scene on disk -- so the example opens in the editor with
its capes already on, and not only when its script has run.

Run it from this folder (`python tools/make_scene.py`) after changing it; the
file it writes is checked in.
"""

import json
from pathlib import Path


def cframe(x, y, z):
    return [x, y, z, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]


def figure(name, at):
    return {
        "class": "MeshPart",
        "name": name,
        "properties": {
            "MeshContent": "asset://models/figure.gltf",
            "CFrame": cframe(*at),
            "Anchored": True,
        },
        "children": [
            {
                # One instance for the cape: the pattern is the top joint of
                # each column, and each is a chain of its own.
                "class": "SpringBone",
                "name": "Cape",
                "properties": {
                    "JointPattern": "Cape_*0",
                    "Stiffness": 0.12,
                    "Damping": 0.18,
                    "LimitAngle": 80.0,
                    "Radius": 0.04,
                },
            },
            # **What the cape rests on.** The body is a block six tenths wide
            # and a third deep, and a capsule is round: one down the middle
            # covers the middle column of the cape and lets the two outer ones
            # swing straight through the block's corners -- which is what the
            # first version of this example did. Three, side by side, as deep
            # as the block is, cover its whole back; a rounder character needs
            # one for the torso and one a leg.
            *[
                {
                    "class": "SpringCollider",
                    "name": name,
                    "properties": {
                        "JointName": "Body",
                        "Offset": [x, 0.17, 0.0],
                        "Length": 1.38,
                        "Radius": 0.17,
                    },
                }
                for name, x in (("BackLeft", -0.2), ("Back", 0.0), ("BackRight", 0.2))
            ],
        ],
    }


# The camera of the example's script, looking at the same place.
eye, target = (1.5, 2.6, 8.5), (1.0, 1.0, -1.5)
back = [eye[i] - target[i] for i in range(3)]
size = sum(c * c for c in back) ** 0.5
back = [c / size for c in back]
right = [back[2], 0.0, -back[0]]
size = sum(c * c for c in right) ** 0.5
right = [c / size for c in right]
up = [back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0]]

scene = {
    "format": "scene",
    "version": 2,
    "root": {
        "class": "Workspace",
        "name": "Workspace",
        "properties": {
            "CurrentCamera": "Workspace.MainCamera",
            # A wind from the figures' left, with gusts in it: what moves a
            # cape on a figure that stands still.
            "GlobalWind": [5.0, 0.0, 1.0],
            "WindGusts": 0.6,
        },
        "children": [
            {
                "class": "Camera",
                "name": "MainCamera",
                "properties": {"CFrame": list(eye) + right + up + back, "FieldOfView": 55.0},
            },
            {
                "class": "Part",
                "name": "Floor",
                "properties": {"CFrame": cframe(0.0, -0.5, 0.0), "Size": [80.0, 1.0, 80.0], "Anchored": True},
            },
            figure("Figure", (0.0, 0.0, 0.0)),
            figure("InTheWind", (-3.0, 0.0, -2.0)),
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "cape.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8")
print("wrote", out)
