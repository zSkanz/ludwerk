# 0174 — The forward pass writes no depth the prepass wrote

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, in the mobile performance batch (ludwerk-08, on the
  owner's request that the engine be made fast on a phone), under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0038 (the depth prepass), ADR 0171 (the GPU's time by pass),
  ADR 0172

## Context

The first times from the owner's phone put two thirds of a frame in one pass:
the forward pass, 23 ms of 36 at the cheapest level, drawing three to four
hundred draws at 1560 by 720.

The renderer draws a depth prepass before it, and says why where it does:
"what it buys ... is early-Z rejection for the forward pass". The forward
pass then drew every opaque surface with a pipeline that tests depth AND
writes it, with a fragment that can discard -- `clip` on the alpha cutoff,
which every material's fragment carries, cutout or not.

Those two together are the one combination a GPU cannot test early. A
fragment that may discard must not write its depth until it has run, and a
pipeline that writes depth has its depth test where its write is: after the
fragment. A desktop GPU hides this -- it tests early anyway and writes late.
A tile-based GPU does not: every vendor's guide says the same thing, that
discard with depth writes on turns off early depth testing and the hidden
surface removal built on it (Adreno's low-resolution Z, Mali's forward pixel
kill, PowerVR's HSR). A fragment of the forward pass that stands behind
another is then shaded in full -- shadow taps, lights, environment -- and
thrown away by the depth test afterwards.

**What the phone said about it, the same evening**: in the scene measured,
not much. Hiding the whole horde -- five hundred skinned meshes, the draws
this decision is about -- left the forward pass's share of the frame where
it was, and hiding the terrain halved it: that scene's milliseconds are the
terrain's fragment, whose depth test was early already (it discards
nothing). So this is the practice every engine follows and it costs nothing
to follow, and it is not what makes that scene fast. It is recorded as
decided on the reading of the code, with the measurement that tempers it,
rather than rewritten as if the measurement had asked for it.

## How mature engines do it

- **Unreal**: with an early Z pass, the base pass draws with depth writes
  disabled and the test `Equal` (`r.EarlyZPassOnlyMaterialMasking`,
  "DBuffer / full prepass ⇒ no depth write in base pass").
- **Unity (URP)**: "Depth Priming" -- after a depth prepass, opaque passes
  use `ZWrite Off` and `ZTest Equal`; recommended against only where the
  prepass itself costs more than it saves.
- **Godot**: the opaque pass after its depth prepass draws with depth test
  only for everything the prepass covered.

All three: what the prepass drew is not written again.

## Decision

1. **An opaque draw the prepass drew is drawn in the forward pass by a
   pipeline that tests depth and writes none.** The same shaders, the same
   test (`LessOrEqual`), `depthWrite` off: `pbr_prepassed`,
   `pbr_skinned_prepassed`, `pbr_instanced_prepassed`,
   `pbr_skinned_instanced_prepassed`, `voxel_prepassed`, and for a surface
   shader `surface_forward_prepassed` and
   `surface_forward_instanced_prepassed`.

2. **What the prepass leaves out keeps the pipeline that writes**: a cutout
   (its holes are its fragment's to cut) and a masked surface shader. The
   rule that chooses is the rule that leaves them out of the prepass, read in
   the same function.

3. **The test stays `LessOrEqual`, not `Equal`.** The two passes compute a
   position the same way and the forward pass has always passed against the
   prepass's depth; `Equal` would buy nothing a GPU can use and would turn
   any future difference of one unit in the last place into a hole.

4. The terrain is left as it is: its fragment discards nothing, so its depth
   test was early already.

## What it does not do

- Foliage is not in the prepass and still writes its own depth behind a
  discard: its hidden fragments are shaded. That is the foliage item of the
  batch (a cheaper fragment, or a prepass of its own).
- It does not remove `clip` from the materials that never cut. A fragment
  with no discard in it at all would let a tiler do more still; with depth
  writes off the test is early either way.

## Consequences

- The picture is the one it was: twelve examples, no pixel different (one,
  the block world, is not the same picture twice under either build).
- A desktop GPU draws the forward pass in the time it did -- measured, four
  scenes at 3840 by 2160. On a phone it is worth what a scene's opaque meshes
  hide of each other: little where a terrain fills the screen under a crowd
  seen from above, more in a street of buildings.
- Seven more pipelines are made. The render capture goldens name them.
- Protocol unchanged (40).
