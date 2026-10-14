// What a tooltip shows of a description from the API definitions.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace engine::app {

// The first paragraph of `doc`, as plain text. A description is written for the
// reference, where a name stands between backticks and the detail follows in
// further paragraphs; a tooltip draws characters, so the marks are taken out
// and the paragraphs after the first are left to the reference. A line break
// inside the paragraph becomes a space.
[[nodiscard]] std::string docSummary(std::string_view doc);

// `text` with a line break in place of a space wherever the next word would
// pass `width`, as `measure` sizes a run of it. **Only at a space**: the
// toolkit's own wrap also breaks after a full stop, which cuts `0.25` and
// `.irradiance` in two and leaves a reader a sentence that ends early. A word
// wider than the line stands on a line of its own, and a line break `text`
// already has is kept.
[[nodiscard]] std::string wrapAtSpaces(std::string_view text, float width,
                                       const std::function<float(std::string_view)>& measure);

} // namespace engine::app
