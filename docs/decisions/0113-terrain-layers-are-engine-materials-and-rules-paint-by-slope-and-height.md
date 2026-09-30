# 0113 — Terrain layers are engine materials, and rules paint by slope and height

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B1 and B2)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"todos os materiais relacionados ao
  terreno tenham que ser materiais normais da engine ... o terreno default usa
  o material default da engine mas eu posso por exemplo colocar mais materiais
  no meu projeto"*, and *"nosso terreno tem uma parada que coloca pedra
  automaticamente devo conseguir ativar desativar isso e decidir uma altura por
  exemplo e um material para isso"*.
- Completes: [0090](0090-a-material-is-an-asset-a-part-wears-one-and-a-script-clones-one.md)'s
  "Not decided here" — *"Making each layer name a material asset is the second
  stage the owner agreed to, and it gets its own record."* This is that record.
- Amends: [0082](0082-terrain-is-a-grid-of-voxels.md) (the meaning of a voxel's
  material byte, and the slope rule in its shading).

## Context

What a survey of the code found on 2026-09-27:

- A voxel's material is a `u8` read against a palette hardcoded in
  `engine/asset/src/terrain_palette.cpp`: eight entries (Grass, Sand, Rock,
  Snow, Mud, Sandstone, Basalt, Ice), **each only a colour**. No texture, no
  link to a material asset; the terrain pass binds neutral one-pixel stand-ins
  to the four texture slots and fixes roughness at 0.92.
- **The "automatic rock" is shading only**: `engine_terrain_surface.hlsli`
  blends any material but Rock and Basalt to Rock's colour where
  `1 - normal.y` passes 0.24 to 0.36 (about 40° to 50°), with a noisy edge.
  The thresholds are literals; nothing turns it off; steep snow and sand go
  grey; and the data still says grass, so a raycast there reports grass.
- Every terrain uploads the palette of the first one.

## Decision

### 1. A terrain has layers, and a layer is a material

- `Terrain.Layers` is an ordered list of up to 255 **material assets** (ADR 0090):
  a voxel's material byte `n` (1 to 255) is `Layers[n]`. Zero stays "no ground".
- The engine ships eight material assets under its own content —
  `terrain/grass`, `sand`, `rock`, `snow`, `mud`, `sandstone`, `basalt`,
  `ice` — **with textures** (albedo, normal, roughness and height). A new
  terrain's layers are those eight, in that order, so a world authored against
  the old palette opens unchanged: id 3 is still rock.
- A project adds its own materials to the list in the editor (the terrain
  panel's palette, which shows each layer's thumbnail) or from a script, and
  paints with them like any other.
- **What a material gains for terrain** — fields any material may carry, read
  by the terrain pass and ignored elsewhere: `HeightMap`, `TextureScale`
  (metres per repeat), `Triplanar` (default on for terrain), `BlendSharpness`
  (used by ADR 0114's height blend).
- **Drawing**: the layers' textures are packed into texture arrays per terrain
  (one array per slot, resampled to one size) and the terrain pass samples by
  layer index, triplanar. The procedural detail of today stays as a subtle
  macro variation on top, so a large field still breaks up.
- A layer whose material names a surface shader (ADR 0091) is drawn with the
  built-in terrain surface and a warning in the editor. Surface shaders on
  terrain are a later decision.
- The palette is per terrain, not the first terrain's for all.

### 2. Rules paint by slope and height

- `Terrain.Rules` is an ordered list of rules; the built-in slope rule becomes
  the first entry of a new terrain's list, so a new world looks as it does today.
- A rule has: `Enabled`, `Material` (a layer), `SlopeMin`/`SlopeMax` (degrees),
  `HeightMin`/`HeightMax` (metres, world space), `Blend` (the width of the
  transition, in degrees or metres), `Noise` (how ragged the edge is),
  `AppliesTo` (the layers it may cover; empty means all but its own).
- Rules are evaluated in the terrain shader per pixel, in order, each covering
  what came before where its conditions hold. **They do not write voxels.**
- **"Apply to voxels"** (editor button and `Terrain:ApplyRules(min, max)`) writes
  the rules' result into the material bytes of a region, for an author who wants
  it permanent; after that the rule can be turned off.
- **What the ground is made of, as the game sees it, is what is drawn**: the
  terrain raycast and ADR 0117's floor material evaluate the same rules on the
  CPU, from the same parameters, so a steep grass voxel that is drawn as rock is
  reported as rock. The CPU evaluation uses the engine's deterministic maths
  (ADR 0083); the noise is the same hash in HLSL and C++, tested to agree.

### 3. The fixes that ride along

- `Terrain:RaiseBall` takes an optional material (today a script cannot lay new
  ground with it).
- `Terrain:WriteHeights` takes one material per column, as the C++ below it
  already does.
- `examples/13-terrain` stops calling id 1 `Rock`.

## Consequences

- A terrain looks like the materials a project chooses, with real textures.
- The file format does not change for this ADR: the byte keeps its meaning for
  ids 1 to 8. A scene gains `Layers` and `Rules`; a scene without them reads
  as the defaults above.
- One texture array set per terrain costs memory in proportion to the layers it
  uses; the editor shows the total.

## Not decided here

- Two materials in one voxel and gradual painting: ADR 0114.
- Surface shaders on terrain layers.

## Amendment, 2026-09-29: a new terrain has no materials

The owner: the engine's eight came with every terrain, and nobody could open
them in Content -- they are built in, not files. So a material a terrain wears
is one a project can see and change, or none.

- **A new terrain has no layers and no rules.** Its ground draws a plain matte
  grey, and the first material given to it is what the ground already there
  becomes (the voxels it was laid with are material 1).
- **The engine's eight stay built in, as parents.** The editor's **Add Starter
  Materials** writes eight files under `content/materials/terrain/`, each a
  variant of one of them: finished-looking, listed in Content, and edited like
  any material. **New Material** writes a base material and adds it as the next
  layer. A layer list offers the project's materials only.
- **Absent still means the eight.** A scene written before this has no
  `terrainLayers` or `terrainRules` and reads the engine's eight and the slope
  rock, so it looks as it did; a terrain with none writes its empty lists. A
  script's `Instance.new("Terrain")` is empty, and the examples that build
  terrain in a script carry their materials as files and set them.

## Amendment, 2026-09-30: what a layer reads, and plain ground is plain

The terrain audit's TA12 and TA13 (`docs/briefs/terrain-audit-2026-09-29-full.md`).

- **Plain ground has no variation.** Ground of a material no layer names drew
  the default grey tinted warm and cool by the ground's large-scale drift --
  brown blotches, ball-sized on a ball. The drift is a textured field's
  break-up, and plain ground is one grey; the shader takes it off in
  proportion to how much of a pixel is plain, through the paint and the rules
  laid over it.
- **The arrays are drawn, not copied** (`terrain_pack.hlsl`): each layer of
  each array a pass of its own, the source read at the mip that fits the
  layer's 512 texels, so a map over twice that no longer shimmers. A map not
  square is still stretched to the square.
- **The surface array is packed**: the material's height, from its height map,
  in R; its metallic-roughness map's roughness and metalness in G and B. glTF
  leaves that map's R undefined unless the image is also the occlusion map,
  and the mesh path never read it; the terrain read it as occlusion and as
  height, so an ordinary map drew its ambient at four tenths and lost every
  height blend. A material with no height map is level ground (the middle)
  with no occlusion; with one, its occlusion is its height, on the scale the
  engine's own layers were drawn with, so theirs look as they did.
- **A layer's flags** ride in its surface row's last slot: 1 triplanar, 2 a
  height map.
- **The layers' rows are a storage buffer**, one per terrain, not the uniform
  block they were in (D379): SDL_GPU binds a uniform block to Vulkan 4 KiB at a
  time, and the rows were 12 KiB -- on Linux and Android no layer ever drew its
  textures. The block keeps the rules and the params.
- **A normal map's up is up the image**: each plane bends by the map's x along
  its first axis and against its second, which the image's V runs down. Along
  both lit every bump from the wrong side along one axis.
- **The grain's bend is in world space**, each plane's slope along that
  plane's own axes. It was one tangent-space nudge through a frame whose
  tangent was always +X: not at right angles to the ground, and nothing on a
  wall facing X.
- **The stand-in arrays are cleared** to white and to a flat normal when made,
  and **a normal with no length** falls back rather than becoming a NaN.

`terrain_shadow_acne`'s albedo run holds plain ground to one grey;
`terrain_material_channels` draws a ground with an empty-R map and with none,
the same, and a normal map tilted up the image leaning the ground to -Z.
