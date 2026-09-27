#include "engine/app/view_host.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/log.h"
#include "engine/core/text_key.h"

namespace engine::app {

core::Vec2 viewSize(core::Vec2 resolution, core::u32 maxSide) noexcept
{
    const float side = static_cast<float>(std::max(maxSide, 1u));
    float width = std::isfinite(resolution.x) ? std::max(resolution.x, 1.0f) : 1.0f;
    float height = std::isfinite(resolution.y) ? std::max(resolution.y, 1.0f) : 1.0f;
    const float longer = std::max(width, height);
    if (longer > side) {
        width *= side / longer;
        height *= side / longer;
    }
    return core::Vec2{std::max(std::round(width), 1.0f), std::max(std::round(height), 1.0f)};
}

std::vector<core::usize> chooseViews(std::span<const ViewCandidate> views, core::u32 budget)
{
    std::vector<core::usize> order;
    for (core::usize index = 0; index < views.size(); ++index) {
        if (views[index].due)
            order.push_back(index);
    }
    // Never drawn first -- a screen still black is the one most worth a
    // picture -- then the picture that has waited longest.
    std::stable_sort(order.begin(), order.end(), [&](core::usize a, core::usize b) {
        if (views[a].drawn != views[b].drawn)
            return !views[a].drawn;
        return views[a].lastDrawn < views[b].lastDrawn;
    });
    if (order.size() > budget)
        order.resize(budget);
    return order;
}

namespace {

// Whether `id` is in the world: under the `Workspace` the frame draws. A
// camera texture waiting in `ReplicatedStorage` to be cloned is a template, and
// a template draws nothing.
[[nodiscard]] bool inWorld(const scene::World& world, core::InstanceId id, core::InstanceId workspace) noexcept
{
    for (core::InstanceId at = world.parentOf(id); at.valid(); at = world.parentOf(at)) {
        if (at == workspace)
            return true;
    }
    return false;
}

} // namespace

void ViewHost::sync(rhi::IDevice& device, rhi::ICmdList& cmd, scene::World& world, core::InstanceId workspace,
                    render::TextureLibrary& library, render::IRenderer* renderer)
{
    std::vector<bool> seen(views_.size(), false);

    world.cameraTextures().forEach([&](core::InstanceId id, const scene::CameraTextureComponent& source) {
        const std::string_view name = world.atoms().text(source.viewName);
        if (name.empty() || !inWorld(world, id, workspace))
            return;

        auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.owner == id; });
        // A renamed view is a new one: the old name goes back to black.
        if (view != views_.end() && view->name != name) {
            const std::ptrdiff_t at = view - views_.begin();
            release(device, library, renderer, *view);
            views_.erase(view);
            seen.erase(seen.begin() + at);
            view = views_.end();
        }
        if (view == views_.end()) {
            // **The first made wins** (ADR 0107): a second drawer of one name
            // draws nothing, and says so once.
            const bool taken = std::any_of(views_.begin(), views_.end(), [&](const View& v) { return v.name == name; });
            if (taken) {
                if (std::find(warned_.begin(), warned_.end(), id) == warned_.end()) {
                    warned_.push_back(id);
                    const std::array<core::I18nArg, 2> args{
                        core::I18nArg{"name", std::string(name)},
                        core::I18nArg{"instance", std::string(world.atoms().text(world.name(id)))}};
                    core::log(core::LogLevel::Warn, ENG_TR("render.warn.view_name_taken"), args);
                }
                return;
            }
            View made;
            made.owner = id;
            made.name = std::string(name);
            made.urn = world.atoms().intern("view://" + made.name);
            made.rendererView = nextRendererView_++;
            views_.push_back(std::move(made));
            seen.push_back(false);
            view = views_.end() - 1;
        }
        seen[static_cast<core::usize>(view - views_.begin())] = true;

        // The texture at the size asked for, capped; remade when that changes.
        const core::Vec2 size = viewSize(source.resolution, maxResolution_);
        const auto width = static_cast<core::u32>(size.x);
        const auto height = static_cast<core::u32>(size.y);
        if (!view->texture.valid() || view->width != width || view->height != height) {
            if (view->texture.valid())
                device.destroy(view->texture);
            view->texture = device.createTexture({
                .format = ViewFormat,
                .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
                .width = width,
                .height = height,
                .debugName = "view",
            });
            view->width = width;
            view->height = height;
            view->cleared = false;
            view->drawn = false;
        }
        if (!view->texture.valid())
            return;
        library.set(view->urn, view->texture, width, height);

        if (!view->cleared) {
            const std::array<rhi::ColorAttachment, 1> black{rhi::ColorAttachment{
                .texture = view->texture,
                .loadOp = rhi::LoadOp::Clear,
                .storeOp = rhi::StoreOp::Store,
                .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
            }};
            cmd.beginRenderPass({.colorAttachments = black, .debugName = "view-clear"});
            cmd.endRenderPass();
            view->cleared = true;
        }
    });

    // What no camera texture declares any more.
    for (core::usize index = views_.size(); index-- > 0;) {
        if (seen[index])
            continue;
        release(device, library, renderer, views_[index]);
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

std::vector<ViewHost::View*> ViewHost::due(const scene::World& world, core::u64 frame)
{
    std::vector<ViewCandidate> candidates(views_.size());
    for (core::usize index = 0; index < views_.size(); ++index) {
        const View& view = views_[index];
        const scene::CameraTextureComponent* source = world.cameraTextures().find(view.owner);
        ViewCandidate& candidate = candidates[index];
        candidate.drawn = view.drawn;
        candidate.lastDrawn = view.lastDrawn;
        if (source == nullptr || !source->enabled || !view.texture.valid() || !world.alive(source->camera) ||
            world.cameras().find(source->camera) == nullptr)
            continue;
        candidate.due = viewDue(view.drawn, view.lastDrawn, frame, source->updateInterval);
    }
    std::vector<View*> out;
    for (const core::usize index : chooseViews(candidates, perFrame_))
        out.push_back(&views_[index]);
    return out;
}

const ViewHost::View* ViewHost::find(std::string_view name) const noexcept
{
    const auto found = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.name == name; });
    return found == views_.end() ? nullptr : &*found;
}

void ViewHost::release(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer, View& view)
{
    // Taken back from the library before it is destroyed, so nothing samples a
    // texture that is gone; the name then shows black again (`MeshLoader`).
    (void)library.take(view.urn);
    if (view.texture.valid())
        device.destroy(view.texture);
    view.texture = {};
    if (renderer != nullptr)
        renderer->releaseView(device, view.rendererView);
}

void ViewHost::destroy(rhi::IDevice& device, render::TextureLibrary& library, render::IRenderer* renderer)
{
    for (View& view : views_)
        release(device, library, renderer, view);
    views_.clear();
    warned_.clear();
}

} // namespace engine::app
