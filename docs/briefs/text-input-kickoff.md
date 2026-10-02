# The text input made complete: the kickoff and the ledger

**By the owner's order of 2026-09-29**, after the script-sides ledger and
before the terrain audit's: *"deixar a nossa text input perfeita ... Ctrl C,
Ctrl A, Ctrl V ... ela precisa ter ... faça ela funcionar perfeitamente"*, and a
survey of what other engines' fields offer, events included. The decision is
[ADR 0139](../decisions/0139-a-text-input-edits-as-every-other-does.md).

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test first for every behaviour: the editing model is pure and is
  tested as such; the platform's clipboard and composition through the host; a
  conformance spec for each property, event and method a script sees.
- R3: every new text a player or a developer reads is a key.
- R7: no name of another platform in code, keys or comments.
- The full local gate before each push.

## T1 — the platform

- [x] Key events carry the modifiers (Shift, Ctrl, Alt, the system key) and a
  press carries its count (double, triple).
- [x] The clipboard: read and write UTF-8 text.
- [x] Input-method composition: the composed text and its cursor as an event;
  the caret's rectangle handed to the system; the on-screen keyboard raised
  with a type (text, number, decimal, phone, email, URL) and a multi-line hint.

## T2 — the editing model (`engine/ui`)

- [x] A text, a caret and an anchor, in bytes, always on a character boundary.
- [x] Characters as a reader sees them: combining marks, joined emoji, flags.
- [x] Words: letters and digits against punctuation against spaces.
- [x] Every command of ADR 0139 §1, with `MaxLength` and what it rejects,
  single-line paste flattening, and `Masked`'s refusals.
- [x] Undo and redo, typing coalesced.

## T3 — the API

- [x] The properties, events and methods of ADR 0139 §2 and §3, and
  `UIService`'s query and events; `Enum.FocusLossReason` and
  `Enum.TextInputKeyboard` at the end of the enums.

## T4 — interaction

- [x] The host's keys become the model's commands, with the clipboard.
- [x] The mouse: place, drag, extend, double and triple press, against the
  same measurements the drawing uses.
- [x] Focus: clear or select on focus, Escape, Return and Tab, `CaptureFocus`
  and `ReleaseFocus`, the reasons.
- [x] Scrolling to keep the caret in view; lines in a multi-line field.

## T5 — drawing

- [x] The selection behind the text, the caret blinking, the mask, the
  placeholder's colour, the composed text underlined, clipped to the field.

## T6 — the close

- [x] The manual's page on text input; the API reference.
- [x] A game driven by real input: every shortcut, the mouse, a paste
  from another application, a masked field, a multi-line one. Two defects
  found and fixed on the way (D361, D362).
- [>] Android: the on-screen keyboard of each type. **Waiting on the owner's
  phone**: none was connected on 2026-09-30.

## Findings

- **The interaction's state is one per process, not one per world.** The
  focused field lives in a static the interaction keeps between frames, so a
  test that builds a second world found the first one's field still "focused"
  and skipped the second's focus event. The fixture now resets it, and taking
  focus asks the field's own component, not only the static.
- **A press's caret was overwritten by the field's older one.** The press placed
  the caret in the session, and the write-back a few lines later copied the
  component's stale caret over it. The session and the component are now synced
  right after a press or a drag, before any command runs.
- **A centred field puts its text where a test does not expect it.** A
  `TextInput`'s default alignment is centred; the tests that press at a
  character's offset set it to the left first.
- **`Focused`, `FocusLost` and the service's pair are four events, not two.**
  A chat hotkey wants to know whether *any* field has the keyboard, which only
  a query on `UIService` and its two events answer without walking the tree.
- **A paste right after a copy pasted nothing (D361).** Two programs on the
  owner's machine -- a clipboard history and a launcher -- open the clipboard
  about 200 ms after every change. SDL tries for 30 ms and then answers "" as if
  the clipboard were empty. Only a real application and a real clipboard show
  it; the unit test had passed from the start.
- **A click between two frames was lost (D362).** The host read a click from
  the button's state at each frame, so a press and a release inside one frame
  never happened -- a quick tap on a touchpad, a touch, and the second click of
  a quick double click.
- **Automation must send scan codes.** `SendInput` with a virtual key and no
  scan code reaches SDL's raw keyboard as scan code 0: Home and End never
  arrived, and what looked like a lost Ctrl+V after End was the driver. A real
  keyboard always sends one.
