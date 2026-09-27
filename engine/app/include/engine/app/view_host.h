// **The views a game draws besides its own** (ADR 0107): every `CameraTexture`
// in the world, each a texture named `view://<ViewName>` that anything taking a
// texture can show.
//
// The host keeps one texture per view and nothing else. It does not draw: the
// frame loop does, because a view is the world extracted from another camera,
// and the loop is what holds the world, the meshes and the renderer. What this
// decides is which views exist, how big their textures are, and which of them
// are drawn this frame -- a budget, oldest picture first, so a wall of twelve
// monitors takes turns instead of costing twelve views.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/core/types.h"
#include "engine/render/render_world.h"
#include "engine/render/renderer.h"
#include "engine/rhi/device.h"
#include "engine/scene/world.h"

namespace engine::app {

// The format every view is drawn in, whatever the window's is.
inline constexpr rhi::TextureFormat ViewFormat = rhi::TextureFormat::Rgba8Unorm;

// A view's texture size: the authored resolution with its longer side capped at
// `maxSide`, the shape kept, and never below one pixel.
[[nodiscard]] core::Vec2 viewSize(core::Vec2 resolution, core::u32 maxSide) noexcept;

// **Which views are drawn this frame**, as indices into `views`, at most
// `budget` of them: those whose interval has come round, never-drawn ones
// first, then the oldest picture. Ties keep the list's order.
struct ViewCandidate
{
    // Due: enabled, with a camera, and its `UpdateInterval` has passed.
    bool due = false;
    bool drawn = false;
    core::u64 lastDrawn = 0;
};
[[nodiscard]] std::vector<core::usize> chooseViews(std::span<const ViewCandidate> views, core::u32 budget);

// Whether a view is due on `frame`: never drawn, or `interval` frames (one or
// more) since it last was.
[[nodiscard]] constexpr bool viewDue(bool drawn, core::u64 lastDrawn, core::u64 frame, core::u32 interval) noexcept
{
    return !drawn || frame >= lastDrawn + (interval > 0 ? interval : 1u);
}

class ViewHost
{
public:
    struct View
    {
        // The `CameraTexture` that declared it.
        core::InstanceId owner;
        std::string name;
        // `view://<name>`, in the world's atom table: the texture library's key.
        core::NameAtom urn;
        rhi::TextureHandle texture;
        core::u32 width = 0;
        core::u32 height = 0;
        // The renderer's view id: its own exposure, shadow fit and targets.
        core::u32 rendererView = 0;
        bool drawn = false;
        core::u64 lastDrawn = 0;
        // What the last draw cost to record, for the F3 overlay.
        core::f64 milliseconds = 0.0;
        // Black until the first picture: a screen that is off.
        bool cleared = false;
    };

    void setLimits(core::u32 perFrame, core::u32 maxResolution) noexcept
    {
        perFrame_ = perFrame;
        maxResolution_ = maxResolution;
    }
    [[nodiscard]] core::u32 perFrame() const noexcept { return perFrame_; }

    // Once a frame, before anything is drawn: every `CameraTexture` with a name
    // gets a texture of its size in `library`, and a view whose instance is
    // gone gives its texture and its renderer state back. Records the first
    // clear of a new texture into `cmd`.
    void sync(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world, core::InstanceId workspace,
              render::TextureLibrary& library, render::IRenderer* renderer);

    // The views to draw on frame `frame`, within the budget.
    [[nodiscard]] std::vector<View*> due(const scene::World& world, core::u64 frame);

    // What the loop calls after drawing one.
    static void drawn(View& view, core::u64 frame, core::f64 milliseconds) noexcept
    {
        view.drawn = true;
        view.lastDrawn = frame;
        view.milliseconds = milliseconds;
    }

    [[nodiscard]] std::span<const View> views() const noexcept { return views_; }

    // A view by its name after `view://`, for the UI's image lookup.
    [[nodiscard]] const View* find(std::string_view name) const noexcept;

    void destroy(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer);

private:
    void release(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer, View& view);

    std::vector<View> views_;
    // Instances already told their name is taken, so the log says it once.
    std::vector<core::InstanceId> warned_;
    core::u32 nextRendererView_ = 1;
    core::u32 perFrame_ = 4;
    core::u32 maxResolution_ = 1024;
};

} // namespace engine::app
