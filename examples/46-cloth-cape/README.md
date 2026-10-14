# 46 — Cloth cape

A cape that is cloth (ADR 0214): a mesh of its own on each figure, worn over
the body's pose (`PoseFrom`), and a `Cloth` that hangs it from a joint on the
shoulders. Every vertex of it moves: it folds, it fills with the wind, it rests
on the body's back and on the wall behind the third figure.

Run it with `run.bat`, or `engine-host examples/46-cloth-cape`.

- **The runner** runs a circle, stops dead, dashes, and is put somewhere else
  in one frame: the cape streams behind the run, swings past the body on the
  stop, and hangs at rest where the figure lands.
- **The one in the wind** stands facing it, and the cape fills.
- **The one at the wall** has its back to a wall and the wind blows its cape
  onto it: the cape lies on the wall and does not go through.

The figures, their capes, the wall and the wind are a scene
(`content/scenes/cloth.scene.json`, written by `tools/make_scene.py`): open the
project in the editor and the capes are there, hanging. The script only moves
the runner.

The models are drawn by `tools/make_models.py` and checked in. The cape is the
example of how to lay one out for cloth: modelled hanging straight down behind
the body, its top row weighted to the `CapePin` joint the `Cloth` pins, the
rest to the body.

Cloth is off at the Low quality level, where the capes hang as modelled. See
the manual's "Cloth".
