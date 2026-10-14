#include "resources.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace engine::rhi::d3d12 {
namespace {
bool compressed(TextureFormat value) noexcept
{
    return value >= TextureFormat::Bc1RgbaUnorm && value <= TextureFormat::Bc7RgbaUnormSrgb;
}
std::uint32_t texelBytes(TextureFormat value) noexcept
{
    switch (value) {
    case TextureFormat::R8Unorm:
        return 1;
    case TextureFormat::Rg8Unorm:
    case TextureFormat::D16Unorm:
        return 2;
    case TextureFormat::Rgba8Unorm:
    case TextureFormat::Rgba8UnormSrgb:
    case TextureFormat::Bgra8Unorm:
    case TextureFormat::Bgra8UnormSrgb:
    case TextureFormat::R32Float:
    case TextureFormat::Rg16Float:
    case TextureFormat::R32Uint:
    case TextureFormat::D24UnormS8Uint:
    case TextureFormat::D32Float:
        return 4;
    case TextureFormat::Rgba16Float:
    case TextureFormat::D32FloatS8Uint:
        return 8;
    case TextureFormat::Rgba32Float:
        return 16;
    default:
        return 0;
    }
}
DXGI_FORMAT resourceFormat(TextureFormat value) noexcept
{
    switch (value) {
    case TextureFormat::D16Unorm:
        return DXGI_FORMAT_R16_TYPELESS;
    case TextureFormat::D24UnormS8Uint:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case TextureFormat::D32Float:
        return DXGI_FORMAT_R32_TYPELESS;
    case TextureFormat::D32FloatS8Uint:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return format(value);
    }
}
D3D12_RESOURCE_DESC bufferDesc(std::uint64_t bytes)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
} // namespace

DXGI_FORMAT format(TextureFormat value) noexcept
{
    switch (value) {
    case TextureFormat::R8Unorm:
        return DXGI_FORMAT_R8_UNORM;
    case TextureFormat::Rg8Unorm:
        return DXGI_FORMAT_R8G8_UNORM;
    case TextureFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Rgba8UnormSrgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Bgra8UnormSrgb:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case TextureFormat::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::Rg16Float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case TextureFormat::R32Uint:
        return DXGI_FORMAT_R32_UINT;
    case TextureFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Rgba32Float:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::D16Unorm:
        return DXGI_FORMAT_D16_UNORM;
    case TextureFormat::D24UnormS8Uint:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::D32Float:
        return DXGI_FORMAT_D32_FLOAT;
    case TextureFormat::D32FloatS8Uint:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case TextureFormat::Bc1RgbaUnorm:
        return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::Bc1RgbaUnormSrgb:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case TextureFormat::Bc3RgbaUnorm:
        return DXGI_FORMAT_BC3_UNORM;
    case TextureFormat::Bc3RgbaUnormSrgb:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case TextureFormat::Bc5RgUnorm:
        return DXGI_FORMAT_BC5_UNORM;
    case TextureFormat::Bc7RgbaUnorm:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::Bc7RgbaUnormSrgb:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

std::uint32_t rowBytes(TextureFormat value, std::uint32_t width) noexcept
{
    if (compressed(value)) {
        const std::uint32_t block =
            value == TextureFormat::Bc1RgbaUnorm || value == TextureFormat::Bc1RgbaUnormSrgb ? 8u : 16u;
        return ((width + 3) / 4) * block;
    }
    return width * texelBytes(value);
}
std::uint32_t rowCount(TextureFormat value, std::uint32_t height) noexcept
{
    return compressed(value) ? (height + 3) / 4 : height;
}

D3D12_CPU_DESCRIPTOR_HANDLE Resources::Texture::target(std::uint32_t layer, std::uint32_t mip) const noexcept
{
    if (!targets || layer >= desc.layers || mip >= desc.mipLevels)
        return {};
    auto result = targets->GetCPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<SIZE_T>(layer * desc.mipLevels + mip) * targetStride;
    return result;
}
D3D12_GPU_VIRTUAL_ADDRESS Resources::Upload::address() const noexcept
{
    return resource ? resource->GetGPUVirtualAddress() + offset : 0;
}

Resources::~Resources()
{
    (void)context_.waitIdle();
    for (auto& page : pages_)
        if (page.mapped)
            page.native->Unmap(0, nullptr);
}

Resources::Buffer* Resources::buffer(BufferHandle handle) noexcept
{
    return handle.id && handle.id <= buffers_.size() && buffers_[handle.id - 1].native ? &buffers_[handle.id - 1]
                                                                                       : nullptr;
}
Resources::Texture* Resources::texture(TextureHandle handle) noexcept
{
    return handle.id && handle.id <= textures_.size() && textures_[handle.id - 1].native ? &textures_[handle.id - 1]
                                                                                         : nullptr;
}

BufferHandle Resources::createBuffer(const BufferDesc& desc)
{
    if (!context_.device() || FAILED(context_.status()) || !desc.sizeBytes)
        return {};
    Buffer value;
    value.desc = desc;
    // Caller-owned debugName views must not be retained past this call.
    value.desc.debugName = {};
    auto native = bufferDesc(desc.sizeBytes);
    if (hasUsage(desc.usage, BufferUsage::ComputeStorageWrite))
        native.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native, value.state, nullptr,
                                                          IID_PPV_ARGS(&value.native))))
        return {};
    buffers_.push_back(std::move(value));
    memory_.bufferBytes += desc.sizeBytes;
    ++memory_.buffers;
    return {static_cast<u32>(buffers_.size())};
}

TextureHandle Resources::createTexture(const TextureDesc& desc)
{
    if (!context_.device() || FAILED(context_.status()) || !desc.width || !desc.height || !desc.layers ||
        !desc.mipLevels || desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.layers > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        desc.mipLevels > 15 || format(desc.format) == DXGI_FORMAT_UNKNOWN)
        return {};
    Texture value;
    value.desc = desc;
    value.desc.debugName = {};
    value.depth = isDepthFormat(desc.format);
    D3D12_RESOURCE_DESC native{};
    native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    native.Width = desc.width;
    native.Height = desc.height;
    native.DepthOrArraySize = static_cast<UINT16>(desc.layers);
    native.MipLevels = static_cast<UINT16>(desc.mipLevels);
    native.Format = resourceFormat(desc.format);
    native.SampleDesc.Count = 1;
    if (hasUsage(desc.usage, TextureUsage::ColorTarget))
        native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (hasUsage(desc.usage, TextureUsage::DepthStencilTarget))
        native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (hasUsage(desc.usage, TextureUsage::ComputeStorageWrite) ||
        hasUsage(desc.usage, TextureUsage::ComputeStorageReadWrite))
        native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(context_.device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &native, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&value.native))))
        return {};
    D3D12_FEATURE_DATA_FORMAT_INFO info{native.Format, 1};
    if (FAILED(context_.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))))
        return {};
    value.states.resize(static_cast<std::size_t>(desc.mipLevels) * desc.layers * info.PlaneCount,
                        D3D12_RESOURCE_STATE_COMMON);
    for (u32 mip = 0; mip < desc.mipLevels; ++mip)
        value.bytes += static_cast<u64>(rowBytes(desc.format, (std::max)(1u, desc.width >> mip))) *
                       rowCount(desc.format, (std::max)(1u, desc.height >> mip)) * desc.layers;
    if ((native.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
        D3D12_DESCRIPTOR_HEAP_DESC targets{};
        targets.Type = value.depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targets.NumDescriptors = desc.layers * desc.mipLevels;
        if (FAILED(context_.device()->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&value.targets))))
            return {};
        value.targetStride = context_.device()->GetDescriptorHandleIncrementSize(targets.Type);
        for (u32 layer = 0; layer < desc.layers; ++layer) {
            for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
                if (value.depth) {
                    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
                    view.Format = format(desc.format);
                    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipSlice = mip;
                    view.Texture2DArray.FirstArraySlice = layer;
                    view.Texture2DArray.ArraySize = 1;
                    context_.device()->CreateDepthStencilView(value.native.Get(), &view, value.target(layer, mip));
                }
                else {
                    D3D12_RENDER_TARGET_VIEW_DESC view{};
                    view.Format = format(desc.format);
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipSlice = mip;
                    view.Texture2DArray.FirstArraySlice = layer;
                    view.Texture2DArray.ArraySize = 1;
                    context_.device()->CreateRenderTargetView(value.native.Get(), &view, value.target(layer, mip));
                }
            }
        }
    }
    memory_.textureBytes += value.bytes;
    ++memory_.textures;
    textures_.push_back(std::move(value));
    return {static_cast<u32>(textures_.size())};
}

TextureHandle Resources::importTarget(ID3D12Resource* resource, u32 width, u32 height)
{
    if (!resource || !width || !height)
        return {};
    Texture value;
    value.native = resource;
    value.borrowed = true;
    value.desc = {
        .format = TextureFormat::Bgra8Unorm, .usage = TextureUsage::ColorTarget, .width = width, .height = height};
    value.states = {D3D12_RESOURCE_STATE_RENDER_TARGET};
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
    if (FAILED(context_.device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&value.targets))))
        return {};
    value.targetStride = context_.device()->GetDescriptorHandleIncrementSize(heap.Type);
    context_.device()->CreateRenderTargetView(resource, nullptr, value.target());
    textures_.push_back(std::move(value));
    return {static_cast<u32>(textures_.size())};
}

void Resources::destroy(BufferHandle handle)
{
    if (auto* value = buffer(handle)) {
        memory_.bufferBytes -= value->desc.sizeBytes;
        --memory_.buffers;
        retired_.push_back({std::move(value->native), context_.retirementValue()});
    }
}
void Resources::destroy(TextureHandle handle)
{
    if (auto* value = texture(handle)) {
        memory_.textureBytes -= value->bytes;
        if (!value->borrowed)
            --memory_.textures;
        const auto fence = context_.retirementValue();
        retired_.push_back({std::move(value->native), fence});
        retiredTargets_.push_back({std::move(value->targets), fence});
    }
}
HRESULT Resources::collect()
{
    if (context_.recording())
        return E_UNEXPECTED;
    std::uint64_t completed = 0;
    const HRESULT result = context_.completed(completed);
    if (FAILED(result))
        return result;
    const auto finished = [completed](const auto& value) { return value.fence <= completed; };
    std::erase_if(retired_, finished);
    std::erase_if(retiredTargets_, finished);
    for (auto& page : pages_)
        if (page.fence <= completed)
            page.used = 0;
    return S_OK;
}

HRESULT Resources::allocateUpload(u64 bytes, u64 alignment, Upload& out)
{
    out = {};
    if (!context_.recording() || !bytes || !alignment || (alignment & (alignment - 1)) != 0 ||
        bytes > (std::numeric_limits<u32>::max)())
        return E_INVALIDARG;
    for (auto& page : pages_) {
        const u64 start = (page.used + alignment - 1) & ~(alignment - 1);
        if (start <= page.size && bytes <= page.size - start) {
            page.used = start + bytes;
            page.fence = context_.retirementValue();
            out = {page.native.Get(), page.mapped + start, start};
            return S_OK;
        }
    }
    Page page;
    page.size = (std::max)(u64{1} << 20, (bytes + alignment - 1) & ~(alignment - 1));
    const auto desc = bufferDesc(page.size);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    HRESULT result = context_.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page.native));
    if (FAILED(result))
        return result;
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    result = page.native->Map(0, &empty, &mapped);
    if (FAILED(result))
        return result;
    page.mapped = static_cast<std::byte*>(mapped);
    page.used = bytes;
    page.fence = context_.retirementValue();
    out = {page.native.Get(), page.mapped, 0};
    pages_.push_back(std::move(page));
    return S_OK;
}

void Resources::transition(Buffer& value, D3D12_RESOURCE_STATES state)
{
    if (value.state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {value.native.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, value.state, state};
    context_.commands()->ResourceBarrier(1, &barrier);
    value.state = state;
}
void Resources::transition(Texture& value, D3D12_RESOURCE_STATES state, u32 subresource)
{
    const u32 first = subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0u : subresource;
    const u32 end = subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? static_cast<u32>(value.states.size())
                                                                           : subresource + 1;
    for (u32 index = first; index < end && index < value.states.size(); ++index) {
        if (value.states[index] == state)
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {value.native.Get(), index, value.states[index], state};
        context_.commands()->ResourceBarrier(1, &barrier);
        value.states[index] = state;
    }
}

HRESULT Resources::upload(BufferHandle handle, std::span<const std::byte> bytes, u32 offset)
{
    auto* value = buffer(handle);
    if (!value || !context_.recording() || bytes.empty() || offset > value->desc.sizeBytes ||
        bytes.size() > value->desc.sizeBytes - offset)
        return E_INVALIDARG;
    Upload staging;
    const HRESULT result = allocateUpload(bytes.size(), 4, staging);
    if (FAILED(result))
        return result;
    std::memcpy(staging.mapped, bytes.data(), bytes.size());
    transition(*value, D3D12_RESOURCE_STATE_COPY_DEST);
    context_.commands()->CopyBufferRegion(value->native.Get(), offset, staging.resource, staging.offset, bytes.size());
    return S_OK;
}
HRESULT Resources::uploadTexture(TextureHandle handle, std::span<const std::byte> bytes, u32 mip)
{
    auto* value = texture(handle);
    if (!value || mip >= value->desc.mipLevels)
        return E_INVALIDARG;
    return uploadRegion(*value, mip, 0, 0, (std::max)(1u, value->desc.width >> mip),
                        (std::max)(1u, value->desc.height >> mip), bytes);
}
HRESULT Resources::uploadTextureRegion(TextureHandle handle, u32 x, u32 y, u32 width, u32 height,
                                       std::span<const std::byte> bytes)
{
    auto* value = texture(handle);
    return value ? uploadRegion(*value, 0, x, y, width, height, bytes) : E_INVALIDARG;
}
HRESULT Resources::uploadRegion(Texture& value, u32 mip, u32 x, u32 y, u32 width, u32 height,
                                std::span<const std::byte> bytes)
{
    const u32 limitX = (std::max)(1u, value.desc.width >> mip), limitY = (std::max)(1u, value.desc.height >> mip);
    const u32 row = rowBytes(value.desc.format, width), rows = rowCount(value.desc.format, height);
    if (!context_.recording() || !width || !height || x > limitX || y > limitY || width > limitX - x ||
        height > limitY - y || value.depth || !row || bytes.size() < static_cast<u64>(row) * rows)
        return E_INVALIDARG;
    if (compressed(value.desc.format) &&
        ((x % 4) || (y % 4) || ((width % 4) && x + width != limitX) || ((height % 4) && y + height != limitY)))
        return E_INVALIDARG;
    const u32 pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Upload staging;
    const HRESULT result =
        allocateUpload(static_cast<u64>(pitch) * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, staging);
    if (FAILED(result))
        return result;
    for (u32 index = 0; index < rows; ++index)
        std::memcpy(staging.mapped + static_cast<u64>(pitch) * index, bytes.data() + static_cast<u64>(row) * index,
                    row);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = staging.resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = staging.offset;
    source.PlacedFootprint.Footprint = {format(value.desc.format), width, height, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = value.native.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = mip;
    transition(value, D3D12_RESOURCE_STATE_COPY_DEST, mip);
    context_.commands()->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
    return S_OK;
}

HRESULT Resources::readBuffer(BufferHandle handle, u32 offset, std::span<std::byte> bytes)
{
    auto* value = buffer(handle);
    if (!value || context_.recording() || bytes.empty() || offset > value->desc.sizeBytes ||
        bytes.size() > value->desc.sizeBytes - offset)
        return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    auto desc = bufferDesc(bytes.size());
    Ptr<ID3D12Resource> readback;
    HRESULT result = context_.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result))
        return result;
    result = context_.begin();
    if (result != S_OK)
        return result == S_FALSE ? E_PENDING : result;
    const auto state = value->state;
    transition(*value, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context_.commands()->CopyBufferRegion(readback.Get(), 0, value->native.Get(), offset, bytes.size());
    transition(*value, state);
    result = context_.submit(false);
    if (FAILED(result) || FAILED(result = context_.waitIdle()))
        return result;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, bytes.size()};
    result = readback->Map(0, &range, &mapped);
    if (FAILED(result))
        return result;
    std::memcpy(bytes.data(), mapped, bytes.size());
    const D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);
    return S_OK;
}

HRESULT Resources::readTexture(TextureHandle handle, std::span<std::byte> bytes, u32 mip, u32 layer)
{
    auto* value = texture(handle);
    if (!value || context_.recording() || value->depth || mip >= value->desc.mipLevels || layer >= value->desc.layers)
        return E_INVALIDARG;
    const u32 row = rowBytes(value->desc.format, (std::max)(1u, value->desc.width >> mip)),
              rows = rowCount(value->desc.format, (std::max)(1u, value->desc.height >> mip));
    const u32 subresource = mip + layer * value->desc.mipLevels;
    if (!row || bytes.size() < static_cast<u64>(row) * rows)
        return E_INVALIDARG;
    auto desc = value->native->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    context_.device()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readDesc = bufferDesc(total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    Ptr<ID3D12Resource> readback;
    HRESULT result = context_.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &readDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result))
        return result;
    result = context_.begin();
    if (result != S_OK)
        return result == S_FALSE ? E_PENDING : result;
    const auto state = value->states[subresource];
    transition(*value, D3D12_RESOURCE_STATE_COPY_SOURCE, subresource);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = value->native.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    context_.commands()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(*value, state, subresource);
    result = context_.submit(false);
    if (FAILED(result) || FAILED(result = context_.waitIdle()))
        return result;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    result = readback->Map(0, &range, &mapped);
    if (FAILED(result))
        return result;
    for (u32 index = 0; index < rows; ++index)
        std::memcpy(bytes.data() + static_cast<u64>(row) * index,
                    static_cast<const std::byte*>(mapped) + footprint.Offset +
                        static_cast<u64>(footprint.Footprint.RowPitch) * index,
                    row);
    const D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);
    return S_OK;
}
} // namespace engine::rhi::d3d12
