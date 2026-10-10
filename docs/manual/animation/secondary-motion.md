# Capes, tails and hair

A clip moves a character's body. What hangs off the body -- a cape, a tail, a
braid, a tassel -- is moved by the body, and no clip can know where the body
is going. A `SpringBone` makes a chain of the rig's joints trail behind
whatever carries it, on top of the clip that is playing.

`examples/35-cape` is a figure that runs, turns, stops dead, dashes and is
teleported, beside one that stands in the wind.

## A cape in two instances

```luau
local hero = workspace.Hero :: MeshPart

-- The top joint of each column, by a pattern: each is a chain of its own.
local cape = Instance.new("SpringBone")
cape.JointPattern = "Cape_*0"
cape.Parent = hero

-- And what the cape must not pass through.
local back = Instance.new("SpringCollider")
back.JointName = "Spine"
back.Length = 1.2
back.Radius = 0.2
back.Parent = hero
```

**The top joint of a chain stays where the animation puts it and turns;
every joint below it swings.** So the tops are the joints on the shoulders,
one a column.

`JointPattern` finds them by name, `*` standing for any run of characters:
`Cape_*0` is `Cape_L0`, `Cape_M0` and `Cape_R0`. A joint below another the
pattern matches belongs to that one's chain, so `Cape_*` finds the same three
tops. `RootJoint` names one top outright -- a tail, a braid -- and the two can
be used together.

Naming a joint that all the columns descend from instead swings the whole
cape about that one point: right for a tail, wrong for a cape.

## What the model needs

Joints, and nothing else. In the modelling tool, give the cape three or four
columns of four or five joints, skin the cape to them as you would to any
joints, and export. There is no cloth to bake and no new file format; a model
already in the project needs no re-import to get a `SpringBone`.

### In the modelling tool, step by step

`examples/35-cape/content/models/figure.gltf` is the reference: open it beside
your own and compare the joints.

1. **Model the cape as part of the character's mesh**, or as a mesh skinned
   to the same armature. A flat sheet with a row of vertices every joint is
   enough; it need not be dense.
2. **Add a column of joints down the cape for each chain**: three or four
   columns across a cape, four or five joints down each. One column is enough
   for a tail or a braid.
3. **Parent the top joint of each column to the body joint the cape hangs
   from** -- the upper spine or the shoulders -- and each joint below to the
   one above it. A column is a chain: every joint has one parent, and no joint
   of one column is the parent of a joint of another.
4. **Put the top joint where the cape is attached**, on the shoulders. That
   joint does not move away from the body; it only turns. Everything under it
   swings.
5. **Name the joints so the top of each column is easy to find**: `Cape_L0`,
   `Cape_M0`, `Cape_R0` and `Cape_L1` ... below them. Names with a part in
   common are what lets one `JointPattern` -- `Cape_*0` -- take the whole
   cape; the name of one top joint is what `RootJoint` takes.
6. **Skin the cape to its joints**: each row of vertices to the joint at its
   height, blended across the columns and between rows as you would weight
   anything. The top row should also carry some weight of the body joint, so
   the cape does not come away at the collar.
7. **Leave the cape hanging at rest in the bind pose**, straight down. The
   rest pose is where the chain is pulled back to, and the direction
   `LimitAngle` is measured from.
8. **Do not key the cape's joints in your clips** unless you want that as the
   shape it returns to: whatever a clip does to them is the place the chain
   is pulled towards, frame by frame.
9. **Export as glTF with the armature**, as for any skinned model. Nothing
   else is baked.

Then, in the engine, one `SpringBone` with a pattern for the cape, and
`SpringCollider`s on the body's joints for what the cape rests on.

## Covering the body

A collider is a capsule: round, however wide the body is. **A cape is kept
out of the capsules, not out of the mesh**, so a chain that hangs outside
every capsule's reach goes through the body there -- and the usual way to
get that is one capsule down the middle of a back that is wider than it is
deep. The outer columns of the cape never meet it.

- **A wide back takes capsules side by side**, each as deep as the body is
  there, overlapping a little. The cape example has three across a block six
  tenths wide.
- **A leg takes one of its own**, on the thigh's joint, when the cape hangs
  below the hips: a capsule follows its joint, so it swings with the stride.
- **`Radius` on the `SpringBone` is half the cloth's thickness.** The cape
  lies that far off the capsules, so a cape that looks sunk into the body
  wants a larger one, and one that floats off it a smaller.

What is kept out is the whole link between two joints, not only the joints:
a cape of long links does not cut a corner of the body, and one that is
thrown at the body -- a dead stop out of a dash -- lands on the side it came
from. The body has the last word over `LimitAngle`: a chain that cannot be
both inside its limit and outside the body is outside the body.

## The dials

| Property | What it does |
| --- | --- |
| `Stiffness` | How strongly a joint is pulled back to its animated place. 0 is a rope, 1 does not move. A cloth is 0.1 to 0.3; a tail 0.4 and up. |
| `Damping` | How much of its motion a joint loses each step. Low swings for a long time; high settles at once and looks heavy. |
| `Inertia` | How much of the body's own motion the chain is left behind by. At 1 a cape streams out behind a run. |
| `LimitAngle` | The most a joint may bend from its animated direction, in degrees. It is what keeps a cape from folding through itself. |
| `GravityScale` | How much of the world's gravity it feels. |
| `WindInfluence` | How much of `Workspace.GlobalWind` pushes it. |
| `Radius` | How thick the chain is where it meets a `SpringCollider`. |

## It is picture, not simulation

Each machine steps its own chains, at its own frame rate, after the pose is
sampled. The swing is not replicated and not in a replay, and a server with no
window does none of it. **In a match the instances are sent, and the swing is
not**: a `SpringBone` and a `SpringCollider` a server-side script makes are on
every machine with what was authored on them, so a cape the server gives a
character swings for every player -- each machine its own swing. **Nothing in a game's rules should read where a cape
is**: a `Bone` on a cape's joint follows the clip, not the swing.

It looks the same at 30, 60 and 144 frames a second, and it does not fly
apart: a hitch or a teleport leaves the cape hanging at rest where the
character now is. A dash trails it.

## What it costs

Nothing for a body with no `SpringBone`. For one that has, about three
microseconds a chain a frame; a body the camera did not reach, or one further
than sixty metres from it, is not stepped at all. Quality `low` steps at half
the rate -- which measures at a little over half the solver's cost -- and
half as far.

## What it does not do

A cape goes through a wall, and does not collide with itself: it collides
with the `SpringCollider`s of its own mesh and nothing else. A cloth with a
mesh of its own is planned on the same colliders and the same dials.

## Where to look next

- [Skeletal animation](manual:animation/skeletal)
- [`SpringBone`](api:SpringBone) · [`SpringCollider`](api:SpringCollider)
