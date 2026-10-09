// Backend-private native D3D12 ownership. No SDL, HWND or WinRT projection is
// required: UWP supplies its CoreWindow's IUnknown at the presentation boundary.
#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace engine::rhi::d3d12 {

class Context final
{
public:
    static constexpr std::uint32_t FrameCount = 3;
    Context() = default;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // A software adapter is explicitly injected by desktop GPU tests only.
    HRESULT initialize(bool debug, IDXGIAdapter* adapter = nullptr);
    HRESULT attachCoreWindow(IUnknown* window, std::uint32_t width, std::uint32_t height);
    HRESULT attachHwnd(HWND window, std::uint32_t width, std::uint32_t height);
    HRESULT detach();
    void setVSync(bool value) noexcept { vsync_ = value; }
    HRESULT resize(std::uint32_t width, std::uint32_t height);
    // A zero-sized/suspended surface returns S_FALSE without starting a frame.
    HRESULT begin();
    HRESULT submit(bool present = true);
    // Terminal teardown only: resource states recorded by helpers are no longer
    // authoritative after discarding a list. Already submitted work still fences.
    void abandon() noexcept;
    HRESULT waitIdle();
    // The completed queue, without waiting or submitting another signal.
    HRESULT completed(std::uint64_t& value);
    // An open list owns the next serial, even before submission. Keep its
    // uploads and destroyed objects alive until that serial completes.
    [[nodiscard]] std::uint64_t retirementValue() const noexcept { return serial_ + (recording_ ? 1 : 0); }
    [[nodiscard]] std::uint32_t frameSlot() const noexcept { return frame_; }
    HRESULT createFactory(REFIID type, void** result);
    HRESULT serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** blob, ID3DBlob** diagnostic);
    void suspend(bool value) noexcept { suspended_ = value; }

    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* commands() const noexcept { return commands_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept { return queue_.Get(); }
    [[nodiscard]] ID3D12Resource* backbuffer() const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE target() const noexcept;
    [[nodiscard]] HRESULT status() const noexcept { return failure_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    template <typename T>
    using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Frame
    {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12Resource> buffer;
        std::uint64_t submitted = 0;
    };
    HRESULT remember(HRESULT result) noexcept;
    HRESULT wait(std::uint64_t value);
    HRESULT buffers();

    Ptr<ID3D12Device> device_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12GraphicsCommandList> commands_;
    Ptr<ID3D12Fence> fence_;
    Ptr<IDXGISwapChain3> swapchain_;
    Ptr<ID3D12DescriptorHeap> targets_;
    std::array<Frame, FrameCount> frames_{};
    HANDLE event_ = nullptr;
    std::uint64_t serial_ = 0;
    std::uint32_t frame_ = 0;
    std::uint32_t targetStride_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    HRESULT failure_ = S_OK;
    bool recording_ = false;
    bool suspended_ = false;
    bool abandoned_ = false;
    bool vsync_ = true;
};

} // namespace engine::rhi::d3d12
