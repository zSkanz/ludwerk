# `tests/editorshots/` — the editor, looked at with no window

The editor's interface can be drawn into a picture with nothing put on a
screen, driven by a script. That is how a panel is looked at before a person
has to, and how a few of them are held as gates.

## The rule, before anything else

**Nothing a drawn gate does or asks may depend on how long a frame took.** A
gate is written on a machine that draws an editor at several hundred frames a
second and runs on a hosted one that draws it, beside a running game, at five.
Whatever is a matter of the clock passes here and fails there, and it reads
as a flake until somebody reads the picture. It has been paid for three
times, each one layer further down:

- **What a check asks**: a probe asked for the colour a profile's
  strip had five pixels up from its foot -- which is how many milliseconds of
  the frame the machine had spent simulating. A probe names a *place* that is
  one colour or is not, never an amount.
- **What a test measures**: a test of where a script spends its time
  failed on a loaded machine, where the time was spent somewhere else.
- **What a drive does**: a `dbl` was two presses two frames apart, and
  the toolkit calls that a double click when they are 0.3 s apart or less by
  the wall clock. On a slow runner it was two clicks, a field that never
  opened, and a picture of the frame before the typing.

So, for whoever writes the next drive or check: count in **frames** and in
**ticks** (`wait`, the step button), wait on **what has happened** (`idle`),
and use the clock (`sleep`) only for something that is itself the clock's --
a character falling to the ground -- and then ask of the picture nothing the
length of that wait could change. A verb added to the drive is read for the
same thing before it is used: what does the toolkit decide by time here?

```
engine-host <project> --edit --headless --width=1600 --height=900 \
    --editor-drive=<file.drive> --editor-shots=<dir>
```

- `--edit --headless` is refused without `--editor-shots`: an editor with no
  window and no pictures is a session nobody sees.
- The interface is laid out in a window of the platform's **offscreen** kind
  -- what every headless run's platform already is -- which is memory: no
  window is made on any screen, hidden or otherwise, and the graphics device
  does not own it. The frame is the headless run's own target, the one
  `--screenshot` reads.
- No layout is remembered: every run is the arrangement the editor makes for
  itself, whatever a person left in the project's `.engine/`.
- **The look is the default one, never the person's own**: the theme and the
  scale somebody chose on this machine (`appearance.json`) are not read, and
  nothing is written to them. The first gates read them, passed on the
  machine they were written on and failed on the next, by a tenth.
- Each `shot <name>` of the drive script writes `<dir>/<name>.png`, of the
  frame the step was reached in.

**A game's own overlay is looked at the same way.** Without `--edit`, a
headless run asked for pictures draws the overlay a player opens with its
key, open from the first frame -- the Frame and Rendering readouts, the
Explorer, the Network panel in a match, the console:

```
engine-host <project> --headless --width=1600 --height=900 \
    --editor-drive=<file.drive> --editor-shots=<dir> [--join=... --host=...]
```

The overlay's panel is see-through except for its title bar, so a probe of
it can say that it is drawn and little else; what its rows say is held by
tests of the numbers, and the picture is for looking at.

## A drive script

One step a line (`engine/app/include/engine/app/editor_drive.h` has them
all): `wait`, `move`, `click`, `rclick`, `dbl`, `drag`, `wheel`, `key`,
`text`, `idle`, `sleep`, `shot`, `quit`. Coordinates are the frame's pixels.

**`wait` counts frames, and a frame is not a length of time.** A quick
machine draws a thousand in the second a slow one draws thirty, so a picture
that waited a count for something that takes time is a picture of how fast
the machine is. Two steps wait for the thing itself: `idle`, until the
editor has finished what it began (a shader being compiled); and `sleep
<milliseconds>`, for a game that follows the clock (a character that has to
have landed). To have a moment of a running game exactly, pause it and press
the transport's step: a step is a tick.

What has proved sturdy:

- **Find an instance by name**: click the Explorer's search field, `text` its
  name, and click the row -- the rows of a filtered tree do not move when a
  service is added to the engine.
- **Open a panel from the command palette**: `key ctrl+shift+p`, `text
  Timeline`, `key enter` -- a menu's rows move when one is added above.
- **Open a file by its name**: `key ctrl+p`, `text` enough of the name to be
  the first match, `key enter` -- a scene, a material, a shader, a graph.
- **Close a popup by clicking elsewhere**: Escape lets go of the selection.
- **Open a panel before the game plays**: the palette is the editor's, and a
  running game has the keyboard.
- Look at the picture before trusting a coordinate: take a `shot`, read the
  PNG, then write the next click.

## The gates

`run_editor_shots.cmake` runs one drive on one project and asks each picture
a few things, from a `.checks` file: a line is a shot's name and the
`imgprobe` probes it must pass (`x,y=r,g,b`, fractions of the frame;
`x,y!=r,g,b` for what must NOT be there).

`-DCOPY=1` runs the drive on a copy of the project, for a drive that saves
something; `-DSAID=<words>` names an error the editor is expected to say (a
compiler's, of a line typed wrong on purpose) -- any other error still fails
the test.

| Test | Project | What it holds |
|---|---|---|
| `editor_shots_timeline` | `examples/40-cinematic` | The Timeline opens on the selected player, and a press on the ruler puts the playhead there (ADR 0204). |
| `editor_shots_timeline-popups` | `examples/40-cinematic` | The Timeline's three popups open: a track being added, a key's menu, the prompt a key is named in. |
| `editor_shots_properties` | `examples/40-cinematic` | A colour in a narrow row is its swatch; while editing, a skinned mesh has its Shape keys and Rig roles sections, open; a `LipSync`'s map picker opens. |
| `overlay_shots_game` | `examples/15-multiplayer` | A game's overlay is drawn with no window: the picture can be taken. |
| `editor_shots_profiler` | `examples/43-ambush` | While the game plays, the Profiler has a frame chosen on its strip and held, a tree of systems, the lines sampled and the GPU's passes timed (ADR 0210) -- and the editor draws at all with the passes timed. |
| `editor_shots_profiler-capture` | `examples/43-ambush` | A profile a run with no editor wrote (`--profile-capture`) is a profile, and the Profiler opens it from its path and shows it as it shows a run. |
| `editor_shots_character` | `examples/37-character` | While editing, an `IKControl`'s joint picker lists the rig's joints; while playing, a character's animation graph is drawn. |
| `editor_shots_bakelight` | a copy of `tests/editorshots/bakelight` | `Lighting`'s Bake (ADR 0253) bakes the room's parts and writes `content/light/room.lightmap` (`-DMADE`); the scene is then marked changed, and Bake can be pressed again. |
| `editor_shots_graph-surface` | a copy of `examples/23-surfaces` | A material graph opens on the canvas (ADR 0211); a link that cannot be made is refused and says so; the list a node is added from is typed into; a line typed wrong, saved and compiled puts the compiler's words on its node. |
| `editor_shots_graph-animation` | `examples/37-character` | A character's animation graph while editing: its page, a blend across two parameters with its plot, the list, a state nothing leads to drawn hollow, and an arrow drawn to it from another state's edge. |
| `editor_shots_graph-running` | `examples/37-character` | The same page while the game runs: the state the layer is in lit; and, paused and stepped four ticks after two parameters are set, one state lit and the one it is fading out of neither lit nor unlit. |

**A probe is a flat colour of the interface itself** -- the playhead, a
slider's grab, a selected row -- never the scene behind it: those are the
same on every rasteriser. A gate fails when a panel's layout changes; take
the pictures again, look at them, and move the probes.

**And a probe holds a place, never an amount**: not how tall a bar
is, how far a progress bar has filled, which band of a stack is at a pixel
-- anything that is how much a machine got done by the shot. Ask that the
thing is there (`!=` its ground), or make the amount the same on every
machine first, by ticks.

On a machine with no graphics device the tests skip, and are on the
workflow's lists of tests that may (`tools/repo/skiplists.luau` holds them
there).
