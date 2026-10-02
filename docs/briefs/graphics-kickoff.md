# The graphics block: the kickoff and the ledger

Decided on 2026-09-30 under the owner's standing rule -- *"do not keep asking me
to decide things; everything should be based on how professional game engines
work"* -- and given its place in the queue on 2026-10-02: after the game-ready
plan's ledgers and before the AI ledger (`ai-kickoff.md`). Six things every
engine of this class does and this one does not yet, each with an ADR of its
own, written when its stage opens, and **an image gate**: a picture of the
same scene before and after, in `tests/screenshots/`, that a person can look at
and a test compares.

**The order is what a phone and a mid-range PC gain first**, not the order the
six were listed in.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A measurement first: the stage's scene in `tests/perf/`, its numbers in
  `docs/perf-baselines.md` before and after, on the desktop and -- where the
  stage is for it -- on the phone.
- Off by a quality level where it costs: each feature has a place in
  `[graphics]` and the presets (ADR 0147), and `low` on a phone is not slower
  than it was.
- The capture goldens and the lavapipe goldens move only for the scenes the
  stage is about, and the commit says which.
- No backend type in the public API (R17); every string through i18n keys.
- Full local gate, Linux included, before every push.

## Stage G-F — the render thread

What it is for: the frame's CPU cost today is the simulation and the drawing on
one thread, so a frame is their sum. A render thread makes it the larger of
the two.

- [ ] The ADR: what crosses the thread boundary (the extracted `RenderWorld`,
  already a copy made between ticks -- ADR 0027), who owns the device, how
  uploads and readbacks are handed over, and how a frame is presented while
  the next is extracted.
- [ ] The renderer runs on a thread of its own, fed one `RenderWorld` a frame;
  the main thread does not wait for the GPU.
- [ ] Loaders that upload (meshes, terrain, voxels, textures) hand their work
  over instead of taking the command list.
- [ ] The editor and a headless run are unchanged: one thread when there is no
  window, or when `[graphics] render_thread = false`.
- [ ] Image gate: every existing golden, unchanged. Measurement: `horde`,
  `blocksprint` and `farflight`, frame time against simulation and drawing
  apart.

## Stage G-A — temporal anti-aliasing, an upscaler, and occlusion culling on the GPU

- [ ] The ADR: motion vectors (what writes them, skinned and instanced
  included), the jitter, the history and its rejection, and the upscaler --
  the engine's own temporal one, with a vendor's behind the same seam if a
  licence allows.
- [ ] TAA as an `AntiAliasing` choice beside FXAA; sharp at rest, no ghost
  behind a moving part.
- [ ] Rendering below the window's resolution and upscaling through the same
  history (`RenderScale` then costs a fraction of what it does today).
- [ ] Occlusion culling on the GPU: a depth pyramid from the last frame, tested
  per instance before the draw is issued.
- [ ] Image gate: a thin fence and a moving part, FXAA against TAA; a city
  street with and without occlusion culling, the same picture and the draw
  counts beside it.

## Stage G-D — skinning and cloth, and hierarchical levels of detail

- [ ] The ADR: skinning in a compute pass (one skinned buffer a mesh a frame,
  shared by every pass that draws it), cloth as a constraint solver over a
  skinned mesh's marked vertices, and HLOD -- a cluster of static parts drawn
  as one merged, simplified mesh past a distance.
- [ ] Compute skinning, with the vertex-shader path kept where compute is not
  there.
- [ ] Cloth: a cape and a flag, pinned vertices, wind (ADR 0115), collision
  against the character's capsule.
- [ ] HLOD built by the partitioner per streaming cell, drawn past the cell's
  load radius.
- [ ] Image gate: forty skinned characters, a cape in wind, and a town seen
  from a hill with and without HLOD.

## Stage G-B — particles on the GPU, colliding with the depth buffer

Follows the particles' second pass (the game-ready plan) -- the properties are
decided there, and this is where they are simulated.

- [ ] The ADR: which emitters move to the GPU (those past a count, or those
  that ask), what stays on the CPU (anything a script reads back), and
  collision against the scene's depth.
- [ ] Simulation in compute; a million particles in the measurement scene.
- [ ] Collision with the depth buffer: sparks that land, rain that splashes.
- [ ] Image gate: a fountain onto a floor, CPU against GPU, and the count each
  holds at sixty frames a second.

## Stage G-C — volumetric fog and light

- [ ] The ADR: a froxel grid, what scatters into it (the sun through the
  shadow cascades, point and spot lights, the atmosphere of ADR 0096), and how
  it is applied to opaque and to blended surfaces.
- [ ] Fog with height and density that light shines THROUGH: shafts through a
  window, a street lamp's cone in mist.
- [ ] The existing rays effect and the air keep working, and say which of
  them a scene should use.
- [ ] Image gate: a room with one window at three fog densities; a night
  street.

## Stage G-E — global illumination by probes

- [ ] The ADR: a grid of irradiance probes updated over frames, what they are
  traced against (the scene's own geometry on the GPU, or a baked form of it),
  how leaks through thin walls are stopped, and what a phone gets instead
  (baked probes, read and never updated).
- [ ] Probes placed by the engine over a scene, relit as lights and the sun
  move.
- [ ] Applied to static and to moving parts alike, in place of the single
  ambient term.
- [ ] Image gate: a red wall beside a white floor; a cave mouth; the day strip
  with and without.

## When the ledger closes

Each stage closes on its image gate and its numbers. The ledger closes on the
owner's word, on a scene of his own.
