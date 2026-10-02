# 0117 — A material carries friction and a footstep sound

- Status: accepted and built (2026-10-02; `docs/briefs/world-kickoff.md`, B7)
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

## As built, 2026-10-02

- **A part's own number wins by being unlike the default.** A scene file
  writes every property, so "the part wrote its friction" is not a fact a file
  keeps; a part whose `Friction` is 0.3 or whose `Restitution` is 0 takes its
  material's, and any other value is the part's own.
- **The floor is `CharacterBody.FloorMaterial`**, the class a character is
  (section 3 said `Humanoid`, which this engine does not have). It is worked
  out when it is read, not on the tick: nothing stored, nothing to hash,
  nothing to send.
- **A terrain's colliders say what the ground is DRAWN as**: per triangle, the
  layer after its paint and its rules, which is the answer a ray gives. Only
  a chunk with a triangle of a surface that is not the default carries the
  byte; a terrain of grass, sand and rock collides exactly as it did.
- **A new terrain has no layers** (the owner, 2026-09-29), and ground with no
  layer is made of nothing: default friction, nil to a ray and to a foot.
- **The engine's own ground materials are loadable and wearable**:
  `Material.load("engine://terrain/ice")` answers, and a part may wear it.
  Ice is 0.03, snow 0.15, mud 0.6; the rest are 0.3, so a world on grass, sand
  and rock simulates as it did. Each is tagged with its name.
- **A terrain's layers and rules are in the world hash** where there are any,
  as ADR 0113's ledger promised for the day they became observable.
- **`FootstepSound` is a `Content` and the engine plays nothing**: the example
  generates three short sounds from seeded noise and names one in each of its
  eight ground materials.

## Not decided here

- Surface-dependent tyre or vehicle models; a game builds them from `Friction`
  and `Tags`.
- A terrain's colliders name no instance, so a character's `Landed` on terrain
  hands over nil and a part touching the ground fires no `Touched` for it.
  `FloorMaterial` asks the ground directly instead.
