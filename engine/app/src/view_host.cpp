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
                    render::TextureLibrary& library, render::IRenderer* renderer, const render::DrawPoses* poses)
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
        ensureTexture(device, cmd, library, *view, static_cast<core::u32>(size.x), static_cast<core::u32>(size.y),
                      {0.0f, 0.0f, 0.0f, 1.0f});
    });

    // **Every `ViewportFrame`**, at the size it is laid out at, clear until its
    // first picture -- an empty slot shows the frame's own background.
    world.viewportFrames().forEach([&](core::InstanceId id, const scene::ViewportFrameComponent&) {
        const scene::UIObjectComponent* element = world.uiObjects().find(id);
        if (element == nullptr || !element->visible || element->absoluteSize.x < 1.0f || element->absoluteSize.y < 1.0f)
            return;
        auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.owner == id; });
        if (view == views_.end()) {
            View made;
            made.owner = id;
            made.frame = true;
            made.name = "#" + std::to_string(id.index);
            made.urn = world.atoms().intern("view://" + made.name);
            made.rendererView = nextRendererView_++;
            views_.push_back(std::move(made));
            seen.push_back(false);
            view = views_.end() - 1;
        }
        seen[static_cast<core::usize>(view - views_.begin())] = true;
        view->signature = frameSignature(world, id, poses);
        const core::Vec2 size = viewSize(element->absoluteSize, maxResolution_);
        auto width = static_cast<core::u32>(size.x);
        auto height = static_cast<core::u32>(size.y);
        // **A frame being resized keeps its texture until the size settles**
        // (audit R10): a tweened frame changed size every frame, and every
        // change remade the texture and the renderer's ~20 targets behind it.
        // Meanwhile the picture is scaled into the frame; it is remade once
        // the size has held for a few frames, or at once past double or half.
        if (view->texture.valid() && (width != view->width || height != view->height)) {
            if (width == view->wantedWidth && height == view->wantedHeight)
                ++view->wantedFrames;
            else
                view->wantedFrames = 1;
            view->wantedWidth = width;
            view->wantedHeight = height;
            constexpr core::u32 SettledFrames = 6;
            const auto far = [](core::u32 wanted, core::u32 held) { return wanted > held * 2 || wanted * 2 < held; };
            if (view->wantedFrames < SettledFrames && !far(width, view->width) && !far(height, view->height)) {
                width = view->width;
                height = view->height;
            }
        }
        ensureTexture(device, cmd, library, *view, width, height, {0.0f, 0.0f, 0.0f, 0.0f});
    });

    // **Every `SubWorld` with a name to draw into** (ADR 0107 §3), black until
    // its world's first picture -- the screen of a cabinet nobody switched on.
    world.subWorlds().forEach([&](core::InstanceId id, const scene::SubWorldComponent& source) {
        const std::string_view name = world.atoms().text(source.viewName);
        if (name.empty() || !inWorld(world, id, workspace))
            return;
        auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.owner == id; });
        if (view != views_.end() && view->name != name) {
            const std::ptrdiff_t at = view - views_.begin();
            release(device, library, renderer, *view);
            views_.erase(view);
            seen.erase(seen.begin() + at);
            view = views_.end();
        }
        if (view == views_.end()) {
            if (std::any_of(views_.begin(), views_.end(), [&](const View& v) { return v.name == name; })) {
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
            made.subWorld = true;
            made.name = std::string(name);
            made.urn = world.atoms().intern("view://" + made.name);
            made.rendererView = nextRendererView_++;
            views_.push_back(std::move(made));
            seen.push_back(false);
            view = views_.end() - 1;
        }
        seen[static_cast<core::usize>(view - views_.begin())] = true;
        const core::Vec2 size = viewSize(source.resolution, maxResolution_);
        ensureTexture(device, cmd, library, *view, static_cast<core::u32>(size.x), static_cast<core::u32>(size.y),
                      {0.0f, 0.0f, 0.0f, 1.0f});
    });

    // What no camera texture declares any more.
    for (core::usize index = views_.size(); index-- > 0;) {
        if (seen[index])
            continue;
        release(device, library, renderer, views_[index]);
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void ViewHost::ensureTexture(rhi::IDevice& device, rhi::ICmdList& cmd, render::TextureLibrary& library, View& view,
                             core::u32 width, core::u32 height, rhi::ColorRgba clear)
{
    if (!view.texture.valid() || view.width != width || view.height != height) {
        if (view.texture.valid())
            device.destroy(view.texture);
        view.texture = device.createTexture({
            .format = ViewFormat,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = width,
            .height = height,
            .debugName = "view",
        });
        view.width = width;
        view.height = height;
        view.cleared = false;
        view.drawn = false;
    }
    if (!view.texture.valid())
        return;
    library.set(view.urn, view.texture, width, height);

    if (!view.cleared) {
        const std::array<rhi::ColorAttachment, 1> first{rhi::ColorAttachment{
            .texture = view.texture,
            .loadOp = rhi::LoadOp::Clear,
            .storeOp = rhi::StoreOp::Store,
            .clearColor = clear,
        }};
        cmd.beginRenderPass({.colorAttachments = first, .debugName = "view-clear"});
        cmd.endRenderPass();
        view.cleared = true;
    }
}

std::vector<ViewHost::View*> ViewHost::due(const scene::World& world, core::u64 frame,
                                           const std::function<bool(core::InstanceId)>& running)
{
    std::vector<ViewCandidate> candidates(views_.size());
    for (core::usize index = 0; index < views_.size(); ++index) {
        const View& view = views_[index];
        ViewCandidate& candidate = candidates[index];
        candidate.drawn = view.drawn;
        candidate.lastDrawn = view.lastDrawn;
        // A frame's picture is redrawn when what it shows has changed.
        if (view.frame) {
            candidate.due = view.texture.valid() && (!view.drawn || view.signature != view.drawnSignature);
            continue;
        }
        if (view.subWorld) {
            const scene::SubWorldComponent* source = world.subWorlds().find(view.owner);
            candidate.due = source != nullptr && view.texture.valid() && running && running(view.owner) &&
                            viewDue(view.drawn, view.lastDrawn, frame, source->updateInterval);
            continue;
        }
        const scene::CameraTextureComponent* source = world.cameraTextures().find(view.owner);
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

namespace {

// FNV-1a, a field at a time: what a frame's picture depends on, as one number.
struct Signature
{
    core::u64 value = 1469598103934665603ull;
    void bytes(const void* data, core::usize size) noexcept
    {
        const auto* at = static_cast<const unsigned char*>(data);
        for (core::usize index = 0; index < size; ++index) {
            value ^= at[index];
            value *= 1099511628211ull;
        }
    }
    template <typename T>
    void pod(const T& item) noexcept
    {
        bytes(&item, sizeof(item));
    }
};

// Every instance under `frame`, depth first.
template <typename Visit>
void forEachInside(const scene::World& world, core::InstanceId frame, Visit&& visit)
{
    std::vector<core::InstanceId> stack;
    for (core::InstanceId child = world.firstChild(frame); child.valid(); child = world.nextSibling(child))
        stack.push_back(child);
    while (!stack.empty()) {
        const core::InstanceId id = stack.back();
        stack.pop_back();
        visit(id);
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
            stack.push_back(child);
    }
}

} // namespace

core::u64 frameSignature(const scene::World& world, core::InstanceId frame, const render::DrawPoses* poses)
{
    render::DrawPoses still;
    const render::DrawPoses& posed =
        poses != nullptr && poses->world() == &world ? *poses : (still.begin(world, nullptr, 0.0f), still);
    Signature signature;
    if (const scene::ViewportFrameComponent* self = world.viewportFrames().find(frame); self != nullptr) {
        signature.pod(self->currentCamera.index);
        signature.pod(self->ambient);
        signature.pod(self->lightColor);
        signature.pod(self->lightDirection);
    }
    forEachInside(world, frame, [&](core::InstanceId id) {
        signature.pod(id.index);
        if (const scene::PartComponent* part = world.parts().find(id); part != nullptr) {
            const core::CFrameD drawn = posed.part(id);
            signature.pod(drawn.position);
            signature.pod(drawn.rotation);
            signature.pod(part->size);
            signature.pod(part->material.id);
            signature.pod(part->materialClone);
            signature.pod(part->shape);
            const asset::MaterialOverrides& worn = part->materialParameters;
            signature.pod(worn.set);
            signature.pod(worn.color);
            signature.pod(worn.transparency);
            signature.pod(worn.emissive);
            signature.pod(worn.metalness);
            signature.pod(worn.roughness);
        }
        if (const scene::MeshPartComponent* mesh = world.meshParts().find(id); mesh != nullptr)
            signature.pod(mesh->meshContent.id);
        if (const scene::CameraComponent* camera = world.cameras().find(id); camera != nullptr) {
            const core::CFrameD drawn = posed.camera(id);
            signature.pod(drawn.position);
            signature.pod(drawn.rotation);
            signature.pod(camera->fieldOfView);
        }
    });
    return signature.value;
}

std::optional<render::ViewOverride> frameLens(const scene::World& world, core::InstanceId frame, float aspect,
                                              const render::DrawPoses* poses)
{
    // Where things are drawn this frame (ADR 0134): a script moving what is
    // inside moves it between ticks like anything else.
    render::DrawPoses still;
    const render::DrawPoses& posed =
        poses != nullptr && poses->world() == &world ? *poses : (still.begin(world, nullptr, 0.0f), still);
    const scene::ViewportFrameComponent* self = world.viewportFrames().find(frame);
    if (self == nullptr)
        return std::nullopt;
    // Its own camera, when that camera is inside it.
    if (const scene::CameraComponent* camera = world.cameras().find(self->currentCamera); camera != nullptr) {
        bool inside = false;
        for (core::InstanceId at = world.parentOf(self->currentCamera); at.valid(); at = world.parentOf(at))
            inside = inside || at == frame;
        if (inside) {
            return render::ViewOverride{.cframe = posed.camera(self->currentCamera),
                                        .fieldOfView = camera->fieldOfView,
                                        .nearPlane = camera->nearPlane,
                                        .farPlane = camera->farPlane,
                                        .projection = camera->projection,
                                        .orthographicSize = camera->orthographicSize};
        }
    }

    // **Framed automatically**: a sphere round every part inside, seen from the
    // front and a little above, far enough back that the sphere fits the
    // narrower of the two angles.
    core::DVec3 low{1e300, 1e300, 1e300};
    core::DVec3 high{-1e300, -1e300, -1e300};
    bool any = false;
    forEachInside(world, frame, [&](core::InstanceId id) {
        const scene::PartComponent* part = world.parts().find(id);
        if (part == nullptr)
            return;
        const double sx = static_cast<double>(part->size.x);
        const double sy = static_cast<double>(part->size.y);
        const double sz = static_cast<double>(part->size.z);
        const double reach = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
        const core::DVec3 at = posed.part(id).position;
        low = core::DVec3{std::min(low.x, at.x - reach), std::min(low.y, at.y - reach), std::min(low.z, at.z - reach)};
        high =
            core::DVec3{std::max(high.x, at.x + reach), std::max(high.y, at.y + reach), std::max(high.z, at.z + reach)};
        any = true;
    });
    if (!any)
        return std::nullopt;
    const core::DVec3 centre{(low.x + high.x) * 0.5, (low.y + high.y) * 0.5, (low.z + high.z) * 0.5};
    const double radius =
        std::max(0.5 * std::sqrt((high.x - low.x) * (high.x - low.x) + (high.y - low.y) * (high.y - low.y) +
                                 (high.z - low.z) * (high.z - low.z)),
                 0.05);
    constexpr float FieldOfView = 40.0f;
    const double half = static_cast<double>(FieldOfView) * 0.5 * 3.14159265358979323846 / 180.0;
    const double narrower = aspect >= 1.0f ? half : std::atan(std::tan(half) * static_cast<double>(aspect));
    const double distance = radius / std::sin(narrower) * 1.05;
    // From the front (+Z) and about fifteen degrees above.
    const core::DVec3 eye{centre.x, centre.y + distance * 0.26, centre.z + distance * 0.966};
    render::ViewOverride lens;
    lens.cframe = core::lookAtCFrame(eye, centre, core::Vec3{0.0f, 1.0f, 0.0f});
    lens.fieldOfView = FieldOfView;
    lens.nearPlane = static_cast<float>(std::max(distance - radius * 2.0, 0.01));
    lens.farPlane = static_cast<float>(distance + radius * 2.0);
    return lens;
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
