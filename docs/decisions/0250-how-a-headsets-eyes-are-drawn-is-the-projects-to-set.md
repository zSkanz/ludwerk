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
