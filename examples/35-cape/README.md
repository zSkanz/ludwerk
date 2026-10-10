# 35 — Cape

A cape on a figure (ADR 0194): one `SpringBone` whose pattern is the top of
each of three columns of joints, one `SpringCollider` for the body they must
not pass through, and a figure
that does what a hero does to a cape -- runs a circle, stops dead, dashes, and
is put somewhere else in one frame -- beside one that stands in the wind.

Run it with `run.bat`, or `engine-host examples/35-cape`.

The figures, their capes and the wind are a scene
(`content/scenes/cape.scene.json`, written by `tools/make_scene.py`): open the
project in the editor and the capes are there, hanging in the wind. The script
only moves one of the figures.

The model is drawn by `tools/make_figure.py` and checked in. It is the example
of how to lay a cape out: each column is a chain of its own under a top joint
on the shoulders, and those top joints are what the pattern finds.

See the manual's "Capes, tails and hair".
