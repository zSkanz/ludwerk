# 0170 — A phone's back button is Escape, and the game's to answer

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a finding from play on the owner's phone
  (ludwerk-08), under the standing rule to decide as professional engines do
  and record it
- Builds on: Android (opened 2026-09-26), ADR 0029 (the input action system)

## Context

Nothing said what a game receives when a player presses back on a phone, or
swipes the back gesture. What happened was that the system took it and the
activity finished: the game closed. A phone game answers back all the time --
it closes a sheet, it opens the pause -- and had no way to hear it.

## How mature engines do it

- **Unity**: back is `KeyCode.Escape`. The game decides what it does, and
  quits when it means to.
- **Godot**: a go-back request the game may handle, with a project setting
  for quitting on it; also deliverable as the `ui_cancel` action.
- **Unreal**: a key of its own, `Android_Back`.

All three deliver it to the game and none closes the app under it.

## Decision

1. **Back is delivered, not acted on.** The engine asks the system for the
   button as a key (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`), once, when the
   platform starts.

2. **It is `Enum.KeyCode.Escape`**: pressed and released, like the key. The
   key a desktop game already closes a menu and opens the pause with does the
   same on a phone with no line of the game changed, and an `InputAction`
   bound to Escape hears back. Unity's choice, for Unity's reason.

   Not a key of its own: a game would have to bind both everywhere, and the
   one case that would tell them apart -- a phone with a keyboard attached
   whose Escape should do something else than its back -- is nobody's.

3. **A game leaves by `game:Shutdown()`.** At its first screen, where back
   has nothing to close, that is what a phone's player expects of it; the
   engine does not guess which screen that is.

## Consequences

- A game that answers nothing to Escape no longer closes on back: it does
  nothing. That is what was asked of it.
- Test (`platform_tests.cpp`): the hint is set when the platform starts; a
  back key down and up arrive as Escape down and up, and no quit.
- On the device: ludwerk-08's to confirm with the next APK.
