# 0246 — UI in a headset is a panel pointed at, and comfort is a module

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0244 (OpenXR), ADR 0245 (`XRService`), `SurfaceGui` and
  `ScreenGui`

## The question

A screen's UI is drawn over the picture, at the screen's pixels; in a
headset there is no screen to draw it over, and a UI glued to the eyes is
unreadable and sickening. And a player who walks with a stick while standing
still is the main cause of discomfort in VR: every VR game offers teleport,
snap turns and a vignette, and a game made with the engine should not have to
write them first.

## Decision

### 1. A `ScreenGui` becomes a panel in front of the player

In a headset, each enabled `ScreenGui` is drawn into a texture at its own
resolution (`ScreenGui.XRPixelsPerMetre`, 1,000 by default) and shown on a
flat panel in the world, `ScreenGui.XRDistance` metres in front of where the
head was when the panel appeared (1.5 by default), turning to follow the head
only when it has turned more than 30 degrees away (`XRFollow`: `Head`, `Lazy`,
`Fixed`). A game that wants a UI somewhere precise puts a `SurfaceGui` on a
part instead, which already lives in the world. Flat, nothing changes.

### 2. Pointed at with a laser, or poked

A controller or a tracked hand points a ray from its pose (ADR 0245); the ray
is drawn as a short beam ending in a dot where it meets a panel or a
`SurfaceGui`, and the trigger (or a pinch) is a click there: the UI receives
it as `InputBegan` / `Activated` with the hit's position in its own pixels, as
a mouse's. **A poke**: an index fingertip that passes into a panel within 2 cm
of its surface presses there, and leaving it releases -- for buttons in reach.
Which hand points, whether the beam is drawn, and its length are
`XRService.PointerHand`, `PointerVisible` and `PointerLength`. A game's own
pointing goes through `XRService:GetPointerRay(hand)`.

### 3. Comfort: `@engine/xrcomfort`

A Luau module, required by a game that wants it, on `XRService` and the
camera -- the engine's core gains nothing for it:

```luau
--!strict
local comfort = require("@engine/xrcomfort")
comfort.start({
    move = "teleport",          -- "teleport" | "smooth"
    turn = "snap", snapDegrees = 30,  -- "snap" | "smooth"
    vignette = 0.6,             -- how much of the view the vignette closes while moving smoothly; 0 none
    seated = false,
    heightOffset = 0,           -- metres added to the player's height; `comfort.calibrateHeight()` sets it
})
```

- **Teleport**: the stick pushed forward draws an arc from the hand, which
  lands on what `NavigationService` (or any walkable surface, when there is no
  mesh) says the player may stand on; released, the view fades for a tenth of
  a second and the play space moves there.
- **Snap turn**: the stick to a side turns the play space by `snapDegrees`
  about the head, in one frame.
- **Smooth move and turn**, with a **vignette** that darkens the edges of the
  view in proportion to how fast the view moves while the head did not.
- **Seated** puts the play space so the head is at a standing player's height,
  and **height calibration** sets the offset from the floor the runtime
  reports.

The settings are a player's, so the module saves them through `SaveService`
(ADR 0111) when a game lets the player change them.

## Proof

- The simulated headset points at a button on a panel and pulls the
  trigger: the button's `Activated` fires once, at the right pixel. A
  fingertip moved through a panel presses and releases it.
- A `ScreenGui` with a `TextLabel` is legible on the panel: its text is
  rendered at the panel's resolution (a golden picture from the simulated
  headset).
- Teleport onto a navigation mesh lands on it, never on a wall; a snap turn
  of 30 degrees turns the view by 30 about the head; the vignette is closed
  while moving and open at rest.
- Flat, the same UI is the screen's.

## Consequences

- New API: `ScreenGui.XRPixelsPerMetre`, `XRDistance`, `XRFollow`;
  `XRService.PointerHand`, `PointerVisible`, `PointerLength`,
  `GetPointerRay`; `@engine/xrcomfort`.
- A game's existing UI works in a headset without being rewritten, and is
  read at arm's length rather than at a screen's distance -- some layouts
  will want larger text, which the panel's resolution makes visible rather
  than blurry.

## Not decided here

A virtual keyboard (the runtime's own, `XR_FB_keyboard` on the Quest, is a
later step), and UI drawn in front of the eyes deliberately (subtitles, a
HUD): a game draws those on a `SurfaceGui` parented to the camera.

## As built, first step (2026-10-18)

- **Each `ScreenGui` is a panel** (§1) while a headset draws: the screen's
  own layout, drawing and pointer leave every screen alone then
  (`ui::screenLive`), and each one this machine shows is laid out against
  a canvas of its own -- 1280 by 720, or `ReferenceHeight` tall at 16:9 --
  and drawn as one more canvas in the world, beside `SurfaceGui` and
  `BillboardGui`, over the world. The host places it each frame from the
  head `XRService` reads, by its `XRFollow`; when the headset stops, the
  screens are laid out for the screen again.
- Proved under the simulated headset with a GPU (`xr_simulated_panel`): a
  label half the canvas wide and three tenths tall is 889 by 300 pixels,
  and is drawn in each eye at the size and height its metres give.
- **Not yet**: the laser, the poke and `@engine/xrcomfort` (§2, §3).

## As built, second step (2026-10-18)

- **The laser** (§2): the pointing hand's aim pose, placed in the world by
  the camera, is a ray that finds the panels and every `SurfaceGui` and
  `BillboardGui` the mouse's ray finds (`pickWorldUiRay`, of which the
  mouse's pick is now one case), out to `PointerLength`; its trigger, or its
  pinch, is the press and the release. While a headset draws, the window's
  own pointer is a spectator's: it neither hovers nor presses. The beam is
  drawn over everything, from the hand to the hit, with a round dot there.
- `GetPointerRay` gives a game the same ray; `SimulateInput` holds one of
  the simulated headset's controls, so a test pulls a trigger.
- Proved under the simulated headset with a GPU (`xr_simulated_panel`): a
  hand held low and to the right points at a button on the panel and pulls
  the trigger, and the button's `Activated` fires once.
- **The poke**: each tracked hand's index fingertip casts a ray along the
  finger from two centimetres behind the tip to two past it, the same pick
  as the laser's; what it meets is pressed, and when the finger leaves it is
  released there -- so a button pushed through and drawn back is activated.
  A poke takes the press from the laser while a finger touches.
  `SimulatePose` puts a simulated fingertip (`Enum.XRPose.LeftIndexTip`,
  `RightIndexTip`), which puts that hand's controller down; in
  `xr_simulated_panel` a finger pushed a centimetre through the button and
  out again activates it once more.
- **Not yet**: `@engine/xrcomfort` (§3).
