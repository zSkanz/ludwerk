# Graphics and display settings

How good the picture is, how the window sits on the display and how fast frames
are made are **one set of settings** (ADR 0147). A project gives them defaults,
a player chooses, and a script -- an options menu -- reads and writes them
through `GraphicsService`.

## Whose word a value is

A setting's value is the first of these that says it:

| | Who | How |
|---|---|---|
| 1 | Whoever started the run | A flag: `--quality=low`, `--no-vsync` |
| 2 | A script, this session | `GraphicsService.Bloom = false` |
| 3 | The player, saved | `settings.json` in their folder |
| 4 | The game | `[graphics]` and `[display]` in `project.toml` |
| 5 | The level | `Low`, `Medium`, `High`, `Ultra` |
| 6 | The engine | Its own default |

A script's write is the newest word, so it is over what the player saved before;
`SaveAsync` makes it the player's. `GetSource` says which of the six a value is.

## By script

```luau
--!strict
local Graphics = game:GetService("GraphicsService")

Graphics.QualityLevel = Enum.GraphicsQuality.Medium
Graphics.VSync = false
Graphics.MaxFrameRate = 144
Graphics.WindowMode = Enum.WindowMode.Borderless

Graphics:SaveAsync() -- remember it for this player
```

A write takes effect at the next frame, and ten writes in one frame are applied
once. Numbers are clamped into their range; a value of the wrong kind raises.

**A level.** `QualityLevel` is `Low`, `Medium`, `High` or `Ultra`. Writing one --
or `ApplyPreset(level)` -- makes every quality setting that level's, whatever a
script, the player or the project said before; the display's settings stay. It
reads `Custom` as soon as any quality setting is not the level's, and `Custom`
cannot be written. `Auto` is the level this machine starts at.

**A group.** A player's menu usually shows a handful of sliders rather than
thirty settings. `SetGroupLevel(group, level)` sets every setting of one group
at once, and `GetGroupLevel(group)` is the level it is at -- nil when its
settings are no one level's:

| `Enum.GraphicsGroup` | Sets |
|---|---|
| `ViewDistance` | `ViewDistance`, `TerrainDetail`, `LODBias`, `MaximumLODLevel` |
| `AntiAliasing` | `AntiAliasing` |
| `PostProcessing` | `Bloom`, `DepthOfField`, `SunRays`, `AutoExposure`, `MotionBlur` |
| `Shadows` | `ShadowQuality`, `ShadowResolution`, `ShadowCascades`, `ShadowDistance`, `ContactShadows` |
| `GlobalIllumination` | `AmbientOcclusion`, `GlobalIllumination` |
| `Reflections` | `Reflections` |
| `Textures` | `TextureQuality`, `AnisotropicFiltering`, `TextureStreamingBudget` |
| `Effects` | `ParticleBudget`, `SoftParticles` |
| `Foliage` | `FoliageDensity` |
| `Shading` | `LightBudget`, `SkinWeights`, `FogQuality` |

`Enum.GraphicsLevel` is `Low`, `Medium`, `High`, `Ultra` and `Cinematic`, which
is `Ultra` until something is finer.

**The display.** `WindowMode` (`Windowed`, `Borderless`, `Fullscreen`),
`Resolution`, `Monitor`, `VSync`, `MaxFrameRate` and `BackgroundFrameRate`.
`GetMonitors()` lists the displays as `{ Name, Size, RefreshRate }`,
`GetSupportedResolutions()` the fullscreen sizes of the one `Monitor` names,
and `GetRefreshRate()` its rate. A handheld's window is its display and its
sync is always on, whatever these say.

**What is drawn by yet.** A few settings are kept, saved and reported ahead of
the renderer that reads them, so a menu written today does not change shape:
`ViewDistance`, `TextureQuality`, `AnisotropicFiltering`, `LODBias`,
`MaximumLODLevel`, `ParticleBudget`, `SoftParticles`, `SkinWeights`,
`TextureStreamingBudget`, `AsyncUploadBudget`, `MotionBlur`, `FogQuality`,
`GlobalIllumination`, `Reflections` and `Brightness`. `IsApplied(name)` says
which, so a menu can leave them out:

```luau
if Graphics:IsApplied("MotionBlur") then
	addToggle("Motion blur", "MotionBlur")
end
```

**Signals.** Every property has its changed signal; `QualityChanged` fires once
a frame when the level or any quality setting changed, whoever changed it.

**Keep them out of the world.** A setting describes this machine. A script that
makes the game play differently by one has made two players at two settings
play different games; see [why](manual:why/graphics-settings).

## For a player: saved, and loaded first

`SaveAsync()` writes the player's choices to `settings.json` in their own
folder, beside the game's saves -- only the ones they changed, so a default a
later version improves reaches everybody who never touched it. It yields until
the file is written and returns whether it was.

The file is read **before the first frame**: a game starts in the player's
window mode and level, not in the project's for a second. A saved setting this
build does not know, or a display that is no longer plugged in, is left at the
project's default and named in the log.

`LoadAsync()` reads the file again and forgets what a script wrote since: a
menu's Revert. `ResetToDefaults()` forgets both, so the project's defaults
stand -- and stays unsaved until `SaveAsync`.

A game that manages its own settings turns the file off:

```toml
[display]
remember_player_settings = false
```

## In a project

`[graphics]` is the quality settings and `[display]` the display's. Any setting
is its name in snake case; a setting that is one of a set takes the item's name
in lower case.

```toml
[graphics]
quality = "high"
shadow_quality = "medium"
terrain_detail = 1.5
foliage_density = 0.75

[display]
window_mode = "borderless"   # windowed | borderless | fullscreen
resolution = [1920, 1080]
vsync = true
max_frame_rate = 0
background_frame_rate = 10
```

A platform's own table -- `[graphics.android]` -- is read over `[graphics]` on
that platform. The older keys, their ranges and what each level sets are in
[Graphics quality settings](manual:rendering/quality).

## In the editor

A game's options menu tried in Play changes the Viewport's picture, and not the
editor's own window, sync or frame rate: the display's settings are the game's
window's. Stop puts back what Play changed.

Project Settings writes `project.toml`. The quality matrix, the Viewport's own
level and the editor's frame rate as preferences are not built yet.

## Where to look next

- [Graphics quality settings](manual:rendering/quality) — the keys, the ranges, the levels
- [Graphics settings belong to the player](manual:why/graphics-settings)
- [`GraphicsService`](api:GraphicsService)
