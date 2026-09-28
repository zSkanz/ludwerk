# 0114 — A voxel holds two materials and a blend

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B3)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"sobre a pintura do terreno com os
  materiais devo conseguir decidir se uma textura vai sobrescrever a outra ou
  se ela vai simplesmente pintando a outra aos poucos ... posso ter algo por
  baixo e algo por cima por exemplo tambem deve ter uma suavização na junção
  de materiais diferentes"*.
- Amends: [0082](0082-terrain-is-a-grid-of-voxels.md) (a voxel is an occupancy
  and a material).
- Builds on: [0113](0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md).

## Context

A voxel is `{u8 occupancy, u8 material}`. `PaintBall` sets the byte outright,
over a hard-edged sphere; the editor's brush shows *Strength* and *Shape*, and
painting ignores both. Where two materials meet, each surface-nets vertex takes
the commonest material of its cell and the colour is interpolated across the
triangle — a transition one voxel wide at the finest level, wider at coarser
ones, and with textures it would be a smear.

"Paint a little at a time" and "something under and something over" cannot be
expressed with one byte. The shape other voxel engines use is two materials
and a weight.

## Decision

### 1. The voxel

- `{u8 occupancy, u8 base, u8 top, u8 cover}`: `base` is the material under,
  `top` the material over, `cover` how much of `top` shows (0 to 255). A voxel
  painted with one material has `top == base` or `cover == 0`.
- Four bytes instead of two. The chunk codec, the world hash, the scene and
  `.lterrain` format (version 4, reading version 3 as `base = material,
  cover = 0`), the replication of terrain edits and the rollback snapshot all
  change with it, in the same commit series, each with its test.

### 2. Paint modes

`Terrain:PaintBall(center, radius, material, options?)` and the editor brush
take a mode:

- **`Replace`** — `base = material, cover = 0`: today's behaviour.
- **`Blend`** — raises `top = material`'s cover by `Strength`, weighted by a
  **falloff** from the centre (`Falloff`, 0 hard to 1 soft); painting a third
  material over a voxel that already shows two moves the more-covered one to
  `base` first.
- **`Under`** — sets `base = material` and leaves `top` and `cover`: the
  ground beneath changes, the layer above does not.
- **`Erase`** — lowers `cover`, revealing `base`.

`Strength` and `Falloff` work for every sculpt tool as well, and the brush's
*Shape* applies to painting.

### 3. The seam is drawn per pixel, by height

- The mesher carries `base`, `top` and `cover` per vertex (interpolated
  weights, not a majority vote).
- The terrain shader blends the two layers per pixel with a **height blend**:
  each layer's height map (ADR 0113) decides which one shows through, and
  `BlendSharpness` decides how hard the edge is — rock shows in the cracks of
  sand instead of fading into it. Where two voxels with different pairs meet,
  the weights of up to four layers are blended the same way.
- The rules of ADR 0113 apply after the painted layers.

## Consequences

- Painting behaves like a paint program: gradual, soft-edged, layered.
- Terrain memory doubles for the voxels that exist (air is never stored), and
  a large field's file grows with it. `docs/perf-baselines.md` records the
  before and after for the flagship's field.
- A replay trace that edits terrain is re-recorded once, with the reason.

## Not decided here

- More than two materials per voxel (splat weights of four or eight). Two and
  a blend is what the owner asked for; a third is added by painting, not stored.
