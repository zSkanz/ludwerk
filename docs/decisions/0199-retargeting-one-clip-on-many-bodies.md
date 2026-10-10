# 0199 — Retargeting: one clip on bodies of other proportions

- Status: accepted; built in the animation batch of the level-up roadmap
- Date: 2026-10-10
- Decided by: the owner (the roadmap's first category: "one clip on several
  skeletons" -- his own case is several heroes of different builds on one
  set of clips); the orchestrator (aliases that cover the names clips really
  arrive with, an explicit map for a rig none fits, a warning that names
  what was left unmapped); detailed by the agent from the code
- Builds on: S6.8 (a clip named from another file plays on this rig), the
  joint map by name, `@engine/ragdoll`'s humanoid profile, R10

## The question

A walk is bought, or made once, on one skeleton. The game has a tall hero,
a short one and a broad one, each exported from another tool with its own
joint names and its own bone lengths. The clip has to move all of them.

## What is there

Read before this was written. A clip from another file is played on a rig
by matching joints **by exactly equal names** (`jointMapFor`), cached a
pair of rigs. Nothing else: the clip's translations are written as they
are, so the body takes the SOURCE's bone lengths wherever a clip keys
translation; a rest pose turned differently -- another bone roll, an A
where the clip's rig stood in a T -- is not accounted for; a rig whose
joints are called `mixamorig:LeftArm` where the clip's are `upperarm_l`
matches nothing and stands still. A joint has a name, a parent, a rest
transform from its parent and an inverse bind; no length, no role.

## Decision

A clip is carried across by **what each joint is**, not by what it is
called, and as **turns from rest**, not as transforms.

### Roles

A rig's joints are given **roles** when it is loaded: hips, spine (up to
three), chest, neck, head; each arm's shoulder, upper arm, lower arm and
hand; each leg's upper leg, lower leg, foot and toes; and the fingers,
three joints each, where the rig has them.

A role is found by the joint's name against **lists of the names rigs
really arrive with** -- the engine's own, kept as data beside the ragdoll's
profile, which already reads four of these families:

- the common free motion library's (`mixamorig:LeftUpLeg`, with or without
  its prefix);
- the two big engines' exported humanoids (`LeftUpperArm`; `upperarm_l`,
  `thigh_r`, `spine_01`, `pelvis`);
- the common Blender rigs' deform bones (`DEF-upper_arm.L`, `upper_arm.L`,
  `thigh.L`, `shin.R`, `forearm.L`);
- plain English either way round (`LeftArm`, `Arm_L`, `l_arm`).

A name is compared with its case, its separators and a rig's own prefix
taken off. **A rig no list fits says its roles in a file beside it**,
`<model>.rig.json` -- a role to a joint's name -- which is also how one
wrong guess is put right. And **what was not found is said**: a warning
that names each role the clip moves and the rig has no joint for, once a
pair of rigs.

Two rigs with roles are matched role to role. Joints with no role -- a
tail, a cape, a weapon bone -- are still matched by equal names, as now,
and so is everything when either rig has too few roles to be a body
(a shirt on the body it was cut for, a creature on its own clips).

### What is carried across

For a matched joint, each tick:

- **The turn, from rest.** The clip says where the source joint is turned
  to; what is taken is how far that is from the source's rest, and it is
  given to the target joint from the target's rest -- through each rig's
  own rest frame, so a bone that rolls differently about its own length,
  or was drawn along another axis, turns the same way in the world.
- **The stance.** Where the two rigs REST differently -- arms straight out
  on one, lowered on the other -- each limb's rest direction is compared
  once, when the pair is first met, and the difference is taken out, so a
  clip made on the first does not hold the second's arms out by the angle
  between them.
- **The hips' travel, scaled.** The one translation that is motion and not
  build: how far the hips move from their rest, multiplied by how tall the
  target's legs are against the source's. A short hero's hips bob less and
  its feet meet the ground.
- **Nothing else of translation, and nothing of scale.** Every other joint
  keeps the target's own rest offset: its own bone lengths, which is what
  makes a tall body tall under a clip made on a short one.

All of it is worked out once a pair of rigs -- two quaternions a joint and
one number for the hips -- and costs a clip two multiplications a channel.

### The API

- Nothing new to play one: `LoadAnimation("asset://clips/walk.glb#Walk")`
  already names a clip in another file.
- `AnimationPlayer.Retargeting: Enum.Retargeting` -- `Automatic` (by roles
  where both rigs are bodies and are not one skeleton, by name otherwise)
  and `ByName` (as it was). A third value, `Off`, was drawn and not built:
  by name the clip's transforms already are as they are, and two names for
  one behaviour is a question nobody could answer.
- A graph (ADR 0197) names clips the same way, and a graph made for one
  hero plays on another by naming the same clip library.

### Which side of the hash (R10)

**The simulation's**, with the pose: what is carried across is a pure
function of the two rigs and the clip, the same on every machine and every
run, with nothing of a frame or a clock in it. The map for a pair is built
when the pair is first played and rebuilt when either rig is loaded again.

### In the editor

With a mesh selected, the rig's roles are listed beside its joints -- which
joint was taken for the left upper arm, and which roles found none -- so a
rig that moves wrongly is read, not guessed at.

## What this does not do

- **No feet pinned to the ground by the retarget itself.** Scaling the hips
  keeps them close; a foot that must not slide is `FootPlacement`'s
  (ADR 0198), which is picture.
- **No hands kept where the source's hands were**: a clap made on long arms
  does not meet on short ones. That is a limb reaching for a place, and
  also ADR 0198's.
- **No bodies that are not bodies**: a horse's walk does not go on a dog by
  roles. Equal names still work.
- **No baked copy**: nothing writes a retargeted clip to disk.

## Proof

Unit tests on two rigs made to differ in every way that matters -- other
names from two of the lists, bones drawn along another axis, another
stance, legs half as long: a clip made on the first raises the second's
arm to the same place in the world, its feet reach the ground, its bone
lengths are its own, and a role the clip moves that the second lacks is
named in one warning. A rig's `.rig.json` overrides a guess. By name is as
it was (the existing cases hold unchanged). `examples/37-character` is
three heroes of other proportions on one library of clips.
