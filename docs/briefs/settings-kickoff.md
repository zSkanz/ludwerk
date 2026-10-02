# Graphics and display settings: the kickoff and the ledger

Decided on 2026-10-01: [ADR 0147](../decisions/0147-graphics-and-display-settings-are-one-model-the-project-sets-the-player-chooses-a-script-reads-and-writes.md).
The owner: *"tanto pra ter interface na engine quanto pra ser possível alterar
por script"*.

**Place in the queue:** it replaces stage P6 of `docs/briefs/terrain-editing-perf.md`
(the frame-rate cap) and runs right after that ledger's P1–P5, before the
editor-i18n E4/E5 and before water. If P6 work has started, it folds in here.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test or a failing measurement first.
- Every string through i18n keys; `GraphicsService` and every new enum in
  `docs/api/` with icons.
- No determinism trace moves: settings are presentation, never simulation.
- Full localgate, Linux and Android included, before every push.

## G0 — the frame pacing (urgent part, first)

- [x] VSync on by default (desktop and Android), `MaxFrameRate`, the background
  throttle; the editor's frame rate following the monitor by default.
  `[display] vsync`, `max_frame_rate`, `background_frame_rate` in
  `project.toml`, the flags over them; `rhi::IDevice::setVSync` asks the
  swapchain for it by name; `FrameLimiter` holds a cap to a grid, and
  `SyncWatch` puts the refresh in its place where the display's sync was asked
  for and is not holding -- no backbuffer, or a driver told to ignore it. A
  throttled frame runs every tick it owes (`catchUpTicksFor`): ten frames a
  second at sixty ticks is six a frame, where the clamp of four ran the game
  at two thirds of its speed.
- [x] Measured (`examples/03-physics-playground`, `--frame-stats`, 300 frames,
  the development machine, a 240 Hz display):

  | | Median | p99 | Worst |
  |---|---|---|---|
  | `--no-vsync --max-frame-rate=60` | **16.665 ms** | 17.04 ms | 17.45 ms |
  | `--no-vsync`, no cap | 0.78 ms | 1.88 ms | 2.26 ms |
  | VSync, in front (the default) | 8.53 ms | 12.84 ms | 13.06 ms |
  | Minimised, the background rate | **100.02 ms** | 100.54 ms | 100.61 ms |
  | The editor, in front (the default) | 8.53 ms | 13.20 ms | 13.41 ms |
  | The editor, `--no-vsync` and no throttle | 0.94 ms | 1.73 ms | 2.63 ms |

  A minimised window had no backbuffer to wait on, and its loop ran flat out:
  that is the two thousand frames a second. Headless and `--pace` are not
  paced by any of it; their tests and goldens did not move.
- [ ] The editor's own rate as a preference (match the monitor, 30 to 240,
  unlimited) is G4's, with the rest of Editor Preferences -> Performance.

## G1 — the model (§1–2)

- [x] The layers, one effective-settings structure read by the renderer and
  the window: `scene::GraphicsModel`, resolving to exactly what the project's
  file resolved to before. Wired: `TerrainDetail`, `FoliageDensity`,
  `ShadowQuality` as a level, window mode, monitor, resolution.
- [ ] `ViewDistance`, `TextureQuality` and `Brightness` are kept and not drawn
  by yet (with the settings whose features do not exist): the first waits on
  the far plane's fade below, the second on a sampler bias the RHI has no word
  for, the third on a term in the final resolve.
- [x] Changes batched: applied once a frame, whole.
- [ ] And budgeted: a change of shadow resolution reallocates the atlas in the
  frame it lands in.
- [ ] **The far plane's cut is never seen** (from the terrain-editing
  ledger's P5, with `ViewDistance`): what is drawn fades into what is behind
  it over the last tenth before the plane -- in the terrain's and the parts'
  shaders -- so a world wider than the view ends in air, not on a line. A
  scene has no fog unless it sets `FogEnd` or wears an `Atmosphere`, and
  clamping either to the plane would move every golden.
- [ ] **VSync at the display's own rate** (G0's finding): a 240 Hz display
  shows a frame every second or third refresh in a window. Measured windowed,
  borderless and exclusive fullscreen -- the owner runs the editor windowed --
  with the frames SDL allows in flight and what the compositor does to a
  windowed swapchain.

## G2 — the script API (§3)

- [x] `GraphicsService`, client-side; properties, `QualityChanged`,
  `ApplyPreset`, `ResetToDefaults`, `GetSource`, `IsApplied`, `GetGroupLevel`,
  `SetGroupLevel`, `GetSupportedResolutions`, `GetMonitors`, `GetRefreshRate`;
  `graphics.spec.luau`, and `graphics_runtime_differential` -- a level set by
  a script at run time is, to the pixel, a game started at that level.

## G3 — saved per player, and Auto (§4–5)

- [x] `SaveAsync`/`LoadAsync`, `settings.json` in the player's folder, loaded
  before the window is made; `remember_player_settings`; a setting this build
  or this machine cannot use is left out and logged.
- [ ] `Auto`: the first-start probe, cached; the picked preset saved. Until
  then `Auto` is the machine's own starting level.

## G4 — the editor (§6)

- [ ] Project Settings → Graphics and Display (writes `project.toml`).
- [ ] Editor Preferences → Performance (editor frame rate, throttle, viewport
  preset).
- [ ] Viewport toolbar quick menu; Play mode's "use my saved player settings".

## G5 — the ready-made screen and the close (§7–8)

- [ ] `@engine/settings`, an options screen in one call, themed and i18n'd.
- [ ] A picture test per preset; the manual page `docs/manual/graphics/settings.md`
  ("for players", "by script", "in the editor").
- [ ] An example game uses `@engine/settings`; the package regenerated; the owner
  opens the editor (it no longer runs at 2 000 FPS) and the example's menu.

## Findings

- **G0: with VSync on, this machine's 240 Hz display shows a frame every
  second or third refresh** -- 8.5 ms median for a frame that costs under one
  -- in a window, on the D3D12 backend, in the dev build with the GPU debug
  layer. The sync holds; the rate is half the display's. To look at with the
  window modes (G1): the frames SDL allows in flight, and what the compositor
  does to a windowed swapchain.
- **G0: the RHI gained one method**, `IDevice::setVSync`, with a body that says
  no -- a backend with no display has nothing to set. ADR 0043 froze the seam
  against backend types and draw paths; this is neither, and is recorded here
  rather than passed over.
- **G0: nothing could say why the owner's editor ran unpaced in front**, where
  SDL's default present mode is already the display's sync and this machine
  holds it. `SyncWatch` is the answer that does not need to know: thirty frames
  in a row faster than the display could have let through, and the refresh is
  the cap.
- **G1: the order of two layers is the other way round from the ADR's
  list.** A script's write is over the player's saved choice: a menu whose
  change waited under the saved value until it was saved would do nothing.
- **G1: a level had no foliage in it**, so the Foliage group's four levels
  were one. Low and medium now draw half and three quarters of it -- which
  changes what a handheld draws, since it starts at medium.
- **G2: a script's first line ran before the frame had given the world its
  layers**, and read a model with no levels in it: `ShadowDistance` was the
  engine's default with nobody's name on it. The world is given the layers
  when it is made; the conformance spec found it.
- **G2: `renderer->setSettings` at run time is exact.** The renderer sizes its
  targets from its settings each frame, so nothing had to be taught to
  rebuild: 0 differing pixels against a run started at the level.
