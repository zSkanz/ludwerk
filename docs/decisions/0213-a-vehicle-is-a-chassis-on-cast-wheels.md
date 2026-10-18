# 0213 — A vehicle is a chassis on cast wheels

- Status: accepted; built (the physics seam, the instances, and the driven car predicted in a match)
- Date: 2026-10-14
- Builds on: ADR 0007 (Jolt), ADR 0127 (movers and constraints), ADR 0101
  (rollback), ADR 0157 (intents written by code), ADR 0208 (a local player's
  character is predicted)

## The question

A car is built today out of a chassis, four wheel parts, four hinges with
motors and four springs, and a script that sets their speeds. It drives, and
it is not a car: a wheel that is a body bounces off a curb a real one rolls
over, four joints held at sixty steps a second wobble at speed, nothing grips
less as it slides, and there is no engine, no gearbox and no brake but a
motor told to stop. Every engine with vehicles builds them the other way, and
the physics library under this one already has that way in it.

## Decision

### A chassis and its wheels, on Jolt's vehicle constraint

The chassis is a body; a **wheel is not**. Each step every wheel is a cylinder
cast from the chassis down along its suspension: where it meets the ground,
the suspension -- a spring with a travel, a frequency and a damping -- pushes
the chassis up, and the tire pushes it along and across by how much it slips,
on a curve of grip. An engine with a torque and a range of turns drives the
wheels named as driven through a gearbox and differentials; brakes and a hand
brake hold them. This is Jolt's `VehicleConstraint` with its wheeled
controller, a step listener in the same solve as everything else: no new
dependency, the same determinism, and saved and restored with the world.

The seam (`IPhysics3D`) gains `createVehicle`, `destroyVehicle`,
`setVehicleInput` and `vehicleState`, with the defaults a backend without
vehicles answers. A chassis that is replaced (resized) is driven again by the
same vehicle; a chassis destroyed takes its vehicle with it.

**The engine's axes**: up is +Y and forward is -Z, as a part's `LookVector`.
Driven wheels are paired, left then right, in the order they are given, each
pair a differential with an equal share of the engine.

### The instances

- **`VehicleController`**, under the chassis part: the engine (`MaxTorque`,
  `MinRPM`, `MaxRPM`), the gearbox (`Transmission`: `Automatic` or `Manual`;
  `GearRatios`, `ReverseRatio`, `ShiftUpRPM`, `ShiftDownRPM`), and what the
  driver asks: `Throttle` (-1 to 1), `Steer` (-1 to 1), `Brake` and
  `HandBrake` (0 to 1), `Gear` for a manual box. It reads back `Speed`,
  `RPM` and `CurrentGear`.
- **`VehicleWheel`**, one a wheel, under the controller, at an `Attachment`
  of the chassis: `Radius`, `Width`, `SuspensionMinLength`,
  `SuspensionMaxLength`, `SuspensionFrequency`, `SuspensionDamping`,
  `MaxSteerAngle`, `Driven`, `MaxBrakeTorque`, `MaxHandBrakeTorque`,
  `Grip`; and `WheelPart`, a part the engine places where the wheel is, turned
  and rolling, each step. It reads back `Grounded`, `Compression`, `Slip`.
- **Who drives**: `Driver`, a player, set by the authority. While a player
  drives, their intents named by `DriveIntent` (a direction: up and down the
  throttle, left and right the steering), `BrakeIntent` and
  `HandBrakeIntent` write the inputs every tick, natively -- on the authority
  with what they sent, on their machine with what they did, and again in a
  replay with what they did then. With no driver a script writes the inputs.
  A seat that puts a character at the wheel is a convenience for later, not
  the vehicle.

### Simulation, rollback and the wire

On the authority, in the fixed step, deterministic on one build (R10), and in
the world's saved state, so rollback and replays hold.

**A vehicle its player drives is predicted on that player's machine, as their
character is** (ADRs 0133, 0208; amended 2026-10-14). A car drawn from
snapshots answers its wheel a round trip late -- nine ticks at 150 ms -- and is
not drivable. So:

- the driven chassis is the middle of the driver's predicted island, and the
  nearest thing in it, beside their character; what it touches is predicted
  with it, as round a character;
- its inputs are the driver's intents, read by the engine in the step itself,
  so a correction steps it again with exactly what the driver did;
- **its own state is the island's, and the authority's over it**: a vehicle
  is more than its chassis -- its gearbox's timers, what its wheels last pushed
  with, its suspension -- and the island a replica remembers each tick holds
  all of it with its bodies. A replay puts that back as this machine had it at
  the answered tick, then sets over it what the authority sends of the
  vehicle at that tick: the engine's turns, the gear and the clutch, and each
  wheel's spin, angle and steering (fields the wire carries, in the world's
  hash). What a wheel touched is worked out again by the next step's casts;
- a `Player` is no instance on the wire, so `Driver` travels as the driver's
  user id, as a part's network owner does;
- the wheels' parts of the driven car are placed by the predicted vehicle,
  and neither written from a snapshot nor drawn from the past.

Only for a driver whose character is a `CharacterBody`: a correction steps the
island by that character's commands (ADR 0133's amendment).

Everybody else's car is drawn between snapshots, as every remote body is.

**A vehicle is corrected at three centimetres, and pulled back softly under
them** (amended 2026-10-14). A crate is all its state, and two machines step
it to the bit from the same (ADR 0133); a car is its chassis and what its
wheels last pushed with, which is not sent. Stepped apart by the two machines,
a car drifts about a centimetre over a third of a second of driving -- every
answer at 150 ms found it there. Corrected at the crate's hundredth of a
millimetre, a driven car was stepped again at every answer, from the
authority's chassis on this machine's wheels -- a car neither machine had --
and drawn two to seven centimetres back and forth. So, for a chassis:

- off by less than **3 cm** (`scene::VehicleCorrectionMetres`), it is not
  corrected or counted: it is stepped again pulled **a fifth of the way**
  (`PhysicsSync::VehiclePull`) to the authority's place and velocity, so a
  drift that does not grow is gone in about a second and two machines never
  settle a few centimetres apart about where a car is, which decides what it
  hits. **A fifth an answer, not a fifth a tick**: the pull happens where an
  answer is taken, so it closes half a drift in three answers whatever the
  snapshot rate. At the default, an answer a tick, that is 50 ms. A game
  that sends a snapshot every third tick closes it in 150 ms, and the drift
  between answers grows for three ticks rather than one. The three
  centimetres bound it either way: past them the car is corrected. Measured
  at an answer a tick; a lower rate was not measured. `NetworkService:GetStats().PredictedDrift` says how far it was at the
  last answer. Three, not five: measured, the drift's mean is one to three
  millimetres and its largest thirteen, and three centimetres is where a
  shove between two cars started to be told apart from it;
- off by more, it is the authority's, counted as a correction, and **drawn
  gliding there over about a quarter of a second**
  (`replication::VehicleVisualDecayPerTick`, 0.8 of what is left a tick) --
  slower than a character's tenth, because a car moves its own length in a
  few ticks and a character's glide showed on it as a jolt. The model the
  chassis is in glides with it, so a cabin welded on and the wheels' parts go
  where it goes. **A constant, not a property**: a game that asks for another
  gets one.

Two defects were found by measuring this, each held by a test that fails
without its fix (`world_host_tests.cpp`, "a replay of a car setting off is
the live drive again"): a replay put the vehicle's engine and wheels back from
its components, which hold what the newest step left -- a car still at the
answered tick was stepped again with its wheels turning, and crept forward on
no throttle; and the island held its bodies and not its vehicles, so the
gearbox's timers and its wheels' impulses came out of the newest step.

Measured (`netcode_vehicle`, a real server and its clients over the real
transport, 2% loss and 20 ms of jitter): a straight, a corner and a stop on
the brake; a run into a parked car; a negative control, in which the
authority moves the car half a metre aside once in the straight; and two
clients driving at each other, each with a wheel that wanders from a seed of
its own.

| link | corrections | largest | drawn jump | simulated jump | the authority's own | drift, mean / max | ticks to answer: car, character |
|---|---|---|---|---|---|---|---|
| none | 0 | -- | 5.0 mm | 1.8 mm | 1.8 mm | 0 / 0 mm | 1, 2 |
| 50 ms | 0 | -- | 4.0 mm | 1.8 mm | 1.8 mm | 0 / 0 mm | 1, 2 |
| 150 ms | 0 | -- | 11.1 mm | 19.9 mm | 1.7 mm | 1.3 / 6.4 mm | 1, 2 |
| 300 ms | 0 | -- | 6.5 mm | 50.3 mm | 1.8 mm | 3.1 / 12.4 mm | 1, 2 |
| into a parked car, 50 ms | 0 | -- | 55.7 mm | 55.7 mm | 55.7 mm | 0.5 / 2.5 mm | 1, 2 |
| moved aside by the authority, 50 ms | 1 | 0.500 m | 100 mm | 500 mm | 500 mm | -- | 1, 2 |
| two drivers, 150 ms, the first | 1 | 31 mm | 58.7 mm | 58.6 mm | 67.3 mm | 8.4 / 31.5 mm | 1, 2 |
| two drivers, 150 ms, the second | 4 | 36 mm | 61.2 mm | 61.7 mm | 66.8 mm | 7.3 / 36.2 mm | 1, 2 |

The **jumps** are the most a car's place moved off the line of its last two
frames: drawn (`GetRenderCFrame`), simulated, and the authority's own car over
the same stretch. A crash stops a car in a tick on every machine -- the parked
car and the two drivers show it on the authority too; what a replica draws
beyond that is the link's. The half metre moved aside is counted, at its size,
and drawn gliding: a fifth of it in the first frame. At 300 ms the simulated
car is stepped again by up to five centimetres and drawn within seven
millimetres of a straight line. The car answers its stick -- its engine's turns
change, or it moves -- in the tick the character does, or the one before.

The gate holds: a straight and a corner to no correction (one for a change
begun in a lost tick, two for each long frame); the stop to a centimetre; the
car's answer to the character's tick or the one after; the drawn car to two
centimetres beyond the authority's own jump in every run but the negative
control; the drift, at its largest over a drive, under the correction; the
negative control to a correction of between 0.3 and 0.8 m -- a gate that saw
none there would be blind; and the two drivers to meeting.

## What this does not do

- Motorcycles and tracked vehicles: Jolt has both controllers; they follow the
  wheeled one if a game asks.
- Anti-roll bars: Jolt has them; not named yet.
- A wheel's tire does not deform, and its grip does not depend on the surface
  it is on.

## Proof

`engine/physics/tests/vehicle_tests.cpp`: a car rests on its springs, goes
forward on the throttle, stops on the brake and turns with the wheel; the same
inputs give the same drive bit for bit, and a restored world drives on as it
would have; a vehicle needs a moving chassis and a wheel, and goes with its
chassis. Then the instances: a conformance spec, an example and a drawn gate.

## Amendment, 2026-10-14: what the gate holds, counted in ticks

- **Every measure is in ticks, not frames.** The simulated jump and the drift
  are read at the end of every tick, as the authority reads its own car; the
  drawn jump, which can only be read in a frame, is divided by the ticks the
  frame covered. A machine under load passed and failed the same drive as
  frames came long or short; with every core busy it now measures what it
  measures alone.
- **A duel's drawn car is held against the larger of the two crashes**, the
  authority's and this machine's own: each driver's machine crashes its car
  into the other where it sees it, a ping in the past, so its crash is not the
  authority's and neither bounds the other (49 to 111 mm the authority's over
  the runs). What is held is that the picture adds no snap on top of the
  physics, and that a duel's correction stays under 10 cm (31 to 46 mm
  measured).
- **The glide is held by the nudge**, the one row where it is all there is to
  see: half a metre corrected is drawn as at most a quarter of it, and a
  centimetre, in its first tick (about a tenth of a metre measured).
- **What a re-simulation moves is drawn gliding too, and held as such**: an
  answer steps the car again from the authority's state, and on a link whose
  intents reach the authority late -- a machine with every core busy -- that
  lands it some centimetres from where it was predicted without being a
  correction (81 mm at 300 ms, measured). Every row may draw beyond the
  authority's own car a quarter of the simulated jump and a centimetre, never
  less than two centimetres: what a fifth-a-tick glide shows of it.

## Amendment, 2026-10-17: what a replica puts back, and when a press arrives

- **A car a machine begins to predict starts from the authority's car
  whole.** What an answer put back was the engine's turns, the gear, the
  clutch, the gearbox's clocks and each wheel's spin, angle and steering. The
  wheels' suspension and the impulses their tires and suspension ended each
  step with -- where the solver starts the next step -- were the replica's
  own, made when the car was born there, and the vehicle gate's logs showed
  one born while settling with a wheel resting where the authority's was in
  the air. They travel with the car now (`SuspensionLength` and the four
  lambdas of `VehicleWheel`, `PreviousDeltaTime` of `VehicleController`,
  protocol 56), written through the vehicle's own saved form field by field
  -- what is portable between two machines, never the body a wheel touches or
  where -- and a vehicle a machine begins to predict is stepped again from the
  authority's state at the first answer, agreeing or not.
- **A brake now and then landed twelve ticks late** on the authority, and the
  driver's car was corrected three times by about 19 cm, a few drives in a
  hundred on a link losing 2 % of its packets. The cause was not the car: an
  action was named to the authority the first time it was pressed, the name
  went reliably, and when its packet was lost the press waited for the name.
  Every action is now named the first tick a script writes it, held or not
  (`session_tests`). Measured with the vehicle gate's stall row, eight at a
  time: 3 drives of 208 corrected before, none of 104 after.
