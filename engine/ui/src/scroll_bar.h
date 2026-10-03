// **Where a `ScrollFrame`'s bar is** (G40): one answer for the drawing and the
// hand, so a thumb is grabbed where it is drawn and the canvas follows it as
// far as the drawing says it went.
#pragma once

#include <algorithm>
#include <cmath>

#include "engine/core/math.h"
#include "engine/scene/components.h"

namespace engine::ui {

// One axis of a vector by number, 0 across and 1 down: what lets the scrolling
// say a rule once rather than once an axis.
[[nodiscard]] inline float& along(core::Vec2& value, int axis) noexcept
{
    return axis == 0 ? value.x : value.y;
}

[[nodiscard]] inline float along(const core::Vec2& value, int axis) noexcept
{
    return axis == 0 ? value.x : value.y;
}

// Whether a hand scrolls `scroll` along `axis` (`ScrollingDirection`).
[[nodiscard]] inline bool scrollsAlong(const scene::ScrollFrameComponent& scroll, int axis) noexcept
{
    return (scroll.scrollingDirection & (axis == 0 ? 1 : 2)) != 0;
}

// Whether a pull past the end along `axis` gives (`ElasticBehavior`), for a
// canvas with `room` to scroll that way.
[[nodiscard]] inline bool elasticAlong(const scene::ScrollFrameComponent& scroll, int axis, float room) noexcept
{
    if (scroll.elasticBehavior == 2 || !scrollsAlong(scroll, axis))
        return false;
    return scroll.elasticBehavior == 1 || room > 0.0f;
}

// How far a pull of `excess` past the end shows, in a view `view` long: as far
// at first, then less and less, and never as far as the whole view -- the
// curve a hand reads as the end of a list.
[[nodiscard]] inline float rubberBand(float excess, float view) noexcept
{
    if (view <= 0.0f || excess == 0.0f)
        return 0.0f;
    const float shown = view * (1.0f - 1.0f / (std::fabs(excess) * 0.55f / view + 1.0f));
    return excess < 0.0f ? -shown : shown;
}

struct ScrollBarShape
{
    core::Rect track;
    core::Rect thumb;
    // How far the thumb can move along the track, and how far the canvas can
    // along the same axis, both in pixels: the ratio is how a dragged thumb
    // moves the canvas.
    float travel = 0.0f;
    float room = 0.0f;
    bool shown = false;
};

// The bar of one axis of a frame drawn in `box`, for a canvas of `canvas`
// pixels seen `view` at a time and scrolled `offset` pixels along. `shown` is
// false for an axis with nowhere to go, and for a bar of no thickness.
[[nodiscard]] inline ScrollBarShape scrollBarShape(core::Rect box, float canvas, float view, float offset,
                                                   float thickness, bool vertical) noexcept
{
    ScrollBarShape shape;
    if (!(canvas > view && view > 0.0f) || thickness <= 0.0f)
        return shape;
    shape.shown = true;

    // Along the far edge, inset by nothing: a bar that floated inside its
    // region would overlap the content it is next to.
    shape.track = vertical ? core::Rect{{box.max.x - thickness, box.min.y}, {box.max.x, box.max.y}}
                           : core::Rect{{box.min.x, box.max.y - thickness}, {box.max.x, box.max.y}};

    // As long a fraction of the track as the view is of the canvas, and never
    // shorter than the bar is wide -- a thumb of two pixels in a very long
    // canvas is a thumb nobody can grab.
    const float trackLength =
        vertical ? (shape.track.max.y - shape.track.min.y) : (shape.track.max.x - shape.track.min.x);
    const float minimum = std::fmin(thickness * 2.0f, trackLength);
    const float length = std::fmax(minimum, trackLength * (view / canvas));
    shape.room = std::fmax(0.0f, canvas - view);
    shape.travel = std::fmax(0.0f, trackLength - length);
    const float start = shape.room > 0.0f ? shape.travel * std::clamp(offset / shape.room, 0.0f, 1.0f) : 0.0f;
    shape.thumb.min = vertical ? core::Vec2{shape.track.min.x, shape.track.min.y + start}
                               : core::Vec2{shape.track.min.x + start, shape.track.min.y};
    shape.thumb.max = vertical ? core::Vec2{shape.track.max.x, shape.track.min.y + start + length}
                               : core::Vec2{shape.track.min.x + start + length, shape.track.max.y};
    return shape;
}

} // namespace engine::ui
