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

#include <span>

#include "engine/core/error.h"
#include "engine/rhi/descs.h"
#include "engine/rhi/types.h"

namespace engine::platform {
class Window;
}

namespace engine::rhi {

struct DeviceDesc
{
    BackendId backend = BackendId::SdlGpu;
    // Turns on the backend's validation layer. Costly, and the M1 gate asks for
    // it to be clean, so it is on in dev builds and off in shipping.
    bool debug = false;
    // The blob format the shader pack will supply. A backend that cannot
    // consume it fails to create rather than failing at the first draw.
    ShaderFormat shaderFormat = ShaderFormat::Unknown;
};

// Queried, never assumed. Interfaces here are coarse by design (architecture.md
// §7): a capability exists when a render path gates on it, not before.
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

    // **Whether a claimed window's frames wait for its display** (ADR 0147,
    // G0). On: each present is shown on a refresh, and the frame waits for
    // one. Off: shown as soon as it is done, torn where it lands mid-refresh
    // -- what "VSync off" is in every engine (D528) -- or, where the backend
    // cannot tear, the newest frame on the next refresh. Answers whether the
    // window presents the way that was asked; a backend with no display has
    // nothing to set and says no.
    virtual bool setVSync(platform::Window& window, bool on)
    {
        (void)window;
        (void)on;
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
    // (roadmap M1's "agent eyes"), not something a frame may call. Streaming
    // readback would need a fence-based API and has no consumer yet.
    //
    // `out` must be at least width * height * bytesPerPixel of the texture's
    // format; false means the backend cannot read this texture back.
    [[nodiscard]] virtual bool readTexture(TextureHandle texture, std::span<std::byte> out) = 0;

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

    // **Whether the device is gone**: the driver reset it, most often because
    // a shader ran past the time the operating system allows one. Once true
    // it stays true -- every create answers an empty handle, every frame is
    // empty, every destroy is skipped -- because nothing may reach a driver
    // that has let go of the device; the backend crashed inside itself when
    // anything did. The only way back is a new device.
    [[nodiscard]] virtual bool lost() const noexcept { return false; }
    // Loses the device the way the driver would, for the test that the engine
    // survives it. Nothing on a backend that cannot be lost.
    virtual void simulateLoss() noexcept {}
};

} // namespace engine::rhi
