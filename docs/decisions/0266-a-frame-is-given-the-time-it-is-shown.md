# 0266 — A frame is given the time it is shown: whole refreshes while the sync holds

- Status: accepted
- Date: 2026-10-29
- Builds on: ADR 0147 (how fast frames are made), ADR 0173 (the rate a
  handheld holds)

## Context

A game on a desk with vertical sync on, at a hundred hertz, its character
followed by a camera over the shoulder: walking sideways, the world sliding
across the view shuddered. Its frames, read from the game's own trace, were
shown every 10.0 ms on average and begun 8, 8 and 12.5 ms apart. The loop
reads the wall's clock as a frame begins, after the wait for the display, and
that wait ends a little early or late each time. Each frame stood on the
screen for one refresh and moved the world by whatever the clock said: by a
quarter too little or too much, every frame. The simulation, the network and
the interpolation between ticks were right; the time they were asked for was
not when the frame would be seen.

## How a mature engine does it

- **Godot** (4.x, "delta smoothing", on by default): with sync on and the
  refresh known, a frame's delta is snapped to whole refreshes, the
  remainder carried so time is not lost.

## Decision

1. **With the sync holding, a frame's time is the last one's and a whole
   number of refresh periods** (`ShownClock`): as many as the clock says, to
   the nearest, at least one.
2. **It stays on the wall's clock**: a sixteenth of what the clock still says
   is added each frame, so a display whose refresh is not quite what it
   reports does not drift away.
3. **Otherwise the clock as it is**: the sync not holding (`SyncWatch`), no
   refresh known, a frame more than eight refreshes long, a clock behind the
   last frame's time, a headless or synthetic clock, a headset, and a frame
   the handheld's governor holds to its own grid at its present (ADR 0173).
4. Only the time the scheduler is given changes; every wait, every
   statistic and every cap still reads the wall's clock.

## Consequences

Measured on the game it was found in, at a hundred hertz: see the commit that
brought it. A display with a variable refresh shows a frame when it is ready;
there the snapping is a small error that the pull back to the clock bounds.
