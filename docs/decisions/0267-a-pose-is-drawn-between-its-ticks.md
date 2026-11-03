# 0267 — A pose is drawn between its ticks, as a part is

- Status: accepted; built
- Date: 2026-10-29
- Builds on: ADR 0198 (the pose a frame is drawn with), ADR 0266 (a frame is
  given the time it is shown)

## Context

A character walking over the network on a display at a hundred hertz, the
simulation at sixty: its body was drawn where it is between two ticks
(`DrawPoses`), and so was the camera on it, but its limbs were drawn in the
pose of the last tick. A track's time was read in the game's trace stepping
a sixtieth of a second at a time -- 2.333, 2.350, 2.350, 2.367, 2.383,
2.383 -- so at a hundred frames a second two frames in five showed the pose
of the frame before, and the others a pose a tick on. The body slid; the legs
stood and leapt on it. The player called it walking while vibrating.

## Decision

1. **The animation system keeps every pose as the tick before left it**
   (`before_`), by holding it: a pose the tick writes is written as a copy
   (`ownPose` already copied a pose somebody else held), so it costs a copy
   of each pose that moves, and nothing of one that does not.
2. **The first thing a frame presents** (`presentBetween`, called by
   `stepSecondaryMotion` before the limbs) is each pose that moved, `alpha`
   of the way from the one to the other -- `DrawPoses`' alpha, so the limbs
   and the body they are on are drawn at the same moment: each joint's offset
   and each axis's length in a line, its turn by `slerp`, the rig walked down
   from its root.
3. **What the frame presents after it is made on it**: the limbs' reach, the
   chains and the cloth. A blend is to the first `present` of its mesh what an
   earlier presenting is to a later one with `over`.
4. **Nothing the simulation reads changes**: `pose`, `jointModel`, the
   sockets, the ragdoll and the world's hash are the tick's, as they were. A
   world drawn at its tick -- headless, or held in the editor -- has no history
   and presents no blend.
