# 0143 — Terrain chunk colliders carry a band, so their seams are inside

- **Status:** accepted (2026-09-30)
- **Amends:** [ADR 0066](0066-the-physics-seam-learns-two-static-shapes.md)
  and [ADR 0082](0082-terrain-is-a-grid-of-voxels.md) -- how a terrain
  chunk's collider is made, and what a moving body does with it
- **Asked for by:** the terrain audit of 2026-09-29 (T5, "internal-edge removal
  on chunk meshes"), and **the owner's ruling of 2026-09-30**, through
  ludwerk-08: fix the seam; do not commit to one compound body in advance;
  choose by measurement. His words: *"não vamos fechar terreno composto,
  porque eu quero que o nosso terreno tenha a capacidade tanto de simular
  carros, carros com física, suspensão, etc., quanto jogadores ... tem que ser
  perfeito."* The requirement: the ground behaves as one continuous surface for
  every kind of body -- vehicles with suspension at 5 to 40 m/s, straight and
  diagonal, flat and on slopes; rolling and sliding balls; characters at walk
  and run; resting bodies -- with determinism unchanged.

## Context

The terrain collides as one static triangle mesh per 32-voxel chunk, built
near what moves. Where two chunks meet, each mesh's border edge has one
triangle in its own mesh, so Jolt counts it as an **active edge**, and a body
crossing it can meet the edge's normal instead of the surface's: a *ghost
contact*. `mEnhancedInternalEdgeRemoval`, which Jolt offers against these,
works per pair of bodies, and two chunks are two bodies.

Measured before this ADR (`engine/scene/tests/terrain_seam_tests.cpp`,
driving Jolt directly on the engine's own collider meshes): a frictionless
ball crossing a seam at 8 m/s hopped 12 cm; a character at 8 m/s lost a third
of a metre per second for a tick; a car on cylinder-cast wheels at 5 m/s felt
a break of 153 N in its suspension where one surface gives 53. And **a ball
rolling with friction hopped 4.5 cm on any triangulated ground, one mesh and
no seam** -- the edges between a mesh's own triangles, which Jolt only removes
with the flag set on the moving body.

## What was measured

The harness joins the same ground nine ways and drives the same bodies over
it, at a seam and, as the reference, over **one mesh with no seam**
(`whole`, `whole+e` with the moving bodies removing internal edges). The
candidates the owner named, and their combinations:

- **chunks** -- today; **chunks+e** -- today with the flag on moving bodies;
- **(a) compound** -- the chunks' meshes in one static compound body (and the
  flag); **comp+ring** -- (a) with (b)'s rings;
- **(b) ring** -- each chunk's mesh one cell wider, so its border edges are
  interior;
- **(c) listener** -- a contact listener that drops a contact on a chunk's
  border whose normal is not the surface's (from the field's gradient);
- **ring+f** -- each chunk with a **band**: its own mesh, and the one-cell
  ring's triangles that touch it, marked; a listener refuses every contact
  with the band, so it only lends its edges to Jolt's active-edge pass;
  **ring+f+e** -- the same, and the flag.

Crossing a seam (the reference column is the same drive over one surface at
the same place; the full table is the harness's skipped case "the seam
between two chunks' colliders, felt nine ways"):

| At the seam | whole+e | chunks | chunks+e | compound | comp+ring | ring | listener | ring+f | **ring+f+e** |
|---|---|---|---|---|---|---|---|---|---|
| sliding ball, flat: jolt off the surface (m/s) | 0.0005 | 1.52 | 0.97 | 0.0000 | 0.0000 | 1.05 | 0.144 | 0.144 | **0.0002** |
| sliding ball, flat: hop (m) | 0.000 | 0.121 | 0.056 | 0.000 | 0.000 | 0.058 | -0.007 | -0.007 | **0.000** |
| rolling ball, flat: hop (m) | 0.000 | 0.045 | 0.060 | 0.000 | 0.000 | 0.044 | 0.045 | 0.045 | **0.000** |
| sliding ball, slope diagonal: jolt (m/s) | 0.062 | 1.81 | 0.31 | 0.062 | 0.062 | 0.97 | 0.28 | 0.025 | **0.062** |
| car, cylinder wheels, 5 m/s: force break (N) | 53 | 153 | 153 | 153 | 58 | 58 | 153 | 58 | **58** |
| character, 8 m/s: speed jolt (m/s) | 0.000 | 0.243 | 0.243 | 0.000 | 0.000 | 0.231 | 0.243 | 0.000 | **0.000** |
| character, 8 m/s: lift (m) | 0.000 | 0.016 | 0.016 | 0.000 | 0.000 | 0.013 | 0.016 | 0.000 | **0.000** |
| box resting across four chunks: creep in 10 s | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** |

- **The compound alone fixes balls and characters, not wheels.** A vehicle's
  wheels are shape *casts*, which do not remove internal edges; the compound
  leaves every chunk's border edge active in its own part.
- **The ring alone fixes wheels, not balls or characters**: the ghost moves to
  the ring's outer edge.
- **The listener alone** fixes the sliding ball and nothing else: a character
  cannot have a contact's normal replaced, nor can a cast.
- **comp+ring and ring+f+e are the reference in every row.** Cars on ray
  wheels feel nothing at any seam in any join; on the slope a car's force is
  dominated by the ground's own facets at 1 m voxels, the same at a seam and
  off one.

What they cost, with 4,096 chunks of rolling ground spread over a 4 km square
(the harness's "what each join costs"; `win-msvc-dev`, the machine loaded;
ring+f's build and dig include its meshing, the others' meshes were made
beforehand):

| Join | build | memory | ray | overlap | one chunk changed |
|---|---|---|---|---|---|
| chunks | 7.4 s | 118 MiB | 2.0 µs | 10.2 µs | 1.9 ms |
| (a) mutable compound | 7.6 s | 118 MiB | **8.3 µs** | 10.9 µs | 1.8 ms |
| (a) static compound, rebuilt on a change | 7.4 s | 118 MiB | 2.0 µs | 8.1 µs | **3.1 ms** |
| comp+ring (static) | 14.3 s | 232 MiB | 2.3 µs | 10.2 µs | 7.0 ms |
| **ring+f** | 25.3 s with meshing | **171 MiB** | **2.0 µs** | **7.7 µs** | 6.3 ms with meshing |

- **A mutable compound's queries grow with its parts** -- it tests every
  part's box: 8 µs a ray at 4,096 chunks, 25 at 16,384, where one body per
  chunk stays at 2 µs through the broadphase.
- **A static compound is rebuilt whole on every change**: 3 ms a dig at 4,096
  parts, 9.5 ms at 16,384.
- **ring+f keeps a chunk its own body**: the queries are today's, a dig
  rebuilds the chunks it touches and no compound, and the bands are 45% more
  collider memory (171 MiB against 118).

In the engine, where a tick's colliders are meshed on the job pool (D393):
`tests/bench/terrain_dig`, a character digging every tick, is 0.7 ms a tick
dearer with the band than without (4.6 against 3.9 on a machine loaded to
35%); the moving bodies' flag costs nothing measurable on `physics1k`,
`churn10k`, `crowd50`, `ragdoll10` and the rest (within 2%, A/B in one run).

## Decision

**Each terrain chunk's collider carries a band of its neighbours' triangles,
and the band never collides; moving bodies remove internal edges.**

1. **The band** (`asset::meshCollider`): the chunk's mesh is made one point
   wider on every side, its own quads first -- exactly the chunk's own mesh --
   then the ring's, keeping only the triangles that touch a point an own one
   uses. One mesh, so an own and a band triangle meeting at the border share
   the edge by index, and to Jolt's active-edge pass the border is inside.
   `ShapeDesc::bandFirst` marks where the band starts; the backend stores 1
   in those triangles' user data.
2. **The band only lends its edges**: the backend's contact listener refuses
   a contact with a band triangle (per contact, for a pair with a static
   mesh), and the character's listener does the same. The neighbour's own
   triangle holds the body there.
3. **Every dynamic body and every character sets
   `mEnhancedInternalEdgeRemoval`**, which removes the ghost edges a rolling
   body met inside any one mesh.
4. **A chunk's collider reads four layers of each neighbour**
   (`TerrainChunk::borderDigest(..., 4)`, made the first time it is asked for),
   so an edit in a neighbour's layers the band covers rebuilds it.

## Consequences

- The ground behaves as one surface at a seam for balls, rolling and sliding,
  for characters at walk and run, for cars on cast wheels at 5 to 40 m/s, and
  for resting bodies: `terrain_seam_tests.cpp` holds each against one mesh
  with no seam, flat and sloped, straight and diagonal, and fails in 33 places
  with the old join.
- **The harness drives Jolt's `VehicleConstraint` directly**, because the
  engine has no vehicle until F2's movers and constraints. It is kept as the
  regression test for vehicles on terrain; when F2 lands, the engine's vehicle
  is held to the same table.
- Determinism: the band's refusals are a function of the triangle; the seam's
  answer is tested the same on one Jolt worker as on four, and the engine's
  count is fixed (ADR 0064). `tests/determinism/terrain` was re-recorded for
  this change of the simulation, and only it moved.
- The bands are 45% more collider memory, and a collider's mesh is a point
  wider: 0.7 ms a tick on the digging bench. A mutable compound per
  terrain was rejected for its queries, a static one for its rebuilds, and
  either alone does not fix wheels.
- A raycast may hit a band triangle; it lies in the neighbour's own surface,
  so the point and normal are the surface's, and the body is the same terrain.

## Amendment, 2026-10-06 (D574): a character does not remove internal edges

Decision 3 set `mEnhancedInternalEdgeRemoval` on every dynamic body **and
every character**. A character no longer sets it.

With the flag, a character standing by the edge two facets of the ground
share keeps one contact for the two and solves its step along that one plane.
The sweep that checks the step then meets the other facet -- a degree or two
off the first, on any rolling ground -- and the controller's correction for
its padding takes the hit back to where the character stands: the whole step
is thrown away, every tick, for as long as it is asked to move. A step has to
be long enough to reach the next facet, so a walk did not show it and a dash
at forty metres a second stood still in 52 of 296.

The flag bought a character nothing at a seam: in the table above the
character rows read 0.000 under `ring+f`, the band and its filter with no
flag, as under `ring+f+e`. What it is for is the body that ROLLS, which hops
on the edges inside one mesh without it; dynamic bodies keep it.

The harness's character runs without the flag under the engine's join, so
"every kind of body crosses a seam between chunks' colliders as it crosses one
surface" holds a character as the engine now makes one.
