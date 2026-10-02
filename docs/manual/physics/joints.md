# Welds and constraints

Two kinds of joint, and the difference is who moves the second part.

- **A weld drives it.** `Weld` and `WeldConstraint` put one part where another
  is, every tick, and the solver is never asked. A sword stays on a hand
  whatever else is happening.
- **A constraint asks the solver.** `HingeConstraint`, `BallSocketConstraint`
  and `FixedConstraint` hand both bodies to the simulation and let it work out
  where they end up: a door swings under its own weight, a ragdoll falls.

Use a weld to attach, a constraint to articulate. The welds first.

| Class | The offset is |
|---|---|
| `Weld` | **Authored** — you say where the two parts sit relative to each other. |
| `WeldConstraint` | **Captured** — it reads the relationship off the world when it activates. |

Both extend `Instance` rather than `PVInstance`: a joint is not a thing in
space. Neither needs to be parented to either part.

## A transform weld, not a solver constraint

> `Weld.Part1` stops being independently simulated and is driven from
> `Weld.Part0` every tick.

That is what this joint is. The solver is not involved, which is why a
`MeshPart` can be welded to a `CharacterBody` — a character controller is not a
body the solver could constrain anyway.

The consequences follow directly:

- **While the weld is active, gravity does not reach `Part1`**, and writing its
  `CFrame` is overwritten at the next tick.
- **`Part0` may be anything** — simulated, anchored, or a character. Whatever
  moves it, `Part1` goes with it.
- **A welded part is kinematic, not static.** It still collides and still pushes
  what it runs into.
- **Welding two dynamic parts so the solver treats them as one rigid assembly is
  a different feature, and it is not this one.**

## The offsets, in one equation

```text
Part0.CFrame * C0  ==  Part1.CFrame * C1
```

`Weld.C0` is the attachment on `Part0`; `Weld.C1` is the attachment on `Part1`.
That equation is the one sentence that says where both offsets go.

```luau
--!strict
local banner = Instance.new("Part")
banner.Size = vector.create(0.4, 1.6, 0.4)
banner.Parent = workspace

local weld = Instance.new("Weld")
weld.Part0 = character          -- a CharacterBody is a legal anchor
weld.Part1 = banner
weld.C0 = CFrame.new(0, 4.2, 0) -- 4.2 metres above the anchor
weld.Parent = workspace
```

## Releasing

```luau
weld.Enabled = false
```

`Weld.Enabled` hands `Part1` back to the simulation **where it stands, with no
velocity**. It is released rather than thrown.

## WeldConstraint: capture instead of author

`WeldConstraint` records where the two parts already are and holds that. Use it
when the parts are already in the right place, which is most of the time — and
use `Weld` when you are *specifying* a relationship rather than freezing one.

Getting that backwards is worth avoiding in both directions: authoring an offset
by hand for two parts already in position is arithmetic nobody should have to
do, and capturing one when you meant to specify it is a joint that silently
depends on where things happened to be.

```luau
--!strict
local joint = Instance.new("WeldConstraint")
joint.Part0 = wall
joint.Part1 = painting     -- captured where it currently hangs
joint.Parent = workspace
```

`WeldConstraint.Enabled` going from `false` to `true` **captures the relative
transform afresh**. That is how a part is re-welded somewhere else: move it,
then enable.

`WeldConstraint.Active` is read-only and reports whether it is currently
holding — enabled, with both parts set, and both in the world. The distinction
matters because a joint with one part missing is not an error: it is half-built,
which is what every script that assigns the two properties on separate lines
briefly produces.

## What is refused

A part welded to itself, and a weld whose two parts are already joined by
another weld — directly or through a chain. Welds form a graph and a cycle has
no resolution order, so the write that would create one is refused at the write,
where the caller can be told which write it was.

## Constraints: the solver's joints

A constraint joins two **`Attachment`s**, not two parts. An attachment is a
named place on a part -- its `CFrame` is relative to the part it is parented
to -- so where a door hinges is a place on the door and a place on the frame,
both of which you can see and move.

```luau
--!strict
local function attach(part: BasePart, world: CFrame): Attachment
    local attachment = Instance.new("Attachment")
    attachment.CFrame = part.CFrame:ToObjectSpace(world)
    attachment.Parent = part
    return attachment
end

-- A shin hanging from a thigh by a knee that bends one way.
local knee = CFrame.new(0, 4.8, 0)
local hinge = Instance.new("HingeConstraint")
hinge.Attachment0 = attach(thigh, knee)
hinge.Attachment1 = attach(shin, knee)
hinge.LimitsEnabled = true
hinge.LowerAngle = -135
hinge.UpperAngle = 5
hinge.CollideConnected = false
hinge.Parent = workspace
```

| Class | Freedom | Limits |
|---|---|---|
| `HingeConstraint` | turns about one axis: a door, a lid, an elbow | `LowerAngle` to `UpperAngle`, in degrees |
| `BallSocketConstraint` | turns every way about one point: a shoulder, a hip, a rope's end | a swing cone of `UpperAngle` and a twist of `TwistLimit` either way |
| `FixedConstraint` | none: two bodies the solver treats as one | -- |

Four things about the joint's frame that every one of them shares:

- **The axis is the attachment's own X.** A hinge turns about its attachment's
  X axis, and a ball socket's cone is measured from it. Pointing the
  attachment points the joint.
- **The two frames are the same place when the constraint is built.** That is
  what "this is where they are attached" means: put both attachments at the
  joint, as the example does, and the parts are held as they stand.
- **Limits count by the right hand about that X**, the second part
  (`Attachment1`'s) against the first: with the thumb along X, the fingers
  curl towards positive. The knee above lets the shin's foot swing 135 degrees
  one way and 5 the other.
- **A limit is not exceeded and pulled back; it is not exceeded.** A limited
  ball socket is a different solver joint from a free one, not a free one with
  corrections on top.

`Enabled = false` leaves the joint in the world holding nothing, which is how a
grip lets go without being destroyed. `CollideConnected = false` stops the two
parts colliding with each other -- an upper arm and a lower arm overlap at the
elbow by construction, and left colliding they shove each other apart every
step.

A constraint can be made and destroyed while the game runs, and one end may be
on an anchored part. It cannot reach a `CharacterBody`: that is swept rather
than solved, so there is no body for a joint to hold -- weld to it instead.

## What is not here

No motor and no spring yet: a joint holds and limits, and does no work of its
own. No `SliderConstraint`, no rope, no `Motor6D`. Movers and powered joints
are ADR 0127's, and not built.

## Where to look next

- [Rigid bodies](manual:physics/bodies)
- [Ragdolls](manual:physics/ragdoll)
- [`Weld`](api:Weld) · [`WeldConstraint`](api:WeldConstraint)
- [`HingeConstraint`](api:HingeConstraint) · [`BallSocketConstraint`](api:BallSocketConstraint) ·
  [`FixedConstraint`](api:FixedConstraint) · [`Attachment`](api:Attachment)
