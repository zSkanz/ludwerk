# 0131 — Sound effects, vibration, and preloading

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F6 and F8)
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
