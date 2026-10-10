# Faces and shape keys

A face is not bones. A blink, a smile, an open jaw, a raised brow are the same
mesh in another shape; an artist makes each as a **shape key** (a *morph
target*, in glTF's words) and gives it a weight: nought is the mesh at rest,
one is that shape, and several add.

**Where this stands.** A model's morph targets are imported and kept, and a
model is **drawn** in the shape its file's own weights give it -- lit, in the
depth pass and in its shadow. Setting a weight from a script, a clip that
animates weights, and sliders in the editor are the next stages (ADR 0196),
and this page grows with them.

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
- At most **eight keys move a body at once**. With more above nought, the
  eight largest are drawn and the rest wait.

## What it does not do yet

- A selection's outline and a highlight are drawn round the mesh at rest.
- Motion blur and the temporal upscalers see the body and its skeleton move,
  not its keys: fine for a face, visible on a key that throws a limb.
- A mesh with shape keys is drawn with the built-in surface, as a skinned one
  is: a surface shader on its material is not used.
- A key's own tangents are not kept; the mesh's are used.
- What the renderer culls by is the mesh at rest, grown to where each key
  alone can take it. Weights past one, or several keys pushing the same way,
  can take a vertex outside that.

## Where to look next

- [Skeletal animation](manual:animation/skeletal)
- [Capes, tails and hair](manual:animation/secondary-motion)
