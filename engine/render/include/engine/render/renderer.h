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
#include <string_view>

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
    // **A part of the window** (ADR 0205): one local player's view of a screen
    // shared between several. Drawn as the main view is -- at the settings'
    // render scale, smoothed by the temporal pass with a history of its own --
    // and not as a picture shown through the interface, which is what every
    // other view but the main one is. Not upscaled by FSR 2, and not a frame
    // others are made between: those keep one history, the main view's.
    bool window = false;
    // Whether this is the window's first part drawn in a frame that does not
    // draw the main view at all: what a frame does ONCE -- the particles on
    // the GPU stepped -- is then done in it.
    bool stepsFrame = false;
    // **The side of a line this view is seen on** (ADR 0205, two views put
    // together across a line through the window's middle). In pixels of the
    // target measured from its middle, y down: the view is kept where
    // `dot(sideNormal, p) <= sideMargin`, and nothing is lit beyond. Zero is
    // all of the target.
    core::Vec2 sideNormal{};
    // How many pixels past the line are still drawn: the line's own half
    // width, and what a filter reads across it.
    core::f32 sideMargin = 0.0f;
};

// **One player's view and where it goes in the window** (ADR 0205): a picture
// the size of its rectangle, in pixels of the target.
struct ComposedView
{
    rhi::TextureHandle picture{};
    core::u32 x = 0;
    core::u32 y = 0;
    core::u32 width = 0;
    core::u32 height = 0;
};

// **A window made of several views** (ADR 0205).
struct ViewComposite
{
    std::array<ComposedView, 4> views{};
    core::u32 count = 0;
    // Two views the size of the target, put together across a line through
    // its middle: the first on the side `lineNormal` points away from, the
    // second on the side it points to. Otherwise each view is its rectangle.
    bool acrossLine = false;
    core::Vec2 lineNormal{1.0f, 0.0f};
    // The line between views, in pixels of the target; 0 is none.
    core::f32 lineThickness = 0.0f;
    core::Color3 lineColor{};
    // The whole window's one picture, shown over the views by `wholeWeight`
    // (0 to 1): how a screen that parts into a grid fades into it.
    rhi::TextureHandle whole{};
    core::f32 wholeWeight = 0.0f;
};

// **What a draw call was for.** A frame's count is one number, and the
// question a slow frame asks is which pass and which kind of thing made it:
// a horde drawn four times over into the sun's cascades reads the same, as a
// total, as four times the effects on screen.
//
// The mesh passes come first, each the same geometry submitted for another
// reason; `Mesh` and `MeshRun` are the picture itself, one object to a call
// and a run of them to a call, and `Blended` and `BlendedRun` the same for
// what is less than opaque; the rest are the kinds that are not meshes.
enum class DrawKind : core::u8
{
    SunShadow,
    LocalShadow,
    Prepass,
    DecalMask,
    Mesh,
    MeshRun,
    Blended,
    BlendedRun,
    Outline,
    Highlight,
    Velocity,
    Foliage,
    Decal,
    Sprite,
    Ribbon,
    Particle,
    WorldUi,
    // A body with a morph target above nought, in the camera's lit passes
    // (ADR 0196): drawn alone, out of the run its mesh would be in. Its
    // shadow and prepass draws are counted with those passes.
    Morph,
    Count,
};

inline constexpr core::usize DrawKindCount = static_cast<core::usize>(DrawKind::Count);

// The name a report writes a kind by.
[[nodiscard]] constexpr const char* drawKindName(DrawKind kind) noexcept
{
    constexpr std::array<const char*, DrawKindCount> Names{
        "sun_shadow", "local_shadow", "prepass", "decal_mask", "mesh",     "mesh_run",
        "blended",    "blended_run",  "outline", "highlight",  "velocity", "foliage",
        "decal",      "sprite",       "ribbon",  "particle",   "world_ui", "morph",
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

// The part of a picture a copy takes, in parts of its width and height.
struct PictureCrop
{
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
    core::f32 width = 1.0f;
    core::f32 height = 1.0f;
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

    // --- What a room reflects (ADR 0202) -------------------------------------
    //
    // Whether this renderer holds a map for the probe `key` names: the
    // `RenderReflectionProbe` of that key is then read by what is drawn, and
    // is the sky until it is.
    [[nodiscard]] virtual bool holdsReflectionProbe(core::u64 key) const noexcept
    {
        (void)key;
        return false;
    }
    // Makes the probe's map out of six pictures of its surroundings, each
    // drawn by `render` with `RenderWorld::capture` set, and the camera
    // matrices they were drawn with. Outside a render pass. False when no map
    // can be kept: the setting is off, or every one is in use this frame.
    virtual bool storeReflectionProbe(rhi::IDevice& device, rhi::ICmdList& cmd, core::u64 key,
                                      std::span<const rhi::TextureHandle, 6> faces,
                                      std::span<const core::Mat4, 6> viewProjections)
    {
        (void)device;
        (void)cmd;
        (void)key;
        (void)faces;
        (void)viewProjections;
        return false;
    }
    // Forgets it: a probe destroyed or turned off gives its map to another.
    virtual void dropReflectionProbe(core::u64 key) { (void)key; }

    // False before `create` succeeds, and after a `create` that failed. A
    // renderer that never started must be skippable rather than fatal: a machine
    // whose content directory is missing its shaders should boot and say why.
    [[nodiscard]] virtual bool valid() const noexcept = 0;

    // The radius around the camera this renderer's shadow map covers, in
    // metres. `extract` needs it to decide which off-screen geometry is still
    // worth keeping as a caster, and the number belongs to the pass list rather
    // than to the host -- a renderer with cascades would answer differently.
    [[nodiscard]] virtual core::f32 shadowRadius() const noexcept = 0;

    // The quality family this renderer honours (ADR 0044). Applying
    // one may rebuild targets, which is why it is a call rather than a field
    // read every frame: the shadow atlas is sized by it.
    //
    // Here rather than on `RenderWorld` because a settings value is not scene
    // state. `extract` produces what the world contains; this is what the
    // machine can afford, and mixing the two would put a stranger's GPU budget
    // into the scene that gets hashed.
    virtual void setSettings(const GraphicsSettings& settings) = 0;

    // **Where the next frame of `world` into `target` is sampled** (ADR
    // 0158), in NDC units: what `jitterCamera` folds into the camera between
    // extraction and `render`. Zero when this frame of that view draws
    // nothing temporal -- and for every view that is neither the main one nor
    // a part of the window. Each view counts its own frames (ADR 0205).
    [[nodiscard]] virtual core::Vec2 cameraJitter(const RenderWorld& world, const RenderTarget& target) const
    {
        (void)world;
        (void)target;
        return {};
    }
    // **An eye's picture into the headset's image** (ADR 0244): a view drawn
    // for the eye, copied into the runtime's swapchain image `target` --
    // decoded first where that image is sRGB, since a runtime reads one as
    // linear light. `crop` is the part of the picture copied, in parts of its
    // width and height: the whole of it into the headset, the middle of it
    // into a window of another shape (the mirror). Inside the frame, with no
    // render pass open.
    // **An eye's depth into the headset's depth image** (ADR 0244 §5): the
    // depth the view last drawn was drawn with, into `target`, a depth image
    // (`target.colorFormat` its depth format). Inside the frame, with no
    // render pass open.
    virtual void presentEyeDepth(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target)
    {
        (void)device;
        (void)cmd;
        (void)target;
    }
    virtual void presentEye(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture,
                            const RenderTarget& target, const PictureCrop& crop)
    {
        (void)device;
        (void)cmd;
        (void)picture;
        (void)target;
        (void)crop;
    }
    // **Several players' views made into one window** (ADR 0205): each
    // picture to its rectangle, or two across a line, with the line drawn.
    virtual void composeViews(rhi::IDevice& device, rhi::ICmdList& cmd, const ViewComposite& views,
                              const RenderTarget& target)
    {
        (void)device;
        (void)cmd;
        (void)views;
        (void)target;
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
    // interface, highlights, skinned runs and morphing bodies, each of which is otherwise made
    // the first frame something of its kind is drawn -- a first shot's
    // sparks, a first name tag -- and on a phone that frame is tens of
    // milliseconds a pipeline. Called behind a loading curtain. What the scene
    // itself shows is made by drawing it there; this is for what it will show.
    virtual void warm(rhi::IDevice& device) { (void)device; }

    // Explicit content preloading includes a user's surface pipelines, not
    // just its bytecode. Null means pending, false a terminal failure. The
    // resulting pipelines are shared with normal rendering and live until
    // the source revision changes. Call behind a loading curtain.
    [[nodiscard]] virtual std::optional<bool> warmSurface(rhi::IDevice& device, std::string_view name)
    {
        (void)device;
        (void)name;
        return false;
    }

    // What the last frame actually submitted.
    //
    // Counted by the renderer rather than derived from the snapshot, and that
    // distinction IS the number: with instancing, a run of objects that share a
    // mesh and a material is one call, so "how many objects are visible" and
    // "how many calls were issued" stopped being the same question with the lighting work.
    // The roadmap's gate for that item is exactly the two of them side by side.
    [[nodiscard]] virtual RendererStats stats() const noexcept = 0;

protected:
    IRenderer() = default;
};

// The v1 renderer (ADR 0027): sun shadow map -> sky -> forward PBR -> tonemap.
// Single cascade, a bounded light count per draw, and no clustered culling --
// each of those is a named non-goal of the first renderer rather than an omission.
[[nodiscard]] std::unique_ptr<IRenderer> createDefaultRenderer();

} // namespace engine::render
