# 0198 — Inverse kinematics: a foot on the ground, a hand on a weapon, a head that looks -- as picture

- Status: accepted; built in the animation batch of the level-up roadmap
- Date: 2026-10-10
- Decided by: the owner (the roadmap's first category: "a foot on uneven
  ground, a hand on a weapon, a head looking at a target"); the orchestrator
  (picture and not simulation; what is drawn on a bone follows; a budget for
  feet); detailed by the agent from the code
- Builds on: ADR 0194 (the pose a frame is DRAWN with, and what may write
  it), ADR 0134 (where a thing is drawn), ADR 0147 (graphics settings), the
  skeleton's joints and their rest lengths, R10

## The question

A clip is made on flat ground, for one weapon, looking straight ahead. The
ground under a character is a stair; the weapon in its hands is another;
what it should be looking at is over there. Bending a pose to meet the
world is inverse kinematics, and the engine has none.

## What is there

Read before this was written. Three places a joint can be moved after a
clip, each with another standing towards the simulation:

1. `Bone.Transform` -- an offset on the simulated pose, hashed, read by
   everything the tick reads. The script's way to turn a joint.
2. `SkeletonHost::setJointOverride` -- a joint's place for one tick, after
   the physics step. The ragdoll's.
3. `AnimationSystem::present` -- a copy of the pose, made each frame for
   what the renderer reached, read by the renderer alone (ADR 0194). The
   spring chains'.

No solver, no target, no ground query from the animation's side.

## Decision

**Inverse kinematics is picture.** It is solved each frame, at the rate
frames are drawn, into the presented pose -- the third place above -- and
never into the pose the tick reads.

The reason is the one ADR 0194 gives for a cape, and it is stronger here: a
foot planted on a stair is planted by a ray cast from where the body is
DRAWN this frame, on this machine, at this frame rate. Written into the
simulated pose it would make the world's state a function of one machine's
frame rate; and solved in the tick it would be solved at where the body was
simulated, a frame or two behind where it is drawn, which is a foot that
swims. Nothing gameplay should read a planted foot. What gameplay does need
of a joint it turns itself -- a head whose turn decides what a guard sees --
it sets with `Bone.Transform`, which is simulation and stays the way.

### The instances

Each is a child of the `MeshPart` it bends, as a `SpringBone` is.

- **`IKControl`** -- one limb reaching for one place.
  - `Type`: `TwoBone` (an arm, a leg: the end joint, the one above it and
    the one above that) or `LookAt` (a joint turned to face a point, with
    the joints above it sharing the turn).
  - `EndJoint` -- the hand, the foot, the head.
  - `Target: Instance?` -- a part or an attachment whose DRAWN place is
    reached for or looked at; `TargetOffset: CFrame` from it. With no
    target the control does nothing.
  - `Pole: Instance?` -- where the elbow or the knee points, for a limb; a
    limb with none bends the way its rest pose bends.
  - `AlignRotation: boolean` -- whether the end takes the target's turn as
    well as its place: a hand set on a grip.
  - `ChainLength: number` -- for a look: how many joints above the end
    share it (a head, a neck, a spine), each taking a part.
  - `MaxAngle: number` -- for a look: how far from ahead it may turn.
  - `Weight: number`, `Enabled: boolean`, and `Smoothing: number` -- the
    seconds a change of target is eased over.
- **`FootPlacement`** -- both feet on whatever is under them.
  - `LeftFoot`, `RightFoot`, `Hips` -- joints.
  - `FootHeight` -- from the ankle joint down to the sole.
  - `StepHeight` -- how far up or down from the clip's ground a foot is
    taken; past it the foot is left to the clip.
  - `AlignToSlope: boolean`, `Weight`, `Enabled`.
  - A ray down from above each ankle, as it is animated, finds the ground;
    the foot is put on it and, with `AlignToSlope`, turned to it; and the
    hips come down by what the lower foot needs, so the higher leg bends
    and the lower one does not hang. Rays are cast into the physics world
    as it stood at the end of the tick, and never at the character's own
    parts.

### What is drawn on a bone follows

A sword is a part welded to a hand's `Bone`; the simulation puts it where
the clip puts the hand. If the hand is then moved for the picture, the
sword must be drawn in it:

- **A part held to a bone is drawn where the presented pose has the bone**
  -- a part welded to a `Bone`, and the parts welded to that one, a few
  links deep, are carried by how far the frame moved the joint. The bone's
  own drawn place is carried too, and so whatever is under it.
- **Spring chains are stepped after the limbs are solved**, from the pose
  with the limbs in it: a cape hangs from shoulders an arm's reach moved.
- An effect or a light held to a bone follows the same way.

The sword's simulated place does not move -- a blade that hits is a hitbox,
and that is the tick's.

### Two things the first real character showed

`examples/37-character` holds a staff in two hands, and was the first to
use this on something that moves:

- **`Smoothing` eases in the body's own space.** Eased in the world, a hand
  on something the body carries trailed behind it by the body's speed times
  the smoothing -- a tenth of a metre at a walk, with the default. What keeps
  its place by the body is reached, however fast the body goes.
- **A limb reaches for what the same body holds where the FRAME draws it.**
  The staff is welded to the right hand's bone; the feet lower the body on a
  stair and the staff is drawn down with the hand, and the left hand reached
  for the grip where the tick had left it, a step's height above. A target
  that rides one of the body's own joints is taken as the frame has moved
  that joint, after the looks and the feet.

### Order in a frame

For a body the renderer reached: the pose the tick made; `LookAt` controls,
spine before head; `FootPlacement`, hips first and then each leg;
`TwoBone` controls; spring chains; then what is drawn on bones is carried.
Controls of one kind are solved in the order of their instances.

### Bounded by what is seen

Like secondary motion, and on its budget: nothing is solved for a body the
renderer did not reach the frame before, or further from the camera than
secondary motion is stepped (60 metres; 30 at the lowest quality), and the
lowest quality turns foot placement off and keeps looks and limbs. **A
`Swarm`'s agents have none of it** -- a horde's feet are its clips' -- by
its bodies being left out, not by a count.

**Measured**, on the engine's own build: a character with a look, a limb and
both feet costs 4.2 microseconds a frame, and a hundred of them 0.46
milliseconds -- the solving and the presenting, and beside it two rays into
the physics world a character for its feet. The test that prints these holds
the counts, not the time.

### Which way a body faces

A look needs to know which way "ahead" is, and a file is free to face its
character along any axis. It is read from the rig: across the shoulders (or
the hips) at rest, a quarter turn about up -- by the roles of ADR 0199. A rig
that is not a body by its names is taken to face as the format the engine
reads has it.

### In a match

Both classes travel with their character as authored (protocol 44; ADR 0197
says why a class a character carries has to). What they do is each
machine's own, and a `Target` has to be something the other machines have.

### In the editor

The classes have their icons, and `EndJoint`, the feet and the hips are
picked from the rig's joints as `RootJoint` is. The controls solve while
editing as the chains move while editing (ADR 0194): a target dragged in
the viewport bends the limb that reaches for it.

## What this does not do

- **No full-body solver**: a limb, a look and two feet -- not a body that
  leans to reach. A reach past an arm's length stops at the arm's length.
- **No hands that find their own grips**: a target is something the game
  places, or a part's attachment.
- **No joint limits beyond `MaxAngle`**: a limb bends in the plane its pole
  gives; a knee is not stopped from bending backwards by anything but
  where its pole is.
- **Nothing the tick can read**: a `Bone` on a planted foot is where the
  clip has the foot.

## Proof

Unit tests on the solvers as numbers: a two-bone limb reaches a point in
its reach exactly and stops straight at one past it, bends towards its
pole, and at weight nought is the clip; a look turns its chain by parts
that add to the whole and stops at its limit; feet on a step put the low
foot down, the hips with it and the high knee bent, and a step past
`StepHeight` is left alone. A part welded to a hand's bone is drawn in the
hand that was moved, and is simulated where the clip has it; the pose the
tick reads is untouched; a cape hangs from a shoulder a control moved.
`examples/37-character` walks a stair with its feet on it, holds a staff
in two hands and watches a target; a screenshot test holds the feet on the
stair.
