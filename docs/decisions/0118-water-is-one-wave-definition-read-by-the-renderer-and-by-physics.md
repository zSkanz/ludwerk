# 0118 — Water is one wave definition, read by the renderer and by physics

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B8)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, after reviewing `examples/11-ocean` in
  conversation and agreeing that water, not an editable mesh with collision, is
  what the ocean needs.
- Relates to: [0091](0091-a-material-may-name-a-surface-shader-the-user-writes.md)
  (surface shaders), [0083](0083-the-simulation-is-deterministic-across-platforms.md)
  (determinism), [0101](0101-rollback-saves-restores-and-steps-the-simulation.md)
  (rollback), [0121](0121-a-script-can-write-an-image-a-sound-and-a-mesh.md)
  (`EditableMesh`, the alternative considered).

## Context

`examples/11-ocean` works: a surface shader displaces nine 256 × 256 grids on
the GPU (786 000 triangles in 0.67 ms), and crates float by Archimedes through
`ApplyImpulse`. Its README states two limits honestly:

- **The wave function is written twice** — in HLSL to draw and in Luau to float
  things — and the two must agree by hand.
- **The boat is driven, not simulated**: `ApplyImpulse` acts at the centre of
  mass, there is no impulse at a point and no angular impulse, so buoyancy
  cannot roll a hull. The script samples the surface at four points and places
  the boat.

The terrain has no water at all; the block world has flowing fluid of its own.

Rebuilding a collision mesh from the waves every tick was considered and
rejected: 66 000 vertices twenty times a second is expensive, and floating is
not a collision — it is the height of the surface under a point.

## Decision

### 1. `Water`

- A `Water` instance in the workspace, with a `Shape`: `Ocean` (unbounded,
  follows the camera), `Box` (a lake or a pool, its size and position), or
  `Spline` (a river along points, with a flow speed).
- **Waves are data**: `Waves` is a list of up to 8 (wavelength, amplitude,
  direction, steepness); speed follows from deep-water dispersion, as the
  example already does. `SurfaceLevel`, `Density` (default 1000 kg/m³),
  `Viscosity` (drag), `Current` (a vector).
- Its look is a material (ADR 0090) whose built-in water surface reads the
  waves; the example's look (clarity, foam at contact, refraction) becomes that
  surface, and an author can replace it with a surface shader of their own.

### 2. One definition, two readers

- The wave height and slope at `(x, z, t)` are a pure function of the `Waves`
  list, evaluated **in C++ on the CPU** for the simulation (the engine's
  deterministic maths, ADR 0083; `t` is `RunService.SimTime`) and **in HLSL**
  for drawing, from the same parameters uploaded once per change. The two are
  tested to agree within a stated tolerance.
- `Water:GetHeightAt(position): number` and `Water:GetNormalAt(position)` for
  scripts.

### 3. Buoyancy is the engine's

- A simulated part inside a `Water` receives buoyancy from its submerged volume
  (sampled at points over its shape) and drag from `Viscosity`, **as a force at
  each sample point**, so a hull pitches and rolls. `BasePart.Buoyant: boolean`
  (default true) opts a part out.
- This runs in the physics step, is in the trace and in the rollback snapshot.

### 4. Impulses where a game needs them

- `BasePart:ApplyImpulseAtPosition(impulse, position)` and
  `BasePart:ApplyAngularImpulse(impulse)` — every vehicle game asks for them,
  and they are what a game's own buoyancy or thrust would need.

### 5. The ocean example is rewritten on it

`examples/11-ocean` loses its Luau wave function and its driven boat: the sea
is a `Water` of shape `Ocean`, the hull is a simulated body that floats and
rolls, and the README's two limits are removed. It is the proof this ADR worked.

## Consequences

- One definition of the sea. A boat is a body.
- Swimming (a character in water) becomes possible to build on `Water`; the
  character controller's swim state is a follow-up, not this decision.

## Not decided here

- An FFT ocean. The CPU must evaluate what the GPU draws; FFT on both sides is
  a later decision with its own cost.
- Terrain water that fills a dug hole by itself (fluid simulation on the voxel
  terrain). The block world's fluid stays where it is.
