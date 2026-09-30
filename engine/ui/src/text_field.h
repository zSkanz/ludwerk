// **Where a text field's characters are on screen** (ADR 0139): one answer for
// the drawing and the interaction, so a caret, a selection and a press all land
// where the glyphs are.
//
// What is drawn is not always what is held: a masked field draws one mask
// character for each character of its text, and an input method's text being
// composed is drawn at the caret without being in the text yet. The view keeps
// the two sets of offsets side by side.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/ui/ui.h"

namespace engine::scene {
class World;
}

namespace engine::ui {

struct FieldView
{
    // What is drawn.
    std::string display;
    // Each character boundary as (offset in the text, offset in `display`),
    // ascending in both, the end included. A composition's own boundaries map
    // to the caret's text offset.
    std::vector<std::pair<core::u32, core::u32>> boundaries;
    // The composition's span in `display`; empty when nothing is composed.
    core::u32 compositionBegin = 0;
    core::u32 compositionEnd = 0;

    std::vector<TextLine> lines;
    std::string font;
    f32 size = 0.0f;
    f32 lineHeight = 0.0f;
    core::Rect box;
    // The top of the first line, scrolled.
    f32 top = 0.0f;
    // The horizontal scroll, subtracted from every x.
    f32 scrollX = 0.0f;
    i32 horizontalAlignment = 0;
    bool multiLine = false;
};

// The view of the `TextInput` `id` as it is now, or an empty one when it is not
// a laid-out field.
[[nodiscard]] FieldView fieldView(const scene::World& world, core::InstanceId id);

// A text offset in `display`, and back (to the nearest boundary at or before).
[[nodiscard]] core::u32 displayOffset(const FieldView& view, core::u32 textOffset) noexcept;
[[nodiscard]] core::u32 textOffsetOf(const FieldView& view, core::u32 displayOffset) noexcept;

// Which line a display offset is on: the last line that starts at or before it.
[[nodiscard]] core::usize lineOf(const FieldView& view, core::u32 displayOffset) noexcept;
// The left edge of a line, aligned and scrolled.
[[nodiscard]] f32 lineLeft(const FieldView& view, const TextLine& line) noexcept;
// The top-left corner of the caret before `displayOffset`, on screen.
[[nodiscard]] core::Vec2 caretPoint(const FieldView& view, core::u32 displayOffset);
// The display offset nearest a point on screen: the line it is on, then the
// boundary nearest its x.
[[nodiscard]] core::u32 displayOffsetAt(const FieldView& view, core::Vec2 point);

} // namespace engine::ui
