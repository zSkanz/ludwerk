# 0147 — Graphics and display settings are one model: the project sets them, the player chooses, a script reads and writes them

- Status: accepted; G0 to G3 built, G4 and G5 to build (`docs/briefs/settings-kickoff.md`; "As built" below)
- Date: 2026-10-01
- Decided by: the owner, on 2026-10-01: *"scripts têm que ser acessíveis também
  ... pra alterar presets de qualidade, por exemplo gráfico, limitação de FPS
  ... planejar totalmente isso aí, tanto pra ter interface na engine quanto pra
  ser possível alterar por script"*. The shape follows what professional engines
  ship (a quality model with presets and per-setting overrides, display and
  frame-rate settings, a saved per-player configuration, and a script API that
  an options menu is built on), under his standing rule of 2026-09-30.
- Builds on: ADR 0044 (quality presets), [0111](0111-a-game-saves-through-saveservice-into-the-players-own-folder.md)
  (the player's own folder), [0138](0138-a-script-carries-the-side-it-runs-on.md)
  (client-side code). Absorbs stage P6 of `docs/briefs/terrain-editing-perf.md`
  (the frame-rate cap).

## Context

What exists on 2026-10-01:

- Four presets (`Low`, `Medium`, `High`, `Ultra`) in `engine/render/src/settings.cpp`,
  chosen by `[graphics] quality` in `project.toml`, with per-key overrides
  (`render_scale`, `shadow_resolution`, `shadow_cascades`, `shadow_distance`,
  `light_budget`, `bloom`, `ambient_occlusion`, `anti_aliasing`,
  `contact_shadows`, `auto_exposure`, `depth_of_field`, `sun_rays`) and
  command-line flags over them.
- `[window] size` and `fullscreen`.
- **Nothing else**: no vertical sync and no frame cap anywhere (the editor draws
  2 000 frames a second); no script can read or change any of it; a player's
  choice is not saved; the editor has no panel for it; a game has nothing to
  build an options menu on.

## Decision

### 1. One model, five layers

A setting's value is the first of these that sets it:

1. the command line (development and benchmarks; `--pace`, `--quality`, ...);
2. **the player's saved settings** (§4);
3. what a script set this session (§3), which becomes (2) when saved;
4. `project.toml` (`[graphics]`, `[display]`), the game's defaults;
5. the preset the above chose, then the engine's defaults.

The effective values are what the renderer and the window use; every layer is
inspectable (§3 `GetSource`).

### 2. What is in it

**Parity is the bar** (the owner, 2026-10-01: *"deve ter a mesma que a Unity e
a Unreal têm"*). The model carries both shapes those engines expose, so a
person coming from either finds what they expect:

**Scalability groups** (the coarse level a player menu shows, one per group,
each `Low`, `Medium`, `High`, `Ultra`, `Cinematic`; the preset sets them all):

| Group | What it scales |
|---|---|
| `ViewDistance` | draw and streaming distances, terrain error budget, LOD bias |
| `AntiAliasing` | the AA method and its quality |
| `PostProcessing` | bloom, depth of field, sun rays, auto exposure, motion blur |
| `Shadows` | resolution, cascades, distance, softness, contact shadows, local-light shadows |
| `GlobalIllumination` | ambient occlusion now; the GI solution when the graphics ledger lands |
| `Reflections` | reflection quality (probes/screen-space when they land) |
| `Textures` | resident texture size, mip bias, anisotropic filtering |
| `Effects` | particles (count budget, soft particles, collision), decals |
| `Foliage` | density and draw distance |
| `Shading` | light budget, material complexity, water and terrain shading detail |

**Detailed settings** (each one a group sets, overridable on its own; any
override makes the preset `Custom`), covering what the detailed quality panels
of those engines list -- including `AnisotropicFiltering` (1–16), `LODBias`,
`MaximumLODLevel`, `SoftParticles`, `ParticleBudget`, `SkinWeights` (bones per
vertex), `TextureStreamingBudget`, `AsyncUploadBudget`, `MotionBlur`, `Fog`
quality, beside the ones below. A setting whose feature does not exist yet in
this engine (GI, reflections, TAA, upscalers) is declared now, documented as
"applies when that feature lands", and read by it when it does, so a game's
menu never changes shape.

**Per platform**: the project may give each platform (Windows, Linux, Android)
its own default level, as those engines' quality matrices do.

**Quality** (the renderer's, today's keys made first-class):

| Setting | Values |
|---|---|
| `QualityLevel` | `Low`, `Medium`, `High`, `Ultra`, `Custom` (any override makes it `Custom`), `Auto` (§5) |
| `RenderScale` | 0.25–2.0 (the upscaler path of the graphics ledger plugs in here) |
| `ShadowQuality` | `Off`, `Low`, `Medium`, `High`, `Ultra` (resolution, cascades, softness) |
| `ShadowDistance` | metres |
| `AntiAliasing` | `Off`, `FXAA` (and `TAA` when the graphics ledger lands) |
| `AmbientOcclusion`, `ContactShadows`, `Bloom`, `DepthOfField`, `SunRays`, `AutoExposure` | on/off |
| `ViewDistance` | a scale on draw and streaming distances |
| `TerrainDetail` | a scale on ADR 0140's per-pixel error budget |
| `FoliageDensity` | 0–1 |
| `TextureQuality` | `Low`, `Medium`, `High` (mip bias / resident size) |
| `ParticleQuality`, `LightBudget` | as today |

**Display:**

| Setting | Values |
|---|---|
| `WindowMode` | `Windowed`, `Borderless`, `Fullscreen` |
| `Resolution` | a `Vector2` from `GetSupportedResolutions()`; windowed: the window's size |
| `Monitor` | an index into `GetMonitors()` |
| `VSync` | on/off. **On by default.** |
| `MaxFrameRate` | 0 (no cap) or a number; applies with VSync off, and caps below the refresh with it on |
| `Brightness` | a gamma offset |
| `BackgroundFrameRate` | the cap while the window is unfocused or minimised (default 10, 0 = no throttle) |

Mobile (Android): VSync is always on; `MaxFrameRate` picks among the display's
supported rates (30/60/90/120); presets default one step lower than desktop.

### 3. By script

A client-side service, **`GraphicsService`** (a script that is not client-side
reads the defaults and a write raises a keyed error; a dedicated server has no
display):

```luau
local Graphics = game:GetService("GraphicsService")
Graphics.QualityLevel = Enum.GraphicsQuality.High
Graphics.VSync = false
Graphics.MaxFrameRate = 144
Graphics.WindowMode = Enum.WindowMode.Borderless
Graphics:SaveAsync() -- remember the player's choice
```

- Every setting in §2 is a property, validated and clamped, with an enum where
  the values are a set. The groups are `GetGroupLevel(group)` /
  `SetGroupLevel(group, level)` with `Enum.GraphicsGroup` and
  `Enum.GraphicsLevel`, as a player menu wants them.
- `Changed` fires per setting (as any property); `QualityChanged` fires when the
  preset or any quality setting changes.
- `ApplyPreset(level)`, `ResetToDefaults()`, `GetSource(name): Enum.SettingSource`
  (`CommandLine`, `Player`, `Script`, `Project`, `Preset`, `Engine`).
- `GetSupportedResolutions(): {Vector2}`, `GetMonitors(): {{Name, Size, RefreshRate}}`,
  `GetRefreshRate(): number`.
- `SaveAsync()`, `LoadAsync()` (§4).
- **Applying**: a change takes effect at the next frame. Changes that rebuild GPU
  resources (shadow resolution, render scale, window mode, resolution) are
  batched so a menu that sets ten things rebuilds once, and are held to a stated
  budget with no frame over it being presented half-built.
- `GraphicsService` is in `docs/api/`, has its icon, and every enum is i18n'd
  where a player sees it.

### 4. Saved per player

- `SaveAsync` writes the player's choices (only the ones they changed) to their
  own folder, beside the game's saves (ADR 0111), as `settings.json`.
- They are loaded **before the first frame**, so a game starts in the player's
  window mode and resolution, not the project's for a second.
- `[display] remember_player_settings = true` (default) can be turned off by a
  game that manages its own.
- A saved setting the hardware no longer supports (a resolution, a monitor)
  falls back to the project's default and is reported, not applied.

### 5. Auto

`QualityLevel = Auto` (the default for a new project on first launch) measures
the machine once (a short GPU and CPU probe at first start, cached) and picks
the preset whose measured frame time fits the display's refresh; the choice is
saved as the player's.

### 6. In the editor

- **Project Settings → Graphics and Display**: the default preset, each
  override with its preset value shown greyed until overridden, the default
  window mode, resolution, VSync and frame cap. It reads and writes
  `project.toml`; every label i18n'd. It is laid out as a **quality matrix**:
  the levels as columns, each group and detailed setting as a row, editable
  per level, with a column per platform's default -- the panel those engines
  give a project.
- **Scalability quick menu** in the viewport toolbar, as the editor of one of
  those engines has: the groups at a glance, one click per level, for the
  viewport.
- **Editor Preferences → Performance**: the editor's own frame rate (**match
  monitor by default**; 30, 60, 120, 144, 240, unlimited), the background
  throttle (on by default), and the editor viewport's quality preset, separate
  from the game's so a heavy game can be edited on a light laptop.
- **The viewport toolbar**: a quick menu with the viewport's preset and the FPS
  readout.
- **Play mode** starts with the project's defaults; a toggle "use my saved player
  settings" tests the player path.

### 7. A ready-made options screen

`@engine/settings` (a module, opt-in): one call builds a themed, i18n'd options
screen over `GraphicsService` (presets, each setting, display, apply, revert,
save), for a game that wants one without writing it. A game's own menu uses the
same API.

### 8. What it costs and proves

- The frame cap is measured: with VSync off and `MaxFrameRate = 60`, frame
  times are 16.7 ms ± a stated jitter; with VSync on, the present waits for the
  display; unfocused, the editor drops to the background rate.
- A picture test per preset (the terrain and look galleries at each preset).
- A conformance spec per property (bounds, enums, layers, `GetSource`), a
  persistence round trip, and the fallback for an unsupported saved setting.
- Headless and benchmark runs are unaffected (`--pace` stays theirs).

## Consequences

- A game's options menu is a few lines, or one call.
- The editor stops burning a GPU at 2 000 frames a second.
- A player's choices survive restarts, and a game starts in them.

## Not decided here

- HDR output, a per-monitor colour profile, and DLSS/XeSS-class upscalers: the
  graphics ledger's.
- Audio and input settings: their own services (audio volumes and key
  rebinding have their own APIs already); a unified options screen may gather
  them later.

## As built, 2026-10-02 (G1 to G3)

- **The model is `scene::GraphicsModel`**, in the world's engine state: a
  layer of plain numbers each for the command line, a script, the player, the
  project and each of the four presets, and the rule for which is read. In
  `scene` because a script writes it and a script reaches a world. The host
  keeps its own layers and puts them back every frame, so a world restored to
  an earlier tick does not bring back an old command line -- and the editor's
  Stop undoes what a game's menu did in Play, for nothing.
- **A script's write is over the player's saved choice**, where section 1
  lists them the other way. A menu's change that waited under the saved value
  until `SaveAsync` would be a menu that does nothing; the newest word is the
  one in force, and `GetSource` says `Script` until it is saved and `Player`
  after. The command line is over both.
- **`project.toml` is read as before and resolves to the same settings**:
  `ProjectConfig::graphics` and the model's own resolution are asserted equal,
  field by field, over files, flags and both kinds of machine
  (`project_config_tests.cpp`). Any setting is now a key, as its name in snake
  case; a choice takes its item's name in lower case.
- **A level sets foliage density too**: 0.5 at low, 0.75 at medium, 1 from
  high. It set nothing there before, so a project at low or medium -- and
  every handheld, which starts at medium -- draws less foliage than it did.
- **`QualityLevel` written is `ApplyPreset`**: every quality setting becomes
  the level's. It reads `Custom` when one is not; `Custom` cannot be written.
  `Auto` is the machine's own starting level until the probe of section 5 is
  built.
- **`ShadowQuality` is a level over two numbers**: writing it writes
  `ShadowResolution` and `ShadowCascades`.
- **`TerrainDetail` is a scale on the two pixels a cell `High` allows**, so the
  four levels are 0.5, 0.67, 1 and 1.33 and the numbers of ADR 0140 are what
  they were.
- **Applied once a frame**: the host compares a hash of every value in force,
  and when it moves gives the renderer its settings whole, the foliage its
  density, the swapchain its sync, the limiter its caps and the window its
  mode. A level set by a script while a game runs draws, to the pixel, what a
  game started at that level draws (`graphics_runtime_differential`: 0
  differing pixels against `--quality=low`, 51 187 against a pinned `ultra`).
  **No budget is held on what a change rebuilds**, which G1 asked for: a
  change of shadow resolution reallocates the atlas in the frame it lands in.
- **Kept and not drawn by yet**, as section 2 allows for what does not exist:
  `ViewDistance`, `TextureQuality`, `AnisotropicFiltering`, `LODBias`,
  `MaximumLODLevel`, `ParticleBudget`, `SoftParticles`, `SkinWeights`,
  `TextureStreamingBudget`, `AsyncUploadBudget`, `MotionBlur`, `FogQuality`,
  `GlobalIllumination`, `Reflections`, `Brightness`. `IsApplied` says which,
  so a menu need not know this list. `ViewDistance` waits on the far plane's
  fade and `Brightness` on a term in the final resolve.
- **The window**: `platform::displays`, `displayModes`, `windowDisplay` and
  `setWindowMode`. Borderless is the desktop's own mode on the display the
  window is on; fullscreen is the display's nearest mode to `Resolution`. The
  editor's own window, sync and caps are never a game's to set.
- **The player's file is `settings.json` beside the saves folder** -- under
  `.engine/` for a project being made -- holding only what was chosen, read
  before the window is made. A name this build does not know, a value of the
  wrong kind and a `Monitor` that is not plugged in are left out and logged.
- **`SaveAsync` and `LoadAsync` yield** to the frame's end and return whether
  the host did it, since the suffix is a promise (api-design section 9).
  `IsApplied` is one method more than the ADR listed.
- **A dedicated server has no display**: it reads the defaults and a write
  raises. The script-side lint counts `GraphicsService` as the player's.
- **A new service is a new instance in every world**: the determinism traces
  were recorded again, as for the two services before it. No property of it is
  hashed, saved with a scene or sent.
