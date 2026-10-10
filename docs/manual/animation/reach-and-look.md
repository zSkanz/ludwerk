# Reaching, looking and feet

A clip is made on flat ground, for one weapon, looking straight ahead. The
ground under the character is a stair, the thing in its hands is another, and
what it should be watching is over there. An `IKControl` bends a limb to reach
a place or turns a head to look at one; a `FootPlacement` puts two feet on
whatever is under them. Both work on top of whatever clip is playing.

`examples/37-character` has all three: a walker with its feet on a stair, a
staff held in two hands, and heads that follow the player.

## A hand on a grip

```luau
--!strict
local hero = workspace.Hero.Body :: MeshPart
local staff = workspace.Hero.Staff :: Part

local grip = Instance.new("Attachment")
grip.CFrame = CFrame.new(0, 0.4, 0)     -- a hand's width up the staff
grip.Parent = staff

local hold = Instance.new("IKControl")
hold.EndJoint = "LeftHand"               -- the joint at the END of the limb
hold.Target = grip
hold.AlignRotation = true                -- the palm takes the grip's turn too
hold.Parent = hero                       -- the MeshPart whose skeleton it bends
```

`Type` is `TwoBone` unless it says otherwise: the end joint, the one above it
and the one above that are a limb -- a hand, an elbow and a shoulder; a foot, a
knee and a hip -- and the end is put on the target. The limb reaches as far as
it is long and stops there, straight.

- `Target` is a part or an attachment; `TargetOffset` is where from it the
  place is, in the target's own space. With no target the control does
  nothing.
- `Pole` is what the elbow or the knee points at. With none, the limb bends
  the way the clip already has it bent.
- `Weight` is how much of it is applied, 0 to 1. Tween it to pick a thing up
  and let it go.
- `Smoothing` is the seconds a change is eased over -- a target that jumps, a
  control switched on -- so a hand moves to a new place instead of snapping
  to it. 0 follows at once. It is eased where the BODY is: a thing that keeps
  its place by the body -- a weapon it carries -- is not trailed behind,
  however fast the body goes.

A limb may reach for something **the same body holds**: a staff welded to a
bone on the right hand, and the left hand on a grip along it. The grip is
taken where the frame draws it -- with the hand that holds the staff, after
the feet have lowered the body on a stair -- so both hands stay on.

## A head that looks

```luau
--!strict
local watch = Instance.new("IKControl")
watch.Type = Enum.IKControlType.LookAt
watch.EndJoint = "Head"
watch.ChainLength = 2          -- the head, and two joints above it share the turn
watch.MaxAngle = 70
watch.Target = workspace.Lantern
watch.Parent = hero
```

The end joint is turned to face the target, and `ChainLength` joints above it
-- a neck, a spine -- each take an equal part of the turn, so a body leans
into a look instead of twisting its neck. Past `MaxAngle` from straight ahead
the look stops at the limit.

**Which way is ahead is read from the rig**, not assumed: across the shoulders
and a quarter turn about up. A model may face along any axis in its file.

## Feet on the ground

```luau
--!strict
local feet = Instance.new("FootPlacement")
feet.LeftFoot = "LeftFoot"     -- the ankles, as the file names them
feet.RightFoot = "RightFoot"
feet.Hips = "Hips"
feet.FootHeight = 0.1          -- from the ankle joint down to the sole
feet.StepHeight = 0.4          -- how far up or down a foot is taken
feet.Parent = hero
```

Each frame a ray down from each foot finds the ground. The foot is put on it,
**keeping however high the clip had lifted it** -- a foot in the air mid-stride
stays in the air, that much above its own ground -- and the hips come down by
what the lower foot needs, so the higher knee bends and the lower leg does not
hang. With `AlignToSlope` a planted foot is turned to lie on its ground.

Ground further than `StepHeight` from the ground the clips were made on -- a
ledge, a drop -- is not stepped onto: that foot is left to the clip. The rays
do not find the character's own parts.

## It is picture, not simulation

All of this is solved on each machine, each frame, into the pose the frame is
DRAWN with -- and never into the pose the tick reads. A foot is planted by a
ray from where the body is drawn this frame, at this machine's frame rate; put
into the simulation it would make the world's state depend on one machine's
frame rate.

So:

- **A `Bone` on a hand a control moved still says where the clip has the
  hand.** Nothing in a game's rules should read a reached-for place.
- What a script turns for the rules to see -- a head whose turn decides what a
  guard notices -- it turns with `Bone.Transform`, which is simulation.
- **What is drawn on a bone follows.** A sword welded to a hand's `Bone` is
  drawn in the hand the frame moved, with whatever is welded to the sword; a
  light or an effect under the bone follows too; and a cape hangs from
  shoulders an arm's reach moved
  ([Capes, tails and hair](manual:animation/secondary-motion)). The sword's
  simulated place does not move: a blade that hits is a hitbox, and that is
  the tick's.
- A server with no window does none of it, and a replay is not changed by it.

In a frame the order is: the pose the tick made; looks; feet; limbs; spring
chains; then what is held to bones is carried.

## In a match

A control and a foot placement travel with their character, as authored --
what is bent, what is reached for, how much -- and the bending is each
machine's own. A `Target` has to be something the other machines have too: a
part the server made, not one only a client script made.

## What it costs, and what bounds it

It is done for bodies the camera reached the frame before, as far from the
camera as secondary motion is stepped (`GraphicsService`'s setting: 60 metres
by default, 30 at the lowest quality). **At the lowest quality feet are not
placed at all**; looks and limbs still are. **A `Swarm`'s agents have none of
it**: a horde's feet are its clips'.

Measured on the engine's own build: a character with a look, a limb and both
feet costs about five microseconds a frame, and a hundred of them half a
millisecond -- plus two rays into the physics world a character for its feet.

## What is not here

- **No full-body solver**: a limb, a look and two feet, not a body that leans
  to reach.
- **No hands that find their own grips**: a target is a thing the game places.
- **No joint limits beyond `MaxAngle`**: a knee bends in the plane its pole
  gives it, and nothing but where the pole is stops it bending backwards.
- **Nothing the tick can read.**

## Where to look next

- [Animation graphs](manual:animation/graphs) -- what makes the pose these bend
- [Capes, tails and hair](manual:animation/secondary-motion) -- the other half of the drawn pose
- [`IKControl`](api:IKControl) · [`FootPlacement`](api:FootPlacement) · [`Bone`](api:Bone)
