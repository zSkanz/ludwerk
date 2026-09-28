# 0127 — Movers and constraints move a part without code

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F2)
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

## Not decided here

- The 2D equivalents beyond the existing 2D joints.
- Legacy "body movers" of the other platform; the constraint-based ones above
  replace them there too.
