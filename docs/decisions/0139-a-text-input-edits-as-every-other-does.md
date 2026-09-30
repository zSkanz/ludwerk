# 0139 — A text input edits as every other text field does

- Status: accepted (to be built; see `docs/briefs/text-input-kickoff.md`)
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29: *"deixar a nossa text input perfeita ...
  Ctrl C, Ctrl A, Ctrl V ... a nossa text input não tem nada disso. E ela precisa
  ter"*, and, the same day, *"pesquisa na internet ... todas as outras engines
  oferecem, a nossa deve ter, deve ter eventos de quando o texto for alterado,
  quando tiver sido focada, quando tiver desfocada"*. A survey of the text
  fields of four other engines and of the web's `<input>` came first; what
  every one of them has is below.
- Builds on: [0041](0041-inputservice-gains-a-raw-event-surface.md) (a focused
  field takes the keyboard), the `TextInput` of E-S6.7 (a caret, the arrows,
  Home, End and both deletes, and nothing else).

## Context

`TextInput` is the one field a game has for a chat box, a name, a server's
address. Today it inserts typed text at a caret and deletes around it. It has
no selection, no clipboard, no undo, no word moves, no mouse placement, no
password mask, no length limit, no second line, no composition for the
languages typed through an input method, and two events -- `Focused` and
`FocusLost(submitted)`. A player who presses Ctrl+A in it gets an `a`.

Every text field anybody uses has all of these, and people notice their absence
at once. The survey found the same set everywhere, with the events grouped the
same way: the text changed, focus gained, focus lost with a reason (submitted,
moved away, cancelled, taken by a script), submitted, input rejected.

## Decision

### 1. Editing: what every field does

- **Selection**, as an anchor and a caret. Shift with any movement extends it;
  typing, pasting or deleting replaces it.
- **Movement**: by character, by word (Ctrl+Left/Right, Option on macOS), to
  the line's ends (Home/End) and to the text's ends (Ctrl+Home/End); Up and
  Down between lines in a multi-line field.
- **Deleting**: Backspace and Delete, and by word with Ctrl (Option on macOS).
- **The clipboard**: Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V (Cmd on macOS), and
  Ctrl+Insert / Shift+Insert, through the operating system's clipboard.
- **Undo and redo**: Ctrl+Z; Ctrl+Y and Ctrl+Shift+Z. Typing coalesces into one
  step until a pause, a move or a different kind of edit.
- **The mouse**: a press places the caret, a drag selects, Shift+press extends,
  a double press selects a word and a triple press the line.
- **Characters, not bytes**: the caret steps over a whole character as the
  reader sees it -- a combining mark, a joined emoji, a flag -- and never lands
  inside one.
- **Input methods**: text composed through an input method (Chinese, Japanese,
  Korean, the system's emoji picker) is drawn at the caret, underlined, while it
  is being composed, and the system is told where the caret is so its candidate
  list opens beside it. Only committed text is an edit. While composing, Return,
  Escape and Backspace are the input method's.

### 2. The properties

| Property | Default | Meaning |
|---|---|---|
| `MultiLine` | false | Return inserts a new line and Ctrl+Return submits; text wraps and scrolls |
| `Editable` | true | False: read-only, still selectable and copyable -- a code to copy |
| `MaxLength` | 0 | The most characters it takes; 0 is no limit. What does not fit is rejected |
| `Masked` | false | Drawn as `MaskCharacter`s; copy and cut are refused, paste is not |
| `MaskCharacter` | `"•"` | What a masked character draws as |
| `ClearTextOnFocus` | false | Focusing empties it |
| `SelectAllOnFocus` | false | Focusing selects all of it |
| `ReleaseFocusOnSubmit` | true | False: Return submits and keeps focus -- a chat box |
| `RevertOnEscape` | false | Escape puts back the text it had when focused |
| `PlaceholderColor` | a dimmed `TextColor` | The placeholder's colour |
| `KeyboardType` | `Default` | The on-screen keyboard to raise: `Default`, `Number`, `Decimal`, `Phone`, `Email`, `Url` |
| `CursorPosition` | -1 | The caret, 1-based in bytes like a Luau string; -1 when not focused. Writable |
| `SelectionStart` | -1 | The selection's other end, or -1 for none. Writable |

### 3. The events and methods

- `Focused()` -- as today.
- `FocusLost(submitted: boolean, reason: Enum.FocusLossReason)` -- `reason` is
  added: `Submitted` (Return), `Moved` (a press elsewhere, Tab), `Cancelled`
  (Escape), `Script` (`ReleaseFocus`, or the field leaving the screen).
  `submitted` stays, for the code already written.
- `Submitted(text: string)` -- Return, whether or not focus is kept.
- `TextChanged(text: string)` -- after each edit the player made: typing, a
  paste, a cut, an undo. A script's write to `Text` is not an edit; it fires
  `GetPropertyChangedSignal("Text")`, as every property does.
- `InputRejected(rejected: string)` -- what `MaxLength` did not let in.
- `CaptureFocus()`, `ReleaseFocus(submitted: boolean?)`, `IsFocused(): boolean`.
- On `UIService`: `GetFocusedTextInput(): TextInput?`, and
  `TextInputFocused(input)` / `TextInputFocusReleased(input)`.

### 4. The keyboard is the field's while it has focus

As ADR 0041 already says for the keys it takes: a focused field consumes the
keyboard, so a player typing `w` does not walk. Tab moves focus to the next
`TextInput` on the same screen, in layout order. On a phone, focusing raises
the on-screen keyboard of `KeyboardType`, and its return key is Return.

### 5. Not decided here

- A right-click menu (cut, copy, paste, select all), character filters beyond
  `MaxLength`, rich text inside a field, right-to-left editing, and a caret or
  selection colour of its own: later, each additive.

## Consequences

- A field behaves as a player's hands expect, on a desktop and on a phone.
- `FocusLost` gains a parameter, which is additive; a handler that takes one
  keeps working.
- The platform layer gains the clipboard, key modifiers, the press count and
  input-method composition, which the editor's own panels do not use: they go
  through ImGui, which has its own.
