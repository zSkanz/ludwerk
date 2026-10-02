#include "text_field.h"

#include <algorithm>
#include <cmath>

#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "engine/ui/text_edit.h"

namespace engine::ui {

using core::u32;
using core::usize;

FieldView fieldView(const scene::World& world, core::InstanceId id)
{
    FieldView view;
    const scene::UIObjectComponent* self = world.uiObjects().find(id);
    const scene::TextLabelComponent* label = world.textLabels().find(id);
    const scene::TextInputComponent* field = world.textInputs().find(id);
    if (self == nullptr || label == nullptr || field == nullptr)
        return view;

    view.box = core::Rect{self->absolutePosition, self->absolutePosition + self->absoluteSize};
    view.font = label->font;
    view.size = label->textSize * self->unitScale;
    view.lineHeight = textLineHeight(view.font, view.size);
    view.horizontalAlignment = label->horizontalAlignment;
    view.multiLine = field->multiLine;
    view.scrollX = field->multiLine ? 0.0f : field->scroll.x;

    const std::string_view text = label->text;
    const u32 caret = characterBoundary(text, field->caret);
    const auto composeHere = [&] {
        if (field->composition.empty())
            return;
        view.compositionBegin = static_cast<u32>(view.display.size());
        for (u32 at = 0; at < field->composition.size();) {
            const u32 next = nextCharacter(field->composition, at);
            view.boundaries.emplace_back(caret, static_cast<u32>(view.display.size()));
            view.display.append(field->composition, at, next - at);
            at = next;
        }
        view.compositionEnd = static_cast<u32>(view.display.size());
    };
    for (u32 at = 0; at < text.size();) {
        if (at == caret)
            composeHere();
        const u32 next = nextCharacter(text, at);
        view.boundaries.emplace_back(at, static_cast<u32>(view.display.size()));
        // A masked field draws one mask character for each character, and a
        // line break stays a line break.
        if (field->masked && text[at] != '\n')
            view.display += field->maskCharacter;
        else
            view.display.append(text, at, next - at);
        at = next;
    }
    if (caret == text.size())
        composeHere();
    view.boundaries.emplace_back(static_cast<u32>(text.size()), static_cast<u32>(view.display.size()));

    const f32 width = view.box.max.x - view.box.min.x;
    view.lines = textLines(view.display, view.font, view.size, view.multiLine ? width : 0.0f);
    const f32 height = view.box.max.y - view.box.min.y;
    const f32 total = static_cast<f32>(view.lines.size()) * view.lineHeight;
    view.top = view.box.min.y;
    if (total <= height) {
        if (label->verticalAlignment == 1)
            view.top += (height - total) * 0.5f;
        else if (label->verticalAlignment == 2)
            view.top += height - total;
    }
    else {
        view.top -= field->scroll.y;
    }
    return view;
}

u32 displayOffset(const FieldView& view, u32 textOffset) noexcept
{
    const auto found = std::lower_bound(view.boundaries.begin(), view.boundaries.end(), textOffset,
                                        [](const std::pair<u32, u32>& entry, u32 at) { return entry.first < at; });
    return found == view.boundaries.end() ? static_cast<u32>(view.display.size()) : found->second;
}

u32 textOffsetOf(const FieldView& view, u32 displayOffset) noexcept
{
    u32 out = 0;
    for (const auto& [text, display] : view.boundaries) {
        if (display > displayOffset)
            break;
        out = text;
    }
    return out;
}

usize lineOf(const FieldView& view, u32 displayOffset) noexcept
{
    usize index = 0;
    for (usize line = 0; line < view.lines.size(); ++line) {
        if (view.lines[line].begin <= displayOffset)
            index = line;
    }
    return index;
}

f32 lineLeft(const FieldView& view, const TextLine& line) noexcept
{
    const f32 width = view.box.max.x - view.box.min.x;
    // A single line longer than its field scrolls from the left; one that
    // fits sits where the label's alignment puts it.
    if (!view.multiLine && line.width > width)
        return view.box.min.x - view.scrollX;
    f32 x = view.box.min.x;
    if (view.horizontalAlignment == 1)
        x += (width - line.width) * 0.5f;
    else if (view.horizontalAlignment == 2)
        x += width - line.width;
    return x - view.scrollX;
}

core::Vec2 caretPoint(const FieldView& view, u32 displayOffset)
{
    if (view.lines.empty())
        return core::Vec2{view.box.min.x, view.top};
    const usize index = lineOf(view, displayOffset);
    const TextLine& line = view.lines[index];
    // A wrapped line dropped the space it broke at: an offset on it is at the
    // line's end.
    const usize at = std::min<usize>(displayOffset, line.end);
    const std::string_view before = std::string_view(view.display).substr(line.begin, at - line.begin);
    return core::Vec2{lineLeft(view, line) + textWidth(before, view.font, view.size),
                      view.top + static_cast<f32>(index) * view.lineHeight};
}

u32 displayOffsetAt(const FieldView& view, core::Vec2 point)
{
    if (view.lines.empty())
        return 0;
    const f32 row = view.lineHeight > 0.0f ? std::floor((point.y - view.top) / view.lineHeight) : 0.0f;
    const usize index = static_cast<usize>(std::clamp(row, 0.0f, static_cast<f32>(view.lines.size() - 1)));
    const TextLine& line = view.lines[index];
    const f32 left = lineLeft(view, line);
    // The boundary whose x is nearest: a press on the right half of a
    // character puts the caret after it.
    u32 best = static_cast<u32>(line.begin);
    f32 bestDistance = std::fabs(point.x - left);
    for (const auto& [text, display] : view.boundaries) {
        (void)text;
        if (display <= line.begin || display > line.end)
            continue;
        const std::string_view before = std::string_view(view.display).substr(line.begin, display - line.begin);
        const f32 distance = std::fabs(point.x - (left + textWidth(before, view.font, view.size)));
        if (distance < bestDistance) {
            best = display;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace engine::ui
