# 0142 — Terrain shadows are pushed by their slope

- **Status:** accepted (2026-09-30)
- **Amends:** [ADR 0082](0082-terrain-is-a-grid-of-voxels.md) -- how the
  terrain is drawn into the shadow maps
- **Asked for by:** the terrain audit of 2026-09-29 (TA8, TA9, TA10, and the
  P2 items "local-light shadows on terrain have no push" and "bright streaks
  on flat ground at a low sun"); ledger
  `docs/briefs/terrain-audit-2026-09-29-full.md`, T3

## Context

Every mesh culls its front faces in the shadow pass, so the map holds a
solid's far side and a lit surface has nothing of its own to shadow itself
with (D051). The ground has no far side, so ADR 0082 drew it with no culling
and pushed it from the light by six texels, whatever its slope; a local light
pushed it by nothing.

A receiver reads the map through a filter up to six texels wide, plus a
bilinear texel. On ground the light meets at a slant, the depths it reads
there are deeper than its own by the slope times that reach, so a fixed push
was either too small at a low sun or, grown for it, metres in the far cascade.
Measured on rounded hills where nothing shadows any face turned to the sun: at
8 degrees, 180 000 of a picture's 500 000 pixels darkened (355 000 at
`ShadowSoftness` 1), and every round shape's terminator faceted.

## Decision

**The terrain is pushed by its own slope** (`terrain_shadow.hlsl`):

- **Back faces culled.** What the map holds is the side the light falls on. A
  face turned from the light lies behind that side, and was only more ground
  to acne.
- **Each fragment is pushed by its depth slope times the filter's reach**,
  plus a constant: the slope as the rasteriser interpolates it (the sum of its
  change across a texel in x and in y, never under the steepest), the reach the
  cascade's filter radius plus the bilinear tap's texel and half a texel's
  margin, and the constant one texel. Ground face-on to the light is pushed a
  texel; ground at a slant as far as the receiver's filter reads.
- **Up to a tenth.** The push stops growing where the light meets the ground
  at N.L 0.1: ground steeper to the light than that is lit by under a tenth,
  and a push that kept growing would let light under whatever stands on it.
- **A local light pushes the terrain too**, by its own filter's reach. A
  perspective depth has no one texel size to cap the push in, and a grazing
  face under a lamp is ground the lamp's light runs along.
- **The receiver's normal offset takes the mesh's normal**, not the one a
  layer's normal map bends: the map holds the mesh.

**Contact shadows ignore the surface a ray starts on** (`contact_shadow.hlsl`,
TA9). The depth buffer is read at pixel centres and a marched point lands
anywhere in a pixel; on ground seen at a slant, half a pixel is tens of
centimetres of depth against a bias of a few. So the starting plane -- one over
depth, affine across the screen -- is carried to the centre of every pixel the
ray reads, and a pixel on it is not a hit. Growing the bias by the slant was
tried first: it lost the contact of what stands on such ground, which is what
the pass is for.

**The terrain's grain noise hashes its lattice by integers**
(`terrainLattice`). The float hash was so sensitive to its last bit that one
lattice point hashed to two values from two cells; the grain's normal is the
noise's slope, and it drew a bright line and a dark one along those cells'
edges -- the streaks of the audit's photo at a low sun.

**Two debug views and two counts prove them**: `--debug-view=shadow` draws
blue where the ground faces the sun, and `imgshadow` counts faces either
shadow darkens; `--debug-view=bend` draws what the shading does to the mesh's
normal, and `imgsteps` counts where it jumps. `terrain_shadow_acne` runs both
over a scene where nothing shadows any face to the sun.

## Consequences

- On that scene: 66 pixels at most a picture darkened by the map, on the
  smallest ball's terminator; 16 by the contact mask; no step in the bend.
- The push is larger than six texels at a low sun in the near cascades and
  smaller than six in the far one: face-on ground a texel, where it was about
  two metres in the last cascade (TA10).
- Pixels of a terrain's shadow pass now write their depth, so they no longer
  take early depth rejection; the pass draws half the triangles it did.
- The grain's pattern is a different one, of the same scale: every picture of
  terrain moves slightly. The contact and instanced goldens were re-recorded
  for the contact pass, without the speckle along each sphere's rim, and the
  lavapipe goldens for the same pass (their ground at a low sun, backlit, lost
  its stipple); the command-stream goldens for the two shaders' sizes.
