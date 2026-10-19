# 0250 — How a headset's eyes are drawn is the project's to set

- Status: accepted
- Date: 2026-10-18
- Builds on: ADR 0244 (the headset's frame), ADR 0248 (a VR game ships to
  the Quest)

## Context

Making a Quest 3 hold 72 Hz took a set of fixed numbers in the renderer
and the host: two samples a pixel, 2048-texel shadow tiles over 20 meters
in two cascades, a first cascade 2 meters deep, no screen effects,
foveation, and the runtime's own refresh rate. Each was measured on the
device. A game sees none of them and can change none of them, though they
are what decides whether its own scene holds its frame rate.

## Decision

1. **`[xr]` in `project.toml` holds them**, with `[xr.android]` over it on
   a standalone headset: `msaa`, `render_scale`, `shadow_resolution`,
   `shadow_distance`, `shadow_cascades`, `shadow_first_split`,
   `screen_effects`, `foveation`, `refresh_rate`, as well as the existing
   `depth`. Each one defaults to the value measured on a Quest 3, or to
   the PC's own where none was measured. A value out of range is left out,
   and the default stands. These are the project's settings, not defaults
   forced onto content: a game that draws less can ask for more.
2. **The log says once a session what it runs with**, after
   `[xr.android]` has been applied: one line with every resolved value.
3. **The refresh rate is a request, never a condition.** Where the runtime
   lets a game choose (`XR_FB_display_refresh_rate`), the log lists the
   rates offered and the session runs at the nearest one to what was
   asked, and says so when it is not the same. Where the runtime refuses
   or offers none, the session goes on at the runtime's own rate. The
   default on a standalone headset is 72 Hz until 90 is measured.
4. **`XRService` has the same values as properties**: `Msaa`,
   `RenderScale`, `ShadowResolution`, `ShadowDistance`, `ShadowCascades`,
   `ShadowFirstSplit`, `ScreenEffects`, `Foveation` (`Enum.XRFoveation`)
   and `RefreshRate`. Read, a property gives what the session runs with:
   the project's value until a script sets one. Set, a value is checked
   against its range and refused outside it, and applied at the next frame.
   The values are host facts: they are not saved, not replicated and not
   hashed. A change to `Msaa`, `RenderScale` or `ShadowResolution` makes
   the targets it changes again, which costs a hitch at the switch.
5. **Foveation off means no density map.** At level 0 no map is made and
   none is attached to a pass, so off is the baseline that the other
   levels are measured against. Medium is the default on a standalone
   headset. On a Quest 3 it took the forward pass from 5.8 to 4.1 ms; high
   measured the same and showed steps on edges toward the lens's rim.

## Consequences

- A game tunes its headset build without touching the engine, and the log
  of a session carries what it was measured with.
- The renderer's fixed numbers for a standalone headset remain the
  defaults. A project that sets nothing runs as before, except for
  foveation, which is now medium instead of high.
- The editor's project settings show the same keys (stage four of this
  work).

## As built, second step (2026-10-19)

6. **The tonemap is folded into a headset eye's forward pass.** Where an
   eye is drawn with samples straight into the runtime's 8-bit sRGB image,
   and nothing drawn needs the frame's light after the pass (no grade,
   blur, bloom or light shafts; no ground, blocks, foliage, sprites or
   surface shaders), the lit surfaces, the sky and the world's interface
   expose, tonemap and dither what they write, and the samples resolve into
   the image. Neither the 16-bit target nor the tonemap pass is in the
   frame: on the PC's standalone profile a frame of the XR room went from
   2.30 to 1.80 ms of GPU, the tonemap's 0.31 ms gone. The exposure is the
   meter's, read back a few frames late through `IDevice::requestReadback`
   (a ring of copies taken once the GPU is known to be done with them,
   never waited on), and the meter reads the folded picture with the curve
   and the exposure undone; it agrees with the 16-bit meter within 1.1%.
   Blending happens after the curve, so a dark translucent surface over a
   bright one shows lighter than in the 16-bit frame; the world's interface
   is drawn as the display shows it (item 7) and is not affected.
   **Automatic exposure stays the default in a headset.** Measured in the
   XR room, turning from the table to the sky moves it by a third of a stop
   in about a second, with no overshoot; a fixed neutral exposure would draw
   every headset game 1.3 stops darker. A game that wants it fixed sets
   `Lighting.AutoExposure` to false.
7. **The world's interface is drawn as the display shows it.** At a
   `Brightness` of 1 or less and a `LightInfluence` of 0, a `SurfaceGui`'s
   or a `BillboardGui`'s colors are shown as they were set, blended in the
   display's linear light: in a folded eye, in the pass itself; anywhere
   else, into an 8-bit sRGB layer of its own, tested against the scene's
   depth, which the tonemap pass lays over the picture. Both give the same
   blend; the meter reads the interface as part of the picture in both, as
   it always did. Above a brightness of 1 the interface is light in the
   scene as before: exposed, tonemapped, and it blooms.
8. **`LightInfluence` on `SurfaceGui` and `BillboardGui`** (0 to 1, 0 by
   default): at 1 the interface is lit as a part of its colors on that face
   would be -- the same lighting function as the surfaces, with the sun in
   the shadows its cascades hold, the sky, the lamps, the probes and what
   the screen bounces -- dielectric and as rough as a plain part, then
   exposed and tonemapped with the scene. Between, a linear mix of its own
   colors and the lit ones. A part's color is used as linear albedo, with no
   sRGB decode, and the lit interface takes its colors the same way so that
   the two match; whether a part's color should be decoded is a separate
   question. Measured: a `SurfaceGui` beside a part of its color, across a
   shadow's edge, matched it within one level in the shadow, on a screen and
   in a headset, at 15:00 and at 20:30.
9. **`ParticleEmitter.LightInfluence`** (0 to 1, 1 by default, which is how
   blended particles were always lit): at 1 a particle takes the ambient
   and the sun, now in the sun's shadow at its center, and where the shadow
   takes the sun it takes the sky's light that a surface facing the camera
   would; at 0 it keeps its own color. The light a particle adds
   (`LightEmission`) is never shadowed. The shadow and the sky are looked up
   once a particle, in the vertex stage, for both the CPU and the GPU paths:
   ten thousand GPU particles in a shadow cost the same with it as without.
   In the sun a particle looks exactly as before.
10. **`render_scale` sizes the runtime's images.** A headset makes its eyes'
    images that fraction of the size it recommends (`IHeadset::setRenderScale`),
    remade between frames when `XRService.RenderScale` changes, and its
    compositor brings them up to the display; the renderer draws each eye
    whole. The log says the size chosen. A headset that cannot leaves the
    scale to the renderer, which then draws smaller and does not fold.
11. **What an eye draws with is made behind the loading curtain.** The
    pipelines of an eye's forward pass, in their twins at the eye's samples,
    density map and image format, are made with the renderer's other
    pipelines before the first frame is shown, where they had been made at
    their first draw -- the hitches of a headset's first seconds. A twin that
    takes longer than 20 ms is said in the log, as a pipeline is.
12. **A first split ends the first cascade, and the last drawn cascade ends at
    the shadow distance.** The four cascades used to divide the distance
    whatever was drawn, and a first split past the one they gave was
    dropped: a standalone headset's two cascades ended at 2.7 meters of 20,
    and nothing farther cast a shadow. A screen's settings, which ask for no
    first split, divide the distance as before.
13. **With the room seen through, what blends is over the room.** The eye's
    alpha had been read from its depth -- none where nothing wrote depth --
    so the world's interface, transparent parts and particles, which write
    none, were drawn under the room and vanished on a Quest. An eye with the
    room seen through is now drawn with nothing behind it: no sky, cleared
    to no coverage, the tonemap writing what each pixel's draws cover, and
    the copy into the runtime's image takes its alpha from that and divides
    the color by it, since the runtime takes it unpremultiplied. A pixel
    nothing covers is metered as the light measured the frame before, so an
    empty view neither opens the exposure nor shuts it.
14. **`BasePart.ShowsRoom`: a window onto the room.** A part with it on is
    drawn solid with no coverage: it hides what is behind it, what is in
    front of it is drawn over the room, and with the room seen through the
    room shows in its place -- a real desk under a virtual one, or a portal.
    In any other picture the coverage is not read, and the part looks as it
    otherwise would. It casts no shadow. It travels on the wire as a look,
    as `CastShadow` does (protocol 59).
15. **The window can show a camera of the game's.**
    `XRService.SpectatorCamera` set, the window on a computer shows the
    world through that camera while the headset draws, extracted and drawn
    for the window at its size and the screen's quality, after the eyes;
    nil keeps the left eye. `[xr] spectator = false` keeps the left eye
    whatever a script sets, and a standalone headset, which has no window,
    draws nothing more. **It is drawn every other frame of the headset's**
    (`[xr] spectator_every`, 1 to 8), into a picture of its own that the
    window shows in every frame: a frame is one submission, so the runtime
    waits for what is drawn after the eyes as well, and a view for watching
    should cost the player as little as it can. Measured on a desktop GPU at
    1600 by 900 and high quality, with the GPU stopped after each pass: a
    frame's mean went from 5.9 ms without a spectator to 11.4 drawn every
    frame and 9.1 every other frame. The next step is to draw it after the
    eyes' submission, in a second one of its own, so that the runtime never
    waits for it at all.
