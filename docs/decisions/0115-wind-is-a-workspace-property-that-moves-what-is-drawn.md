# 0115 — Wind is a `Workspace` property that moves what is drawn

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B5)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"se eu adicionar uma malha que é uma
  grama se eu quiser adicionar movimento de vento a ela eu devo conseguir ...
  devemos ter opção de vento na workspace"*.
- Relates to: [0072](0072-particles-are-a-picture-simulated-on-the-frame.md)
  (particles), [0091](0091-a-material-may-name-a-surface-shader-the-user-writes.md)
  (surface shaders), [0116](0116-foliage-is-drawn-from-rules-over-terrain-and-never-simulated.md)
  (foliage), [0083](0083-the-simulation-is-deterministic-across-platforms.md)
  (determinism).

## Context

Nothing in the engine has wind. Grass that does not move reads as plastic, and
the owner's foliage (ADR 0116) needs it from the first day. The platform whose
API shape this engine follows has a global wind vector on its world object that
moves grass, particles and clouds and pushes nothing.

## Decision

1. **`Workspace.GlobalWind: vector`** — direction and speed, in metres per
   second. Default zero: a world that does not set it looks as it does today.
2. **`Workspace.WindGusts: number`** (0 to 1) — how much the speed rises and
   falls in gusts; **`Workspace.WindTurbulence: number`** (0 to 1) — how much
   the direction wanders locally. Gusts travel across the world with the wind,
   so a gust is seen crossing a field.
3. **Visual only.** Wind moves what is drawn: foliage, particles, and any
   surface shader that reads it. It pushes no body and enters no trace, so
   R10 does not see it.
4. **One function, two places.** The wind at a point and a time is a pure
   function of `GlobalWind`, the gust settings, the position and
   `RunService.SimTime` (never the wall clock), written in HLSL for drawing and
   in C++ for `Workspace:GetWindAt(position): vector`. The two are tested to
   agree, so a script can put a flag, a sail or a sound exactly where the
   picture says the wind is — the lesson `examples/11-ocean` taught by having
   its wave function written twice.
5. **Particles**: `ParticleEmitter.WindAffectsDrift: boolean` (default false,
   so existing effects do not change) adds the wind at the particle's position
   to its velocity.
6. **Surface shaders**: `SurfaceInputs` gains `Wind` (the vector at the vertex,
   gusts included), available in `surfaceVertex` and `surfaceSurface`; the
   `surface.hlsli` version rises by one minor step.
7. **Foliage**: ADR 0116's built-in sway reads it.
8. **Replication**: the three properties replicate as any `Workspace` property,
   so every client sees the same wind.

## Consequences

- One property makes a world feel alive, and costs nothing when it is zero.
- An author can make any mesh sway with a few lines of a surface shader.

## Not decided here

- Wind as a force on bodies (sails, kites, drifting objects). It would enter
  the simulation, and a game that wants it can apply `GetWindAt` as an impulse
  itself.
- Wind zones (local volumes with their own wind).
