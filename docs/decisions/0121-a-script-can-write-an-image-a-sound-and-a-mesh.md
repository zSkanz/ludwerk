# 0121 — A script can write an image, a sound and a mesh

- Status: accepted (to be built; see `docs/briefs/media-kickoff.md`, D1 and D3)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"eu não quero limitar o jogador a
  criatividade dele vamos supor que ele quer fazer um player de vídeo na engine
  ele deve conseguir"*, and, for the mesh, *"to pensando em colocarmos
  EditableMesh no game a ideia é que ele possa ou não simular física"*.
- Relates to: [0107](0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md)
  (`view://` textures), [0009](0009-miniaudio-module-is-the-seam.md) (audio),
  [0007](0007-jolt-3d-physics.md) (physics), [0118](0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md)
  (water, where an editable mesh with collision was rejected for the ocean).

## Context

A script can arrange what the engine provides; it cannot make a picture, a
sound or a shape the engine did not foresee. An emulator, a drawing program, a
video decoder written in Luau, a procedural cave, a destructible wall — each
needs a primitive underneath: pixels, samples, vertices.

## Decision

### 1. `EditableImage`

- `EditableImage.new(width, height)`; `:WritePixels(rect, buffer)`,
  `:ReadPixels(rect): buffer` (RGBA8), `:Clear(color)`, `Size`.
- It is a texture: `EditableImage.TextureName` is a `view://`-style name that
  any texture slot takes (an image label, a decal, a material), through the
  registry ADR 0107 built. Only the rows that changed are uploaded, once per
  frame.
- Budget: `[render] max_editable_image_bytes`.

### 2. `AudioStream`

- `AudioStream.new(sampleRate, channels)`; `:Push(buffer)` of float or int16
  samples; `Buffered` (seconds queued); `Starved` signal. It plays through a
  `Sound`-like output (volume, `SoundGroup`, 3D position when parented to a
  part), mixed by miniaudio.

### 3. `EditableMesh`

- `EditableMesh.new()`; vertices, triangles, normals, UVs and colours set and
  read through `buffer`s or per element (`AddVertex`, `SetPosition`,
  `AddTriangle`, `RemoveTriangle`, …); `:ComputeNormals()`.
- A `MeshPart` shows it (`MeshPart:SetEditableMesh(mesh)`), so it has a
  position, a material and everything a part has.
- **The data always lives on the CPU**, because the script that edits it runs
  there; the GPU receives only the ranges that changed, once per frame.
- **`CollisionMode`** on the mesh part:
  - `None` — drawn only; the cheapest.
  - `Static` — the engine rebuilds a Jolt triangle-mesh shape when the mesh
    changes, coalescing edits to at most once per tick; anchored parts only.
  - `Dynamic` — a convex hull (or, opt-in, a decomposition into hulls) for a
    body that moves.
- A mesh's shape is simulation state when it collides: its edits are in the
  trace, applied on the tick.
- **Deformation on the GPU is not this class**: a surface shader's
  `surfaceVertex` already moves vertices for drawing, and it is always visual
  only. Anything physics must see is computed on the CPU (R10: the GPU's floats
  differ between vendors).

## Consequences

- A game can make what the engine did not foresee.
- Three new classes of heavy objects a script can create; each has a budget and
  shows in F3.

## Not decided here

- A video decoder — ADR 0122 builds `VideoPlayer` on these primitives.
- Compute shaders written by a user.
