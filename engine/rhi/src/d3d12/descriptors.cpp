#include "descriptors.h"

#include <cstring>
#include <limits>

namespace engine::rhi::d3d12 {
namespace {
DXGI_FORMAT sampleFormat(TextureFormat value)
{
    switch (value) {
    case TextureFormat::D16Unorm:
        return DXGI_FORMAT_R16_UNORM;
    case TextureFormat::D24UnormS8Uint:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case TextureFormat::D32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::D32FloatS8Uint:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return format(value);
    }
}
} // namespace

HRESULT Descriptors::initialize()
{
    if (!context_.device() || heaps_[0].reads)
        return E_UNEXPECTED;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for (auto& frame : heaps_) {
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = ReadCapacity;
        HRESULT result = context_.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&frame.reads));
        if (FAILED(result))
            return result;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc.NumDescriptors = SamplerCapacity;
        result = context_.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&frame.samplers));
        if (FAILED(result))
            return result;
    }
    readStride_ = context_.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    samplerStride_ = context_.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    return S_OK;
}

void Descriptors::reset() noexcept
{
    frame_ = context_.frameSlot();
    nextRead_ = nextSampler_ = 0;
    zero_ = 0;
    tables_.clear();
    readTables_.clear();
    heapsBound_ = false;
}
std::size_t Descriptors::ReadHash::operator()(const ReadKey& key) const noexcept
{
    u64 hash = 14695981039346656037ull;
    for (u32 index = 0; index < key.count; ++index) {
        hash ^= key.words[index];
        hash *= 1099511628211ull;
    }
    return static_cast<std::size_t>(hash);
}
SamplerHandle Descriptors::createSampler(const SamplerDesc& desc)
{
    samplerDescs_.push_back({desc, true});
    samplerDescs_.back().desc.debugName = {};
    return {static_cast<u32>(samplerDescs_.size())};
}
void Descriptors::destroy(SamplerHandle handle)
{
    if (handle.id && handle.id <= samplerDescs_.size())
        samplerDescs_[handle.id - 1].alive = false;
}
D3D12_CPU_DESCRIPTOR_HANDLE Descriptors::cpu(u32 offset, bool sampler) const noexcept
{
    auto result = (sampler ? heaps_[frame_].samplers : heaps_[frame_].reads)->GetCPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<SIZE_T>(offset) * (sampler ? samplerStride_ : readStride_);
    return result;
}
D3D12_GPU_DESCRIPTOR_HANDLE Descriptors::gpu(u32 offset, bool sampler) const noexcept
{
    auto result = (sampler ? heaps_[frame_].samplers : heaps_[frame_].reads)->GetGPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<UINT64>(offset) * (sampler ? samplerStride_ : readStride_);
    return result;
}
HRESULT Descriptors::uniform(std::span<const std::byte> bytes, D3D12_GPU_VIRTUAL_ADDRESS& address)
{
    address = 0;
    if (bytes.empty() || bytes.size() > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16)
        return E_INVALIDARG;
    Resources::Upload upload;
    const u64 padded = (bytes.size() + 255) & ~u64{255};
    const HRESULT result = resources_.allocateUpload(padded, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, upload);
    if (FAILED(result))
        return result;
    std::memset(upload.mapped, 0, static_cast<std::size_t>(padded));
    std::memcpy(upload.mapped, bytes.data(), bytes.size());
    address = upload.address();
    return S_OK;
}
void Descriptors::textureView(TextureHandle handle, D3D12_CPU_DESCRIPTOR_HANDLE target, Bindings::TextureView range)
{
    auto* value = resources_.texture(handle);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = value ? sampleFormat(value->desc.format) : DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = range.restrictedArray || (value && value->desc.layers > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                                                                    : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
        view.Texture2DArray.MostDetailedMip = range.restrictedArray ? range.mip : 0;
        view.Texture2DArray.FirstArraySlice = range.restrictedArray ? range.layer : 0;
        view.Texture2DArray.MipLevels = range.restrictedArray ? 1 : value->desc.mipLevels;
        view.Texture2DArray.ArraySize = range.restrictedArray ? 1 : value->desc.layers;
    }
    else {
        view.Texture2D.MipLevels = value ? value->desc.mipLevels : 1;
    }
    if (value && range.restrictedArray)
        resources_.transition(*value, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              range.mip + range.layer * value->desc.mipLevels);
    else if (value)
        resources_.transition(*value, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (target.ptr)
        context_.device()->CreateShaderResourceView(value ? value->native.Get() : nullptr, &view, target);
}
void Descriptors::bufferView(BufferHandle handle, D3D12_CPU_DESCRIPTOR_HANDLE target, bool write)
{
    auto* value = resources_.buffer(handle);
    if (write) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = value ? value->desc.sizeBytes / 4 : 1;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        if (value)
            resources_.transition(*value, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context_.device()->CreateUnorderedAccessView(value ? value->native.Get() : nullptr, nullptr, &view, target);
    }
    else {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = value ? value->desc.sizeBytes / 4 : 1;
        view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        if (value)
            resources_.transition(*value, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (target.ptr)
            context_.device()->CreateShaderResourceView(value ? value->native.Get() : nullptr, &view, target);
    }
}
void Descriptors::textureWrite(ComputeTextureWrite handle, D3D12_CPU_DESCRIPTOR_HANDLE target)
{
    auto* value = resources_.texture(handle.texture);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = value ? format(value->desc.format) : DXGI_FORMAT_R32_UINT;
    view.ViewDimension =
        value && value->desc.layers > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
    if (view.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) {
        view.Texture2DArray.MipSlice = handle.mipLevel;
        view.Texture2DArray.ArraySize = value->desc.layers;
    }
    else {
        view.Texture2D.MipSlice = handle.mipLevel;
    }
    if (value) {
        for (u32 layer = 0; layer < value->desc.layers; ++layer)
            resources_.transition(*value, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  handle.mipLevel + layer * value->desc.mipLevels);
    }
    context_.device()->CreateUnorderedAccessView(value ? value->native.Get() : nullptr, nullptr, &view, target);
}

HRESULT Descriptors::bind(const StageLayout& layout, const Bindings& bindings, bool compute)
{
    if (!context_.recording() || !heaps_[frame_].reads || !heaps_[frame_].samplers ||
        layout.uniforms.size() > bindings.uniforms.size() || layout.samplerCount > bindings.textures.size() ||
        layout.bufferReads > bindings.buffers.size() || layout.bufferWrites > bindings.writes.size() ||
        layout.textureReads > bindings.storageTextures.size() || layout.textureWrites > bindings.textureWrites.size())
        return E_INVALIDARG;
    for (u32 index = 0; index < layout.samplerCount; ++index) {
        const auto& range = bindings.textureViews[index];
        if (!range.restrictedArray)
            continue;
        const auto* value = resources_.texture(bindings.textures[index].texture);
        if (!value || range.mip >= value->desc.mipLevels || range.layer >= value->desc.layers)
            return E_INVALIDARG;
    }
    for (u32 index = 0; index < layout.bufferWrites; ++index) {
        const auto* value = resources_.buffer(bindings.writes[index]);
        if (!value || !hasUsage(value->desc.usage, BufferUsage::ComputeStorageWrite))
            return E_INVALIDARG;
    }
    for (u32 index = 0; index < layout.textureWrites; ++index) {
        const auto& write = bindings.textureWrites[index];
        const auto* value = resources_.texture(write.texture);
        if (!value || write.mipLevel >= value->desc.mipLevels ||
            (!hasUsage(value->desc.usage, TextureUsage::ComputeStorageWrite) &&
             !hasUsage(value->desc.usage, TextureUsage::ComputeStorageReadWrite)))
            return E_INVALIDARG;
    }
    auto* cmd = context_.commands();
    if (!heapsBound_) {
        ID3D12DescriptorHeap* heaps[] = {heaps_[frame_].reads.Get(), heaps_[frame_].samplers.Get()};
        cmd->SetDescriptorHeaps(2, heaps);
        heapsBound_ = true;
    }
    const auto table = [&](u32 root, u32 start, bool sampler) {
        if (root == StageLayout::Absent)
            return;
        if (compute)
            cmd->SetComputeRootDescriptorTable(root, gpu(start, sampler));
        else
            cmd->SetGraphicsRootDescriptorTable(root, gpu(start, sampler));
    };
    ReadKey key;
    key.words[key.count++] = layout.samplerCount;
    key.words[key.count++] = layout.textureReads;
    key.words[key.count++] = layout.bufferReads;
    const auto identity = [](const auto* value) -> u64 {
        return value ? reinterpret_cast<std::uintptr_t>(value->native.Get()) : 0;
    };
    for (u32 index = 0; index < layout.samplerCount; ++index) {
        const auto& range = bindings.textureViews[index];
        key.words[key.count++] = identity(resources_.texture(bindings.textures[index].texture));
        key.words[key.count++] = range.restrictedArray;
        key.words[key.count++] = range.restrictedArray ? (static_cast<u64>(range.layer) << 32) | range.mip : 0;
    }
    for (u32 index = 0; index < layout.textureReads; ++index)
        key.words[key.count++] = identity(resources_.texture(bindings.storageTextures[index]));
    for (u32 index = 0; index < layout.bufferReads; ++index)
        key.words[key.count++] = identity(resources_.buffer(bindings.buffers[index]));
    const auto readTable = readTables_.find(key);
    const bool reuse = readTable != readTables_.end();
    const u32 needed = layout.writeCount + (reuse ? 0 : layout.readCount);
    if (needed > ReadCapacity - nextRead_)
        return E_OUTOFMEMORY;
    const u32 firstRead = reuse ? readTable->second : nextRead_;
    // Reusing immutable descriptors must not skip transitions: a texture may
    // have been rendered to, or a buffer uploaded/written, since the last draw.
    const auto readTarget = [&]() { return reuse ? D3D12_CPU_DESCRIPTOR_HANDLE{} : cpu(nextRead_++, false); };
    for (u32 index = 0; index < layout.samplerCount; ++index)
        textureView(bindings.textures[index].texture, readTarget(), bindings.textureViews[index]);
    for (u32 index = 0; index < layout.textureReads; ++index)
        textureView(bindings.storageTextures[index], readTarget());
    for (u32 index = 0; index < layout.bufferReads; ++index)
        bufferView(bindings.buffers[index], readTarget(), false);
    if (!reuse && layout.readCount)
        readTables_.emplace(key, firstRead);
    table(layout.reads, firstRead, false);
    const u32 firstWrite = nextRead_;
    for (u32 index = 0; index < layout.textureWrites; ++index)
        textureWrite(bindings.textureWrites[index], cpu(nextRead_++, false));
    for (u32 index = 0; index < layout.bufferWrites; ++index)
        bufferView(bindings.writes[index], cpu(nextRead_++, false), true);
    table(layout.writes, firstWrite, false);
    if (layout.samplerCount) {
        SamplerTable wanted;
        wanted.count = layout.samplerCount;
        for (u32 index = 0; index < wanted.count; ++index)
            wanted.handles[index] = bindings.textures[index].sampler;
        bool found = false;
        for (const auto& existing : tables_) {
            if (existing.count == wanted.count && existing.handles == wanted.handles) {
                wanted.start = existing.start;
                found = true;
                break;
            }
        }
        if (!found) {
            if (wanted.count > SamplerCapacity - nextSampler_)
                return E_OUTOFMEMORY;
            wanted.start = nextSampler_;
            for (u32 index = 0; index < wanted.count; ++index) {
                const u32 id = wanted.handles[index].id;
                const SamplerDesc source = id && id <= samplerDescs_.size() && samplerDescs_[id - 1].alive
                                               ? samplerDescs_[id - 1].desc
                                               : SamplerDesc{};
                D3D12_SAMPLER_DESC desc{};
                desc.Filter = D3D12_ENCODE_BASIC_FILTER(
                    source.minFilter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                    source.magFilter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                    source.mipmapMode == MipmapMode::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                    D3D12_FILTER_REDUCTION_TYPE_STANDARD);
                desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<unsigned>(source.addressU) + 1);
                desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<unsigned>(source.addressV) + 1);
                desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<unsigned>(source.addressW) + 1);
                desc.MipLODBias = source.mipLodBias;
                desc.MaxAnisotropy = 1;
                desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                desc.MaxLOD = (std::numeric_limits<float>::max)();
                context_.device()->CreateSampler(&desc, cpu(nextSampler_++, true));
            }
            tables_.push_back(wanted);
        }
        table(layout.samplers, wanted.start, true);
    }
    for (std::size_t index = 0; index < layout.uniforms.size(); ++index) {
        auto address = bindings.uniforms[index];
        if (!address) {
            if (!zero_) {
                const std::array<std::byte, 256> empty{};
                const HRESULT result = uniform(empty, zero_);
                if (FAILED(result))
                    return result;
            }
            address = zero_;
        }
        if (compute)
            cmd->SetComputeRootConstantBufferView(layout.uniforms[index], address);
        else
            cmd->SetGraphicsRootConstantBufferView(layout.uniforms[index], address);
    }
    return S_OK;
}
} // namespace engine::rhi::d3d12
