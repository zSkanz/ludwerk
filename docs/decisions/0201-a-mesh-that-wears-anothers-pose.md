# 0201 — A mesh that wears another's pose

- Status: accepted; built in batch 1b of the level-up roadmap (protocol 45)
- Date: 2026-10-10
- Decided by: the owner (a body with a skeleton picks up a weapon in play and
  the body's animation moves the weapon's own joints too; and the modular
  character -- armour, clothes, hair swapped at run time); the orchestrator
  (the shape: a mesh follows a leader joint by joint, joints only it has are
  driven by the leader's tracks, no second pose, the same side of the hash);
  detailed by the agent from the code
- Builds on: ADR 0197 (graphs), ADR 0198 (IK is picture), ADR 0199 (roles),
  ADR 0194 (spring chains), ADR 0196 (shape keys), H10 (shared poses), R10

## The question

A hero is a body, and what it wears and holds: a breastplate skinned to the
body's skeleton, hair, a bow whose string the draw pulls. The pieces are
swapped while the game runs. They must move as one thing -- the same joints
in the same places on the same frame -- and a bow's own joints must be moved
by the clip the body is playing.

## What is there

Read before this was written; the first half of both cases already works.

- **A player parented to a `Model` drives every skinned mesh under it**, found
  again every tick, so a piece parented under the model at run time moves on
  the next. Each mesh is posed **separately**, from the same tracks, its
  joints matched to the clip's by name (by role across rigs, ADR 0199).
- So a breastplate under the hero's model follows the body's clips today, and
  a bow under it is moved by whatever tracks of the playing clip name its
  joints. What does not hold:
  - **The frame's corrections are a mesh's own.** `IKControl`,
    `FootPlacement`, a spring chain and a ragdoll write the joints of the one
    mesh they are on (`present`, `setJointOverride`). The body's feet find the
    stair and the greaves stay where the clip had them: the seam opens
    exactly when the picture is corrected.
  - **A piece with part of the skeleton is wrong.** A glove has the hand's
    joints and none of the arm's; posed on its own, its hand hangs from
    nothing.
  - **Each piece is a pose.** Six pieces are six passes over the tracks, and
    H10 shares a pose only between meshes of the same rig.
  - It needs the player on a `Model` above both. A player on the body's own
    `MeshPart` drives nothing else.

## The decision

### `MeshPart.PoseFrom`

A reference to another `MeshPart`, the **leader**. A mesh that names one is a
**follower**: it is not posed from tracks. Its pose is taken from its
leader's, joint by joint.

- **A joint both have is the leader's, exactly**: the same matrix in the
  leader's space, on the same tick and in the same frame's picture. Matched
  **by name**; where the names differ and both rigs have roles, by role (the
  map of ADR 0199, built once a pair of rigs and kept). What moved the
  leader's joint does not matter -- a clip, a graph, a limb reaching, feet on
  a stair, a spring chain, a ragdoll -- the follower's joint is there too.
- **A joint only the follower has** rides its parent: at rest from it, unless
  a track playing on the leader names it -- then it is moved by that track.
  A bow's string is pulled by the draw clip the body plays, through one
  `AnimationPlayer`, one graph and one set of events. A clip names such a
  joint the way it names any: it is in the clip's own rig (a prop joint under
  the hand, as rigs are commonly made).
- **A follower is drawn in its leader's place.** Where its own part stands
  is not asked for the picture; the leader's is. A piece need not be welded
  for a seam to stay shut, and cannot open one by being a frame behind.
- **A held thing that shares no joint with its leader hangs from a `Bone`.**
  A bow has a grip and a string and none of the body's joints: nothing says
  where its root is. `Bone` already is the one way to say "on this joint",
  so a follower **parented under a `Bone` of its leader** has its root -- and
  every joint of its own with no parent -- on that joint, at the bone's
  offset, in the tick and in the frame. Its named joints still take the
  leader's tracks: the string is pulled by the draw clip. Handing the bow to
  the other hand is parenting it under the other hand's `Bone`. A follower
  that shares joints with its leader and is under a `Bone` as well is placed
  by its shared joints; the bone is not asked.
- A follower may itself be followed; the chain is resolved to its first
  leader. A mesh that names itself, a loop, or a leader with no skeleton
  follows nobody and is posed as any mesh is.
- **What a follower keeps of its own**: its `SpringBone` chains (hair, a
  tasset), run after its leader's joints are in place, and its shape keys.
  An `AnimationPlayer` under a follower plays its weight tracks and no joint.

Set and cleared at run time like any property. The frame after, the piece is
on the body; cleared, it is a mesh posed on its own again.

### Physics and queries

**A follower has no body.** It collides with nothing, nothing touches it and
a ray passes through it, whatever its `CanCollide`, `CanTouch` and `CanQuery`
say -- it is picture on its leader, and what is hit is the leader, or the
character that carries it. So it can never be what its wearer stands on:
D615 was a carried part taken for ground, and a piece with no body is not in
that question at all. Its own `CFrame` is left as it was written and means
nothing while it follows; cleared of its leader, it is a part again, where
its `CFrame` says. A thing that must be hit apart from its wearer -- a
shield that blocks -- is a `Part` on a `Bone`, as before.

### Cost

A follower costs its skinning and its draw. **No tracks are walked for it**
when every joint it has is matched or at rest -- armour, clothes, hair: its
matrices are copied. One with joints of its own that a playing track names
is walked for those tracks only.

- **H10**: a leader's pose is shared as before. A follower's matrices are
  made from its leader's pose and its own rig, so two heroes that share a
  pose share each piece's too: the signature is the leader's and the
  follower's rig.
- **The instanced skinned run** takes followers like any skinned mesh: each
  has a palette, and pieces of one mesh and material draw as one run.
- **Measured** (`pose_follow_tests.cpp`, the mean of a tick's pose pass over
  three hundred ticks, three runs, the machine otherwise idle): a body of
  forty joints on a clip that turns every one, with five pieces of twelve
  joints each, posed one tick in three as a body of that size on the screen
  is. One hero: 1.4 microseconds a tick alone, 2.9 to 3.2 with its five
  pieces worn, 3.1 to 3.4 with the same five posed under a model as they
  were before. Fifty heroes, each at its own moment of the clip: 27
  microseconds alone, 53 to 55 worn, 77 to 78 posed -- a piece worn is about
  a tenth of a microsecond a tick, half of what posing it costs, and the
  whole dressed crowd is a twentieth of a millisecond. Fifty at ONE moment,
  where the bodies share a pose and the pieces theirs: 20 microseconds.
  The saving is the tracks not walked; what a worn piece still costs is its
  joints' matrices copied and its palette made.

### Seen, and posed

A follower in view is a reason to pose its leader, whatever is seen of the
leader -- a body that is not drawn under a full suit is the ordinary modular
character. A follower is posed on the ticks its leader is.

### Simulation, or picture

The same as the leader's, joint for joint. The tick's pose of a follower is
made from the tick's pose of its leader, so a `Bone` on a follower's own
joint -- a muzzle on a slide -- reads on the tick what a `Bone` on a body
reads, reproduces in a replay, and is in the hash exactly as far as the
leader's pose is. The frame's corrections reach the follower's picture and
not its tick, as on the leader (ADR 0198).

### In a match

`PoseFrom` is an authored reference: it travels when the instance is made
and when it changes (protocol 45), and nothing travels a tick. Each machine
makes the follower's pose from its own copy of the leader's.

## Decided while it was built

- **A follower has a skeleton of its own.** A mesh with none that names a
  leader follows nobody: it has no joints to put anywhere, and is a rigid
  thing -- a `Bone`'s job.
- **By name first; by role only where both rigs are bodies.** A role guessed
  for a bow's joint must not land it on a hip.
- **Held means its parent is a `Bone` of the first leader itself**, not of
  another piece the leader wears.
- **`IKControl` and `FootPlacement` under a follower do not run**: its joints
  are its leader's, and a control on the leader is what reaches for both.
  `Bone.Transform` on a follower turns only joints of its own.
- **A piece a weld or a joint holds keeps its body**: the constraint needs
  one to hold. A piece that is only worn needs neither.
- **Known, and not done**: in the editor with the world not ticking, nothing
  poses, so a piece put on is drawn at its file's rest (a held bow at its
  leader's origin) until the world runs.

## Not decided here

- **Hiding what a piece covers.** A body under a breastplate still draws, and
  may poke through. Masking the covered faces is a modular character's next
  need and a feature of its own.
- **Merging pieces into one mesh** when a character's set stops changing: a
  draw a piece is what this costs, and a crowd of dressed characters will
  want one.

## What it leaves

`examples/37-character` gains both cases: the hero picks up and puts down a
bow whose string the draw clip pulls, and swaps a piece of armour skinned to
the body. Tests: a follower's shared joints equal its leader's on the tick
and in the frame, under IK and under a spring chain; a piece with part of a
skeleton; a joint of its own moved by the leader's track; attach and detach;
a leader out of view under a follower in view; two processes, the piece on
the joiner; determinism.

## Amended 2026-10-10: found building the example

- **A worn piece stays in its scene** (D626). A scene's partition takes a
  part with nothing under it into a streamed cell, and a cell's record has
  nowhere to put a reference: a piece in a scene file came back following
  nobody. A part that points at something by path now stays, as what it
  points at always did.
- **The example holds a crossbow.** The public-domain library the example
  was remade on has a shot and a reload for a crossbow and no draw for a
  bow. Its rig was given a `String` joint under the hand's slot, keyed in
  those two clips; the crossbow is a file with `Crossbow` and `String` and
  hangs from a `Bone` on that slot. The acceptance is the same: a joint only
  the held thing has, moved by the clip the body plays, and handed to the
  other hand by parenting it under the other hand's bone.
