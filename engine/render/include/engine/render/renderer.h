// The renderer contract (ADR 0027).
//
// A renderer consumes a `RenderWorld` and an `rhi::IDevice` and nothing else.
// That boundary is the whole decision: the renderer architecture -- forward,
// deferred, GPU-driven -- is a separate axis of replaceability from the graphics
// API, and this is the seam for it. An alternative renderer is a build-time
// selection like any backend (ADR 0023) and may not reach into `scene` or into
// `rhi` internals.
//
// ADR 0027's sketch is `render(IDevice&, const RenderWorld&)`. The signature
// here carries two more things the sketch predates: the command list, because
// `app` owns the frame and a renderer that opened its own could not share one
// with the debug overlay; and the target, because headless renders into an
// offscreen texture and windowed into a swapchain image, and which one is the
// host's business rather than the renderer's.
#pragma once

#include <array>
#include <optional>

#include "engine/core/error.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/render/settings.h"
#include "engine/render/shader_library.h"
#include "engine/rhi/device.h"

namespace engine::render {

class ISurfaceSource;

struct RenderTarget
{
    rhi::TextureHandle color{};
    rhi::TextureFormat colorFormat = rhi::TextureFormat::Undefined;
    core::u32 width = 0;
    core::u32 height = 0;
    // **Which view this frame continues** (ADR 0107). A renderer remembers a
    // view from frame to frame -- the exposure it adapted to, the shadow fit
    // it keeps, the targets sized to it -- and a camera drawing into a texture
    // is a second view with a history of its own. The main view is 0, and a
    // frame that draws only it behaves exactly as before views existed.
    core::u32 view = 0;
};

// **What a draw call was for.** A frame's count is one number, and the
// question a slow frame asks is which pass and which kind of thing made it:
// a horde drawn four times over into the sun's cascades reads the same, as a
// total, as four times the effects on screen.
//
// The mesh passes come first, each the same geometry submitted for another
// reason; `Mesh` and `MeshRun` are the picture itself, one object to a call
// and a run of them to a call; the rest are the kinds that are not meshes.
enum class DrawKind : core::u8
{
    SunShadow,
    LocalShadow,
    Prepass,
    DecalMask,
    Mesh,
    MeshRun,
    Blended,
    Outline,
    Highlight,
    Velocity,
    Foliage,
    Decal,
    Sprite,
    Ribbon,
    Particle,
    WorldUi,
    Count,
};

inline constexpr core::usize DrawKindCount = static_cast<core::usize>(DrawKind::Count);

// The name a report writes a kind by.
[[nodiscard]] constexpr const char* drawKindName(DrawKind kind) noexcept
{
    constexpr std::array<const char*, DrawKindCount> Names{
        "sun_shadow", "local_shadow", "prepass", "decal_mask", "mesh",   "mesh_run", "blended",  "outline",
        "highlight",  "velocity",     "foliage", "decal",      "sprite", "ribbon",   "particle", "world_ui",
    };
    return Names[static_cast<core::usize>(kind)];
}

// The counters a frame leaves behind, for `DebugService` and the perf table.
struct RendererStats
{
    // Calls issued: `drawIndexed` and `draw`, across every pass of the frame.
    core::u32 drawCalls = 0;
    // The same calls by what each was for: these sum to `drawCalls`.
    std::array<core::u32, DrawKindCount> drawsByKind{};
    // How many of those were instanced, and how many objects they covered. A
    // frame where `instances` is far larger than `instancedDraws` is a frame the
    // instanced path did something in; a frame where they are equal is one where
    // it did not, which is the gate's own test.
    core::u32 instancedDraws = 0;
    core::u32 instances = 0;
};

class IRenderer
{
public:
    virtual ~IRenderer() = default;

    IRenderer(const IRenderer&) = delete;
    IRenderer& operator=(const IRenderer&) = delete;

    // Built against one colour format, because a graphics pipeline is. A caller
    // that renders into two formats needs two renderers, which is honest: they
    // are two sets of pipelines whatever the API pretends.
    [[nodiscard]] virtual std::optional<core::EngineError> create(rhi::IDevice& device, const ShaderLibrary& shaders,
                                                                  rhi::TextureFormat colorFormat) = 0;

    virtual void destroy(rhi::IDevice& device) = 0;

    // Records the frame. Must be called with no render pass open; the renderer
    // opens and closes its own, and leaves none open on return, because the
    // overlay draws after it into the same command list.
    virtual void render(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target, const RenderWorld& world,
                        const MeshCache& meshes) = 0;

    // Forgets a view other than the main one, and frees what it held. A view
    // nothing draws any more -- a camera texture destroyed, a sub-world
    // unloaded -- would otherwise keep a full set of screen-sized targets.
    // Call it outside a frame's command list, or after the list is submitted.
    virtual void releaseView(rhi::IDevice& device, core::u32 view)
    {
        (void)device;
        (void)view;
    }

    // False before `create` succeeds, and after a `create` that failed. A
    // renderer that never started must be skippable rather than fatal: a machine
    // whose content directory is missing its shaders should boot and say why.
    [[nodiscard]] virtual bool valid() const noexcept = 0;

    // The radius around the camera this renderer's shadow map covers, in
    // metres. `extract` needs it to decide which off-screen geometry is still
    // worth keeping as a caster, and the number belongs to the pass list rather
    // than to the host -- a renderer with cascades would answer differently.
    [[nodiscard]] virtual core::f32 shadowRadius() const noexcept = 0;

    // The quality family this renderer honours (roadmap M8, ADR 0044). Applying
    // one may rebuild targets, which is why it is a call rather than a field
    // read every frame: the shadow atlas is sized by it.
    //
    // Here rather than on `RenderWorld` because a settings value is not scene
    // state. `extract` produces what the world contains; this is what the
    // machine can afford, and mixing the two would put a stranger's GPU budget
    // into the scene that gets hashed.
    virtual void setSettings(const GraphicsSettings& settings) = 0;

    // **Where the next frame of `world` into a target this size is sampled**
    // (ADR 0158), in NDC units: what `jitterCamera` folds into the camera
    // between extraction and `render`. Zero when this frame of the main view
    // draws nothing temporal.
    [[nodiscard]] virtual core::Vec2 cameraJitter(const RenderWorld& world, core::u32 targetWidth,
                                                  core::u32 targetHeight) const
    {
        (void)world;
        (void)targetWidth;
        (void)targetHeight;
        return {};
    }

    // Where surface shaders named by URN come from (ADR 0091): the editor's
    // compiler, or a packaged game's pack. Null -- the default -- draws every
    // such surface with the built-in one. Not owned.
    // **Whether a frame is to be made between this one and the last**
    // (ADR 0165): the setting, a device that can, and a world it can be made
    // of. The host then draws the world into a picture of its own -- two, by
    // turns -- where it would have drawn the window, shows the made frame
    // first and the drawn one after, and draws the interface on each.
    [[nodiscard]] virtual bool generatesFrames(const RenderWorld& world) const noexcept
    {
        (void)world;
        return false;
    }
    // **The frame between `previous` and `current`**, two pictures of the
    // world's main view of `width` by `height`, the second the one `render`
    // has just drawn. Nothing where there is no frame to make: the first
    // picture after the setting came on or the camera was cut, and a device
    // that cannot. The picture answered is the renderer's, and is this
    // frame's until the next call.
    [[nodiscard]] virtual rhi::TextureHandle interpolateFrame(rhi::IDevice& device, rhi::ICmdList& cmd,
                                                              rhi::TextureHandle previous, rhi::TextureHandle current,
                                                              core::u32 width, core::u32 height)
    {
        (void)device;
        (void)cmd;
        (void)previous;
        (void)current;
        (void)width;
        (void)height;
        return {};
    }
    // **A picture of the world, onto a target of its size** (ADR 0165): how a
    // frame drawn into a picture, or made between two, reaches the window.
    virtual void showPicture(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture,
                             const RenderTarget& target)
    {
        (void)device;
        (void)cmd;
        (void)picture;
        (void)target;
    }
    virtual void setSurfaceSource(ISurfaceSource* source) { (void)source; }
    [[nodiscard]] virtual const GraphicsSettings& settings() const noexcept = 0;

    // **Makes now what a game first asks for in the middle of play** (ADR
    // 0176): the pipelines of particles, beams and trails, decals, the world's
    // interface, highlights and skinned runs, each of which is otherwise made
    // the first frame something of its kind is drawn -- a first shot's
    // sparks, a first name tag -- and on a phone that frame is tens of
    // milliseconds a pipeline. Called behind a loading curtain. What the scene
    // itself shows is made by drawing it there; this is for what it will show.
    virtual void warm(rhi::IDevice& device) { (void)device; }

    // What the last frame actually submitted.
    //
    // Counted by the renderer rather than derived from the snapshot, and that
    // distinction IS the number: with instancing, a run of objects that share a
    // mesh and a material is one call, so "how many objects are visible" and
    // "how many calls were issued" stopped being the same question at M7.5.
    // The roadmap's gate for that item is exactly the two of them side by side.
    [[nodiscard]] virtual RendererStats stats() const noexcept = 0;

protected:
    IRenderer() = default;
};

// The v1 renderer (ADR 0027): sun shadow map -> sky -> forward PBR -> tonemap.
// Single cascade, a bounded light count per draw, and no clustered culling --
// each of those is a named non-goal of M4 rather than an omission.
[[nodiscard]] std::unique_ptr<IRenderer> createDefaultRenderer();

} // namespace engine::render
