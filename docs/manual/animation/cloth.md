# Cloth

A `Cloth` makes a mesh hang like cloth: every vertex of it is simulated,
pulled by gravity and the wind, kept at its distance from its neighbors, and
kept out of the body it rests on and the walls near it. Where a
[`SpringBone`](manual:animation/secondary-motion) swings a few columns of
joints, a cloth folds, fills and drapes.

`examples/46-cloth-cape` is three caped figures: one that runs, stops dead,
dashes and is teleported, one standing in the wind, and one with its back to
a wall the wind blows its cape onto.

## A cape in a few instances

The cape is a mesh of its own that wears the body's pose, with a `Cloth`
under it:

```luau
local body = workspace.Hero.Body :: MeshPart

local cape = Instance.new("MeshPart")
cape.MeshContent = "asset://models/cape.gltf"
cape.PoseFrom = body              -- drawn on the body, with its joints
cape.CanCollide = false
cape.Parent = workspace.Hero

local cloth = Instance.new("Cloth")
cloth.PinJoints = "CapePin"       -- the joint the cape hangs from
cloth.Parent = cape

-- What the cape rests on: capsules on the body's joints, as for spring bones.
local back = Instance.new("SpringCollider")
back.JointName = "Spine"
back.Length = 1.2
back.Radius = 0.2
back.Parent = body
```

**A vertex weighted at least half to a joint `PinJoints` matches is pinned**:
it stays exactly where the animation puts it, and the cloth hangs from it.
Every other vertex hangs free, never further than `MaxDistance` from where
the animation would put it. `PinJoints` is a pattern, as `JointPattern` is:
`Cape*` takes `CapeLeft` and `CapeRight`.

## What the model needs

1. **Model the cape as its own mesh**, hanging straight down behind the body,
   a few centimeters off it. A grid is best: a row of vertices every ten
   centimeters or so. Under a few hundred vertices is plenty for a cape; the
   most a cloth may have is 8,192, and a larger mesh is drawn as its animation
   has it.
2. **Skin it to the body's armature**, and give it one more joint for the
   cape's top -- `CapePin` in the example, a child of the upper spine at the
   shoulders.
3. **Weight the top row to that joint** and nothing else.
4. **Weight the rest as the cape would be weighted with no cloth**: to the
   spine, or blended down the back. That is where it is drawn when the cloth
   is off, at a lower quality level or far away, and where it hangs from when
   it is on.
5. **Export as glTF with the armature.** A cape modeled on both of its sides
   -- a back sheet at the same corners as the front -- is one cloth: the
   engine takes vertices at the same place as one point.

`examples/46-cloth-cape/tools/make_models.py` draws such a cape and the body
it hangs on.

## The dials

| Property | What it does |
| --- | --- |
| `PinJoints` | The joints the cloth hangs from, by a pattern of their names. |
| `MaxDistance` | How far, in meters, a free vertex may move from where the animation puts it. A cape that must not fly over the head has a small one. |
| `Stiffness` | How little it stretches, 0 to 1. Canvas is near 1; knitted cloth lower. |
| `BendStiffness` | How little it folds, 0 to 1. Silk is near 0, leather near 1. |
| `Damping` | How much of its motion it loses a second. Low flutters on; high settles at once. |
| `GravityScale` | How much of the world's gravity it feels. |
| `WindInfluence` | How much of `Workspace.GlobalWind` pushes it. A ten meter a second wind through a cape pushes it as hard as gravity pulls it, as it does a spring chain. |
| `Thickness` | How far it stays from what it rests on. |
| `CollidesWithWorld` | Whether the anchored parts near it keep it out: the wall it is pressed against, the floor it drags on. |

A cloth is pushed by the air it moves through as well as by the wind: a
runner's cape streams behind, and one carried edge-on barely trails, as cloth
does.

## What it rests on

- **Its body, through `SpringCollider`s**: those under its own mesh, and those
  under the mesh it wears the pose of. The body's own mesh is not a wall to
  it -- a model's own parts are what its colliders describe -- so cover the
  back with capsules side by side, as for a [spring cape](manual:animation/secondary-motion).
- **The anchored parts near it**, when `CollidesWithWorld` is on: walls,
  floors, a door a script swings. It never pushes them; a cape cannot move a
  crate.
- Not other cloths, not itself, not the terrain.

## It is picture, not simulation

Each machine hangs its own cloth, at its own frame rate, after the pose is
sampled, in a physics world of its own that is never the game's. The cloth is
not replicated and not in a replay, and a server with no window does none of
it. **The instance is sent; where its vertices are is not**: a cape a server
script gives a character hangs on every machine, each its own way. Nothing in
a game's rules should read where a cloth is.

It settles in the same place at 30, 60 and 144 frames a second. A hitch or a
teleport puts it back where its animation has it, at rest; a dash trails it.

## What it costs, and where it is off

Measured on the development build: a cape of 252 vertices, 0.13 ms a frame;
twenty of them, 0.67 ms. A cloth further from the camera than the quality
level's distance, or one the camera did not reach, is not stepped, and is
drawn as its animation has it.

**Cloth is off at the Low level, and on a phone or a handheld below High**,
until it is measured on one: there the cape is drawn as its animation has it.

## Where to look next

- [Capes, tails and hair](manual:animation/secondary-motion) -- spring chains, and the colliders both use
- [Wearing and holding](manual:animation/wearing-and-holding) -- `PoseFrom`
- [`Cloth`](api:Cloth)
