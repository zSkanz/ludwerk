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

## An options screen in one call

A game that wants the usual screen and not the work of writing it asks for the
engine's:

```luau
local settings = require("@engine/settings")

optionsButton.Activated:Connect(function()
    local screen = settings.open()
    screen.Closed:Connect(function()
        -- back to the pause menu
    end)
end)
```

It has a Graphics page and a Display page, a row for each setting **this build
draws by** -- one `IsApplied` says nothing reads yet has no row -- and four
buttons. Nothing of it is private: every row reads and writes `GraphicsService`,
and every word comes from `LocalizationService:Translate`, so a game that writes
its own menu has the same two services to write it with.

- **A quality row writes at once**, so the player sees the picture change
  behind the menu. **The three that move the window** -- its mode, its
  resolution and its monitor -- **are held until Apply**: a window that changed
  mode at every step through a list would be a window nobody could read the
  list on.
- **Apply** writes what is held and keeps everything (`SaveAsync`). **Revert**
  goes back to what was last kept (`LoadAsync`), and so does leaving without
  applying. **Defaults** is `ResetToDefaults`, kept only by Apply.
- The quality row steps Automatic, Low, Medium, High, Ultra. It reads Custom
  when a setting was changed by itself, and a step from Custom goes to
  Automatic.
- A mouse, a finger, a gamepad and the arrow keys all drive it: every control
  is a button, the [selection](manual:ui/selection) walks them, and a row the
  selection reaches is scrolled into sight. If something was selected when it
  opened it takes the selection, and gives it back when it closes.
- On a handheld the Display page is the frame rate limit alone: there is no
  window to arrange.

`settings.open` takes a table, every field optional:

| Field | What it is |
|---|---|
| `Theme` | `Backdrop`, `BackdropTransparency`, `Panel`, `Raised`, `Accent`, `Text`, `Dim` (colours), `Font` (an `asset://` typeface) and `CornerRadius` |
| `Hide` | Settings to leave out, by their `GraphicsService` name: `{ "MotionBlur" }` |
| `Title` | The heading, already in the player's language |
| `DisplayOrder` | The screen's place among the game's own; 100 by default |

What it returns:

| Member | What it does |
|---|---|
| `Gui` | The `ScreenGui`, yours to hide or to move in the draw order |
| `IsOpen`, `IsDirty` | Whether it is open, and whether something waits for Apply |
| `Closed` | A signal, fired once |
| `Step(setting, direction)` | What a row's two buttons do: one choice down (`-1`) or up (`1`). Returns whether anything changed |
| `GetValueText(setting)` | The words a row shows, or nil when there is no such row |
| `ShowPage(page)` | `"Graphics"` or `"Display"` |
| `Apply()`, `Revert()` | As the buttons; both **yield** |
| `ResetToDefaults()`, `Close()` | As the buttons |

**Its words are the engine's catalog, and a game's own catalog wins.** To call
Apply something else, or to say it in a language the engine has no catalog
for, put the key in the project's `i18n/<locale>.json`:

```json
{ "engine.settings.apply": "Save" }
```

The keys are `engine.settings.*` for the screen and `engine.graphics.*` for the
settings and their choices; see [Localization](manual:guides/i18n).

## In the editor

**Project Settings, Graphics and Display** is `project.toml`'s `[graphics]` and
`[display]` as a form. Each setting has a box: ticked, the project says the
value and the row is the project's; unticked, the row shows greyed what the
level in force gives, and the key is not in the file. **Apply** writes the
ticked ones and removes the unticked, each under its own table, and leaves the
rest of the file -- its comments, its order -- as it was. "Remember the
player's settings" is `remember_player_settings`.

A game's options menu tried in Play changes the Viewport's picture, and not the
editor's own window, sync or frame rate: the display's settings are the game's
window's. Stop puts back what Play changed.

Not built yet: the levels side by side as a matrix, the Viewport's own level,
the editor's frame rate as a preference, and Play with a player's saved
settings.

## Where to look next

- [Graphics quality settings](manual:rendering/quality) — the keys, the ranges, the levels
- [Graphics settings belong to the player](manual:why/graphics-settings)
- [`GraphicsService`](api:GraphicsService) · [`LocalizationService`](api:LocalizationService)
- [A gamepad and the arrow keys](manual:ui/selection) — what drives the options screen
