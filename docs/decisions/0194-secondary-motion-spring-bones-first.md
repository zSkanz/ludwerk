# 0194 — Secondary motion: spring bones first, cloth on the same colliders later

- Status: accepted
- Date: 2026-10-09
- Decided by: the owner ("something we need urgently is a way to simulate
  cloth in the engine: I am adding characters that have capes ... they are
  heroes"); shaped by the orchestrator as two tiers, the cheap one first;
  detailed by the agent from the code
- Builds on: the skeletal animation system and its override stage (the
  ragdoll's), ADR 0115 (the workspace's wind), ADR 0134 (drawn positions),
  ADR 0147 (graphics settings), R10

## The question

A hero has a cape. An animator can key a cape for a walk cycle and it will be
wrong the moment the hero turns, stops, dashes or stands in a wind. What moves
a cape is the body that carries it, and that is not in any clip.

## Decision

**Two tiers, and the first is built now.**

1. **Spring bones**: a chain of the rig's own joints, each kept at its length
   from its parent, carried by its own motion, pulled back towards where the
   clip has it, never bent from that by more than a limit, and kept out of a
   few capsules tied to the body's joints. This is what most games ship for
   capes, hair, tails and tassels, and it holds on a phone.
2. **Cloth**: a mesh of its own on Jolt's soft bodies -- already in the tree,
   no new dependency -- pinned to joints. Built later, and only if the first
   is not enough. It shares the first tier's colliders, wind and property
   names, so a game that outgrows chains changes one instance and not its
   whole setup.

### It is picture, not simulation

It runs on each machine after the pose is sampled, at the rate frames are
drawn. It is not replicated, not in the world's hash, not in a replay, and a
host with no window does none of it. R10 is untouched because nothing the
simulation reads is written.

That last sentence is a constraint the code imposed on the design. The
animation system has a stage that substitutes joints after sampling -- the
ragdoll's -- and the first plan was to be its second writer. But a `Bone` is an
attachment the physics tick places from the pose, so a cape written into the
pose would be read back by the simulation at one machine's frame rate. So what
a chain moves is a **copy of the pose, kept for drawing alone**
(`AnimationSystem::present`, `drawnPose`): `extract` asks for it, and
`jointModel`, the sockets and the ragdoll never see it. Bodies with no spring
bone keep sharing poses and palettes exactly as before; the copy exists only
for a body that has one.

### The instances

`SpringBone`, parented to the `MeshPart` whose rig has the chain:
`RootJoint` (the joint the chain hangs from: it stays where the animation
puts it and turns; its descendants swing), `JointPattern` (every joint whose
name it matches, `*` for any run of characters, that is not below another it
matches: each the top of a chain of its own, so one instance moves a whole
cape), `Enabled`, `Stiffness`, `Damping`,
`GravityScale`, `Inertia` (how much of the body's own motion the chain is left
behind by), `LimitAngle`, `Radius`, `WindInfluence`.

`SpringCollider`, parented to the same `MeshPart`: `JointName`, `Radius`,
`Length` (a capsule along the joint's up; nought is a ball), `Offset`. Every
collider of a mesh applies to every chain of that mesh; a list on each chain
was considered and is not worth a property a game would always fill the same
way.

**What an artist makes**: three or four columns of four or five joints for a
cape, each column a chain under a top joint on the shoulders, and one
`SpringBone` whose pattern is those tops. Naming a joint that all the columns
descend from also works, and swings the whole cape about that one point,
which is right for a tail and wrong for a cape.

### It looks the same at any frame rate and never flies apart

One rule gives both: fixed steps of a hundred and twentieth of a second, at
most four a frame, and what four do not cover is dropped rather than
integrated. A frame of a quarter of a second or more is a hitch: the chain is
put back as the animation has it, at rest. And a first joint that moves
further in one frame than twice the chain's own length was not carried there
-- it was put there -- and gets the same treatment, which is what a teleport
needs. A dash is under that and trails.

### Cost is bounded by what is seen

Nothing is stepped for a body the renderer did not reach the frame before, or
one further from the camera than `secondaryMotionDistance`. A chain that
stops being stepped is forgotten, so it starts from rest when it returns.
Quality `low` halves the distance and takes the solver's coarse step, a
sixtieth of a second in place of a hundred and twentieth. (It first stepped
on every other frame, and measured DEARER than `high`: the frame between
still has to fit the chain to where the body went, and the stepping frame
then takes twice the steps. The solver is three fifths of a chain's cost, so
the step is what halves it: 0.59 to 0.33 ms for a hundred bodies in the
development build.) A body with no `SpringBone`
costs nothing, and a horde of four hundred pays only for the ones that have
one.

Measured on the development build, with no window: the example's two figures
(6 chains, 30 joints) 0.02 ms a frame; a hundred figures (300 chains, 1,500
joints) about 0.9 ms a stepped frame, three microseconds a chain.

## What this does not do

- No cloth mesh, no self-collision, no collision with the world: a cape goes
  through a wall. Tier 2.
- A mesh stretched unevenly (`Size` against `MeshSize`) has its chains sized
  by the mean of the three.
- The editor shows both classes in Properties, with their own icons and the
  rig's joints offered for `RootJoint` and `JointName`. **The chains move
  while editing too**: the world the editor draws is the host's own, and a
  frame steps its chains whatever the run state -- a cape hangs, feels the
  workspace's wind, and swings when its body is moved. That is held by a test
  of exactly that case (a still body, nothing playing, no script:
  `spring_bones_tests.cpp`), since the editor needs a window and no test
  opens one; it has not been looked at in the editor's own viewport by the
  one who wrote it.
- No `[graphics]` key of its own yet: the rate and the distance follow the
  quality level.

## Proof

`engine/render/tests/spring_chain_tests.cpp`: a chain hangs where the
animation has it and keeps its length; dragged and released it settles in the
same place at 30, 60 and 144 frames a second; it trails and never further than
its length; half a second in one frame, and a teleport, leave it at rest and
quiet afterwards; it never bends past its limit; it stays out of a capsule;
its first joint turns to face its child.

`engine/render/tests/spring_bones_tests.cpp`: a cape on a still body with
nothing playing moves in the wind in the pose that is DRAWN, each joint still
its length from the one above and the top where the animation has it -- while
the pose the simulation reads is untouched; switched off, or naming a joint
the rig lacks, it is drawn as the animation has it; and one instance with a
pattern is a chain for each column, whether the pattern names the tops or
every joint of the cape.

`secondary_motion_steps`: the example run for a second and a half, its six
chains of thirty joints stepped in a real frame, with no warning.

The determinism replays reproduce unmoved.

`examples/35-cape`: a figure that runs a circle, stops dead, dashes, and is
put somewhere else in one frame, beside one that stands in the wind. Its
figures, their capes and the wind are a scene on disk, so it opens in the
editor with the capes already on.
