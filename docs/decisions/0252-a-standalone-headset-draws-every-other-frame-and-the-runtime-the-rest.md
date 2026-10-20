# 0252 — A standalone headset draws every other frame, and the runtime the rest

- Status: accepted
- Date: 2026-10-19
- Builds on: [0244](0244-a-game-runs-in-a-headset-through-openxr.md) §5
  (depth to the runtime), [0250](0250-how-a-headsets-eyes-are-drawn-is-the-projects-to-set.md)
  (what a headset's eyes cost and who sets it)

## Context

A Quest 3 shows 72 frames a second, and a game must draw two eyes in each.
ADR 0250 brought the XR room inside that budget by taking things away:
samples, shadow reach, screen effects. A game with more on screen than a
room has nowhere left to take from.

The Quest's runtime offers a way out that its own titles use: the game draws
at half the display's rate, and the runtime makes each frame between from
the last one drawn. It needs to know how each pixel was moving to do that,
so a game that asks for it hands over, with each eye, a small picture of
the motion of what it drew and the depth of it. Head turns are the
runtime's already: it reprojects every frame from the depth it is given.
What it cannot know is how the things in the scene moved -- a ball thrown,
a door swinging, a character walking -- and how the game moved the player's
space: a teleport, a snap turn, a vehicle.

## Decision

1. **`[xr] space_warp`** is `off`, `auto` or `on`. `auto`, the default, is
   on where the runtime offers it on a standalone headset and off
   elsewhere; `on` where the runtime does not offer it says so in the log
   and draws every frame. `XRService.SpaceWarp` reads and sets it in
   play, from the next frame. The log line that says how a headset's eyes
   are drawn says whether frames are made between.
2. **Half the frames, drawn.** With it on, the runtime paces the game to
   half the display's rate: 36 at 72, 45 at 90. The simulation is not
   paced with it -- its tick is fixed (R10) -- and the eyes are drawn at the
   pose the runtime predicts for the frame they are shown in, as now.
3. **The motion of each pixel**, per eye, into an image of the size the
   runtime recommends, sixteen-bit floats: where each surface was in the
   eye's normalized device coordinates the frame before, against where it
   is now, both through THIS frame's view -- so the head's motion, which
   the runtime takes from the depth, is not given twice. Drawn by the
   motion pass the temporal anti-aliasing already has (ADR 0158), parts
   and skinned meshes with their previous transforms and poses. Only what
   is opaque is drawn into it: a transparent surface has two motions in
   one pixel, and a particle's is not worth the cost. During a frame the
   runtime makes, glass, smoke and sparks move with whatever is behind
   them; a moment that must be exact -- a cutscene's title over a moving
   crowd -- turns it off with `XRService.SpaceWarp` and on again after.
   There is no switch for one object: the runtime makes the whole frame.
4. **The world's interface is drawn into both as a solid quad**, though it
   blends: a `SurfaceGui`, a `BillboardGui` and a headset's panels write
   their quad's depth and its motion. Left out, the runtime moved a panel
   with what was behind it, and text smeared where the player reads -- a
   panel that follows the head the worst of them, still against the eye
   while the world behind it turns.
5. **Depth at the same size**, into the depth image the motion is read
   with, in the eye's own convention -- nought near, one far -- with the
   near and far planes the eye was drawn with.
6. **The player's space moving**: how the game moved the space the eyes
   are in since the frame before -- a teleport, a snap turn, a seat in a
   vehicle -- goes with every frame. A cut (a teleport, a scene changed)
   is a frame the runtime makes nothing from; it is marked so, and the
   next frame starts again.
7. **A script's render step runs at the rate frames are drawn.**
   `RunService.RenderStepped` and `PreRender` fire once a drawn frame, so
   at half the display's rate while frames are made between, with a delta
   twice as long. What a game animates there is still smooth -- the
   runtime moves it in the frames between -- but a script that counts
   render steps as display frames counts half. The API reference says so.
8. **Measured before it is the default.** On the device: the frames the
   runtime reports a second, the GPU time of a drawn frame, and a look in
   the headset at hands, a thrown ball, the interface, a panel that
   follows the head with text on it while the head turns, and thin things
   against the sky -- where frames made between show their seams. Until
   then `auto` is `off`.

## Consequences

- A game gets about twice the GPU time a frame on a Quest, for the price
  of the motion and depth pictures (a quarter of the eye's pixels or less)
  and of what the runtime's frames get wrong: what moves fast and is thin,
  what is transparent, and what changes without moving (a light switched,
  a texture swapped) are drawn at half the rate.
- A PC headset is unchanged: the extension is a standalone runtime's.
  The simulated headset draws the motion picture as a debug view, so the
  shader is checked on a PC.
- Input is read at the display's rate and the simulation steps at its own,
  so nothing a player does waits for a drawn frame.

## Alternatives considered

- **Lower the resolution instead** (`render_scale`): it costs every frame
  its sharpness to buy the same time, where frames made between cost only
  what moves fast.
- **The motion with the head in it**, the view of the frame before: the
  runtime reprojects the head from the depth, so it would move every
  still thing twice. The device test in item 8 confirms the choice by a
  still scene under a turning head.
