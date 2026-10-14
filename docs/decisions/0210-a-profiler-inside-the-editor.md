# 0210 — A profiler inside the editor

- Status: accepted; built as the second item of category 4
- Date: 2026-10-12
- Builds on: ADR 0171 (the GPU's time by pass), ADR 0208 (corrections a
  player), the engine's scopes (`core::profile`)

## The question

The engine measures a great deal and shows it as text: `--frame-stats` prints
a tree of scopes as a run ends, the frame report a line every few seconds,
the Stats panel a few numbers. None of them answers what a person opens a
profiler to ask: *that* frame, the one that hitched -- where did it go?

## The decision

### 1. It reads what is measured already

Nothing new is timed. A frame of the profiler is what the engine had:

- **the scopes** (`ENG_PROFILE_SCOPE`): the tree of one frame, handed over as
  it closes (`core::profile::lastFrame`, new: the ring kept only totals);
- **a script's time**: what `enterResume`/`leaveResume` already bracket,
  credited to the script whose thread it was;
- **the GPU's passes** (ADR 0171), where they are being timed;
- **memory**: the script heap by what holds it, and what the device holds in
  textures and buffers;
- **the network** in a match: `GetStats()`'s numbers, with the corrections
  counted in the frame they landed in, a player apiece (ADR 0208).

### 2. Shut, it costs a branch

One answer decides whether anything is measured (`Profiler::measures`): the
panel is on a screen that is shown and is reading the run, or a capture was
asked for. While it is no, the scopes are off (a load and a branch each, as
they were), no script's time is kept, no line is sampled and no pass is
stopped. A panel that is a tab behind another measures nothing.

### 3. Two switches that say their own price

- **Time the GPU's passes.** A pass is timed by stopping the frame after it
  (ADR 0171), which makes the frame slower by a stop a pass. The tab says how
  much of the frame shown was that.
- **Sample script lines.** Where the script is, read at a safepoint no more
  often than every fifth of a millisecond of script time. **It is a sample
  and is called one**: the table is shares of the samples, never
  milliseconds a line, and says how many samples it is made of.

### 4. A model, and a drawing of it

`ProfileHistory` (`engine/app/profile_history.h`) is the frames: kept in a
ring, held, chosen, stepped, found by being the worst, read as a tree of
systems, a table of scripts, a list of passes. It knows nothing of ImGui and
is tested with no window. The panel draws it:

- **a strip**, a bar a frame, as tall as the frame was long, in bands by
  system -- simulation, scripts, rendering, interface, waiting, the rest --
  with the display's frame as a line across;
- **a frame is chosen** by pressing or dragging on the strip, and stepped
  with the arrow keys; choosing holds the run, Resume lets go;
- **five tabs**: Systems, Scripts, GPU, Memory, Network -- of the frame
  chosen, or with none chosen the median of the frames kept;
- **the Systems tree is heaviest first**, each level, so the branch that
  holds the frame is the first screen and not under a hundred rows of
  hundredths; every branch folds; and when another frame is chosen the
  heaviest path is opened down to its leaf and the rest shut. Pressing the
  Scope column's header has the order the scopes were entered in. (From the first picture: ten milliseconds of a frame were below the fold.)

**A scope's band is its own or the one it stands in.** A scope whose name
says a band is in it (`scripts.*`, `render.*`, `wait.*`, ...); one whose name
says none is in the band of the scope above it; and what a scope spent that
the scopes under it do not account for is its own band's. So the scripts'
step inside the simulation's tick is the scripts', the wait under the present
is waiting, and a frame's bands add up to the frame.

### 5. The frame worth reading

Somebody opens a profiler because of a hitch. **Worst frame** chooses the
longest frame on the strip and holds it; **Next worst** the next longest
after the one chosen, and round again. A frame over the display's frame by
more than a tenth is marked along the top of the strip, and counted: a frame
held to the display lands a hair either side of its period, and a strip
where every other frame is marked says nothing about the one that hitched.
The strip is never taller than four of the display's frames, so one long
frame does not make slivers of the frames it is read against.

### 6. A capture, taken home

`--profile-capture=FILE` keeps the last minute of frames and writes them as
the run ends (`--profile-lines` samples the lines into it). It needs no
editor and no graphics device: it is for the machine the hitch happens on.
The panel opens such a file from a path typed on its bar and shows it
exactly as it shows a run -- held, since nobody is pushing -- and Save
writes what the panel is showing. The format is JSON, `"format":
"profile-capture"`, version 1; a file of another version is refused by name.

## What it costs

Measured on the development machine (a desktop card through Direct3D 12, the
dev build with the GPU's debug layer on, 1920 by 1080 with no window; the
least of three runs, over 3,000 frames):

| | `39-interior` | `43-ambush` |
|---|---|---|
| shut | 1.41 ms a frame | 0.25 ms |
| open: the scopes and a script's time | +0.08 ms | +0.07 ms |
| and the lines sampled | within the noise of that (+0.06) | +0.09 ms |
| and the GPU's passes timed | +8.0 ms | +1.4 ms |

About 140 scopes are entered a frame, at two readings of the clock each:
0.004 ms. The rest of the 0.07 is handing the frame over. Timing the passes
is dear in this build because every stop waits for a validated device; ADR
0171 has the price on a build without the layer.

## How it is held

- `profile_history_tests.cpp`: the bands on the engine's real tree; the ring
  and the hold; choosing and stepping; the worst and the next; the tree with
  each scope's own time; scripts and lines; corrections in the frame they
  landed in; a file written and read back, bands included; a file that is
  not one refused with its reason.
- `profiler_panel_tests.cpp`: the panel driven with no window -- it pauses,
  goes to the worst frame and the next, chooses the frame pressed, steps,
  shows each tab, writes both switches, opens a file typed into its field
  and goes back to the run; and `measures` is no for every way of being shut.
- `editor_shots_profiler`: the panel drawn on `examples/43-ambush` while it
  plays, both switches on, a frame chosen, each tab.
- `editor_shots_profiler-capture`: a run with no editor writes a profile,
  which is asked to be one, and the editor opens it.
- Script timing and line sampling: two cases in `instance_binding_tests.cpp`.

## What the pictures found

Three things no test had, all found by drawing the panel and looking:

- **Every bar was one grey band.** The rule that sorts scopes into bands had
  been written against the names of its own test; the host's tree is
  `frame.simulation` over `simulation` over `tick` over `scripts.Heartbeat`.
  The rule is by the tree now (section 4) -- and was then found written
  twice, the file reader's copy still the old one, when a capture opened in
  the editor drew a wait as "the rest". It is one function.
- **The editor stopped inside the graphics library with the passes timed**
 : the interface's draw pushed its uniforms into a command buffer
  already submitted.
- **A file's frames stood in a seventh of the strip**: a strip has room for
  every frame a run may keep, and a file is all there will be.

## What it is not, yet

Written in the roadmap under category 4 as the profiler's next steps:

- **a flame chart over time** -- this is a tree with totals, a frame at a
  time; what ran when inside the frame is not kept;
- **a live link to a game running elsewhere** -- a phone, a console: today a
  capture is carried as a file;
- **GPU memory by texture** -- the Memory tab has the device's totals.
