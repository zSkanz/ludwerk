# Vibration

`HapticService` is what a player feels in their hands: a gamepad's motors, and
a phone's own vibration.

```luau
--!strict
local HapticService = game:GetService("HapticService")
local Gamepad = Enum.UserInputType.Gamepad

-- An engine that rumbles while it runs: one call when it starts...
HapticService:SetMotor(Gamepad, Enum.VibrationMotor.Large, 0.4)
-- ...and one when it stops.
HapticService:SetMotor(Gamepad, Enum.VibrationMotor.Large, 0)

-- A hit, on a phone: a tenth of a second at full strength.
HapticService:Vibrate(1, 0.1)
```

## Two shapes

**A gamepad's motor is a level.** `SetMotor(inputType, motor, value)` leaves it
running at `value` -- 0 still, 1 full -- until it is set again. `Large` is the
heavy, slow motor (a rumble) and `Small` the light, fast one (a buzz); some
controllers have `LeftTrigger` and `RightTrigger` too. A motor that is not
there is ignored, so a game need not ask first.

**A phone shakes for a moment.** `Vibrate(strength, seconds)` shakes the device
itself and stops by itself, after at most five seconds.

## What is there

| Call | Answers |
|---|---|
| `IsVibrationSupported(Enum.UserInputType.Gamepad)` | whether a connected gamepad has motors |
| `IsVibrationSupported(Enum.UserInputType.Touch)` | whether the device itself can vibrate -- a phone |
| `IsMotorSupported(inputType, motor)` | whether that one motor is there |

Ask them to decide what to show in a settings screen, not before every call.

## When it stops

Every motor is still while the game's window is not the one in front -- nobody
is holding the pad of a game they are not playing -- and picks up at the level
it had when the window is in front again. Closing the game stops everything.

A dedicated server and a headless run have no hands: they answer false to
every question and take every call quietly.

## Where to look next

- [Actions, bindings and contexts](manual:input/actions)
- [`HapticService`](api:HapticService)
