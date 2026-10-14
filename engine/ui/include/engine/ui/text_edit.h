// What a text field does to its text (ADR 0139): a caret and an anchor, the
// moves, the deletes, the clipboard's three verbs, undo -- as pure functions
// over a string, so every one of them is a case in a test and none needs a
// window, a font or a keyboard.
//
// **Offsets are bytes into UTF-8**, as a Luau string's are, and every one this
// module hands back is on a CHARACTER boundary: a character as a reader sees
// it, a base with its combining marks, an emoji joined to the next, a flag's
// two letters. A caret never lands inside one.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::ui {

// **The field's editing state**: its text, the caret and the selection's other
// end (equal when nothing is selected), and what undo can put back.
struct TextEditState
{
    std::string text;
    core::u32 caret = 0;
    core::u32 anchor = 0;

    struct Snapshot
    {
        std::string text;
        core::u32 caret = 0;
        core::u32 anchor = 0;
    };
    std::vector<Snapshot> undo;
    std::vector<Snapshot> redo;

    // What the last edit was, so a run of typing is one undo step.
    enum class Last : core::u8
    {
        Nothing,
        Typing,
        Deleting,
        Other,
    };
    Last last = Last::Nothing;
};

// What the field allows (ADR 0139 §2).
struct TextEditRules
{
    // The most characters the text may hold; 0 is no limit.
    core::u32 maxLength = 0;
    // A new line is text; otherwise a pasted one is flattened to a space.
    bool multiLine = false;
    // Masked: copying and cutting give nothing away.
    bool masked = false;
    // Read-only: nothing changes the text, and moving and selecting still work.
    bool editable = true;
};

// What an edit did.
struct TextEditResult
{
    bool changed = false;
    // What `maxLength` kept out, for `InputRejected`.
    std::string rejected;
};

enum class TextMove : core::u8
{
    Left,
    Right,
    WordLeft,
    WordRight,
    LineStart,
    LineEnd,
    TextStart,
    TextEnd,
};

// --- Characters and words ------------------------------------------------------

// The boundary after / before `at` -- one character as a reader sees it.
[[nodiscard]] core::u32 nextCharacter(std::string_view text, core::u32 at) noexcept;
[[nodiscard]] core::u32 previousCharacter(std::string_view text, core::u32 at) noexcept;
// The nearest boundary at or before `at`: what a byte offset from outside --
// a script, a hit test -- is snapped to.
[[nodiscard]] core::u32 characterBoundary(std::string_view text, core::u32 at) noexcept;
// How many characters `text` holds.
[[nodiscard]] core::u32 characterCount(std::string_view text) noexcept;

// Where Ctrl+Right lands from `at`: past the word (or the run of punctuation)
// it is in or before, and the spaces after it. Ctrl+Left mirrors it.
[[nodiscard]] core::u32 nextWord(std::string_view text, core::u32 at) noexcept;
[[nodiscard]] core::u32 previousWord(std::string_view text, core::u32 at) noexcept;
// The word around `at`, for a double press: [start, end).
[[nodiscard]] std::pair<core::u32, core::u32> wordAt(std::string_view text, core::u32 at) noexcept;
// The line around `at` in a multi-line text, for a triple press.
[[nodiscard]] std::pair<core::u32, core::u32> lineAt(std::string_view text, core::u32 at) noexcept;

// --- The state -------------------------------------------------------------------

[[nodiscard]] bool hasSelection(const TextEditState& state) noexcept;
[[nodiscard]] std::string_view selectedText(const TextEditState& state) noexcept;

// Replaces everything, as a script's write to `Text` does: the caret goes to
// the end and the history is cleared, since what it would undo to is not the
// player's.
void setText(TextEditState& state, std::string_view text);

// Moves the caret; `extend` keeps the anchor where it is (Shift). A move
// without `extend` over a selection collapses it to the side it moved toward.
void move(TextEditState& state, TextMove how, bool extend);
// Puts the caret at `at` (snapped to a boundary), extending or not.
void setCaret(TextEditState& state, core::u32 at, bool extend);
void select(TextEditState& state, core::u32 from, core::u32 to);
void selectAll(TextEditState& state);

// Inserts at the caret, replacing the selection: typing, a paste, a new line.
// `typing` is what coalesces into one undo step.
TextEditResult insert(TextEditState& state, std::string_view added, const TextEditRules& rules, bool typing = true);
// Backspace and Delete; `word` for Ctrl. Over a selection, delete it.
TextEditResult deleteBackward(TextEditState& state, bool word, const TextEditRules& rules);
TextEditResult deleteForward(TextEditState& state, bool word, const TextEditRules& rules);

// The clipboard's verbs: what to put on it (empty when masked or nothing is
// selected), and for `cut` the deletion.
[[nodiscard]] std::string copy(const TextEditState& state, const TextEditRules& rules);
TextEditResult cut(TextEditState& state, const TextEditRules& rules, std::string& taken);

bool undo(TextEditState& state);
bool redo(TextEditState& state);

} // namespace engine::ui
