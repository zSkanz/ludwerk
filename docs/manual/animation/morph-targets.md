# Faces and shape keys

A face is not bones. A blink, a smile, an open jaw, a raised brow are the same
mesh in another shape; an artist makes each as a **shape key** (a *morph
target*, in glTF's words) and gives it a weight: nought is the mesh at rest,
one is that shape, and several add.

A model's shape keys come in with it. A clip that animates their weights
plays them, a script sets them by name, and the model is drawn in the shape
they add up to -- lit, in the depth pass and in its shadow.

`examples/36-face` is three busts of one model: one talks by its clip, one
smiles, frowns and blinks by a script, one is at rest.

## In the editor

Select a `MeshPart` whose model has shape keys: **Shape keys**, under its
properties, has a slider for each, and the viewport follows as you drag. A
slider runs from nought to one; hold Ctrl and click it to type any number.
The small button beside it gives that key back to the clips and the file.

**It is a preview.** A weight is not saved with the scene and is not undone:
what a face does in the game is its clips' and its scripts' to say. A face
that should REST in a smile is a model exported with that key's value above
nought, which is kept.

## A blink from a script

```luau
local face = workspace.Hero.Face :: MeshPart

print(face:GetMorphTargets()) --> { "Smile", "Blink", "JawOpen", ... }

face:SetMorphWeight("Blink", 1)
task.wait(0.12)
face:ClearMorphWeight("Blink") -- back to whatever the clips say of it
```

- `GetMorphTargets()` is the names, in the file's order. **Empty until the
  mesh has loaded**: ask after `ContentProvider:PreloadAsync`, or do not ask
  -- a weight set by name on a mesh that has not arrived is kept and takes
  effect when it does.
- `SetMorphWeight(name, weight)` takes any number. Nought is rest and one is
  the shape, and a file's own weights go past one and below nought, so yours
  may. It raises for a name the mesh does not have, once the mesh is there to
  say so.
- `GetMorphWeight(name)` is what the part is drawn with on this machine.
- `ClearMorphWeight(name)` gives the target back.

**Who wins.** For each target: what a script set, while it is set; else what
the clips playing on the mesh make of it; else the weight its file gives it.
A script's value is not blended with the clip's -- it replaces it, every
frame, until it is cleared.

## Which machine a weight lives on

**A weight is picture, not simulation.** It is not in the world's state: not
replicated, not in a replay, not in a save.

- **What a script sets stays on the machine that script ran on.** Set it in a
  client script, where the part is drawn. A server's script that calls
  `SetMorphWeight` moves a face nobody is looking at, and the script check
  says so (`SetMorphWeight is a player's: this script runs on the server`).
- **What a clip plays is seen where the clip is played.** An animation is
  not replicated either: a track plays on the machine whose script called
  `Play`. A face that every player must see talking is played by a client
  script, on every machine, as a character's walk is -- told to by something
  that IS replicated: an attribute, a `CharacterBody`'s state, a
  `RemoteEvent`. The clip is the better carrier of the two, because every
  machine that plays it works the same weights out of it; a script's weights
  would have to be sent a number at a time.

## Weights in a clip

A glTF animation that keys shape key values comes in as part of the clip of
that name, beside whatever it does to the joints, and plays with it:
`AnimationPlayer:LoadAnimation("Talk")` and `Play` as for any clip.

- A clip **fading in eases its weights in** from the file's own, and one
  fading out eases them back.
- Two clips on the same target are averaged by their track weights, as two
  clips on a joint are.
- A target a clip says nothing of keeps the file's weight.
- One `AnimationPlayer` on a `Model` plays a clip's weights on every mesh
  under it that has those targets -- a body and a face exported as two meshes
  of one file.
- A clip from **another file** moves a mesh's targets by their names, so a
  library of expressions can serve every head that names its keys the same.

## Making them

In Blender, shape keys on the mesh; export as glTF with **Shape Keys** ticked
(and **Shape Key Normals**, if the shapes change how the surface catches the
light). Their names come through as you gave them.

- **Give the face a material of its own.** A model is drawn a material at a
  time, and only the parts a shape key reaches are drawn the costly way while
  one is above nought. A character whose face shares a material with its
  whole body pays for the whole body.
- **Keep the keys on the face.** What the graphics card holds is sixteen bytes
  for each vertex between the first and the last any key moves, for each key:
  fifty keys over a face of three thousand vertices is 2.4 MB, and the same
  fifty over a body of forty thousand is 32 MB. Past 128 MB a model is drawn
  without its keys, and the log says so.
- **A level of detail keeps its keys**, since a coarser level is fewer
  triangles over the same vertices -- but a simplifier is free to drop the
  very vertices a small key moves. Look at a face's far levels with its keys
  on.

## What it costs

- A model with shape keys **all at nought** costs what it would without them.
  Four hundred skinned bodies with three keys each, none in use: the same
  eleven draws and the same frame as four hundred with no keys (measured,
  1.21 ms against 1.22).
- A body with **any key above nought** is drawn on its own, out of the run it
  would share with its neighbours, in every pass: about eight microseconds a
  body a frame on the development build. The same four hundred all moving at
  once are 1,712 draws and 4.2 ms where they were eleven and 1.2. A handful
  of faces talking is nothing; a crowd of two hundred all talking is not what
  this stage is for.
- The moment a body's keys are all back at nought it is in its run again.
- **The first blink does not hitch.** What draws a morphing body is made when
  a model that has shape keys is loaded -- behind the loading screen, or by
  `ContentProvider:PreloadAsync` -- and not on the frame a weight first
  leaves nought.
- At most **eight keys move a body at once**. With more above nought, the
  eight largest are drawn and the rest wait.

## What it does not do yet

- A selection's outline and a highlight are drawn round the mesh at rest.
- Motion blur and the temporal upscalers see the body and its skeleton move,
  not its keys: fine for a face, visible on a key that throws a limb.
- A mesh with shape keys is drawn with the built-in surface, as a skinned one
  is: a surface shader on its material is not used.
- A key's own tangents are not kept; the mesh's are used.
- **What the renderer culls by is the mesh at rest, grown to where each key
  alone can take it at a weight of one and of minus one.** A weight outside
  that, or several large keys pushing the same way, can take a vertex past
  those bounds -- and then the mesh can vanish at the edge of the screen
  while part of it is still in view. Keep big movements in the skeleton.

## Where to look next

- [Skeletal animation](manual:animation/skeletal)
- [Capes, tails and hair](manual:animation/secondary-motion)
- [`MeshPart`](api:MeshPart) · [`AnimationPlayer`](api:AnimationPlayer)
