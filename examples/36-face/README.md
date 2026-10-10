# 36 — Face

Faces that talk, smile and blink (ADR 0196): three busts of one model whose
face has five shape keys -- `Blink`, `Smile`, `JawOpen`, `BrowUp`, `BrowDown`.

- The one on the left **talks**, and no script moves its jaw: its clip keys
  the weights, and `AnimationPlayer:LoadAnimation("Talk")` plays them. In a
  multiplayer game this is the way to have every player see it: each
  machine's client script plays the clip, and each works the same weights
  out of it.
- The one in the middle is moved by the **script**, with
  `MeshPart:SetMorphWeight`: a mood that comes and goes, and a blink every few
  seconds. What a script sets stays on the machine the script ran on.
- The one on the right does nothing, and costs what a model without shape
  keys costs.

Run it with `run.bat`, or `engine-host examples/36-face`.

The busts and their stands are a scene (`content/scenes/face.scene.json`,
written by `tools/make_scene.py`): open the project in the editor, select a
bust, and its keys are sliders under **Shape keys** in Properties -- a
preview, not saved with the scene.

`tools/make_head.py` draws the model, and is the example of how to lay a face
out: one mesh with a skeleton and a material a part (skin, the whites of the
eyes, what is dark, the mouth), each key moving a few dozen vertices, and the
clip that talks keying weights beside the head's own nod.

See [Faces and shape keys](../../docs/manual/animation/morph-targets.md).
