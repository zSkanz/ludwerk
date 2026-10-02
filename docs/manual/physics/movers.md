# Movers and powered joints

A fan that turns, a lift that goes up, a pet that hovers at a shoulder, a door
that closes by itself: none of them needs a script running every tick. Each is
an instance with properties, and the simulation does the rest.

There are two families.

- **A powered joint** is a constraint that does work: a hinge with a motor, a
  rail with a servo, a ball socket that holds a pose. The solver holds the
  joint and drives it in the same step.
- **A mover** acts on one part, the one its `Attachment0` is on: hold this
  velocity, go to that place, push this hard. The four that *hold* something --
  a place, a facing, a speed, a spin -- are motors the simulation solves
  together with the joints the part is in, so a limb pulled to a pose and the
  joints that hold the limb agree in the same tick. The two that only *push*,
  `VectorForce` and `Torque`, are forces.

Both act on **unanchored** parts -- an anchored part is not moved by physics --
and both are placed with [attachments](manual:physics/joints), as every
constraint is.

## Powered joints

`HingeConstraint` and `PrismaticConstraint` have an `ActuatorType`:

| `ActuatorType` | What it does | Set |
|---|---|---|
| `None` | holds, and that is all | -- |
| `Motor` | moves at a speed | `AngularVelocity` and `MotorMaxTorque` on a hinge; `Velocity` and `MotorMaxForce` on a rail |
| `Servo` | goes to a target and holds it | `TargetAngle`, `AngularSpeed`, `ServoMaxTorque` on a hinge; `TargetPosition`, `Speed`, `ServoMaxForce` on a rail |

A motor too weak for its load does not reach its speed, and a servo too weak
does not arrive: the caps are the most the joint may use, and what is heavier
than that wins. `MotorMaxAcceleration` makes a motor wind up instead of
starting at full speed.

Angles are in degrees and are measured as the hinge's limits are; speeds are in
radians a second about the attachment's X, and metres a second along it.

`GetMotorTorque()` and `GetMotorForce()` say what the joint's own motor used
over the last tick, in newton-metres and newtons: a servo holding a weight
level reads the weight times its arm, and one reading its cap is doing all it
can. They are apart from `GetForce()` and `GetTorque()` on purpose -- those
are what the joint *bore*, which is what breaks it.

### A spinner

A fan, a wheel, a carousel: a hinge to an anchored base, with a motor.

```luau
--!strict
local base = Instance.new("Part")
base.Anchored = true
base.Position = vector.create(0, 5, 0)
base.Parent = workspace

local blade = Instance.new("Part")
blade.Size = vector.create(8, 0.4, 1)
blade.Position = vector.create(0, 6.5, 0)
blade.Parent = workspace

-- The hinge turns about its attachments' X: pointed up, the blade spins flat.
local up = CFrame.fromEuler(0, 0, math.pi / 2)
local onBase = Instance.new("Attachment")
onBase.CFrame = CFrame.new(0, 1.5, 0) * up
onBase.Parent = base
local onBlade = Instance.new("Attachment")
onBlade.CFrame = up
onBlade.Parent = blade

local hinge = Instance.new("HingeConstraint")
hinge.Attachment0 = onBase
hinge.Attachment1 = onBlade
hinge.CollideConnected = false
hinge.ActuatorType = Enum.ActuatorType.Motor
hinge.AngularVelocity = 6 -- about a turn a second
hinge.MotorMaxTorque = 500
hinge.Parent = workspace
```

### A lift

A `PrismaticConstraint` is a rail: the part slides along the attachments' X and
does nothing else. A servo on it is a lift.

```luau
--!strict
local function lift(base: BasePart, platform: BasePart): PrismaticConstraint
    local up = CFrame.fromEuler(0, 0, math.pi / 2)
    local onBase = Instance.new("Attachment")
    onBase.CFrame = up
    onBase.Parent = base
    local onPlatform = Instance.new("Attachment")
    onPlatform.CFrame = platform.CFrame:ToObjectSpace(base.CFrame) * up
    onPlatform.Parent = platform

    local rail = Instance.new("PrismaticConstraint")
    rail.Attachment0 = onBase
    rail.Attachment1 = onPlatform
    rail.CollideConnected = false
    rail.ActuatorType = Enum.ActuatorType.Servo
    rail.Speed = 3
    rail.ServoMaxForce = 50000 -- what it can carry
    rail.Parent = workspace
    return rail
end

-- Call it to a floor:
-- rail.TargetPosition = 12
```

`LimitsEnabled` with `LowerLimit` and `UpperLimit` are the rail's ends.

### A joint that holds a pose

A `BallSocketConstraint` with `ActuatorType = Servo` pulls `Attachment1` to an
orientation in `Attachment0`'s frame -- `TargetOrientation`, the identity being
the pose the two were joined in -- with no more than `ServoMaxTorque`. That is
a shoulder that holds an arm out and gives when the arm is pulled harder: what
a character that stands by its own joints is made of.

Which way a turn swings a limb, with the joint's X along the limb: about X it
twists; about Y by a positive angle its far end swings towards the frame's -Z;
about Z, towards its +Y.

### A servo is a spring

A servo is a **critically damped spring to its target**: it arrives without
crossing and without ringing. `AngularResponsiveness` and
`LinearResponsiveness` are that spring's natural frequency, in radians a
second -- higher arrives sooner -- and it is sized for the two parts the joint
holds, about the joint itself: a ball a metre out on an arm counts as a metre
out. So the same number settles a finger and a crane the same way.

`AngularSpeed` and `Speed` cap how fast it travels: far from its target a
servo moves at that speed, and near it is the spring.

**What hangs further down a chain is not counted.** A shoulder knows the upper
arm, not the forearm and the hand beyond the elbow. A joint that carries more
than its own part takes the spring's two numbers directly -- `Stiffness` and
`Damping` -- and so does one that should be soft, or bouncy:

| | About an axis | Along a line |
|---|---|---|
| `Stiffness` | newton-metres a radian | newtons a metre |
| `Damping` | newton-metre-seconds a radian | newton-seconds a metre |
| what is moved | its inertia `I`, kg m^2 about the joint | its mass `m`, kg |
| settles at `w` rad/s | `Stiffness = w * w * I` | `Stiffness = w * w * m` |
| critical damping | `Damping = 2 * math.sqrt(Stiffness * I)` | `Damping = 2 * math.sqrt(Stiffness * m)` |

Less than critical overshoots and rings; more arrives late and stays. A limp
arm, a firm spine and a punch differ in both.

**A servo at its cap is a constant torque.** One too weak for its load sags to
where the load's pull is the cap -- half the torque a level arm asks, and it
hangs at thirty degrees -- and, having nothing left to brake with, swings
about there until something else takes its speed: the part's `LinearDamping`,
friction, a second joint.

`BasePart.Mass` says what a part weighs the moment it is in the world, and
again the moment its size or density changes -- it does not wait for the
simulation to step -- so a spring can be sized in the line after the part is
made.

## Ropes, rods and springs

| Class | What it holds | Set |
|---|---|---|
| `RopeConstraint` | the two ends no further apart than `Length`; slack, it does nothing | `Length`, and the winch |
| `RodConstraint` | the two ends exactly `Length` apart | `Length` |
| `SpringConstraint` | pulls and pushes towards `FreeLength` | `FreeLength`, `Stiffness`, `Damping`; `LimitsEnabled` with `MinLength` and `MaxLength` for hard stops |

`Visible = true` draws any of the three in the game, as a thin line of its
`Color` and `Thickness`, lit like the parts it joins.

A rope's **winch** works `Length` itself towards `WinchTarget` at `WinchSpeed`,
and stalls when the rope's tension is more than `WinchForce`: a crane taking in
cable.

A rope comes taut within one tick's travel: the tick it runs out in, it is
longer than `Length` by what its end moved in that tick, and from the next it
is its length.

### A rope bridge

Planks joined end to end by short ropes, hung from two anchored posts.

```luau
--!strict
local function rope(a: BasePart, aAt: vector, b: BasePart, bAt: vector)
    local first = Instance.new("Attachment")
    first.CFrame = CFrame.new(aAt)
    first.Parent = a
    local second = Instance.new("Attachment")
    second.CFrame = CFrame.new(bAt)
    second.Parent = b
    local link = Instance.new("RopeConstraint")
    link.Attachment0 = first
    link.Attachment1 = second
    link.Length = 0.4
    link.Visible = true
    link.Thickness = 0.08
    link.Parent = workspace
end

local function post(x: number): Part
    local created = Instance.new("Part")
    created.Anchored = true
    created.Size = vector.create(1, 4, 4)
    created.Position = vector.create(x, 10, 0)
    created.Parent = workspace
    return created
end

local previous: BasePart = post(-10)
local previousEdge = 0.5
for index = 1, 8 do
    local plank = Instance.new("Part")
    plank.Size = vector.create(2, 0.3, 4)
    plank.Position = vector.create(-10 + index * 2.2, 11.5, 0)
    plank.Parent = workspace
    -- Two ropes a joint, one at each side, so a plank does not spin.
    for _, side in { -1.8, 1.8 } do
        rope(previous, vector.create(previousEdge, 1.5, side), plank, vector.create(-1, 0, side))
    end
    previous, previousEdge = plank, 1
end
local far = post(-10 + 9 * 2.2)
for _, side in { -1.8, 1.8 } do
    rope(previous, vector.create(1, 0, side), far, vector.create(-0.5, 1.5, side))
end
```

## Movers

| Class | What it does | Main properties |
|---|---|---|
| `LinearVelocity` | holds a velocity | `VectorVelocity`, `MaxForce`, `RelativeTo`; `VelocityConstraintMode` for a line or a plane only |
| `AngularVelocity` | holds a spin | `AngularVelocity`, `MaxTorque`, `RelativeTo` |
| `AlignPosition` | pulls to a place and holds | `Mode`, `Position`, `MaxForce`, `MaxVelocity`, `Responsiveness`, `RigidityEnabled` |
| `AlignOrientation` | turns to an orientation and holds | `Mode`, `CFrame`, `MaxTorque`, `MaxAngularVelocity`, `Responsiveness`, `RigidityEnabled` |
| `VectorForce` | a constant force | `Force`, `RelativeTo`, `ApplyAtCenterOfMass` |
| `Torque` | a constant torque | `Torque`, `RelativeTo` |

- **A cap is a cap.** An `AlignPosition` holds a weight its `MaxForce` is
  enough for and sags under one it is not; a `LinearVelocity` with half the
  force a part weighs lets it fall at half of gravity. A cap is along each
  axis the mover acts on, and `GetMotorForce()` and `GetMotorTorque()` say how
  much of it was used.
- **`Responsiveness`** is the natural frequency, in radians a second, of the
  critically damped spring an align is; `MaxVelocity` and `MaxAngularVelocity`
  are the speed it travels at while it is far. It is sized for the **one part**
  the mover is on.
- **`RelativeTo`** says whose axes a vector is written in. `World` is the
  world's; `Attachment0` follows the part, so a thruster pushes where the part
  points.
- **`AlignPosition` and `AlignOrientation` do not spin or carry a part at a
  speed.** They pull it towards a target and hold it there. Something that
  keeps turning is an `AngularVelocity`, or a hinge with a motor.
- **`Mode = TwoAttachment`** makes the target `Attachment1` instead of the
  mover's own `Position` or `CFrame`. With `ReactionForceEnabled` or
  `ReactionTorqueEnabled` the part that attachment is on is pulled back by as
  much: a hand pulling a crate is pulled by the crate, and an arm aligned to a
  chest turns the chest.
- **`Stiffness` and `Damping`**, above zero, make an align a spring in place
  of its `Responsiveness`, in the units of the table above: soft and damped, or
  stiff and bouncy. **A part with others jointed to it takes these**, sized for
  all that has to move -- the hips of a body are turned by a spring sized for
  the body. Any stiffness is safe: a spring a hundred times stiffer than a
  tick can show holds still and does not explode.
- **A long row of rigid joints lags.** A mover's pull travels a joint a round
  of the solver. A body and its limbs -- two or three joints from the part
  that is held -- follow the spring they were given; a rigid chain of five
  turned from its middle swings at its ends, and eleven is out of reach. Hold
  a long chain by more than one of its pieces.

### A hovering pet

An `AlignPosition` to a place beside its owner and an `AlignOrientation` to
face the way the owner faces. The pet is a real part: it bumps into walls on
the way, and something heavy enough pushes it aside.

```luau
--!strict
local function follow(pet: BasePart, owner: BasePart)
    local onPet = Instance.new("Attachment")
    onPet.Parent = pet
    -- Over the right shoulder.
    local seat = Instance.new("Attachment")
    seat.CFrame = CFrame.new(2, 3, 1)
    seat.Parent = owner

    local place = Instance.new("AlignPosition")
    place.Mode = Enum.PositionAlignmentMode.TwoAttachment
    place.Attachment0 = onPet
    place.Attachment1 = seat
    place.MaxForce = 2000
    place.MaxVelocity = 30
    place.Responsiveness = 8
    place.Parent = pet

    local facing = Instance.new("AlignOrientation")
    facing.Mode = Enum.OrientationAlignmentMode.TwoAttachment
    facing.Attachment0 = onPet
    facing.Attachment1 = seat
    facing.MaxTorque = 2000
    facing.Parent = pet
end
```

## Breaking

Every constraint has `BreakForce` and `BreakTorque`. When the joint carries
more than either, it gives: it is disabled -- not destroyed -- and `Broken`
fires. `GetForce()` and `GetTorque()` say what it carried over the last tick,
in newtons and newton-metres, which is how a grip knows it is about to slip.

```luau
--!strict
local function grip(hand: Attachment, thing: Attachment): BallSocketConstraint
    local hold = Instance.new("BallSocketConstraint")
    hold.Attachment0 = hand
    hold.Attachment1 = thing
    hold.BreakForce = 400
    hold.Parent = workspace
    hold.Broken:Connect(function()
        hold:Destroy()
    end)
    return hold
end
```

## Two parts that pass through each other

A `NoCollisionConstraint` takes `Part0` and `Part1` and stops that one pair
colliding; both still collide with everything else. It is what a collision
group cannot say -- a group is every part in it against every part in another
-- and what carrying something needs: the thing carried against the one
carrying it.

## Seeing them

`Visible = true` on any constraint draws it in the editor and under the debug
overlay: the line between its ends, a hinge's or a rail's axis, where an
`AlignPosition` pulls to, which way a force or a velocity points. A selected
one is drawn whether or not it is `Visible`.

## In a match

The authority simulates, and movers and joints are part of what it simulates:
every screen is sent where the parts ended up. They are not sent themselves, so
a part a player's own machine predicts is not pushed by a mover there until the
authority's answer arrives.

## Where to look next

- [Welds and constraints](manual:physics/joints)
- [Rigid bodies](manual:physics/bodies)
- [A ragdoll](manual:physics/ragdoll)
