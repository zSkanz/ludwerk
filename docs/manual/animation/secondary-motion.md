# Capes, tails and hair

A clip moves a character's body. What hangs off the body -- a cape, a tail, a
braid, a tassel -- is moved by the body, and no clip can know where the body
is going. A `SpringBone` makes a chain of the rig's joints trail behind
whatever carries it, on top of the clip that is playing.

`examples/35-cape` is a figure that runs, turns, stops dead, dashes and is
teleported, beside one that stands in the wind.

## A cape in four instances

```luau
local hero = workspace.Hero :: MeshPart

-- A chain a column: the joint named is the top of the column.
for _, column in { "Cape_L0", "Cape_M0", "Cape_R0" } do
    local spring = Instance.new("SpringBone")
    spring.RootJoint = column
    spring.Parent = hero
end

-- And what the cape must not pass through.
local back = Instance.new("SpringCollider")
back.JointName = "Spine"
back.Length = 1.2
back.Radius = 0.2
back.Parent = hero
```

**`RootJoint` stays where the animation puts it and turns; every joint below
it swings.** So name the top joint of each column, the one on the shoulders.
One `SpringBone` on a joint that all the columns descend from moves them all
too, but swings the whole cape about that one point -- right for a tail,
wrong for a cape.

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
   `Cape_M0`, `Cape_R0` and `Cape_L1` ... below them. The name of the top
   joint is what `RootJoint` takes.
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

Then, in the engine, one `SpringBone` a column, and a `SpringCollider` or two
on the body's joints -- usually one for the back and one a leg.

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
sampled. They are not replicated and not in a replay, and a server with no
window does none of it. **Nothing in a game's rules should read where a cape
is**: a `Bone` on a cape's joint follows the clip, not the swing.

It looks the same at 30, 60 and 144 frames a second, and it does not fly
apart: a hitch or a teleport leaves the cape hanging at rest where the
character now is. A dash trails it.

## What it costs

Nothing for a body with no `SpringBone`. For one that has, about three
microseconds a chain a frame; a body the camera did not reach, or one further
than sixty metres from it, is not stepped at all. Quality `low` steps at half
the rate and half as far.

## What it does not do

A cape goes through a wall, and does not collide with itself: it collides
with the `SpringCollider`s of its own mesh and nothing else. A cloth with a
mesh of its own is planned on the same colliders and the same dials.

## Where to look next

- [Skeletal animation](manual:animation/skeletal)
- [`SpringBone`](api:SpringBone) · [`SpringCollider`](api:SpringCollider)
