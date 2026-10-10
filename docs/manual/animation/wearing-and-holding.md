# Wearing and holding

A hero is a body, and what it wears and holds: a breastplate, a hood, hair, a
bow whose string the draw pulls. The pieces are separate models, they are
swapped while the game runs, and they have to move as one thing -- the same
joints in the same places on the same frame.

That is one property: `MeshPart.PoseFrom`.

`examples/37-character` has both kinds: a head with a hood that is taken off,
and a crossbow handed from one hand to the other.

## A piece that is worn

Export the piece on the body's skeleton -- a file with the skeleton and one
mesh, as modular characters are made -- and name the body in it:

```luau
--!strict
local body = workspace.Hero.Body :: MeshPart

local hood = Instance.new("MeshPart")
hood.MeshContent = "asset://models/rogue_hood.glb"
hood.CanCollide = false
hood.PoseFrom = body
hood.Parent = workspace.Hero
```

A mesh that names another in `PoseFrom` is a **follower**, and the other is
its **leader**. A follower is not posed from clips. Its joints are its
leader's, joint for joint:

- **Whatever moved the leader's joint moved the follower's**: a clip, a
  graph, a hand reaching, feet on a stair, a head that looks, a spring chain,
  a ragdoll. A seam cannot open, because there is no second pose to disagree.
- **Joints are matched by name**, and by role across rigs that name them
  differently ([Retargeting](manual:animation/retargeting)). A piece may have
  only part of the skeleton -- a glove has a hand and no arm.
- **A follower is drawn in its leader's place.** Its own `CFrame` is not
  asked while it follows; it needs no weld and cannot be a frame behind.
- **It keeps what is its own**: its `SpringBone` chains -- hair, a tasset --
  run after the leader's joints are in place, and its shape keys.

Taking it off is `hood:Destroy()`, or `hood.PoseFrom = nil` to have it back as
a mesh of its own, where its `CFrame` says. Changing one piece for another is
making the new one and destroying the old; the frame after, it is on the
body.

## A thing that is held

A bow, a rifle with a slide, a fishing rod: a model with joints of its own
and none of the body's. Nothing in it says where it is -- so it is hung from
a `Bone`:

```luau
--!strict
local body = workspace.Hero.Body :: MeshPart

local hold = Instance.new("Bone")
hold.JointName = "hand.r"
hold.Parent = body

local bow = Instance.new("MeshPart")
bow.MeshContent = "asset://models/bow.glb"
bow.CanCollide = false
bow.PoseFrom = body
bow.Parent = hold -- under a bone of its leader: it hangs from that joint
```

A follower parented under a `Bone` of its leader has its root on that joint,
at the bone's own `CFrame`. **Handing it to the other hand is parenting it
under the other hand's bone.**

**Its own joints are moved by the clips the body plays.** A joint only the
follower has rests where its file put it, unless a track of the clip being
played names it -- then that track moves it. So the string of a bow is a
joint called, say, `String`; the rig the clips were made on has a `String`
joint under its hand, as rigs are made with prop joints; and the draw clip
keys it beside the arms. One `AnimationPlayer`, one graph, one set of
events, and the string is in time with the hand because it is the same clip.

## What a follower is not

- **It has no body.** It collides with nothing, nothing touches it and a ray
  passes through it, whatever its `CanCollide`, `CanTouch` and `CanQuery` say:
  it is picture on its leader, and what is hit is the leader or the character
  that carries it. A character can never stand on what it wears. Something
  that must be hit apart from its wearer -- a shield that blocks -- is a
  `Part` welded to a `Bone`.
- **It has no animation of its own joints.** An `AnimationPlayer` under a
  follower plays its shape keys' weight tracks and no joint.
- **It is not a chain of command.** A follower may itself be followed, and
  the chain is resolved to its first leader; a mesh that names itself, a
  loop, or a leader with no skeleton follows nobody and says so once.

## What it costs

A piece worn is its skinning and its draw. No track is walked for it when
every joint it has is matched or at rest -- its matrices are copied from the
leader's. One with joints of its own that a playing track names is walked
for those tracks only.

Measured on a body of forty joints with five pieces of twelve: about a tenth
of a microsecond a tick a piece, half of what posing the same piece under a
`Model` costs. Fifty dressed heroes, each at its own moment of its clip, are
a twentieth of a millisecond a tick. Pieces of one mesh and material draw as
one run, as any skinned meshes do.

## In a match, and in a replay

`PoseFrom` is a reference like any other: it travels when the instance is
made and when it changes, and nothing travels a tick. Each machine makes a
follower's pose from its own copy of the leader's. A `Bone` on a follower's
own joint -- a muzzle on a slide -- reads on the tick what a `Bone` on a body
reads, and reproduces in a replay.

## In a scene

A worn piece in a scene file is a `MeshPart` with `PoseFrom` written as the
path of its leader, and it stays in the scene with its leader: a streamed
world does not take it into a cell.

## Where to look next

- [Skeletal animation](manual:animation/skeletal)
- [Retargeting: one clip on many bodies](manual:animation/retargeting)
- [Capes, tails and hair](manual:animation/secondary-motion)
- [`MeshPart`](api:MeshPart) · [`Bone`](api:Bone)
