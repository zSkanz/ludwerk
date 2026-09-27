// The hand-written half of `ui`'s reflection (architecture.md §4).
//
// Forty-five properties across thirteen classes, and almost every one of them
// stores a value and marks a layout dirty. The two that are not boilerplate are
// worth finding: `AbsolutePosition` and `AbsoluteSize` have no setter at all,
// because they are the solver's OUTPUT -- writing one would be arguing with the
// layout rather than changing it.
//
// **The dirty marking is the design and not an optimisation.** A write that
// changes what the solver would produce walks up to the nearest `ScreenGui` and
// marks it; a purely visual write does not. That is what makes "a screen nothing
// changed runs no solver" a fact rather than a hope, and it is why the
// milestone's benchmark asserts ZERO solver invocations on an idle frame --
// "about zero microseconds" would be a measurement of the clock rather than of
// the engine.
#include "engine/scene/world.h"
#include "engine/ui/scene_types.h"

// By a path relative to this file rather than through an include directory:
// every module's generated header has the same name, so two of them on one
// include path would resolve by search order. `scene`, `render` and `input` all
// reach theirs the same way.
#include <cmath>

#include "../generated/class_descriptors.gen.h"

namespace engine::ui {
namespace native {
namespace {

using core::f32;
using core::f64;
using scene::Value;

[[nodiscard]] bool isFinite(f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool isFinite(core::Vec2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool isFinite(core::UDim value) noexcept
{
    return std::isfinite(value.scale) && std::isfinite(value.offset);
}

// Up the tree to the `ScreenGui` this element belongs to, marking it dirty.
//
// Walked rather than cached, and the walk is what makes a UI tree's depth the
// cost of a layout-affecting write. Ten levels is a deep UI and ten parent
// lookups is nothing; a cached pointer would be a second thing to invalidate on
// every reparent, which is the bookkeeping that goes wrong quietly.
//
// An element outside any ScreenGui marks nothing, and that is correct: it is not
// laid out, so there is no layout to redo.
void markLayoutDirty(scene::World& world, core::InstanceId id)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (scene::ScreenGuiComponent* screen = world.screenGuis().find(current); screen != nullptr) {
            screen->layoutDirty = true;
            return;
        }
    }
}

// An enum write, checked against the right enum AND against the item list, the
// same way `input`'s is. The id comes from the generated header rather than from
// a lookup by name: a property write should not pay a hash probe for a number
// the generator already decided.
[[nodiscard]] bool takeEnum(const scene::World& world, const Value& value, scene::EnumId enumId, core::i32& out)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (item == nullptr || item->enumId != enumId)
        return false;
    if (world.enums().findValue(enumId, item->value) == nullptr)
        return false;
    out = item->value;
    return true;
}

} // namespace

// --- ScreenGui -------------------------------------------------------------

Value getScreenGuiEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    return component == nullptr ? Value{} : Value{component->enabled};
}

bool setScreenGuiEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->enabled = *flag;
    markLayoutDirty(world, id);
    return true;
}

Value getScreenGuiKeepOnSceneLoad(const scene::World& world, core::InstanceId id)
{
    const scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    return component == nullptr ? Value{} : Value{component->keepOnSceneLoad};
}

bool setScreenGuiKeepOnSceneLoad(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (component == nullptr || flag == nullptr)
        return false;
    component->keepOnSceneLoad = *flag;
    return true;
}

Value getScreenGuiDisplayOrder(const scene::World& world, core::InstanceId id)
{
    const scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    return component == nullptr ? Value{} : Value{component->displayOrder};
}

bool setScreenGuiDisplayOrder(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->displayOrder = static_cast<f32>(*number);
    return true;
}

Value getScreenGuiScreenInsets(const scene::World& world, core::InstanceId id)
{
    const scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    return component == nullptr ? Value{} : Value{component->screenInsets};
}

bool setScreenGuiScreenInsets(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScreenGuiComponent* component = world.screenGuis().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->screenInsets = *flag;
    markLayoutDirty(world, id);
    return true;
}

// --- UIObject --------------------------------------------------------------

Value getUIObjectPosition(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->position};
}

bool setUIObjectPosition(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* udim2 = std::get_if<core::UDim2>(&value);
    if (udim2 == nullptr || !isFinite(udim2->x) || !isFinite(udim2->y))
        return false;
    component->position = *udim2;
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectSize(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->size};
}

bool setUIObjectSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* udim2 = std::get_if<core::UDim2>(&value);
    if (udim2 == nullptr || !isFinite(udim2->x) || !isFinite(udim2->y))
        return false;
    component->size = *udim2;
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectAnchorPoint(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->anchorPoint};
}

bool setUIObjectAnchorPoint(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* point = std::get_if<core::Vec2>(&value);
    if (point == nullptr || !isFinite(*point))
        return false;
    component->anchorPoint = *point;
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectRotation(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->rotation};
}

bool setUIObjectRotation(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->rotation = static_cast<f32>(*number);
    return true;
}

Value getUIObjectBackgroundColor(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->backgroundColor};
}

bool setUIObjectBackgroundColor(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* color = std::get_if<core::Color3>(&value);
    if (color == nullptr)
        return false;
    component->backgroundColor = *color;
    return true;
}

Value getUIObjectBackgroundTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->backgroundTransparency};
}

bool setUIObjectBackgroundTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->backgroundTransparency = static_cast<f32>(*number);
    return true;
}

Value getUIObjectVisible(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->visible};
}

bool setUIObjectVisible(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->visible = *flag;
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectZIndex(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->zIndex};
}

bool setUIObjectZIndex(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->zIndex = static_cast<f32>(*number);
    return true;
}

Value getUIObjectLayoutOrder(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->layoutOrder};
}

bool setUIObjectLayoutOrder(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->layoutOrder = static_cast<f32>(*number);
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectAutomaticSize(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{}
                                : Value{scene::EnumValue{generated::AutomaticSizeEnumId, component->automaticSize}};
}

bool setUIObjectAutomaticSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::AutomaticSizeEnumId, item))
        return false;
    component->automaticSize = item;
    markLayoutDirty(world, id);
    return true;
}

Value getUIObjectClipsDescendants(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->clipsDescendants};
}

bool setUIObjectClipsDescendants(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIObjectComponent* component = world.uiObjects().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->clipsDescendants = *flag;
    return true;
}

Value getUIObjectAbsolutePosition(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->absolutePosition};
}

Value getUIObjectAbsoluteSize(const scene::World& world, core::InstanceId id)
{
    const scene::UIObjectComponent* component = world.uiObjects().find(id);
    return component == nullptr ? Value{} : Value{component->absoluteSize};
}

// --- TextLabel -------------------------------------------------------------

Value getTextLabelText(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->text};
}

bool setTextLabelText(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        return false;
    component->text = *text;
    // **A script assigning `Text` puts the caret at the end** (S6.7), which is
    // what every text field does when its value is set programmatically -- and
    // it is the only answer that is always in range. A caret left where it was
    // points into a string that no longer exists: at best somewhere arbitrary,
    // at worst inside a UTF-8 sequence.
    if (scene::TextInputComponent* field = world.textInputs().find(id); field != nullptr)
        field->caret = static_cast<core::u32>(component->text.size());
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelTextColor(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->textColor};
}

bool setTextLabelTextColor(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* color = std::get_if<core::Color3>(&value);
    if (color == nullptr)
        return false;
    component->textColor = *color;
    return true;
}

Value getTextLabelTextTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->textTransparency)};
}

// Kept as written, like `BackgroundTransparency`; the draw clamps.
bool setTextLabelTextTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->textTransparency = static_cast<f32>(*number);
    return true;
}

Value getTextLabelTextSize(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->textSize};
}

bool setTextLabelTextSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->textSize = static_cast<f32>(*number);
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelFont(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->font};
}

bool setTextLabelFont(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        return false;
    component->font = *text;
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelTextXAlignment(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::HorizontalAlignmentEnumId, component->horizontalAlignment}};
}

bool setTextLabelTextXAlignment(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::HorizontalAlignmentEnumId, item))
        return false;
    component->horizontalAlignment = item;
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelTextYAlignment(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::VerticalAlignmentEnumId, component->verticalAlignment}};
}

bool setTextLabelTextYAlignment(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::VerticalAlignmentEnumId, item))
        return false;
    component->verticalAlignment = item;
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelTextWrapped(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->textWrapped};
}

bool setTextLabelTextWrapped(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->textWrapped = *flag;
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelRichText(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->richText};
}

bool setTextLabelRichText(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->richText = *flag;
    markLayoutDirty(world, id);
    return true;
}

Value getTextLabelTextScaled(const scene::World& world, core::InstanceId id)
{
    const scene::TextLabelComponent* component = world.textLabels().find(id);
    return component == nullptr ? Value{} : Value{component->textScaled};
}

bool setTextLabelTextScaled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextLabelComponent* component = world.textLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->textScaled = *flag;
    markLayoutDirty(world, id);
    return true;
}

// --- TextInput -------------------------------------------------------------

Value getTextInputPlaceholderText(const scene::World& world, core::InstanceId id)
{
    const scene::TextInputComponent* component = world.textInputs().find(id);
    return component == nullptr ? Value{} : Value{component->placeholderText};
}

bool setTextInputPlaceholderText(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::TextInputComponent* component = world.textInputs().find(id);
    if (component == nullptr)
        return false;
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        return false;
    component->placeholderText = *text;
    markLayoutDirty(world, id);
    return true;
}

// --- ImageLabel ------------------------------------------------------------

Value getImageLabelImage(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{component->image};
}

bool setImageLabelImage(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        return false;
    component->image = *text;
    return true;
}

Value getImageLabelImageColor(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{component->imageColor};
}

bool setImageLabelImageColor(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* color = std::get_if<core::Color3>(&value);
    if (color == nullptr)
        return false;
    component->imageColor = *color;
    return true;
}

Value getImageLabelScaleType(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{scene::EnumValue{generated::ScaleTypeEnumId, component->scaleType}};
}

bool setImageLabelScaleType(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::ScaleTypeEnumId, item))
        return false;
    component->scaleType = item;
    return true;
}

Value getImageLabelSliceCenter(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{component->sliceCenter};
}

bool setImageLabelSliceCenter(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    if (component == nullptr)
        return false;
    const auto* rect = std::get_if<core::Rect>(&value);
    if (rect == nullptr || !isFinite(rect->min) || !isFinite(rect->max))
        return false;
    component->sliceCenter = *rect;
    return true;
}

Value getImageLabelImageRectOffset(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{component->imageRectOffset};
}

bool setImageLabelImageRectOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    const auto* offset = std::get_if<core::Vec2>(&value);
    if (component == nullptr || offset == nullptr || !isFinite(*offset))
        return false;
    component->imageRectOffset = *offset;
    return true;
}

Value getImageLabelImageRectSize(const scene::World& world, core::InstanceId id)
{
    const scene::ImageLabelComponent* component = world.imageLabels().find(id);
    return component == nullptr ? Value{} : Value{component->imageRectSize};
}

bool setImageLabelImageRectSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ImageLabelComponent* component = world.imageLabels().find(id);
    const auto* size = std::get_if<core::Vec2>(&value);
    if (component == nullptr || size == nullptr || !isFinite(*size))
        return false;
    component->imageRectSize = *size;
    return true;
}

// --- ScrollFrame -----------------------------------------------------------

Value getScrollFrameCanvasSize(const scene::World& world, core::InstanceId id)
{
    const scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    return component == nullptr ? Value{} : Value{component->canvasSize};
}

bool setScrollFrameCanvasSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    if (component == nullptr)
        return false;
    const auto* udim2 = std::get_if<core::UDim2>(&value);
    if (udim2 == nullptr || !isFinite(udim2->x) || !isFinite(udim2->y))
        return false;
    component->canvasSize = *udim2;
    markLayoutDirty(world, id);
    return true;
}

Value getScrollFrameCanvasPosition(const scene::World& world, core::InstanceId id)
{
    const scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    return component == nullptr ? Value{} : Value{component->canvasPosition};
}

bool setScrollFrameCanvasPosition(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    if (component == nullptr)
        return false;
    const auto* point = std::get_if<core::Vec2>(&value);
    if (point == nullptr || !isFinite(*point))
        return false;
    component->canvasPosition = *point;
    markLayoutDirty(world, id);
    return true;
}

Value getScrollFrameScrollBarThickness(const scene::World& world, core::InstanceId id)
{
    const scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    return component == nullptr ? Value{} : Value{component->scrollBarThickness};
}

bool setScrollFrameScrollBarThickness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ScrollFrameComponent* component = world.scrollFrames().find(id);
    if (component == nullptr)
        return false;
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number))
        return false;
    component->scrollBarThickness = static_cast<f32>(*number);
    markLayoutDirty(world, id);
    return true;
}

// --- UIListLayout ----------------------------------------------------------

Value getUIListLayoutFillDirection(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr ? Value{}
                                : Value{scene::EnumValue{generated::FillDirectionEnumId, component->fillDirection}};
}

bool setUIListLayoutFillDirection(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::FillDirectionEnumId, item))
        return false;
    component->fillDirection = item;
    markLayoutDirty(world, id);
    return true;
}

Value getUIListLayoutPadding(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr ? Value{} : Value{component->padding};
}

bool setUIListLayoutPadding(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->padding = *udim;
    markLayoutDirty(world, id);
    return true;
}

Value getUIListLayoutHorizontalAlignment(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::HorizontalAlignmentEnumId, component->horizontalAlignment}};
}

bool setUIListLayoutHorizontalAlignment(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::HorizontalAlignmentEnumId, item))
        return false;
    component->horizontalAlignment = item;
    markLayoutDirty(world, id);
    return true;
}

Value getUIListLayoutVerticalAlignment(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::VerticalAlignmentEnumId, component->verticalAlignment}};
}

bool setUIListLayoutVerticalAlignment(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::VerticalAlignmentEnumId, item))
        return false;
    component->verticalAlignment = item;
    markLayoutDirty(world, id);
    return true;
}

Value getUIListLayoutSortOrder(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr ? Value{} : Value{scene::EnumValue{generated::SortOrderEnumId, component->sortOrder}};
}

bool setUIListLayoutSortOrder(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::SortOrderEnumId, item))
        return false;
    component->sortOrder = item;
    markLayoutDirty(world, id);
    return true;
}

Value getUIListLayoutWraps(const scene::World& world, core::InstanceId id)
{
    const scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    return component == nullptr ? Value{} : Value{component->wraps};
}

bool setUIListLayoutWraps(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIListLayoutComponent* component = world.listLayouts().find(id);
    if (component == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr)
        return false;
    component->wraps = *flag;
    markLayoutDirty(world, id);
    return true;
}

// --- UIPadding -------------------------------------------------------------

Value getUIPaddingPaddingTop(const scene::World& world, core::InstanceId id)
{
    const scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    return component == nullptr ? Value{} : Value{component->paddingTop};
}

bool setUIPaddingPaddingTop(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->paddingTop = *udim;
    markLayoutDirty(world, id);
    return true;
}

Value getUIPaddingPaddingBottom(const scene::World& world, core::InstanceId id)
{
    const scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    return component == nullptr ? Value{} : Value{component->paddingBottom};
}

bool setUIPaddingPaddingBottom(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->paddingBottom = *udim;
    markLayoutDirty(world, id);
    return true;
}

Value getUIPaddingPaddingLeft(const scene::World& world, core::InstanceId id)
{
    const scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    return component == nullptr ? Value{} : Value{component->paddingLeft};
}

bool setUIPaddingPaddingLeft(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->paddingLeft = *udim;
    markLayoutDirty(world, id);
    return true;
}

Value getUIPaddingPaddingRight(const scene::World& world, core::InstanceId id)
{
    const scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    return component == nullptr ? Value{} : Value{component->paddingRight};
}

bool setUIPaddingPaddingRight(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIPaddingComponent* component = world.uiPaddings().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->paddingRight = *udim;
    markLayoutDirty(world, id);
    return true;
}

// --- UICorner --------------------------------------------------------------

Value getUICornerCornerRadius(const scene::World& world, core::InstanceId id)
{
    const scene::UICornerComponent* component = world.uiCorners().find(id);
    return component == nullptr ? Value{} : Value{component->cornerRadius};
}

bool setUICornerCornerRadius(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UICornerComponent* component = world.uiCorners().find(id);
    if (component == nullptr)
        return false;
    const auto* udim = std::get_if<core::UDim>(&value);
    if (udim == nullptr || !isFinite(*udim))
        return false;
    component->cornerRadius = *udim;
    return true;
}

// --- UIGradient (ADR 0110) ---------------------------------------------------
//
// None of these mark a layout dirty: a gradient changes the drawing, and the
// draw list is rebuilt every frame from what the components hold.

Value getUIGradientColor(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{component->color};
}

bool setUIGradientColor(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* sequence = std::get_if<core::ColorSequence>(&value);
    if (component == nullptr || sequence == nullptr || !core::validSequence(sequence->keypoints))
        return false;
    component->color = *sequence;
    return true;
}

Value getUIGradientTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{component->transparency};
}

bool setUIGradientTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* sequence = std::get_if<core::NumberSequence>(&value);
    if (component == nullptr || sequence == nullptr || !core::validSequence(sequence->keypoints))
        return false;
    component->transparency = *sequence;
    return true;
}

Value getUIGradientOffset(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{component->offset};
}

bool setUIGradientOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* offset = std::get_if<core::Vec2>(&value);
    if (component == nullptr || offset == nullptr || !isFinite(*offset))
        return false;
    component->offset = *offset;
    return true;
}

Value getUIGradientRotation(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->rotation)};
}

bool setUIGradientRotation(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (component == nullptr || number == nullptr || !isFinite(*number))
        return false;
    component->rotation = static_cast<f32>(*number);
    return true;
}

Value getUIGradientEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{component->enabled};
}

bool setUIGradientEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (component == nullptr || flag == nullptr)
        return false;
    component->enabled = *flag;
    return true;
}

Value getUIGradientType(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{scene::EnumValue{generated::GradientTypeEnumId, component->type}};
}

bool setUIGradientType(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::GradientTypeEnumId, item))
        return false;
    component->type = item;
    return true;
}

Value getUIGradientTileMode(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{}
                                : Value{scene::EnumValue{generated::GradientTileModeEnumId, component->tileMode}};
}

bool setUIGradientTileMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::GradientTileModeEnumId, item))
        return false;
    component->tileMode = item;
    return true;
}

Value getUIGradientScale(const scene::World& world, core::InstanceId id)
{
    const scene::UIGradientComponent* component = world.uiGradients().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->scale)};
}

bool setUIGradientScale(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIGradientComponent* component = world.uiGradients().find(id);
    const auto* number = std::get_if<f64>(&value);
    // Above zero: a scale of nothing is a sequence squeezed to a point, which
    // every tiling would then divide by.
    if (component == nullptr || number == nullptr || !isFinite(*number) || !(*number > 0.0))
        return false;
    component->scale = static_cast<f32>(*number);
    return true;
}

// --- UIStroke (ADR 0110) ------------------------------------------------------

Value getUIStrokeColor(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{component->color};
}

bool setUIStrokeColor(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* color = std::get_if<core::Color3>(&value);
    if (component == nullptr || color == nullptr)
        return false;
    component->color = *color;
    return true;
}

Value getUIStrokeThickness(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->thickness)};
}

bool setUIStrokeThickness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (component == nullptr || number == nullptr || !isFinite(*number) || *number < 0.0)
        return false;
    component->thickness = static_cast<f32>(*number);
    return true;
}

Value getUIStrokeTransparency(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->transparency)};
}

bool setUIStrokeTransparency(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (component == nullptr || number == nullptr || !isFinite(*number))
        return false;
    component->transparency = static_cast<f32>(*number);
    return true;
}

Value getUIStrokeEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{component->enabled};
}

bool setUIStrokeEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (component == nullptr || flag == nullptr)
        return false;
    component->enabled = *flag;
    return true;
}

Value getUIStrokeApplyStrokeMode(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{}
                                : Value{scene::EnumValue{generated::ApplyStrokeModeEnumId, component->applyStrokeMode}};
}

bool setUIStrokeApplyStrokeMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::ApplyStrokeModeEnumId, item))
        return false;
    component->applyStrokeMode = item;
    return true;
}

Value getUIStrokeLineJoinMode(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{}
                                : Value{scene::EnumValue{generated::LineJoinModeEnumId, component->lineJoinMode}};
}

bool setUIStrokeLineJoinMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::LineJoinModeEnumId, item))
        return false;
    component->lineJoinMode = item;
    return true;
}

Value getUIStrokeStrokeSizingMode(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::StrokeSizingModeEnumId, component->strokeSizingMode}};
}

bool setUIStrokeStrokeSizingMode(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::StrokeSizingModeEnumId, item))
        return false;
    component->strokeSizingMode = item;
    return true;
}

Value getUIStrokeBorderStrokePosition(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr
               ? Value{}
               : Value{scene::EnumValue{generated::BorderStrokePositionEnumId, component->borderStrokePosition}};
}

bool setUIStrokeBorderStrokePosition(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    core::i32 item = 0;
    if (component == nullptr || !takeEnum(world, value, generated::BorderStrokePositionEnumId, item))
        return false;
    component->borderStrokePosition = item;
    return true;
}

Value getUIStrokeBorderOffset(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{component->borderOffset};
}

bool setUIStrokeBorderOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* udim = std::get_if<core::UDim>(&value);
    if (component == nullptr || udim == nullptr || !isFinite(*udim))
        return false;
    component->borderOffset = *udim;
    return true;
}

Value getUIStrokeZIndex(const scene::World& world, core::InstanceId id)
{
    const scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->zIndex)};
}

bool setUIStrokeZIndex(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::UIStrokeComponent* component = world.uiStrokes().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (component == nullptr || number == nullptr || !isFinite(*number))
        return false;
    component->zIndex = static_cast<f32>(*number);
    return true;
}

// --- Storage ----------------------------------------------------------------
//
// One pair per class that declares its own components. `World::create` walks the
// ancestry root-first and calls every hook it finds, so a `TextButton` gets both
// the `UIObject` pair and the `TextLabel` pair without either naming the other.

// --- BillboardGui and SurfaceGui (F3) ------------------------------------------
//
// Neither has a dirty flag: a world tree is laid out every frame it is drawn,
// because a billboard's canvas changes size with distance. So these setters
// store and nothing more.

namespace {

// A part, or nothing. What `Adornee` accepts on both classes.
[[nodiscard]] bool takeAdornee(const scene::World& world, const Value& value, core::InstanceId& out)
{
    const auto* id = std::get_if<core::InstanceId>(&value);
    if (id == nullptr)
        return false;
    if (id->valid() && world.parts().find(*id) == nullptr)
        return false;
    out = *id;
    return true;
}

[[nodiscard]] bool takeAtLeastZero(const Value& value, f32& out)
{
    const auto* number = std::get_if<core::f64>(&value);
    if (number == nullptr || !std::isfinite(*number) || *number < 0.0)
        return false;
    out = static_cast<f32>(*number);
    return true;
}

} // namespace

Value getBillboardGuiEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->enabled};
}

bool setBillboardGuiEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (gui == nullptr || flag == nullptr)
        return false;
    gui->enabled = *flag;
    return true;
}

Value getBillboardGuiAdornee(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->adornee};
}

bool setBillboardGuiAdornee(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui != nullptr && takeAdornee(world, value, gui->adornee);
}

Value getBillboardGuiSize(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->size};
}

bool setBillboardGuiSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    const auto* size = std::get_if<core::UDim2>(&value);
    if (gui == nullptr || size == nullptr || !isFinite(size->x) || !isFinite(size->y))
        return false;
    gui->size = *size;
    return true;
}

Value getBillboardGuiWorldOffset(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->worldOffset};
}

bool setBillboardGuiWorldOffset(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    const auto* offset = std::get_if<core::Vec3>(&value);
    if (gui == nullptr || offset == nullptr || !std::isfinite(offset->x) || !std::isfinite(offset->y) ||
        !std::isfinite(offset->z))
        return false;
    gui->worldOffset = *offset;
    return true;
}

Value getBillboardGuiAlwaysOnTop(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->alwaysOnTop};
}

bool setBillboardGuiAlwaysOnTop(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (gui == nullptr || flag == nullptr)
        return false;
    gui->alwaysOnTop = *flag;
    return true;
}

Value getBillboardGuiMaxDistance(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{static_cast<core::f64>(gui->maxDistance)};
}

bool setBillboardGuiMaxDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui != nullptr && takeAtLeastZero(value, gui->maxDistance);
}

Value getBillboardGuiBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui == nullptr ? Value{} : Value{static_cast<core::f64>(gui->brightness)};
}

bool setBillboardGuiBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::BillboardGuiComponent* gui = world.billboardGuis().find(id);
    return gui != nullptr && takeAtLeastZero(value, gui->brightness);
}

void attachBillboardGuiComponents(scene::World& world, core::InstanceId id)
{
    world.billboardGuis().add(id, scene::BillboardGuiComponent{});
}

void detachBillboardGuiComponents(scene::World& world, core::InstanceId id)
{
    world.billboardGuis().remove(id);
}

Value getSurfaceGuiEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->enabled};
}

bool setSurfaceGuiEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (gui == nullptr || flag == nullptr)
        return false;
    gui->enabled = *flag;
    return true;
}

Value getSurfaceGuiAdornee(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->adornee};
}

bool setSurfaceGuiAdornee(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui != nullptr && takeAdornee(world, value, gui->adornee);
}

Value getSurfaceGuiFace(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{scene::EnumValue{generated::FaceEnumId, gui->face}};
}

bool setSurfaceGuiFace(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    if (gui == nullptr)
        return false;
    core::i32 item = 0;
    if (!takeEnum(world, value, generated::FaceEnumId, item))
        return false;
    gui->face = item;
    return true;
}

Value getSurfaceGuiPixelsPerMetre(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{static_cast<core::f64>(gui->pixelsPerMetre)};
}

bool setSurfaceGuiPixelsPerMetre(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    f32 density = 0.0f;
    // Zero pixels to a metre is a canvas with no size, which lays out nothing
    // and divides by zero on the way to the world.
    if (gui == nullptr || !takeAtLeastZero(value, density) || !(density > 0.0f))
        return false;
    gui->pixelsPerMetre = density;
    return true;
}

Value getSurfaceGuiAlwaysOnTop(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{gui->alwaysOnTop};
}

bool setSurfaceGuiAlwaysOnTop(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (gui == nullptr || flag == nullptr)
        return false;
    gui->alwaysOnTop = *flag;
    return true;
}

Value getSurfaceGuiBrightness(const scene::World& world, core::InstanceId id)
{
    const scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui == nullptr ? Value{} : Value{static_cast<core::f64>(gui->brightness)};
}

bool setSurfaceGuiBrightness(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SurfaceGuiComponent* gui = world.surfaceGuis().find(id);
    return gui != nullptr && takeAtLeastZero(value, gui->brightness);
}

void attachSurfaceGuiComponents(scene::World& world, core::InstanceId id)
{
    world.surfaceGuis().add(id, scene::SurfaceGuiComponent{});
}

void detachSurfaceGuiComponents(scene::World& world, core::InstanceId id)
{
    world.surfaceGuis().remove(id);
}

void attachScreenGuiComponents(scene::World& world, core::InstanceId id)
{
    world.screenGuis().add(id, scene::ScreenGuiComponent{});
}

void detachScreenGuiComponents(scene::World& world, core::InstanceId id)
{
    world.screenGuis().remove(id);
}

void attachUIObjectComponents(scene::World& world, core::InstanceId id)
{
    world.uiObjects().add(id, scene::UIObjectComponent{});
}

void detachUIObjectComponents(scene::World& world, core::InstanceId id)
{
    world.uiObjects().remove(id);
}

void attachTextLabelComponents(scene::World& world, core::InstanceId id)
{
    world.textLabels().add(id, scene::TextLabelComponent{});
}

void detachTextLabelComponents(scene::World& world, core::InstanceId id)
{
    world.textLabels().remove(id);
}

void attachTextInputComponents(scene::World& world, core::InstanceId id)
{
    world.textInputs().add(id, scene::TextInputComponent{});
}

void detachTextInputComponents(scene::World& world, core::InstanceId id)
{
    world.textInputs().remove(id);
}

void attachImageLabelComponents(scene::World& world, core::InstanceId id)
{
    world.imageLabels().add(id, scene::ImageLabelComponent{});
}

void detachImageLabelComponents(scene::World& world, core::InstanceId id)
{
    world.imageLabels().remove(id);
}

void attachScrollFrameComponents(scene::World& world, core::InstanceId id)
{
    world.scrollFrames().add(id, scene::ScrollFrameComponent{});
}

void detachScrollFrameComponents(scene::World& world, core::InstanceId id)
{
    world.scrollFrames().remove(id);
}

void attachUIListLayoutComponents(scene::World& world, core::InstanceId id)
{
    world.listLayouts().add(id, scene::UIListLayoutComponent{});
}

void detachUIListLayoutComponents(scene::World& world, core::InstanceId id)
{
    world.listLayouts().remove(id);
}

void attachUIPaddingComponents(scene::World& world, core::InstanceId id)
{
    world.uiPaddings().add(id, scene::UIPaddingComponent{});
}

void detachUIPaddingComponents(scene::World& world, core::InstanceId id)
{
    world.uiPaddings().remove(id);
}

void attachUICornerComponents(scene::World& world, core::InstanceId id)
{
    world.uiCorners().add(id, scene::UICornerComponent{});
}

void detachUICornerComponents(scene::World& world, core::InstanceId id)
{
    world.uiCorners().remove(id);
}

// --- ViewportFrame (ADR 0107) ---------------------------------------------------

void attachViewportFrameComponents(scene::World& world, core::InstanceId id)
{
    world.viewportFrames().add(id, scene::ViewportFrameComponent{});
}

void detachViewportFrameComponents(scene::World& world, core::InstanceId id)
{
    world.viewportFrames().remove(id);
}

Value getViewportFrameCurrentCamera(const scene::World& world, core::InstanceId id)
{
    const scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    if (frame == nullptr || !world.alive(frame->currentCamera))
        return Value{};
    return Value{frame->currentCamera};
}

bool setViewportFrameCurrentCamera(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    if (frame == nullptr)
        return false;
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        // Typed `Camera?`, as `Workspace.CurrentCamera` is.
        const scene::ClassId cameraClass = world.classes().findId(world.atoms().lookup("Camera"));
        if (cameraClass == scene::InvalidClass || !world.isA(*reference, cameraClass))
            return false;
        frame->currentCamera = *reference;
        return true;
    }
    if (scene::valueType(value) != scene::ValueType::Nil)
        return false;
    frame->currentCamera = core::InstanceId{};
    return true;
}

Value getViewportFrameAmbient(const scene::World& world, core::InstanceId id)
{
    const scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    return frame == nullptr ? Value{} : Value{frame->ambient};
}

bool setViewportFrameAmbient(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    if (color == nullptr || frame == nullptr)
        return false;
    frame->ambient = *color;
    return true;
}

Value getViewportFrameLightColor(const scene::World& world, core::InstanceId id)
{
    const scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    return frame == nullptr ? Value{} : Value{frame->lightColor};
}

bool setViewportFrameLightColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    if (color == nullptr || frame == nullptr)
        return false;
    frame->lightColor = *color;
    return true;
}

Value getViewportFrameLightDirection(const scene::World& world, core::InstanceId id)
{
    const scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    return frame == nullptr ? Value{} : Value{frame->lightDirection};
}

bool setViewportFrameLightDirection(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* direction = std::get_if<core::Vec3>(&value);
    scene::ViewportFrameComponent* frame = world.viewportFrames().find(id);
    // A direction: finite, and not nothing.
    if (direction == nullptr || frame == nullptr || !std::isfinite(direction->x) || !std::isfinite(direction->y) ||
        !std::isfinite(direction->z) ||
        direction->x * direction->x + direction->y * direction->y + direction->z * direction->z < 1e-12f)
        return false;
    frame->lightDirection = *direction;
    return true;
}

void attachUIGradientComponents(scene::World& world, core::InstanceId id)
{
    world.uiGradients().add(id, scene::UIGradientComponent{});
}

void detachUIGradientComponents(scene::World& world, core::InstanceId id)
{
    world.uiGradients().remove(id);
}

void attachUIStrokeComponents(scene::World& world, core::InstanceId id)
{
    world.uiStrokes().add(id, scene::UIStrokeComponent{});
}

void detachUIStrokeComponents(scene::World& world, core::InstanceId id)
{
    world.uiStrokes().remove(id);
}

// --- UIService --------------------------------------------------------------
//
// Two numbers about the WINDOW rather than about any instance, so they live in
// `EngineState` beside the scheduler's -- the same arrangement
// `PhysicsService.FixedTimestep` uses, and for the same reason: there is exactly
// one of each service in a world.

Value getUIServiceSafeAreaInsets(const scene::World& world, core::InstanceId)
{
    return Value{world.engineState().safeAreaInsets};
}

Value getUIServiceDisplayScale(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().displayScale)};
}

Value getUIServiceScreenOrientation(const scene::World& world, core::InstanceId)
{
    return Value{scene::EnumValue{generated::ScreenOrientationEnumId, world.engineState().screenOrientation}};
}

bool setUIServiceScreenOrientation(scene::World& world, core::InstanceId, const Value& value)
{
    return takeEnum(world, value, generated::ScreenOrientationEnumId, world.engineState().screenOrientation);
}

} // namespace native

void registerSceneTypes(scene::ClassRegistry& classes, core::AtomTable& atoms)
{
    generated::registerClasses(classes, atoms);
}

} // namespace engine::ui
