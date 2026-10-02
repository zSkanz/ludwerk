# The mobile ledger: sixty frames a second on a phone

Opened 2026-10-02, from the first run on the owner's phone (S25 Ultra,
3120 by 1440): the far flight began near fifty-five frames a second and was at
twenty-seven to thirty-five a minute later, and neither the preset nor the
camera's `FarPlane` changed what the phone was asked to do. Android only (R15).
The settings are ADR 0147's; this ledger pulls forward what a phone needs of
them, and G1 to G5 (`settings-kickoff.md`) build the rest of the model on it.

**The target**: sixty frames a second, held, on that phone, in the far flight
and the platformer. The device is measured by the coordinating session; this
side hands over an APK and reads the log's frame reports.

## M1 -- what a phone is given by default

- [x] **A level lower** (ADR 0147 section 2): `medium` where nobody named a level.
- [x] **The world under a cap**: 720, 900 and 1080 pixels on the shorter side
  at low, medium and high; the interface at the display's resolution.
  `[graphics] render_cap`.
- [x] **A platform's own table**: `[graphics.android]` over `[graphics]`.
- [x] **The terrain's error budget in rendered pixels**, so the cap also cuts
  the ground built and drawn.
- [x] **The profile**: `--frame-report` and `[debug] frame_report_seconds`.
- [ ] Measured on the device: the far flight for two minutes, the reports at
  ten seconds and at a hundred and ten.

## What the owner's first evening on the phone found

- [x] **A tap presses the interface** (D430): buttons, fields, the keyboard.
- [x] **An exported game can open a socket** (D431): the manifest's permissions.

## M2 -- what is loaded and how far (after M1's numbers)

- [ ] `ViewDistance` (ADR 0147): a scale on the draw distance, the terrain's
  reach and the streaming radii, by level and by platform. The far plane's cut
  must not be seen (`settings-kickoff.md`, G1), and what streams is the
  simulation's on a server: the scale is the client's draw and load, not the
  world's `LoadRadius`.
- [ ] The frame rate a phone runs at: the display's 120 Hz, or a steady 60.

## M3 -- the interface at a phone's density

- [ ] An interface laid out in pixels is a third of the size on a phone's
  display. A scale by the display's density, so a button is as large in the
  hand as on a monitor.

## Later

- Shadow and post costs on a tile-based GPU, measured pass by pass.
- A thermal measure, so the engine lowers its level before the phone does.
