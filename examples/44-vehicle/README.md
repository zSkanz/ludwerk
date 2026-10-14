# 44-vehicle

A car of parts on four wheels (ADR 0213), on a ground with a road, a ramp to
jump and cones to knock over.

```
run.bat
```

- **W** and **S**, or the stick, drive forward and back; **A** and **D** steer.
  An automatic gearbox shifts by itself, and holding **S** at rest reverses.
- **Space** or **B** pulls the hand brake: the rear wheels lock and the car
  slides round them.
- **R** or **Y** puts it back on its wheels where it is.

The speed and the gear at the bottom are read back from the controller, which
is what a speedometer and an engine's sound would read.

## What is where

| | |
|---|---|
| `src/client/init.luau` | All of it: the ground, the car -- an unanchored chassis with a `VehicleController`, a cabin welded on, four `VehicleWheel`s at attachments, each with a `WheelPart` drawn where it is -- the two input actions read into `Throttle`, `Steer` and `HandBrake`, a camera that follows, and the dashboard. |
| `i18n/` | The words on the screen, in English and Brazilian Portuguese. |

## For a picture

`--scene-data` drives it with no hand on the keys:

```
run.bat --headless --frames=180 --exit --screenshot=out.png "--scene-data={\"throttle\":1,\"steer\":0.3}"
```

`throttle` and `steer` are what the keys would give, from -1 to 1.
