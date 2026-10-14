"""Writes `content/scenes/cloth.scene.json`: the floor, the camera, a wall and
three caped figures, as a scene on disk -- so the example opens in the editor
with its capes already hanging, and not only when its script has run.

Run it from this folder (`python tools/make_scene.py`) after changing it; the
file it writes is checked in.
"""

import json
import math
from pathlib import Path


def cframe(x, y, z, yaw=0.0):
    # Turned `yaw` radians about up: a figure facing -Z turned to face along
    # (-sin yaw, 0, -cos yaw).
    c, s = math.cos(yaw), math.sin(yaw)
    return [x, y, z, c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c]


def figure(name, at, yaw=0.0):
    body = f"Workspace.{name}.Body"
    return {
        "class": "Model",
        "name": name,
        "children": [
            {
                "class": "MeshPart",
                "name": "Body",
                "properties": {
                    "MeshContent": "asset://models/figure.gltf",
                    "CFrame": cframe(*at, yaw),
                    "Anchored": True,
                },
                "children": [
                    # **What the cape rests on**: three capsules side by side
                    # down the body's back, as deep as the block is -- a
                    # capsule is round and the body is not.
                    {
                        "class": "SpringCollider",
                        "name": collider,
                        "properties": {
                            "JointName": "Body",
                            "Offset": [x, 0.17, 0.0],
                            "Length": 1.38,
                            "Radius": 0.17,
                        },
                    }
                    for collider, x in (("BackLeft", -0.2), ("Back", 0.0), ("BackRight", 0.2))
                ],
            },
            {
                # The cape: a mesh of its own that wears the body's pose, and
                # the `Cloth` that hangs it from the joint on the shoulders.
                "class": "MeshPart",
                "name": "Cape",
                "properties": {
                    "MeshContent": "asset://models/cape.gltf",
                    "CFrame": cframe(*at, yaw),
                    "Anchored": True,
                    "CanCollide": False,
                    "PoseFrom": body,
                },
                "children": [
                    {
                        "class": "Cloth",
                        "name": "Cloth",
                        "properties": {
                            "PinJoints": "CapePin",
                            "MaxDistance": 1.4,
                            "Stiffness": 0.95,
                            "BendStiffness": 0.15,
                            "Damping": 0.15,
                            "Thickness": 0.02,
                        },
                    }
                ],
            },
        ],
    }


# The camera of the example's script, looking at the same place.
eye, target = (0.6, 2.0, 4.6), (0.4, 1.0, -2.0)
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
            # cape on a figure that stands still, and what presses the third
            # one's cape against the wall behind it.
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
            # The wall the third figure stands with its back to, the wind
            # blowing its cape onto it.
            {
                "class": "Part",
                "name": "Wall",
                "properties": {
                    "CFrame": cframe(4.6, 1.5, -2.0),
                    "Size": [0.3, 3.0, 2.4],
                    "Anchored": True,
                    "MaterialParameters": {"Color": [0.62, 0.6, 0.55]},
                },
            },
            figure("Runner", (0.0, 0.0, 0.0)),
            figure("InTheWind", (-3.0, 0.0, -2.0), -math.pi / 2),
            # Facing -X, its back -- and its cape -- toward the wall at +X.
            figure("AtTheWall", (4.05, 0.0, -2.0), -math.pi / 2),
        ],
    },
}

out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "cloth.scene.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(scene, indent=1) + "\n", encoding="utf-8")
print("wrote", out)
