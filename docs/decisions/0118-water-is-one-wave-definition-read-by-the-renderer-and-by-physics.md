# 0118 — Water is one wave definition, read by the renderer and by physics

- Status: accepted, built 2026-09-29 (B8, protocol 28; see the amendment)
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

## Amendment -- 2026-09-29, as built (B8, protocol 28)

- **The waves are `WaterWave` children, not a list property**: one instance per
  wave (`Wavelength`, `Amplitude`, `Direction` in degrees, `Steepness`,
  `Phase`), up to eight in child order, and a river's course is `WaterPoint`
  children. Instances replicate, save and show in the editor with no new value
  type.
- **The wave is `A (sin p - s/2 cos 2p)`**, `p = k (d . xz) - w t + phase`,
  `w = sqrt(9.81 k)`: the second harmonic sharpens the crest and keeps the mean
  at `SurfaceLevel`, and the height under a column stays one closed form -- a
  Gerstner wave's crest moves sideways and has none. The speed uses the sea's
  gravity, not `Workspace.Gravity`, so a game that lowers gravity does not
  change the picture.
- **`Density` is in the unit `BasePart.Density` uses** (default 1), not kg/m³:
  a part floats when its density is below the water's, as the two numbers read.
- **Buoyancy is 27 samples through the part's box** (its shape's fill for a
  ball or a cylinder), each an impulse at its point in `PhysicsSync::step`
  before the scene is handed to the physics. Drag pulls each sample towards the
  water's motion -- `Current`, a river's flow along its points at `FlowSpeed`,
  and **the waves' orbital motion**, fading with depth below the still level
  as linear wave theory measures it. Without that last term the drag held a
  hull back from a rising wave, it went under and was thrown out; with it, it
  rides. A wave's phase is linear in a sample's offset, so the 27 samples'
  sines and cosines come from four pairs by the angle-sum rule: 1.1 µs a
  floating body a tick (`docs/perf-baselines.md`, *Water*).
- **A part a weld drives is not floated** (it is placed, not simulated); the
  part it hangs off is. Characters are not floated; swimming stays a follow-up.
- **Where waters overlap, the highest surface holds a point, and of two at one
  level the one that moves** -- a river running out into a lake carries what it
  carries until it is in the lake.
- **`BasePart.Buoyant` is in the world hash, as every reflected property is**,
  so adding it re-recorded the five determinism traces. It was proved to be the
  only cause: with it left out of the hash, the old traces replayed unchanged.
  It is not on the wire -- the floating is simulated where the part is, and a
  replica is driven by the authority's result.
- **The look is the engine's surface shader `shaders/surface/water.surface.hlsl`**,
  selected by the renderer for every water with the waves as parameters
  (`WaveA0..7`, `WaveB0..7`, `WaveCount`); a render test evaluates the shader's
  sum in floats from the parameters it is handed and compares it with the
  simulation's in doubles. A sea is three rings of one shared 256-quad grid round
  the camera, snapped to the coarsest ring's spacing; a lake is one tile over
  its box; a river a ribbon rebuilt when its points change. **A water does not
  take a material of the author's yet**: replacing the look is a follow-up, and
  a surface shader on a mesh is still the way to draw water the engine cannot
  describe.
- **`Viscosity`'s drag grows with the submerged volume, not the shape**, so the
  default of 1 settles a crate within a second and holds a fast hull back by
  close to its weight. `examples/11-ocean` uses 0.4; the manual says so.
