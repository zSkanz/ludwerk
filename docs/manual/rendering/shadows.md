# Shadows

**The sun casts shadows, and so can a lamp.** The sun (or the moon at night)
casts through cascaded shadow maps. A `PointLight` or `SpotLight` with
`Shadows = true` casts through a shared atlas of sixteen tiles: a spot takes one
tile and a point light six. [Point and spot lights](manual:rendering/lights)
says what happens when more lights ask than there are tiles.

## What a script controls

Two `Lighting` properties shape the sun's shadow:

- **`Lighting.GlobalShadows`**: whether the sun or moon casts at all. Off is the
  look of an overcast day, or a stylised world with no shadows; lamps that cast
  still do.
- **`Lighting.ShadowSoftness`**: how soft the edge is, from 0 (as hard as the
  shadow map allows) to 1 (a quarter of a metre of penumbra either side). The
  default, 0.2, is the edge the engine always drew.

What a script controls is **where the sun is**, and that is `Lighting.ClockTime`
and `Lighting.GeographicLatitude`:

```luau
--!strict
local Lighting = game:GetService("Lighting")

-- Early morning: long shadows, raking across the ground.
Lighting.ClockTime = 7.5
Lighting.GeographicLatitude = 45
```

A low sun gives long shadows and a hard test of the shadow map; a sun overhead
gives short ones and hides most of what a cascade split does.

## What casts: `CastShadow`

Every part casts a shadow -- a `Part`, a `MeshPart`, a character -- until it is
told not to:

```luau
local flash = Instance.new("Part")
flash.CastShadow = false   -- still drawn, still lit; it has no shadow
```

| What it is | `CastShadow` |
|---|---|
| Something with weight in the scene: a wall, a crate, a character | `true`, the default |
| Light, or an effect: a muzzle flash, a blade's trail, a pickup's glow, a ring on the ground under a boss | `false` |

It is read by the sun's shadow and by every lamp's, it replicates like any
part of a part's look, and it changes only what the part **casts**: a part
with it off still receives the shadows of others.

A part that is see-through (`Transparency` above 0) casts no shadow whatever
this says, and a hidden one draws nothing at all. Turn `CastShadow` off rather
than fading a part by a few percent to lose its shadow: a faded part is drawn
in the blended pass, sorted every frame, and does not batch.

## What the project controls

Three dials, in `project.toml` under `[graphics]` and on the host's own command
line. They are engine settings rather than scene properties, because a scene
must not decide the player's GPU budget.

| Setting | Default | Range |
|---|---|---|
| `shadow_resolution` | 1024 | 256 to 2048, rounded **down** to a power of two |
| `shadow_cascades` | 4 | 0 to 4. **0 means the sun casts no shadow** |
| `shadow_distance` | 120.0 | 10 to 1000 metres |

```toml
[graphics]
shadow_resolution = 2048
shadow_cascades = 4
shadow_distance = 160.0
```

**Fewer cascades buy submission, not memory.** The atlas is two tiles by two
whatever the count is, so `shadow_resolution` is the dial that buys memory —
512 costs 4 MiB, 1024 costs 16 MiB, 2048 costs 64 MiB. What fewer cascades
remove is drawing: every caster is drawn once per cascade it touches.

A cascade nothing renders into is a cleared tile, and a cleared depth reads as
lit.

## How it works, briefly

Four cascades in one atlas, split by a blend of uniform and logarithmic
spacing. The filter radius is constant in **world** space across cascades, so
softness does not change when an object crosses a split, and the last part of
each cascade's range samples both neighbours and blends — so a split is not a
visible line.

The ortho box is snapped to texel increments in light space, which is what stops
the shadow crawling as the camera moves.

## Two honest gaps

- **A see-through part casts no shadow at all.** A pane of glass does not
  darken or tint what is behind it; a part is either solid and casts, or
  blends and does not.
- **An alpha-masked mesh material writes depth where its own fragments would
  have been discarded**, so a cut-out quad on a mesh casts the shadow of the
  whole quad. Blocks are the exception: a `Cutout` block type's shadow is drawn
  with the same hole test as the block itself, so a tree of leaf blocks casts
  its leaves rather than a square.

## Where to look next

- [Lighting and the sky](manual:rendering/lighting)
- [Graphics quality settings](manual:rendering/quality) — the three layers those
  dials go through
- [Point and spot lights](manual:rendering/lights)
