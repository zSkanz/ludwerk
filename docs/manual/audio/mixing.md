# Groups, mixing and the listener

## The gain chain

Every voice's volume is a product, and there are exactly four terms:

```text
Sound.Volume  ×  Sound.Group.Volume  ×  AudioService.MasterVolume  ×  distance
```

The last term is 1 for a 2D sound. The mix is soft-clipped at the end rather
than allowed to wrap.

## Groups

An `AudioGroup` is a bus. Point a `Sound` at one and its volume is multiplied by
the group's:

```luau
--!strict
local AudioService = game:GetService("AudioService")

local music = Instance.new("AudioGroup")
music.Name = "Music"
music.Volume = 0.6
music.Parent = AudioService

local track = Instance.new("Sound")
track.Content = "asset://audio/theme.ogg"
track.Looped = true
track.Group = music
track.Parent = AudioService   -- not a BasePart, so 2D
track:Play()
```

That is what makes "turn the music down" one property rather than a loop over
every music track.

`AudioGroup.Volume` is a **multiplier, not decibels**: 0 is silent, 1 is
unchanged, and above 1 is louder and will clip if the material was already near
full scale.

**Nesting does nothing.** A group parented to a group is just an instance
parented to an instance; only a `Sound`'s own `Group` is read.

It is called `AudioGroup` rather than `SoundGroup` because the service is
`AudioService`, and one prefix across a family beats two.

## Master volume

`AudioService.MasterVolume` is multiplied into every sound, after its own volume
and its group's. It is the one a settings screen writes.

## The listener is the camera

> The listener is `Workspace.CurrentCamera`, and it is not settable.

A game with two ideas about where the player is hearing from is a game with a
bug, and the camera is already the one thing that answers that question. So
whatever moves the camera moves the ear.

Two consequences:

- **Only the camera's position is used.** Its orientation is ignored, because
  there is no panning to orient — see [Sounds](manual:audio/sounds).
- **With `CurrentCamera` set to `nil`, the ear sits at the world origin.**

## Fire and forget

```luau
--!strict
local AudioService = game:GetService("AudioService")

local click = AudioService:PlayLocal("asset://audio/click.ogg")
click.Volume = 0.8
```

`AudioService.PlayLocal` creates a 2D `Sound` parented to the service and plays
it, for the case where naming an instance is all ceremony. It takes only the
content, which is why it hands the `Sound` back — that handle is how you set the
volume.

**It does not clean up after itself in this release.** The `Sound` stays a child
of the service once it has ended, so a caller firing one per frame accumulates
them. Where that matters, keep the handle and destroy it:

```luau
local shot = AudioService:PlayLocal("asset://audio/shot.ogg")
shot.Ended:Connect(function()
    shot:Destroy()
end)
```

## The overlay numbers

```luau
--!strict
local DebugService = game:GetService("DebugService")

print(DebugService:GetStat("AudioVoices"))        -- how many are mixing
print(DebugService:GetStat("AudioUnderruns"))     -- the device wanting samples it did not get
print(DebugService:GetStat("AudioClipsMissing"))  -- how many are the placeholder tone
```

A headless run opens no audio device at all and reports zero, which is the
truthful answer rather than a missing one.

## Effects

Nine instances change a sound on its way to the speakers. Parent one to a
`Sound` and that sound goes through it; parent one to an `AudioGroup` and
everything in the group does, mixed together first.

```luau
--!strict
-- Everything in the cave group echoes off its walls.
local cave = Instance.new("AudioGroup")
cave.Parent = game:GetService("AudioService")
local room = Instance.new("ReverbSoundEffect")
room.RoomSize = 0.85
room.WetLevel = 0.4
room.Parent = cave

-- A voice on a radio: no lows, no highs, a little broken up.
local function radio(voice: Sound)
    local band = Instance.new("EqualizerSoundEffect")
    band.LowGain = -30
    band.HighGain = -30
    band.Parent = voice
    local crackle = Instance.new("DistortionSoundEffect")
    crackle.Level = 0.3
    crackle.Priority = 1 -- after the equalizer
    crackle.Parent = voice
end
```

| Class | What it does | Its numbers |
|---|---|---|
| `ReverbSoundEffect` | a room | `RoomSize`, `Damping`, `WetLevel`, `DryLevel`, `Width` |
| `EchoSoundEffect` | the sound again, later | `Delay` (seconds), `Feedback`, `WetLevel`, `DryLevel` |
| `EqualizerSoundEffect` | three tone controls | `LowGain`, `MidGain`, `HighGain` (decibels), `MidLow`, `MidHigh` (hertz) |
| `LowPassSoundEffect` | takes the highs away | `Cutoff` (hertz), `Resonance` |
| `HighPassSoundEffect` | takes the lows away | `Cutoff` (hertz), `Resonance` |
| `DistortionSoundEffect` | breaks it up | `Level` |
| `CompressorSoundEffect` | turns the loud parts down | `Threshold` (decibels), `Ratio`, `Attack`, `Release` (seconds), `MakeupGain` |
| `ChorusSoundEffect` | one voice as several | `Rate` (hertz), `Depth`, `Mix` |
| `PitchShiftSoundEffect` | higher or lower, the same length | `Octave` (0.5 to 2) |

- **`Enabled`** switches one off without removing it, and **`Priority`** orders
  the effects under one parent: lower first. A tone squared off and then
  filtered is not the tone filtered and then squared off.
- **An effect on a `Sound` stops with the sound; one on an `AudioGroup` rings
  on.** A reverb or an echo belongs on a group, where its tail outlives the
  sound that fed it.
- **A number outside its range is refused**, with the property's page saying
  what the range is.
- **Nothing a script can read changes.** A sound through a pitch shift is as
  long as it was and ends when it did: what the speakers do is downstream of
  the simulation.

## Where to look next

- [Sounds](manual:audio/sounds)
- [`AudioService`](api:AudioService) · [`AudioGroup`](api:AudioGroup)
