#include "context.h"

#include <limits>

namespace engine::rhi::d3d12 {

Context::~Context()
{
    abandon();
    (void)waitIdle();
    if (event_)
        CloseHandle(event_);
    for (auto& frame : frames_) {
        frame.buffer.Reset();
        frame.allocator.Reset();
    }
    swapchain_.Reset();
    targets_.Reset();
    commands_.Reset();
    queue_.Reset();
    fence_.Reset();
    device_.Reset();
}

void Context::abandon() noexcept
{
    if (recording_ && commands_)
        (void)commands_->Close();
    recording_ = false;
    abandoned_ = true;
}

HRESULT Context::attachHwnd(HWND window, std::uint32_t width, std::uint32_t height)
{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    if (!device_ || !window || swapchain_ || recording_ || !width || !height)
        return E_INVALIDARG;
    Ptr<IDXGIFactory2> factory;
    HRESULT result = createFactory(IID_PPV_ARGS(&factory));
    if (FAILED(result))
        return remember(result);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FrameCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Ptr<IDXGISwapChain1> created;
    result = factory->CreateSwapChainForHwnd(queue_.Get(), window, &desc, nullptr, nullptr, &created);
    if (FAILED(result))
        return remember(result);
    result = created.As(&swapchain_);
    if (FAILED(result))
        return remember(result);
    width_ = width;
    height_ = height;
    return buffers();
#else
    (void)window;
    (void)width;
    (void)height;
    return E_NOTIMPL;
#endif
}

HRESULT Context::detach()
{
    if (recording_)
        return E_UNEXPECTED;
    const HRESULT result = waitIdle();
    if (FAILED(result))
        return result;
    for (auto& frame : frames_)
        frame.buffer.Reset();
    swapchain_.Reset();
    width_ = height_ = 0;
    return S_OK;
}

HRESULT Context::remember(HRESULT result) noexcept
{
    // Once a driver is gone nothing may submit work to it again. Ordinary API
    // failures remain recoverable, and are returned to the owning host.
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG)
        failure_ = result;
    return result;
}

HRESULT Context::initialize(bool debug, IDXGIAdapter* adapter)
{
    if (device_)
        return E_UNEXPECTED;
        // Graphics Tools are optional on desktop and their debug entry point is
        // not a loader requirement for an Xbox AppContainer executable.
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    if (debug) {
        Ptr<ID3D12Debug> validation;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&validation))))
            validation->EnableDebugLayer();
    }
#else
    (void)debug;
#endif
    HRESULT result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(result))
        return remember(result);
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_));
    if (FAILED(result))
        return remember(result);
    for (auto& frame : frames_) {
        result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result))
            return remember(result);
    }
    result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands_));
    if (FAILED(result))
        return remember(result);
    result = commands_->Close();
    if (FAILED(result))
        return remember(result);
    result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(result))
        return remember(result);
    event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!event_)
        return HRESULT_FROM_WIN32(GetLastError());
    D3D12_DESCRIPTOR_HEAP_DESC targets{};
    targets.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targets.NumDescriptors = FrameCount;
    result = device_->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&targets_));
    if (FAILED(result))
        return remember(result);
    targetStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return S_OK;
}

HRESULT Context::attachCoreWindow(IUnknown* window, std::uint32_t width, std::uint32_t height)
{
    if (!device_ || !window || swapchain_ || recording_ || width == 0 || height == 0)
        return E_INVALIDARG;
    Ptr<IDXGIFactory2> factory;
    HRESULT result = createFactory(IID_PPV_ARGS(&factory));
    if (FAILED(result))
        return remember(result);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FrameCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Ptr<IDXGISwapChain1> created;
    result = factory->CreateSwapChainForCoreWindow(queue_.Get(), window, &desc, nullptr, &created);
    if (FAILED(result))
        return remember(result);
    result = created.As(&swapchain_);
    if (FAILED(result))
        return remember(result);
    width_ = width;
    height_ = height;
    return buffers();
}

HRESULT Context::buffers()
{
    auto handle = targets_->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < FrameCount; ++index) {
        const HRESULT result = swapchain_->GetBuffer(index, IID_PPV_ARGS(&frames_[index].buffer));
        if (FAILED(result))
            return remember(result);
        device_->CreateRenderTargetView(frames_[index].buffer.Get(), nullptr, handle);
        handle.ptr += targetStride_;
    }
    frame_ = swapchain_->GetCurrentBackBufferIndex();
    return S_OK;
}

HRESULT Context::resize(std::uint32_t width, std::uint32_t height)
{
    if (!swapchain_ || recording_)
        return E_UNEXPECTED;
    if (width == 0 || height == 0) {
        width_ = width;
        height_ = height;
        return S_FALSE;
    }
    if (width == width_ && height == height_)
        return S_OK;
    HRESULT result = waitIdle();
    if (FAILED(result))
        return result;
    for (auto& frame : frames_)
        frame.buffer.Reset();
    result = swapchain_->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(result))
        return remember(result);
    width_ = width;
    height_ = height;
    return buffers();
}

HRESULT Context::wait(std::uint64_t value)
{
    if (FAILED(failure_))
        return failure_;
    if (!fence_ || value == 0)
        return S_OK;
    const auto completed = fence_->GetCompletedValue();
    if (completed == (std::numeric_limits<std::uint64_t>::max)())
        return remember(DXGI_ERROR_DEVICE_REMOVED);
    if (completed >= value)
        return S_OK;
    const HRESULT result = fence_->SetEventOnCompletion(value, event_);
    if (FAILED(result))
        return remember(result);
    // Finite waits let a removed device be detected instead of hanging forever
    // on a fence a lost GPU will never signal.
    for (;;) {
        const DWORD waited = WaitForSingleObjectEx(event_, 250, FALSE);
        if (waited == WAIT_OBJECT_0)
            return S_OK;
        if (waited != WAIT_TIMEOUT)
            return HRESULT_FROM_WIN32(GetLastError());
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed))
            return remember(removed);
    }
}

HRESULT Context::begin()
{
    if (FAILED(failure_))
        return failure_;
    if (!device_ || !event_ || recording_ || abandoned_)
        return E_UNEXPECTED;
    if (suspended_ || (swapchain_ && (width_ == 0 || height_ == 0)))
        return S_FALSE;
    if (swapchain_)
        frame_ = swapchain_->GetCurrentBackBufferIndex();
    HRESULT result = wait(frames_[frame_].submitted);
    if (FAILED(result))
        return result;
    result = frames_[frame_].allocator->Reset();
    if (FAILED(result))
        return remember(result);
    result = commands_->Reset(frames_[frame_].allocator.Get(), nullptr);
    if (FAILED(result))
        return remember(result);
    recording_ = true;
    if (swapchain_) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backbuffer();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commands_->ResourceBarrier(1, &barrier);
    }
    return S_OK;
}

HRESULT Context::submit(bool present)
{
    if (!recording_ || FAILED(failure_))
        return FAILED(failure_) ? failure_ : E_UNEXPECTED;
    if (swapchain_) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backbuffer();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commands_->ResourceBarrier(1, &barrier);
    }
    recording_ = false;
    HRESULT result = commands_->Close();
    if (FAILED(result))
        return remember(result);
    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    // Signal even when Present fails without removing the device: allocators
    // must never be reset while the queue still uses their submitted work.
    const HRESULT presented = present && swapchain_ ? swapchain_->Present(vsync_ ? 1 : 0, 0) : S_OK;
    result = queue_->Signal(fence_.Get(), ++serial_);
    if (FAILED(result))
        return remember(result);
    frames_[frame_].submitted = serial_;
    if (!swapchain_)
        frame_ = (frame_ + 1) % FrameCount;
    return remember(presented);
}

HRESULT Context::waitIdle()
{
    if (FAILED(failure_))
        return failure_;
    if (!queue_ || !fence_ || !event_)
        return S_OK;
    // Waiting for previously submitted work must not consume the serial that
    // upload/retirement records reserve for the current, still-open list.
    if (recording_)
        return wait(serial_);
    const HRESULT result = queue_->Signal(fence_.Get(), ++serial_);
    return FAILED(result) ? remember(result) : wait(serial_);
}

HRESULT Context::completed(std::uint64_t& value)
{
    value = 0;
    if (FAILED(failure_))
        return failure_;
    if (!fence_)
        return S_OK;
    value = fence_->GetCompletedValue();
    return value == (std::numeric_limits<std::uint64_t>::max)() ? remember(DXGI_ERROR_DEVICE_REMOVED) : S_OK;
}

ID3D12Resource* Context::backbuffer() const noexcept
{
    return frames_[frame_].buffer.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Context::target() const noexcept
{
    auto result = targets_ ? targets_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    result.ptr += static_cast<SIZE_T>(frame_) * targetStride_;
    return result;
}

HRESULT Context::createFactory(REFIID type, void** result)
{
    return CreateDXGIFactory2(0, type, result);
}

HRESULT Context::serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** blob, ID3DBlob** diagnostic)
{
    return D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob, diagnostic);
}

} // namespace engine::rhi::d3d12
