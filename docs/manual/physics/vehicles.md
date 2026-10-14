# Vehicles

A car, a kart or a truck is a **chassis on wheels**: one unanchored part held
up by a suspension at each corner, pushed along by tires that grip and slip,
driven by an engine through a gearbox. Put a `VehicleController` under the
part and a `VehicleWheel` under the controller for each wheel, and set what
the driver asks each frame.

```luau
local chassis = workspace.Car                -- an unanchored part
local controller = Instance.new("VehicleController")
controller.Parent = chassis

for _, corner in { { -0.9, -1.4 }, { 0.9, -1.4 }, { -0.9, 1.4 }, { 0.9, 1.4 } } do
    local front = corner[2] < 0                 -- forward is the chassis' LookVector, -Z
    local hub = Instance.new("Attachment")
    hub.CFrame = CFrame.new(corner[1], -0.2, corner[2])
    hub.Parent = chassis

    local wheel = Instance.new("VehicleWheel")
    wheel.Attachment = hub
    wheel.MaxSteerAngle = if front then 30 else 0
    wheel.Driven = not front                  -- rear-wheel drive
    wheel.Parent = controller
end

controller.Throttle = 1                       -- -1 to 1
controller.Steer = 0.5                        -- -1 left to 1 right
```

## The wheels are not parts

A wheel is a **cast**: each tick a cylinder of the wheel's `Radius` and `Width`
is cast from its attachment down along the suspension. Where it meets the
ground the suspension pushes the chassis up, and the tire pushes it along and
across by how much it slips. So a wheel rolls over a curb a wheel part would
bounce off, and nothing wobbles at speed.

To see the wheels, give each a `WheelPart`: a part the engine places where the
wheel is, turned by its steering and rolling, after every tick. Make it
`CanCollide = false` -- it is a picture of the wheel, not the wheel.

## The suspension and the tire

| `VehicleWheel` | What it is |
|---|---|
| `SuspensionMinLength`, `SuspensionMaxLength` | the travel, from the attachment to the wheel's center |
| `SuspensionFrequency` | stiffness, as bounces a second: 1 soft, 2 firm, 3 a race car |
| `SuspensionDamping` | how much of a bounce it takes away, 0 to 1 |
| `MaxSteerAngle` | degrees at full `Steer`; 0 does not steer |
| `Driven` | the engine turns it |
| `MaxBrakeTorque`, `MaxHandBrakeTorque` | the brakes; the hand brake holds only the wheels that have one |
| `Grip` | the tire's hold, as a scale of the ground's friction |

`Grounded`, `Compression` (0 out to 1 in) and `Slip` (degrees sideways) are
read back after each tick: dust, skid marks and a squeal are made from them.

Driven wheels are paired in the order they are under the controller, left then
right, each pair sharing the engine through a differential: give a car's
driven wheels left first.

## The engine and the gearbox

| `VehicleController` | What it is |
|---|---|
| `MaxTorque` | newton meters; **0 is enough for the chassis' mass** |
| `MinRPM`, `MaxRPM` | idle and limit |
| `Transmission` | `Automatic` shifts at `ShiftUpRPM` and `ShiftDownRPM`; `Manual` stays in `Gear` |
| `FinalDriveRatio` | higher accelerates harder and tops out lower |
| `MaxLeanAngle` | degrees the chassis may lean before it is held upright; 180 lets it roll over |

`Speed` (meters a second along the chassis' forward, negative backward),
`RPM` and `CurrentGear` are read back after each tick. With the automatic box,
a negative `Throttle` at rest selects reverse.

**A torque or a brake left at 0 is the one for the chassis' mass.** A part
weighs its `Density` times its volume, and a car of default parts weighs a few
kilograms; the defaults keep the proportions of a road car at any mass. Give
numbers of your own when the car is built to a real car's weight.

## Who drives

The controller's inputs are properties a script can set each frame, from a
key, a stick or an AI. **Or a player drives it**: set `Driver` to the player,
and their input actions named by `DriveIntent` (a `Direction2D`: up and down
the throttle, left and right the steering), `BrakeIntent` and
`HandBrakeIntent` write the inputs every tick, with no script.

```luau
controller.Driver = player          -- on the server
controller.DriveIntent = "Drive"    -- the default names
controller.BrakeIntent = "Brake"
```

## In a match

The vehicle runs on the authority, in the simulation, and is saved and
restored with it: a replay drives it the same way, and rollback puts it back.

**The driver's own machine predicts the car**, as it predicts their character:
it answers the wheel in the tick it is turned, at any ping. Two machines
driving one car come apart by a little -- what its wheels last pushed with is
not sent -- so a car is allowed **three centimeters** from where the authority
has it and is pulled back gently at every answer; further than that, it is put
back where the authority had it, its engine, gear and wheels too, driven again
by what the player did, and **drawn gliding there over about a quarter of a
second** rather than jumping. Measured at 50, 150 and 300 ms of round trip, a
straight, a corner and a stop are not corrected, the car stays within about a
centimeter of the authority's, and it is drawn within a centimeter of a
straight line. Two cars pushing each other are corrected a few times, by three
or four centimeters, and glide. `NetworkService:GetStats().PredictedDrift`
says how far the car was at the last answer. The driver needs a
`CharacterBody` character for this.

Everybody else sees the car between snapshots, as any moving part. The inputs
and what the controller and its wheels read back travel with it, so a
replica's speedometer and engine sound are the authority's.

## What it does not do

- Motorcycles and tracked vehicles, and anti-roll bars.
- A tire's grip does not depend on the surface under it.

## Where to look next

- [Movers and powered joints](manual:physics/movers)
- [Rigid bodies](manual:physics/bodies)
- [`VehicleController`](api:VehicleController) · [`VehicleWheel`](api:VehicleWheel)
