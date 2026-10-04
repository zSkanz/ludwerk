# 0160 — Particles take a picture, frames, a turn and curves; they collide; a hundred thousand run on the GPU; decals lay over and glow

- Status: accepted
- Date: 2026-10-03, extended 2026-10-04
- Decided by: the agent, on the owner's questions through ludwerk-08
  (2026-10-03: "do particles take a texture?", then collision, a simulation on
  the GPU and decals that are not only dark; 2026-10-04, the owner: "our
  particles have to be optimized"), under the standing rule to decide as
  professional engines do and record it
- Amends: F2's `ParticleEmitter`, which drew three shapes and met nothing, and
  F2's `Decal`, which could only darken

## Context

A `ParticleEmitter` draws each particle as one of three shapes made in the
shader: a soft puff, a disc or a square. Its colour, size and transparency go
from a start value to an end value in a straight line over the particle's life.
Fire, smoke, leaves, debris and magic in a game need more:

- a picture;
- frames that animate;
- a turn and a spin;
- a curve over life, such as a puff that swells and then thins.

Particles also pass through the world. Sparks fall through the floor, rain
falls through a roof, and debris never lands. And every particle is simulated
on the CPU, one at a time, sorted and uploaded each frame: a few thousand an
emitter is the design's ceiling, and a storm, a waterfall or a battlefield of
embers is past it.

A `Decal` multiplies what it lands on. It cannot brighten, so it cannot be a
painted marking on dark ground, a warning ring under a boss, or a glowing
circle.

## How mature engines do it

### What a particle looks like

- **Unity's particle system** has:
  - a material texture, and a Texture Sheet Animation module (a grid of tiles,
    played over the lifetime or at a frame rate, or a random row);
  - a start rotation and a rotation over lifetime;
  - colour, size and alpha over lifetime as gradients and curves that
    multiply the start values.
- **Unreal's Niagara** has a sprite renderer with a SubUV grid, sprite
  rotation and rotation rate, and colour and size scale curves over normalized
  age.
- **Godot's GPUParticles** has a texture with particle animation (h and v
  frames, speed, offset), an angle and angular velocity with randomness, and
  colour, scale and alpha curves over lifetime.

They agree on all of it. The curves multiply what the start values set rather
than replacing them.

### What a particle meets, and where it is simulated

- **Unreal's Niagara** runs an emitter on the CPU or on the GPU, chosen per
  emitter. Its Collision module on the GPU reads the scene's depth buffer, the
  global distance field, or hardware ray tracing; on the CPU it traces rays
  against the physics scene. Depth is the cheap default and is documented as
  seeing only what is on the screen.
- **Unity** has two systems. The built-in one is on the CPU and collides
  against physics colliders or planes, with a quality setting that caps the
  rays. VFX Graph is on the GPU and has Collide with Depth Buffer, signed
  distance fields and primitive shapes, with bounce, friction and a lifetime
  loss per hit. Its particles are unsorted unless a sort is asked for.
- **Godot** has `CPUParticles3D` and `GPUParticles3D`. The GPU ones collide
  against collider nodes, among them `GPUParticlesCollisionHeightField3D`: a
  height map the engine renders of the ground round a point, following the
  camera, made for exactly rain and sparks on terrain.

They agree on three things:

1. The simulation is chosen per emitter, CPU or GPU, and the GPU one is the
   answer to "many".
2. On the GPU the cheap collision is the depth the frame already has, and its
   known limit is what the camera cannot see.
3. Ground that must hold off the screen is a height map round the camera.

### What a decal does to its surface

Unreal's deferred decals have blend modes (Translucent, Stain, Normal,
Emissive); Unity's URP and HDRP decal projectors lay colour over a surface by
alpha and have an emissive output; Godot's `Decal` has albedo mix and an
emission energy. Multiply-only is the odd one out.

## Decision

### What a particle looks like

1. **`Texture`** (Content): the picture each particle is drawn as, facing the
   camera.
   - Its colour is multiplied by the particle's, and its alpha is the
     particle's outline.
   - None draws `Shape`, as before, so an existing emitter is unchanged.
   - It loads like a decal's image, as colour.
2. **A flipbook**: `FlipbookColumns` and `FlipbookRows`, 1 to 16 each, read
   left to right and then top to bottom. `Enum.ParticleFlipbookMode` sets how
   a particle steps through them:
   - Loop, at `FlipbookFramerate` frames a second;
   - OverLife: every frame once, with the last frame held at death;
   - Random: one still frame of its own, chosen at birth.
3. **A turn**:
   - `Rotation` and `RotationSpread`: degrees at birth, give or take, at
     random;
   - `RotationSpeed` and `RotationSpeedSpread`: degrees a second, give or
     take.

   The turn is in the plane facing the camera.
4. **Curves over life**, multiplying the start and end values that are
   already there, as Unity's and Godot's do:
   - `ColorOverLife` (`ColorSequence`, white changes nothing);
   - `SizeOverLife` (`NumberSequence`, one changes nothing);
   - `TransparencyOverLife` (`NumberSequence`, zero changes nothing). It is
     laid over the start and end transparency as one see-through layer over
     another.
5. **Drawn in runs of one picture.** The particles keep their back-to-front
   order across every emitter, and consecutive particles with one picture
   are one draw. A frame of untextured particles is still one draw. The
   instance grows from 48 bytes to 64 (rotation, a textured flag, the
   frame's rectangle).
6. **An emitter that asks for no spread is born as it was.** The random
   rotation, the random spin and the Random frame draw from the emitter's
   seeded generator only when they are asked for. Every emitter made before
   this produces the same particles in the same places.

### What a particle meets

7. **`Collision`** (`Enum.ParticleCollision`): None, the default, so an
   existing emitter still passes through everything; `SceneDepth`; `Terrain`;
   `Both`.
   - **`Terrain`** is the ground's height, in view or not. Both simulations
     read one **ground map**: 128 by 128 heights a metre apart round the
     camera, snapped to 16 m so it does not rebuild as the camera walks, and
     filled 16 rows an update so building it is never a hitch. Outside the
     map, a particle on the CPU asks the terrain's field itself; one on the
     GPU meets nothing.
   - **`SceneDepth`** on the GPU is the last frame's depth: a particle is
     projected into it, and it has hit when it is behind the surface there
     and within a thickness of it. The surface's facing comes from the depth
     either side. On the CPU it is a ray through the physics world from where
     the particle was to where it is, at most 2048 rays an update; past that
     the rest pass through this frame.
8. **`CollisionResponse`** (`Enum.ParticleCollisionResponse`): `Bounce`,
   `Stick`, `Kill`.
   - `Bounce` (0 to 1, default 0.5) is how much of the speed into the surface
     comes back; `Friction` (0 to 1, default 0.2) is how much of the speed
     along it each touch takes.
   - A bounce slower than 0.35 m/s comes to rest, so a pile of sparks does
     not shiver.
   - `CollisionRadius` is how far from its middle a particle touches; 0 is
     half its size.

### Where a particle is simulated

9. **`Simulation`** (`Enum.ParticleSimulation`): `Cpu`, the default, or `Gpu`.
   Per emitter, as Niagara's and Godot's is.
10. **A GPU emitter is a ring of particles in a buffer and one compute pass.**
    - 48 bytes a particle: place, age, velocity, lifetime, rotation, spin,
      frame, and whether it has stuck.
    - The ring holds what `Rate` times `Lifetime` can have alive, to the next
      power of two, at most 262 144 an emitter. `Emit` writes its burst into
      the same ring.
    - `particle_sim` runs once a frame for each emitter, on the main view:
      it gives birth into the slots the CPU names -- each particle's start
      drawn from a hash of the emitter's seed and the slot's serial number, so
      nothing is uploaded -- and ages, accelerates, drags, blows, collides
      and kills every slot.
    - The CPU keeps a count and a clock for the emitter and nothing else.
      Its cost does not grow with the particles.
11. **Drawn from the buffer, unsorted.** `particle_gpu` is `particle.hlsl`
    with a vertex stage that reads a particle by its instance number: the
    same picture, flipbook, turn, curves, soft edge against surfaces and
    lighting. One draw an emitter. The curves are baked to sixteen samples.
    - Particles of one GPU emitter are drawn among themselves in no order,
      and every GPU emitter is drawn after the sorted CPU particles. Added
      light (`LightEmission` 1) does not show an order. Paint
      (`LightEmission` 0) can, where particles overlap thickly: smoke belongs
      on the CPU.
12. **A machine with no compute shaders simulates on the CPU** whatever
    `Simulation` says, capped as a CPU emitter is, and says so once in the
    log (`render.warn.particles_gpu_unavailable`). The game runs; the storm
    is thinner.
13. **The RHI gains one call**, `ICmdList::bindComputeTextures`: textures
    with samplers for a compute pass, which the depth and the ground map
    need. No backend type crosses the public API (R17).

### Decals

14. **`Decal.BlendMode`** (`Enum.DecalBlendMode`):
    - `Multiply`, the default: what a decal always did.
    - `Alpha`: the image laid over the surface by its alpha, lit as the
      surface is -- sun, shadow, sky and the lights that reach it.
    - `Additive`: the image's light added to what is there, unlit.
15. **`Decal.Emissive`**: how brightly the image glows of its own, over what
    lights it. Read by `Alpha` and `Additive`. Above 1 it blooms.
16. Three pipelines, one for each blend; decals keep their order, and a run
    of one mode is one bind.

## Measured

On the development machine (a desktop GPU, the SDL3 GPU backend on its
default driver), a window of 1280 by 720, the display's sync off, about a
hundred thousand particles alive, falling and bouncing on flat terrain:

| Emitters | Collision | Frame, median | `particles.update`, median |
|---|---|---|---|
| GPU, one of 100 000 | Both | 1.1 to 1.3 ms | 0.002 ms |
| CPU, 96 000 across emitters | None | 2.95 ms | 0.08 ms |
| CPU, 96 000 | Terrain, from the ground map | 6.08 ms | 0.92 ms |
| CPU, 96 000 | Terrain, from the field (before the map) | 251.9 ms | 241 ms |

- The owner's target was a hundred thousand at sixty frames a second: 16.7 ms.
  The GPU path is at about a thirteenth of it, and `particles_gpu_gate` holds
  the median under it on every machine that has a device with compute.
- The CPU path's terrain collision was two hundred and sixty times slower
  before it read the ground map: a height from the field walks a column of
  voxels. That is why the map serves both simulations.

## Trade-offs, stated

| | `Cpu` | `Gpu` |
|---|---|---|
| How many | A few thousand an emitter | Up to 262 144 an emitter |
| Order | Back to front with every other CPU particle | None among themselves; after the CPU's |
| `SceneDepth` | A ray against parts, 2048 an update | The last frame's depth: on screen and not hidden only |
| `Terrain` | The ground map, then the field beyond it | The ground map: a square of 128 m round the camera |
| Needs | Nothing | Compute shaders; else it is `Cpu` |

- **Depth collision knows only what was drawn.** A spark that falls behind a
  wall, or off the screen, meets nothing there. A particle that came to rest
  on something the camera then turns away from stays at rest: a stuck or
  resting particle is not asked again.
- **The ground map is the ground's top.** A cave's floor under an overhang is
  not on it; `SceneDepth` sees it while it is on the screen.
- **A GPU particle is not on the CPU at all.** Nothing can read where one is,
  and none is sent anywhere: an emitter replicates, as before, and each
  machine makes its own particles.

## Consequences

- Fire is a flipbook on a loop, smoke an OverLife sheet with a size that
  swells, and debris Random frames with a spin. None of it needs a script.
- A picture not loaded yet draws the particle's shape for the frames until it
  is.
- Rain that stops on roofs, sparks that bounce and settle, and a hundred
  thousand embers are properties of an emitter.
- A warning ring, a painted line and a glowing circle are decals.
- Tests:
  - `particles_tests.cpp`: the frame a particle shows in each mode; curves and
    a spin half-way through a life; an emitter with no spread makes the same
    particles as before; a bounce that loses speed and settles on a slope;
    Kill and Stick; the CPU's ray; an emitter asking for the GPU, and its
    fall back with no compute; the ground map's heights and slope.
  - `tests/conformance/world/particles.spec.luau`: the properties, their
    defaults and what they refuse.
  - `particles_gpu_gate`: on a real device, sparks lie on the ground under a
    Terrain emitter and under a SceneDepth one and not under a None one; and
    a hundred thousand are timed, where a GPU and not a software rasteriser
    draws them.
  - `decal_blend_gate`: on dark ground a multiplied decal stays dark, a laid
    one shows its colour, an added one is brighter than both.

## Amendment, 2026-10-04: `BasePart.ReceivesDecals`

A decal is projected onto the picture's depth, which is of everything in its
box. A game's first marks on the ground -- a scorch, a warning ring -- were
drawn across the characters standing on them.

- **A part says it receives none**: `BasePart.ReceivesDecals`, on by default,
  on `Part`, `MeshPart` and `CharacterBody`; replicated (field 19, protocol
  39) and kept in a partitioned scene's chunks. Terrain, blocks and foliage
  always receive.
- **By a mask and not a stencil.** Every engine that has this marks the
  receivers in a stencil or a G-buffer channel while it draws them. The frozen
  RHI has neither, and adding stencil to it is every backend and every depth
  target for one bit. So the parts that receive none are drawn again, depth
  only, into a target of their own, and a decal's pixel is discarded where
  that depth and the picture's are the same surface. A part in front of one
  of them, or the ground seen through a cutout's holes, is nearer or further
  and is painted.
- **Paid only where it is used**: the mask is drawn on a frame that has both
  a decal and such a part in view, and made the first time it is needed. A
  run of instances is all receivers or all not, as it is all casters or all
  not.
- **What it does not do**: a see-through part is not in the picture's depth,
  so no decal painted it before and none does now.
- Tests: `decal_receives_gate` (two pictures of one scene, before a decal and
  after: the part that receives none is the same pixels in both, the part
  beside it and the floor are darker); `render_world_tests.cpp`,
  `session_tests.cpp`, `chunk_tests.cpp` and
  `tests/conformance/world/material.spec.luau` for the property.

## Not decided here

- **A sort for GPU particles.** Unity's VFX Graph has one as an option. It
  costs a sort a frame on the GPU, and waits for a game whose smoke needs the
  count.
- **A distance field or ray tracing for collision off the screen.** Niagara
  has both. They wait for a scene representation the renderer does not have.
- **Lit, shadowed, mesh or trail particles**, and events on collision
  (a splash where a drop lands).
