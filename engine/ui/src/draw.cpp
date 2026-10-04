#include <algorithm>
#include <cmath>
#include <string>

#include "engine/scene/world.h"
#include "engine/ui/ui.h"
#include "scroll_bar.h"
#include "text_field.h"

namespace engine::ui {
namespace {

using core::Rect;
using core::Vec2;

// One element, with everything the sort needs. Flattened before sorting rather
// than sorted in the tree, because `ZIndex` is ONE ordering across the whole
// ScreenGui: a per-parent stacking context is the part of CSS nobody can hold
// in their head, and §2.2 says so.
struct Entry
{
    core::InstanceId id;
    f32 zIndex = 0.0f;
    // Document-order rank, which is what breaks a ZIndex tie. Assigned by the
    // walk, so it is a pure function of the tree (R10).
    u32 order = 0;
    u32 scissor = 0;
    // The accumulated turn -- this element's `Rotation` composed with every
    // turned ancestor's. Carried down the walk beside `scissor`, and for the
    // same reason: both are inherited state that an element cannot compute from
    // itself alone.
    Vec2 turn{1.0f, 0.0f};
    Vec2 turnOffset{0.0f, 0.0f};
    // **A `CanvasGroup`** (ADR 0128): which list of `Collected::groups` holds
    // its descendants, plus one, and the clip they are drawn under inside its
    // picture. Zero is every other element.
    u32 ownsGroup = 0;
    u32 innerScissor = 0;
};

// A tree's entries: its own, and each canvas group's descendants apart --
// `ZIndex` orders a group's contents among themselves, inside its picture.
struct Collected
{
    std::vector<Entry> entries;
    std::vector<std::vector<Entry>> groups;
};

// `Rotation` is degrees CLOCKWISE, and screen Y points down, so the ordinary
// matrix is already the clockwise one: it takes `(1, 0)` to `(cos, sin)`, which
// is rightwards and DOWN.
[[nodiscard]] Vec2 turnAbout(f32 degrees, Vec2 pivot, Vec2& offset)
{
    const f32 radians = degrees * (3.14159265358979323846f / 180.0f);
    const Vec2 axis{std::cos(radians), std::sin(radians)};
    // `t = P - R*P`, which is what makes `p -> R*p + t` fix the pivot.
    offset = Vec2{pivot.x - (axis.x * pivot.x - axis.y * pivot.y), pivot.y - (axis.y * pivot.x + axis.x * pivot.y)};
    return axis;
}

// `(R_a, t_a) . (R_b, t_b)` = `(R_a*R_b, R_a*t_b + t_a)`.
void composeTurn(Vec2 outerAxis, Vec2 outerOffset, Vec2& axis, Vec2& offset)
{
    const Vec2 combined{outerAxis.x * axis.x - outerAxis.y * axis.y, outerAxis.y * axis.x + outerAxis.x * axis.y};
    offset = Vec2{outerAxis.x * offset.x - outerAxis.y * offset.y + outerOffset.x,
                  outerAxis.y * offset.x + outerAxis.x * offset.y + outerOffset.y};
    axis = combined;
}

// --- Gradients and strokes (ADR 0110) -------------------------------------------

// A `UIGradient` as the quads it colours carry it: its row in the frame's
// table and where it lies. `slot` zero is no gradient.
struct GradientPaint
{
    u32 slot = 0;
    u32 type = 0;
    u32 tile = 0;
    f32 angle = 0.0f;
    f32 scale = 1.0f;
    Vec2 offset{};
};

// The first enabled `UIGradient` under `holder` -- an element or a stroke --
// laid over `box`. The first counts and the rest do not, which is the ADR's
// "one parent, one gradient".
[[nodiscard]] GradientPaint gradientUnder(const scene::World& world, core::InstanceId holder, const Rect& box,
                                          DrawList& out)
{
    GradientPaint paint;
    for (core::InstanceId child = world.firstChild(holder); child.valid(); child = world.nextSibling(child)) {
        const scene::UIGradientComponent* gradient = world.uiGradients().find(child);
        if (gradient == nullptr)
            continue;
        if (!gradient->enabled)
            return paint;
        paint.slot = out.gradientSlot(DrawGradient{gradient->color, gradient->transparency});
        paint.type = static_cast<u32>(std::clamp(gradient->type, 0, 2));
        paint.tile = static_cast<u32>(std::clamp(gradient->tileMode, 0, 2));
        paint.angle = gradient->rotation * (3.14159265358979323846f / 180.0f);
        paint.scale = gradient->scale;
        paint.offset = Vec2{gradient->offset.x * (box.max.x - box.min.x), gradient->offset.y * (box.max.y - box.min.y)};
        return paint;
    }
    return paint;
}

void applyGradient(DrawQuad& quad, const GradientPaint& paint, const Rect& box)
{
    quad.gradient = paint.slot;
    quad.gradientBox = box;
    quad.gradientType = paint.type;
    quad.gradientTile = paint.tile;
    quad.gradientAngle = paint.angle;
    quad.gradientScale = paint.scale;
    quad.gradientOffset = paint.offset;
}

// One `UIStroke` under an element, with the child order that breaks a `ZIndex`
// tie.
struct StrokeEntry
{
    core::InstanceId id;
    const scene::UIStrokeComponent* stroke = nullptr;
    u32 order = 0;
};

// How many pixels one of an element's units is (`UIObjectComponent::unitScale`).
[[nodiscard]] f32 unitScaleOf(const scene::World& world, core::InstanceId id) noexcept
{
    const scene::UIObjectComponent* object = world.uiObjects().find(id);
    return object != nullptr ? object->unitScale : 1.0f;
}

// Everything an element owes after its own drawing: its gradient over what it
// drew, then its border strokes in `ZIndex` order. A destructor, like the turn
// stamp beside it, because `emit` leaves by four doors.
struct AppearanceStamp
{
    const scene::World& world;
    DrawList& out;
    core::usize first = 0;
    Rect box;
    f32 cornerRadius = 0.0f;
    u32 scissor = 0;
    GradientPaint gradient;
    std::vector<StrokeEntry> borders;
    // The element's units, in pixels (`UIObjectComponent::unitScale`).
    f32 unitScale = 1.0f;

    ~AppearanceStamp()
    {
        for (core::usize index = first; index < out.quads.size(); ++index) {
            DrawQuad& quad = out.quads[index];
            // An outline carries its stroke's gradient, over the same box.
            if (quad.outline) {
                if (quad.gradient != 0)
                    quad.gradientBox = box;
                continue;
            }
            if (gradient.slot != 0)
                applyGradient(quad, gradient, box);
        }

        const f32 shorter = std::fmin(box.max.x - box.min.x, box.max.y - box.min.y);
        for (const StrokeEntry& entry : borders) {
            const scene::UIStrokeComponent& stroke = *entry.stroke;
            const f32 thickness =
                stroke.strokeSizingMode == 1 ? stroke.thickness * shorter : stroke.thickness * unitScale;
            const f32 alpha = 1.0f - std::fmin(std::fmax(stroke.transparency, 0.0f), 1.0f);
            if (!(thickness > 0.0f) || !(alpha > 0.0f))
                continue;
            // Where the band lies, in pixels from the edge, outwards positive.
            const f32 offset = stroke.borderOffset.scale * shorter + stroke.borderOffset.offset * unitScale;
            f32 inner = offset;
            if (stroke.borderStrokePosition == 1)
                inner = offset - thickness * 0.5f;
            else if (stroke.borderStrokePosition == 2)
                inner = offset - thickness;
            const f32 outer = inner + thickness;

            // The quad covers the band's outside and one pixel more, for the
            // soft edge; the band inside the box is inside it anyway.
            const f32 grow = std::fmax(outer, 0.0f) + 1.0f;
            DrawQuad band;
            band.min = Vec2{box.min.x - grow, box.min.y - grow};
            band.max = Vec2{box.max.x + grow, box.max.y + grow};
            band.color = stroke.color;
            band.alpha = alpha;
            band.scissor = scissor;
            band.cornerRadius = cornerRadius;
            band.borderStroke = true;
            band.strokeBox = box;
            band.strokeInner = inner;
            band.strokeOuter = outer;
            band.strokeJoin = static_cast<u32>(std::clamp(stroke.lineJoinMode, 0, 2));
            const Rect reach{band.min, band.max};
            if (const GradientPaint own = gradientUnder(world, entry.id, reach, out); own.slot != 0)
                applyGradient(band, own, reach);
            out.quads.push_back(band);
        }
    }
};

// --- Images ------------------------------------------------------------------

// Set by the app, which is the only thing that can see both a content mount and
// a GPU. Null is the ordinary state of a test and of a headless run with no
// content: every image then draws as its flat tint.
ImageProvider g_imageProvider = nullptr;
void* g_imageProviderUser = nullptr;

[[nodiscard]] bool resolveImage(std::string_view urn, ResolvedImage& out)
{
    if (g_imageProvider == nullptr) {
        return false;
    }
    return g_imageProvider(g_imageProviderUser, urn, out) && out.texture != 0 && out.width > 0 && out.height > 0;
}

// One textured quad, in box pixels and source pixels.
//
// Source pixels rather than normalised UVs at every call site, because every
// rule below -- a nine-slice cut, a tile step, a letterbox -- is stated in the
// picture's own pixels, and converting once here is what keeps them readable.
void pushImageQuad(core::Rect box, core::Rect source, const ResolvedImage& image, core::Color3 tint, u32 scissor,
                   f32 cornerRadius, std::vector<DrawQuad>& out)
{
    if (box.max.x <= box.min.x || box.max.y <= box.min.y) {
        return;
    }
    DrawQuad quad;
    quad.min = box.min;
    quad.max = box.max;
    quad.uvMin = Vec2{source.min.x / static_cast<f32>(image.width), source.min.y / static_cast<f32>(image.height)};
    quad.uvMax = Vec2{source.max.x / static_cast<f32>(image.width), source.max.y / static_cast<f32>(image.height)};
    quad.color = tint;
    quad.alpha = 1.0f;
    quad.texture = image.texture;
    quad.scissor = scissor;
    quad.cornerRadius = cornerRadius;
    out.push_back(quad);
}

// Nine-slice. The four corners keep their own size, the four edges stretch along
// one axis, and the middle stretches along both -- which is how a panel keeps
// its rounded corners at any size.
void appendSlice(core::Rect box, const ResolvedImage& image, core::Rect centre, core::Color3 tint, u32 scissor,
                 std::vector<DrawQuad>& out)
{
    const auto width = static_cast<f32>(image.width);
    const auto height = static_cast<f32>(image.height);

    // The cuts, in source pixels, clamped into the picture. An inverted or
    // out-of-range `SliceCenter` is the caller's to see: its own doc says it is
    // kept as given rather than corrected, so this clamps only enough to keep
    // the arithmetic from producing negative rectangles.
    const f32 left = std::fmin(std::fmax(centre.min.x, 0.0f), width);
    const f32 top = std::fmin(std::fmax(centre.min.y, 0.0f), height);
    const f32 right = std::fmin(std::fmax(centre.max.x, left), width);
    const f32 bottom = std::fmin(std::fmax(centre.max.y, top), height);

    // Source columns and rows.
    const f32 sx[4] = {0.0f, left, right, width};
    const f32 sy[4] = {0.0f, top, bottom, height};

    // Destination columns and rows. The corners take their source size; the
    // middle takes whatever is left, and never less than nothing -- a box
    // narrower than its own two corners collapses the middle rather than
    // drawing the corners on top of each other.
    const f32 boxWidth = box.max.x - box.min.x;
    const f32 boxHeight = box.max.y - box.min.y;
    const f32 leftEdge = std::fmin(left, boxWidth);
    const f32 rightEdge = std::fmin(width - right, std::fmax(boxWidth - leftEdge, 0.0f));
    const f32 topEdge = std::fmin(top, boxHeight);
    const f32 bottomEdge = std::fmin(height - bottom, std::fmax(boxHeight - topEdge, 0.0f));

    const f32 dx[4] = {box.min.x, box.min.x + leftEdge, box.max.x - rightEdge, box.max.x};
    const f32 dy[4] = {box.min.y, box.min.y + topEdge, box.max.y - bottomEdge, box.max.y};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            pushImageQuad(core::Rect{{dx[column], dy[row]}, {dx[column + 1], dy[row + 1]}},
                          core::Rect{{sx[column], sy[row]}, {sx[column + 1], sy[row + 1]}}, image, tint, scissor,
                          // No rounding on a slice: the picture is what supplies
                          // the corner, which is the entire point of using one.
                          0.0f, out);
        }
    }
}

// Repeated at its own size until the box is full, clipped at the far edges.
void appendTile(core::Rect box, const ResolvedImage& image, core::Color3 tint, u32 scissor, std::vector<DrawQuad>& out)
{
    const auto width = static_cast<f32>(image.width);
    const auto height = static_cast<f32>(image.height);

    // Bounded, and the bound is a real decision: a one-pixel picture tiled over
    // a full-screen frame is nearly a million quads, and the frame that built
    // them would be the hitch. Past the cap the picture stretches instead, which
    // is visibly wrong in a way somebody can find -- unlike a frame that simply
    // stops responding.
    constexpr int MaxTiles = 4096;
    const f32 columns = std::ceil((box.max.x - box.min.x) / std::fmax(width, 1.0f));
    const f32 rows = std::ceil((box.max.y - box.min.y) / std::fmax(height, 1.0f));
    if (columns * rows > static_cast<f32>(MaxTiles)) {
        pushImageQuad(box, core::Rect{{0.0f, 0.0f}, {width, height}}, image, tint, scissor, 0.0f, out);
        return;
    }

    for (f32 y = box.min.y; y < box.max.y; y += height) {
        for (f32 x = box.min.x; x < box.max.x; x += width) {
            // The last tile in a row or column is CUT rather than overhanging:
            // the source rectangle shrinks with the destination, so the picture
            // is clipped at its own resolution instead of being squashed.
            const f32 visibleWidth = std::fmin(width, box.max.x - x);
            const f32 visibleHeight = std::fmin(height, box.max.y - y);
            pushImageQuad(core::Rect{{x, y}, {x + visibleWidth, y + visibleHeight}},
                          core::Rect{{0.0f, 0.0f}, {visibleWidth, visibleHeight}}, image, tint, scissor, 0.0f, out);
        }
    }
}

void appendImageQuads(core::Rect box, const ResolvedImage& image, bool ready, i32 scaleType, core::Rect sliceCenter,
                      core::Color3 tint, u32 scissor, f32 cornerRadius, std::vector<DrawQuad>& out,
                      core::Vec2 rectOffset = {}, core::Vec2 rectSize = {})
{
    if (!ready) {
        // The flat tint, which is what M6 drew for every image and is now what a
        // picture looks like while it is still arriving.
        DrawQuad quad;
        quad.min = box.min;
        quad.max = box.max;
        quad.color = tint;
        quad.alpha = 1.0f;
        quad.scissor = scissor;
        quad.cornerRadius = cornerRadius;
        out.push_back(quad);
        return;
    }

    switch (scaleType) {
    case 1:
        appendSlice(box, image, sliceCenter, tint, scissor, out);
        return;
    case 2:
        appendTile(box, image, tint, scissor, out);
        return;
    case 3:
    case 4: {
        // **Fit and Crop** (G8): the picture keeps its shape. Fit scales it to
        // the smaller of the two ratios and centres it in the box; Crop to the
        // larger, fills the box, and takes the overhang off the source --
        // equally from both sides, so the middle of the picture is what shows.
        core::Rect source{{0.0f, 0.0f}, {static_cast<f32>(image.width), static_cast<f32>(image.height)}};
        if (rectSize.x != 0.0f && rectSize.y != 0.0f)
            source = core::Rect{rectOffset, Vec2{rectOffset.x + rectSize.x, rectOffset.y + rectSize.y}};
        const f32 sourceWidth = std::fabs(source.max.x - source.min.x);
        const f32 sourceHeight = std::fabs(source.max.y - source.min.y);
        const f32 boxWidth = box.max.x - box.min.x;
        const f32 boxHeight = box.max.y - box.min.y;
        if (!(sourceWidth > 0.0f) || !(sourceHeight > 0.0f) || !(boxWidth > 0.0f) || !(boxHeight > 0.0f))
            return;
        const f32 across = boxWidth / sourceWidth;
        const f32 down = boxHeight / sourceHeight;
        if (scaleType == 3) {
            const f32 scale = std::fmin(across, down);
            const Vec2 size{sourceWidth * scale, sourceHeight * scale};
            const Vec2 corner{box.min.x + (boxWidth - size.x) * 0.5f, box.min.y + (boxHeight - size.y) * 0.5f};
            pushImageQuad(core::Rect{corner, Vec2{corner.x + size.x, corner.y + size.y}}, source, image, tint, scissor,
                          cornerRadius, out);
            return;
        }
        const f32 scale = std::fmax(across, down);
        // What of the source the box shows, in the source's own direction.
        const f32 keepX = (boxWidth / scale) / sourceWidth;
        const f32 keepY = (boxHeight / scale) / sourceHeight;
        const Vec2 span{source.max.x - source.min.x, source.max.y - source.min.y};
        const Vec2 cut{span.x * (1.0f - keepX) * 0.5f, span.y * (1.0f - keepY) * 0.5f};
        const core::Rect shown{Vec2{source.min.x + cut.x, source.min.y + cut.y},
                               Vec2{source.max.x - cut.x, source.max.y - cut.y}};
        pushImageQuad(box, shown, image, tint, scissor, cornerRadius, out);
        return;
    }
    default: {
        // Stretch: the whole picture into the whole box, aspect ratio and all
        // -- or, with `ImageRectSize`, that part of it. A negative size reads
        // backwards, which the UVs do by themselves: a larger start than end.
        core::Rect source{{0.0f, 0.0f}, {static_cast<f32>(image.width), static_cast<f32>(image.height)}};
        if (rectSize.x != 0.0f && rectSize.y != 0.0f)
            source = core::Rect{rectOffset, Vec2{rectOffset.x + rectSize.x, rectOffset.y + rectSize.y}};
        pushImageQuad(box, source, image, tint, scissor, cornerRadius, out);
        return;
    }
    }
}

// --- Scroll bars -------------------------------------------------------------

// The bar for one axis, appended to `out`, where `scrollBarShape` says it is.
//
// **Drawn with the PARENT's scissor rather than the region's own**, which is the
// one thing about a scroll bar that is easy to get wrong: a `ScrollFrame` clips
// its descendants, and a bar clipped by that clip would be scrolled away by the
// content it is reporting on.
void appendScrollBar(const ScrollBarShape& shape, f32 thickness, core::Color3 color, f32 alpha, u32 scissor,
                     std::vector<DrawQuad>& out)
{
    if (!shape.shown || alpha <= 0.0f)
        return;

    // A quarter of the thumb's, so the track reads as a groove rather than as
    // a second panel.
    DrawQuad trackQuad;
    trackQuad.min = shape.track.min;
    trackQuad.max = shape.track.max;
    trackQuad.color = color;
    trackQuad.alpha = 0.25f * alpha;
    trackQuad.scissor = scissor;
    out.push_back(trackQuad);

    DrawQuad thumb;
    thumb.min = shape.thumb.min;
    thumb.max = shape.thumb.max;
    thumb.color = color;
    thumb.alpha = alpha;
    thumb.scissor = scissor;
    // Rounded to half its thickness, which is a capsule. The one piece of
    // styling here, and it costs nothing: `UICorner`'s distance field is on
    // every quad already (D030).
    thumb.cornerRadius = thickness * 0.5f;
    out.push_back(thumb);
}

void collect(const scene::World& world, core::InstanceId id, u32 scissor, Vec2 turn, Vec2 turnOffset,
             std::vector<Entry>& entries, std::vector<Rect>& scissors, std::vector<std::vector<Entry>>* groups)
{
    const scene::UIObjectComponent* self = world.uiObjects().find(id);
    if (self == nullptr || !self->visible)
        return;

    // The pivot is the ANCHOR POINT, in window pixels: an element anchored at
    // its centre spins in place and one anchored at a corner swings from it.
    // Taken from the UNROTATED box, which is what makes `Rotation` a pure
    // drawing property -- the layout that produced `absolutePosition` never saw
    // the turn, and the next frame's layout will not either.
    if (self->rotation != 0.0f) {
        const Vec2 pivot{self->absolutePosition.x + self->anchorPoint.x * self->absoluteSize.x,
                         self->absolutePosition.y + self->anchorPoint.y * self->absoluteSize.y};
        Vec2 ownOffset{};
        Vec2 ownAxis = turnAbout(self->rotation, pivot, ownOffset);
        composeTurn(turn, turnOffset, ownAxis, ownOffset);
        turn = ownAxis;
        turnOffset = ownOffset;
    }

    entries.push_back(Entry{id, self->zIndex, static_cast<u32>(entries.size()), scissor, turn, turnOffset, 0, 0});

    // **A `CanvasGroup`** (ADR 0128): what is inside is drawn into its picture,
    // upright and clipped to its box alone -- the turn and the clip it is under
    // are the picture's, applied once when the picture is shown.
    //
    // **On the screen only.** A world canvas -- a `SurfaceGui`, a
    // `BillboardGui` -- is itself drawn in the world's pass, which has no
    // pictures to draw into: `groups` is null there, and a group is a frame.
    if (groups != nullptr && world.canvasGroups().find(id) != nullptr && self->absoluteSize.x >= 1.0f &&
        self->absoluteSize.y >= 1.0f) {
        scissors.push_back(Rect{self->absolutePosition, self->absolutePosition + self->absoluteSize});
        const u32 inner = static_cast<u32>(scissors.size() - 1);
        // Into a list of its own first: `groups` grows while a group inside
        // this one is collected.
        std::vector<Entry> inside;
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
            collect(world, child, inner, Vec2{1.0f, 0.0f}, Vec2{}, inside, scissors, groups);
        groups->push_back(std::move(inside));
        Entry& own = entries.back();
        own.ownsGroup = static_cast<u32>(groups->size());
        own.innerScissor = inner;
        return;
    }

    u32 childScissor = scissor;
    // A ScrollFrame clips whatever `ClipsDescendants` says: a scrolling region
    // that did not clip would not be one.
    const bool clips = self->clipsDescendants || world.scrollFrames().find(id) != nullptr;
    if (clips) {
        const Rect own{self->absolutePosition, self->absolutePosition + self->absoluteSize};
        const Rect& outer = scissors[scissor];
        // Intersected with what is already in force, so a clip inside a clip
        // narrows rather than widening: a child that escaped its grandparent's
        // clip is the classic scrolling-list defect.
        scissors.push_back(Rect{Vec2{std::fmax(own.min.x, outer.min.x), std::fmax(own.min.y, outer.min.y)},
                                Vec2{std::fmin(own.max.x, outer.max.x), std::fmin(own.max.y, outer.max.y)}});
        childScissor = static_cast<u32>(scissors.size() - 1);
    }

    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
        collect(world, child, childScissor, turn, turnOffset, entries, scissors, groups);
}

// **A text field** (ADR 0139): its text as the field shows it -- masked,
// with a composition at the caret -- in lines that scroll inside the field and
// are clipped to it, the selection behind them, the composition underlined and
// the caret, all placed by the same view the interaction hit-tests against.
void emitField(const scene::World& world, const Entry& entry, const scene::TextLabelComponent& label,
               const scene::TextInputComponent& field, core::Rect box, f32 textAlpha, const TextStroke& stroke,
               DrawList& out)
{
    const Rect outer = out.scissors[entry.scissor];
    out.scissors.push_back(Rect{Vec2{std::fmax(box.min.x, outer.min.x), std::fmax(box.min.y, outer.min.y)},
                                Vec2{std::fmin(box.max.x, outer.max.x), std::fmin(box.max.y, outer.max.y)}});
    const u32 scissor = static_cast<u32>(out.scissors.size() - 1);

    // An empty field that is not being typed into shows its placeholder.
    if (label.text.empty() && field.composition.empty() && !field.focused) {
        if (!field.placeholderText.empty())
            buildTextGeometry(field.placeholderText, label.font, label.textSize * unitScaleOf(world, entry.id),
                              field.multiLine ? box.max.x - box.min.x : 0.0f, box, label.horizontalAlignment,
                              label.verticalAlignment, field.placeholderColor, textAlpha, scissor, out.quads);
        return;
    }

    const FieldView view = fieldView(world, entry.id);
    const std::string_view display = view.display;
    const auto xOf = [&](const TextLine& line, core::usize at) {
        const core::usize end = std::min<core::usize>(at, line.end);
        return lineLeft(view, line) + textWidth(display.substr(line.begin, end - line.begin), view.font, view.size);
    };
    const auto flat = [&](Vec2 min, Vec2 max, core::Color3 color, f32 alpha) {
        DrawQuad quad;
        quad.min = min;
        quad.max = max;
        quad.color = color;
        quad.alpha = alpha;
        quad.scissor = scissor;
        out.quads.push_back(quad);
    };

    // The selection, behind the text.
    if (field.focused && field.caret != field.anchor) {
        const core::u32 from = displayOffset(view, std::min(field.caret, field.anchor));
        const core::u32 to = displayOffset(view, std::max(field.caret, field.anchor));
        for (core::usize index = 0; index < view.lines.size(); ++index) {
            const TextLine& line = view.lines[index];
            if (to <= line.begin || from > line.end)
                continue;
            f32 left = xOf(line, std::max<core::usize>(from, line.begin));
            f32 right = xOf(line, std::min<core::usize>(to, line.end));
            // A selected line break shows as a sliver past the line's end.
            if (to > line.end)
                right += view.size * 0.3f;
            if (right <= left)
                continue;
            const f32 top = view.top + static_cast<f32>(index) * view.lineHeight;
            flat(Vec2{left, top}, Vec2{right, top + view.lineHeight}, core::Color3{0.22f, 0.46f, 0.92f}, 0.45f);
        }
    }

    // The text, a line at a time where the view put each line.
    for (core::usize index = 0; index < view.lines.size(); ++index) {
        const TextLine& line = view.lines[index];
        if (line.end <= line.begin)
            continue;
        const f32 left = lineLeft(view, line);
        const f32 top = view.top + static_cast<f32>(index) * view.lineHeight;
        buildTextGeometry(display.substr(line.begin, line.end - line.begin), view.font, view.size, 0.0f,
                          Rect{Vec2{left, top}, Vec2{left + line.width + 1.0f, top + view.lineHeight}}, 0, 0,
                          label.textColor, textAlpha, scissor, out.quads, stroke);
    }

    // What an input method is composing, underlined.
    if (view.compositionEnd > view.compositionBegin) {
        for (core::usize index = 0; index < view.lines.size(); ++index) {
            const TextLine& line = view.lines[index];
            if (view.compositionEnd <= line.begin || view.compositionBegin >= line.end)
                continue;
            const f32 left = xOf(line, std::max<core::usize>(view.compositionBegin, line.begin));
            const f32 right = xOf(line, std::min<core::usize>(view.compositionEnd, line.end));
            const f32 bottom = view.top + static_cast<f32>(index + 1) * view.lineHeight;
            flat(Vec2{left, bottom - 1.5f}, Vec2{right, bottom}, label.textColor, 1.0f);
        }
    }

    // The caret, where the next character goes, blinking.
    if (field.focused && field.caretVisible) {
        const core::u32 at = field.composition.empty() ? displayOffset(view, field.caret)
                                                       : view.compositionBegin + field.compositionCursor;
        const Vec2 point = caretPoint(view, at);
        // A hair over one pixel, so it is visible at every scale without being
        // a glyph in its own right.
        constexpr f32 CaretWidth = 1.5f;
        flat(point, Vec2{point.x + CaretWidth, point.y + view.lineHeight}, label.textColor, 1.0f);
    }
}

// `over` draws the element over another box than its own: a selection image,
// which has no place of its own.
void emit(const scene::World& world, const Entry& entry, DrawList& out, const Rect* over = nullptr)
{
    const scene::UIObjectComponent* self = world.uiObjects().find(entry.id);
    if (self == nullptr)
        return;

    // **Stamped over the range at the end rather than set at each push.** One
    // element emits a background, two scroll bars, a picture, a glyph per
    // character and possibly a caret, through four functions -- and a turn that
    // had to be remembered at every one of those sites is a turn that will be
    // forgotten at the next one somebody adds.
    struct TurnStamp
    {
        DrawList& list;
        const Entry& entry;
        core::usize first;

        ~TurnStamp()
        {
            if (entry.turn.x == 1.0f && entry.turn.y == 0.0f)
                return;
            for (core::usize index = first; index < list.quads.size(); ++index) {
                list.quads[index].turn = entry.turn;
                list.quads[index].turnOffset = entry.turnOffset;
            }
        }
    };
    // A destructor rather than a line at the bottom, because `emit` has four
    // early returns and three of them are past the first quad it pushed.
    const TurnStamp stamp{out, entry, out.quads.size()};

    const Rect box =
        over != nullptr ? *over : Rect{self->absolutePosition, self->absolutePosition + self->absoluteSize};
    const f32 backgroundAlpha = 1.0f - self->backgroundTransparency;

    // A fully transparent background still lays out and still hit-tests; it
    // just does not draw. Skipping the quad rather than submitting an invisible
    // one is the difference between a HUD that costs nothing and one that costs
    // a draw per element.
    // `UICorner`, if the element has one (D030). Clamped to half the shorter
    // side, because a radius past that is a circle and anything beyond it is
    // arithmetic with no meaning -- and clamping here rather than in the shader
    // keeps the fragment stage a distance function with no special cases.
    f32 cornerRadius = 0.0f;
    for (core::InstanceId child = world.firstChild(entry.id); child.valid(); child = world.nextSibling(child)) {
        if (const scene::UICornerComponent* corner = world.uiCorners().find(child); corner != nullptr) {
            const f32 width = box.max.x - box.min.x;
            const f32 height = box.max.y - box.min.y;
            const f32 shorter = std::fmin(width, height);
            // A `UDim`, so `Scale` is a fraction of the SHORTER side: a radius
            // that meant a fraction of the width would make a wide button's
            // corners taller than its height.
            const f32 radius = corner->cornerRadius.scale * shorter + corner->cornerRadius.offset * self->unitScale;
            cornerRadius = std::fmax(0.0f, std::fmin(radius, shorter * 0.5f));
            break;
        }
    }

    // **A gradient over everything the element draws, and its strokes after**
    // (ADR 0110). A text object's contextual stroke is the text's outline and
    // goes to the glyphs below; every other stroke is a border.
    const bool isText = world.textLabels().find(entry.id) != nullptr;
    AppearanceStamp appearance{world, out, out.quads.size(), box, cornerRadius, entry.scissor, {}, {}};
    appearance.unitScale = self->unitScale;
    appearance.gradient = gradientUnder(world, entry.id, box, out);
    TextStroke textStroke;
    bool textStroked = false;
    u32 strokeOrder = 0;
    for (core::InstanceId child = world.firstChild(entry.id); child.valid(); child = world.nextSibling(child)) {
        const scene::UIStrokeComponent* stroke = world.uiStrokes().find(child);
        if (stroke == nullptr || !stroke->enabled)
            continue;
        if (isText && stroke->applyStrokeMode == 0) {
            // One text stroke: the first.
            if (textStroked)
                continue;
            textStroked = true;
            textStroke.scaled = stroke->strokeSizingMode == 1;
            textStroke.thickness = textStroke.scaled ? stroke->thickness : stroke->thickness * self->unitScale;
            textStroke.color = stroke->color;
            textStroke.alpha = 1.0f - std::fmin(std::fmax(stroke->transparency, 0.0f), 1.0f);
            textStroke.join = static_cast<u32>(std::clamp(stroke->lineJoinMode, 0, 2));
            if (const GradientPaint own = gradientUnder(world, child, box, out); own.slot != 0) {
                textStroke.gradient = own.slot;
                textStroke.gradientType = own.type;
                textStroke.gradientTile = own.tile;
                textStroke.gradientAngle = own.angle;
                textStroke.gradientScale = own.scale;
                textStroke.gradientOffset = own.offset;
            }
            continue;
        }
        appearance.borders.push_back(StrokeEntry{child, stroke, strokeOrder++});
    }
    std::stable_sort(appearance.borders.begin(), appearance.borders.end(),
                     [](const StrokeEntry& a, const StrokeEntry& b) { return a.stroke->zIndex < b.stroke->zIndex; });

    if (backgroundAlpha > 0.0f) {
        DrawQuad quad;
        quad.min = box.min;
        quad.max = box.max;
        quad.color = self->backgroundColor;
        quad.alpha = backgroundAlpha;
        quad.scissor = entry.scissor;
        quad.cornerRadius = cornerRadius;
        out.quads.push_back(quad);
    }

    if (const scene::ScrollFrameComponent* scroll = world.scrollFrames().find(entry.id); scroll != nullptr) {
        // The bars, on the entry's OWN scissor -- which is the parent's clip,
        // not the region's. A `ScrollFrame` clips its descendants, and a bar
        // clipped by that clip would scroll away with the content it reports on.
        // The canvas the layout settled on is in pixels, like the box; its
        // position and the bar's thickness are in the tree's units.
        //
        // **The view is the one the layout settled on, not the box measured
        // again** (D547). The canvas is never smaller than the view, and where
        // there is nothing to scroll the layout makes them the same number.
        // The box's far edge less its near one is that number rounded twice
        // more -- a hair smaller at some window sizes, at others not -- and a
        // canvas a hair wider than its view is a bar the length of the frame:
        // a list that scrolls down grew one across its bottom when its window
        // was resized. **And an axis no hand scrolls has no bar**
        // (`ScrollingDirection`), whatever the canvas says.
        const f32 unit = self->unitScale;
        const core::Vec2 size = scroll->absoluteWindowSize;
        const core::Vec2 canvas = scroll->absoluteCanvasSize;
        const f32 thickness = scroll->scrollBarThickness * unit;
        const f32 alpha = 1.0f - std::clamp(scroll->scrollBarImageTransparency, 0.0f, 1.0f);
        if (scrollsAlong(*scroll, 1)) {
            appendScrollBar(scrollBarShape(box, canvas.y, size.y, scroll->canvasPosition.y * unit, thickness, true),
                            thickness, scroll->scrollBarImageColor, alpha, entry.scissor, out.quads);
        }
        if (scrollsAlong(*scroll, 0)) {
            appendScrollBar(scrollBarShape(box, canvas.x, size.x, scroll->canvasPosition.x * unit, thickness, false),
                            thickness, scroll->scrollBarImageColor, alpha, entry.scissor, out.quads);
        }
    }

    if (const scene::ImageLabelComponent* image = world.imageLabels().find(entry.id); image != nullptr) {
        if (!image->image.empty()) {
            // A picture the provider cannot resolve -- not loaded, or a URI that
            // names nothing -- draws as the flat tint. That is what an image
            // still arriving looks like, and it is better than a hole.
            ResolvedImage resolved;
            const bool ready = resolveImage(image->image, resolved);
            // **The picture's own see-through** (`ImageTransparency`, D453):
            // an icon fades and its box stays, as a label's words do.
            const f32 shown = 1.0f - std::clamp(image->imageTransparency, 0.0f, 1.0f);
            if (shown > 0.0f) {
                const usize first = out.quads.size();
                appendImageQuads(box, resolved, ready, image->scaleType, image->sliceCenter, image->imageColor,
                                 entry.scissor, cornerRadius, out.quads, image->imageRectOffset, image->imageRectSize);
                for (usize at = first; at < out.quads.size(); ++at)
                    out.quads[at].alpha *= shown;
            }
        }
        return;
    }
    // **A `ViewportFrame`'s picture** (ADR 0107): what the host drew of the
    // instances inside it, under the name it registers for the frame. Nothing
    // until the first picture -- the frame's own background shows, which is
    // what an empty slot looks like.
    if (world.viewportFrames().find(entry.id) != nullptr) {
        ResolvedImage resolved;
        const std::string name = "view://#" + std::to_string(entry.id.index);
        if (resolveImage(name, resolved))
            appendImageQuads(box, resolved, true, 0, core::Rect{}, core::Color3{1.0f, 1.0f, 1.0f}, entry.scissor,
                             cornerRadius, out.quads);
        return;
    }
    if (const scene::TextLabelComponent* label = world.textLabels().find(entry.id); label != nullptr) {
        std::string_view text = label->text;
        core::Color3 color = label->textColor;
        // `TextTransparency`: the words' own see-through, clamped here because
        // the property keeps what was written.
        const f32 textAlpha = 1.0f - std::fmin(1.0f, std::fmax(0.0f, label->textTransparency));
        // **Markup, when the label asks for it** -- and never in a field being
        // typed into, whose caret counts the characters of what is written, tags
        // and all. A placeholder is plain for the same reason.
        if (const scene::TextInputComponent* field = world.textInputs().find(entry.id); field != nullptr) {
            emitField(world, entry, *label, *field, box, textAlpha, textStroke, out);
            return;
        }
        const bool rich = label->richText;
        // Nothing to draw. A `TextInput`'s placeholder is `emitField`'s.
        if (text.empty())
            return;

        // `TextScaled` re-measures at the size that fills the box rather than
        // stretching a bitmap: there is no distance field in v1, and a stretched
        // glyph is what "scaled text" usually looks like.
        f32 size = label->textSize * self->unitScale;
        if (label->textScaled && !text.empty()) {
            const TextRunMetrics unit =
                rich ? measureRichText(text, label->font, 100.0f, 0.0f) : measureText(text, label->font, 100.0f, 0.0f);
            if (unit.size.x > 0.0f && unit.size.y > 0.0f) {
                const f32 fits =
                    100.0f * std::fmin(self->absoluteSize.x / unit.size.x, self->absoluteSize.y / unit.size.y);
                size = scaledTextSize(fits);
            }
            // **Between two sizes, when a `UITextSizeConstraint` says so**
            // (ADR 0128), in the element's units like `TextSize` itself.
            for (core::InstanceId child = world.firstChild(entry.id); child.valid(); child = world.nextSibling(child)) {
                if (const scene::UITextSizeConstraintComponent* limits = world.uiTextSizeConstraints().find(child);
                    limits != nullptr) {
                    const f32 least = limits->minTextSize * self->unitScale;
                    const f32 most = std::fmax(least, limits->maxTextSize * self->unitScale);
                    size = std::clamp(size, least, most);
                    break;
                }
            }
        }

        // **Text the box does not hold** (ADR 0168): shown past it, cut at
        // it, or cut short and ended with an ellipsis -- by what the font
        // measures, at the size it is drawn. Scaled text fits by what it is.
        // Markup has no one place to end, so under it an ellipsis is a cut.
        const f32 wrapWidth = label->textWrapped ? self->absoluteSize.x : 0.0f;
        u32 scissor = entry.scissor;
        std::string ended;
        if (label->textOverflow != 0 && !label->textScaled) {
            if (label->textOverflow == 2 && !rich) {
                ended = ellipsizedText(text, label->font, size, wrapWidth, self->absoluteSize);
                text = ended;
            }
            else {
                const Rect outer = out.scissors[entry.scissor];
                out.scissors.push_back(
                    Rect{Vec2{std::fmax(box.min.x, outer.min.x), std::fmax(box.min.y, outer.min.y)},
                         Vec2{std::fmin(box.max.x, outer.max.x), std::fmin(box.max.y, outer.max.y)}});
                scissor = static_cast<u32>(out.scissors.size() - 1);
            }
        }

        if (rich)
            buildRichTextGeometry(text, label->font, size, wrapWidth, box, label->horizontalAlignment,
                                  label->verticalAlignment, color, textAlpha, scissor, out.quads, textStroke);
        else
            buildTextGeometry(text, label->font, size, wrapWidth, box, label->horizontalAlignment,
                              label->verticalAlignment, color, textAlpha, scissor, out.quads, textStroke);
    }
}

// --- Canvas groups and the selection (ADR 0128) -------------------------------

// FNV-1a over the fields that decide what a quad draws. Field by field: a
// struct's padding is whatever was in memory (D474).
struct Signature
{
    u64 value = 1469598103934665603ull;

    template <typename T>
    void add(const T& field) noexcept
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&field);
        for (usize index = 0; index < sizeof(T); ++index) {
            value ^= bytes[index];
            value *= 1099511628211ull;
        }
    }

    void add(Vec2 point) noexcept
    {
        add(point.x);
        add(point.y);
    }

    void add(const Rect& rect) noexcept
    {
        add(rect.min);
        add(rect.max);
    }
};

[[nodiscard]] u64 signatureOf(const DrawList& list, usize first, usize end)
{
    Signature signature;
    signature.add(glyphAtlas().version);
    for (usize index = first; index < end; ++index) {
        const DrawQuad& quad = list.quads[index];
        signature.add(quad.min);
        signature.add(quad.max);
        signature.add(quad.uvMin);
        signature.add(quad.uvMax);
        signature.add(quad.color.r);
        signature.add(quad.color.g);
        signature.add(quad.color.b);
        signature.add(quad.alpha);
        signature.add(quad.texture);
        signature.add(list.scissors[quad.scissor]);
        signature.add(quad.cornerRadius);
        signature.add(quad.turn);
        signature.add(quad.turnOffset);
        signature.add(quad.slant);
        signature.add(quad.gradient);
        signature.add(quad.borderStroke);
        signature.add(quad.strokeBox);
        signature.add(quad.strokeInner);
        signature.add(quad.strokeOuter);
        signature.add(quad.strokeJoin);
        signature.add(quad.outline);
        signature.add(quad.group);
        signature.add(quad.groupPicture);
    }
    return signature.value;
}

// Every entry of one list, in `ZIndex` then document order, stably: one flat
// ordering, and the tie-break is what makes it total -- two elements at ZIndex
// 0 draw in the order they were built, on every run. `into` is the group whose
// picture they are drawn into, plus one; 0 is the screen.
void emitAll(const scene::World& world, std::vector<Entry>& entries, std::vector<std::vector<Entry>>& groups, u32 into,
             DrawList& out)
{
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.zIndex < b.zIndex; });

    for (const Entry& entry : entries) {
        const usize first = out.quads.size();
        if (entry.ownsGroup == 0) {
            emit(world, entry, out);
            for (usize index = first; index < out.quads.size(); ++index) {
                out.quads[index].group = into;
                // A gradient's colours are not in its quads: a row of the
                // frame's table is, and the row can change under them.
                if (into != 0 && out.quads[index].gradient != 0)
                    out.groups[into - 1].live = true;
            }
            if (into != 0 && world.viewportFrames().find(entry.id) != nullptr)
                out.groups[into - 1].live = true;
            continue;
        }

        const scene::UIObjectComponent* self = world.uiObjects().find(entry.id);
        const scene::CanvasGroupComponent* canvas = world.canvasGroups().find(entry.id);
        if (self == nullptr || canvas == nullptr)
            continue;
        DrawGroup group;
        group.owner = entry.id;
        group.box = Rect{self->absolutePosition, self->absolutePosition + self->absoluteSize};
        group.parent = into;
        out.groups.push_back(group);
        const u32 index = static_cast<u32>(out.groups.size());

        // The frame itself is in its picture, so a window fades with its
        // background and not in front of it.
        Entry inside = entry;
        inside.scissor = entry.innerScissor;
        inside.turn = Vec2{1.0f, 0.0f};
        inside.turnOffset = Vec2{};
        emit(world, inside, out);
        for (usize at = first; at < out.quads.size(); ++at) {
            out.quads[at].group = index;
            if (out.quads[at].gradient != 0)
                out.groups[index - 1].live = true;
        }
        emitAll(world, groups[entry.ownsGroup - 1], groups, index, out);
        out.groups[index - 1].signature = signatureOf(out, first, out.quads.size());

        // The picture, where the group is: one quad, with the group's colour
        // and transparency, under the clip and the turn the group is under.
        const f32 alpha = 1.0f - std::clamp(canvas->groupTransparency, 0.0f, 1.0f);
        if (alpha <= 0.0f)
            continue;
        DrawQuad picture;
        picture.min = group.box.min;
        picture.max = group.box.max;
        picture.uvMin = Vec2{0.0f, 0.0f};
        picture.uvMax = Vec2{1.0f, 1.0f};
        picture.color = canvas->groupColor;
        picture.alpha = alpha;
        picture.scissor = entry.scissor;
        picture.turn = entry.turn;
        picture.turnOffset = entry.turnOffset;
        picture.group = into;
        picture.groupPicture = index;
        out.quads.push_back(picture);
    }
}

// **What shows which object is selected** (ADR 0128), over everything else on
// its screen: the object's own `SelectionImageObject` at its box, or the
// default -- an outline a little outside it, following its corners.
void emitSelection(const scene::World& world, core::InstanceId screen, DrawList& out)
{
    const core::InstanceId selected = world.engineState().uiSelected;
    const scene::UIObjectComponent* object = world.alive(selected) ? world.uiObjects().find(selected) : nullptr;
    if (object == nullptr)
        return;
    // On this screen, and shown all the way up to it.
    bool here = false;
    for (core::InstanceId current = selected; current.valid(); current = world.parentOf(current)) {
        if (current == screen) {
            here = true;
            break;
        }
        if (const scene::UIObjectComponent* above = world.uiObjects().find(current);
            above != nullptr && !above->visible)
            return;
    }
    if (!here)
        return;

    const Rect box{object->absolutePosition, object->absolutePosition + object->absoluteSize};
    if (world.alive(object->selectionImageObject) && world.uiObjects().find(object->selectionImageObject) != nullptr) {
        emit(world, Entry{object->selectionImageObject, 0.0f, 0, 0, Vec2{1.0f, 0.0f}, Vec2{}, 0, 0}, out, &box);
        return;
    }

    f32 cornerRadius = 0.0f;
    const f32 shorter = std::fmin(box.max.x - box.min.x, box.max.y - box.min.y);
    for (core::InstanceId child = world.firstChild(selected); child.valid(); child = world.nextSibling(child)) {
        if (const scene::UICornerComponent* corner = world.uiCorners().find(child); corner != nullptr) {
            const f32 radius = corner->cornerRadius.scale * shorter + corner->cornerRadius.offset * object->unitScale;
            cornerRadius = std::fmax(0.0f, std::fmin(radius, shorter * 0.5f));
            break;
        }
    }
    // Two units out and three thick, in the one colour a HUD is least likely
    // to be: the outline has to read over a button of any colour.
    const f32 inner = 2.0f * object->unitScale;
    const f32 outer = inner + 3.0f * object->unitScale;
    const f32 grow = outer + 1.0f;
    DrawQuad band;
    band.min = Vec2{box.min.x - grow, box.min.y - grow};
    band.max = Vec2{box.max.x + grow, box.max.y + grow};
    band.color = core::Color3{0.25f, 0.62f, 1.0f};
    band.alpha = 1.0f;
    band.scissor = 0;
    band.cornerRadius = cornerRadius;
    band.borderStroke = true;
    band.strokeBox = box;
    band.strokeInner = inner;
    band.strokeOuter = outer;
    out.quads.push_back(band);
}

} // namespace

void buildDrawList(const scene::World& world, core::InstanceId uiService, DrawList& out)
{
    out.clear();
    if (!uiService.valid())
        return;

    // Index 0 is the whole window, and it is always present so that "no clip"
    // needs no special case anywhere downstream.
    out.scissors.push_back(Rect{Vec2{-1.0e9f, -1.0e9f}, Vec2{1.0e9f, 1.0e9f}});

    // Every enabled ScreenGui, in `DisplayOrder`. Stable, so two trees at one
    // order keep document order rather than swapping between frames.
    std::vector<std::pair<f32, core::InstanceId>> screens;
    for (core::InstanceId child = world.firstChild(uiService); child.valid(); child = world.nextSibling(child)) {
        if (const scene::ScreenGuiComponent* screen = world.screenGuis().find(child);
            screen != nullptr && screen->enabled) {
            screens.emplace_back(screen->displayOrder, child);
        }
    }
    std::stable_sort(screens.begin(), screens.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    Collected collected;
    for (const auto& [order, screen] : screens) {
        collected.entries.clear();
        collected.groups.clear();
        for (core::InstanceId element = world.firstChild(screen); element.valid();
             element = world.nextSibling(element)) {
            collect(world, element, 0, Vec2{1.0f, 0.0f}, Vec2{}, collected.entries, out.scissors, &collected.groups);
        }
        emitAll(world, collected.entries, collected.groups, 0, out);
        emitSelection(world, screen, out);
    }
}

void buildCanvasDrawList(const scene::World& world, core::InstanceId root, DrawList& out)
{
    out.clear();
    out.scissors.push_back(Rect{Vec2{-1.0e9f, -1.0e9f}, Vec2{1.0e9f, 1.0e9f}});
    if (!root.valid())
        return;
    Collected collected;
    for (core::InstanceId element = world.firstChild(root); element.valid(); element = world.nextSibling(element))
        collect(world, element, 0, Vec2{1.0f, 0.0f}, Vec2{}, collected.entries, out.scissors, nullptr);
    emitAll(world, collected.entries, collected.groups, 0, out);
}

u32 DrawList::gradientSlot(const DrawGradient& gradient)
{
    // A linear scan: a frame has a handful of distinct gradients, and the
    // order they were first named in is the table's row order (R10).
    for (usize index = 0; index < gradients.size(); ++index) {
        if (gradients[index] == gradient)
            return static_cast<u32>(index) + 1u;
    }
    gradients.push_back(gradient);
    return static_cast<u32>(gradients.size());
}

void setImageProvider(ImageProvider provider, void* user) noexcept
{
    g_imageProvider = provider;
    g_imageProviderUser = user;
}

} // namespace engine::ui
