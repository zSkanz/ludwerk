#pragma once

// **Where a line may end in text with no spaces between its words** (ADR
// 0260): Thai and Lao write a sentence as one run of letters, and a line may
// end only between two words of it. Which those are is found in a word list --
// ICU's, under the Unicode licence -- that the engine carries as content, and
// that only a game with text in one of those scripts needs.

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::ui::detail {

// Whether `c` is a letter or mark of a script whose words are found in a list.
[[nodiscard]] bool dictionaryScript(core::u32 c) noexcept;

// The byte offsets in `text` where a word of such a script begins inside a
// run of them -- never a run's own start, which the other rules decide --
// appended to `out` in order. Nothing for a script whose list is not there
// to read: its runs break only where the other rules allow.
void dictionaryBreaks(std::string_view text, std::vector<core::usize>& out);

// A test's own word list for the script `letter` is in, in place of the one
// read from content; an empty list puts the content's back.
void setWordListForTests(char32_t letter, std::vector<std::u32string> words);

} // namespace engine::ui::detail
