# Graphics quality settings

Graphics settings are **engine settings, not `Lighting` properties**, and a
script cannot write them. `Lighting` describes the world and travels with the
scene; these describe the machine it is being shown on. A scene must not decide
a stranger's GPU budget.

There is no settings service and no in-game quality slider. The name is not
reserved either — declaring a class nothing implements is exactly what the API
definition forbids.

## The table

```toml
[graphics]
quality = "high"          # low | medium | high | ultra
render_scale = 1.0        # 0.5 to 1.0
render_cap = 0            # the most pixels on the world's shorter side; 0 is no cap
shadow_resolution = 1024  # texels; one cascade's tile, atlas is 2x2 tiles
shadow_cascades = 4       # 0 through 4; 0 is "the sun casts no shadow"
shadow_distance = 120.0   # metres
light_budget = 256        # lights one frame may carry
bloom = true
ambient_occlusion = true
contact_shadows = true
anti_aliasing = true
auto_exposure = true
```

Every key is optional. An absent one leaves whatever the preset chose.

## A platform's own table

A platform may have a table to itself, read over `[graphics]` on that platform
and nowhere else: `[graphics.android]`, `[graphics.windows]`,
`[graphics.linux]`, `[graphics.macos]`, with the same keys.

```toml
[graphics]
quality = "high"
shadow_distance = 140.0

[graphics.android]
quality = "low"           # high everywhere, low on a phone
render_cap = 720
```

## On a phone

**A handheld starts one level lower**: a project that names no level is
`medium` on Android and `high` on a desktop. The same frame is paid for there
in heat and battery, and a phone that starts at sixty frames a second and
warms is at thirty a minute later.

**And the world is rendered under a cap.** A phone's display is around 1440
pixels on its shorter side, at a density where no eye separates them: on a
handheld each level caps the world's shorter side -- 720 at low, 900 at
medium, 1080 at high, none at ultra -- and the picture is resolved up to the
display, as `render_scale` does. The interface is drawn at the display's own
resolution, so text stays sharp. `render_cap` in the project's file, or in
`[graphics.android]`, sets it by hand; `0` removes it. The terrain's level of
detail is chosen in the pixels the world is rendered at, so a capped picture
also builds and draws less ground.

## The presets

`high` is exactly what the engine ships with, to the value — a project that says
nothing gets it.

| Field | low | medium | **high** | ultra |
|---|---|---|---|---|
| `render_scale` | 0.75 | 1.0 | **1.0** | 1.0 |
| `shadow_resolution` | 512 | 1024 | **1024** | 2048 |
| `shadow_cascades` | 2 | 3 | **4** | 4 |
| `shadow_distance` | 70.0 | 100.0 | **120.0** | 160.0 |
| `light_budget` | 32 | 96 | **256** | 256 |
| `bloom` | false | true | **true** | true |
| `ambient_occlusion` | false | false | **true** | true |
| `contact_shadows` | false | true | **true** | true |
| `anti_aliasing` | true | true | **true** | true |
| `auto_exposure` | true | true | **true** | true |
| `depth_of_field` | false | false | **true** | true |
| `sun_rays` | false | true | **true** | true |

`depth_of_field` and `sun_rays` say whether a world's `DepthOfFieldEffect` and
`SunRaysEffect` are drawn on this machine; a world that has neither pays nothing
for either way (ADR 0096).

**The preset also sets how coarse distant terrain may be**: how many pixels
what a coarser level gets wrong may cover before a nearer, finer one is drawn
-- 4 at low, 3 at medium, 2 at high and 1.5 at ultra (ADR 0140). It has no key
of its own. See [Terrain](manual:world/terrain).

Two of those are worth a sentence each.

**Low turns down render scale first**, because it is the only dial that reduces
every per-pixel pass at once, and a machine that needs this preset is a machine
that is fragment-bound.

**Ultra is shadow density, not shadow range.** Its distance is 160 m rather than
something larger: at 220 m the far cascade measured barely better than high's,
so four times the atlas bought nine per cent, and a preset called Ultra was —
for anything past thirty metres — exactly as blocky as the one below it. At 160
it is genuinely sharper.

## What render scale touches

The **world** renders at that fraction and is upscaled into the target. The post
chain and the UI are unaffected: the 2D pass draws at full resolution on top,
which is the whole reason a render scale is worth having.

The floor is 0.5 because below that the world is upscaled by more than two and
the crisp UI drawn over it makes the difference impossible to ignore.

## Three layers

1. **The preset** — a named set of every field.
2. **`project.toml`'s `[graphics]`** — the game author's default.
3. **The host's own flags** — what a person debugging, a benchmark or a
   capture uses.

Each is expressed as an *override* rather than as a value, so that "nobody said
anything" and "somebody asked for the default" stay different answers. The
result is clamped once, at the end.

One rule is not implied by "each layer overrides the one before", and it is the
one that matters:

> **A preset the player names replaces the file's per-key entries as well as its
> level.**

A file's `shadow_resolution` is a refinement *of the level that file names*.
`--quality=low` says that level is not available on this machine, so carrying
its refinements across would hand a weak machine the single heaviest dial in the
file while every other one was turned down. The host's own per-key flags always
apply, because they were typed by the same person as the preset.

## The host flags

```text
--quality=low|medium|high|ultra
--render-scale=F
--shadow-resolution=N
--shadow-cascades=N
--shadow-distance=F
--light-budget=N
--frame-report=SECONDS
--bloom            / --no-bloom
--ambient-occlusion / --no-ambient-occlusion
--anti-aliasing    / --no-anti-aliasing
--auto-exposure    / --no-auto-exposure
```

Parsing is strict: `--render-scale=0.75x` is a usage error rather than 0.75.

`--frame-report=10`, or `[debug] frame_report_seconds = 10` in the project's
file, writes a line to the log every ten seconds: the frames a second, the
median, 95th-percentile and worst frame, the draws and triangles, and the
resolution and level the world was rendered at. It is how a phone, which has
no profiler attached, says what it did in its first ten seconds and in its
seventh.

## Clamping

Values are clamped rather than refused, once, at the last door:
`render_scale` to 0.5–1.0; `render_cap` to 360–4320 when it is not zero; `shadow_resolution` to 256–2048 and then **down** to
a power of two; `shadow_cascades` to at most 4; `shadow_distance` to 10–1000;
`light_budget` to at most 256.

## What a script can see

Not the settings — but the frame they produced:

```luau
--!strict
local DebugService = game:GetService("DebugService")

print(DebugService:GetStat("FPS"), DebugService:GetStat("FrameTimeMs"))
print(DebugService:GetStat("DrawCalls"), DebugService:GetStat("VisibleObjects"))
```

## Where to look next

- [The post chain](manual:rendering/post) — what each toggle switches
- [Shadows](manual:rendering/shadows) — what the three shadow dials do
- [The debug overlay](manual:guides/debug-overlay) — reading those numbers live
