# Retargeting: one clip on many bodies

A walk is bought, or made once, on one skeleton. The game has a tall hero, a
short one and a broad one, each exported from another tool with its own joint
names and its own bone lengths. **The same clip moves all of them**, with
nothing to set up for the common case:

```luau
--!strict
-- A clip in another file, on this hero's own skeleton.
local walk = animator:LoadAnimation("asset://clips/humanoid.glb#Walk")
walk.Looped = true
walk:Play(0.2)
```

An [animation graph](manual:animation/graphs) does the same for every clip it
names, when it names a `library`: a graph made for one hero plays on another.

## What is carried across

A clip is carried by **what each joint is**, not by what it is called, and as
**turns from rest**, not as transforms.

- **The turn, from rest.** What is taken from the clip is how far each joint
  is turned from where its own skeleton rests, and that turn is given to the
  matching joint from where IT rests. A bone that was drawn along another
  axis, or rolled differently about its own length, still turns the same way
  in the world.
- **The stance.** Where two skeletons rest differently -- arms straight out
  on one, lowered on the other -- the difference is taken out, so a clip made
  on the first does not hold the second's arms out by the angle between them.
- **The hips' travel, scaled.** How far the hips move from their rest is
  multiplied by how long the body's legs are against the clip's. A short
  hero's hips bob less, and its feet meet the ground.
- **Nothing else of where a joint is, and nothing of its stretch.** Every
  other bone keeps its own length -- which is what makes a tall body tall
  under a clip made on a short one.

## How a joint's role is found

When a model is loaded its joints are given **roles**: hips, spine, chest,
upper chest, neck, head; each arm's shoulder, upper arm, lower arm and hand;
each leg's upper leg, lower leg, foot and toes; and the fingers, three joints
each.

A role is found from the joint's name, against the names rigs really arrive
with -- with their case, their separators and a rig's own prefix taken off,
and a side read wherever it sits (`LeftArm`, `upperarm_l`, `thigh.L`,
`Bip01 L Thigh`). The families the engine knows:

- the common free motion library's (`Hips`, `Spine1`, `LeftForeArm`,
  `RightUpLeg`, with or without its prefix);
- the two large engines' humanoids (`LeftUpperArm`, `LeftLowerLeg`; and
  `pelvis`, `spine_01`, `upperarm_l`, `calf_r`);
- the usual Blender rigs' deform bones (`upper_arm.L`, `forearm.L`, `shin.R`,
  with or without `DEF-`);
- a biped's (`Pelvis`, `L UpperArm`, `R Calf`);
- plain English either way round (`LeftArm`, `Arm_L`, `l_arm`).

Twist bones, IK handles and other helpers are never given a role.

**A rig no list fits says its roles in a file beside it**, named as the model
is with `.rig.json` -- `hero.glb` and `hero.rig.json`:

```json
{
  "format": "rig",
  "version": 1,
  "roles": {
    "Hips": "Root_M",
    "LeftUpperArm": "Bone_014",
    "LeftLowerArm": "Bone_015",
    "Head": ""
  }
}
```

A role given a joint takes it, whoever had it; a role given `""` has none --
which is how one wrong guess is put right. The file is read with the model
and again when either is saved while `ludwerk dev` runs.

**In the editor**, Properties on a skinned mesh lists the roles its joints
were given, so a body that moves wrongly is read, not guessed at.

## When roles are used, and when names are

`AnimationPlayer.Retargeting` chooses:

| Value | What it does |
|---|---|
| `Automatic` | By roles, where **both** skeletons are bodies -- hips, a spine, and both arms and both legs, each with its upper and its lower bone -- and are **not one skeleton** under two files' names. By equal joint names otherwise. |
| `ByName` | By equal joint names only, the clip's transforms as they are. A joint one side lacks is skipped, and nothing is said. |

Two files that are one skeleton -- a body and the shirt cut for it, a model
and the clips exported from the same rig -- have the same joints, named the
same, resting the same; a clip between them is the rig's own and is carried
as it always was, its stretch and all.

Joints with no role -- a tail, a cape, a weapon bone -- are matched by equal
names in every mode.

**What the body lacks is said once.** Carried by roles, a clip that moves a
role the model has no joint for -- toes, on a rig with none -- is named in
one warning for that pair of files, with the roles. Those parts stay as the
model rests.

In a match `Retargeting` travels with the player: every machine carries the
clips the same way.

## What it does not do

- **It does not pin feet to the ground.** Scaling the hips keeps them close;
  a foot that must not slide is
  [`FootPlacement`](manual:animation/reach-and-look)'s.
- **It does not keep hands where the clip's hands were**: a clap made on long
  arms does not meet on short ones. That is a limb reaching for a place --
  an [`IKControl`](manual:animation/reach-and-look).
- **It does not carry between bodies that are not bodies**: a horse's walk
  does not go on a dog by roles. Equal names still work.
- **It writes nothing to disk**: a clip is carried as it is played.

## Where to look next

- [Animation graphs](manual:animation/graphs)
- [Skeletal animation](manual:animation/skeletal)
- [Meshes and models](manual:world/meshes)
- [`AnimationPlayer`](api:AnimationPlayer)
