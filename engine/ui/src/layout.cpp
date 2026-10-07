#include <algorithm>
#include <cmath>

#include "engine/core/easing.h"
#include "engine/scene/ui_pages.h"
#include "engine/scene/world.h"
#include "engine/ui/ui.h"
#include "scroll_bar.h"

namespace engine::ui {
namespace {

using core::Rect;
using core::UDim;
using core::UDim2;
using core::Vec2;

LayoutStats g_stats;

// `Enum.AutomaticSize`.
constexpr i32 AutoNone = 0;
constexpr i32 AutoX = 1;
constexpr i32 AutoY = 2;
constexpr i32 AutoXY = 3;

// `Enum.FillDirection`, `Enum.HorizontalAlignment`, `Enum.VerticalAlignment`,
// `Enum.SortOrder`. Spelled once rather than compared against literals at the
// four places each is read.
constexpr i32 FillHorizontal = 0;
// `Start` is 0 and is the fall-through case in every alignment branch, so it is
// named in this comment rather than as a constant nothing reads.
constexpr i32 AlignCenter = 1;
constexpr i32 AlignEnd = 2;
// No layout: document order.
constexpr i32 SortByDocument = scene::kUiSortByDocument;

// `Enum.UIFlexAlignment`, `Enum.UIFlexMode`, `Enum.ItemLineAlignment`,
// `Enum.AspectType`, `Enum.DominantAxis` and `Enum.StartCorner` (ADR 0128).
// `None` and `Automatic` are 0 and are the fall-through cases.
constexpr i32 FlexFill = 1;
constexpr i32 FlexSpaceAround = 2;
constexpr i32 FlexSpaceBetween = 3;
constexpr i32 FlexSpaceEvenly = 4;
constexpr i32 ModeGrow = 1;
constexpr i32 ModeShrink = 2;
constexpr i32 ModeFill = 3;
constexpr i32 ModeCustom = 4;
constexpr i32 LineAutomatic = 0;
constexpr i32 LineCenter = 2;
constexpr i32 LineEnd = 3;
constexpr i32 LineStretch = 4;
constexpr i32 AspectFit = 0;
constexpr i32 DominantWidth = 0;
constexpr i32 CornerTopRight = 1;
constexpr i32 CornerBottomLeft = 2;
constexpr i32 CornerBottomRight = 3;

// "As long as it likes": the room a line has along an axis its container
// sizes to its content on.
constexpr f32 Unbounded = 1.0e9f;

[[nodiscard]] f32 resolve(UDim value, f32 parent) noexcept
{
    return parent * value.scale + value.offset;
}

[[nodiscard]] Vec2 resolve(const UDim2& value, Vec2 parent) noexcept
{
    return Vec2{resolve(value.x, parent.x), resolve(value.y, parent.y)};
}

// The modifiers an element carries. Looked up once per element rather than
// once per child: a list of ten children would otherwise scan its parent's
// children ten times looking for the same two instances.
//
// **One layout to a parent**: a list, a grid or pages, and the first child that
// is any of them is the one. Two would be two answers to where a child goes.
struct Modifiers
{
    const scene::UIListLayoutComponent* list = nullptr;
    const scene::UIGridLayoutComponent* grid = nullptr;
    const scene::UIPageLayoutComponent* page = nullptr;
    // The instance of whichever of the three it has: where an output is written.
    core::InstanceId layout;
    const scene::UIPaddingComponent* padding = nullptr;
    // What is about the element itself rather than about its children (ADR 0128).
    const scene::UIScaleComponent* scale = nullptr;
    const scene::UIAspectRatioConstraintComponent* aspect = nullptr;
    const scene::UISizeConstraintComponent* limits = nullptr;
    const scene::UIFlexItemComponent* flexItem = nullptr;

    [[nodiscard]] i32 sortOrder() const noexcept
    {
        if (list != nullptr)
            return list->sortOrder;
        if (grid != nullptr)
            return grid->sortOrder;
        if (page != nullptr)
            return page->sortOrder;
        return SortByDocument;
    }
};

[[nodiscard]] Modifiers modifiersOf(const scene::World& world, core::InstanceId parent)
{
    Modifiers found;
    for (core::InstanceId child = world.firstChild(parent); child.valid(); child = world.nextSibling(child)) {
        if (!found.layout.valid()) {
            found.list = world.listLayouts().find(child);
            if (found.list == nullptr)
                found.grid = world.uiGridLayouts().find(child);
            if (found.list == nullptr && found.grid == nullptr)
                found.page = world.uiPageLayouts().find(child);
            if (found.list != nullptr || found.grid != nullptr || found.page != nullptr)
                found.layout = child;
        }
        if (found.padding == nullptr)
            found.padding = world.uiPaddings().find(child);
        if (found.scale == nullptr)
            found.scale = world.uiScales().find(child);
        if (found.aspect == nullptr)
            found.aspect = world.uiAspectRatioConstraints().find(child);
        if (found.limits == nullptr)
            found.limits = world.uiSizeConstraints().find(child);
        if (found.flexItem == nullptr)
            found.flexItem = world.uiFlexItems().find(child);
    }
    return found;
}

// The rectangle a parent offers its children, after its padding. In the
// parent's own pixel space, so `min` is usually not zero.
[[nodiscard]] Rect contentRect(Vec2 origin, Vec2 size, const scene::UIPaddingComponent* padding) noexcept
{
    if (padding == nullptr)
        return Rect{origin, origin + size};

    const f32 left = resolve(padding->paddingLeft, size.x);
    const f32 right = resolve(padding->paddingRight, size.x);
    const f32 top = resolve(padding->paddingTop, size.y);
    const f32 bottom = resolve(padding->paddingBottom, size.y);
    return Rect{Vec2{origin.x + left, origin.y + top}, Vec2{origin.x + size.x - right, origin.y + size.y - bottom}};
}

[[nodiscard]] Vec2 extentOf(Rect rect) noexcept
{
    return Vec2{rect.max.x - rect.min.x, rect.max.y - rect.min.y};
}

// The `UIObject` children of a parent, in the order a layout should walk them
// (`scene::uiChildrenInOrder`).
//
// **Only the visible ones** (D478). An invisible child is not laid out, so it
// has no room to keep: a hidden row of a list left a gap where it had been, a
// hidden slot of a grid a hole, and a parent that sizes to its content stayed
// as large as what nobody could see.
void collectChildren(const scene::World& world, core::InstanceId parent, i32 sortOrder,
                     std::vector<core::InstanceId>& out)
{
    scene::uiChildrenInOrder(world, parent, sortOrder, out);
}

// The size a `TextLabel` wants, or nothing for an element that is not one.
[[nodiscard]] Vec2 textExtent(const scene::World& world, core::InstanceId id, f32 wrapWidth)
{
    const scene::TextLabelComponent* label = world.textLabels().find(id);
    if (label == nullptr || label->text.empty())
        return Vec2{};
    // Measured the way it is drawn: markup when it is rich, and never in a
    // field being typed into (`draw.cpp` says why).
    const f32 wrap = label->textWrapped ? wrapWidth : 0.0f;
    const bool rich = label->richText && world.textInputs().find(id) == nullptr;
    const TextRunMetrics metrics = rich ? measureRichText(label->text, label->font, label->textSize, wrap)
                                        : measureText(label->text, label->font, label->textSize, wrap);
    return metrics.size;
}

// --- A grid (ADR 0128) --------------------------------------------------------

// How a grid of `count` equal cells falls into lines, given the room a line
// has. Arithmetic on five numbers, asked once to size a parent that fits its
// content and once to place the cells -- so the two cannot disagree.
struct GridShape
{
    Vec2 cell;
    Vec2 gap;
    // Cells in a full line, lines, and what the block of them measures.
    i32 perLine = 1;
    i32 lines = 0;
    Vec2 extent;
};

[[nodiscard]] GridShape gridShape(const scene::UIGridLayoutComponent& grid, i32 count, Vec2 room, bool unboundedMain)
{
    GridShape shape;
    shape.cell = resolve(grid.cellSize, room);
    shape.gap = resolve(grid.cellPadding, room);
    if (count <= 0)
        return shape;

    const bool horizontal = grid.fillDirection == FillHorizontal;
    const f32 cell = horizontal ? shape.cell.x : shape.cell.y;
    const f32 gap = horizontal ? shape.gap.x : shape.gap.y;
    const f32 limit = unboundedMain ? Unbounded : (horizontal ? room.x : room.y);

    // As many as fit, and never fewer than one: a cell wider than its parent
    // still goes somewhere. The hair is for a line that fits exactly, which a
    // sum of floats can find a millionth short.
    i32 fits = count;
    if (cell + gap > 0.0f)
        fits = static_cast<i32>(std::floor((limit + gap) / (cell + gap) + 1.0e-4f));
    fits = std::clamp(fits, 1, count);
    if (grid.fillDirectionMaxCells > 0)
        fits = std::min(fits, grid.fillDirectionMaxCells);
    shape.perLine = fits;
    shape.lines = (count + fits - 1) / fits;

    const i32 used = std::min(count, fits);
    const f32 main = static_cast<f32>(used) * cell + static_cast<f32>(used - 1) * gap;
    const f32 crossCell = horizontal ? shape.cell.y : shape.cell.x;
    const f32 crossGap = horizontal ? shape.gap.y : shape.gap.x;
    const f32 cross = static_cast<f32>(shape.lines) * crossCell + static_cast<f32>(shape.lines - 1) * crossGap;
    shape.extent = horizontal ? Vec2{main, cross} : Vec2{cross, main};
    return shape;
}

// --- Constraints (ADR 0128) ---------------------------------------------------

[[nodiscard]] f32 scaleOf(const Modifiers& mods) noexcept
{
    return mods.scale != nullptr ? std::fmax(0.0f, mods.scale->scale) : 1.0f;
}

// **After an element's own size and before its children are laid out, as a
// clamp.** The shape first and the limits after it, so that the limits have
// the last word: "never under 240 wide" is the harder promise, and a shape
// that could break it would make a panel too narrow to read on a small window
// -- which is what the limit is there to stop.
[[nodiscard]] Vec2 constrained(const Modifiers& mods, Vec2 size, Vec2 available) noexcept
{
    if (mods.aspect != nullptr && mods.aspect->aspectRatio > 0.0f) {
        const f32 ratio = mods.aspect->aspectRatio;
        if (mods.aspect->aspectType == AspectFit) {
            // The largest box of that shape inside the element's own.
            if (size.x > size.y * ratio)
                size.x = size.y * ratio;
            else
                size.y = size.x / ratio;
        }
        else if (mods.aspect->dominantAxis == DominantWidth) {
            size.x = available.x;
            size.y = size.x / ratio;
        }
        else {
            size.y = available.y;
            size.x = size.y * ratio;
        }
    }
    if (mods.limits != nullptr) {
        // A maximum of zero is no limit. The minimum after it, so it wins.
        if (mods.limits->maxSize.x > 0.0f)
            size.x = std::fmin(size.x, mods.limits->maxSize.x);
        if (mods.limits->maxSize.y > 0.0f)
            size.y = std::fmin(size.y, mods.limits->maxSize.y);
        size.x = std::fmax(size.x, mods.limits->minSize.x);
        size.y = std::fmax(size.y, mods.limits->minSize.y);
    }
    return size;
}

struct Pass
{
    const scene::World& world;
    std::vector<core::InstanceId> scratch;

    // Bottom-up: the box this element TAKES, in pixels, given the space its
    // parent can offer -- its own size, what `AutomaticSize` makes of its
    // content, its constraints, and its `UIScale`. The axes `AutomaticSize`
    // does not cover come from `Size`, so that a wrapped label knows how wide
    // it may be.
    [[nodiscard]] Vec2 desiredSize(core::InstanceId id, Vec2 available)
    {
        const scene::UIObjectComponent* self = world.uiObjects().find(id);
        if (self == nullptr)
            return Vec2{};
        const Modifiers mods = modifiersOf(world, id);
        return desiredSize(id, *self, mods, available);
    }

    [[nodiscard]] Vec2 desiredSize(core::InstanceId id, const scene::UIObjectComponent& self, const Modifiers& mods,
                                   Vec2 available)
    {
        return constrained(mods, naturalSize(id, self, mods, available), available) * scaleOf(mods);
    }

    // Before any constraint: `Size`, or the content on an automatic axis.
    [[nodiscard]] Vec2 naturalSize(core::InstanceId id, const scene::UIObjectComponent& self, const Modifiers& mods,
                                   Vec2 available)
    {
        Vec2 fixed = resolve(self.size, available);
        if (self.automaticSize == AutoNone)
            return fixed;
        const bool autoX = self.automaticSize == AutoX || self.automaticSize == AutoXY;
        const bool autoY = self.automaticSize == AutoY || self.automaticSize == AutoXY;
        const Vec2 content = contentExtent(id, mods, fixed, autoX, autoY);
        if (autoX)
            fixed.x = content.x;
        if (autoY)
            fixed.y = content.y;
        return fixed;
    }

    // The content an element of size `fixed` has to hold, padding included:
    // its text, or the extent of its children laid out inside it. What an
    // automatic size is, and what a `ScrollFrame`'s automatic canvas is.
    [[nodiscard]] Vec2 contentExtent(core::InstanceId id, const Modifiers& mods, Vec2 fixed, bool autoX, bool autoY)
    {
        // Its words wrap in the room its own padding leaves (G21): where they
        // are drawn, so what is measured is what is drawn.
        f32 wrapRoom = fixed.x;
        if (mods.padding != nullptr)
            wrapRoom = std::fmax(0.0f, fixed.x - resolve(mods.padding->paddingLeft, fixed.x) -
                                           resolve(mods.padding->paddingRight, fixed.x));
        Vec2 content = textExtent(world, id, wrapRoom);

        std::vector<core::InstanceId> children;
        collectChildren(world, id, mods.sortOrder(), children);
        if (!children.empty()) {
            Vec2 childExtent;
            if (mods.list != nullptr) {
                const f32 gap =
                    resolve(mods.list->padding, mods.list->fillDirection == FillHorizontal ? fixed.x : fixed.y);
                for (usize index = 0; index < children.size(); ++index) {
                    const Vec2 want = desiredSize(children[index], fixed);
                    if (mods.list->fillDirection == FillHorizontal) {
                        childExtent.x += want.x + (index == 0 ? 0.0f : gap);
                        childExtent.y = std::fmax(childExtent.y, want.y);
                    }
                    else {
                        childExtent.y += want.y + (index == 0 ? 0.0f : gap);
                        childExtent.x = std::fmax(childExtent.x, want.x);
                    }
                }
            }
            else if (mods.grid != nullptr) {
                // A grid whose line runs along an automatic axis has all the
                // room it likes there: one line, or `FillDirectionMaxCells`.
                const bool horizontal = mods.grid->fillDirection == FillHorizontal;
                childExtent =
                    gridShape(*mods.grid, static_cast<i32>(children.size()), fixed, horizontal ? autoX : autoY).extent;
            }
            else if (mods.page != nullptr) {
                // One page shows: as large as the largest of them.
                for (const core::InstanceId child : children) {
                    const Vec2 want = desiredSize(child, fixed);
                    childExtent.x = std::fmax(childExtent.x, want.x);
                    childExtent.y = std::fmax(childExtent.y, want.y);
                }
            }
            else {
                // Without a layout, children are placed absolutely, so the
                // extent is the furthest any of them reaches -- position and
                // size together, because a child at (0.9, 0) is what makes a
                // parent wide.
                for (const core::InstanceId child : children) {
                    const scene::UIObjectComponent* component = world.uiObjects().find(child);
                    const Vec2 want = desiredSize(child, fixed);
                    const Vec2 at = resolve(component->position, fixed) -
                                    Vec2{component->anchorPoint.x * want.x, component->anchorPoint.y * want.y};
                    childExtent.x = std::fmax(childExtent.x, at.x + want.x);
                    childExtent.y = std::fmax(childExtent.y, at.y + want.y);
                }
            }
            content.x = std::fmax(content.x, childExtent.x);
            content.y = std::fmax(content.y, childExtent.y);
        }

        // Padding is around the content, so it is added rather than subtracted
        // on this pass -- the one place the two directions of the same property
        // read differently, and the one worth a sentence.
        if (mods.padding != nullptr) {
            content.x += resolve(mods.padding->paddingLeft, fixed.x) + resolve(mods.padding->paddingRight, fixed.x);
            content.y += resolve(mods.padding->paddingTop, fixed.y) + resolve(mods.padding->paddingBottom, fixed.y);
        }
        return content;
    }
};

void place(scene::World& world, Pass& pass, core::InstanceId id, Vec2 parentOrigin, Vec2 parentSize, Vec2 forcedSize,
           bool useForced);

// **Every rectangle under `root` multiplied by `scale` about `pivot`**, and
// each element told its scale, for what is measured in units and read when it
// is drawn. What turns a tree laid out in its own units into pixels (D437,
// with the pivot at the origin), and what a `UIScale` does to its element's
// descendants (ADR 0128, with the pivot at the element's corner).
void scaleElement(scene::World& world, core::InstanceId id, Vec2 pivot, f32 scale)
{
    if (scene::UIObjectComponent* object = world.uiObjects().find(id); object != nullptr) {
        object->absolutePosition = pivot + (object->absolutePosition - pivot) * scale;
        object->absoluteSize = object->absoluteSize * scale;
        object->unitScale *= scale;
    }
    // A layout's own output is in the same pixels.
    if (scene::UIListLayoutComponent* list = world.listLayouts().find(id); list != nullptr)
        list->absoluteContentSize = list->absoluteContentSize * scale;
    if (scene::UIGridLayoutComponent* grid = world.uiGridLayouts().find(id); grid != nullptr)
        grid->absoluteContentSize = grid->absoluteContentSize * scale;
    if (scene::ScrollFrameComponent* scroll = world.scrollFrames().find(id); scroll != nullptr) {
        scroll->absoluteCanvasSize = scroll->absoluteCanvasSize * scale;
        scroll->absoluteWindowSize = scroll->absoluteWindowSize * scale;
    }
}

void scaleTree(scene::World& world, core::InstanceId root, Vec2 pivot, f32 scale)
{
    for (core::InstanceId child = world.firstChild(root); child.valid(); child = world.nextSibling(child)) {
        // **Only what this pass placed** (D551). An element that is not
        // visible is not placed, and neither is anything under it (`place`):
        // their rectangles are the ones they last had, in pixels already.
        // Scaled again here at every layout, a hidden row on a tree drawn
        // for half the screen's lines doubled sixty times a second -- past
        // any number in three seconds, and not a number after that.
        if (const scene::UIObjectComponent* object = world.uiObjects().find(child);
            object != nullptr && !object->visible)
            continue;
        scaleElement(world, child, pivot, scale);
        scaleTree(world, child, pivot, scale);
    }
}

// --- A list, with flex (ADR 0128) ---------------------------------------------

// What one child of a list brings to its line.
struct ListItem
{
    Vec2 size;
    // Its share of room left over, and of room that is short.
    f32 grow = 0.0f;
    f32 shrink = 0.0f;
    // `Enum.ItemLineAlignment`, after the layout's own has filled in `Automatic`.
    i32 lineAlignment = LineAutomatic;
};

struct ListLine
{
    usize begin = 0;
    usize end = 0;
    f32 main = 0.0f;
    f32 cross = 0.0f;
};

// Room shared out as space rather than as size: how far in the first one
// starts, and how much goes between two. `count` things in `room` left over.
struct Spacing
{
    f32 lead = 0.0f;
    f32 between = 0.0f;
};

[[nodiscard]] Spacing spacingFor(i32 flex, f32 room, usize count) noexcept
{
    Spacing spacing;
    if (room <= 0.0f || count == 0)
        return spacing;
    const f32 n = static_cast<f32>(count);
    if (flex == FlexSpaceAround) {
        spacing.between = room / n;
        spacing.lead = spacing.between * 0.5f;
    }
    else if (flex == FlexSpaceBetween && count > 1) {
        spacing.between = room / (n - 1.0f);
    }
    else if (flex == FlexSpaceEvenly) {
        spacing.between = room / (n + 1.0f);
        spacing.lead = spacing.between;
    }
    return spacing;
}

[[nodiscard]] bool sharesSpace(i32 flex, usize count) noexcept
{
    return flex == FlexSpaceAround || flex == FlexSpaceEvenly || (flex == FlexSpaceBetween && count > 1);
}

void placeList(scene::World& world, Pass& pass, const Modifiers& mods, const std::vector<core::InstanceId>& children,
               Rect content)
{
    const scene::UIListLayoutComponent& list = *mods.list;
    const Vec2 contentSize = extentOf(content);

    // A single-axis stack. `Wraps` breaks it into rows when the line runs out
    // of room, which is the one case where an element's position depends on the
    // width of its predecessors rather than only on its own properties.
    const bool horizontal = list.fillDirection == FillHorizontal;
    const f32 gap = resolve(list.padding, horizontal ? contentSize.x : contentSize.y);
    const f32 lineLimit = horizontal ? contentSize.x : contentSize.y;
    const f32 crossLimit = horizontal ? contentSize.y : contentSize.x;
    const i32 mainFlex = horizontal ? list.horizontalFlex : list.verticalFlex;
    const i32 crossFlex = horizontal ? list.verticalFlex : list.horizontalFlex;

    // Measured first, so the cross-axis alignment has a total to work from. A
    // stack that aligned as it went could not centre itself.
    std::vector<ListItem> items;
    items.reserve(children.size());
    for (const core::InstanceId child : children) {
        ListItem item;
        const scene::UIObjectComponent* object = world.uiObjects().find(child);
        const Modifiers own = modifiersOf(world, child);
        item.size = pass.desiredSize(child, *object, own, contentSize);
        item.lineAlignment = list.itemLineAlignment;
        if (own.flexItem != nullptr) {
            const scene::UIFlexItemComponent& flex = *own.flexItem;
            item.grow = flex.flexMode == ModeGrow || flex.flexMode == ModeFill ? 1.0f
                        : flex.flexMode == ModeCustom                          ? flex.growRatio
                                                                               : 0.0f;
            item.shrink = flex.flexMode == ModeShrink || flex.flexMode == ModeFill ? 1.0f
                          : flex.flexMode == ModeCustom                            ? flex.shrinkRatio
                                                                                   : 0.0f;
            if (flex.itemLineAlignment != LineAutomatic)
                item.lineAlignment = flex.itemLineAlignment;
        }
        items.push_back(item);
    }
    const auto mainOf = [horizontal](Vec2 size) { return horizontal ? size.x : size.y; };
    const auto crossOf = [horizontal](Vec2 size) { return horizontal ? size.y : size.x; };

    // The lines, from the sizes the children asked for: flex changes how long a
    // child is, never which line it is on.
    std::vector<ListLine> lines;
    for (usize start = 0; start < items.size();) {
        ListLine line;
        line.begin = start;
        line.end = start;
        while (line.end < items.size()) {
            const f32 next = line.main + mainOf(items[line.end].size) + (line.end == line.begin ? 0.0f : gap);
            if (list.wraps && line.end != line.begin && next > lineLimit)
                break;
            line.main = next;
            line.cross = std::fmax(line.cross, crossOf(items[line.end].size));
            ++line.end;
        }
        lines.push_back(line);
        start = line.end;
        if (!list.wraps)
            break;
    }

    // What the children take as they were asked for, before any flex: what a
    // scrolling region is sized from.
    {
        f32 longest = 0.0f;
        f32 across = 0.0f;
        for (usize index = 0; index < lines.size(); ++index) {
            longest = std::fmax(longest, lines[index].main);
            across += lines[index].cross + (index == 0 ? 0.0f : gap);
        }
        if (scene::UIListLayoutComponent* out = world.listLayouts().find(mods.layout); out != nullptr)
            out->absoluteContentSize = horizontal ? Vec2{longest, across} : Vec2{across, longest};
    }

    // **Across the lines** (ADR 0128): room the lines leave over is given to
    // them (`Fill`) or put between them. Only where there are lines to speak of
    // -- a list that does not wrap is one line as deep as its container.
    f32 crossRoom = 0.0f;
    if (list.wraps) {
        f32 total = 0.0f;
        for (usize index = 0; index < lines.size(); ++index)
            total += lines[index].cross + (index == 0 ? 0.0f : gap);
        crossRoom = std::fmax(0.0f, crossLimit - total);
    }
    const Spacing lineSpacing = spacingFor(crossFlex, crossRoom, lines.size());
    const f32 lineGrowth = crossFlex == FlexFill && !lines.empty() ? crossRoom / static_cast<f32>(lines.size()) : 0.0f;

    f32 lineOffset = lineSpacing.lead;
    for (const ListLine& line : lines) {
        const usize count = line.end - line.begin;

        // **Along the line**: room left over goes to the children that grow,
        // by their shares; with none, to all of them equally under `Fill`.
        // Room that is short comes out of the ones that shrink. One pass, so a
        // child that reaches nothing takes no more and the rest is not shared
        // out again -- arithmetic, not a solver (ADR 0040).
        f32 room = lineLimit - line.main;
        if (room > 0.0f) {
            f32 shares = 0.0f;
            for (usize index = line.begin; index < line.end; ++index)
                shares += items[index].grow;
            if (shares > 0.0f) {
                for (usize index = line.begin; index < line.end; ++index) {
                    const f32 more = room * items[index].grow / shares;
                    (horizontal ? items[index].size.x : items[index].size.y) += more;
                }
                room = 0.0f;
            }
            else if (mainFlex == FlexFill) {
                for (usize index = line.begin; index < line.end; ++index)
                    (horizontal ? items[index].size.x : items[index].size.y) += room / static_cast<f32>(count);
                room = 0.0f;
            }
        }
        else if (room < 0.0f) {
            f32 shares = 0.0f;
            for (usize index = line.begin; index < line.end; ++index)
                shares += items[index].shrink;
            if (shares > 0.0f) {
                // What the line is short by, before any of it is taken back.
                const f32 lacking = -room;
                for (usize index = line.begin; index < line.end; ++index) {
                    f32& main = horizontal ? items[index].size.x : items[index].size.y;
                    const f32 less = lacking * items[index].shrink / shares;
                    room += std::fmin(main, less);
                    main = std::fmax(0.0f, main - less);
                }
            }
        }

        // Where the line starts: shared-out space when the layout asks for it
        // and there is some, the alignment otherwise.
        const Spacing spacing = spacingFor(mainFlex, room, count);
        f32 cursor = spacing.lead;
        if (room <= 0.0f || !sharesSpace(mainFlex, count)) {
            const i32 mainAlign = horizontal ? list.horizontalAlignment : list.verticalAlignment;
            if (mainAlign == AlignCenter)
                cursor = room * 0.5f;
            else if (mainAlign == AlignEnd)
                cursor = room;
            else
                cursor = 0.0f;
        }

        const i32 crossAlign = horizontal ? list.verticalAlignment : list.horizontalAlignment;

        // **What a cross-axis alignment aligns AGAINST** (D029). Without wrap it
        // is the container's own extent, exactly as the main axis uses
        // `lineLimit` above -- and using the widest child instead is what made a
        // centred column of equal-width children sit on the left edge:
        // `lineCross - cross` is zero for every one of them, so "centred" placed
        // them all at `content.min`.
        //
        // **With wrap it is the line's own band, and that is a decision rather
        // than a fallout.** Wrapped lines stack along the cross axis, each
        // occupying `lineCross` of it; centring one against the whole container
        // would put every line on top of every other. Within its band is the
        // only arrangement that is still a stack.
        const f32 band = list.wraps ? line.cross + lineGrowth : crossLimit;

        for (usize index = line.begin; index < line.end; ++index) {
            Vec2 childSize = items[index].size;
            const f32 main = mainOf(childSize);
            f32 cross = crossOf(childSize);

            // Where it sits across the band: as the layout's alignment says
            // while nobody said otherwise, and as `ItemLineAlignment` says
            // when somebody did.
            f32 crossOffset = lineOffset;
            const i32 lineAlignment = items[index].lineAlignment;
            if (lineAlignment == LineStretch) {
                cross = band;
                (horizontal ? childSize.y : childSize.x) = band;
            }
            else if (lineAlignment == LineCenter || (lineAlignment == LineAutomatic && crossAlign == AlignCenter)) {
                crossOffset += (band - cross) * 0.5f;
            }
            else if (lineAlignment == LineEnd || (lineAlignment == LineAutomatic && crossAlign == AlignEnd)) {
                crossOffset += band - cross;
            }

            const Vec2 at = horizontal ? Vec2{content.min.x + cursor, content.min.y + crossOffset}
                                       : Vec2{content.min.x + crossOffset, content.min.y + cursor};
            place(world, pass, children[index], at, contentSize, childSize, true);
            cursor += main + gap + spacing.between;
        }

        lineOffset += band + gap + lineSpacing.between;
    }
}

void placeGrid(scene::World& world, Pass& pass, const Modifiers& mods, const std::vector<core::InstanceId>& children,
               Rect content)
{
    const scene::UIGridLayoutComponent& grid = *mods.grid;
    const Vec2 contentSize = extentOf(content);
    const i32 count = static_cast<i32>(children.size());
    const GridShape shape = gridShape(grid, count, contentSize, false);
    if (scene::UIGridLayoutComponent* out = world.uiGridLayouts().find(mods.layout); out != nullptr)
        out->absoluteContentSize = shape.extent;

    // The block of cells, placed in the parent by the two alignments.
    Vec2 origin = content.min;
    if (grid.horizontalAlignment == AlignCenter)
        origin.x += (contentSize.x - shape.extent.x) * 0.5f;
    else if (grid.horizontalAlignment == AlignEnd)
        origin.x += contentSize.x - shape.extent.x;
    if (grid.verticalAlignment == AlignCenter)
        origin.y += (contentSize.y - shape.extent.y) * 0.5f;
    else if (grid.verticalAlignment == AlignEnd)
        origin.y += contentSize.y - shape.extent.y;

    const bool horizontal = grid.fillDirection == FillHorizontal;
    const i32 used = std::min(count, shape.perLine);
    const i32 columns = horizontal ? used : shape.lines;
    const i32 rows = horizontal ? shape.lines : used;
    const bool fromRight = grid.startCorner == CornerTopRight || grid.startCorner == CornerBottomRight;
    const bool fromBottom = grid.startCorner == CornerBottomLeft || grid.startCorner == CornerBottomRight;

    for (i32 index = 0; index < count; ++index) {
        const i32 line = index / shape.perLine;
        const i32 slot = index % shape.perLine;
        i32 column = horizontal ? slot : line;
        i32 row = horizontal ? line : slot;
        if (fromRight)
            column = columns - 1 - column;
        if (fromBottom)
            row = rows - 1 - row;
        const Vec2 at{origin.x + static_cast<f32>(column) * (shape.cell.x + shape.gap.x),
                      origin.y + static_cast<f32>(row) * (shape.cell.y + shape.gap.y)};
        place(world, pass, children[static_cast<usize>(index)], at, contentSize, shape.cell, true);
    }
}

// --- Pages (ADR 0128) ---------------------------------------------------------

void placePages(scene::World& world, Pass& pass, const Modifiers& mods, const std::vector<core::InstanceId>& children,
                Rect content)
{
    scene::UIPageLayoutComponent* page = world.uiPageLayouts().find(mods.layout);
    if (page == nullptr)
        return;
    const Vec2 contentSize = extentOf(content);
    const i32 count = static_cast<i32>(children.size());

    // **The page shown is one of the pages there are.** A page destroyed, hidden
    // or sorted elsewhere since the last turn leaves the index pointing at
    // another: the layout shows what is there now, and says so in `CurrentPage`.
    page->index = std::clamp(page->index, 0, count - 1);
    page->currentPage = children[static_cast<usize>(page->index)];
    if (!page->sliding)
        page->position = static_cast<f32>(page->index);

    const bool horizontal = page->fillDirection == FillHorizontal;
    const f32 limit = horizontal ? contentSize.x : contentSize.y;
    const f32 crossLimit = horizontal ? contentSize.y : contentSize.x;
    const f32 gap = resolve(page->padding, limit);
    const i32 mainAlign = horizontal ? page->horizontalAlignment : page->verticalAlignment;
    const i32 crossAlign = horizontal ? page->verticalAlignment : page->horizontalAlignment;
    const f32 mainShare = mainAlign == AlignCenter ? 0.5f : mainAlign == AlignEnd ? 1.0f : 0.0f;
    const f32 crossShare = crossAlign == AlignCenter ? 0.5f : crossAlign == AlignEnd ? 1.0f : 0.0f;

    // Side by side: where each page starts along the strip, and the strip's
    // whole length -- which is how far apart the last page and the first are
    // when the pages go round.
    std::vector<Vec2> sizes;
    std::vector<f32> starts;
    sizes.reserve(children.size());
    starts.reserve(children.size());
    f32 total = 0.0f;
    for (const core::InstanceId child : children) {
        sizes.push_back(pass.desiredSize(child, contentSize));
        starts.push_back(total);
        total += (horizontal ? sizes.back().x : sizes.back().y) + gap;
    }

    // Where the strip is when page `whole` is the one shown: that page, placed
    // by the alignment. Past either end it is a whole strip further on, which
    // is only ever asked of pages that go round.
    const auto shownAt = [&](i32 whole) {
        const i32 laps = static_cast<i32>(std::floor(static_cast<f32>(whole) / static_cast<f32>(count)));
        const usize at = static_cast<usize>(whole - laps * count);
        const f32 main = horizontal ? sizes[at].x : sizes[at].y;
        return starts[at] - (limit - main) * mainShare + static_cast<f32>(laps) * total;
    };
    f32 position = page->position;
    if (!page->circular)
        position = std::clamp(position, 0.0f, static_cast<f32>(count - 1));
    const f32 below = std::floor(position);
    const f32 from = shownAt(static_cast<i32>(below));
    const f32 to = position > below ? shownAt(static_cast<i32>(below) + 1) : from;
    const f32 scroll = from + (to - from) * (position - below);

    for (usize index = 0; index < children.size(); ++index) {
        f32 main = starts[index] - scroll;
        // Round the end, the nearest copy of the page is the one drawn.
        if (page->circular && count > 1 && total > 0.0f)
            main -= total * std::floor((main + total * 0.5f) / total);
        const f32 cross = (crossLimit - (horizontal ? sizes[index].y : sizes[index].x)) * crossShare;
        const Vec2 at = horizontal ? Vec2{content.min.x + main, content.min.y + cross}
                                   : Vec2{content.min.x + cross, content.min.y + main};
        place(world, pass, children[index], at, contentSize, sizes[index], true);
    }
}

// Top-down: assign this element's rectangle, then its children's.
void place(scene::World& world, Pass& pass, core::InstanceId id, Vec2 parentOrigin, Vec2 parentSize, Vec2 forcedSize,
           bool useForced)
{
    scene::UIObjectComponent* self = world.uiObjects().find(id);
    if (self == nullptr || !self->visible)
        return;
    const Modifiers mods = modifiersOf(world, id);

    // `useForced` is how a layout hands a child the slot it computed: the size
    // AND the top-left are the layout's, and the child's own `Position` and
    // `AnchorPoint` are not consulted. A child inside a layout does not place
    // itself -- that is what a layout is.
    const Vec2 taken = useForced ? forcedSize : pass.desiredSize(id, *self, mods, parentSize);
    const Vec2 topLeft = useForced ? parentOrigin
                                   : parentOrigin + resolve(self->position, parentSize) -
                                         Vec2{self->anchorPoint.x * taken.x, self->anchorPoint.y * taken.y};

    // **A `UIScale`** (ADR 0128): the box it takes is `Scale` times its size,
    // and everything inside is laid out as if nothing had changed and scaled
    // afterwards -- offsets, text and corners alike, which is the difference
    // between scaling an element and resizing it.
    const f32 scale = scaleOf(mods);
    const Vec2 size = scale > 0.0f && scale != 1.0f ? taken * (1.0f / scale) : taken;

    self->absolutePosition = topLeft;
    self->absoluteSize = size;
    // In pixels until a `UIScale` or a scaled `ScreenGui` says otherwise.
    self->unitScale = 1.0f;
    ++g_stats.elementsLaidOut;

    Rect content = contentRect(topLeft, size, mods.padding);

    // A ScrollFrame's children lay out against its canvas, shifted by how far
    // it has been scrolled. Clamped here rather than at the write, so a script
    // that scrolls past the end settles at the end on the next layout instead
    // of showing emptiness.
    if (scene::ScrollFrameComponent* scroll = world.scrollFrames().find(id); scroll != nullptr) {
        Vec2 canvas = resolve(scroll->canvasSize, size);
        // **An automatic canvas holds its contents** (G40): on such an axis it
        // is the larger of `CanvasSize` and what the children take, measured
        // the way an automatic size is -- against the frame along the axis
        // that grows, and against the canvas along the one that does not.
        if (scroll->automaticCanvasSize != AutoNone) {
            const bool autoX = scroll->automaticCanvasSize == AutoX || scroll->automaticCanvasSize == AutoXY;
            const bool autoY = scroll->automaticCanvasSize == AutoY || scroll->automaticCanvasSize == AutoXY;
            const Vec2 base{autoX ? size.x : std::fmax(canvas.x, size.x), autoY ? size.y : std::fmax(canvas.y, size.y)};
            const Vec2 contents = pass.contentExtent(id, mods, base, autoX, autoY);
            if (autoX)
                canvas.x = std::fmax(canvas.x, contents.x);
            if (autoY)
                canvas.y = std::fmax(canvas.y, contents.y);
        }
        scroll->absoluteCanvasSize = Vec2{std::fmax(canvas.x, size.x), std::fmax(canvas.y, size.y)};
        scroll->absoluteWindowSize = size;
        const Vec2 room{std::fmax(0.0f, canvas.x - size.x), std::fmax(0.0f, canvas.y - size.y)};
        scroll->canvasPosition = Vec2{std::clamp(scroll->canvasPosition.x, 0.0f, room.x),
                                      std::clamp(scroll->canvasPosition.y, 0.0f, room.y)};
        // **The canvas is never smaller than the frame, on either axis** (D479).
        // A list that scrolls down says `CanvasSize = (0, 400)`, and the zero
        // was taken as a canvas with no width: every child sized as a fraction
        // of it was nothing wide, and a grid had room for one cell a row. An
        // axis that is not scrolled is as long as the frame is, less its
        // padding like any other parent's.
        const Vec2 inner = extentOf(content);
        const Vec2 padded{size.x - inner.x, size.y - inner.y};
        const Vec2 extent{std::fmax(canvas.x, size.x) - padded.x, std::fmax(canvas.y, size.y) - padded.y};
        // A hand's pull past the end is drawn here and nowhere else.
        const Vec2 shown = scroll->canvasPosition + scroll->overscroll;
        content = Rect{content.min - shown, content.min - shown + extent};
    }

    std::vector<core::InstanceId> children;
    collectChildren(world, id, mods.sortOrder(), children);
    if (!children.empty()) {
        if (mods.list != nullptr) {
            placeList(world, pass, mods, children, content);
        }
        else if (mods.grid != nullptr) {
            placeGrid(world, pass, mods, children, content);
        }
        else if (mods.page != nullptr) {
            placePages(world, pass, mods, children, content);
        }
        else {
            const Vec2 contentSize = extentOf(content);
            for (const core::InstanceId child : children)
                place(world, pass, child, content.min, contentSize, Vec2{}, false);
        }
    }
    else {
        // A layout with nothing to arrange says so, rather than keeping what
        // it measured when it had children.
        if (scene::UIListLayoutComponent* list = world.listLayouts().find(mods.layout); list != nullptr)
            list->absoluteContentSize = Vec2{};
        if (scene::UIGridLayoutComponent* grid = world.uiGridLayouts().find(mods.layout); grid != nullptr)
            grid->absoluteContentSize = Vec2{};
        if (scene::UIPageLayoutComponent* page = world.uiPageLayouts().find(mods.layout); page != nullptr) {
            page->currentPage = {};
            page->index = 0;
        }
    }

    if (scale != 1.0f) {
        scaleElement(world, id, topLeft, scale);
        scaleTree(world, id, topLeft, scale);
    }
}

} // namespace

f32 screenScale(const scene::ScreenGuiComponent& screen, core::Vec2 windowSize) noexcept
{
    return screen.referenceHeight > 0.0f && windowSize.y > 0.0f ? windowSize.y / screen.referenceHeight : 1.0f;
}

core::Rect textRectOf(const scene::World& world, core::InstanceId element, core::Rect box) noexcept
{
    const scene::UIPaddingComponent* padding = nullptr;
    for (core::InstanceId child = world.firstChild(element); child.valid() && padding == nullptr;
         child = world.nextSibling(child))
        padding = world.uiPaddings().find(child);
    if (padding == nullptr)
        return box;
    Rect inner = contentRect(box.min, Vec2{box.max.x - box.min.x, box.max.y - box.min.y}, padding);
    if (inner.max.x < inner.min.x)
        inner.min.x = inner.max.x = (inner.min.x + inner.max.x) * 0.5f;
    if (inner.max.y < inner.min.y)
        inner.min.y = inner.max.y = (inner.min.y + inner.max.y) * 0.5f;
    return inner;
}

void layout(scene::World& world, core::InstanceId uiService, core::Vec2 windowSize)
{
    if (!uiService.valid())
        return;

    Pass pass{world, {}};

    // Document order over `UIService`'s children, which is where every
    // `ScreenGui` lives. Not sorted by `DisplayOrder` here: that orders DRAWING
    // and two trees never affect each other's layout, so sorting would be work
    // with no observable result.
    for (core::InstanceId child = world.firstChild(uiService); child.valid(); child = world.nextSibling(child)) {
        scene::ScreenGuiComponent* screen = world.screenGuis().find(child);
        if (screen == nullptr || !screen->enabled || !screen->layoutDirty)
            continue;

        ++g_stats.solverRuns;
        screen->layoutDirty = false;

        // The insets are the window's, and a tree opts out of them with
        // `ScreenInsets = false` -- a full-bleed background is the exception
        // and a HUD clipped by a notch is the failure.
        Vec2 origin;
        Vec2 available = windowSize;
        if (screen->screenInsets) {
            const core::Rect& insets = world.engineState().safeAreaInsets;
            origin = insets.min;
            available = Vec2{windowSize.x - insets.min.x - insets.max.x, windowSize.y - insets.min.y - insets.max.y};
        }

        // **In the tree's own units** (D437): a window `ReferenceHeight`
        // tall, as wide as this one is for that height. Laid out there, and
        // scaled to the pixels afterwards -- so `AbsolutePosition` and a hit
        // test are in window pixels, as they always were.
        const f32 scale = screenScale(*screen, windowSize);
        if (scale != 1.0f) {
            origin = origin * (1.0f / scale);
            available = available * (1.0f / scale);
        }

        for (core::InstanceId element = world.firstChild(child); element.valid();
             element = world.nextSibling(element)) {
            place(world, pass, element, origin, available, Vec2{}, false);
        }
        if (scale != 1.0f)
            scaleTree(world, child, Vec2{}, scale);
    }
}

void layoutCanvas(scene::World& world, core::InstanceId root, core::Vec2 canvasSize)
{
    if (!root.valid())
        return;
    Pass pass{world, {}};
    ++g_stats.solverRuns;
    for (core::InstanceId element = world.firstChild(root); element.valid(); element = world.nextSibling(element))
        place(world, pass, element, Vec2{}, canvasSize, Vec2{}, false);
}

const LayoutStats& layoutStats() noexcept
{
    return g_stats;
}

void resetLayoutStats() noexcept
{
    g_stats = LayoutStats{};
}

// --- Pages that slide (ADR 0128) ----------------------------------------------

void advance(scene::World& world, f32 seconds)
{
    // In pool order, which is creation order (R10); and collected first, since
    // an event is a write to the queue and not to the pool.
    std::vector<core::InstanceId> stopped;
    world.uiPageLayouts().forEach([&](core::InstanceId id, scene::UIPageLayoutComponent& page) {
        if (!page.sliding)
            return;
        page.slideElapsed += std::fmax(0.0f, seconds);
        const f32 progress = page.tweenTime > 0.0f ? std::fmin(1.0f, page.slideElapsed / page.tweenTime) : 1.0f;
        const f32 eased = core::ease(progress, static_cast<core::EasingStyle>(page.easingStyle),
                                     static_cast<core::EasingDirection>(page.easingDirection));
        page.position = page.slideFrom + (page.slideTo - page.slideFrom) * eased;
        if (progress >= 1.0f) {
            page.position = static_cast<f32>(page.index);
            page.sliding = false;
            stopped.push_back(id);
        }
        scene::markUiLayoutDirty(world, id);
    });
    for (const core::InstanceId id : stopped) {
        const scene::UIPageLayoutComponent* page = world.uiPageLayouts().find(id);
        if (page == nullptr || !world.alive(page->currentPage))
            continue;
        world.changes().push(
            scene::Change{scene::ChangeKind::InstanceEvent, id, page->currentPage, world.atoms().intern("Stopped")});
    }

    // **A list that was thrown glides, and one pulled past its end springs
    // back** (G40). The glide slows by a constant fraction a second, which is
    // what makes the distance a flick covers proportional to how fast it was;
    // reaching an end, an elastic frame hands what speed is left to the
    // spring, which bounces and settles. The spring is critically damped and
    // solved exactly rather than stepped, so a long frame cannot make it
    // overshoot or ring.
    const f32 dt = std::fmax(0.0f, seconds);
    std::vector<core::InstanceId> scrolled;
    world.scrollFrames().forEach([&](core::InstanceId id, scene::ScrollFrameComponent& scroll) {
        if (scroll.held || dt <= 0.0f)
            return;
        const scene::UIObjectComponent* object = world.uiObjects().find(id);
        if (object == nullptr)
            return;
        // Per second: the glide keeps e^-2.5 of its speed, so it travels its
        // speed over 2.5; the spring settles in about a third of a second.
        constexpr f32 Friction = 2.5f;
        constexpr f32 Stiffness = 14.0f;
        constexpr f32 StopSpeed = 8.0f;
        constexpr f32 BounceShare = 0.5f;
        const f32 unit = object->unitScale > 0.0f ? object->unitScale : 1.0f;
        const Vec2 view = object->absoluteSize * (1.0f / unit);
        const Vec2 canvas = scroll.absoluteCanvasSize * (1.0f / unit);
        Vec2 position = scroll.canvasPosition;
        bool overscrolled = false;
        for (int axis = 0; axis < 2; ++axis) {
            const f32 room = std::fmax(0.0f, along(canvas, axis) - along(view, axis));
            f32& speed = along(scroll.flingVelocity, axis);
            if (speed != 0.0f) {
                const f32 target = along(position, axis) + speed * dt;
                const f32 settled = std::clamp(target, 0.0f, room);
                along(position, axis) = settled;
                if (settled != target) {
                    if (elasticAlong(scroll, axis, room))
                        along(scroll.overscrollVelocity, axis) = speed * BounceShare;
                    speed = 0.0f;
                }
                speed *= std::exp(-Friction * dt);
                if (std::fabs(speed) < StopSpeed)
                    speed = 0.0f;
            }
            f32& stretch = along(scroll.overscroll, axis);
            f32& stretchSpeed = along(scroll.overscrollVelocity, axis);
            if (stretch != 0.0f || stretchSpeed != 0.0f) {
                const f32 decay = std::exp(-Stiffness * dt);
                const f32 lead = stretchSpeed + Stiffness * stretch;
                stretch = (stretch + lead * dt) * decay;
                stretchSpeed = (stretchSpeed - Stiffness * lead * dt) * decay;
                if (std::fabs(stretch) < 0.25f && std::fabs(stretchSpeed) < StopSpeed) {
                    stretch = 0.0f;
                    stretchSpeed = 0.0f;
                }
                overscrolled = true;
            }
        }
        if (position != scroll.canvasPosition) {
            scroll.canvasPosition = position;
            scrolled.push_back(id);
        }
        else if (overscrolled) {
            scene::markUiLayoutDirty(world, id);
        }
    });
    for (const core::InstanceId id : scrolled) {
        world.changes().push(
            scene::Change{scene::ChangeKind::PropertyChanged, id, {}, world.atoms().intern("CanvasPosition")});
        scene::markUiLayoutDirty(world, id);
    }
}

} // namespace engine::ui
