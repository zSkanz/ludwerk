# 0189 - Input preference and physical gamepad family

- Status: accepted
- Date: 2026-10-09
- Decided by: the owner, InputService extension requirements 11 through 15.

## Decision

Extend InputService and the existing SDL3 event path. PreferredInput aliases
LastInputDeviceType and retains Enum.InputDeviceType (KeyboardMouse, Gamepad,
Touch). Reuse InputDeviceChanged rather than duplicating its category signal.

PreferredGamepadType uses the engine's own Enum.GamepadType: Unknown, Xbox,
PlayStation, Nintendo, Generic. PreferredGamepadId identifies the last used
controller, with zero meaning none. Neither value describes the operating
system. SDL's real physical type is translated inside platform; public scripts
never depend on SDL enum values. Physical family does not prescribe button
layout or a glyph set, so remapping and future glyph providers remain possible.

Connection does not select a controller. Button presses and axes beyond the
existing preference threshold select it; idle drift and zero mouse delta do
not. Mouse activity and keyboard input select KeyboardMouse while retaining
controller metadata. Unplugging the preferred controller clears its ID/family,
without inventing use of another device. Other controllers retain their held
state. The existing aggregate action stream combines held buttons and selects
the strongest absolute axis; this does not add player assignment semantics.

Type and ID changes have separate signals. Connection signals include both ID
and family, including cached family on removal. Preference changes are emitted
on Simulation dispatch and use the existing deferred script signal machinery.

PreferredInput is a presentation alias: the existing LastInputDeviceType already
includes the input category in the deterministic world hash. Physical gamepad
family and connection ID are host facts and do not enter that hash or saved
scenes. This keeps controller hardware and duplicate alias data from changing
recorded worlds. Gameplay changes made by scripts consuming those properties
remain covered by the normal world hash.

## Verification

Exercise connection without preference, actual use, keyboard/mouse transitions,
unknown devices, same-family controller changes, idle dispatch, multiple-pad
removal, focus loss and deferred Luau delivery. Hardware-specific identification
still requires real controllers; synthetic events verify the engine contract.
