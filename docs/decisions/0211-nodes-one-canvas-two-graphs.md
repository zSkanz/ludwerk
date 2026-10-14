# 0211 — Nodes: one canvas, two graphs

- Status: accepted; built as the third item of category 4
- Date: 2026-10-13
- Builds on: ADR 0090 (materials), ADR 0091 (surface shaders), ADR 0115 (the
  wind), ADR 0197 (animation graphs)

## The question

A surface shader is a file of code, and an animation graph is a file of JSON.
Both are right as what the engine reads, and both are the wrong thing to hand
somebody who thinks in pictures: a dissolve is "two noises mixed, a step for
the cut", and a character's movement is "from Move to Jump when it leaves the
ground". The roadmap asks for both to be drawn, on one canvas.

What had to be decided is what a drawing IS: another thing the engine reads,
or a way of writing the files it reads already.

## The decision

### 1. A graph is a way of writing a file the engine already reads

**A material graph writes a surface shader.** `rust.surfacegraph.json`
writes `rust.surface.hlsl` beside itself, and a material names that file
exactly as it names one a person wrote. Nothing downstream knows there was a
graph: the same compiler, the same reload, the same parameters in the
material panel, the same bytecode in a pack.

**An animation graph is the file it was.** The canvas opens and saves the
`*.animgraph.json` an `AnimationPlayer` already plays (ADR 0197). There is no
second format.

### 2. One way, and the exit is to delete the graph

A material graph writes its shader each time it is saved. A shader edited by
hand is not read back -- reading arbitrary code back into nodes is a promise
nobody keeps. What makes that a fair exit and not a trap is that the written
file is one a person could have written:

- **a statement a node**, in the order they feed each other;
- **each local named after its node** -- a node somebody called "Rim amount"
  is `rimAmount`;
- parameters and textures as the contract declares them (`ENG_PARAM`,
  `ENG_TEXTURE`), so the material panel shows them as it shows any;
- and **its first lines say what wrote it and what to do**: edit the graph,
  not this file; to keep the surface as code, delete the graph and the file is
  yours.

The Graph panel says the same beside the canvas, and the manual does.

### 3. A line a node, so an error lands where it is

Every node's statement is one line of the written file, and the writer says
which node each line is. The compiler's "line 38" is then a node on the
canvas, and its words are drawn under that node. What is wrong before the
compiler is asked -- an input that has to be linked and is not, two widths
that do not meet, a parameter with no name -- is found by the writer and
drawn the same way; the shader is still written whole, with nought where a
node could not be, so the rest still compiles and shows.

### 4. The expression node stays, and reads as code

One node is a line of HLSL with up to four named inputs (`a` to `d`) and a
type. It is how a graph says what no node says -- `b > 0 ? 1 - saturate((a -
b) / c) : 0` -- without leaving the graph. It is drawn as code: a header of
its own, and the line visible on the node in the code face. Its inputs and
its output are typed like any other.

### 5. Where a node is, is the editor's

**A material graph's file** is nodes, links and where each node is: it is the
editor's file and nothing else reads it. A pack does not carry it at all --
what a game draws with is the shader it wrote.

**An animation graph's file** is read by the game, so where its states are is
kept apart: under one key, `editor`, at the top of the file. **The reader
passes over that key and nothing else** -- it is as strict as it was about
every other key, and a place that is not two numbers is ignored, never a
reason a graph is not read. **A pack carries the graph without it**: the
asset compiler writes the graph again with no `editor` key before it takes
the cache's key, so what is cached is what is stored. A graph nobody placed
has no such key, is laid out by the canvas, and is carried to the byte.

### 6. One canvas, tested with no window

`NodeCanvas` (`engine/app/node_canvas.h`) is nodes, pins, links, a selection,
a view and its own undo. It knows which links may be made and says why one
may not; it is driven by a pointer -- a place, a button, a wheel -- so
everything a person does on it a test does. It draws nothing. The panel draws
it on ImGui's draw list: rectangles, circles, curves and text. **No node
library**: what is hard about a canvas is the model, and the model is ours.

A graph's rules are said once when its canvas is made:

- a **material** graph's values cannot depend on themselves, and an input has
  one source (a new link replaces the old); a pin is typed -- a number, or two,
  three or four wide -- a number widens to any width, and the widths do not
  turn into each other. An arithmetic node is as wide as what is put into it,
  and its pin is told how wide it came out;
- an **animation** page's states may be gone back to and entered from many.
  Its links are **arrows, not wires**: a state is a box with no pins drawn, an
  arrow leaves it from its **edge** -- a press on the rim draws one out, a
  press further in moves the box -- and lands on whatever box it is let go
  over.

### 7. A node is added from a list that is searched

A right press or the space bar on the canvas, where nothing is, opens a list
of what can be put there: a field that is typed into, the kinds that have
those letters in their name under their headings, the arrow keys along it,
Enter to take one. **A link let go over nothing opens the same list, narrowed
to what has a pin it fits**, and the one chosen is linked as it is added.

### 8. An edit to an animation graph is decided by the file's own reader

A material graph IS its canvas. An animation graph is not -- a state carries
clips, an arrow carries conditions, a layer is a page -- so there the graph
is what is kept and the canvas is a picture of one page of it.

Every edit is made to a copy; the copy is written and read back; and only a
copy that reads is the graph. Two states of one name, a blend along a
trigger, a fade that runs backwards, a layer left with no state: each is
refused **in the reader's own words, with where**, and the file on disk is
never one a game could not read. One rule replaces a second validator that
would have drifted from the first.

### 9. What an animation page shows

- **A layer is a page**; the bar has a button a layer.
- **A state is a box**: its name, and what it plays in a few words. Where the
  layer starts is marked. **A state nothing leads to is drawn as not there
  yet** -- hollow, faint, its edge broken -- and says "no way in".
- **A transition is an arrow**, with what it waits for at its middle. Several
  between the same two states are one arrow, and the fields beside the canvas
  list them. **"From any state"** is a box of its own, one a page, offered in
  the list.
- **Beside the canvas**: a state's clip, or its blend -- along one parameter
  as a line, across two as a plot with its triangles -- loop, in step, speed;
  an arrow's conditions, a row each, what it waits for, its fade; a layer's
  name, weight, joints; and the graph's parameters.
- **While the game runs** the page shows what the graph is doing, for the
  player selected (or the first that runs this graph): the state the layer is
  in lit, what it is fading out of dimmer by its weight, the arrow just taken
  lit for a moment, how far through the state it is along its foot, and the
  parameters as they are. **It is then looked at and not changed**, and says
  so in a line along its top.

### 10. Copy and paste, as text

A selection is copied as a graph's own file -- a material graph of the nodes
and the links among them, an animation graph of one layer with the states
and the arrows among them -- onto the clipboard. Pasted into the same graph
or another of its kind, it is read by the same reader: nodes get ids of their
own, states get names nothing has, and the parameters a state or an arrow
reads come with it by name.

### 11. Where it is in the editor

Each open graph is a window beside the Viewport, as each open script is: a
tab, which can be dragged out to stand beside the world. A double click on
either kind of file in Content opens it; Content's menu has **New Surface
Graph...** and **New Animation Graph...**, each written as a graph that is
one already (a colour into a surface; a layer with a state); `Ctrl+P` goes to
one by its name. With none open, the panel costs one comparison a frame.

## What it is held by

- the canvas's model, with no window: what may be linked and why not, what a
  pointer does, its undo, arrows from an edge (`node_canvas_tests.cpp`);
- the material graph's writer against expected text; **every kind of node
  compiled by the real compiler**, and a compile error landing on its node;
  every example's graph in the writer's own form, with the shader beside it
  being the one it writes, and that shader compiled
  (`surface_graph_tests.cpp`, `graph_editor_tests.cpp`);
- the animation graph's writer: every graph read, written and read again is
  the same graph, the `editor` key is the one key passed over, and a pack
  carries a graph without it (`animation_graph_tests.cpp`,
  `compiler_tests.cpp`);
- the panel driven with no window: the list, links made and refused, the
  keys, a state moved, an arrow from an edge, looked at while the game runs
  (`graph_panel_tests.cpp`);
- and **pictures through `--editor-shots`**, each looked at and each a gate
  (`editor_shots_graph-surface`, `-animation`, `-running`): a material graph
  open; a link refused; the list with letters typed; a compiler's error on
  its node; the character's page while editing; a blend across two parameters
  selected; a state with no way in; an arrow drawn to it; and the page while
  the game runs, one state lit and one fading.

Two verbs were added to the editor's drive for those pictures, because a
picture that waits a count of frames is a picture of how fast the machine is:
`idle` waits for the shader compiler to finish what it began, and
`sleep` waits on the clock. The running page's fade is made by pausing the
game, setting two parameters and stepping four ticks -- the same picture on
any machine.

## What it does not do

- **Functions**: a group of nodes used as one node, in several graphs.
- **Reading a hand-edited shader back** into nodes -- by decision (§2).
- **Comment boxes and reroute points** on a canvas.
- **Dragging a blend's points** on its plot: they are typed in the rows
  beside it.
- **A preview ball** in the panel: a material graph's result is seen on what
  wears it, in the world.
- **A scrubber for one state's clip**: the page shows the game, not a clip by
  itself.
- **A clip picked from a list of the library's**: a state's clip is typed.
