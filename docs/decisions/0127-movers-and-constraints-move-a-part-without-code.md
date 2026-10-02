# 0127 — Movers and constraints move a part without code

- Status: accepted (built, 2026-10-02, with the amendments at the end; see
  `docs/briefs/toolkit-kickoff.md`, F2)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, asking for ways to make a part turn or
  move *"seja por física ... sem a necessidade de código"*, naming
  `AlignOrientation` and `AlignPosition`, and approving the movers and
  constraints a survey of public documentation listed (R7).
- Builds on: [0007](0007-jolt-3d-physics.md) (Jolt), [0066](0066-the-physics-seam-learns-two-static-shapes.md)
  (the physics seam), [0101](0101-rollback-saves-restores-and-steps-the-simulation.md)
  (rollback), [0118](0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md)
  (impulses at a point).

## Context

The engine has `Weld`, `WeldConstraint`, `FixedConstraint`,
`BallSocketConstraint` and a `HingeConstraint` with angle limits only — no
motor, no servo — and no mover at all. A fan, a wheel, a lift, a hovering pet or
a door that closes by itself is a script today. Jolt, underneath, has motors and
servos on hinges and sliders, distance constraints with springs, and applies
forces and torques per step.

`AlignPosition` and `AlignOrientation` do not spin a part: they pull it towards a
target and hold it there. A part that keeps turning is an `AngularVelocity`, or a
hinge with a motor.

## Decision

All of these act on **unanchored** parts, through `Attachment`s, as the
existing constraints do; an anchored part is not moved by physics.

### 1. Movers

| Class | What it does | Main properties |
|---|---|---|
| `LinearVelocity` | holds a velocity | `VelocityConstraintMode` (`Vector`, `Line`, `Plane`), `VectorVelocity`, `MaxForce`, `RelativeTo` |
| `AngularVelocity` | holds a spin | `AngularVelocity`, `MaxTorque`, `RelativeTo` |
| `AlignPosition` | pulls to a position and holds | `Mode` (`OneAttachment` to `Position`, `TwoAttachment` to another attachment), `MaxForce`, `MaxVelocity`, `Responsiveness`, `RigidityEnabled`, `ApplyAtCenterOfMass` |
| `AlignOrientation` | turns to an orientation and holds | `Mode`, `CFrame`, `MaxTorque`, `MaxAngularVelocity`, `Responsiveness`, `RigidityEnabled` |
| `VectorForce` | a constant force | `Force`, `RelativeTo`, `ApplyAtCenterOfMass` |
| `Torque` | a constant torque | `Torque`, `RelativeTo` |

Each is evaluated inside the physics step, before the solver, as forces and
torques on the body (at a point where it says so), capped by its maxima.

### 2. Constraints

- **`HingeConstraint` gains an actuator**: `ActuatorType` (`None`, `Motor`,
  `Servo`); for `Motor`: `AngularVelocity`, `MotorMaxTorque`,
  `MotorMaxAcceleration`; for `Servo`: `TargetAngle`, `AngularSpeed`,
  `ServoMaxTorque`, `AngularResponsiveness`. Jolt's hinge motor settings.
- **`PrismaticConstraint`** — a rail along an axis, with limits and the same
  actuator on a line (`Velocity`, `TargetPosition`, `Speed`, `MotorMaxForce`,
  `ServoMaxForce`). Jolt's slider.
- **`RopeConstraint`** — at most `Length` apart, with `Restitution`; optional
  winch (`WinchEnabled`, `WinchTarget`, `WinchSpeed`, `WinchForce`).
- **`RodConstraint`** — exactly `Length` apart.
- **`SpringConstraint`** (3D) — `FreeLength`, `Stiffness`, `Damping`,
  `LimitsEnabled`, `MinLength`, `MaxLength`.
- **`NoCollisionConstraint`** — `Part0` and `Part1` do not collide with each
  other.
- Rope, rod and spring are Jolt distance constraints with their limits and
  spring settings.

### 3. The rules they share

- **Deterministic**: they run in the physics step, their state (a winch's
  length, a servo's error) is in the trace and in the rollback snapshot, and
  their evaluation order is the instances' stable order.
- **In a match**: they replicate as instances; the machine that simulates the
  body (the authority, or its network owner) applies them.
- **Seen**: `Visible` draws each one (arrows, rails, springs, ropes) in the
  editor and under F3; ropes and rods are also drawable in a game
  (`Visible = true` at run time), as thin lines with `Color` and `Thickness`.
- The manual gains recipes: a spinner (a hinge to an anchored base with a
  motor), a hovering pet (`AlignPosition` + `AlignOrientation` to the player), a
  lift (a prismatic servo), a rope bridge.

## Consequences

- A fan, a wheel, a lift, a crane, a door that closes, a pet that follows —
  made of instances, set in Properties, without a script.

## Amendments, 2026-10-02

From a physics brawler made on the packaged engine -- jointed fighters that
stand, punch, grab, lift and throw -- whose verdict was that the solver was not
the limit: *what is missing is a way for a joint to do work.* Decided by
ludwerk-08 for the owner; built with the rest.

- **A1 -- a ball socket holds a pose.** `BallSocketConstraint.ActuatorType`
  (`None`, `Servo`), `TargetOrientation` (the rotation of `Attachment1` in
  `Attachment0`'s frame), `ServoMaxTorque`, and the spring terms of A3. The
  solver's swing-twist motor. Section 2 gave a motor only to the hinge, and
  shoulders, hips, a spine and a neck are ball sockets.
- **A2 -- what a mover does to one body it can do, reversed, to the other.**
  `AlignPosition.ReactionForceEnabled`, `AlignOrientation.ReactionTorqueEnabled`
  and `AngularVelocity.ReactionTorqueEnabled`, false by default, for a mover
  with a second attachment. Without it an arm aligned to a chest turns in the
  air and does nothing to the chest.
- **A3 -- a spring's two terms beside the responsiveness.** `Stiffness` and
  `Damping` on `AlignPosition`, `AlignOrientation` and every servo: above zero
  they replace `Responsiveness`, and a limp arm, a firm spine and a punch differ
  in both. Stiffness and damping rather than frequency and ratio, because
  `SpringConstraint` already speaks them.
- **N1 -- a part says what it weighs.** `BasePart.Mass` and `AssemblyMass`,
  read-only. The reason there was no `Mass` -- it must not contradict
  `Density` -- does not apply to one that cannot be written.
- **N2 -- a contact says what it was.** `BasePart.Collided(other, position,
  normal, speed)` for a part whose `ContactDetails` is on.
- **N3 -- a joint gives.** `Constraint.BreakForce`, `BreakTorque`, `Broken`,
  and `GetForce()` and `GetTorque()`. Methods and not properties: what a joint
  carried is new every tick, and a property would put it in every world's hash.
- **N4** -- `BasePart.LinearDamping` and `AngularDamping`.
- **N5 -- a velocity can be written.** `BasePart.LinearVelocity` and
  `AngularVelocity` were read-only "because an assignment is an impulse with
  the mass divided out"; it is also what serving a ball is, and every game that
  needed it divided the mass out by hand.
- **N6** -- `Workspace:GetBodiesInSphere`.

What building it settled:

- **Movers are impulses before the step, not solver constraints.** Each is a
  function of the bodies as the step finds them and of its own properties, with
  nothing kept between ticks -- so there is no mover state to put in a
  snapshot, and section 3's "state in the trace" is true by there being none. A
  pull at a point uses the mass that point seems to have, not the body's: a
  push at the end of a plank also turns it. A mover that HOLDS a velocity or a
  place answers gravity in the tick it acts; a spring is left to find its own
  balance against the weight.
- **A servo is a speed towards its target** -- no faster than its own, falling
  to nothing as it arrives -- held by the solver's velocity motor under the
  servo's cap; with a stiffness, the solver's position motor as a spring. Its
  error is measured each tick, not remembered.
- **A winch writes `Length`.** How much rope is out is the rope's length, a
  property like any other: hashed, saved, readable. There is no hidden
  "current length".
- **`RopeConstraint.Restitution` is not built**: the solver's distance limit
  has none, and a property nothing reads is worse than one that is absent.
- **A rope comes taut within one tick's travel.** A limit is noticed once it is
  crossed.
- **Defaults that work when switched on**: the caps (`MaxForce`, `MaxTorque`,
  the motors' and servos') are 10 000, not zero, so setting `ActuatorType` and
  a speed moves something.
- **In a match the authority simulates them and they are not on the wire**, as
  the joints before them: a replica is sent where the parts ended up. Section
  3's "the machine that simulates the body applies them" is therefore true of
  the authority only; on a body a replica predicts or owns, a mover is not
  applied until `Attachment` and these classes travel. That is an open box in
  the stage's ledger, with the network's.
- **Seen**: `Visible` draws every one as lines in the editor and under the
  debug overlay; a rope, a rod and a spring are also a thin lit cylinder in the
  game.

## Amendments, 2026-10-02 (second): what the brawler found in them

Section 1 said every mover is "impulses worked out before each step". That was
built, and it is wrong for a body anything else holds. Decided by ludwerk-08
for the owner (D470, D471), with what building it found:

- **A mover that holds something is a motor in the solver.** `AlignPosition`,
  `AlignOrientation`, `LinearVelocity` and `AngularVelocity` are a six-axis
  joint with every axis free and a motor on the ones they hold, against the
  world or -- with `Reaction*Enabled` -- between the two bodies. `VectorForce`,
  `Torque` and a spring without stops stay forces. Still nothing remembered
  between ticks: what is asked of the solver is a function of the world as the
  step finds it.
- **A servo is a critically damped spring to its target**, its
  `Responsiveness` the spring's natural frequency in radians a second. Far
  from the target it travels at its speed (`AngularSpeed`, `Speed`,
  `MaxVelocity`, `MaxAngularVelocity`); nearer than two speeds over its
  frequency -- from where such a spring arrives without crossing -- it is the
  spring.
- **A hinge's and a ball socket's motor is the engine's own constraint**
  beside the solver's joint: one row that is the turn with the pivot already
  held. The solver's own joint motors are rows about each body's centre of
  mass, and an iterative solver does not settle them against the pivot's rows
  when a body swings from a pivot away from where it balances.
- **Units, everywhere**: `Stiffness` in newton-metres a radian or newtons a
  metre, `Damping` in newton-metre-seconds a radian or newton-seconds a metre,
  angles in degrees, speeds in radians a second. Critical damping is
  `2 * sqrt(Stiffness * inertia)`.
- **The frequency form is sized for what the joint or the mover is on** -- the
  two parts of a joint about the joint, the one part of a mover -- and not for
  what hangs beyond. A loaded joint and the hips of a body take `Stiffness`.
- **A cap is along each axis**, not a magnitude: the solver's motors are rows.
- **`GetMotorForce()` and `GetMotorTorque()`** say what a motor used, apart
  from `GetForce()` and `GetTorque()`: those are what a joint bore, and
  `BreakForce` is a threshold on what it bore.
- **`BasePart.Mass` answers from the part's properties** (D472), at once.
- **What an iterative solver cannot do stays undone**: a torque travels a
  joint a round, so a rigid chain turned from its middle lags at its ends --
  three pieces follow their spring, five want four times a tick's rounds, and
  eleven is out of reach. The solver's rounds are not raised for it: every
  island would pay. A `Workspace` setting for them is a box in the ledger.

## Not decided here

- The 2D equivalents beyond the existing 2D joints.
- Legacy "body movers" of the other platform; the constraint-based ones above
  replace them there too.
