# 42-nightfall

Light that bounces in the open, under a light that never stands still, and a
sequence that waits for a decision (ADRs 0203, 0204, 0206).

```
run.bat
```

A courtyard as the sun goes down. A walker crosses to a doorway in a red wall;
a torch flickers under a canopy beside it.

- **There is no room to draw a box round**, so the bounced light is an
  `IrradianceVolume` that `FollowsCamera`: the part under it gives its size
  and the volume stands round the camera as three `Levels`, each twice the
  last, carried after the camera a few cells at a time. The wall reddens the
  ground beside it wherever the camera has walked to.
- **The tone is the authored one.** `Mode = BounceOnly` leaves
  `Lighting.Ambient` lighting the place exactly as typed, and adds only what
  bounces. `Tint` is a look on top.
- **The sun moves every frame and the torch flickers every frame**, and the
  volume is `Dynamic`: its levels are lit again one at a time, each by the
  light there was when its turn began, inside
  `GraphicsService.GlobalIlluminationBudget` milliseconds a frame. `IsReady`
  goes true once and stays true.
- **The sequence keys the hour on `Lighting` itself.** A service is bound
  like anything else: `bind("Lighting", Lighting)`, and a property track on
  `ClockTime`.
- **It fades in from black behind the bars of a wider picture, and shows a
  caption nobody says**: `fade`, `bars` and `caption` tracks.
- **The camera stays at her shoulder** with `relativeTo`, wherever the
  script's own walk has put her.
- **It stops at a hold** eight seconds in, until `Release`. She goes on
  standing there; the sun does not go on setting.
- **It has three shots**, marked; `SkipToNext` lands the hour where the
  sequence would have left it.

Keys:

- **Space** or **A**: go on from the hold.
- **N** or **X**: the next shot.
- **Enter** or **Start**: play it again.

The manual's pages are
[Indirect light](../../docs/manual/rendering/indirect-light.md) and
[Cinematics](../../docs/manual/animation/cinematics.md).
