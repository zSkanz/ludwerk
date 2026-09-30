// The text field's editing model (ADR 0139), case by case.

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ui/text_edit.h"

using namespace engine;
using ui::TextEditRules;
using ui::TextEditState;
using ui::TextMove;

namespace {

[[nodiscard]] TextEditState typed(std::string_view text)
{
    TextEditState state;
    ui::setText(state, text);
    return state;
}

// The boundaries `nextCharacter` walks through from the start.
[[nodiscard]] std::vector<core::u32> boundaries(std::string_view text)
{
    std::vector<core::u32> out{0};
    for (core::u32 at = 0; at < text.size();) {
        const core::u32 next = ui::nextCharacter(text, at);
        REQUIRE(next > at);
        out.push_back(next);
        at = next;
    }
    return out;
}

} // namespace

TEST_CASE("a character is what a reader sees, not a byte and not a code point")
{
    // ASCII, a precomposed é, and an e with a combining acute.
    CHECK(boundaries("ab") == std::vector<core::u32>{0, 1, 2});
    CHECK(boundaries("\xC3\xA9") == std::vector<core::u32>{0, 2});
    CHECK(boundaries("e\xCC\x81x") == std::vector<core::u32>{0, 3, 4});
    // A family joined by zero-width joiners is one character.
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    CHECK(boundaries(family) == std::vector<core::u32>{0, static_cast<core::u32>(family.size())});
    // A flag is two regional indicators; two flags are two characters.
    const std::string flags = "\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7\xF0\x9F\x87\xB5\xF0\x9F\x87\xB9";
    CHECK(boundaries(flags) == std::vector<core::u32>{0, 8, 16});
    // A skin tone and a variation selector belong to what they follow.
    CHECK(boundaries("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD!") == std::vector<core::u32>{0, 8, 9});
    CHECK(boundaries("\xE2\x9D\xA4\xEF\xB8\x8F") == std::vector<core::u32>{0, 6});
    // CR LF is one line break.
    CHECK(boundaries("a\r\nb") == std::vector<core::u32>{0, 1, 3, 4});

    // Backwards lands on the same boundaries, and a byte in the middle of one
    // snaps to its start.
    CHECK(ui::previousCharacter("e\xCC\x81x", 3) == 0);
    CHECK(ui::characterBoundary("e\xCC\x81x", 2) == 0);
    CHECK(ui::characterBoundary(family, 5) == 0);
    CHECK(ui::characterCount("e\xCC\x81x\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7") == 3);
}

TEST_CASE("Ctrl+arrows move by words, runs of punctuation and the spaces after them")
{
    const std::string_view text = "hello, world  foo";
    CHECK(ui::nextWord(text, 0) == 5);   // past "hello"
    CHECK(ui::nextWord(text, 5) == 7);   // past ", "
    CHECK(ui::nextWord(text, 7) == 14);  // past "world  "
    CHECK(ui::nextWord(text, 14) == 17); // past "foo"
    CHECK(ui::previousWord(text, 17) == 14);
    CHECK(ui::previousWord(text, 14) == 7);
    CHECK(ui::previousWord(text, 7) == 5);
    CHECK(ui::previousWord(text, 5) == 0);

    CHECK(ui::wordAt(text, 9) == std::pair<core::u32, core::u32>{7, 12});
    CHECK(ui::lineAt("one\ntwo\nthree", 5) == std::pair<core::u32, core::u32>{4, 7});
}

TEST_CASE("Shift extends a selection, and a plain move collapses it to the side it went")
{
    TextEditState state = typed("hello world");
    ui::move(state, TextMove::TextStart, false);
    ui::move(state, TextMove::WordRight, true);
    CHECK(ui::selectedText(state) == "hello ");
    ui::move(state, TextMove::Left, false);
    CHECK_FALSE(ui::hasSelection(state));
    CHECK(state.caret == 0);

    ui::selectAll(state);
    CHECK(ui::selectedText(state) == "hello world");
    ui::move(state, TextMove::Right, false);
    CHECK(state.caret == 11);
}

TEST_CASE("typing replaces the selection, and what MaxLength keeps out is rejected")
{
    TextEditState state = typed("hello world");
    ui::select(state, 0, 5);
    TextEditRules rules;
    CHECK(ui::insert(state, "bye", rules).changed);
    CHECK(state.text == "bye world");
    CHECK(state.caret == 3);

    TextEditState limited = typed("abc");
    rules.maxLength = 5;
    const ui::TextEditResult result = ui::insert(limited,
                                                 "d\xC3\xA9"
                                                 "fgh",
                                                 rules);
    CHECK(limited.text == "abcd\xC3\xA9");
    CHECK(result.rejected == "fgh");
}

TEST_CASE("a single-line field flattens what is pasted into it")
{
    TextEditState state = typed("");
    TextEditRules rules;
    (void)ui::insert(state, "one\r\ntwo\nthree", rules, false);
    CHECK(state.text == "one two three");
    rules.multiLine = true;
    TextEditState lines = typed("");
    (void)ui::insert(lines, "one\ntwo", rules, false);
    CHECK(lines.text == "one\ntwo");
}

TEST_CASE("the deletes, by character and by word, and over a selection")
{
    TextEditRules rules;
    TextEditState state = typed("hello world");
    (void)ui::deleteBackward(state, true, rules);
    CHECK(state.text == "hello ");
    (void)ui::deleteBackward(state, false, rules);
    CHECK(state.text == "hello");
    ui::move(state, TextMove::TextStart, false);
    (void)ui::deleteForward(state, true, rules);
    CHECK(state.text.empty());

    TextEditState marks = typed("ae\xCC\x81");
    (void)ui::deleteBackward(marks, false, rules);
    CHECK(marks.text == "a");
}

TEST_CASE("a masked field gives nothing away, and a read-only one keeps its text")
{
    TextEditState state = typed("secret");
    ui::selectAll(state);
    TextEditRules rules;
    CHECK(ui::copy(state, rules) == "secret");
    rules.masked = true;
    CHECK(ui::copy(state, rules).empty());
    std::string taken;
    CHECK_FALSE(ui::cut(state, rules, taken).changed);
    CHECK(taken.empty());
    CHECK(state.text == "secret");

    rules = {};
    rules.editable = false;
    CHECK_FALSE(ui::insert(state, "x", rules).changed);
    CHECK_FALSE(ui::deleteBackward(state, false, rules).changed);
    CHECK(ui::copy(state, rules) == "secret");
}

TEST_CASE("undo takes back a run of typing at once, and redo puts it back")
{
    TextEditRules rules;
    TextEditState state = typed("");
    (void)ui::insert(state, "a", rules);
    (void)ui::insert(state, "b", rules);
    (void)ui::insert(state, "c", rules);
    ui::move(state, TextMove::Left, false);
    (void)ui::insert(state, "X", rules);
    CHECK(state.text == "abXc");

    CHECK(ui::undo(state));
    CHECK(state.text == "abc");
    CHECK(ui::undo(state));
    CHECK(state.text.empty());
    CHECK_FALSE(ui::undo(state));
    CHECK(ui::redo(state));
    CHECK(state.text == "abc");

    // A script's write is not the player's to undo.
    ui::setText(state, "set");
    CHECK_FALSE(ui::undo(state));
    CHECK(state.caret == 3);
}
