// The SDL3 GPU backend: the v1 default (ADR 0005).
//
// The seam was shaped to sit close to SDL_GPU so this adapter stays thin, and
// mostly it is. Two places it is not, and both are the adapter absorbing a cost
// so callers do not pay it:
//
//   Passes. SDL_GPU has mutually exclusive render, compute and copy passes;
//   the seam has one command list. Uploading opens a copy pass lazily and
//   beginRenderPass closes it, so "upload, then draw" works the way it reads.
//
//   Staging. `upload` takes bytes. The transfer buffer, the map, the copy pass
//   and the release all happen here, because a caller that had to know about
//   them would be writing SDL_GPU code through a wrapper.

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/platform/sdl_interop.h"
#include "engine/rhi/backends.h"
#include "engine/rhi/sdlgpu_interop.h"
#include "sdlgpu_enums.h"

namespace engine::rhi {
namespace {

using sdlgpu::bytesPerPixel;
using sdlgpu::fromSdl;
using sdlgpu::fromSdlShaderFormats;
using sdlgpu::toSdl;

// Ids are the slot index plus one, and a destroyed slot is never reused. That
// costs a pointer per dead resource for the life of the device and buys the
// property that a stale handle resolves to null rather than to whatever
// resource landed in the recycled slot -- a silently wrong texture is far worse
// to debug than a missing one.
template <class T>
[[nodiscard]] u32 addSlot(std::vector<T>& table, T value)
{
    table.push_back(value);
    return static_cast<u32>(table.size());
}

template <class T>
[[nodiscard]] T* slot(std::vector<T>& table, u32 id) noexcept
{
    return (id != 0 && id <= table.size()) ? &table[id - 1] : nullptr;
}

// **A bind call's native list, on the stack** (audit R6): every bind made a
// `std::vector`, and a frame of a busy scene makes thousands of binds. Sixteen
// covers every slot count a backend allows a stage; a longer list, which no
// caller makes, spills to the heap rather than being cut.
template <class T>
class BindList
{
public:
    explicit BindList(usize count) : m_count(count)
    {
        if (count > m_inline.size())
            m_spill.resize(count);
    }
    [[nodiscard]] T* data() noexcept { return m_spill.empty() ? m_inline.data() : m_spill.data(); }
    [[nodiscard]] T& operator[](usize index) noexcept { return data()[index]; }
    [[nodiscard]] usize size() const noexcept { return m_count; }
    void shrink(usize count) noexcept { m_count = count; }

private:
    std::array<T, 16> m_inline{};
    std::vector<T> m_spill;
    usize m_count = 0;
};

struct TextureEntry
{
    SDL_GPUTexture* texture = nullptr;
    TextureFormat format = TextureFormat::Undefined;
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;
    // Swapchain textures belong to SDL and must not be released. Their slot is
    // reused every frame, which is also what keeps the table from growing once
    // per frame forever.
    bool owned = true;
};

class SdlGpuDevice;

class SdlGpuCmdList final : public ICmdList
{
public:
    explicit SdlGpuCmdList(SdlGpuDevice& device) noexcept : device_(device) {}

    void begin(SDL_GPUCommandBuffer* buffer) noexcept;
    // Releases the staging buffer. Called by the device before it goes.
    void releaseStaging() noexcept;
    [[nodiscard]] SDL_GPUCommandBuffer* buffer() const noexcept { return buffer_; }
    [[nodiscard]] SDL_GPURenderPass* renderPass() const noexcept { return renderPass_; }

    // Closes whatever pass is open. Called before a pass of a different kind
    // starts and before submit, so no caller has to track pass state.
    void endOpenPass() noexcept;

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void setPipeline(PipelineHandle pipeline) override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect& scissor) override;

    void bindVertexBuffers(u32 firstSlot, std::span<const BufferHandle> buffers) override;
    void bindIndexBuffer(BufferHandle buffer, IndexType type) override;

    void bindUniforms(ShaderStage stage, u32 slotIndex, std::span<const std::byte> data) override;
    void bindTextures(ShaderStage stage, u32 firstSlot, std::span<const TextureBinding> bindings) override;

    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) override;

    void upload(BufferHandle buffer, std::span<const std::byte> data, u32 offsetBytes) override;
    void uploadTexture(TextureHandle texture, std::span<const std::byte> data, u32 mipLevel) override;
    void uploadTextureRegion(TextureHandle texture, u32 x, u32 y, u32 width, u32 height,
                             std::span<const std::byte> data) override;
    void blitTexture(TextureHandle source, TextureHandle destination, u32 destinationLayer) override;
    void generateMipmaps(TextureHandle texture) override;

    void bindStorageBuffers(ShaderStage stage, u32 firstSlot, std::span<const BufferHandle> buffers) override;
    void drawIndexedIndirect(BufferHandle buffer, u32 offsetBytes, u32 drawCount) override;
    void beginComputePass(std::span<const BufferHandle> writes,
                          std::span<const ComputeTextureWrite> textureWrites) override;
    void endComputePass() override;
    void setComputePipeline(ComputePipelineHandle pipeline) override;
    void bindComputeStorageBuffers(u32 firstSlot, std::span<const BufferHandle> buffers) override;
    void bindComputeTextures(u32 firstSlot, std::span<const TextureBinding> bindings) override;
    void bindComputeUniforms(u32 slot, std::span<const std::byte> data) override;
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;

    void pushDebugGroup(std::string_view name) override;
    void popDebugGroup() override;

    // **Drops the frame without a word to the driver**, for a lost device:
    // ending a pass or releasing a buffer is a call into a backend that has
    // already failed. Every method after this is a no-op until `begin`.
    void abandon() noexcept
    {
        renderPass_ = nullptr;
        copyPass_ = nullptr;
        computePass_ = nullptr;
        buffer_ = nullptr;
        staging_ = nullptr;
        stagingCapacity_ = 0;
    }

private:
    [[nodiscard]] SDL_GPUCopyPass* ensureCopyPass() noexcept;

    // Where one upload's bytes were put: a range of the frame's staging buffer,
    // or -- for an upload that does not fit it -- a transfer buffer of its own,
    // which `releaseStaged` gives back.
    struct Staged
    {
        SDL_GPUTransferBuffer* transfer = nullptr;
        u32 offset = 0;
        bool owned = false;
    };
    [[nodiscard]] Staged stage(std::span<const std::byte> data, u32 alignment) noexcept;
    void releaseStaged(const Staged& staged) noexcept;

    SdlGpuDevice& device_;
    SDL_GPUCommandBuffer* buffer_ = nullptr;
    SDL_GPURenderPass* renderPass_ = nullptr;
    SDL_GPUCopyPass* copyPass_ = nullptr;
    SDL_GPUComputePass* computePass_ = nullptr;
    // What the open render pass has bound, so a bind of the same is skipped
    // (audit R6). Forgotten at every pass, since a pass starts with nothing.
    std::array<SDL_GPUBuffer*, 16> boundVertex_{};
    SDL_GPUBuffer* boundIndex_ = nullptr;
    IndexType boundIndexType_ = IndexType::U16;

    // **One transfer buffer for a frame's uploads, not one per upload.** Every
    // `upload` used to create a transfer buffer and release it: an allocation
    // and a release of a driver resource per call, and a frame of a lit city
    // makes a few dozen -- measured at 0.37 ms for three light-table textures
    // of 90 KB, and more again at submit, where SDL frees them. Now a frame
    // writes each upload at the next aligned offset of one buffer, and the
    // first write of a frame CYCLES it, which is SDL's guarantee that bytes a
    // frame still in flight is reading are never overwritten.
    SDL_GPUTransferBuffer* staging_ = nullptr;
    u32 stagingCapacity_ = 0;
    u32 stagingUsed_ = 0;
    // What the last frame asked for in all, so the next one starts big enough.
    u32 stagingWanted_ = 0;
    bool stagingCycled_ = false;
};

class SdlGpuDevice final : public IDevice
{
public:
    SdlGpuDevice(SDL_GPUDevice* device, ShaderFormat shaderFormat) noexcept
        : device_(device), shaderFormat_(shaderFormat), cmdList_(*this)
    {}

    ~SdlGpuDevice() override
    {
        // A lost device is left as it is: releasing into it is exactly the
        // kind of call that crashed inside the backend. The process is about
        // to end, and the operating system takes it all back.
        if (device_ == nullptr || lost_)
            return;

        SDL_WaitForGPUIdle(device_);
        releaseInFlight();
        cmdList_.releaseStaging();

        for (SDL_GPUGraphicsPipeline* pipeline : pipelines_)
            if (pipeline != nullptr)
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        for (SDL_GPUComputePipeline* pipeline : computePipelines_)
            if (pipeline != nullptr)
                SDL_ReleaseGPUComputePipeline(device_, pipeline);
        for (SDL_GPUShader* shader : shaders_)
            if (shader != nullptr)
                SDL_ReleaseGPUShader(device_, shader);
        for (SDL_GPUSampler* sampler : samplers_)
            if (sampler != nullptr)
                SDL_ReleaseGPUSampler(device_, sampler);
        for (const TextureEntry& entry : textures_)
            if (entry.texture != nullptr && entry.owned)
                SDL_ReleaseGPUTexture(device_, entry.texture);
        for (SDL_GPUBuffer* buffer : buffers_)
            if (buffer != nullptr)
                SDL_ReleaseGPUBuffer(device_, buffer);
        if (fallbackTexture_ != nullptr)
            SDL_ReleaseGPUTexture(device_, fallbackTexture_);
        if (fallbackSampler_ != nullptr)
            SDL_ReleaseGPUSampler(device_, fallbackSampler_);

        for (const ClaimedWindow& claimed : windows_)
            SDL_ReleaseWindowFromGPUDevice(device_, claimed.window);

        SDL_DestroyGPUDevice(device_);
    }

    [[nodiscard]] BackendId backend() const noexcept override { return BackendId::SdlGpu; }

    [[nodiscard]] Capabilities caps() const noexcept override
    {
        Capabilities caps;
        caps.shaderFormat = shaderFormat_;
        caps.maxTextureSize = 16384;
        caps.rendersPixels = true;
        caps.compute = true;
        return caps;
    }

    [[nodiscard]] bool lost() const noexcept override { return lost_; }
    void simulateLoss() noexcept override { markLost("simulated"); }

    // Called where an SDL call failed: the device is lost when the error says
    // so. SDL has no event or query for it, only the driver's code in the
    // message it sets -- DXGI's 0x887A0005/6/7/20, Vulkan's
    // VK_ERROR_DEVICE_LOST -- which is also what makes this independent of the
    // language the rest of the message is in.
    void noteFailure() noexcept
    {
        std::string error = SDL_GetError();
        std::transform(error.begin(), error.end(), error.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        for (const char* code : {"0X887A0005", "0X887A0006", "0X887A0007", "0X887A0020", "VK_ERROR_DEVICE_LOST"}) {
            if (error.find(code) != std::string::npos) {
                markLost(SDL_GetError());
                return;
            }
        }
    }

    void markLost(std::string_view reason) noexcept
    {
        if (lost_)
            return;
        lost_ = true;
        cmdList_.abandon();
        const std::array<core::I18nArg, 1> args{core::I18nArg{"reason", reason}};
        core::log(core::LogLevel::Error, ENG_TR("rhi.err.device_lost"), args);
    }

    [[nodiscard]] bool claimWindow(platform::Window& window) override
    {
        if (lost_)
            return false;
        SDL_Window* native = platform::nativeWindow(window);
        if (!SDL_ClaimWindowForGPUDevice(device_, native))
            return false;

        // One texture slot per window, rewritten each acquire. Handing out a
        // fresh handle per frame would grow the table forever.
        const u32 id = addSlot(textures_, TextureEntry{.owned = false});
        windows_.push_back({native, id});
        return true;
    }

    bool setVSync(platform::Window& window, bool on) override
    {
        if (lost_)
            return false;
        SDL_Window* native = platform::nativeWindow(window);
        // **Without the sync, at once and torn** (D528): what "VSync off" is
        // in every engine. The newest frame on the next refresh only where the
        // backend cannot tear -- a mailbox through a windowed swapchain is
        // paced at the display's rate on some drivers, which held a machine
        // with VSync off to its monitor's refresh.
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (!on) {
            if (SDL_WindowSupportsGPUPresentMode(device_, native, SDL_GPU_PRESENTMODE_IMMEDIATE))
                mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (SDL_WindowSupportsGPUPresentMode(device_, native, SDL_GPU_PRESENTMODE_MAILBOX))
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
            else
                return false;
        }
        if (!SDL_SetGPUSwapchainParameters(device_, native, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            return false;
        for (ClaimedWindow& entry : windows_) {
            if (entry.window == native)
                entry.presentMode = mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? PresentMode::Immediate
                                    : mode == SDL_GPU_PRESENTMODE_MAILBOX ? PresentMode::Mailbox
                                                                          : PresentMode::Vsync;
        }
        return true;
    }

    [[nodiscard]] PresentMode presentMode(platform::Window& window) const override
    {
        SDL_Window* native = platform::nativeWindow(window);
        for (const ClaimedWindow& entry : windows_) {
            if (entry.window == native)
                return entry.presentMode;
        }
        return PresentMode::None;
    }

    [[nodiscard]] std::string_view driverName() const noexcept override
    {
        const char* name = device_ != nullptr ? SDL_GetGPUDeviceDriver(device_) : nullptr;
        return name != nullptr ? std::string_view{name} : std::string_view{};
    }

    [[nodiscard]] std::string_view adapterName() const noexcept override
    {
        if (device_ == nullptr)
            return {};
        // The property set is the device's own, and the string lives as long
        // as it does.
        const char* name =
            SDL_GetStringProperty(SDL_GetGPUDeviceProperties(device_), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
        return name != nullptr ? std::string_view{name} : std::string_view{};
    }

    void releaseWindow(platform::Window& window) override
    {
        if (lost_)
            return;
        SDL_Window* native = platform::nativeWindow(window);
        for (usize i = 0; i < windows_.size(); ++i) {
            if (windows_[i].window != native)
                continue;

            if (TextureEntry* entry = slot(textures_, windows_[i].textureId); entry != nullptr)
                *entry = TextureEntry{.owned = false};

            SDL_ReleaseWindowFromGPUDevice(device_, native);
            windows_.erase(windows_.begin() + static_cast<isize>(i));
            return;
        }
    }

    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc) override
    {
        if (lost_)
            return {};
        const SDL_GPUBufferCreateInfo info{
            .usage = toSdl(desc.usage),
            .size = desc.sizeBytes,
            .props = 0,
        };
        SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
        if (buffer == nullptr) {
            noteFailure();
            return {};
        }

        if (!desc.debugName.empty())
            SDL_SetGPUBufferName(device_, buffer, std::string(desc.debugName).c_str());

        bufferSizes_.push_back(desc.sizeBytes);
        return {addSlot(buffers_, buffer)};
    }

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc) override
    {
        if (lost_)
            return {};
        const SDL_GPUTextureCreateInfo info{
            .type = desc.layers > 1 ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D,
            .format = toSdl(desc.format),
            .usage = toSdl(desc.usage),
            .width = desc.width,
            .height = desc.height,
            .layer_count_or_depth = desc.layers,
            .num_levels = desc.mipLevels,
            .sample_count = SDL_GPU_SAMPLECOUNT_1,
            .props = 0,
        };
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
        if (texture == nullptr) {
            noteFailure();
            return {};
        }

        if (!desc.debugName.empty())
            SDL_SetGPUTextureName(device_, texture, std::string(desc.debugName).c_str());

        return {addSlot(textures_, TextureEntry{
                                       .texture = texture,
                                       .format = desc.format,
                                       .width = desc.width,
                                       .height = desc.height,
                                       .layers = desc.layers,
                                       .owned = true,
                                   })};
    }

    [[nodiscard]] SamplerHandle createSampler(const SamplerDesc& desc) override
    {
        if (lost_)
            return {};
        const SDL_GPUSamplerCreateInfo info{
            .min_filter = toSdl(desc.minFilter),
            .mag_filter = toSdl(desc.magFilter),
            .mipmap_mode = toSdl(desc.mipmapMode),
            .address_mode_u = toSdl(desc.addressU),
            .address_mode_v = toSdl(desc.addressV),
            .address_mode_w = toSdl(desc.addressW),
            .mip_lod_bias = 0.0f,
            .max_anisotropy = 0.0f,
            .compare_op = SDL_GPU_COMPAREOP_NEVER,
            .min_lod = 0.0f,
            .max_lod = 1000.0f,
            .enable_anisotropy = false,
            .enable_compare = false,
            .padding1 = 0,
            .padding2 = 0,
            .props = 0,
        };
        SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &info);
        if (sampler == nullptr)
            noteFailure();
        return sampler != nullptr ? SamplerHandle{addSlot(samplers_, sampler)} : SamplerHandle{};
    }

    [[nodiscard]] ShaderHandle createShader(const ShaderDesc& desc) override
    {
        if (lost_)
            return {};
        const std::string entryPoint(desc.entryPoint);
        const SDL_GPUShaderCreateInfo info{
            .code_size = desc.code.size(),
            .code = reinterpret_cast<const Uint8*>(desc.code.data()),
            .entrypoint = entryPoint.c_str(),
            .format = toSdl(desc.format),
            .stage = toSdl(desc.stage),
            .num_samplers = desc.samplerCount,
            .num_storage_textures = 0,
            .num_storage_buffers = desc.storageBufferCount,
            .num_uniform_buffers = desc.uniformBufferCount,
            .props = 0,
        };
        SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
        if (shader == nullptr)
            noteFailure();
        return shader != nullptr ? ShaderHandle{addSlot(shaders_, shader)} : ShaderHandle{};
    }

    [[nodiscard]] PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;

    [[nodiscard]] ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override
    {
        if (lost_)
            return {};
        const std::string entryPoint(desc.entryPoint);
        const SDL_GPUComputePipelineCreateInfo info{
            .code_size = desc.code.size(),
            .code = reinterpret_cast<const Uint8*>(desc.code.data()),
            .entrypoint = entryPoint.c_str(),
            .format = toSdl(desc.format),
            .num_samplers = desc.samplerCount,
            .num_readonly_storage_textures = 0,
            .num_readonly_storage_buffers = desc.readonlyStorageBufferCount,
            .num_readwrite_storage_textures = desc.readwriteStorageTextureCount,
            .num_readwrite_storage_buffers = desc.readwriteStorageBufferCount,
            .num_uniform_buffers = desc.uniformBufferCount,
            .threadcount_x = desc.threadCountX,
            .threadcount_y = desc.threadCountY,
            .threadcount_z = desc.threadCountZ,
            .props = 0,
        };
        SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
        if (pipeline == nullptr) {
            noteFailure();
            return {};
        }
        return {addSlot(computePipelines_, pipeline)};
    }

    void destroy(BufferHandle handle) override
    {
        if (lost_)
            return;
        if (SDL_GPUBuffer** entry = slot(buffers_, handle.id); entry != nullptr && *entry != nullptr) {
            SDL_ReleaseGPUBuffer(device_, *entry);
            *entry = nullptr;
        }
    }

    void destroy(TextureHandle handle) override
    {
        if (lost_)
            return;
        if (TextureEntry* entry = slot(textures_, handle.id); entry != nullptr && entry->texture != nullptr) {
            if (entry->owned)
                SDL_ReleaseGPUTexture(device_, entry->texture);
            *entry = TextureEntry{};
        }
    }

    void destroy(SamplerHandle handle) override
    {
        if (lost_)
            return;
        if (SDL_GPUSampler** entry = slot(samplers_, handle.id); entry != nullptr && *entry != nullptr) {
            SDL_ReleaseGPUSampler(device_, *entry);
            *entry = nullptr;
        }
    }

    void destroy(ShaderHandle handle) override
    {
        if (lost_)
            return;
        if (SDL_GPUShader** entry = slot(shaders_, handle.id); entry != nullptr && *entry != nullptr) {
            SDL_ReleaseGPUShader(device_, *entry);
            *entry = nullptr;
        }
    }

    void destroy(PipelineHandle handle) override
    {
        if (lost_)
            return;
        if (SDL_GPUGraphicsPipeline** entry = slot(pipelines_, handle.id); entry != nullptr && *entry != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, *entry);
            *entry = nullptr;
        }
    }

    void destroy(ComputePipelineHandle handle) override
    {
        if (lost_)
            return;
        if (SDL_GPUComputePipeline** entry = slot(computePipelines_, handle.id);
            entry != nullptr && *entry != nullptr) {
            SDL_ReleaseGPUComputePipeline(device_, *entry);
            *entry = nullptr;
        }
    }

    [[nodiscard]] ICmdList* beginFrame() override
    {
        // A lost device's frame is an empty one: every command a no-op.
        if (lost_) {
            cmdList_.begin(nullptr);
            return &cmdList_;
        }
        SDL_GPUCommandBuffer* buffer = SDL_AcquireGPUCommandBuffer(device_);
        if (buffer == nullptr)
            noteFailure();
        cmdList_.begin(lost_ ? nullptr : buffer);
        return &cmdList_;
    }

    [[nodiscard]] Swapchain acquireSwapchain(platform::Window& window) override;

    void submitAndPresent() override
    {
        if (cmdList_.buffer() == nullptr)
            return;

        cmdList_.endOpenPass();
        // **No more than `MaxFramesInFlight` frames ahead of the GPU** (audit
        // R8). A window's present waits for the display, which is what kept a
        // windowed run in step; a headless one had nothing to wait on, so the
        // CPU queued frame after frame the GPU had not drawn, and memory grew a
        // gigabyte a second (11-ocean to 4 GiB in five seconds). The oldest
        // frame's fence is waited on once the queue is full.
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdList_.buffer());
        if (fence == nullptr)
            noteFailure();
        else
            inFlight_.push_back(fence);
        while (inFlight_.size() > MaxFramesInFlight) {
            SDL_GPUFence* oldest = inFlight_.front();
            inFlight_.erase(inFlight_.begin());
            if (!lost_)
                (void)SDL_WaitForGPUFences(device_, true, &oldest, 1);
            SDL_ReleaseGPUFence(device_, oldest);
        }
        cmdList_.begin(nullptr);
    }

    void waitIdle() override
    {
        if (!lost_) {
            SDL_WaitForGPUIdle(device_);
            releaseInFlight();
        }
    }

    [[nodiscard]] bool readTexture(TextureHandle texture, std::span<std::byte> out) override;
    [[nodiscard]] bool readBuffer(BufferHandle buffer, u32 offsetBytes, std::span<std::byte> out) override;

    // Used by the command list, which lives inside this file, and by the
    // interop accessors at the bottom of it.
    [[nodiscard]] SDL_GPUDevice* handle() const noexcept { return device_; }
    [[nodiscard]] SDL_GPUCommandBuffer* commandBuffer() const noexcept { return cmdList_.buffer(); }
    [[nodiscard]] SDL_GPURenderPass* renderPass() const noexcept { return cmdList_.renderPass(); }
    [[nodiscard]] SDL_GPUBuffer* buffer(BufferHandle handle) noexcept
    {
        SDL_GPUBuffer** entry = slot(buffers_, handle.id);
        return entry != nullptr ? *entry : nullptr;
    }
    [[nodiscard]] u32 bufferSize(BufferHandle handle) noexcept
    {
        u32* entry = slot(bufferSizes_, handle.id);
        return entry != nullptr ? *entry : 0;
    }
    [[nodiscard]] TextureEntry* texture(TextureHandle handle) noexcept { return slot(textures_, handle.id); }
    [[nodiscard]] const TextureEntry* texture(TextureHandle handle) const noexcept
    {
        return (handle.id != 0 && handle.id <= textures_.size()) ? &textures_[handle.id - 1] : nullptr;
    }
    [[nodiscard]] SDL_GPUSampler* sampler(SamplerHandle handle) noexcept
    {
        SDL_GPUSampler** entry = slot(samplers_, handle.id);
        return entry != nullptr ? *entry : nullptr;
    }

    // **What a binding of a handle that names nothing gets instead** -- a
    // texture destroyed while something still held its handle, a sampler never
    // made. SDL dereferences what it is given, so a null there is the process
    // ending inside the backend (the 26-security-cameras crash). A 1x1 texture
    // and a plain sampler draw wrong instead, and the first one is said.
    [[nodiscard]] SDL_GPUTexture* fallbackTexture() noexcept
    {
        if (fallbackTexture_ == nullptr && device_ != nullptr) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1;
            info.height = 1;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            fallbackTexture_ = SDL_CreateGPUTexture(device_, &info);
        }
        return fallbackTexture_;
    }
    [[nodiscard]] SDL_GPUSampler* fallbackSampler() noexcept
    {
        if (fallbackSampler_ == nullptr && device_ != nullptr) {
            const SDL_GPUSamplerCreateInfo info{};
            fallbackSampler_ = SDL_CreateGPUSampler(device_, &info);
        }
        return fallbackSampler_;
    }
    void noteStaleBinding() noexcept
    {
        if (staleBindingSaid_)
            return;
        staleBindingSaid_ = true;
        core::log(core::LogLevel::Error, ENG_TR("rhi.err.stale_binding"));
    }
    [[nodiscard]] SDL_GPUShader* shader(ShaderHandle handle) noexcept
    {
        SDL_GPUShader** entry = slot(shaders_, handle.id);
        return entry != nullptr ? *entry : nullptr;
    }
    [[nodiscard]] SDL_GPUGraphicsPipeline* pipeline(PipelineHandle handle) noexcept
    {
        SDL_GPUGraphicsPipeline** entry = slot(pipelines_, handle.id);
        return entry != nullptr ? *entry : nullptr;
    }
    [[nodiscard]] SDL_GPUComputePipeline* computePipeline(ComputePipelineHandle handle) noexcept
    {
        SDL_GPUComputePipeline** entry = slot(computePipelines_, handle.id);
        return entry != nullptr ? *entry : nullptr;
    }

private:
    using isize = std::ptrdiff_t;

    struct ClaimedWindow
    {
        SDL_Window* window = nullptr;
        u32 textureId = 0;
        // SDL's own default until `setVSync` says otherwise.
        PresentMode presentMode = PresentMode::Vsync;
    };

    SDL_GPUDevice* device_ = nullptr;
    ShaderFormat shaderFormat_ = ShaderFormat::Unknown;
    SdlGpuCmdList cmdList_;
    bool lost_ = false;

    // The frames submitted and not yet known finished, oldest first.
    static constexpr std::size_t MaxFramesInFlight = 3;
    std::vector<SDL_GPUFence*> inFlight_;
    // After the GPU is idle: every fence in flight has signalled.
    void releaseInFlight() noexcept
    {
        for (SDL_GPUFence* fence : inFlight_)
            SDL_ReleaseGPUFence(device_, fence);
        inFlight_.clear();
    }

    std::vector<SDL_GPUBuffer*> buffers_;
    // Each buffer's size, by the same slot.
    std::vector<u32> bufferSizes_;
    std::vector<TextureEntry> textures_;
    std::vector<SDL_GPUSampler*> samplers_;
    SDL_GPUTexture* fallbackTexture_ = nullptr;
    SDL_GPUSampler* fallbackSampler_ = nullptr;
    bool staleBindingSaid_ = false;
    std::vector<SDL_GPUShader*> shaders_;
    std::vector<SDL_GPUGraphicsPipeline*> pipelines_;
    std::vector<SDL_GPUComputePipeline*> computePipelines_;
    std::vector<ClaimedWindow> windows_;
};

PipelineHandle SdlGpuDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    if (lost_)
        return {};
    std::vector<SDL_GPUVertexBufferDescription> vertexBuffers;
    vertexBuffers.reserve(desc.vertexBuffers.size());
    for (const VertexBufferLayout& layout : desc.vertexBuffers) {
        vertexBuffers.push_back({
            .slot = layout.slot,
            .pitch = layout.strideBytes,
            .input_rate = layout.perInstance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX,
            // Zero, always. SDL_gpu.h:1651 calls this field "Reserved for future
            // use. Must be set to 0" and asserts on anything else -- the rate is
            // implied by `input_rate`, and one step per instance is the only one
            // SDL_GPU offers. Setting it to 1 for a per-instance stream looks
            // more correct and aborts the process.
            .instance_step_rate = 0,
        });
    }

    std::vector<SDL_GPUVertexAttribute> attributes;
    attributes.reserve(desc.vertexAttributes.size());
    for (const VertexAttribute& attribute : desc.vertexAttributes) {
        attributes.push_back({
            .location = attribute.location,
            .buffer_slot = attribute.bufferSlot,
            .format = toSdl(attribute.format),
            .offset = attribute.offsetBytes,
        });
    }

    std::vector<SDL_GPUColorTargetDescription> colorTargets;
    colorTargets.reserve(desc.colorTargets.size());
    for (const ColorTargetDesc& target : desc.colorTargets) {
        colorTargets.push_back({
            .format = toSdl(target.format),
            .blend_state =
                {
                    .src_color_blendfactor = toSdl(target.blend.srcColor),
                    .dst_color_blendfactor = toSdl(target.blend.dstColor),
                    .color_blend_op = toSdl(target.blend.colorOp),
                    .src_alpha_blendfactor = toSdl(target.blend.srcAlpha),
                    .dst_alpha_blendfactor = toSdl(target.blend.dstAlpha),
                    .alpha_blend_op = toSdl(target.blend.alphaOp),
                    .color_write_mask = 0xF,
                    .enable_blend = target.blend.enabled,
                    .enable_color_write_mask = false,
                    .padding1 = 0,
                    .padding2 = 0,
                },
        });
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    // Null shaders reach SDL, which rejects the pipeline and says why. Checking
    // here first would turn a described failure into a silent empty handle.
    info.vertex_shader = shader(desc.vertexShader);
    info.fragment_shader = shader(desc.fragmentShader);
    info.vertex_input_state = {
        .vertex_buffer_descriptions = vertexBuffers.data(),
        .num_vertex_buffers = static_cast<Uint32>(vertexBuffers.size()),
        .vertex_attributes = attributes.data(),
        .num_vertex_attributes = static_cast<Uint32>(attributes.size()),
    };
    info.primitive_type = toSdl(desc.primitive);
    info.rasterizer_state.fill_mode = toSdl(desc.rasterizer.fillMode);
    info.rasterizer_state.cull_mode = toSdl(desc.rasterizer.cullMode);
    info.rasterizer_state.front_face = toSdl(desc.rasterizer.frontFace);
    info.rasterizer_state.enable_depth_clip = desc.rasterizer.depthClip;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = toSdl(desc.depthStencil.depthCompare);
    info.depth_stencil_state.enable_depth_test = desc.depthStencil.depthTest;
    info.depth_stencil_state.enable_depth_write = desc.depthStencil.depthWrite;
    info.target_info = {
        .color_target_descriptions = colorTargets.data(),
        .num_color_targets = static_cast<Uint32>(colorTargets.size()),
        .depth_stencil_format = toSdl(desc.depthStencilFormat),
        .has_depth_stencil_target = desc.depthStencilFormat != TextureFormat::Undefined,
        .padding1 = 0,
        .padding2 = 0,
        .padding3 = 0,
    };

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    if (pipeline == nullptr)
        noteFailure();
    return pipeline != nullptr ? PipelineHandle{addSlot(pipelines_, pipeline)} : PipelineHandle{};
}

Swapchain SdlGpuDevice::acquireSwapchain(platform::Window& window)
{
    SDL_GPUCommandBuffer* buffer = cmdList_.buffer();
    if (buffer == nullptr)
        return {};

    SDL_Window* native = platform::nativeWindow(window);
    u32 textureId = 0;
    for (const ClaimedWindow& claimed : windows_) {
        if (claimed.window == native) {
            textureId = claimed.textureId;
            break;
        }
    }
    if (textureId == 0)
        return {};

    SDL_GPUTexture* swapchainTexture = nullptr;
    Uint32 width = 0;
    Uint32 height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(buffer, native, &swapchainTexture, &width, &height)) {
        noteFailure();
        return {};
    }
    if (swapchainTexture == nullptr) {
        // A minimized or occluded window has no backbuffer this frame. Normal,
        // not an error -- the seam says so and every caller has to handle it.
        return {};
    }

    const TextureFormat format = fromSdl(SDL_GetGPUSwapchainTextureFormat(device_, native));
    TextureEntry* entry = slot(textures_, textureId);
    *entry = TextureEntry{
        .texture = swapchainTexture,
        .format = format,
        .width = width,
        .height = height,
        .owned = false,
    };

    return {.texture = TextureHandle{textureId}, .width = width, .height = height, .format = format};
}

bool SdlGpuDevice::readTexture(TextureHandle texture, std::span<std::byte> out)
{
    if (lost_)
        return false;
    const TextureEntry* entry = slot(textures_, texture.id);
    if (entry == nullptr || entry->texture == nullptr)
        return false;

    const u32 pixelSize = bytesPerPixel(entry->format);
    if (pixelSize == 0)
        return false;

    const usize needed = static_cast<usize>(entry->width) * entry->height * pixelSize;
    if (out.size() < needed)
        return false;

    const SDL_GPUTransferBufferCreateInfo transferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = static_cast<Uint32>(needed),
        .props = 0,
    };
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (transfer == nullptr) {
        noteFailure();
        return false;
    }

    SDL_GPUCommandBuffer* buffer = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(buffer);

    const SDL_GPUTextureRegion region{
        .texture = entry->texture,
        .mip_level = 0,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
        .w = entry->width,
        .h = entry->height,
        .d = 1,
    };
    const SDL_GPUTextureTransferInfo destination{
        .transfer_buffer = transfer,
        .offset = 0,
        .pixels_per_row = entry->width,
        .rows_per_layer = entry->height,
    };
    SDL_DownloadFromGPUTexture(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);

    // Blocking by design: this is the screenshot path, and the caller was told.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(buffer);
    if (fence == nullptr) {
        noteFailure();
        if (!lost_)
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    bool ok = false;
    if (const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false); mapped != nullptr) {
        std::memcpy(out.data(), mapped, needed);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        ok = true;
    }

    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

bool SdlGpuDevice::readBuffer(BufferHandle handle, u32 offsetBytes, std::span<std::byte> out)
{
    if (lost_ || out.empty())
        return false;
    SDL_GPUBuffer* source = buffer(handle);
    if (source == nullptr)
        return false;

    const SDL_GPUTransferBufferCreateInfo transferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = static_cast<Uint32>(out.size()),
        .props = 0,
    };
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (transfer == nullptr) {
        noteFailure();
        return false;
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUBufferRegion region{
        .buffer = source,
        .offset = offsetBytes,
        .size = static_cast<Uint32>(out.size()),
    };
    const SDL_GPUTransferBufferLocation destination{.transfer_buffer = transfer, .offset = 0};
    SDL_DownloadFromGPUBuffer(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);

    // Blocking, as `readTexture` is: a test's question, never a frame's.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
        noteFailure();
        if (!lost_)
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    bool ok = false;
    if (const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false); mapped != nullptr) {
        std::memcpy(out.data(), mapped, out.size());
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        ok = true;
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

// --- command list -----------------------------------------------------------

void SdlGpuCmdList::endOpenPass() noexcept
{
    if (renderPass_ != nullptr) {
        SDL_EndGPURenderPass(renderPass_);
        renderPass_ = nullptr;
    }
    if (copyPass_ != nullptr) {
        SDL_EndGPUCopyPass(copyPass_);
        copyPass_ = nullptr;
    }
    if (computePass_ != nullptr) {
        SDL_EndGPUComputePass(computePass_);
        computePass_ = nullptr;
    }
}

SDL_GPUCopyPass* SdlGpuCmdList::ensureCopyPass() noexcept
{
    if (renderPass_ != nullptr) {
        // Uploading mid-pass is not something the backend can paper over: the
        // GPU is rasterizing into the target right now. Saying so beats a
        // driver-level crash three frames later.
        core::log(core::LogLevel::Error, ENG_TR("rhi.err.upload_inside_pass"));
        return nullptr;
    }
    // An upload between compute dispatches -- resetting the counters a cull
    // accumulates into -- ends the compute pass; the copy lands before the
    // next pass reads it.
    if (computePass_ != nullptr) {
        SDL_EndGPUComputePass(computePass_);
        computePass_ = nullptr;
    }
    if (copyPass_ == nullptr && buffer_ != nullptr)
        copyPass_ = SDL_BeginGPUCopyPass(buffer_);
    return copyPass_;
}

namespace {

// An upload larger than this keeps a transfer buffer of its own: a texture
// arriving at load time is megabytes once, and growing the per-frame buffer to
// hold it would keep that memory for the rest of the run.
constexpr u32 kStagingLargestUpload = 4u * 1024u * 1024u;
// **A buffer's upload may be larger** (audit R13): a buffer rewritten whole is
// the instance data, written again every frame, and above the texture limit it
// took and gave back a transfer buffer of its own every frame -- fifty thousand
// parts are ~6 MB of it.
constexpr u32 kStagingLargestBufferUpload = 16u * 1024u * 1024u;
// The frame buffer's first size, and the ceiling it grows to.
constexpr u32 kStagingInitial = 1024u * 1024u;
constexpr u32 kStagingCeiling = 32u * 1024u * 1024u;
// D3D12 places a texture copy's source on a 512-byte boundary and rejects
// anything else; a buffer copy needs far less, and 16 keeps every element type
// this engine uploads naturally aligned.
constexpr u32 kTextureStagingAlignment = 512;
constexpr u32 kBufferStagingAlignment = 16;

[[nodiscard]] constexpr u32 alignUp(u32 value, u32 alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

void SdlGpuCmdList::begin(SDL_GPUCommandBuffer* buffer) noexcept
{
    buffer_ = buffer;
    if (buffer == nullptr)
        return;

    // A new frame: the staging buffer's ranges are free again once it cycles.
    // It grows here, between frames, and never in the middle of one -- an upload
    // that does not fit mid-frame takes a buffer of its own instead, and the
    // next frame starts big enough for all of them.
    if (stagingWanted_ > stagingCapacity_ && stagingCapacity_ < kStagingCeiling) {
        u32 capacity = std::max(stagingCapacity_ * 2, kStagingInitial);
        while (capacity < stagingWanted_ && capacity < kStagingCeiling)
            capacity *= 2;
        capacity = std::min(capacity, kStagingCeiling);
        releaseStaging();
        const SDL_GPUTransferBufferCreateInfo info{
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size = capacity,
            .props = 0,
        };
        staging_ = SDL_CreateGPUTransferBuffer(device_.handle(), &info);
        stagingCapacity_ = staging_ != nullptr ? capacity : 0;
    }
    stagingUsed_ = 0;
    stagingWanted_ = 0;
    stagingCycled_ = false;
}

void SdlGpuCmdList::releaseStaging() noexcept
{
    // SDL frees a transfer buffer "as soon as it is safe to do so", so this
    // never races a copy a frame still in flight is making from it.
    if (staging_ != nullptr)
        SDL_ReleaseGPUTransferBuffer(device_.handle(), staging_);
    staging_ = nullptr;
    stagingCapacity_ = 0;
}

SdlGpuCmdList::Staged SdlGpuCmdList::stage(std::span<const std::byte> data, u32 alignment) noexcept
{
    SDL_GPUDevice* device = device_.handle();
    const u32 size = static_cast<u32>(data.size());
    const u32 offset = alignUp(stagingUsed_, alignment);
    const u32 largest = alignment == kBufferStagingAlignment ? kStagingLargestBufferUpload : kStagingLargestUpload;
    if (size <= largest)
        stagingWanted_ = std::max(stagingWanted_, offset + size);

    if (staging_ != nullptr && size <= largest && offset + size <= stagingCapacity_) {
        // Cycled on the frame's first write only: every later write of the frame
        // goes to the same backing, at a range nothing else has used.
        void* mapped = SDL_MapGPUTransferBuffer(device, staging_, !stagingCycled_);
        if (mapped == nullptr) {
            device_.noteFailure();
            if (device_.lost())
                return {};
        }
        if (mapped != nullptr) {
            stagingCycled_ = true;
            std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());
            SDL_UnmapGPUTransferBuffer(device, staging_);
            stagingUsed_ = offset + size;
            return Staged{staging_, offset, false};
        }
    }

    // A transfer buffer of its own: too large to keep, or the frame's buffer is
    // full or not made yet.
    const SDL_GPUTransferBufferCreateInfo info{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = size,
        .props = 0,
    };
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &info);
    if (transfer == nullptr) {
        device_.noteFailure();
        return {};
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == nullptr) {
        device_.noteFailure();
        if (!device_.lost())
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        return {};
    }
    std::memcpy(mapped, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(device, transfer);
    return Staged{transfer, 0, true};
}

void SdlGpuCmdList::releaseStaged(const Staged& staged) noexcept
{
    // Released right after its copy is recorded, which is safe for the reason
    // `releaseStaging` gives; the shared buffer is kept.
    if (staged.owned && staged.transfer != nullptr)
        SDL_ReleaseGPUTransferBuffer(device_.handle(), staged.transfer);
}

void SdlGpuCmdList::beginRenderPass(const RenderPassDesc& desc)
{
    endOpenPass();
    if (buffer_ == nullptr)
        return;
    boundVertex_.fill(nullptr);
    boundIndex_ = nullptr;

    BindList<SDL_GPUColorTargetInfo> colors(desc.colorAttachments.size());
    usize colorCount = 0;
    for (const ColorAttachment& attachment : desc.colorAttachments) {
        TextureEntry* entry = device_.texture(attachment.texture);
        if (entry == nullptr || entry->texture == nullptr)
            continue;

        SDL_GPUColorTargetInfo info{};
        info.texture = entry->texture;
        info.clear_color = {attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b,
                            attachment.clearColor.a};
        info.load_op = toSdl(attachment.loadOp);
        info.store_op = toSdl(attachment.storeOp);
        info.layer_or_depth_plane = attachment.layer;
        colors[colorCount++] = info;
    }
    colors.shrink(colorCount);

    SDL_GPUDepthStencilTargetInfo depth{};
    const TextureEntry* depthEntry = device_.texture(desc.depthStencil.texture);
    const bool hasDepth = depthEntry != nullptr && depthEntry->texture != nullptr;
    if (hasDepth) {
        depth.texture = depthEntry->texture;
        depth.clear_depth = desc.depthStencil.clearDepth;
        depth.load_op = toSdl(desc.depthStencil.loadOp);
        depth.store_op = toSdl(desc.depthStencil.storeOp);
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    }

    renderPass_ =
        SDL_BeginGPURenderPass(buffer_, colors.data(), static_cast<Uint32>(colors.size()), hasDepth ? &depth : nullptr);
}

void SdlGpuCmdList::endRenderPass()
{
    if (renderPass_ != nullptr) {
        SDL_EndGPURenderPass(renderPass_);
        renderPass_ = nullptr;
    }
}

void SdlGpuCmdList::setPipeline(PipelineHandle pipeline)
{
    if (renderPass_ == nullptr)
        return;
    if (SDL_GPUGraphicsPipeline* native = device_.pipeline(pipeline); native != nullptr)
        SDL_BindGPUGraphicsPipeline(renderPass_, native);
}

void SdlGpuCmdList::setViewport(const Viewport& viewport)
{
    if (renderPass_ == nullptr)
        return;

    const SDL_GPUViewport native{viewport.x,      viewport.y,        viewport.width,
                                 viewport.height, viewport.minDepth, viewport.maxDepth};
    SDL_SetGPUViewport(renderPass_, &native);
}

void SdlGpuCmdList::setScissor(const Rect& scissor)
{
    if (renderPass_ == nullptr)
        return;

    const SDL_Rect native{scissor.x, scissor.y, scissor.width, scissor.height};
    SDL_SetGPUScissor(renderPass_, &native);
}

void SdlGpuCmdList::bindVertexBuffers(u32 firstSlot, std::span<const BufferHandle> buffers)
{
    if (renderPass_ == nullptr || buffers.empty())
        return;

    // The same buffers in the same slots as the last bind of this pass are
    // already bound (audit R6): a scene of one mesh drawn a thousand ways
    // rebound it a thousand times.
    bool same = firstSlot + buffers.size() <= boundVertex_.size();
    BindList<SDL_GPUBufferBinding> bindings(buffers.size());
    for (usize index = 0; index < buffers.size(); ++index) {
        SDL_GPUBuffer* native = device_.buffer(buffers[index]);
        bindings[index] = {.buffer = native, .offset = 0};
        same = same && boundVertex_[firstSlot + index] == native;
    }
    if (same)
        return;
    for (usize index = 0; index < buffers.size() && firstSlot + index < boundVertex_.size(); ++index)
        boundVertex_[firstSlot + index] = bindings[index].buffer;

    SDL_BindGPUVertexBuffers(renderPass_, firstSlot, bindings.data(), static_cast<Uint32>(bindings.size()));
}

void SdlGpuCmdList::bindIndexBuffer(BufferHandle buffer, IndexType type)
{
    if (renderPass_ == nullptr)
        return;

    const SDL_GPUBufferBinding binding{.buffer = device_.buffer(buffer), .offset = 0};
    if (binding.buffer == boundIndex_ && type == boundIndexType_)
        return;
    boundIndex_ = binding.buffer;
    boundIndexType_ = type;
    SDL_BindGPUIndexBuffer(renderPass_, &binding, toSdl(type));
}

void SdlGpuCmdList::bindUniforms(ShaderStage stage, u32 slotIndex, std::span<const std::byte> data)
{
    if (buffer_ == nullptr)
        return;

    switch (stage) {
    case ShaderStage::Vertex:
        SDL_PushGPUVertexUniformData(buffer_, slotIndex, data.data(), static_cast<Uint32>(data.size()));
        break;
    case ShaderStage::Fragment:
        SDL_PushGPUFragmentUniformData(buffer_, slotIndex, data.data(), static_cast<Uint32>(data.size()));
        break;
    }
}

void SdlGpuCmdList::bindTextures(ShaderStage stage, u32 firstSlot, std::span<const TextureBinding> bindings)
{
    if (renderPass_ == nullptr || bindings.empty())
        return;

    BindList<SDL_GPUTextureSamplerBinding> native(bindings.size());
    usize filled = 0;
    for (const TextureBinding& binding : bindings) {
        const TextureEntry* entry = device_.texture(binding.texture);
        SDL_GPUTexture* texture = entry != nullptr ? entry->texture : nullptr;
        SDL_GPUSampler* sampler = device_.sampler(binding.sampler);
        if (texture == nullptr || sampler == nullptr) {
            device_.noteStaleBinding();
            if (texture == nullptr)
                texture = device_.fallbackTexture();
            if (sampler == nullptr)
                sampler = device_.fallbackSampler();
            if (texture == nullptr || sampler == nullptr)
                return;
        }
        native[filled++] = {.texture = texture, .sampler = sampler};
    }

    const auto count = static_cast<Uint32>(native.size());
    switch (stage) {
    case ShaderStage::Vertex:
        SDL_BindGPUVertexSamplers(renderPass_, firstSlot, native.data(), count);
        break;
    case ShaderStage::Fragment:
        SDL_BindGPUFragmentSamplers(renderPass_, firstSlot, native.data(), count);
        break;
    }
}

void SdlGpuCmdList::draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance)
{
    if (renderPass_ != nullptr)
        SDL_DrawGPUPrimitives(renderPass_, vertexCount, instanceCount, firstVertex, firstInstance);
}

void SdlGpuCmdList::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance)
{
    if (renderPass_ != nullptr)
        SDL_DrawGPUIndexedPrimitives(renderPass_, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void SdlGpuCmdList::upload(BufferHandle buffer, std::span<const std::byte> data, u32 offsetBytes)
{
    SDL_GPUBuffer* target = device_.buffer(buffer);
    if (target == nullptr || data.empty())
        return;

    SDL_GPUCopyPass* pass = ensureCopyPass();
    if (pass == nullptr)
        return;

    const Staged staged = stage(data, kBufferStagingAlignment);
    if (staged.transfer == nullptr)
        return;
    const SDL_GPUTransferBufferLocation source{.transfer_buffer = staged.transfer, .offset = staged.offset};
    const SDL_GPUBufferRegion destination{
        .buffer = target, .offset = offsetBytes, .size = static_cast<Uint32>(data.size())};
    // **A write of the whole buffer cycles it** (audit R11): nothing of the
    // old contents survives, so a frame still in flight can keep reading them
    // from the old backing instead of the copy waiting for it to finish. A
    // write of a part must not -- the rest is live.
    const bool whole = offsetBytes == 0 && data.size() == device_.bufferSize(buffer);
    SDL_UploadToGPUBuffer(pass, &source, &destination, whole);
    releaseStaged(staged);
}

void SdlGpuCmdList::uploadTexture(TextureHandle texture, std::span<const std::byte> data, u32 mipLevel)
{
    TextureEntry* entry = device_.texture(texture);
    if (entry == nullptr || entry->texture == nullptr || data.empty())
        return;

    SDL_GPUCopyPass* pass = ensureCopyPass();
    if (pass == nullptr)
        return;

    const Staged staged = stage(data, kTextureStagingAlignment);
    if (staged.transfer == nullptr)
        return;

    // Clamped at one: a 64x64 texture's seventh mip is 1x1, and `>> 6`
    // reaches zero one level later. A zero-sized region uploads nothing and
    // reports nothing, which is the shape of a texture that is fine until
    // somebody looks at its smallest level.
    const u32 mipWidth = std::max(1u, entry->width >> mipLevel);
    const u32 mipHeight = std::max(1u, entry->height >> mipLevel);
    const SDL_GPUTextureTransferInfo source{
        .transfer_buffer = staged.transfer,
        .offset = staged.offset,
        .pixels_per_row = mipWidth,
        .rows_per_layer = mipHeight,
    };
    const SDL_GPUTextureRegion region{
        .texture = entry->texture,
        .mip_level = mipLevel,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
        .w = mipWidth,
        .h = mipHeight,
        .d = 1,
    };
    SDL_UploadToGPUTexture(pass, &source, &region, false);
    releaseStaged(staged);
}

void SdlGpuCmdList::blitTexture(TextureHandle source, TextureHandle destination, u32 destinationLayer)
{
    TextureEntry* from = device_.texture(source);
    TextureEntry* to = device_.texture(destination);
    if (from == nullptr || to == nullptr || from->texture == nullptr || to->texture == nullptr ||
        destinationLayer >= to->layers || buffer_ == nullptr)
        return;
    // A blit is a pass of its own in SDL: nothing may be open around it.
    endOpenPass();
    SDL_GPUBlitInfo info{};
    info.source = SDL_GPUBlitRegion{.texture = from->texture,
                                    .mip_level = 0,
                                    .layer_or_depth_plane = 0,
                                    .x = 0,
                                    .y = 0,
                                    .w = from->width,
                                    .h = from->height};
    info.destination = SDL_GPUBlitRegion{.texture = to->texture,
                                         .mip_level = 0,
                                         .layer_or_depth_plane = destinationLayer,
                                         .x = 0,
                                         .y = 0,
                                         .w = to->width,
                                         .h = to->height};
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(buffer_, &info);
}

void SdlGpuCmdList::generateMipmaps(TextureHandle texture)
{
    TextureEntry* entry = device_.texture(texture);
    if (entry == nullptr || entry->texture == nullptr || buffer_ == nullptr)
        return;
    endOpenPass();
    SDL_GenerateMipmapsForGPUTexture(buffer_, entry->texture);
}

void SdlGpuCmdList::uploadTextureRegion(TextureHandle texture, u32 x, u32 y, u32 width, u32 height,
                                        std::span<const std::byte> data)
{
    TextureEntry* entry = device_.texture(texture);
    if (entry == nullptr || entry->texture == nullptr || data.empty() || width == 0 || height == 0)
        return;
    // Refused rather than clipped: a rectangle past the edge is a caller's
    // arithmetic, and writing the part that fits would hide it.
    if (x >= entry->width || y >= entry->height || width > entry->width - x || height > entry->height - y)
        return;

    SDL_GPUCopyPass* pass = ensureCopyPass();
    if (pass == nullptr)
        return;

    const Staged staged = stage(data, kTextureStagingAlignment);
    if (staged.transfer == nullptr)
        return;
    const SDL_GPUTextureTransferInfo source{
        .transfer_buffer = staged.transfer,
        .offset = staged.offset,
        .pixels_per_row = width,
        .rows_per_layer = height,
    };
    const SDL_GPUTextureRegion region{
        .texture = entry->texture,
        .mip_level = 0,
        .layer = 0,
        .x = x,
        .y = y,
        .z = 0,
        .w = width,
        .h = height,
        .d = 1,
    };
    // Not cycling: the rest of the texture is live and has to survive.
    SDL_UploadToGPUTexture(pass, &source, &region, false);
    releaseStaged(staged);
}

void SdlGpuCmdList::bindStorageBuffers(ShaderStage stage, u32 firstSlot, std::span<const BufferHandle> buffers)
{
    if (renderPass_ == nullptr || buffers.empty())
        return;
    BindList<SDL_GPUBuffer*> native(buffers.size());
    for (usize index = 0; index < buffers.size(); ++index)
        native[index] = device_.buffer(buffers[index]);
    const auto count = static_cast<Uint32>(native.size());
    switch (stage) {
    case ShaderStage::Vertex:
        SDL_BindGPUVertexStorageBuffers(renderPass_, firstSlot, native.data(), count);
        break;
    case ShaderStage::Fragment:
        SDL_BindGPUFragmentStorageBuffers(renderPass_, firstSlot, native.data(), count);
        break;
    }
}

void SdlGpuCmdList::drawIndexedIndirect(BufferHandle buffer, u32 offsetBytes, u32 drawCount)
{
    if (renderPass_ == nullptr || drawCount == 0)
        return;
    if (SDL_GPUBuffer* native = device_.buffer(buffer); native != nullptr)
        SDL_DrawGPUIndexedPrimitivesIndirect(renderPass_, native, offsetBytes, drawCount);
}

void SdlGpuCmdList::beginComputePass(std::span<const BufferHandle> writes,
                                     std::span<const ComputeTextureWrite> textureWrites)
{
    endOpenPass();
    if (buffer_ == nullptr)
        return;
    BindList<SDL_GPUStorageTextureReadWriteBinding> textures(textureWrites.size());
    for (usize index = 0; index < textureWrites.size(); ++index) {
        SDL_GPUStorageTextureReadWriteBinding binding{};
        TextureEntry* entry = device_.texture(textureWrites[index].texture);
        // A handle that names nothing is the pass not begun: SDL dereferences
        // what it is given, and a null here is the process ending in it.
        if (entry == nullptr || entry->texture == nullptr) {
            device_.noteStaleBinding();
            return;
        }
        binding.texture = entry->texture;
        binding.mip_level = textureWrites[index].mipLevel;
        binding.layer = 0;
        // Never cycled: a pass that writes part of an image keeps the rest.
        binding.cycle = false;
        textures[index] = binding;
    }
    BindList<SDL_GPUStorageBufferReadWriteBinding> bindings(writes.size());
    for (usize index = 0; index < writes.size(); ++index) {
        SDL_GPUStorageBufferReadWriteBinding binding{};
        binding.buffer = device_.buffer(writes[index]);
        // Never cycled: what a pass writes is what the draws after it read.
        binding.cycle = false;
        bindings[index] = binding;
    }
    computePass_ = SDL_BeginGPUComputePass(buffer_, textures.data(), static_cast<Uint32>(textures.size()),
                                           bindings.data(), static_cast<Uint32>(bindings.size()));
}

void SdlGpuCmdList::endComputePass()
{
    if (computePass_ != nullptr) {
        SDL_EndGPUComputePass(computePass_);
        computePass_ = nullptr;
    }
}

void SdlGpuCmdList::setComputePipeline(ComputePipelineHandle pipeline)
{
    if (computePass_ == nullptr)
        return;
    if (SDL_GPUComputePipeline* native = device_.computePipeline(pipeline); native != nullptr)
        SDL_BindGPUComputePipeline(computePass_, native);
}

void SdlGpuCmdList::bindComputeStorageBuffers(u32 firstSlot, std::span<const BufferHandle> buffers)
{
    if (computePass_ == nullptr || buffers.empty())
        return;
    BindList<SDL_GPUBuffer*> native(buffers.size());
    for (usize index = 0; index < buffers.size(); ++index)
        native[index] = device_.buffer(buffers[index]);
    SDL_BindGPUComputeStorageBuffers(computePass_, firstSlot, native.data(), static_cast<Uint32>(native.size()));
}

void SdlGpuCmdList::bindComputeTextures(u32 firstSlot, std::span<const TextureBinding> bindings)
{
    if (computePass_ == nullptr || bindings.empty())
        return;
    BindList<SDL_GPUTextureSamplerBinding> native(bindings.size());
    usize filled = 0;
    for (const TextureBinding& binding : bindings) {
        const TextureEntry* entry = device_.texture(binding.texture);
        SDL_GPUTexture* texture = entry != nullptr ? entry->texture : nullptr;
        SDL_GPUSampler* sampler = device_.sampler(binding.sampler);
        // As a fragment stage's: a stale handle is noted and the fallback
        // bound, so a pass never runs with a slot empty.
        if (texture == nullptr || sampler == nullptr) {
            device_.noteStaleBinding();
            if (texture == nullptr)
                texture = device_.fallbackTexture();
            if (sampler == nullptr)
                sampler = device_.fallbackSampler();
            if (texture == nullptr || sampler == nullptr)
                return;
        }
        native[filled++] = {.texture = texture, .sampler = sampler};
    }
    SDL_BindGPUComputeSamplers(computePass_, firstSlot, native.data(), static_cast<Uint32>(native.size()));
}

void SdlGpuCmdList::bindComputeUniforms(u32 slot, std::span<const std::byte> data)
{
    if (buffer_ != nullptr)
        SDL_PushGPUComputeUniformData(buffer_, slot, data.data(), static_cast<Uint32>(data.size()));
}

void SdlGpuCmdList::dispatch(u32 groupsX, u32 groupsY, u32 groupsZ)
{
    if (computePass_ != nullptr && groupsX != 0 && groupsY != 0 && groupsZ != 0)
        SDL_DispatchGPUCompute(computePass_, groupsX, groupsY, groupsZ);
}

void SdlGpuCmdList::pushDebugGroup(std::string_view name)
{
    if (buffer_ != nullptr)
        SDL_PushGPUDebugGroup(buffer_, std::string(name).c_str());
}

void SdlGpuCmdList::popDebugGroup()
{
    if (buffer_ != nullptr)
        SDL_PopGPUDebugGroup(buffer_);
}

// The one downcast in this file, and the only one that cannot be wrong:
// `backend()` is the question every IDevice answers about itself, so a device
// from another backend leaves here as null instead of as a bad cast.
[[nodiscard]] const SdlGpuDevice* asSdlGpu(const IDevice& device) noexcept
{
    return device.backend() == BackendId::SdlGpu ? static_cast<const SdlGpuDevice*>(&device) : nullptr;
}

} // namespace

// --- interop (engine/rhi/sdlgpu_interop.h) -----------------------------------
//
// Defined here rather than in a file of their own because the type they reach
// into is this one, and it is deliberately unnameable outside this translation
// unit.

SDL_GPUDevice* nativeDevice(const IDevice& device) noexcept
{
    const SdlGpuDevice* self = asSdlGpu(device);
    return self != nullptr ? self->handle() : nullptr;
}

SDL_GPUCommandBuffer* nativeCommandBuffer(const IDevice& device) noexcept
{
    const SdlGpuDevice* self = asSdlGpu(device);
    return self != nullptr ? self->commandBuffer() : nullptr;
}

SDL_GPURenderPass* nativeRenderPass(const IDevice& device) noexcept
{
    const SdlGpuDevice* self = asSdlGpu(device);
    return self != nullptr ? self->renderPass() : nullptr;
}

SDL_GPUTexture* nativeTexture(const IDevice& device, TextureHandle handle) noexcept
{
    const SdlGpuDevice* self = asSdlGpu(device);
    if (self == nullptr)
        return nullptr;

    const TextureEntry* entry = self->texture(handle);
    return entry != nullptr ? entry->texture : nullptr;
}

DeviceResult createSdlGpuDevice(const DeviceDesc& desc, core::EngineError* outError)
{
    // Ask for every format the engine can supply and report back which one the
    // device chose, rather than demanding one and failing on a machine that
    // would have worked. `caps().shaderFormat` is what the shader pack loads
    // against.
    const SDL_GPUShaderFormat requested =
        desc.shaderFormat == ShaderFormat::Unknown
            ? (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL)
            : toSdl(desc.shaderFormat);

    SDL_GPUDevice* device = SDL_CreateGPUDevice(requested, desc.debug, nullptr);
    if (device == nullptr) {
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("rhi.err.device_create_failed"), {}, SDL_GetError());
        return nullptr;
    }

    return std::make_unique<SdlGpuDevice>(device, fromSdlShaderFormats(SDL_GetGPUShaderFormats(device)));
}

} // namespace engine::rhi
