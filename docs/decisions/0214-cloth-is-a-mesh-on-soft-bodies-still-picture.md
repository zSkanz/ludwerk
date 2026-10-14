# 0214 — Cloth is a mesh on soft bodies, still picture

- Status: accepted; built
- Date: 2026-10-14
- Builds on: ADR 0194 (secondary motion: spring bones first, cloth on the
  same colliders later -- this is its second tier), ADR 0201 (a mesh that
  wears another's pose), ADR 0196 (morph targets, whose pipelines draw it),
  ADR 0115 (the workspace's wind), ADR 0147 (graphics settings), R10

## The question

A spring cape (ADR 0194) is three or four columns of joints. It swings, and it
cannot fold, cannot fill with the wind between its columns, and goes through a
wall. A cape a player looks at for a whole game is a sheet: every vertex of it
moves, it lies on what it touches, and it costs what a picture may cost.

## Decision

### A `Cloth` under the mesh that is the cloth

**`Cloth`**, parented to a skinned `MeshPart`, makes every vertex of that mesh
a particle of a soft body: its edges and the folds between its faces are the
constraints, Jolt's soft bodies (already in the tree; no new dependency) solve
them, and each machine hangs it. Its properties are the spring cape's where
they mean the same -- `Damping`, `GravityScale`, `WindInfluence` -- and its
own where they do not: `PinJoints`, `MaxDistance`, `Stiffness`,
`BendStiffness`, `Thickness`, `CollidesWithWorld`.

**What pins it is the rig**: a vertex weighted at least half to a joint
`PinJoints` matches stays exactly where the animation puts it; every other
vertex hangs, held within `MaxDistance` of where the animation would put it
(Jolt's skinned constraints) and tethered to the nearest pinned vertex at the
length it was modelled (its long-range attachments), so a cape of a hundred
links does not stretch under its own weight. A vertex colour to paint the pins
with was considered: the mesh format carries none, and a joint for the cape's
top is what an artist already adds for a spring cape.

**Vertices at the same place are one particle**: a mesh splits a vertex at
every seam of its normals and its UVs, and a cape modelled on both sides has
every corner twice. A face two sheets share is one face.

### The world it hangs in is its own

A cloth is stepped in **a physics world of its own, never the game's**: one
for every cloth of a host, made the first frame there is one. In it are the
cloths, the `SpringCollider`s of their meshes -- and of the mesh a cloth wears
the pose of -- as moving capsules, and, for a cloth with `CollidesWithWorld`,
**the anchored parts near it as the host's own world has them**: asked of that
world twice a second or when the cloth has moved half a metre, copied as their
own shapes, and followed every frame, since an anchored door a script swings
is anchored too. Never a part of the cloth's own model: the body is what its
colliders describe, and a body's hull, rounded by the solver's margin, stands
a few centimetres into a cape modelled a few centimetres off the back -- the
first run of the example threw every standing cape up over its shoulders. A
cape never pushes anything: nothing it touches is in the game's world.

**The air**: each face is pushed along its normal by the air crossing it, in
proportion to its area -- still air for a cloth carried through it, the
workspace's wind for one standing in it. A runner's cape streams; one carried
edge-on barely trails, as cloth does. A cloth weighs about what heavy wool
does, four tenths of a kilogram a square metre, and a ten metre a second wind
through it pushes it as hard as gravity pulls it -- what a spring chain's wind
does, so a cape changed from one tier to the other is blown the same way.

### Drawn as a morph target of weight one

The vertices come back as how far each moved from where it was modelled, in
the mesh's own space, and its normal as its faces now turn it: a row a vertex
in a buffer the frame uploads once, read by **the morph pipelines ADR 0196
made** as a single target of weight one. The draw is not skinned -- the cloth
is already where it hangs -- and draws alone. No new shader. A cloth's bounds
are its mesh's and where the cloth now is, together.

### It is picture, not simulation

As tier 1: each machine, after the pose, at the rate frames are drawn. **The
instance travels (protocol 52, with the vehicle's fields); where its vertices
are does not.** Not in the world's hash, not in a replay, nothing on a host
with no window. R10 is untouched because nothing the simulation reads is
written.

It settles in the same place at 30, 60 and 144 frames a second: a frame is one
step of its length, at most a thirtieth of a second, the solver's iterations
sub-stepping it, and the pinned vertices carried across the step from where
the last frame had them to where this one does. A frame of a quarter of a
second or more, and a carrier moved further in one frame than twice the
cloth's size, put it where its animation has it, at rest.

### Where it is off

A mesh of more than 8,192 vertices is no cloth's. A cloth the camera did not
reach, or further than the secondary motion distance, is not stepped and is
drawn as its animation has it; so is every cloth **at the Low level, and on a
phone or a handheld below High**, until cloth is measured on one. A frame
draws at most 131,072 cloth vertices.

## Measured

On the development build (`cloth_tests.cpp`), a cape of 252 vertices:

| | a frame |
|---|---|
| one hero | 0.13 ms |
| twenty heroes | 0.67 ms |
| the example's three, in a real run | 0.26 ms |

**It rests on the body and does not pass through**: pressed onto three
capsules down a back by a gale, no vertex and no point along any edge is
inside one; it lies at its `Thickness` off them. **On a wall**: blown onto
one, every vertex stays out of it and the hem lies on it.

**It does not flicker.** Two measures. The vertices: the most any vertex
moves back and forth from one frame to the next -- its second difference --
is under a millimetre in still air, a steady wind and a gusting one, and 0.12
mm pressed on a wall; a pixel of the example is a centimetre. The picture
(`cloth_gate`): five seconds of the example, a picture a frame, and the pixels
that pop -- out of line for one frame and back the next -- **counted on the
capes' own pixels alone** (`imgflicker --pops=6,24 --only=r,40`), so a change
to the shadows or the anti-aliasing elsewhere can neither pass nor fail it.
The capes cover about 770 pixels a frame: 1,036 of their pixel-frames popped
in 297 frames, under half of one per cent, with the cloth; 55 with the capes
rigid, which stand still. Held at 1,300. A cloth that went back and forth by
a pixel would pop most of the pixels it covers in every frame.

The whole picture's count is printed for reading and holds nothing: 7,337
pops with the cloth, 3,367 with the capes rigid. The difference is the edges
and shadows of capes that move where the rigid ones stand still: made again
with the sun's shadow, the occlusion and the contact shadows set aside
(`--skip=shadow,occlusion,contact`, and `cloth` as well for the second run),
the two are 2,822 and 2,383, 439 apart.

## What this does not do

- Collide with itself, with another cloth, or with the terrain.
- Pin by a painted mask: the rig pins it.
- Move the tangents: a normal map on a cloth is lit by the tangents it was
  modelled with.
- Run on a phone yet: it is off below High there until measured.

## Proof

`engine/physics/tests/soft_body_tests.cpp`: a sheet hangs from what pins it
at the length it was made, trails through still air when carried through its
face and settles under its joint, is pushed by a wind, rests on a capsule
without passing through, and is put somewhere else whole.

`engine/render/tests/cloth_tests.cpp`: a cape hangs from its pin at its length
in the frame's rows, both of its sides one cloth; rests on the body's
colliders under a gale; settles in the same place at 30, 60 and 144 frames a
second; put somewhere else is at rest, and is not stepped when off at the
level or on the instance; lies on a wall, out of it and still; moves smoothly
in still air, a wind and gusts; and what one hero and twenty cost.

`cloth_gate`: the example's three capes stepped at High and none at Low, and
the pops above. `examples/46-cloth-cape`, and its picture.
