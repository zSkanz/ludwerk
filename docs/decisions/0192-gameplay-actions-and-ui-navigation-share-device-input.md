# 0192 - Gameplay actions and UI navigation share device input

- Status: accepted and implemented
- Date: 2026-10-09
- Builds on: ADR 0039 (Input Actions), ADR 0128 (UI navigation), ADR 0189 (input preference)

## Decision

Games bind gameplay through InputContext/InputAction/InputBinding. Physical
controller positions, keyboard composites and virtual accessibility channels
use the existing resolver. Operating system and controller family do not choose
which gameplay actions exist. Presentation reads PreferredInput separately;
phone layout remains appropriate for its screen when a controller is used.

UIService remains the sole automatic navigator. Automatic nearest-neighbour
selection uses the highest enabled ScreenGui display layer that contains shown,
selectable objects. Screens at equal DisplayOrder share that layer; decorative
screens and hidden controls do not trap focus. Explicit NextSelection links and
SelectedObject remain the game's deliberate overrides. Opening a modal clears
obsolete selection; gameplay disables AutoSelect and clears SelectedObject so
left-stick movement and the lower face button cannot navigate/activate the HUD.

The game supplies its SelectionImageObject, with ownership under its button,
when it needs a focus outline matching its visual style. It does not implement
another input navigator or translate controller buttons into keyboard events.

## Verification and limits

Windows and Linux native tests cover modal selection, equal-order screens,
disabled/hidden/decorative overlays and action release on disconnect/focus loss.
Hordewake's integration test drives the same action resolver through explicitly
virtual channels and checks movement, camera, pause/resume and scoped cleanup.
This is separate from physical controller validation. Game input support does
not make an unsupported console runtime exportable; the full UWP player remains
a separate platform/RHI port.
