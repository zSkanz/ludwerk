#include "engine/ui/text_edit.h"

#include <algorithm>
#include <utility>

namespace engine::ui {

namespace {

using core::u32;

// A code point and how many bytes it took; U+FFFD for a malformed byte, which
// then counts as one character of its own rather than swallowing its
// neighbours.
struct Decoded
{
    char32_t point = 0;
    u32 size = 1;
};

[[nodiscard]] Decoded decodeAt(std::string_view text, u32 at) noexcept
{
    const auto byte = [&](u32 i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80)
        return {lead, 1};
    u32 size = 0;
    char32_t point = 0;
    if ((lead & 0xE0) == 0xC0) {
        size = 2;
        point = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0) {
        size = 3;
        point = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0) {
        size = 4;
        point = lead & 0x07;
    }
    else {
        return {0xFFFD, 1};
    }
    if (at + size > text.size())
        return {0xFFFD, 1};
    for (u32 i = 1; i < size; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80)
            return {0xFFFD, 1};
        point = (point << 6) | (byte(at + i) & 0x3F);
    }
    return {point, size};
}

// The start of the code point that ends just before `at`.
[[nodiscard]] u32 previousPoint(std::string_view text, u32 at) noexcept
{
    if (at == 0)
        return 0;
    u32 back = at - 1;
    while (back > 0 && (static_cast<unsigned char>(text[back]) & 0xC0) == 0x80 && at - back < 4)
        --back;
    return back;
}

// **What attaches to the character before it** rather than starting one:
// combining marks, the variation selectors, a skin tone, the zero-width
// joiner and the tag characters of a subdivision flag. Ranges rather than the
// full property tables, which the engine does not carry; these are the ones a
// player types.
[[nodiscard]] bool extends(char32_t c) noexcept
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD) ||
           (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || (c >= 0x0900 && c <= 0x0903) ||
           (c >= 0x093A && c <= 0x094F) || (c >= 0x0E31 && c <= 0x0E3A && c != 0x0E32 && c != 0x0E33) ||
           (c >= 0x0E47 && c <= 0x0E4E) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0x302A && c <= 0x302F) || (c >= 0x3099 && c <= 0x309A) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           c == 0x200D || (c >= 0xE0020 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF);
}

[[nodiscard]] bool regionalIndicator(char32_t c) noexcept
{
    return c >= 0x1F1E6 && c <= 0x1F1FF;
}

// Where a word, a run of punctuation and a run of spaces each end.
enum class Class : core::u8
{
    Space,
    Word,
    Punctuation,
};

[[nodiscard]] Class classOf(char32_t c) noexcept
{
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0 || c == 0x3000)
        return Class::Space;
    if (c < 0x80) {
        const bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        return word ? Class::Word : Class::Punctuation;
    }
    // General punctuation and the CJK and full-width marks; every other
    // non-ASCII character is a letter to a word move.
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3001 && c <= 0x3003) || (c >= 0xFF01 && c <= 0xFF0F) || c == 0x00BF ||
        c == 0x00A1)
        return Class::Punctuation;
    return Class::Word;
}

[[nodiscard]] Class classAt(std::string_view text, u32 at) noexcept
{
    return classOf(decodeAt(text, at).point);
}

// The first byte of the character that `at` (a boundary) ends.
[[nodiscard]] u32 characterBefore(std::string_view text, u32 at) noexcept
{
    return previousCharacter(text, at);
}

void remember(TextEditState& state)
{
    constexpr std::size_t MaxUndo = 200;
    state.undo.push_back(TextEditState::Snapshot{state.text, state.caret, state.anchor});
    if (state.undo.size() > MaxUndo)
        state.undo.erase(state.undo.begin());
    state.redo.clear();
}

// Records an undo step unless this edit continues a run of the same kind.
void beginEdit(TextEditState& state, TextEditState::Last kind)
{
    const bool continues = kind != TextEditState::Last::Other && state.last == kind && !hasSelection(state);
    if (!continues)
        remember(state);
    state.last = kind;
}

void replaceSelection(TextEditState& state, std::string_view with)
{
    const u32 from = std::min(state.caret, state.anchor);
    const u32 to = std::max(state.caret, state.anchor);
    state.text.replace(from, to - from, with);
    state.caret = state.anchor = from + static_cast<u32>(with.size());
}

} // namespace

u32 nextCharacter(std::string_view text, u32 at) noexcept
{
    const u32 size = static_cast<u32>(text.size());
    if (at >= size)
        return size;
    const Decoded first = decodeAt(text, at);
    u32 next = at + first.size;
    // CR LF is one break.
    if (first.point == '\r' && next < size && text[next] == '\n')
        return next + 1;
    // A flag is a PAIR of regional indicators.
    if (regionalIndicator(first.point) && next < size) {
        const Decoded second = decodeAt(text, next);
        if (regionalIndicator(second.point))
            next += second.size;
    }
    while (next < size) {
        const Decoded following = decodeAt(text, next);
        if (following.point == 0x200D) {
            // A joiner takes the character after it too.
            next += following.size;
            if (next < size)
                next += decodeAt(text, next).size;
            continue;
        }
        if (!extends(following.point))
            break;
        next += following.size;
    }
    return next;
}

u32 previousCharacter(std::string_view text, u32 at) noexcept
{
    if (at == 0)
        return 0;
    // Walked forward from a start that cannot be inside a character -- the
    // text's own, or a line break's -- which is the one way to agree with
    // `nextCharacter` about joiners and flag pairs.
    u32 start = 0;
    for (u32 scan = at; scan > 0;) {
        scan = previousPoint(text, scan);
        if (text[scan] == '\n') {
            start = scan;
            break;
        }
    }
    u32 last = start;
    for (u32 walk = start; walk < at;) {
        last = walk;
        walk = nextCharacter(text, walk);
    }
    return last;
}

u32 characterBoundary(std::string_view text, u32 at) noexcept
{
    at = std::min(at, static_cast<u32>(text.size()));
    if (at == text.size())
        return at;
    u32 walk = previousCharacter(text, at);
    const u32 next = nextCharacter(text, walk);
    return next == at ? at : walk;
}

u32 characterCount(std::string_view text) noexcept
{
    u32 count = 0;
    for (u32 at = 0; at < text.size(); at = nextCharacter(text, at))
        ++count;
    return count;
}

u32 nextWord(std::string_view text, u32 at) noexcept
{
    const u32 size = static_cast<u32>(text.size());
    if (at >= size)
        return size;
    const Class run = classAt(text, at);
    if (run != Class::Space) {
        while (at < size && classAt(text, at) == run)
            at = nextCharacter(text, at);
    }
    while (at < size && classAt(text, at) == Class::Space)
        at = nextCharacter(text, at);
    return at;
}

u32 previousWord(std::string_view text, u32 at) noexcept
{
    while (at > 0 && classAt(text, characterBefore(text, at)) == Class::Space)
        at = characterBefore(text, at);
    if (at == 0)
        return 0;
    const Class run = classAt(text, characterBefore(text, at));
    while (at > 0 && classAt(text, characterBefore(text, at)) == run)
        at = characterBefore(text, at);
    return at;
}

std::pair<u32, u32> wordAt(std::string_view text, u32 at) noexcept
{
    const u32 size = static_cast<u32>(text.size());
    if (size == 0)
        return {0, 0};
    at = characterBoundary(text, std::min(at, size - 1));
    const Class run = classAt(text, at);
    u32 from = at;
    while (from > 0 && classAt(text, characterBefore(text, from)) == run)
        from = characterBefore(text, from);
    u32 to = at;
    while (to < size && classAt(text, to) == run)
        to = nextCharacter(text, to);
    return {from, to};
}

std::pair<u32, u32> lineAt(std::string_view text, u32 at) noexcept
{
    at = std::min(at, static_cast<u32>(text.size()));
    const std::size_t before = at == 0 ? std::string_view::npos : text.rfind('\n', at - 1);
    const u32 from = before == std::string_view::npos ? 0 : static_cast<u32>(before + 1);
    const std::size_t after = text.find('\n', at);
    const u32 to = after == std::string_view::npos ? static_cast<u32>(text.size()) : static_cast<u32>(after);
    return {from, to};
}

bool hasSelection(const TextEditState& state) noexcept
{
    return state.caret != state.anchor;
}

std::string_view selectedText(const TextEditState& state) noexcept
{
    const u32 from = std::min(state.caret, state.anchor);
    const u32 to = std::max(state.caret, state.anchor);
    return std::string_view(state.text).substr(from, to - from);
}

void setText(TextEditState& state, std::string_view text)
{
    state.text.assign(text);
    state.caret = state.anchor = static_cast<u32>(state.text.size());
    state.undo.clear();
    state.redo.clear();
    state.last = TextEditState::Last::Nothing;
}

void move(TextEditState& state, TextMove how, bool extend)
{
    const std::string_view text = state.text;
    state.last = TextEditState::Last::Nothing;
    // A plain move over a selection collapses it, to the side it went toward,
    // for the one-step moves; a jump goes where it goes.
    if (!extend && hasSelection(state) && (how == TextMove::Left || how == TextMove::Right)) {
        const u32 to =
            how == TextMove::Left ? std::min(state.caret, state.anchor) : std::max(state.caret, state.anchor);
        state.caret = state.anchor = to;
        return;
    }
    u32 at = state.caret;
    switch (how) {
    case TextMove::Left:
        at = previousCharacter(text, at);
        break;
    case TextMove::Right:
        at = nextCharacter(text, at);
        break;
    case TextMove::WordLeft:
        at = previousWord(text, at);
        break;
    case TextMove::WordRight:
        at = nextWord(text, at);
        break;
    case TextMove::LineStart:
        at = lineAt(text, at).first;
        break;
    case TextMove::LineEnd:
        at = lineAt(text, at).second;
        break;
    case TextMove::TextStart:
        at = 0;
        break;
    case TextMove::TextEnd:
        at = static_cast<u32>(text.size());
        break;
    }
    state.caret = at;
    if (!extend)
        state.anchor = at;
}

void setCaret(TextEditState& state, u32 at, bool extend)
{
    state.caret = characterBoundary(state.text, at);
    if (!extend)
        state.anchor = state.caret;
    state.last = TextEditState::Last::Nothing;
}

void select(TextEditState& state, u32 from, u32 to)
{
    state.anchor = characterBoundary(state.text, from);
    state.caret = characterBoundary(state.text, to);
    state.last = TextEditState::Last::Nothing;
}

void selectAll(TextEditState& state)
{
    state.anchor = 0;
    state.caret = static_cast<u32>(state.text.size());
    state.last = TextEditState::Last::Nothing;
}

TextEditResult insert(TextEditState& state, std::string_view added, const TextEditRules& rules, bool typing)
{
    TextEditResult result;
    if (!rules.editable)
        return result;
    // A single line takes a pasted break as one space; CR LF is one break.
    std::string text;
    text.reserve(added.size());
    for (std::size_t i = 0; i < added.size(); ++i) {
        const char c = added[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < added.size() && added[i + 1] == '\n')
                ++i;
            text += rules.multiLine ? '\n' : ' ';
            continue;
        }
        text += c;
    }
    // What fits: the limit counts the characters left once the selection goes.
    if (rules.maxLength > 0) {
        const u32 kept = characterCount(state.text) - characterCount(selectedText(state));
        const u32 room = kept >= rules.maxLength ? 0 : rules.maxLength - kept;
        u32 fits = 0;
        u32 count = 0;
        while (fits < text.size() && count < room) {
            fits = nextCharacter(text, fits);
            ++count;
        }
        result.rejected = text.substr(fits);
        text.resize(fits);
    }
    if (text.empty() && !hasSelection(state))
        return result;
    beginEdit(state, typing ? TextEditState::Last::Typing : TextEditState::Last::Other);
    replaceSelection(state, text);
    result.changed = true;
    return result;
}

TextEditResult deleteBackward(TextEditState& state, bool word, const TextEditRules& rules)
{
    TextEditResult result;
    if (!rules.editable)
        return result;
    if (!hasSelection(state)) {
        if (state.caret == 0)
            return result;
        // The span to delete is the selection [anchor, caret).
        state.anchor = word ? previousWord(state.text, state.caret) : previousCharacter(state.text, state.caret);
    }
    beginEdit(state, word ? TextEditState::Last::Other : TextEditState::Last::Deleting);
    replaceSelection(state, {});
    result.changed = true;
    return result;
}

TextEditResult deleteForward(TextEditState& state, bool word, const TextEditRules& rules)
{
    TextEditResult result;
    if (!rules.editable)
        return result;
    if (!hasSelection(state)) {
        if (state.caret >= state.text.size())
            return result;
        state.anchor = word ? nextWord(state.text, state.caret) : nextCharacter(state.text, state.caret);
    }
    beginEdit(state, word ? TextEditState::Last::Other : TextEditState::Last::Deleting);
    replaceSelection(state, {});
    result.changed = true;
    return result;
}

std::string copy(const TextEditState& state, const TextEditRules& rules)
{
    if (rules.masked)
        return {};
    return std::string(selectedText(state));
}

TextEditResult cut(TextEditState& state, const TextEditRules& rules, std::string& taken)
{
    TextEditResult result;
    taken.clear();
    if (rules.masked || !rules.editable || !hasSelection(state))
        return result;
    taken = std::string(selectedText(state));
    beginEdit(state, TextEditState::Last::Other);
    replaceSelection(state, {});
    result.changed = true;
    return result;
}

bool undo(TextEditState& state)
{
    if (state.undo.empty())
        return false;
    state.redo.push_back(TextEditState::Snapshot{state.text, state.caret, state.anchor});
    TextEditState::Snapshot back = std::move(state.undo.back());
    state.undo.pop_back();
    state.text = std::move(back.text);
    state.caret = back.caret;
    state.anchor = back.anchor;
    state.last = TextEditState::Last::Nothing;
    return true;
}

bool redo(TextEditState& state)
{
    if (state.redo.empty())
        return false;
    state.undo.push_back(TextEditState::Snapshot{state.text, state.caret, state.anchor});
    TextEditState::Snapshot forward = std::move(state.redo.back());
    state.redo.pop_back();
    state.text = std::move(forward.text);
    state.caret = forward.caret;
    state.anchor = forward.anchor;
    state.last = TextEditState::Last::Nothing;
    return true;
}

} // namespace engine::ui
