# Material graphs

A [surface shader](manual:rendering/surface-shaders) drawn as nodes. A **material graph**
is a file, `*.surfacegraph.json`, and what it does is write a surface shader
beside itself: `rust.surfacegraph.json` writes `rust.surface.hlsl`, and a
material names that file exactly as it names one you wrote by hand. Nothing
else knows there was a graph.

`examples/23-surfaces` has one: its dissolve, made again as a graph beside the
hand-written shader, and worn by the fourth block.

## It is one way

**The graph writes the shader; the shader is not read back.** It is written
again each time the graph is saved, and its first lines say so:

```hlsl
// Written by rust.surfacegraph.json -- edit the graph, not this file: it is written
// again whenever the graph is saved, and a change made here is lost. To keep
// the surface as code, delete the graph; this file is then yours.
```

That last sentence is the way out. The written file is meant to be read: a
statement a node, in the order they feed each other, each local named after
its node. Delete the graph and you have a surface shader like any other.

## Making one

In Content: right click, **New Surface Graph...**, and a name. It is written
as a graph that is a surface already -- a color a material can set, into the
surface's color -- with its shader beside it, and it opens.

A double click on a graph in Content opens it; so does `Ctrl+P` and its name.
Each open graph is a tab beside the Viewport, and can be dragged out to stand
next to the world.

Give a material the written shader, as you would any: the material panel's
**Shader** field, or `"shader": "asset://shaders/rust.surface.hlsl"` in the
material's file.

## The canvas

- **Move the view** with the middle button, or by dragging with the right
  one; the wheel zooms about the pointer. **Show all** (or `F`) fits every
  node.
- **Select** a node by pressing it; `Shift` or `Ctrl` adds to the selection;
  a drag from empty canvas boxes several. Drag a selected node to move what
  is selected.
- **Link** by dragging from a pin to a pin, either way round. An input has
  one source: a new link replaces the old. A press on a link selects it.
- **Add a node** from the list: a right press or the **space bar** on empty
  canvas, or **Add...**. Type a few letters, move with the arrows, `Enter`.
  **Let a link go over empty canvas** and the same list opens with only what
  has a pin that fits it, and what you choose is linked as it is added.
- `Delete` removes what is selected. `Ctrl+Z` and `Ctrl+Y` undo and redo --
  the graph's own, not the scene's. `Ctrl+C`, `Ctrl+V` and `Ctrl+D` copy,
  paste and duplicate; what is copied is text, a graph's own file, so it can
  be pasted into another graph or into a message. `Ctrl+S` saves.

Select a node and its fields are beside the canvas: its name -- which names
the local its statement makes -- and whatever its kind has.

## What a pin carries

A pin is a number, or a vector two, three or four wide, and is colored by
which. **A number goes into any width; the widths do not turn into each
other.** A link that cannot be made is not made, and says why where it would
have been.

An arithmetic node -- Add, Multiply, Lerp -- is as wide as the widest thing
put into it, and its output pin takes that color once something is.

## The nodes

**Inputs**, what a surface reads: UV 0 and UV 1, World position, World
normal, Vertex color, Time, Camera position, Screen UV, Wind; and, for the
vertex end only, Object position and Object normal.

**Values**:

- **Parameter** -- what a material sets, and what the material panel shows as
  a field. Its **Name**, its **Type** (number, a vector, color, toggle), its
  **Default**, and a **Range** (`0 1`) for a slider.
- **Constant** -- a value written into the shader.
- **Texture** -- a texture a material sets, sampled: its color and its alpha.
- **Normal map** -- a texture sampled and unpacked to a normal, for the
  surface's Normal.

**Math**: Add, Subtract, Multiply, Divide, Lerp, Saturate, One minus, Power,
Min, Max, Absolute, Step, Smoothstep, Sine, Fraction, Normalize, Dot product,
Length.

**Shape**: **Split** a vector into its parts; **Combine** parts into one.

**Patterns**: **Fresnel** (brighter at a glancing angle), **Panner** (a UV
moved by time), **Noise** (smooth noise, nought to one, from a position).

**Code**: **Expression** -- one line of HLSL, with up to four inputs called
`a`, `b`, `c` and `d` and the type it comes to. It is how a graph says what no
node says, without leaving the graph:

```hlsl
b > 0.0f ? 1.0f - saturate((a - b) / c) : 0.0f
```

**Ends**, one of each at most:

- **Surface** -- what the surface is: BaseColor, Alpha, Metallic, Roughness,
  Normal, Emissive. Whatever is not linked keeps the engine's default.
- **Vertex** -- moves a vertex before it is drawn and before it casts its
  shadow: an Offset in the object's space, and a Normal.

A node nothing uses writes nothing.

## When something is wrong

**Before the compiler is asked**, the graph says what it can see: an input
that has to be linked and is not, two widths that do not meet, a parameter
with no name, a value that is not numbers. The node is outlined, the words are
under it and in **What is wrong** beside the canvas. The shader is still
written -- with nought where that node would have been -- so the rest of the
surface still draws.

**When it is saved**, the shader is compiled at once. A compile error is put
on the node whose line it is, in the compiler's own words: every node is one
line of the written file, and the graph knows which.

A graph changed since it was saved says so: what the compiler last read is
the shader on disk.

## What it does not do

There are no functions (a group of nodes used as one), no comment boxes, and
no preview in the panel -- a graph's result is seen on what wears it. A
shader edited by hand is not read back into nodes.

## Where to look next

- [Surface shaders](manual:rendering/surface-shaders) -- the contract a graph writes against
- [Drawing an animation graph](manual:animation/graph-panel) -- the same canvas, for a character's clips
