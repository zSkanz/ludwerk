#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "pipelines.h"
#include "resources.h"

namespace engine::rhi::d3d12 {

struct Bindings
{
    struct TextureView
    {
        u32 mip = 0, layer = 0;
        bool restrictedArray = false;
    };
    std::array<TextureBinding, 16> textures{};
    std::array<TextureView, 16> textureViews{};
    std::array<BufferHandle, 16> buffers{}, writes{};
    std::array<TextureHandle, 16> storageTextures{};
    std::array<ComputeTextureWrite, 16> textureWrites{};
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 16> uniforms{};
};

class Descriptors final
{
public:
    Descriptors(Context& context, Resources& resources) noexcept : context_(context), resources_(resources) {}
    HRESULT initialize();
    // Caller has fenced this Context frame slot before reuse. Sampled descriptor
    // tables are immutable snapshots shared only within the current recording.
    void reset() noexcept;
    SamplerHandle createSampler(const SamplerDesc& desc);
    void destroy(SamplerHandle handle);
    HRESULT bind(const StageLayout& layout, const Bindings& bindings, bool compute);
    HRESULT uniform(std::span<const std::byte> bytes, D3D12_GPU_VIRTUAL_ADDRESS& address);

private:
    struct Sampler
    {
        SamplerDesc desc{};
        bool alive = true;
    };
    struct SamplerTable
    {
        std::array<SamplerHandle, 16> handles{};
        u32 count = 0, start = 0;
    };
    struct ReadKey
    {
        // Native identities also distinguish a destroyed handle from its null view.
        std::array<u64, 83> words{};
        u32 count = 0;
        bool operator==(const ReadKey&) const = default;
    };
    struct ReadHash
    {
        std::size_t operator()(const ReadKey& key) const noexcept;
    };
    void textureView(TextureHandle texture, D3D12_CPU_DESCRIPTOR_HANDLE target, Bindings::TextureView range = {});
    void bufferView(BufferHandle buffer, D3D12_CPU_DESCRIPTOR_HANDLE target, bool write);
    void textureWrite(ComputeTextureWrite texture, D3D12_CPU_DESCRIPTOR_HANDLE target);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(u32 offset, bool sampler) const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(u32 offset, bool sampler) const noexcept;
    Context& context_;
    Resources& resources_;
    struct Heaps
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> reads, samplers;
    };
    std::array<Heaps, Context::FrameCount> heaps_;
    u32 frame_ = 0;
    std::vector<Sampler> samplerDescs_;
    std::vector<SamplerTable> tables_;
    std::unordered_map<ReadKey, u32, ReadHash> readTables_;
    bool heapsBound_ = false;
    u32 readStride_ = 0, samplerStride_ = 0;
    u32 nextRead_ = 0, nextSampler_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS zero_ = 0;
    static constexpr u32 ReadCapacity = 65536, SamplerCapacity = 2048;
};

} // namespace engine::rhi::d3d12
