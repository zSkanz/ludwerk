# 0175 — The lean ground: a terrain's material at the levels that cannot afford all of it

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, in the mobile performance batch (ludwerk-08, on the
  owner's request that the engine be made fast on a phone), under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0113 (a terrain's layers are materials) and its amendment
  (the repeat broken up), ADR 0140, ADR 0171 (the instrument that found it)

## Context

Measured on the owner's phone, at the cheapest level, with the instrument of
ADR 0171: the forward pass is two thirds of the frame, and with the terrain
hidden its share falls from 65% to 29%. Hiding the horde of five hundred
skinned meshes, the foliage, or the sun's shadow changes nothing that can be
measured. The frame is the terrain's fragment.

What that fragment does at every pixel of the ground, for a layer with the
defaults a material is given:

- its three maps -- colour, normal, surface -- read once at the layer's
  repeat and once more at the far scale (`TilingFarScale`, 6 by default), each
  with explicit gradients: six reads, nine more with `HexTiling`;
- three value noises to bend and share that second read, two for the colour's
  drift, one for the rules' raggedness, and five more for every plane the
  pixel faces to tilt a textured layer's shade by a twentieth and its normal
  by a grain half a metre across: forty-four hashes of the lattice, each a
  dozen integer operations;
- four reads of the forward layout's material slots, which hold stand-ins --
  white, flat, white, black -- to multiply by one and add nothing;
- and where two or three layers meet, or one is painted over another, all of
  the above for each.

Every part of it was added for a reason a person saw on a monitor. On a phone
at seven hundred lines, on a tile-based GPU, it is the frame rate.

## How mature engines do it

- **Unity**: the terrain shader on the mobile and URP "simple lit" paths
  blends four layers with one read of each map and no detail noise; the
  anti-tiling and triplanar options are separate shaders a project opts into.
- **Unreal**: landscape materials are authored per platform with "feature
  level switch" and "quality switch" nodes; the mobile path is expected to be
  three layers per component, no tessellation, no distance blend beyond a
  macro texture.
- **Godot**: `Terrain3D`-style shaders expose "auto shader", "dual scaling"
  and "macro variation" as toggles, off on mobile by convention.

All three: the ground's material has a cheaper form, chosen by platform or by
quality level, that keeps the layers and gives up the breaking-up.

## Decision

1. **The ground has a lean form** (`GraphicsSettings::terrainLean`), drawn by
   the same shader on a branch of a uniform:
   - a layer's maps are read once at a plane, and its **colour** once more at
     the far scale -- one read where the full ground takes three and bends
     them by two noises. The colour's repeat is what the eye finds across a
     field; the normal's and the roughness's it does not;
   - no hexagonal cells;
   - one noise for the whole pixel, 37 m across, which is both the colour's
     drift and how much of the far scale shows -- where there were eleven;
   - a plane of the triplanar projection only where the ground faces it by a
     quarter: thirty degrees of slope is one plane's, where it was two;
   - none of the procedural variation over a textured layer.

2. **It is the level's**: `Low` on every machine, and `Medium` on a handheld.
   `High` and `Ultra` draw the ground as they did.

3. **A project can say otherwise**: `[graphics] terrain_surface = "full"` or
   `"lean"`, per platform table as every graphics key is, and
   `--terrain-surface=`. No player setting: it is not a choice a player can
   judge, and the level already is one.

4. **Free at every level, lean or not**:
   - the four stand-in slots are named behind a branch no frame takes and
     never read. They are in the shader because a slot it does not name is a
     slot the compiler removes, and the slots must stay the run SDL's GPU API
     binds from zero; they were read to keep them;
   - a material whose `TilingVariation` is zero is not given two noises to
     multiply by one;
   - a terrain with no rule does not compute the rules' noise.

## What it does not do

- It does not limit how many layers meet at a pixel. A triangle with three
  materials is rare and its cost is three of the lean form.
- A layer's normal map is still read: at a low sun it is most of what makes
  ground look like ground.
- No noise texture in place of the hashes yet. With one noise left a pixel
  the lean form no longer needs it; the full form would, and is not a phone's.

## Consequences

- The lean ground shows a texture's repeat a little more in the middle
  distance than the full one, which bends its second sample; side by side on
  the engine's examples the two read as the same field.
- The full ground changes by one step of one channel in about two pixels in a
  hundred: the stand-in for a normal map was not exactly flat -- 128 of 255
  is not a half -- and tilted every normal by four thousandths. The lavapipe
  goldens with terrain in them are recorded again.
- Measured on a desktop GPU at 3840 by 2160 (the forward pass of three
  terrain scenes, least of three): 0.99 to 0.70 ms, 0.76 to 0.55, 0.78 to
  0.58 -- a quarter to a third off the pass on a GPU that barely feels a
  texture read. The phone's number is the one this is for.
- Protocol unchanged (40).
