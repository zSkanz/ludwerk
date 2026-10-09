#pragma once

#include <span>
#include <vector>

#include "context.h"
#include "engine/rhi/descs.h"
#include "engine/rhi/device.h"

namespace engine::rhi::d3d12 {

[[nodiscard]] DXGI_FORMAT format(TextureFormat value) noexcept;
[[nodiscard]] std::uint32_t rowBytes(TextureFormat value, std::uint32_t width) noexcept;
[[nodiscard]] std::uint32_t rowCount(TextureFormat value, std::uint32_t height) noexcept;

// Resource ownership and transfer commands used by the native adapter. A stale
// handle never aliases a newly created object. Destruction is fence deferred;
// uploads keep their staging pages until collect() observes the completed queue.
class Resources final
{
public:
    template <typename T>
    using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Buffer
    {
        Ptr<ID3D12Resource> native;
        BufferDesc desc{};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    struct Texture
    {
        Ptr<ID3D12Resource> native;
        Ptr<ID3D12DescriptorHeap> targets;
        TextureDesc desc{};
        std::vector<D3D12_RESOURCE_STATES> states;
        std::uint64_t bytes = 0;
        std::uint32_t targetStride = 0;
        bool depth = false;
        bool borrowed = false;
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE target(std::uint32_t layer = 0, std::uint32_t mip = 0) const noexcept;
    };
    struct Upload
    {
        ID3D12Resource* resource = nullptr;
        std::byte* mapped = nullptr;
        std::uint64_t offset = 0;
        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS address() const noexcept;
    };
    explicit Resources(Context& context) noexcept : context_(context) {}
    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);
    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] TextureHandle importTarget(ID3D12Resource* resource, u32 width, u32 height);
    void destroy(BufferHandle handle);
    void destroy(TextureHandle handle);
    [[nodiscard]] Buffer* buffer(BufferHandle handle) noexcept;
    [[nodiscard]] Texture* texture(TextureHandle handle) noexcept;
    [[nodiscard]] DeviceMemory memory() const noexcept { return memory_; }
    // Only between frames: reclaim completed uploads/objects without draining
    // the queue. Pages still referenced by submitted frames keep their contents.
    HRESULT collect();
    HRESULT upload(BufferHandle handle, std::span<const std::byte> bytes, std::uint32_t offset);
    HRESULT uploadTexture(TextureHandle handle, std::span<const std::byte> bytes, std::uint32_t mip);
    HRESULT uploadTextureRegion(TextureHandle handle, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                std::uint32_t height, std::span<const std::byte> bytes);
    HRESULT readBuffer(BufferHandle handle, std::uint32_t offset, std::span<std::byte> bytes);
    HRESULT readTexture(TextureHandle handle, std::span<std::byte> bytes, u32 mip = 0, u32 layer = 0);
    HRESULT allocateUpload(std::uint64_t bytes, std::uint64_t alignment, Upload& out);
    void transition(Buffer& buffer, D3D12_RESOURCE_STATES state);
    void transition(Texture& texture, D3D12_RESOURCE_STATES state,
                    std::uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

private:
    struct Page
    {
        Ptr<ID3D12Resource> native;
        std::byte* mapped = nullptr;
        std::uint64_t size = 0, used = 0, fence = 0;
    };
    template <typename T>
    struct Retired
    {
        Ptr<T> native;
        std::uint64_t fence = 0;
    };
    HRESULT uploadRegion(Texture& texture, std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                         std::uint32_t height, std::span<const std::byte> bytes);
    Context& context_;
    std::vector<Buffer> buffers_;
    std::vector<Texture> textures_;
    std::vector<Page> pages_;
    std::vector<Retired<ID3D12Resource>> retired_;
    std::vector<Retired<ID3D12DescriptorHeap>> retiredTargets_;
    DeviceMemory memory_{};
};

} // namespace engine::rhi::d3d12
