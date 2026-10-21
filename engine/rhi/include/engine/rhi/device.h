// The RHI device and command interface (ADR 0005).
//
// One frame:
//
//     ICmdList* cmd = device.beginFrame();
//     const Swapchain target = device.acquireSwapchain(window);
//     if (target.texture.valid()) { cmd->beginRenderPass(...); ...; cmd->endRenderPass(); }
//     device.submitAndPresent();
//
// `acquireSwapchain` comes after `beginFrame` because every backend worth
// supporting acquires the backbuffer through a command buffer, and an
// invalid texture is a normal outcome -- a minimized window, or headless --
// not an error.
#pragma once

#include <optional>
#include <span>
#include <string>

#include "engine/core/error.h"
#include "engine/rhi/descs.h"
#include "engine/rhi/types.h"
#include "engine/rhi/xr.h"

namespace engine::platform {
class Window;
}

namespace engine::rhi {

struct DeviceDesc
{
    BackendId backend = BackendId::SdlGpu;
    // Turns on the backend's validation layer. Costly, and the first window's gate asks for
    // it to be clean, so it is on in dev builds and off in shipping.
    bool debug = false;
    // The blob format the shader pack will supply. A backend that cannot
    // consume it fails to create rather than failing at the first draw.
    ShaderFormat shaderFormat = ShaderFormat::Unknown;
    // **Ask the GPU for nothing it may lack**: the device a phone
    // without depth clamping is given, asked for outright -- what a test and
    // `--gpu-least` use to draw as such a phone does.
    bool leastFeatures = false;
    // **Which of the system's graphics APIs to draw through**, by the name the
    // platform library knows it by (`vulkan`, `direct3d12`, `metal`), or empty
    // for the library's own choice. Where the named one cannot be made the
    // library's choice is taken, and the log says which was.
    std::string driver = {};
    // **`driver` is a default, not somebody's order.** It is kept only when
    // the device it makes is on the card the system calls its strongest: a
    // machine whose stronger card that API cannot see would otherwise be
    // moved onto its weaker one for a default's sake, and draw at a fraction
    // of the rate with nothing wrong in the picture to say why.
    bool driverIfSameCard = false;
    // **A headset, when there is one** (ADR 0244): the device is made on the
    // adapter its runtime asks for, and `IDevice::headset` answers it.
    XrRequest xr = {};
    // **Where the pipelines a run made are kept for the next** (ADR 0257):
    // a file, UTF-8, the device reads as it is made and keeps as it runs;
    // empty keeps none. A Vulkan device's alone.
    std::string pipelineCacheFile = {};
    // **The threads its pipelines are made on** (ADR 0257): its own, apart
    // from the job system, so that a material is never made behind the
    // world's work; 0 is one on a machine of four hardware threads or fewer
    // and two above, and four at most.
    u32 pipelineThreads = 0;
};

// **What the device holds, in bytes**: every texture and every buffer made
// and not yet destroyed, as their descriptions size them. Not what the driver
// spends -- it pads, it keeps copies, it has pools of its own -- but the part
// of it a game chose: its pictures, with their mips, and its meshes.
struct DeviceMemory
{
    core::u64 textureBytes = 0;
    core::u32 textures = 0;
    core::u64 bufferBytes = 0;
    core::u32 buffers = 0;
};

// Queried, never assumed. Interfaces here are coarse by design (architecture.md
// §7): a capability exists when a render path gates on it, not before.
// **A pipeline's twin to make with it** (ADR 0257): the pass it will be drawn
// in -- its samples, a density map shading it, and the format of its first
// target (Undefined: the pipeline's own). What `warmPipeline` makes one at a
// time.
struct PipelineVariant
{
    u32 samples = 1;
    bool densityShaded = false;
    TextureFormat colorFormat = TextureFormat::Undefined;
};

struct Capabilities
{
    ShaderFormat shaderFormat = ShaderFormat::Unknown;
    u32 maxTextureSize = 0;
    // False on backends that record or discard rather than rasterize, which is
    // how a caller knows a readback would be meaningless.
    bool rendersPixels = false;
    // Compute passes, storage buffers and indirect draws (ADR 0116).
    bool compute = false;
    // Whether the GPU samples ASTC's four-by-four blocks (ADR 0180): a
    // phone's does, a desktop's mostly does not.
    bool astcTextures = false;
    // Whether it samples half-float colour in blocks (ADR 0253 §6): BC6H,
    // which every desktop GPU does, and ASTC's HDR profile, which a phone's
    // or a headset's may.
    bool bc6hTextures = false;
    bool astcHdrTextures = false;
    // Whether depth can be clamped where a pipeline does not clip it. A GPU
    // without it clips: what stands nearer a light than its map begins is
    // cut, where it was flattened onto the map's near plane.
    bool depthClamp = true;
    // **The area of a target one texel of a fragment density map covers**,
    // in pixels; nought where the device has no density map
    // (`TextureUsage::DensityMap`). A map is sized by it.
    u32 densityTexelWidth = 0;
    u32 densityTexelHeight = 0;
};

struct Swapchain
{
    TextureHandle texture{};
    u32 width = 0;
    u32 height = 0;
    TextureFormat format = TextureFormat::Undefined;
};

// Recorded inside a frame; never outlives the `submitAndPresent` that ends it.
// Not an owning type -- the device owns the recording -- so it is handed out by
// pointer and never stored.
class ICmdList
{
public:
    ICmdList() = default;
    virtual ~ICmdList() = default;

    ICmdList(const ICmdList&) = delete;
    ICmdList& operator=(const ICmdList&) = delete;

    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    virtual void setPipeline(PipelineHandle pipeline) = 0;
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect& scissor) = 0;

    virtual void bindVertexBuffers(u32 firstSlot, std::span<const BufferHandle> buffers) = 0;
    virtual void bindIndexBuffer(BufferHandle buffer, IndexType type) = 0;

    // Push-style: the data is copied into the frame's own storage, so the
    // caller's buffer may die on the next line. Sized in bytes rather than
    // typed, because the layout contract lives in the shader.
    virtual void bindUniforms(ShaderStage stage, u32 slot, std::span<const std::byte> data) = 0;
    virtual void bindTextures(ShaderStage stage, u32 firstSlot, std::span<const TextureBinding> bindings) = 0;

    virtual void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) = 0;
    virtual void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                             u32 firstInstance) = 0;

    // Staging is the backend's problem: it owns the transfer buffers, the copy
    // pass, and the decision about whether the write can be seen this frame.
    // Callers upload and draw; that is the whole point of the seam.
    virtual void upload(BufferHandle buffer, std::span<const std::byte> data, u32 offsetBytes) = 0;
    virtual void uploadTexture(TextureHandle texture, std::span<const std::byte> data, u32 mipLevel) = 0;
    // **One layer of an array, whole** (ADR 0253 §6): what puts a page of
    // blocks in its layer, where `blitTexture` draws -- and nothing draws into
    // a block format.
    virtual void uploadTextureLayer(TextureHandle texture, u32 layer, std::span<const std::byte> data) = 0;

    // A rectangle of mip 0, tightly packed rows of `width` texels (ADR 0071).
    //
    // **The one addition the GPU terrain needed**, and the reason it exists:
    // terrain heights live in an atlas the size of the whole world, and a brush
    // stroke changes a 32 by 32 tile of it. Re-uploading the atlas per stroke is
    // megabytes a frame to change kilobytes; this changes the kilobytes. A
    // rectangle that does not fit inside the texture is refused, silently, by
    // every backend -- the caller computed it wrong and a partial write would
    // hide that.
    virtual void uploadTextureRegion(TextureHandle texture, u32 x, u32 y, u32 width, u32 height,
                                     std::span<const std::byte> data) = 0;

    // **A texture copied into one layer of another, scaled to fit** (ADR
    // 0113): mip 0 to mip 0, filtered linearly. What builds a terrain's layer
    // arrays out of textures already on the GPU, whatever size or compression
    // they arrived in, so nothing is decoded twice. Outside a render pass; the
    // destination needs `ColorTarget` usage.
    virtual void blitTexture(TextureHandle source, TextureHandle destination, u32 destinationLayer) = 0;

    // **One level of a texture into one level of one layer of another** (ADR
    // 0202): what puts a map filtered level by level in a texture of its own
    // -- by compute, which writes a level of a plain texture and no layer of
    // an array -- into its layer of the array every probe's map is kept in.
    // Scaled to fit and filtered linearly, as `blitTexture` is; the two levels
    // are the same size wherever this is used, and it is then a copy. Outside
    // a render pass; the destination needs `ColorTarget` usage.
    virtual void blitTextureLevel(TextureHandle source, u32 sourceLevel, TextureHandle destination,
                                  u32 destinationLayer, u32 destinationLevel) = 0;

    // Every mip level after the first, filtered down from the first, on every
    // layer. Outside a render pass; the texture needs `Sampled` and
    // `ColorTarget` usage.
    virtual void generateMipmaps(TextureHandle texture) = 0;

    // Named regions in a GPU capture. Free in shipping builds, invaluable in
    // every other one.
    // --- Storage buffers, indirect draws and compute (ADR 0116) --------------
    //
    // A graphics stage reads storage buffers bound here, after its textures.
    virtual void bindStorageBuffers(ShaderStage stage, u32 firstSlot, std::span<const BufferHandle> buffers) = 0;
    // `drawCount` indexed draws, `DrawIndexedIndirectCommand`s read from
    // `buffer` at `offsetBytes` -- the arguments a compute pass wrote.
    virtual void drawIndexedIndirect(BufferHandle buffer, u32 offsetBytes, u32 drawCount) = 0;

    // **A compute pass**, which writes the buffers and the textures it names
    // here and no others; it ends any pass open, and a render or copy pass
    // ends it. A texture is written a mip at a time (ADR 0164), and is made
    // with `TextureUsage::ComputeStorageWrite`; the shader's first written
    // texture is the first named.
    virtual void beginComputePass(std::span<const BufferHandle> writes,
                                  std::span<const ComputeTextureWrite> textureWrites) = 0;
    void beginComputePass(std::span<const BufferHandle> writes) { beginComputePass(writes, {}); }
    virtual void endComputePass() = 0;
    virtual void setComputePipeline(ComputePipelineHandle pipeline) = 0;
    // The buffers the compute shader only reads.
    virtual void bindComputeStorageBuffers(u32 firstSlot, std::span<const BufferHandle> buffers) = 0;
    // **The textures a compute shader samples** (ADR 0160): each with its
    // sampler, from `firstSlot`, as a fragment stage's are. What lets a
    // particle be tested against the picture's depth, and a temporal upscaler
    // read the frame; before the buffers a shader reads, in its bindings.
    virtual void bindComputeTextures(u32 firstSlot, std::span<const TextureBinding> bindings) = 0;
    // **The textures a compute shader loads from, texel by texel** (ADR 0165):
    // no sampler, so an image of integers can be one. Each was made with
    // `TextureUsage::ComputeStorageRead`. A slot is counted from the first of
    // them, whatever the shader samples besides.
    virtual void bindComputeStorageTextures(u32 firstSlot, std::span<const TextureHandle> textures) = 0;
    virtual void bindComputeUniforms(u32 slot, std::span<const std::byte> data) = 0;
    virtual void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;

    virtual void pushDebugGroup(std::string_view name) = 0;
    virtual void popDebugGroup() = 0;
};

// **What one pass took of the GPU, in the last frame submitted** (ADR 0171).
// `name` is the debug group the pass was recorded in, and the pass's own name
// after a slash where the two differ ("forward/decals"); `submits` is how many
// times the frame was stopped to time it, each of which costs what the entry
// named "floor" took: a stop whose pass clears one pixel.
struct PassTime
{
    std::string_view name{};
    f64 milliseconds = 0.0;
    u32 submits = 0;
};

class IDevice
{
public:
    IDevice() = default;
    virtual ~IDevice() = default;

    IDevice(const IDevice&) = delete;
    IDevice& operator=(const IDevice&) = delete;

    [[nodiscard]] virtual BackendId backend() const noexcept = 0;
    [[nodiscard]] virtual Capabilities caps() const noexcept = 0;

    // A window must be claimed before its swapchain can be acquired, and
    // released before it is destroyed.
    [[nodiscard]] virtual bool claimWindow(platform::Window& window) = 0;
    virtual void releaseWindow(platform::Window& window) = 0;

    // **Whether a claimed window's frames wait for its display** (ADR 0147).
    // On: each present is shown on a refresh, and the frame waits for
    // one. Off: shown as soon as it is done, torn where it lands mid-refresh
    // -- what "VSync off" is in every engine -- or, where the backend
    // cannot tear, the newest frame on the next refresh. Answers whether the
    // window presents the way that was asked; a backend with no display has
    // nothing to set and says no.
    virtual bool setVSync(platform::Window& window, bool on)
    {
        (void)window;
        (void)on;
        return false;
    }
    // **A claimed window's swapchain made again**, presenting as it did:
    // what a window moved to another display needs. A swapchain is bound to
    // the display it was made on -- a window opened on a 100 Hz display and
    // dragged to a 240 Hz one went on presenting at 100, through Direct3D 12
    // and Vulkan alike. Waits for the device. Answers whether it was made.
    virtual bool remakeSwapchain(platform::Window& window)
    {
        (void)window;
        return false;
    }
    // How a claimed window presents after the last `setVSync`, and which
    // driver the device runs on ("vulkan", "direct3d12", "metal") -- the two
    // facts a report of a frame rate held to the display is answered from.
    [[nodiscard]] virtual PresentMode presentMode(platform::Window& window) const
    {
        (void)window;
        return PresentMode::None;
    }
    [[nodiscard]] virtual std::string_view driverName() const noexcept { return {}; }
    // What the driver calls the adapter it draws on -- a card's name, or a
    // software rasteriser's ("llvmpipe", "Microsoft Basic Render Driver"):
    // what tells a slow frame on a GPU from a frame drawn by the CPU. Empty
    // where the backend does not say.
    [[nodiscard]] virtual std::string_view adapterName() const noexcept { return {}; }
    // **The headset the device was made for** (ADR 0244), or null: none was
    // asked for, none was found, or the backend has none. The device's, and
    // gone with it.
    [[nodiscard]] virtual IHeadset* headset() noexcept { return nullptr; }

    [[nodiscard]] virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    [[nodiscard]] virtual ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;

    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(SamplerHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;
    virtual void destroy(ComputePipelineHandle handle) = 0;

    [[nodiscard]] virtual ICmdList* beginFrame() = 0;
    [[nodiscard]] virtual Swapchain acquireSwapchain(platform::Window& window) = 0;
    virtual void submitAndPresent() = 0;
    virtual void waitIdle() = 0;

    // Blocking, and slow on purpose: it waits for the GPU to go idle and copies
    // the whole texture back. This is the screenshot and golden-image path
    // (the first window's "eyes"), not something a frame may call. Streaming
    // readback would need a fence-based API and has no consumer yet.
    //
    // `out` must be at least width * height * bytesPerPixel of the texture's
    // format; false means the backend cannot read this texture back.
    [[nodiscard]] virtual bool readTexture(TextureHandle texture, std::span<std::byte> out) = 0;

    // **A pipeline's twin made ahead of its first draw** (a headset's eye,
    // behind the loading curtain): the one a pass of `samples` samples, shaded
    // through a density map or not, into a first target of `colorFormat`
    // (Undefined: the pipeline's own) draws it with. A backend that makes no
    // twins has nothing to make.
    virtual void warmPipeline(PipelineHandle pipeline, u32 samples, bool densityShaded, TextureFormat colorFormat)
    {
        (void)pipeline;
        (void)samples;
        (void)densityShaded;
        (void)colorFormat;
    }

    // **A pipeline made off the frame** (ADR 0257): the handle is good at
    // once, and what it names is made on a job -- the driver's compile, which
    // for a material's pipeline is tenths of a second on a cold cache -- with
    // the twins `variants` name. Until `pipelineMade` says true, binding it
    // binds nothing: the caller draws without it. A backend that cannot makes
    // it here and now, which is what this does.
    [[nodiscard]] virtual PipelineHandle createGraphicsPipelineOffFrame(const GraphicsPipelineDesc& desc,
                                                                        std::span<const PipelineVariant> variants)
    {
        const PipelineHandle made = createGraphicsPipeline(desc);
        for (const PipelineVariant& variant : variants) {
            if (made.valid())
                warmPipeline(made, variant.samples, variant.densityShaded, variant.colorFormat);
        }
        return made;
    }

    // Whether a pipeline is made: true, still being made (nothing), or
    // refused (false, and said). Asked on the device's own thread, which is
    // where what a job made is taken in.
    [[nodiscard]] virtual std::optional<bool> pipelineMade(PipelineHandle pipeline) { return pipeline.valid(); }

    // **The pipelines made so far, kept on disk now** (ADR 0257): what an
    // app calls as it goes to the background or loses focus -- a phone or a
    // headset may end it at any moment after, and never say. A device that
    // keeps none has nothing to do.
    virtual void keepPipelines() {}

    // **A texture's texels a few frames later, never waiting** (a headset
    // eye's exposure, folded into its forward pass): the top-left `width` by
    // `height` copied in the frame being recorded, and handed back once the
    // GPU is known to have finished that frame -- a frame or three on. A
    // ticket, nought where the backend cannot; `takeReadback` says false until
    // the copy is there, then fills `out` and frees the ticket.
    [[nodiscard]] virtual u32 requestReadback(TextureHandle texture, u32 width, u32 height)
    {
        (void)texture;
        (void)width;
        (void)height;
        return 0;
    }
    [[nodiscard]] virtual bool takeReadback(u32 ticket, std::span<std::byte> out)
    {
        (void)ticket;
        (void)out;
        return false;
    }

    // The same for a buffer: `out.size()` bytes from `offsetBytes`. Blocking
    // and for tests and tools -- what a compute pass wrote, checked by value.
    [[nodiscard]] virtual bool readBuffer(BufferHandle buffer, u32 offsetBytes, std::span<std::byte> out) = 0;

    // **A frame timed pass by pass** (ADR 0171). The backend has no timer on
    // the GPU to read, so a timed frame is recorded as one command buffer per
    // pass and each is waited for: the GPU is idle when a pass starts and the
    // wait is what the pass took. It is a measuring mode -- the frame rate under
    // it is not the game's -- and it takes effect at the next `beginFrame`.
    // Passes drawn to a window's own texture cannot leave the frame's buffer,
    // so they are timed together, as one entry named after all of them.
    // Nothing on a backend that draws nothing.
    virtual void setPassTiming(bool on) { (void)on; }
    // The last frame's, in the order the passes ran; empty when timing is off.
    // Valid until the next `submitAndPresent`.
    [[nodiscard]] virtual std::span<const PassTime> passTimes() const noexcept { return {}; }

    // Nothing on a backend that holds nothing.
    [[nodiscard]] virtual DeviceMemory memory() const noexcept { return {}; }

    // **Whether the device is gone**: the driver reset it, most often because
    // a shader ran past the time the operating system allows one. Once true
    // it stays true -- every create answers an empty handle, every frame is
    // empty, every destroy is skipped -- because nothing may reach a driver
    // that has let go of the device; the backend crashed inside itself when
    // anything did. The only way back is a new device.
    [[nodiscard]] virtual bool lost() const noexcept { return false; }
    // What the driver said when it was lost, as the backend heard it -- its
    // own code is in it (`0x887A0005` and its kin, `VK_ERROR_DEVICE_LOST`).
    // Empty while it is not lost, and for a loss that was simulated here.
    [[nodiscard]] virtual std::string_view lostReason() const noexcept { return {}; }
    // **Asks the driver itself whether the device is gone**, where a
    // backend can: `lost` is what this backend has HEARD, from a call that
    // failed with the driver's code in its message, and a call that fails
    // without that -- or whose failure nobody looked at -- leaves a lost
    // device unknown. Whoever is about to report a failure of any kind asks
    // this first. The same answer as `lost` afterwards.
    [[nodiscard]] virtual bool askIfLost() noexcept { return lost(); }
    // Loses the device the way the driver would, for the test that the engine
    // survives it. Nothing on a backend that cannot be lost.
    virtual void simulateLoss() noexcept {}
    // **Takes the device away for real, underneath the backend**: what
    // a hosted runner's software device did to three test hosts -- the
    // driver's own removal, which the backend learns of from the first call
    // that fails and not from being told. `simulateLoss` marks the device
    // lost here; this leaves every layer below to find out, which is the path
    // that crashed. Where a backend has no way to do it, the simulated loss.
    virtual void removeDevice() noexcept { simulateLoss(); }
};

} // namespace engine::rhi
