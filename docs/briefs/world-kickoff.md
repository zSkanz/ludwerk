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

- [ ] IDL: `FoliageLayer` (`Enabled`, `Materials` with multipliers, `Density`,
  `SlopeMin/Max`, `HeightMin/Max`, `Clumping`, `MinSpacing`, `DrawDistance`,
  `FadeDistance`, `Seed`) and `FoliageMesh` (`Mesh`, `Material`, `Weight`,
  `ScaleMin/Max`, `RandomRotation`, `AlignToNormal`, `Sink`, `WindResponse`,
  `Stiffness`, `CastShadow`).
- [ ] Placement: a pure function of terrain, rules, seed and chunk; per chunk on
  the job threads when a chunk streams in; only touched chunks regenerate after
  an edit; never under a roof (sky visibility).
- [ ] Not replicated, not hashed, nothing on a dedicated server; a test that the
  world hash and the wire are unchanged with foliage on.
- [ ] The GPU-driven path: per-chunk instance buffers, a compute cull by frustum
  and distance writing indirect arguments per mesh and LOD; not through
  `kMaxInstances`.
- [ ] LODs at import with meshoptimizer; dithered fade; an optional card for the
  farthest level.
- [ ] Wind: the built-in sway (height-weighted bend, a phase per instance);
  a surface shader's `surfaceVertex` receives `Wind` and the instance's random.
- [ ] `[render] foliage_density`, `foliage_shadow_distance`; lower defaults on
  Android; thinning in a stable order.
- [ ] Editor: a **Foliage** tab — layers and meshes with thumbnails, live
  preview, a density brush that writes a per-column mask saved and streamed with
  the terrain.
- [ ] F3 and Stats: instances drawn, chunks resident, cull and draw GPU time.
- [ ] **The benchmark**: a dense field to the horizon with wind; a GPU budget
  recorded on the reference machine; a soak like M8's. The flagship gains grass.
- [ ] Tests: the same seed gives the same instances on every platform; a sculpt
  regenerates only its chunks; density 0.5 keeps a subset of density 1.0.
- [ ] Docs: a manual page *Foliage*.

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
