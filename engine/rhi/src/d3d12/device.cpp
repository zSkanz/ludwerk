#include <chrono>
#include <d3d12sdklayers.h>
#include <string>
#include <vector>

#include "commands.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/platform/windows_interop.h"
#include "engine/rhi/backends.h"

namespace engine::rhi::d3d12 {
namespace {
class Device final : public IDevice
{
public:
    Device()
        : resources_(context_), pipelines_(context_), descriptors_(context_, resources_),
          commands_(context_, resources_, pipelines_, descriptors_)
    {}
    ~Device() override
    {
        context_.abandon();
        (void)context_.waitIdle();
    }
    HRESULT initialize(const DeviceDesc& desc, std::span<const std::byte> vertex, std::span<const std::byte> fragment)
    {
        if (desc.shaderFormat != ShaderFormat::Unknown && desc.shaderFormat != ShaderFormat::Dxil)
            return E_INVALIDARG;
        HRESULT result = context_.initialize(desc.debug);
        if (SUCCEEDED(result) && desc.debug)
            (void)context_.device()->QueryInterface(IID_PPV_ARGS(&validation_));
        if (FAILED(result) || FAILED(result = descriptors_.initialize()) || FAILED(result = commands_.initialize()) ||
            FAILED(result = commands_.initializeBlit(vertex, fragment)))
            return result;
        caps_ = {.shaderFormat = ShaderFormat::Dxil,
                 .maxTextureSize = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                 .rendersPixels = true,
                 .compute = true,
                 .astcTextures = false,
                 .depthClamp = !desc.leastFeatures};
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 info{};
        if (SUCCEEDED(context_.createFactory(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(context_.device()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&info))) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, info.Description, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                adapter_.resize(static_cast<std::size_t>(bytes));
                WideCharToMultiByte(CP_UTF8, 0, info.Description, -1, adapter_.data(), bytes, nullptr, nullptr);
                adapter_.pop_back();
            }
        }
        return S_OK;
    }
    BackendId backend() const noexcept override { return BackendId::D3D12; }
    Capabilities caps() const noexcept override { return caps_; }
    std::string_view driverName() const noexcept override { return "direct3d12"; }
    std::string_view adapterName() const noexcept override { return adapter_; }
    bool lost() const noexcept override { return FAILED(failure_) || FAILED(context_.status()); }
    void simulateLoss() noexcept override { fail(DXGI_ERROR_DEVICE_REMOVED); }
    bool claimWindow(platform::Window& window) override
    {
        if (lost() || context_.recording() || window_)
            return false;
        const auto surface = platform::windowsSurface(window);
        const auto size = platform::windowPixelSize(window);
        if (size.width <= 0 || size.height <= 0)
            return false;
        HRESULT result = E_INVALIDARG;
        if (surface.kind == platform::WindowsSurfaceKind::CoreWindow)
            result = context_.attachCoreWindow(static_cast<IUnknown*>(surface.object), static_cast<u32>(size.width),
                                               static_cast<u32>(size.height));
        else if (surface.kind == platform::WindowsSurfaceKind::Hwnd)
            result = context_.attachHwnd(static_cast<HWND>(surface.object), static_cast<u32>(size.width),
                                         static_cast<u32>(size.height));
        if (FAILED(result))
            return false;
        window_ = &window;
        return true;
    }
    void releaseWindow(platform::Window& window) override
    {
        if (&window != window_)
            return;
        if (context_.recording()) {
            // Window ownership ends only between frames; fail rather than
            // present to a window the caller is about to destroy.
            fail(E_UNEXPECTED);
            window_ = nullptr;
            return;
        }
        releaseTargets();
        fail(context_.detach());
        window_ = nullptr;
    }
    bool setVSync(platform::Window& window, bool value) override
    {
        if (&window != window_ || lost())
            return false;
        vsync_ = value;
        context_.setVSync(value);
        return true;
    }
    PresentMode presentMode(platform::Window& window) const override
    {
        return &window == window_ ? (vsync_ ? PresentMode::Vsync : PresentMode::Mailbox) : PresentMode::None;
    }
    BufferHandle createBuffer(const BufferDesc& desc) override
    {
        return lost() ? BufferHandle{} : resources_.createBuffer(desc);
    }
    TextureHandle createTexture(const TextureDesc& desc) override
    {
        return lost() ? TextureHandle{} : resources_.createTexture(desc);
    }
    SamplerHandle createSampler(const SamplerDesc& desc) override
    {
        return lost() ? SamplerHandle{} : descriptors_.createSampler(desc);
    }
    ShaderHandle createShader(const ShaderDesc& desc) override
    {
        return lost() ? ShaderHandle{} : pipelines_.createShader(desc);
    }
    PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override
    {
        return lost() ? PipelineHandle{} : pipelines_.createGraphics(desc);
    }
    ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override
    {
        return lost() ? ComputePipelineHandle{} : pipelines_.createCompute(desc);
    }
    void destroy(BufferHandle value) override
    {
        if (!lost())
            resources_.destroy(value);
    }
    void destroy(TextureHandle value) override
    {
        if (!lost())
            resources_.destroy(value);
    }
    void destroy(SamplerHandle value) override
    {
        if (!lost())
            descriptors_.destroy(value);
    }
    void destroy(ShaderHandle value) override
    {
        if (!lost())
            pipelines_.destroy(value);
    }
    void destroy(PipelineHandle value) override
    {
        if (!lost())
            pipelines_.destroy(value);
    }
    void destroy(ComputePipelineHandle value) override
    {
        if (!lost())
            pipelines_.destroy(value);
    }
    ICmdList* beginFrame() override
    {
        reportValidation();
        if (lost() || context_.recording())
            return nullptr;
        if (window_) {
            const auto size = platform::windowPixelSize(*window_);
            if (size.width <= 0 || size.height <= 0)
                return nullptr;
            if (static_cast<u32>(size.width) != context_.width() ||
                static_cast<u32>(size.height) != context_.height()) {
                releaseTargets();
                fail(context_.resize(static_cast<u32>(size.width), static_cast<u32>(size.height)));
            }
        }
        if (lost())
            return nullptr;
        // Completed objects/pages are reclaimed without draining the queue.
        // Context::begin fences only the allocator/descriptor slot being reused.
        fail(timed("resource_retirement", [&] { return resources_.collect(); }), "resource retirement");
        fail(timed("pipeline_retirement", [&] { return pipelines_.collect(); }), "pipeline retirement");
        if (lost())
            return nullptr;
        const HRESULT result = timed("begin_frame", [&] { return context_.begin(); });
        fail(result, "begin frame");
        if (result == S_OK && !lost()) {
            descriptors_.reset();
            commands_.reset();
        }
        return result == S_OK && !lost() ? &commands_ : nullptr;
    }
    Swapchain acquireSwapchain(platform::Window& window) override
    {
        if (lost() || !context_.recording() || &window != window_)
            return {};
        auto* native = context_.backbuffer();
        for (auto& target : targets_) {
            if (target.native == native)
                return {target.handle, context_.width(), context_.height(), TextureFormat::Bgra8Unorm};
        }
        for (auto& target : targets_) {
            if (!target.native) {
                target.handle = resources_.importTarget(native, context_.width(), context_.height());
                if (!target.handle.valid()) {
                    fail(E_OUTOFMEMORY);
                    return {};
                }
                target.native = native;
                return {target.handle, context_.width(), context_.height(), TextureFormat::Bgra8Unorm};
            }
        }
        fail(E_UNEXPECTED);
        return {};
    }
    void submitAndPresent() override
    {
        if (lost() || !context_.recording())
            return;
        commands_.endRenderPass();
        commands_.endComputePass();
        fail(commands_.status(), "record commands");
        if (lost())
            return;
        for (const auto& target : targets_) {
            if (target.native == context_.backbuffer()) {
                if (auto* texture = resources_.texture(target.handle))
                    resources_.transition(*texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
        fail(timed("submit_present", [&] { return context_.submit(); }), "submit/present");
    }
    void waitIdle() override
    {
        if (!lost())
            fail(context_.waitIdle());
    }
    bool readTexture(TextureHandle value, std::span<std::byte> out) override
    {
        return !lost() && SUCCEEDED(resources_.readTexture(value, out));
    }
    bool readBuffer(BufferHandle value, u32 offset, std::span<std::byte> out) override
    {
        return !lost() && SUCCEEDED(resources_.readBuffer(value, offset, out));
    }
    DeviceMemory memory() const noexcept override { return resources_.memory(); }

private:
    template <typename Operation>
    static HRESULT timed(std::string_view name, Operation&& operation)
    {
        const auto started = std::chrono::steady_clock::now();
        const HRESULT result = operation();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
        if (elapsed.count() >= 100.0) {
            const core::I18nArg args[]{{"operation", name}, {"milliseconds", elapsed.count()}};
            core::log(core::LogLevel::Warn, ENG_TR("rhi.warn.native_wait"), args);
        }
        return result;
    }

    void reportValidation()
    {
        if (!validation_)
            return;
        const auto count = validation_->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T bytes = 0;
            if (FAILED(validation_->GetMessage(index, nullptr, &bytes)))
                continue;
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(validation_->GetMessage(index, message, &bytes)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                core::logText(core::LogLevel::Warn, message->pDescription);
        }
        validation_->ClearStoredMessages();
    }
    void fail(HRESULT result, std::string_view operation = "device operation")
    {
        if (FAILED(result) && SUCCEEDED(failure_)) {
            failure_ = result;
            reportValidation();
            const HRESULT removed = context_.device() != nullptr ? context_.device()->GetDeviceRemovedReason() : S_OK;
            const core::I18nArg args[]{
                {"reason", "native D3D12 " + std::string(operation) +
                               " HRESULT=" + std::to_string(static_cast<unsigned long>(result)) +
                               " removedReason=" + std::to_string(static_cast<unsigned long>(removed))}};
            core::log(core::LogLevel::Error, ENG_TR("rhi.err.device_lost"), args);
            context_.abandon();
        }
    }
    void releaseTargets()
    {
        // ResizeBuffers/detach requires every reference to the old backbuffers
        // released. This explicit surface transition is allowed to drain.
        fail(context_.waitIdle(), "release swapchain targets");
        if (lost())
            return;
        for (auto& target : targets_) {
            if (target.handle.valid())
                resources_.destroy(target.handle);
            target = {};
        }
        fail(resources_.collect());
    }
    Context context_;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation_;
    Resources resources_;
    Pipelines pipelines_;
    Descriptors descriptors_;
    Commands commands_;
    struct Target
    {
        ID3D12Resource* native = nullptr;
        TextureHandle handle{};
    };
    std::array<Target, Context::FrameCount> targets_{};
    platform::Window* window_ = nullptr;
    Capabilities caps_{};
    std::string adapter_;
    HRESULT failure_ = S_OK;
    bool vsync_ = true;
};
} // namespace
} // namespace engine::rhi::d3d12

namespace engine::rhi {
DeviceResult createD3D12Device(const DeviceDesc& desc, std::span<const std::byte> blitVertex,
                               std::span<const std::byte> blitFragment, core::EngineError* outError)
{
    auto device = std::make_unique<d3d12::Device>();
    const HRESULT result = device->initialize(desc, blitVertex, blitFragment);
    if (FAILED(result)) {
        if (outError)
            *outError = core::makeError(ENG_TR("rhi.err.device_create_failed"), {},
                                        "native D3D12 HRESULT=" + std::to_string(static_cast<unsigned long>(result)));
        return nullptr;
    }
    return device;
}
} // namespace engine::rhi
