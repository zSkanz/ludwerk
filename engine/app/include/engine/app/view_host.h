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

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/types.h"
#include "engine/render/animation.h"
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

// **What a `ViewportFrame`'s picture depends on**, as one number: the parts,
// meshes and cameras inside it, where they are DRAWN (`poses`, ADR 0134) --
// between ticks, a moving item's picture changes every frame -- how big and
// what they wear, and the frame's own light. The picture is redrawn when this
// changes and at no other time -- forty still items in an inventory are forty
// pictures once.
//
// **And how they are posed** (ADR 0107, amended): `poseOf` answers a skinned
// mesh's joints as they stand, or nothing for a mesh no clip drives. A hero
// breathing in a selection screen is a picture that changes; one standing in
// its bind pose is a picture once, as before.
using FramePoseOf = std::function<std::span<const core::Mat4>(core::InstanceId meshPart)>;
[[nodiscard]] core::u64 frameSignature(const scene::World& world, core::InstanceId frame,
                                       const render::DrawPoses* poses = nullptr, const FramePoseOf* poseOf = nullptr);

// **The skinned meshes inside the frames a player can see** (ADR 0107,
// amended), each with the share of the screen's height its frame takes: what
// the animation system is told it is looked at, every frame, whether or not
// the frame was redrawn. Without it a mesh in a frame was posed once -- nothing
// had seen it since, so it was not posed; it was not posed, so its picture did
// not change; its picture did not change, so nothing drew it to see it.
// `animates` says whether a mesh wears a skeleton something poses.
void collectFrameSkins(const scene::World& world, float viewportHeight,
                       const std::function<bool(core::InstanceId meshPart)>& animates,
                       std::vector<render::SeenSkin>& out);

// The camera a `ViewportFrame` looks through: its `CurrentCamera` when that is a
// camera inside it, and otherwise one that frames everything inside from the
// front and a little above. Nothing when there is nothing inside to frame.
[[nodiscard]] std::optional<render::ViewOverride> frameLens(const scene::World& world, core::InstanceId frame,
                                                            float aspect, const render::DrawPoses* poses = nullptr);

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
        // A `ViewportFrame`'s laid-out size while it differs from the
        // texture's, and for how many frames in a row it has been that size.
        core::u32 wantedWidth = 0;
        core::u32 wantedHeight = 0;
        core::u32 wantedFrames = 0;
        // The renderer's view id: its own exposure, shadow fit and targets.
        core::u32 rendererView = 0;
        bool drawn = false;
        core::u64 lastDrawn = 0;
        // What the last draw cost to record, for the F3 overlay.
        core::f64 milliseconds = 0.0;
        // Black until the first picture: a screen that is off.
        bool cleared = false;
        // **A `ViewportFrame`'s view** rather than a camera texture's: named
        // `#<index>`, drawn transparent by the frame's own light, and only
        // when `signature` moves from the one it was drawn at.
        bool frame = false;
        core::u64 signature = 0;
        core::u64 drawnSignature = 0;
        // **A `SubWorld`'s view** (ADR 0107 §3): the world it runs, from that
        // world's own camera -- drawn by the frame from the sub-world's host.
        bool subWorld = false;
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
              render::TextureLibrary& library, render::IRenderer* renderer, const render::DrawPoses* poses = nullptr,
              const FramePoseOf* poseOf = nullptr);

    // The views to draw on frame `frame`, within the budget.
    // A `SubWorld`'s view is due only while `running` says its world is up
    // and ticking; without the question, never.
    [[nodiscard]] std::vector<View*> due(const scene::World& world, core::u64 frame,
                                         const std::function<bool(core::InstanceId)>& running = nullptr);

    // What the loop calls after drawing one.
    static void drawn(View& view, core::u64 frame, core::f64 milliseconds) noexcept
    {
        view.drawn = true;
        view.lastDrawn = frame;
        view.milliseconds = milliseconds;
        view.drawnSignature = view.signature;
    }

    [[nodiscard]] std::span<const View> views() const noexcept { return views_; }

    // A view by its name after `view://`, for the UI's image lookup.
    [[nodiscard]] const View* find(std::string_view name) const noexcept;

    void destroy(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer);

private:
    void release(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer, View& view);
    // The texture at `width` by `height`, made or remade, set in the library,
    // and cleared the first time to `clear`.
    void ensureTexture(rhi::IDevice& device, rhi::ICmdList& cmd, render::TextureLibrary& library, View& view,
                       core::u32 width, core::u32 height, rhi::ColorRgba clear);

    std::vector<View> views_;
    // Instances already told their name is taken, so the log says it once.
    std::vector<core::InstanceId> warned_;
    core::u32 nextRendererView_ = 1;
    core::u32 perFrame_ = 4;
    core::u32 maxResolution_ = 1024;
};

} // namespace engine::app
