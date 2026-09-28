# 0116 — Foliage is drawn from rules over terrain and never simulated

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B6)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"quero que nosso terreno tenha suporte
  a folhagem por exemplo eu poder passar uma malha 3d não só uma específica uma
  lista de malhas e editar adicionar ou remover e dar regras para quando vai
  aparecer essas malhas são meramente visuais ou seja não simulam física deve
  ser o mais otimizado possível"*.
- Builds on: [0113](0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md)
  (layers), [0115](0115-wind-is-a-workspace-property-that-moves-what-is-drawn.md)
  (wind), [0075](0075-terrain-and-block-worlds-stream-in-cells.md) (streaming in
  cells), [0097](0097-a-frames-fixed-costs-come-first-and-static-geometry-persists-when-measured.md)
  (the instanced path).

## Context

The terrain has no grass, flowers or stones scattered over it; a field is bare
ground. The instanced path draws at most 65 536 instances a frame
(`kMaxInstances` in `renderer_default.cpp`), built on the CPU from the draw list —
a field of grass passes that in a few dozen metres.

## Decision

### 1. Instances an author edits

```
Terrain
└── FoliageLayer          -- where it grows
    ├── FoliageMesh       -- what grows, one per mesh
    └── FoliageMesh
```

- **`FoliageLayer`** (child of a `Terrain`): `Enabled`; `Materials` — the
  terrain layers it grows on, each with a density multiplier; `Density`
  (instances per square metre); `SlopeMin`/`SlopeMax`; `HeightMin`/`HeightMax`;
  `Clumping` (noise that gathers instances into patches); `MinSpacing`;
  `DrawDistance` and `FadeDistance`; `Seed`. It never grows where the terrain's
  sky visibility (the term caves are lit by) says the ground is under a roof.
- **`FoliageMesh`** (child of a `FoliageLayer`): `Mesh` (any mesh asset of the
  project), `Material` (optional; the mesh's own otherwise), `Weight` (its share
  among its layer's meshes), `ScaleMin`/`ScaleMax`, `RandomRotation`,
  `AlignToNormal` (0 upright to 1 along the ground), `Sink` (metres into the
  ground), `WindResponse` (0 still to 1 full sway), `Stiffness`, `CastShadow`.
- Adding, removing and editing is creating, deleting and setting instances —
  with undo, in the Explorer and Properties, and from a script.

### 2. Visual only

- No body, no collision, no raycast hit, no navmesh input.
- **Not replicated and not in the world hash.** Placement is a pure function of
  the terrain, the rules, the seed and the chunk: every machine grows the same
  foliage from the same data, and the wire carries none of it.
- A dedicated server grows nothing.

### 3. A painted mask on top of the rules

- The editor's terrain panel gains a **Foliage** tab: the layers and their
  meshes with thumbnails, a live preview, and a brush that **adds or removes
  density** by hand. The brush writes a density mask per layer (a byte per
  terrain column, saved with the terrain, streamed with its cells); the rules
  decide where foliage may grow and the mask scales it.

### 4. Wind

- **Built in**: a foliage mesh bends from its base, weighted by the vertex's
  height within the mesh's bounds, with a phase per instance, by
  `GlobalWind` × `WindResponse` / `Stiffness`. Grass, bushes and flowers sway
  with no setup.
- **Custom**: a `FoliageMesh` whose material names a surface shader runs its
  `surfaceVertex`, which receives `Wind` (ADR 0115) and the instance's random
  value, and does what it likes.

### 5. As fast as it can be

- **Generated per chunk on the job threads** when a chunk streams in, and
  again only for the chunks a sculpt or a paint touched.
- **A path of its own, GPU-driven**: per-chunk instance buffers; a compute pass
  culls by frustum and distance and writes indirect draw arguments per mesh and
  LOD. The general instanced path and its 65 536 limit are not used.
- **LODs made at import** with meshoptimizer (already vendored), with a dithered
  fade between levels and at the draw distance. The farthest level may be a
  camera-facing card, generated at import.
- **Shadows only near the camera** (`[render] foliage_shadow_distance`).
- **Quality scales density**: `[render] foliage_density` (a multiplier; the
  Android default is lower), applied by dropping instances in a stable order so
  lowering it thins the field instead of reshuffling it.
- **Visible**: F3 and the editor's Stats show instances drawn, chunks resident
  and the GPU time of the cull and the draw.
- **The gate**: a benchmark scene (a dense field to the horizon, wind on) with
  a GPU budget recorded in `docs/perf-baselines.md` on the reference machine,
  and a soak like M8's.

## Consequences

- A terrain becomes a landscape, with meshes a project chooses.
- The renderer gains its first GPU-driven path; the terrain and the general
  instanced path may later move onto it.
- The mask is one more stream per terrain cell.

## Not decided here

- Foliage a character pushes aside as it walks through (an interaction field).
- Foliage on parts and meshes other than terrain.
- Trees with collision: a tree that blocks a player is a part or a model, not
  foliage.
