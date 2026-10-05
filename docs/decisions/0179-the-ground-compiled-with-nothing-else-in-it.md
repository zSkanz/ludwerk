# 0179 — The ground, compiled with nothing else in it

- Status: accepted
- Date: 2026-10-05
- Decided by: the agent, in the performance programme the owner put first
  (through the coordinator: "focus on optimisation: what we can optimise,
  optimise"), under the standing rule to decide as professional engines do and
  record it
- Builds on: ADR 0175 (the lean ground), ADR 0171 (the instrument), ADR 0113
  and 0114 (what the ground is made of)

## Context

On the owner's phone the ground is four fifths of the forward pass, and the
forward pass four fifths of the frame. Three things were measured there, each
of which should have taken a large part of that away, and none did:

- the lean ground (ADR 0175) reads a third of what the full one reads, and
  was an eighth faster;
- leaving out every light (`--skip=unlit`) took an eighth of the ground away;
- four shadow taps where there were sixteen (ADR 0172) moved nothing.

`terrain.hlsl` is one shader for every ground there is: the full one and the
lean one, hexagonal cells, three planes, three corners' layers, three painted
over them, sixteen rules each able to read a layer of its own, and the lit
surface every mesh shares, with its four, eight or sixteen taps -- each behind
a branch on a uniform. Every read is handed its gradients, which is what lets
it sit inside a branch at all, and is also what lets a compiler lift it out
of one. A desktop's driver takes the branch. A phone's may work both sides out
and keep one: cheaper than branching where the sides are small. Here they are
not, and a ground that pays for most of its branches whichever it takes fits
all three measurements.

On the desktop the ground's fragment costs what its pieces add up to (ADR
0171, amended): the branches are taken there.

## How mature engines do it

A mobile renderer's shaders are compiled per feature set, not branched: Unreal
compiles a mobile base pass per material and per lighting policy, with
features removed by the preprocessor; Unity's mobile terrain blends at most
four layers a pass by their colour, and its shader variants are stripped at
build time. A uniform branch round a texture read is what both avoid on a
tile-based GPU.

## Decision

1. **A ground shader of its own for the fast path**, `terrain_fast.hlsl`,
   compiled with nothing in it but what a pixel of it does:

   - **at most four layers a pixel**: the two heaviest of the triangle's
     corners, the heaviest painted over them, and the rule that covers most;
   - **the colour alone**, read once a layer at one plane -- the one the
     ground faces most -- at a level worked out once from the pixel's
     footprint, and a second time at the far scale for the heaviest corner's
     layer only: five reads at the most;
   - **the mesh's own normal**, and light that is diffuse alone: the sun
     through four taps of one cascade, the sky's irradiance, the ambient, and
     the lights of the pixel's cluster. No reflection of the sky, no contact
     shadow, no screen-space occlusion, no shadow of a lamp on the ground;
   - **five textures bound** where the full ground's layout is sixteen.

   The rules cover what the CPU says they cover (`asset::drawnMaterial`):
   their bands and their noise are the full ground's arithmetic, and only the
   blend of several at once is not.

2. **And one with nothing at all**, `terrain_flat.hlsl`: one colour. Not a
   look -- the floor the others are measured against, compiled apart because
   whether a driver takes a branch is the thing being asked.

3. **`[graphics] terrain_surface = "fast"`** (and `"flat"`), and
   `--terrain-surface=`, beside `"full"` and `"lean"`. No level chooses it yet:
   it is chosen on the phone's numbers, and what they say goes here.

4. A debug view (`DebugView`) is the full ground's to draw whatever the
   surface: the variants have none.

## What it does not do

- It does not make the fast ground a level's default. That is decided by the
  phone, with this and the lean ground on one install.
- It does not give the same treatment to the lit fragment every mesh shares,
  which has the same branches. If the ground's numbers bear the reading out,
  that is next, and is the larger change.
- The full ground's pipeline is still made, fast ground or not: a second of a
  phone's loading that the fast ground does not need.
- `terrain.hlsl` and the variants hold the terrain's vertex stage twice, line
  for line (`engine_terrain_vertex.hlsli`).

## Consequences

- On the desktop, at 7680 by 4320, the ground is 2.6 ms of the forward pass
  lean and 1.1 ms fast; the picture beside the lean one's differs by a level
  and a half a pixel on average, the ground a touch darker for want of the
  sky's reflection.
- The flat ground first read its colour from the layers' buffer, and on the
  desktop that one read, in a fragment with no texture and no uniform beside
  it, cost more than the whole lean ground (5.9 ms against 4.0 for the pass).
  It reads nothing now. Why that read is slow there is not known.
