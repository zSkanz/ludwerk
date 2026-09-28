# 0117 — A material carries friction and a footstep sound

- Status: accepted (to be built; see `docs/briefs/world-kickoff.md`, B7)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the plan in
  `docs/briefs/game-ready-plan.md`.
- Builds on: [0090](0090-a-material-is-an-asset-a-part-wears-one-and-a-script-clones-one.md),
  [0113](0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md).

## Context

A material decides how a surface looks and nothing about how it behaves. Every
body collides with the default friction of 0.3 (`physics/types.h`); terrain
chunks too. `TerrainHit.material` exists in C++, but `Workspace:Raycast` drops
it: `RaycastResult` has `Instance`, `Position`, `Normal` and `Distance`. A game
cannot play a different footstep on grass and on stone, or make ice slippery,
without tagging every part by hand.

## Decision

1. **A material asset gains physical fields**: `Friction` (default 0.3, today's
   value), `Restitution` (default 0), `FootstepSound` (a sound asset, optional),
   `Tags` (strings a game reads). A part wearing the material collides with its
   friction and restitution; a terrain chunk's triangles carry the layer's
   (per-triangle material in the Jolt mesh shape).
2. **`RaycastResult.Material`** — the material asset hit: the part's, or the
   terrain layer as drawn (ADR 0113's rules included).
3. **The floor under a character**: `Humanoid.FloorMaterial` — the material
   under the character's feet, updated on the tick, nil in the air.
4. **Footsteps are a game's to play**, from `FloorMaterial.FootstepSound` and
   the character's speed; `examples/10-open-world` does it, and the manual page
   shows the few lines.
5. Friction and restitution are simulation state: they are in the trace and the
   rollback snapshot, and a change to a material's friction is a change to the
   world.

## Consequences

- Ice is slippery by being ice. Footsteps follow the ground.
- The terrain's collision shape grows by a byte per triangle.

## Not decided here

- Surface-dependent tyre or vehicle models; a game builds them from `Friction`
  and `Tags`.
