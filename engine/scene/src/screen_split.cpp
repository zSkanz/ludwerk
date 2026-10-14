#include "engine/scene/screen_split.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/scene/players.h"
#include "engine/scene/world.h"

namespace engine::scene {

namespace {

[[nodiscard]] core::Rect rectOf(core::u32 left, core::u32 top, core::u32 right, core::u32 bottom) noexcept
{
    return core::Rect{{static_cast<core::f32>(left), static_cast<core::f32>(top)},
                      {static_cast<core::f32>(right), static_cast<core::f32>(bottom)}};
}

} // namespace

std::array<core::Rect, 4> screenSplitRects(core::i32 mode, core::u32 count, core::u32 width, core::u32 height) noexcept
{
    std::array<core::Rect, 4> rects{};
    count = std::min<core::u32>(count, 4);
    if (count == 0)
        return rects;
    // **Edges on whole pixels, each from the one division** -- the right edge
    // of one view IS the left edge of the next -- so no column of the window
    // is left out or drawn twice at a width that does not divide.
    const auto cut = [](core::u32 extent, core::u32 index, core::u32 parts) { return extent * index / parts; };
    // Three and four in a grid are the window's quarters; one and two are not
    // a grid of anything, and are side by side.
    if (mode == 3 && count >= 3) {
        const core::u32 midX = width / 2;
        const core::u32 midY = height / 2;
        rects[0] = rectOf(0, 0, midX, midY);
        rects[1] = rectOf(midX, 0, width, midY);
        rects[2] = rectOf(0, midY, midX, height);
        if (count == 4)
            rects[3] = rectOf(midX, midY, width, height);
        return rects;
    }
    for (core::u32 index = 0; index < count; ++index) {
        if (mode == 2)
            rects[index] = rectOf(0, cut(height, index, count), width, cut(height, index + 1, count));
        else
            rects[index] = rectOf(cut(width, index, count), 0, cut(width, index + 1, count), height);
    }
    return rects;
}

core::u32 localViewers(const World& world, std::array<ScreenView, 4>& out)
{
    core::u32 count = 0;
    std::vector<core::InstanceId> seated;
    localPlayers(world, seated);
    for (const core::InstanceId child : seated) {
        if (count == out.size())
            break;
        const PlayerComponent* player = world.players().find(child);
        if (!world.alive(player->camera) || world.destroyed(player->camera) ||
            world.cameras().find(player->camera) == nullptr)
            continue;
        out[count] = ScreenView{};
        out[count].player = child;
        out[count].camera = player->camera;
        ++count;
    }
    return count;
}

ScreenSplitFrame resolveScreenSplit(const World& world, core::InstanceId workspace, core::u32 width, core::u32 height)
{
    ScreenSplitFrame frame;
    const WorkspaceComponent* space = world.workspaces().find(workspace);
    if (space == nullptr || space->screenSplit == 0 || width == 0 || height == 0)
        return frame;
    std::array<ScreenView, 4> viewers{};
    const core::u32 count = localViewers(world, viewers);
    if (count == 0)
        return frame;
    const core::Rect whole = rectOf(0, 0, width, height);
    // As thick on a small window as on a large one, to the eye.
    const core::f32 thickness = space->splitLineThickness * static_cast<core::f32>(height) / 1080.0f;
    frame.lineColor = space->splitLineColor;

    const bool dynamic = space->screenSplit == 4;
    if (dynamic) {
        // **One picture while they are together.** A game that never parts
        // its players pays for one view, through the camera that frames them.
        if (count < 2 || space->splitBlend <= 0.0f)
            return frame;
        if (count == 2) {
            // The way from the first to the second, y down on a screen.
            const core::f32 angle = space->splitAngle * std::numbers::pi_v<core::f32> / 180.0f;
            const core::Vec2 normal{std::cos(angle), -std::sin(angle)};
            frame.form = ScreenForm::Line;
            frame.count = 2;
            frame.views[0] = viewers[0];
            frame.views[0].rect = whole;
            frame.views[0].sideNormal = normal;
            frame.views[1] = viewers[1];
            frame.views[1].rect = whole;
            frame.views[1].sideNormal = -normal;
            // It appears and goes, where a cut would be seen.
            frame.lineThickness = thickness * std::min(space->splitBlend, 1.0f);
            return frame;
        }
        // Three and four: the grid, faded in over the one picture.
        frame.wholeWeight = 1.0f - std::min(space->splitBlend, 1.0f);
    }
    else if (count == 1) {
        // One player with a camera has the window, through that camera.
        frame.camera = viewers[0].camera;
        return frame;
    }

    const std::array<core::Rect, 4> rects = screenSplitRects(dynamic ? 3 : space->screenSplit, count, width, height);
    frame.form = ScreenForm::Rects;
    frame.count = count;
    for (core::u32 index = 0; index < count; ++index) {
        frame.views[index] = viewers[index];
        frame.views[index].rect = rects[index];
    }
    frame.lineThickness = thickness;
    return frame;
}

const ScreenView* screenViewAt(std::span<const ScreenView> views, core::Vec2 pixel) noexcept
{
    for (const ScreenView& view : views) {
        if (pixel.x < view.rect.min.x || pixel.y < view.rect.min.y || pixel.x >= view.rect.max.x ||
            pixel.y >= view.rect.max.y)
            continue;
        if (view.sideNormal.x != 0.0f || view.sideNormal.y != 0.0f) {
            const core::Vec2 middle{0.5f * (view.rect.min.x + view.rect.max.x),
                                    0.5f * (view.rect.min.y + view.rect.max.y)};
            const core::Vec2 from = pixel - middle;
            if (from.x * view.sideNormal.x + from.y * view.sideNormal.y > 0.0f)
                continue;
        }
        return &view;
    }
    return nullptr;
}

} // namespace engine::scene
