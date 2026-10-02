# 0131 — Sound effects, vibration, and preloading

- Status: accepted and built (sections 1 and 2 on 2026-10-02, F6; section 3 before them, F8)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving sound effects, vibration and
  asset preloading from a survey of public documentation (R7).
- Builds on: [0009](0009-miniaudio-module-is-the-seam.md) (miniaudio is the
  seam), [0125](0125-a-scene-is-prepared-in-the-background-and-activated-when-the-game-says.md)
  (a scene's assets warmed in the background; its "not decided here" named
  preloading).

## Decision

### 1. Sound effects

- Instances parented to a `Sound` or an `AudioGroup`, applied in `Priority`
  order: `ReverbSoundEffect` (room size, damping, wet/dry, width),
  `EchoSoundEffect` (delay, feedback, wet/dry), `EqualizerSoundEffect` (low,
  mid, high gain and the mid band), `LowPassSoundEffect` and
  `HighPassSoundEffect` (cutoff, resonance), `DistortionSoundEffect` (level),
  `CompressorSoundEffect` (threshold, ratio, attack, release, makeup gain),
  `ChorusSoundEffect` (rate, depth, mix), `PitchShiftSoundEffect` (octave).
  Each has `Enabled`.
- Built on miniaudio's node graph: its delay, filter and shelf nodes, and the
  reverb node from its `extras/nodes`, already vendored — **no new dependency**.
  Distortion, compressor, chorus and pitch shift are the engine's own small
  nodes.
- Effects run on the audio thread; changing a property is a message to it, never
  a lock.

### 2. Vibration

- `HapticService`: `IsVibrationSupported(inputType)`,
  `IsMotorSupported(inputType, motor)`, `SetMotor(inputType, motor, value)`
  (`Enum.VibrationMotor`: `Large`, `Small`, `LeftTrigger`, `RightTrigger`,
  `LeftHand`, `RightHand`), and `Vibrate(strength, seconds)` for a phone.
- Through SDL3's gamepad rumble and haptic API, and the device's vibrator on
  Android.
- A game's vibration stops when its window loses focus or the game closes.

### 3. Preloading

- `ContentProvider:PreloadAsync(items, callback?)`: `items` are content paths or
  instances (every asset an instance references, recursively); yields until they
  are loaded and uploaded; `callback(item, status)` per item.
  `ContentProvider.RequestQueueSize`.
- Uses the warming path of ADR 0125 and its budget.

## Consequences

- A cave that echoes, a radio that crackles, a controller that rumbles, and a
  first frame without assets popping in.

## As built, 2026-10-02

- **The effects are the engine's own processors in the engine's own mixer**,
  not nodes of miniaudio's graph. That graph has been compiled out since the
  mixer was written (`MA_NO_NODE_GRAPH`): voices are mixed by hand on a
  timeline the simulation owns, so an effect is a function over a block of
  that mixer's samples. The filters are the textbook second-order ones
  miniaudio's are; the reverb is the one miniaudio vendors (`verblib`), used
  without the graph; the echo, distortion, compressor, chorus and pitch shift
  are the engine's. No new dependency, as decided.
- **Parameters travel with the voices**, under the lock the voices are
  published under and the audio thread only ever tries -- the mixer's own
  rule, which this did not need to change. Nothing allocates on the audio
  thread: an effect's state is made on the frame and handed to it.
- **An effect on a `Sound` stops with the sound; one on an `AudioGroup` runs
  every block**, voices or none, so a room and an echo ring on. That is the
  rule the manual gives for where to put one.
- **`Priority` is a number, lower first**, and equals apply in the order they
  were made.
- **The equalizer's middle band is two frequencies**, `MidLow` and `MidHigh`:
  below is the low band, above the high one.
- **Vibration is levels, re-sent each frame for half a second**: a game that
  stops pumping -- closed, crashed, frozen -- stops shaking by itself. Still
  while the window is not in front; the levels return with it.
- **Not on hardware yet (D476)**: the vendored SDL is built with its
  joystick, haptic and HID support off, as it has been since M1 -- which also
  means no gamepad has ever worked. Turning them on is a build decision of
  its own (three platforms, one of which only CI builds) and is the next
  push's, by itself.
