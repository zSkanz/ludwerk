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

- [ ] VSync on by default (desktop and Android), `MaxFrameRate`, the background
  throttle; the editor's frame rate following the monitor by default.
- [ ] Measured: VSync off + 60 cap gives 16.7 ms ± jitter; the editor unfocused
  drops to the background rate; headless and `--pace` unchanged.

## G1 — the model (§1–2)

- [ ] The five layers, one effective-settings structure read by the renderer
  and the window; every §2 setting wired, the new ones (`ViewDistance`,
  `TerrainDetail`, `FoliageDensity`, `TextureQuality`, `ShadowQuality` as a
  level, window mode, monitor, brightness) included.
- [ ] Changes that rebuild GPU resources batched and budgeted.

## G2 — the script API (§3)

- [ ] `GraphicsService`, client-side; properties, signals, `ApplyPreset`,
  `ResetToDefaults`, `GetSource`, `GetSupportedResolutions`, `GetMonitors`,
  `GetRefreshRate`; conformance specs per property.

## G3 — saved per player, and Auto (§4–5)

- [ ] `SaveAsync`/`LoadAsync`, `settings.json` in the player's folder, loaded
  before the first frame; `remember_player_settings`; fallback for unsupported.
- [ ] `Auto`: the first-start probe, cached; the picked preset saved.

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

(Filled in as the work finds what this plan assumed wrongly.)
