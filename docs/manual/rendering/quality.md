# Graphics quality settings

Graphics settings are **engine settings, not `Lighting` properties**.
`Lighting` describes the world and travels with the scene; these describe the
machine it is being shown on. A scene must not decide a stranger's GPU budget.

This page is the project's side of them: the keys of `[graphics]`, their
ranges, and what each level sets. A player's choice and a script's --
`GraphicsService`, an options menu -- are
[Graphics and display settings](manual:rendering/settings).

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
anti_aliasing = "smaa"    # off | fxaa | smaa | taa (true is fxaa)
upscaling = "none"        # none | fsr1 | fsr2 -- how a render scale under 1 is brought up
sharpness = 0.2           # 0 to 1, after an FSR 1 upscale, after TAA and in FSR 2
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
| shadow filter taps | 4 | 8 | **16** | 16 |
| `anti_aliasing` | fxaa | smaa | **smaa** | taa |
| `upscaling` | fsr1 | none | **none** | none |
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

**The shadow filter's taps follow the shadow quality** (ADR 0172): how many
times the sun's shadow map is read at a pixel to soften its edge. They have no
key of their own -- `shadow_quality = "low"` is the small map, two cascades
and four taps together.

**On a phone every level upscales with FSR 1**, since each but Ultra caps the
resolution the world is drawn at, and uses FXAA below High and never TAA.
Bloom reaches less far there -- three levels of it where a desktop has five,
five passes where there were nine -- and contact shadows start at High
(ADR 0172); `contact_shadows = true` under `[graphics.android]` says
otherwise.

## Anti-aliasing and upscaling

Four ways to smooth edges (ADR 0158):

- **FXAA**: one pass over the finished picture, finding edges by contrast. The
  cheapest; it softens fine texture a little.
- **SMAA**: edges found by their shape and blended by the area each covers, in
  three passes. Sharper than FXAA for a little more. The default.
- **TAA**: each frame is drawn a fraction of a pixel off from the last, and
  blended into the frames before it through motion vectors. It is the one that
  stops a fence, a wire or far grass crawling as the camera moves. It runs on
  the world's main camera; a `ViewportFrame`, a sub-world and a picture of
  sprites alone use SMAA instead.
- **Off**.

**`upscaling = "fsr1"`** brings a world drawn at a `render_scale` under 1 up to
the window with AMD FidelityFX Super Resolution 1 -- an upscale that keeps
edges, then sharpening -- where `none` stretches it. `sharpness` is how much
RCAS sharpens after it, and after TAA, which softens.

**`upscaling = "fsr2"`** is AMD FidelityFX Super Resolution 2 (ADR 0164). Where
FSR 1 reads the one finished frame, FSR 2 builds each of the window's pixels
from the frames before this one: every frame is drawn a fraction of a pixel
off from the last, as under TAA, and over a few dozen of them every part of
every pixel has been rendered. A bar one pixel wide at half the resolution,
which no single frame holds, is put back.

- **It is the anti-aliasing too.** On a frame FSR 2 upscales, `anti_aliasing`
  is not read. At a `render_scale` of 1 it upscales nothing and smooths --
  sharper than TAA, and dearer.
- **The scale is `render_scale`**, and AMD's names for its modes are scales:

  | AMD's name | `render_scale` |
  |---|---|
  | Native | 1 |
  | Quality | 0.67 |
  | Balanced | 0.59 |
  | Performance | 0.5 |
  | Ultra performance | 0.33 |

- **`sharpness`** is FSR 2's own sharpening; 0 turns it off.
- **Exposure, bloom and the tonemap read the upscaled picture**, at the
  window's resolution. Depth of field, sun rays and a `BlurEffect` are done
  before it, at the world's.
- **What blends is handled for you.** Particles, glass and labels in the
  world have no motion of their own for a temporal pass to follow; the engine
  tells FSR 2 where they are, and they are drawn mostly from the current
  frame instead of being dragged into streaks.

It costs GPU time and memory that FSR 1 does not. On a GeForce RTX 4070 Ti
SUPER, what it adds to a frame:

| Window | At 1 | 0.67 | 0.5 | 0.33 |
|---|---|---|---|---|
| 1920 x 1080 | 0.5 ms | 0.4 | 0.3 | 0.25 |
| 2560 x 1440 | 0.9 ms | 0.6 | 0.5 | 0.45 |
| 3840 x 2160 | 1.9 ms | 1.3 | 1.1 | 0.9 |

Its images take about 150 MB at 1920 x 1080, 275 MB at 2560 x 1440 and 615 MB
at 3840 x 2160. A slower GPU pays more for the same pixels, which is why **no
preset turns it on**: a machine on Low is short of exactly that time. Offer it
in your settings screen -- the engine's own does -- or set it in the project's
file for a game you have measured. What it buys is the world drawn at half the
pixels or fewer: on a frame the GPU spends on the world, that is most of the
frame back.

**Where FSR 2 cannot run, FSR 1 does**, and the log says so once: a device
without compute shaders, a camera without perspective, a `ViewportFrame` or a
sub-world, and a picture of sprites alone.

**One thing it does worse than TAA**: at a `render_scale` of 1, something
thinner than a pixel in front of a far background -- a wire or a distant pole
against the sky -- shimmers as the camera moves, where TAA blurs it steady.
Below 1 such things are too thin to be drawn at all, by any upscale.

## Frame generation

**`frame_generation = true`**, under `[display]`, shows a frame the engine did
not draw between every two it did (ADR 0165): AMD FSR 3's frame generation. A game drawn sixty
times a second is shown a hundred and twenty. The frame between is made from
the two finished pictures of the world and from what the engine knows of each
pixel -- how far it moved and how far away it is -- in a few milliseconds of
GPU, where drawing it would have cost a whole frame.

- **It makes motion smoother, not the game faster.** Nothing is simulated for
  the made frame, and a drawn frame waits for the made one before it to be
  shown first: what the player does is seen **half a frame later** than
  without it, about 9 ms at sixty drawn frames a second. Use it over a base
  of sixty, where that is not felt and the made frames are close to the drawn
  ones; under thirty it shows its seams and the wait is 20 ms and more. A
  game played against the clock leaves it off.
- **The interface is never interpolated.** It is drawn on every frame shown,
  made or drawn, as sharp as it always is.
- **It works with every `anti_aliasing` and `upscaling`.** With FSR 2 it is
  what AMD calls FSR 3: the world drawn at a fraction of the window, built up
  to it, and a frame made between.
- **The display is waited for while it is on**, whatever `vsync` says: that
  wait is what gives the made frame and the drawn one each its turn on the
  screen. `max_frame_rate` caps the frames drawn; twice as many are shown.
- **It is one of the display's settings, not the level's.** No preset turns
  it on, choosing a level leaves it as it is, and with it on the level still
  reads as the level. Offer it in your settings screen -- the engine's own
  does, beside `VSync` -- with a word about the wait.

On a GeForce RTX 4070 Ti SUPER a made frame adds 1.2 ms to a drawn one at
1920 x 1080, 2.0 at 2560 x 1440 and 4.3 at 3840 x 2160, and its images take
161 MB, 286 MB and 643 MB -- a third less of each with FSR 2 at 0.67.

**What it gets wrong**: the strip of background a fast thing uncovers is in
neither drawn frame where the frame between needs it, and is painted from what
is round it -- a soft fringe a pixel or two wide at the thing's edges, for one
frame. Flames and glass, which have no motion of their own, are carried by
what the two pictures look like and can wobble against what is behind them.

**Where no frame is made, frames are shown as drawn**, and the log says so
once where it is the device: one without compute shaders, **macOS and iOS**
(Metal), a camera without perspective, a picture of sprites alone, the frame
after the camera was cut, and behind a loading screen. When a run ends the log
says how many frames were made.

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

**Nor is a view into a texture.** A `ViewportFrame`, a sub-world's picture
or a camera's is UI, and is drawn at its frame's own pixel size.

**A picture of sprites alone is not scaled.** A world with sprites and no mesh,
terrain or foliage is drawn at the window's resolution whatever `render_scale`
says, upscales nothing and takes no temporal pass. A 2D game is as sharp on a
phone, whose preset scales to three quarters, as on a desktop.

**Sprites among 3D surfaces follow the 3D picture**, except a sprite drawn in
its own colours (`Part2D.ExactColor`, the default). Pixel art is never
jittered or blended by TAA, never filtered by FSR 1 and never sharpened. Scaled
up, it takes the nearest texel.

The floor is a third, which is the furthest FSR 2 is made to bring a picture
up from. Stretched, or through FSR 1, a world drawn below half looks like what
it is under the crisp UI drawn over it: go below a half with FSR 2 only.

## Three layers, of six

Before a player or a script has said anything, a setting is the last of these
that says it:

1. **The preset** — a named set of every field.
2. **`project.toml`'s `[graphics]`** — the game author's default.
3. **The host's own flags** — what a person debugging, a benchmark or a
   capture uses.

A script's write and the player's saved choice go between the last two: over
the project's file, under a flag
([Graphics and display settings](manual:rendering/settings)).

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
--render-cap=N
--gpu-pass-times
--hide=LIST
--shadow-taps=N
--log-ui-touches
--bloom            / --no-bloom
--ambient-occlusion / --no-ambient-occlusion
--anti-aliasing    / --no-anti-aliasing
--anti-aliasing=off|fxaa|smaa|taa
--upscaling=none|fsr1|fsr2
--sharpness=F
--frame-generation / --no-frame-generation
--auto-exposure    / --no-auto-exposure
```

Parsing is strict: `--render-scale=0.75x` is a usage error rather than 0.75.

`--frame-report=10`, or `[debug] frame_report_seconds = 10` in the project's
file, writes a line to the log every ten seconds: the frames a second, the
median, 95th-percentile and worst frame, the draws and triangles, and the
resolution and level the world was rendered at. It is how a phone, which has
no profiler attached, says what it did in its first ten seconds and in its
seventh.

### Measuring a frame

A phone takes no command line, so each of these is also a key of `[debug]` in
the project's file. None is a setting of the game: they are off unless asked
for, and every frame report ends with a line naming the ones in force.

```toml
[debug]
frame_report_seconds = 10
gpu_pass_times = true
hide = "foliage,world_ui"
shadow_taps = 4
log_ui_touches = true
```

**`gpu_pass_times`** (`--gpu-pass-times`) adds a line under the frame report:
what each pass of the renderer took of the GPU, a frame's mean in
milliseconds, the costliest first.

```text
GPU milliseconds by pass, the mean of 412 frames ...: forward 6.21, shadow 3.02,
depth-prepass 1.10, bloom/bloom-down 0.61, fxaa+ui 0.52, upload 0.31. In all
11.77 ms, in 14 stops a frame; a stop whose pass clears one pixel takes 0.09 ms ...
```

A pass is named by the group it is drawn in, and by its own name after a
slash where the two differ. Everything drawn to the window itself is one
entry, named after all of it (`fxaa+ui`); hide `ui` to tell the two apart.

The engine has no timer on the GPU to read, so a timed frame is stopped after
every pass and the wait is the measure. Three things follow:

- **The frame rate of a timed run is not the game's.** Read the frame rate
  from a run without the key.
- Every pass carries what a stop costs, which the line says: a pass that
  reads near that floor cost nearly nothing.
- On a phone the GPU may slow its clock while it waits. Compare the passes
  with each other, and their sum with the median of an untimed run, rather
  than reading them as absolutes.

The picture is the one an untimed frame draws.

**`hide`** (`--hide=LIST`) is a list of what is not drawn, separated by
commas: `parts` (meshes that are none of the others), `skinned`, `terrain`,
`voxels`, `foliage`, `transparent`, `particles`, `ribbons`, `decals`,
`sprites`, `world_ui`, `ui`, `lights` (every light but the sun) and
`highlights`. The simulation goes on -- a hidden enemy still walks -- so the
difference in the frame's time is what drawing the thing cost.

**`shadow_taps`** (`--shadow-taps=N`) is how many taps the sun's shadow filter
takes at a pixel, 1 to 16, said over the level's own count (4 at low, 8 at
medium, 16 above).

**`log_ui_touches`** (`--log-ui-touches`) writes a line for each finger that
comes down: the whole name and rectangle of the element of the interface that
took it, or that none did and the finger is the game's.

What `[graphics]` already sets needs no key here: `shadow_cascades = 0`,
`contact_shadows`, `ambient_occlusion`, `bloom`, `anti_aliasing`, `upscaling`,
`foliage_density` and `render_cap` each take a cost out of the frame the same
way.

**On a phone, without a build for every setting.** A game exported with
`[debug] launch_arguments = true` takes the host's flags when it is launched,
as the `args` extra of the launching intent:

```text
adb shell am force-stop com.example.game
adb shell am start -n com.example.game/engine.player.PlayerActivity \
    --es args "--gpu-pass-times --frame-report=10 --hide=ui --render-cap=720"
```

Stop the game first: an intent sent to a game that is running starts nothing.
Without the key the arguments are ignored and the log says so -- any app on a
phone can send that intent, so a game that ships leaves the key out.

## Clamping

Values are clamped rather than refused, once, at the last door:
`render_scale` to 0.5–1.0; `render_cap` to 360–4320 when it is not zero; `shadow_resolution` to 256–2048 and then **down** to
a power of two; `shadow_cascades` to at most 4; `shadow_distance` to 10–1000;
`light_budget` to at most 256.

## More than the old keys

Any setting `GraphicsService` has is a key here, as its name in snake case:
`shadow_quality = "medium"`, `terrain_detail = 1.5`, `foliage_density = 0.5`.
A level also sets how much foliage is drawn -- half at low, three quarters at
medium, all of it from high -- over the project's own `[render]
foliage_density`.

## What a script can see

The settings, through `GraphicsService`. And, to adapt what the game does
rather than what it shows, the frame they produced:

```luau
--!strict
local DebugService = game:GetService("DebugService")

print(DebugService:GetStat("FPS"), DebugService:GetStat("FrameTimeMs"))
print(DebugService:GetStat("DrawCalls"), DebugService:GetStat("VisibleObjects"))
```

## Where to look next

- [Graphics and display settings](manual:rendering/settings) — a player's choice, by script
- [The post chain](manual:rendering/post) — what each toggle switches
- [Shadows](manual:rendering/shadows) — what the three shadow dials do
- [The debug overlay](manual:guides/debug-overlay) — reading those numbers live
