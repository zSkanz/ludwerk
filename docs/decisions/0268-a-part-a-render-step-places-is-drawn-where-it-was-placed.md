# 0268 — A part a render step places is drawn where it was placed

- Status: accepted
- Date: 2026-10-29
- Builds on: ADR 0136 (scripts run at the display's rate, and a camera
  written there is presented)

## Context

A game drew its characters' figures on their bodies: every frame, in a
render step, a figure's part was put where its body is drawn
(`GetRenderCFrame`), and the camera over its shoulder on the same point. The
camera was drawn as written (ADR 0136). The figure was drawn between ticks
again: `alpha` of the way from where the last tick found it -- where an
earlier frame had put it -- to where this frame put it. So it stood behind
its body by a share of a tick that changed every frame, and the camera did
not. Recorded from the player's own screen at a hundred hertz, the figure's
head moved some fourteen pixels left and right, frame to frame, walking
sideways past the camera; at sixty, with a camera two metres away, the
player called it a flick.

## Decision

1. **A part whose place a render phase writes** -- `CFrame`, `Position`,
   `Orientation`, and so `PivotTo` and whatever moves a model through them --
   is marked with the tick it was placed after (`placedAfterTick`).
2. **While that is the current tick it is drawn where it was placed**
   (`DrawPoses::part`), as a presented camera is; a correction's slide still
   moves it. What hangs from it in the picture -- a follower drawn in its
   leader's place, an attachment -- is drawn from that place.
3. **A tick ends it**: once a tick has run with no render phase placing the
   part again, it is drawn between ticks as every part is. The simulated
   `CFrame` is the one written, as before; nothing the simulation, the hash
   or the wire reads changes.
