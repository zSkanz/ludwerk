# 0008 — Box2D 3.1 for the post-v1 2D layer

- Status: accepted; built
- Date: 2026-08-19

## Context
Ludwerk targets complete 2D games as well as 3D (a requirement), with 3D
first. The dedicated 2D layer (sprites, tilemaps, 2D
physics — not just an orthographic camera) comes after version 1.

## Decision
**Box2D 3.1.1** (MIT, C17, SIMD — the industry standard) is the committed 2D
physics backend, behind its own `IPhysics2D` interface (separate from
`IPhysics3D`). It is vendored, manifest-pinned and integrated in the 2D layer
(`engine/physics/src/physics2d/box2d_physics.cpp`, synced by
`engine/scene/src/physics_sync_2d.cpp`).

## Consequences
The 2D layer starts from a settled physics decision. Note Box2D's cadence may
slow (upstream author's focus moved to Box3D) — acceptable for a stable,
finished v3 line.
