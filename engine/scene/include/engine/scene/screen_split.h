// **The window shared between the players at one machine** (ADR 0205): who
// has a view this frame, and where in the window it is.
//
// A function of the world and the window's size and nothing else, so the host
// asks it once a frame and everything that needs to agree with the picture --
// the renderer, a camera's `WorldToViewportPoint`, the pointer's ray, a
// player's interface -- reads the one answer.
#pragma once

#include <array>
#include <span>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {

class World;

// One local player's view of the window.
struct ScreenView
{
    core::InstanceId player;
    core::InstanceId camera;
    // In pixels of what the world is drawn into: the window, or the editor's
    // Viewport panel.
    core::Rect rect;
    // **Two views across a line** (`Enum.ScreenSplit.Dynamic`): the view is
    // seen where `dot(sideNormal, p) <= 0`, `p` measured from the middle of
    // `rect` with y down. Zero is all of the rectangle.
    core::Vec2 sideNormal{};

    [[nodiscard]] bool operator==(const ScreenView&) const noexcept = default;
};

// How the views make the window.
enum class ScreenForm : core::u8
{
    // One picture: through `Workspace.CurrentCamera`, or through `camera`
    // where that is set (the only local player with one).
    One,
    // Each view in its rectangle.
    Rects,
    // Two views the size of the window, across a line through its middle.
    Line,
};

struct ScreenSplitFrame
{
    ScreenForm form = ScreenForm::One;
    std::array<ScreenView, 4> views{};
    core::u32 count = 0;
    // `One`: the camera the one picture is through, when it is a player's and
    // not the workspace's.
    core::InstanceId camera;
    // `Rects`, from `Dynamic` with three or four players on the way apart:
    // how much of the one picture through `CurrentCamera` shows over the
    // rectangles, 0 to 1. Above 0 that picture is drawn too.
    core::f32 wholeWeight = 0.0f;
    // The line between views, in pixels of the window, and its colour.
    core::f32 lineThickness = 0.0f;
    core::Color3 lineColor{};

    [[nodiscard]] std::span<const ScreenView> shown() const noexcept { return {views.data(), count}; }
};

// The rectangles of `count` views (1 to 4) in a window `width` by `height`
// pixels, for `Enum.ScreenSplit` `mode` (1 Columns, 2 Rows, 3 Grid). They
// meet on whole pixels and leave none out, except the fourth quarter of three
// in a grid, which is nobody's.
[[nodiscard]] std::array<core::Rect, 4> screenSplitRects(core::i32 mode, core::u32 count, core::u32 width,
                                                         core::u32 height) noexcept;

// This machine's players that have a view: local, not leaving, with a
// `Camera` that is one, in the order they joined; four at most.
core::u32 localViewers(const World& world, std::array<ScreenView, 4>& out);

// What `workspace` says the window is this frame.
[[nodiscard]] ScreenSplitFrame resolveScreenSplit(const World& world, core::InstanceId workspace, core::u32 width,
                                                  core::u32 height);

// Which of the frame's views a point of the window is in: the rectangle that
// holds it, and of two across a line the one on whose side it is. Null where
// the window is one picture, or the point is in nobody's part.
[[nodiscard]] const ScreenView* screenViewAt(std::span<const ScreenView> views, core::Vec2 pixel) noexcept;

} // namespace engine::scene
