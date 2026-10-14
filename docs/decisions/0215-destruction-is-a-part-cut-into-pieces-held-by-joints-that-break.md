# 0215 — Destruction is a part cut into pieces held by joints that break

- Status: accepted; built
- Date: 2026-10-14
- Builds on: ADR 0127 (constraints, and the breakable joints already there:
  `BreakForce`, `BreakTorque`, `Broken`), ADR 0099 (who simulates a part),
  ADR 0160 (particles)

## The question

A wall that breaks is today a wall a game builds out of bricks by hand,
welded by hand, with numbers chosen by hand. And a match has a second question
a single machine does not: what of a breaking wall is the simulation -- the
same on every machine, in the hash, sent -- and what is only a picture.

## Decision

### The cut

**`BasePart:Fracture(options?)`** cuts a box part into pieces of itself and
returns them in a `Model` where the part was; the part is destroyed. The
largest piece is cut across its longest side, between three and seven tenths
of the way, again and again, by `Seed` -- pieces of different sizes, as broken
things are, and none a sliver -- and every two pieces that share a face are
held by a `FixedConstraint` at the middle of what they share, breaking under
`Strength` newtons (by default twice the part's weight: it stands under
itself, and a blow of a few times its weight in one tick breaks it where it
lands). The pieces are unanchored copies of the part -- its material, its
colour, how it collides -- and the joints are the breakable joints ADR 0127
already has: nothing new holds or breaks them.

**A part that stood where it was built keeps its footing**: an anchored
sliver under its bottom face, which does not collide, holds the pieces along
the bottom by joints four times a piece's -- the footing carries the push of
the whole wall, and without the margin it gave first and the wall fell over
in one slab. With it, a ball punches a hole where it strikes and the rest
stands.

`Pieces` is 1 to 64. Only a `Part` whose `Shape` is `Block` can be cut: a
part is the engine's drawn and simulated unit, and a piece of a ball or a mesh
would be a shape the engine cannot make at run time. A model cut ahead, in a
modelling tool, is a set of parts already, and holds together the same way
with the same joints.

### Simulation and debris, decided by what they are

- **The pieces are simulation**: parts, the authority's in a match, stepped
  by the solver, in the world's hash, and sent to every machine as any moving
  part is. A wall that blocks a corridor after it breaks is pieces. The cut
  is deterministic from the seed and the part, but what the pieces then do
  is the authority's answer like anything else that moves.
- **Dust and chips are debris**: a `ParticleEmitter` burst where a joint
  breaks (`Broken` gives the joint; `Emit` the burst). Particles are drawn on
  each machine and simulated nowhere: never in the hash, sent as nothing but
  the number emitted, and faded by their own lifetime.

| | a piece | debris |
|---|---|---|
| what it is | a `Part`, the cut of the wall | a particle of a `ParticleEmitter` burst |
| in the world's hash | yes | no |
| on the wire | yes, as a part: what moved, each snapshot | the number emitted, once |
| predicted on a player's machine | when it is in reach of that player's character, as any part is (ADR 0208): the island round the character is stepped ahead; otherwise drawn from the authority's snapshots | never: each machine draws its own |
| can block, be stood on, be pushed | yes | no |
| gone | when the game removes it | by its own lifetime |

**Why**: a piece decides where a player can walk and what a bullet meets, and
every machine must agree about that, so it is simulation. Dust decides
nothing, and a burst simulated nowhere costs no machine anything to agree on.

**A game says which by which it makes**: what must block, be stood on or
be pushed is a piece; what is only seen is a particle. The engine does not
turn a small piece into debris behind the game's back: a piece that vanished
on one machine and stood on another would be a different corridor.

### The rule that keeps it cheap

1. **As few pieces as the moment needs.** A piece moving costs about 70 bytes
   a snapshot to each player, what any moving part costs; fifty falling are
   about 106 KB a second. Twelve to forty is a wall; sixty-four is the most.
2. **A wall costs nothing until it is struck**: cut and standing, its pieces
   are held asleep by their joints and send nothing (measured: the same 1.5
   KB a second as the world without it). Cutting ahead of time is free.
3. **Pieces at rest stop costing**: a piece the solver has put to sleep is
   not sent again until something wakes it; three seconds after the blow the
   fifty cost 39 KB a second, and less as the last settle.
4. **Everything only seen is a particle**: chips, dust, sparks, smoke.
5. **Remove what no longer matters**: a game that leaves rubble for a
   minute and then clears it removes the pieces as it would any part.

### What it costs on the wire

Measured (`netcode_fracture`, a real server and joiner, 50 ms), an
eight-metre wall cut into fifty pieces:

| | bytes a second, each player |
|---|---|
| nothing happening | 1.5 KB |
| cut and standing, untouched | 1.5 KB -- the pieces held by their joints sleep |
| struck at its top, the three seconds the pieces fall | **106 KB** -- about 70 bytes a moving piece a snapshot, what any moving part costs |
| the three seconds after | 39 KB |

All fifty arrive. So a wall can be breakable from the start at no cost, and
a game breaks walls into as few pieces as the moment needs and leaves the
rest to particles. The 64-piece limit keeps one cut under what a link
carries.

## What this does not do

- Pieces that are not boxes (a Voronoi cut needs meshes made at run time).
- Damage that accumulates: a joint breaks at once over its strength, and
  holds fully under it.
- Pieces that fade out by themselves: a game removes what it no longer needs,
  as it does any part.

## Proof

`engine/scene/tests/fracture_tests.cpp` (the cut fills the box, every piece
is held to the rest, a seed is a cut); `tests/conformance/physics/fracture.spec.luau`
(the pieces where the part was, the same seed the same pieces, the wall stands
under itself and a blow breaks it, a ball refused); `netcode_fracture` (the
wire, above); the example `45-breaking-wall` and its picture.
