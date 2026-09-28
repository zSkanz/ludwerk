# Terrain, foliage, wind and water: the kickoff and the ledger

Block B of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27. The order across blocks is in that file (B1, B2, B5, B6, B3, B4, B8,
then B7 later); this one is the order of work inside block B and where each
piece stands.

Decisions: [ADR 0113](../decisions/0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md)
(B1, B2), [ADR 0114](../decisions/0114-a-voxel-holds-two-materials-and-a-blend.md)
(B3), [ADR 0115](../decisions/0115-wind-is-a-workspace-property-that-moves-what-is-drawn.md)
(B5), [ADR 0116](../decisions/0116-foliage-is-drawn-from-rules-over-terrain-and-never-simulated.md)
(B6), [ADR 0117](../decisions/0117-a-material-carries-friction-and-a-footstep-sound.md)
(B7), [ADR 0118](../decisions/0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md)
(B8).

## Where the terrain stood on 2026-09-27

A survey of the code, so no stage re-derives it:

- A voxel is `{u8 occupancy, u8 material}`; the material is read against
  eight built-in entries in `engine/asset/src/terrain_palette.cpp`, each a
  colour only. No textures: the terrain pass binds neutral stand-ins
  (`renderer_default.cpp`, around line 2680) and fixes roughness at 0.92.
- **The automatic rock is shading only**:
  `shaders/include/engine_terrain_surface.hlsli:119-125`, `smoothstep(0.24,
  0.36, 1 - normal.y)` with noise, every material but Rock (3) and Basalt (7).
  Not configurable; the voxel still says grass.
- `PaintBall` sets the byte outright over a hard sphere; the editor's
  *Strength* and *Shape* do nothing to painting (`editor.cpp:4036`).
- Where materials meet: the commonest material per surface-nets vertex,
  colour interpolated across the triangle (`terrain_mesher.cpp:637-665`).
- Only the first terrain's palette is uploaded (`renderer_default.cpp:3765`).
- `RaiseBall` from a script cannot choose a material; `WriteHeights` from a
  script takes one material (the C++ takes one per column).
- `examples/13-terrain` declares `Rock = 1`, which is Grass.
- No foliage, no wind, no terrain water; `RaycastResult` has no material; every
  body has friction 0.3.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- The rules of [`game-ready-plan.md`](game-ready-plan.md). Plus: **a world
  authored before this block opens and looks the same after B1 and B2** — the
  flagship's reference screenshots do not move until B1's textures replace the
  flat colours, and then they are re-recorded once, with before and after in the
  commit.
- Terrain frame time and memory before and after each stage in
  `docs/perf-baselines.md`, on the flagship.

## Stage B1 — terrain layers are engine materials (ADR 0113 §1, §3)

- [x] Eight engine material assets, `engine://terrain/grass` …
  `engine://terrain/ice`, with albedo, normal, roughness and height textures --
  built in and drawn from tileable noise (`terrain_layers.cpp`), so nothing is
  sourced and nothing binary is committed.
- [x] Material fields for terrain: `HeightMap`, `Triplanar`, `BlendSharpness`;
  `TextureScale` is the existing `TileSize` (metres per repeat, both).
- [x] `Terrain:GetLayers()` / `SetLayers()` (up to 255, id `n` is entry `n`),
  defaulting to the eight; saved with the scene only when not the eight.
- [x] Texture arrays per terrain (colour, normal, surface; 512, mips), built
  by GPU blits from the layers' loaded textures; triplanar by layer index;
  today's procedural detail kept as macro variation.
- [x] The palette per terrain (not the first terrain's for all).
- [x] A layer whose material names a surface shader draws with the built-in
  surface and warns in the editor.
- [~] Editor: the paint palette shows the terrain's layers (colour swatches,
  not rendered thumbnails); replace a layer, add one, remove the last; a
  project material picked from Content. **Not reorder** -- see Findings.
- [x] `RaiseBall` takes a material; `WriteHeights` takes one per column;
  `examples/13-terrain`'s ids corrected (and the conformance spec's).
- [~] Tests: the eight and their order; built-in materials and textures
  resolve, tileable; every vertex carries its triangle's layers; layers
  round-trip a scene and the eight are not written; `GetLayers`, `SetLayers`,
  `RaiseBall` and `WriteHeights` materials in conformance. **Not yet**: a
  screenshot of a project material painted, and two terrains in one frame.

## Stage B2 — rules by slope and height (ADR 0113 §2)

- [x] `Terrain:GetRules()` / `SetRules()` (ordered, up to 16): `Enabled`,
  `Material`, `SlopeMin/Max`, `HeightMin/Max`, `Blend`, `Noise`, `AppliesTo`.
  A new terrain's first rule is today's slope rock, to the cosine.
- [x] Evaluated per pixel in the terrain shader, in order.
- [~] The same evaluation on the CPU (`asset::drawnMaterial`, the engine's
  cosine, the same integer hash) in the terrain raycast's hit; **not yet**
  reported to a script (`RaycastResult.Material` is B7's), and HLSL and C++
  agree by construction rather than by a GPU test.
- [x] `Terrain:ApplyRules(min, max)` and the editor's **Apply to voxels**.
- [x] Editor: a *Rules* section in the terrain panel — add, remove, reorder,
  every field, live.
- [x] Tests: rock off makes steep grass green; a height rule puts snow above a
  level; steep grass evaluates as rock on the CPU; Apply writes the bytes and a
  second Apply changes nothing; rules round-trip a scene (and an empty list
  stays empty); conformance for the script API. Checked by eye: example 13 as
  it was, and with the rock off and snow above 7 m.

## Stage B5 — wind (ADR 0115)

- [x] IDL: `Workspace.GlobalWind`, `WindGusts`, `WindTurbulence`,
  `Workspace:GetWindAt(position)`; replicated (protocol 19, `Workspace` as a
  service of properties); out of the world hash (`Presentation`).
- [x] The wind function in HLSL and C++ from `SimTime` (`scene/wind.h`,
  `engine/wind.hlsli`), line for line; tested on the CPU (steady, gusts
  bounded and travelling downwind, turbulence keeping the speed).
- [x] `ParticleEmitter.WindAffectsDrift`.
- [~] `SurfaceInputs.Wind` in `surface.hlsli` (contract 1.1), in both
  functions. **Not yet**: an example that reads it.
- [x] Tests: zero wind changes no screenshot (the gates pass unchanged); a
  particle drifts downwind and only with drift on; the wind replicates;
  conformance for the script API. Traces move only by the new names' atoms.
- [x] Docs: a manual page, *Wind*.

## Stage B6 — foliage (ADR 0116)

- [x] IDL: `FoliageLayer` (`Enabled`, `GetMaterials`/`SetMaterials` with
  multipliers, `Density`, `SlopeMin/Max`, `HeightMin/Max`, `Clumping`,
  `MinSpacing`, `DrawDistance`, `FadeDistance`, `Seed`) and `FoliageMesh`
  (`Mesh`, `Material`, `Weight`, `ScaleMin/Max`, `RandomRotation`,
  `AlignToNormal`, `Sink`, `WindResponse`, `Stiffness`, `CastShadow`).
- [x] Placement (`asset::growFoliage`): a pure function of the tile's level-0
  surface, the rules and the seed, from integer hashes; a tile is a chunk
  column, grown on the job threads as it comes within the draw distance and
  again only when its chunks, its painted mask or the layer change; never under
  a roof (the mesher's sky term).
- [x] Not replicated (both classes `Excluded` in the wire), not hashed (every
  property `Presentation`); a test that the world hash does not move when any of
  it changes. Nothing grows without a renderer, so a dedicated server grows none.
- [x] The GPU-driven path: a buffer per tile, a compute cull by frustum, draw
  distance and quality density into a list per mesh and level, a second pass
  writing the indirect arguments; not through `kMaxInstances`. The RHI gained
  compute pipelines, storage buffers, indirect draws and `readBuffer` on all
  three backends.
- [~] LODs: the cull picks the level whose error stops showing as a pixel from
  the chain every compiled mesh already carries; a dithered fade at the draw
  distance. **Not yet**: a dithered cross-fade between levels, and a card for
  the farthest.
- [~] Wind: the built-in sway (height-weighted bend, a phase per instance).
  **Not yet**: a material's surface shader running its own `surfaceVertex` on
  foliage.
- [x] `[render] foliage_density`, `foliage_shadow_distance`; 0.5 and 15 m on
  Android; thinning in the stable order `random` gives.
- [~] Editor: a **Foliage** section in the Terrain panel -- layers, Add Layer,
  Add Mesh, and a density brush (Paint and Thin) writing a per-column mask saved
  with the scene. **Not yet**: thumbnails of the meshes, and the mask streamed
  with a terrain saved as cells (it is saved in the scene).
- [~] F3 and Stats: tiles resident, instances resident, tiles grown this frame.
  **Not yet**: instances drawn and the cull's GPU time, which need a readback
  and timestamps the RHI does not have.
- [x] **The benchmark**: `tests/perf/foliage`, a field to the horizon in wind,
  +1.50 ms at 1080p against a 2 ms budget (`docs/perf-baselines.md`). The
  flagship grows grass, so `openworld_soak` walks through a streamed field.
- [x] Tests: the same seed grows the same instances, another seed or tile
  others; an edit regrows only the tiles it touched; a lower density keeps a
  subset; a painted mask scales by column; the hash is untouched; the script API
  in conformance; `examples/29-meadow` renders it with the GPU debug layer on.
- [x] Docs: a manual page *Foliage*.

## Stage B3 — two materials a voxel, paint modes, the seam (ADR 0114)

- [ ] The voxel `{occupancy, base, top, cover}`; codec, world hash, scene and
  `.lterrain` v4 (reads v3), terrain replication, rollback snapshot.
- [ ] `PaintBall(center, radius, material, options?)` with `Mode`
  (`Replace`, `Blend`, `Under`, `Erase`), `Strength`, `Falloff`; the sculpt
  tools honour `Strength` and `Falloff` too.
- [ ] The mesher carries interpolated `base`/`top`/`cover` per vertex.
- [ ] The height blend per pixel, `BlendSharpness`; up to four layers where
  pairs differ; the rules apply after.
- [ ] The traces that edit terrain re-recorded once, with the reason.
- [ ] Tests: `Blend` raises cover gradually; `Under` changes only the base;
  `Erase` reveals it; v3 files open identical; a screenshot of a soft seam.

## Stage B4 — terrain tools

- [ ] The brush: *Shape* applies to painting; soft falloff visible in the
  cursor.
- [ ] Eyedropper (pick the material under the cursor, `Alt`+click).
- [ ] Masks: paint only within a slope range, a height range, or over chosen
  materials.
- [ ] *Replace Material* in a region, in the editor.
- [ ] A noise generator (hills and mountains: octaves, scale, height, seed)
  beside *Generate Flat Ground*; heightmap export (16-bit PNG and RAW).
- [ ] Tests: each mask limits a stroke; export then import round-trips heights.

## Stage B8 — water (ADR 0118)

- [ ] IDL: `Water` (`Shape`: `Ocean`/`Box`/`Spline`; `Waves` up to 8;
  `SurfaceLevel`, `Density`, `Viscosity`, `Current`), `GetHeightAt`,
  `GetNormalAt`; `BasePart.Buoyant`.
- [ ] The wave function in C++ (deterministic) and HLSL from one parameter
  upload; tested to agree within a stated tolerance.
- [ ] The built-in water surface (clarity, foam at contact, refraction — the
  example's look), replaceable by a surface shader.
- [ ] Buoyancy and drag as forces at sample points in the physics step; in the
  trace and the rollback snapshot.
- [ ] `BasePart:ApplyImpulseAtPosition`, `BasePart:ApplyAngularImpulse`.
- [ ] **`examples/11-ocean` rewritten**: a `Water` of shape `Ocean`, a simulated
  hull that pitches and rolls, no Luau wave function; its README's two limits
  removed.
- [ ] A lake (`Box`) and a river (`Spline`) in an example over terrain.
- [ ] Tests: a box of density 0.5 floats half submerged; a hull rolls under an
  off-centre load; `GetHeightAt` equals the drawn surface at sampled points;
  the trace reproduces.
- [ ] Docs: a manual page *Water*.

## Stage B7 — friction and footsteps (ADR 0117)

- [ ] Material fields `Friction`, `Restitution`, `FootstepSound`, `Tags`.
- [ ] A part collides with its material's friction and restitution; terrain
  triangles carry their layer's (per-triangle material in the Jolt shape).
- [ ] `RaycastResult.Material`; `Humanoid.FloorMaterial`.
- [ ] Footsteps in `examples/10-open-world`.
- [ ] Traces re-recorded once where friction changes a result, with the reason.
- [ ] Tests: ice slides farther than grass; the raycast reports a rule-drawn
  material.

## Findings

(Filled as the work goes: what the ADRs assumed that reality corrected.)

- **`Terrain.Layers` is two methods.** An instance property is a
  `scene::Value`, which has no list of strings; `GetLayers` and `SetLayers`
  carry the list, and the scene file a `terrainLayers` key beside `terrain`.
- **Reorder is not offered.** A voxel stores the id, so reordering two layers
  repaints every voxel of both; "replace this layer" is the honest verb, and a
  reorder that rewrote the voxels (streamed cells included) is its own piece
  of work.
- **The fragment stage never saw a material id** -- it was a colour
  interpolated from the vertices. The mesher now packs each triangle's three
  ids and the vertex's corner into the UVs (exact in a float), and gives the
  triangles along a seam vertices of their own; a field of one material costs
  nothing.
- **An SDL_GPU array texture needs two layers to be one**: a one-layer
  "array" is created as a plain 2D texture and cannot be bound to a
  `Texture2DArray`. Arrays are at least two layers.
- **The forward layout's slots must stay contiguous**: the terrain shader
  still reads t0-t3 (the neutral stand-ins), or the compiler strips them and
  the bindings shift.
- **Not measured yet**: `docs/perf-baselines.md` has no before and after for
  B1 on the flagship.
- **Rules are two methods too**, for the reason layers are, with rule tables of
  PascalCase fields; a missing field keeps its default.
- **The noise is an integer hash**, not the old float `frac` hash: a float hash
  is not the same bits on every GPU, and the CPU has to agree with it. The
  default rule's edge is therefore ragged in a different pattern than before
  rules, at the same scale.
- **Rules are not in the world hash yet.** They decide what a raycast reports
  only once `RaycastResult.Material` exists (B7); B7 adds them to the hash with
  the field that makes them observable.
- **The workspace never replicated a property before the wind**, not even
  `Gravity`. It travels now as a service of properties, last in the class list
  so no earlier service's fixed id moved.
- **A surface shader's block grew by 32 bytes** for the wind: every declared
  parameter's offset moved by that much, which only generated code and the
  reflection tests see.
- **`SV_InstanceID` does not include the first instance on D3D12**, so an
  indirect draw cannot find its slice of a shared list by `firstInstance`. Each
  mesh-and-level list is found by a base the draw pushes as a uniform instead.
- **The foliage pipelines are made the first frame a world has foliage**, like
  the decals' and the world UI's: made at start-up they would have put four new
  lines into every capture golden of every scene.
- **The memory transport told a peer on close and ENet did not** -- found on
  the way, as D217: the kind of gap only a real transport shows.
- **A foliage tile is a chunk column**, and its fingerprint is the digests of
  its chunks and its neighbours' -- the mesher reads two voxels past a side --
  plus its painted mask, so a sculpt next door can regrow a tile whose own
  ground did not change. Cheaper than keying on border digests, and a tile is a
  few milliseconds on a worker.
- **`Parents` in the IDL is the editor's hint, not a rule the engine enforces**:
  a `FoliageMesh` can be parented anywhere, and grows only under a layer.
