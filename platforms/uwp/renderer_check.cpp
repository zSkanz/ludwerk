// Hardware validation of the native RHI foundation and shipping DXIL shaders.
// This executable is deliberately NOT named or packaged as the game player.
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include "../../engine/rhi/src/d3d12/context.h"
#include "../../engine/rhi/src/d3d12/pipelines.h"
#include "../../engine/rhi/src/d3d12/resources.h"

namespace engine::uwp {
namespace Windows = winrt::Windows;
using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using Microsoft::WRL::ComPtr;

struct RendererCheck : implements<RendererCheck, IFrameworkViewSource, IFrameworkView>
{
    ~RendererCheck()
    {
        context_.abandon();
        (void)context_.waitIdle();
    }
    IFrameworkView CreateView() { return *this; }
    void log(const std::string& text)
    {
        std::ofstream file(local_ / "renderer-check.log", std::ios::app);
        file << text << '\n';
    }
    void require(HRESULT result, const char* operation)
    {
        if (FAILED(result)) {
            log(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(result)));
            check_hresult(result);
        }
    }
    void Initialize(const CoreApplicationView& view)
    {
        local_ = Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        std::ofstream(local_ / "renderer-check.log", std::ios::trunc) << "NATIVE_D3D12_BEGIN\n";
        view.Activated([this](const auto&, const auto&) { window_.Activate(); });
        suspend_ = CoreApplication::Suspending(auto_revoke, [this](const auto&, const SuspendingEventArgs& args) {
            const auto done = args.SuspendingOperation().GetDeferral();
            require(context_.waitIdle(), "SUSPEND_WAIT");
            context_.suspend(true);
            log("NATIVE_D3D12_SUSPEND");
            done.Complete();
        });
        resume_ = CoreApplication::Resuming(auto_revoke, [this](const auto&, const auto&) {
            context_.suspend(false);
            resize_ = true;
            log("NATIVE_D3D12_RESUME");
        });
    }
    void SetWindow(const CoreWindow& window)
    {
        window_ = window;
        window.Closed([this](const auto&, const auto&) { closed_ = true; });
        window.VisibilityChanged(
            [this](const auto&, const VisibilityChangedEventArgs& event) { visible_ = event.Visible(); });
        window.SizeChanged([this](const auto&, const auto&) { resize_ = true; });
        back_ = SystemNavigationManager::GetForCurrentView().BackRequested(
            auto_revoke, [](const auto&, const BackRequestedEventArgs& event) { event.Handled(true); });
    }
    std::array<std::uint32_t, 2> size()
    {
        const auto bounds = window_.Bounds();
        const double scale =
            Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        return {static_cast<std::uint32_t>(bounds.Width * scale), static_cast<std::uint32_t>(bounds.Height * scale)};
    }
    std::vector<char> shader(const char* name)
    {
        const std::filesystem::path installed(Package::Current().InstalledLocation().Path().c_str());
        std::ifstream input(installed / "shaders" / name, std::ios::binary);
        if (!input)
            throw hresult_error(E_FAIL);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    void Load(const hstring&)
    {
        require(context_.initialize(false), "D3D12_CREATE");
        log("NATIVE_D3D12_DEVICE_READY");
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
        const HRESULT supported =
            context_.device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
        log("SHADER_MODEL=" + std::to_string(static_cast<unsigned>(model.HighestShaderModel)) +
            " QUERY_HRESULT=" + std::to_string(static_cast<unsigned long>(supported)));
        LUID luid = context_.device()->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory;
        require(context_.createFactory(IID_PPV_ARGS(&factory)), "FACTORY");
        ComPtr<IDXGIAdapter1> adapter;
        require(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "ADAPTER");
        DXGI_ADAPTER_DESC1 details{};
        require(adapter->GetDesc1(&details), "ADAPTER_DESC");
        log("ADAPTER=" + to_string(details.Description) +
            " SOFTWARE=" + std::to_string((details.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0));
        const auto pixels = size();
        require(context_.attachCoreWindow(reinterpret_cast<IUnknown*>(get_abi(window_)), pixels[0], pixels[1]),
                "COREWINDOW_SWAPCHAIN");
        log("NATIVE_D3D12_SWAPCHAIN_READY");

        resources_ = std::make_unique<rhi::d3d12::Resources>(context_);
        pipelines_ = std::make_unique<rhi::d3d12::Pipelines>(context_);
        const auto vertex = shader("debug_line.vertex.dxil");
        const auto fragment = shader("debug_line.fragment.dxil");
        const auto vs = pipelines_->createShader({.stage = rhi::ShaderStage::Vertex,
                                                  .format = rhi::ShaderFormat::Dxil,
                                                  .code = std::as_bytes(std::span(vertex)),
                                                  .uniformBufferCount = 1});
        const auto fs = pipelines_->createShader({.stage = rhi::ShaderStage::Fragment,
                                                  .format = rhi::ShaderFormat::Dxil,
                                                  .code = std::as_bytes(std::span(fragment))});
        const rhi::VertexBufferLayout layout[] = {{.slot = 0, .strideBytes = 28}};
        const rhi::VertexAttribute attributes[] = {
            {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3},
            {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 12}};
        const rhi::ColorTargetDesc targets[] = {{.format = rhi::TextureFormat::Bgra8Unorm}};
        pipeline_ = pipelines_->createGraphics({.vertexShader = vs,
                                                .fragmentShader = fs,
                                                .vertexBuffers = layout,
                                                .vertexAttributes = attributes,
                                                .rasterizer = {.cullMode = rhi::CullMode::None},
                                                .colorTargets = targets});
        log("SHIPPING_DXIL_PIPELINE_HANDLE=" + std::to_string(pipeline_.id));
        if (!pipeline_.valid())
            return;
        const float vertices[] = {0,    0.6f, 0, 1,    0.8f,  0.2f, 1,    -0.6f, -0.6f, 0, 0.2f,
                                  0.9f, 0.6f, 1, 0.6f, -0.6f, 0,    0.3f, 0.5f,  1,     1};
        const float identity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        vertices_ = resources_->createBuffer({.usage = rhi::BufferUsage::Vertex, .sizeBytes = sizeof(vertices)});
        transform_ = resources_->createBuffer({.sizeBytes = 256});
        if (!vertices_.valid() || !transform_.valid())
            throw hresult_error(E_OUTOFMEMORY);
        require(context_.begin(), "INITIAL_UPLOAD_BEGIN");
        require(resources_->upload(vertices_, std::as_bytes(std::span(vertices)), 0), "VERTICES_UPLOAD");
        require(resources_->upload(transform_, std::as_bytes(std::span(identity)), 0), "TRANSFORM_UPLOAD");
        resources_->transition(*resources_->buffer(vertices_), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        resources_->transition(*resources_->buffer(transform_), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        require(context_.submit(false), "INITIAL_UPLOAD_SUBMIT");
        require(resources_->collect(), "INITIAL_UPLOAD_COLLECT");
        log("NATIVE_RHI_RESOURCES_AND_LAYOUT_READY");
        log("NATIVE_D3D12_SHIPPING_SHADERS_READY");
    }
    void Run()
    {
        std::uint64_t frames = 0;
        while (!closed_) {
            window_.Dispatcher().ProcessEvents(visible_ ? CoreProcessEventsOption::ProcessAllIfPresent
                                                        : CoreProcessEventsOption::ProcessOneAndAllPending);
            if (closed_ || !visible_)
                continue;
            if (resize_) {
                const auto pixels = size();
                require(context_.resize(pixels[0], pixels[1]), "RESIZE");
                resize_ = false;
            }
            const HRESULT began = context_.begin();
            require(began, "FRAME_BEGIN");
            if (began == S_FALSE)
                continue;
            const auto target = context_.target();
            const float background[] = {0.025f, 0.045f, 0.08f, 1};
            context_.commands()->ClearRenderTargetView(target, background, 0, nullptr);
            context_.commands()->OMSetRenderTargets(1, &target, FALSE, nullptr);
            if (pipeline_.valid()) {
                const auto* pipeline = pipelines_->graphics(pipeline_);
                const D3D12_VIEWPORT viewport{
                    0, 0, static_cast<float>(context_.width()), static_cast<float>(context_.height()), 0, 1};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(context_.width()),
                                         static_cast<LONG>(context_.height())};
                const D3D12_VERTEX_BUFFER_VIEW vertices{resources_->buffer(vertices_)->native->GetGPUVirtualAddress(),
                                                        84, 28};
                auto* cmd = context_.commands();
                cmd->SetPipelineState(pipeline->state.Get());
                cmd->SetGraphicsRootSignature(pipeline->root.Get());
                cmd->SetGraphicsRootConstantBufferView(pipeline->stages[0].uniforms[0],
                                                       resources_->buffer(transform_)->native->GetGPUVirtualAddress());
                cmd->RSSetViewports(1, &viewport);
                cmd->RSSetScissorRects(1, &scissor);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->IASetVertexBuffers(0, 1, &vertices);
                cmd->DrawInstanced(3, 1, 0, 0);
            }
            require(context_.submit(), "FRAME_PRESENT");
            if (++frames == 120)
                log("NATIVE_D3D12_PRESENTED_120_FRAMES");
        }
        require(context_.waitIdle(), "EXIT_WAIT");
    }
    void Uninitialize() {}

    rhi::d3d12::Context context_;
    CoreWindow window_{nullptr};
    std::filesystem::path local_;
    std::unique_ptr<rhi::d3d12::Resources> resources_;
    std::unique_ptr<rhi::d3d12::Pipelines> pipelines_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle vertices_, transform_;
    CoreApplication::Suspending_revoker suspend_;
    CoreApplication::Resuming_revoker resume_;
    SystemNavigationManager::BackRequested_revoker back_;
    bool visible_ = true, closed_ = false, resize_ = false;
};
} // namespace engine::uwp

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        winrt::init_apartment();
        winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<engine::uwp::RendererCheck>());
        return 0;
    } catch (const winrt::hresult_error& error) {
        const std::filesystem::path local(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
        std::ofstream(local / "renderer-check.log", std::ios::app)
            << "NATIVE_D3D12_FATAL HRESULT=" << error.code().value << " " << winrt::to_string(error.message()) << '\n';
        return 1;
    }
}
