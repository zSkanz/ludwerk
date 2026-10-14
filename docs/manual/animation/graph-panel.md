# Drawing an animation graph

An [animation graph](manual:animation/graphs) is a file, and the editor draws it: states as
boxes, transitions as arrows, a layer a page. What you edit is the
`*.animgraph.json` itself -- the canvas opens it and saves it, and there is no
second format. A graph you wrote by hand opens; a graph you drew can be read.

`examples/37-character` has one, `content/anim/hero.animgraph.json`.

## Opening one

A double click on a graph in Content, or `Ctrl+P` and its name. **New
Animation Graph...** in Content's menu writes one that is a graph already -- a
layer called Body with one state -- and opens it. Each open graph is a tab
beside the Viewport, and can be dragged out to stand next to the world.

## A page

**A layer is a page.** The bar over the canvas has a button for each, bottom
layer first, and `+` adds one.

**A state is a box**, with its name and what it plays under it. The state the
layer starts in has a mark down its left edge and the word *start*.

**A transition is an arrow**, and what it waits for is written at its middle:
`Grounded == false, Rise > 1`, or *at its end*. If there are several ways
from one state to another they are one arrow, and the fields beside the
canvas list each.

**From any state** is a box of its own, one a page: an arrow out of it is a
transition taken from wherever the layer is.

**A state nothing leads to looks it**: hollow, faint, its edge broken, and
*no way in* under its name. It is not an error -- a state you have just added
is one -- but a state left like that is never played.

## Editing

- **Add a state** from the list: a right press or the **space bar** on empty
  canvas, or **Add...**. The same list offers *From any state* where the page
  has none.
- **Draw an arrow** by dragging from a state's **edge** to another state. The
  edge lights under the pointer and the pointer becomes a hand: a press there
  draws an arrow out, and a press further in moves the state. **Let it go
  over empty canvas** and the list offers a new state for it to end in.
- **Move** a state by dragging it from its middle. Once one is moved by hand,
  the page's places are saved in the file.
- **Select** a state or an arrow to see its fields beside the canvas. With
  nothing selected, the fields are the layer's.
- `Delete` removes what is selected, with everything that named it: a
  state's arrows go with the state. `Ctrl+Z` and `Ctrl+Y` undo and redo --
  the graph's own, not the scene's. `Ctrl+C` and `Ctrl+V` copy and paste
  states and the arrows among them, as text; pasted into another graph, the
  parameters they read come with them by name. `Ctrl+S` saves.

**A state's fields**: its name; whether the layer starts here; what it plays
-- nothing (the layer under shows through), one clip, a blend along one
parameter, or a blend across two -- and then the clip, or the blend's
parameters and its clips with where each sits. A blend is drawn: along a line
for one parameter, on a plot with its triangles for two. *Loops*, *In step*
(a blend's clips share one phase, so clips of different lengths keep their
feet together) and *Speed*.

**An arrow's fields**: its conditions, a row each -- a parameter, a
comparison, a value; a trigger is only asked whether it fired -- and **Add a
condition**; *Waits* (not taken before this much of the state has played: 1
is its end); and its *Fade*, in seconds. **Add another way** makes a second
transition between the same two states, with conditions of its own.

**A layer's fields**: its name, *Additive*, its weight -- a number, or a
parameter that says it -- and the joints it moves (each with everything
below it; empty is the whole rig).

**Parameters** are under those: a name, whether it is a number, a boolean or
a trigger, its value while nothing sets it, and where it is read from
(`CharacterBody.Speed`, `Attribute.Attack`, or nothing).

## When an edit is refused

Every edit is checked by the same reader the game uses, before it is made.
Two states of one name, a blend along a trigger, a fade below nought, a layer
left with no state, a parameter something still reads: **the edit is not
made, and the panel says why**, with where -- by the reader's own account of
what would be wrong with the file. What is saved is always a graph the game
can read.

## Where the states are

A page's places are kept in the graph's file under one key, `editor`:

```json
"editor": {
  "layers": {
    "Body": { "any": [ -270, 8 ], "states": { "Move": [ 0, 0 ], "Jump": [ 290, -140 ] } }
  }
}
```

The game passes over it -- it is the one key of the file it does not read --
and **an export does not carry it**: a built game's graph is written without
it. A graph with no such key is laid out by the canvas, and is not changed by
being opened.

## While the game runs

Press Play with a graph open and its page shows what the graph is doing, for
the `AnimationPlayer` that is selected, or the first in the world that plays
this graph:

- the state the layer is in is **lit**, with how far through it is along its
  foot;
- a state it is **fading out of** is lit less, by its weight;
- the **arrow just taken** is lit for a moment;
- the parameters show what they are now.

**A running graph is looked at, not changed**, and the line along the top of
the canvas says so. Stop the game to edit it. To try a value while it runs,
select the player and set the parameter in Properties, under *Animation
graph*: it is set as a script's `SetParameter` sets it, until it is handed
back.

To read one moment of a transition: pause the game, set the parameters in
Properties, and step a tick at a time.

## What it does not do

A state's clip is typed, not picked from a list of the library's clips. A
blend's points are typed in their rows, not dragged on the plot. There is no
scrubber for one state's clip: the page shows the game, not a clip by itself.

## Where to look next

- [Animation graphs](manual:animation/graphs) -- the file, key by key, and the script's side of it
- [Material graphs](manual:rendering/material-graphs) -- the same canvas, for a surface
