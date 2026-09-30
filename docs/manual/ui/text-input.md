# Text input

A `TextInput` is the field a game types into: a chat box, a player's name, a
server's address, a note. It edits the way every text field on the machine
does, so a player's hands already know it (ADR 0139).

```luau
--!strict
local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local chat = Instance.new("TextInput")
chat.Size = UDim2.new(0, 400, 0, 32)
chat.Position = UDim2.new(0, 16, 1, -48)
chat.PlaceholderText = "Say something"
chat.MaxLength = 200
-- A chat box keeps the keyboard after sending, for the next line.
chat.ReleaseFocusOnSubmit = false
chat.Parent = screen

chat.Submitted:Connect(function(text: string)
    if text ~= "" then
        print(`sent: {text}`)
        chat.Text = ""
    end
end)
```

## What the keys and the mouse do

| Keys | What happens |
|---|---|
| Left, Right | One character -- as a reader sees it, so an accented letter or an emoji is one |
| Ctrl+Left, Ctrl+Right | One word, or one run of punctuation |
| Home, End | The start or end of the line; with Ctrl, of the text |
| Up, Down | The line above or below in a multi-line field; the ends in a single-line one |
| Shift with any of these | Extends the selection |
| Backspace, Delete | One character; with Ctrl, one word; over a selection, the selection |
| Ctrl+A | Selects everything |
| Ctrl+C, Ctrl+X, Ctrl+V | Copy, cut and paste, through the system's clipboard |
| Ctrl+Insert, Shift+Insert, Shift+Delete | Copy, paste and cut, the older way |
| Ctrl+Z; Ctrl+Y or Ctrl+Shift+Z | Undo and redo -- a run of typing is one step |
| Return | Submits; in a multi-line field, a new line, and Ctrl+Return submits |
| Escape | Lets go of the keyboard |
| Tab, Shift+Tab | The next or the previous field on the same screen |

On a Mac, Cmd is the modifier where these say Ctrl, and Option moves and
deletes by words.

A **press** puts the caret under the pointer, and a **drag** selects. Shift with
a press extends the selection, a **double press** selects a word and a **triple
press** the line -- or everything, in a single-line field. Typing, pasting or
deleting replaces what is selected.

While a field has the keyboard, the keys are its: a player typing `w` into the
chat does not walk (ADR 0041).

## Its properties

| Property | What it does |
|---|---|
| `TextInput.PlaceholderText`, `TextInput.PlaceholderColor` | What an empty field shows until it is typed into |
| `TextInput.MultiLine` | Several lines: wrapped at the field's width, scrolled to keep the caret in view |
| `TextInput.Editable` | False makes it read-only: still selectable and copyable -- a code to copy |
| `TextInput.MaxLength` | The most characters it takes; 0 is no limit |
| `TextInput.Masked`, `TextInput.MaskCharacter` | A password: drawn as dots, and copying gives nothing away |
| `TextInput.ClearTextOnFocus` | Focusing empties it |
| `TextInput.SelectAllOnFocus` | Focusing selects everything, so typing replaces it |
| `TextInput.ReleaseFocusOnSubmit` | False keeps the keyboard after Return |
| `TextInput.RevertOnEscape` | Escape puts back the text it had when focused |
| `TextInput.KeyboardType` | The keyboard a phone raises: numbers, an address, a phone number |
| `TextInput.CursorPosition`, `TextInput.SelectionStart` | The caret and the selection, readable and writable |

`CursorPosition` and `SelectionStart` count as a Luau string does: from 1, in
bytes, so `string.sub(field.Text, 1, field.CursorPosition - 1)` is the text
before the caret. Both are -1 while the field is not focused.

A script's write to `Text` puts the caret at the end and clears the field's undo
history: what it would undo to is not the player's.

## Its events

| Event | When |
|---|---|
| `TextInput.Focused` | It took the keyboard |
| `TextInput.FocusLost` | It let go, with `submitted` and a `Enum.FocusLossReason`: `Submitted`, `Moved` (a press elsewhere, Tab), `Cancelled` (Escape) or `Script` |
| `TextInput.Submitted` | Return was pressed, with the text -- whether or not it lets go |
| `TextInput.TextChanged` | The player changed the text: typing, a paste, a cut, an undo |
| `TextInput.InputRejected` | `MaxLength` kept something out, with what it was |

`TextChanged` is the player's edits only. A script's own write to `Text` fires
`GetPropertyChangedSignal("Text")`, which fires for both.

```luau
--!strict
local name = Instance.new("TextInput")
name.MaxLength = 16
name.InputRejected:Connect(function(rejected: string)
    print(`a name is at most 16 characters; "{rejected}" did not fit`)
end)
name.FocusLost:Connect(function(submitted: boolean, reason: EnumItem)
    if reason == Enum.FocusLossReason.Cancelled then
        return -- Escape: the player changed their mind
    end
    print(`name: {name.Text}`)
end)
```

## Focus from code

`TextInput:CaptureFocus` gives a field the keyboard, `TextInput:ReleaseFocus`
takes it away, and `TextInput:IsFocused` says whether it has it. Both take
effect at the next frame. `UIService:GetFocusedTextInput` answers which field
has the keyboard, and `UIService.TextInputFocused` and
`UIService.TextInputFocusReleased` say when that changes -- which is what a
chat hotkey reads:

```luau
--!strict
local InputService = game:GetService("InputService")
local UIService = game:GetService("UIService")

InputService.InputBegan:Connect(function(input: InputObject)
    -- Slash opens the chat, unless a field is being typed into already.
    if input.KeyCode == Enum.KeyCode.Slash and UIService:GetFocusedTextInput() == nil then
        chat:CaptureFocus()
    end
end)
```

## On a phone, and in other languages

Focusing a field raises the phone's keyboard, of the `KeyboardType` it asks
for; a masked field raises a password keyboard. The keyboard's return key is
Return.

Chinese, Japanese and Korean are typed through an **input method**, which
builds a character from several keys. What it is still composing is drawn at
the caret, underlined, and becomes text only when it is committed; its list of
candidates opens beside the caret. While it composes, Return, Escape and
Backspace are the input method's, not the field's.

## Where to look next

- [Buttons and interaction](manual:ui/interaction)
- [Text and images](manual:ui/text-and-images)
- [`TextInput`](api:TextInput)
