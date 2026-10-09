#include "commands.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::rhi::d3d12 {

Commands::Commands(Context& context, Resources& resources, Pipelines& pipelines, Descriptors& descriptors) noexcept
    : context_(context), resources_(resources), pipelines_(pipelines), descriptors_(descriptors)
{}

HRESULT Commands::initialize()
{
    if (!context_.device())
        return E_UNEXPECTED;
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(DrawIndexedIndirectCommand);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    return context_.device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&indexedIndirect_));
}

void Commands::reset() noexcept
{
    graphics_ = {};
    compute_ = {};
    vertices_ = {};
    index_ = {};
    pipeline_ = {};
    computePipeline_ = {};
    boundState_ = nullptr;
    boundGraphicsRoot_ = nullptr;
    boundTopology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    boundVertices_ = {};
    boundIndex_ = {};
    for (auto& stage : uniformSnapshots_)
        for (auto& snapshot : stage)
            snapshot.address = 0;
    failure_ = S_OK;
    debugDepth_ = 0;
    renderPass_ = computePass_ = false;
}

void Commands::fail(HRESULT result) noexcept
{
    if (SUCCEEDED(failure_) && FAILED(result))
        failure_ = result;
}
bool Commands::recording()
{
    if (!context_.recording())
        fail(E_UNEXPECTED);
    return SUCCEEDED(failure_);
}
Bindings* Commands::stage(ShaderStage value)
{
    if (value == ShaderStage::Vertex)
        return &graphics_[0];
    if (value == ShaderStage::Fragment)
        return &graphics_[1];
    fail(E_INVALIDARG);
    return nullptr;
}

void Commands::beginRenderPass(const RenderPassDesc& desc)
{
    if (!recording())
        return;
    endComputePass();
    endRenderPass();
    if (desc.colorAttachments.size() > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) {
        fail(E_INVALIDARG);
        return;
    }
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    u32 width = 0, height = 0;
    for (std::size_t index = 0; index < desc.colorAttachments.size(); ++index) {
        const auto& attachment = desc.colorAttachments[index];
        auto* texture = resources_.texture(attachment.texture);
        if (!texture || texture->depth || !texture->targets || attachment.layer >= texture->desc.layers) {
            fail(E_INVALIDARG);
            return;
        }
        if (width && (width != texture->desc.width || height != texture->desc.height)) {
            fail(E_INVALIDARG);
            return;
        }
        resources_.transition(*texture, D3D12_RESOURCE_STATE_RENDER_TARGET, attachment.layer * texture->desc.mipLevels);
        targets[index] = texture->target(attachment.layer);
        width = texture->desc.width;
        height = texture->desc.height;
        if (attachment.loadOp == LoadOp::Clear) {
            const auto& c = attachment.clearColor;
            const float color[] = {c.r, c.g, c.b, c.a};
            context_.commands()->ClearRenderTargetView(targets[index], color, 0, nullptr);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    if (desc.depthStencil.texture.valid()) {
        auto* texture = resources_.texture(desc.depthStencil.texture);
        if (!texture || !texture->depth || !texture->targets) {
            fail(E_INVALIDARG);
            return;
        }
        if (width && (width != texture->desc.width || height != texture->desc.height)) {
            fail(E_INVALIDARG);
            return;
        }
        resources_.transition(*texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        depth = texture->target();
        if (!width) {
            width = texture->desc.width;
            height = texture->desc.height;
        }
        if (desc.depthStencil.loadOp == LoadOp::Clear)
            context_.commands()->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, desc.depthStencil.clearDepth, 0,
                                                       0, nullptr);
    }
    if (!width || !height) {
        fail(E_INVALIDARG);
        return;
    }
    context_.commands()->OMSetRenderTargets(static_cast<UINT>(desc.colorAttachments.size()), targets.data(), FALSE,
                                            depth.ptr ? &depth : nullptr);
    renderPass_ = true;
    setViewport({0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
    setScissor({0, 0, static_cast<i32>(width), static_cast<i32>(height)});
}

void Commands::endRenderPass()
{
    if (renderPass_ && context_.recording())
        context_.commands()->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    renderPass_ = false;
}
void Commands::setPipeline(PipelineHandle pipeline)
{
    pipeline_ = pipeline;
}
void Commands::setViewport(const Viewport& v)
{
    if (!recording())
        return;
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.width) || !std::isfinite(v.height) ||
        !std::isfinite(v.minDepth) || !std::isfinite(v.maxDepth) || v.width < 0 || v.height < 0 || v.minDepth < 0 ||
        v.maxDepth > 1 || v.minDepth > v.maxDepth) {
        fail(E_INVALIDARG);
        return;
    }
    const D3D12_VIEWPORT viewport{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
    context_.commands()->RSSetViewports(1, &viewport);
}
void Commands::setScissor(const Rect& v)
{
    if (!recording())
        return;
    const auto right = static_cast<i64>(v.x) + v.width, bottom = static_cast<i64>(v.y) + v.height;
    if (v.width < 0 || v.height < 0 || right > (std::numeric_limits<LONG>::max)() ||
        bottom > (std::numeric_limits<LONG>::max)()) {
        fail(E_INVALIDARG);
        return;
    }
    const D3D12_RECT rect{v.x, v.y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    context_.commands()->RSSetScissorRects(1, &rect);
}
void Commands::bindVertexBuffers(u32 first, std::span<const BufferHandle> buffers)
{
    copy(vertices_, first, buffers);
}
void Commands::bindIndexBuffer(BufferHandle buffer, IndexType type)
{
    index_ = buffer;
    indexType_ = type;
}
void Commands::bindUniforms(ShaderStage value, u32 slot, std::span<const std::byte> data)
{
    auto* bindings = stage(value);
    if (!bindings)
        return;
    if (slot >= bindings->uniforms.size()) {
        fail(E_INVALIDARG);
        return;
    }
    uniform(value == ShaderStage::Vertex ? 0u : 1u, slot, data, bindings->uniforms[slot]);
}

void Commands::uniform(u32 stage, u32 slot, std::span<const std::byte> data, D3D12_GPU_VIRTUAL_ADDRESS& address)
{
    auto& snapshot = uniformSnapshots_[stage][slot];
    if (snapshot.address && snapshot.bytes.size() == data.size() &&
        std::equal(data.begin(), data.end(), snapshot.bytes.begin())) {
        address = snapshot.address;
        return;
    }
    const HRESULT result = descriptors_.uniform(data, address);
    fail(result);
    if (SUCCEEDED(result)) {
        snapshot.bytes.assign(data.begin(), data.end());
        snapshot.address = address;
    }
}
void Commands::bindTextures(ShaderStage value, u32 first, std::span<const TextureBinding> textures)
{
    if (auto* bindings = stage(value))
        copy(bindings->textures, first, textures);
}
void Commands::bindStorageBuffers(ShaderStage value, u32 first, std::span<const BufferHandle> buffers)
{
    if (auto* bindings = stage(value))
        copy(bindings->buffers, first, buffers);
}

void Commands::applyGraphics(const NativePipeline& pipeline)
{
    auto* cmd = context_.commands();
    if (boundState_ != pipeline.state.Get()) {
        cmd->SetPipelineState(pipeline.state.Get());
        boundState_ = pipeline.state.Get();
    }
    if (boundGraphicsRoot_ != pipeline.root.Get()) {
        cmd->SetGraphicsRootSignature(pipeline.root.Get());
        boundGraphicsRoot_ = pipeline.root.Get();
    }
    if (boundTopology_ != pipeline.topology) {
        cmd->IASetPrimitiveTopology(pipeline.topology);
        boundTopology_ = pipeline.topology;
    }
}

bool Commands::prepareGraphics(bool indexed)
{
    if (!recording())
        return false;
    auto* pipeline = pipelines_.graphics(pipeline_);
    if (!renderPass_ || !pipeline || (indexed && !resources_.buffer(index_))) {
        fail(E_UNEXPECTED);
        return false;
    }
    auto* cmd = context_.commands();
    applyGraphics(*pipeline);
    for (u32 slot = 0; slot < pipeline->strides.size(); ++slot) {
        if (!pipeline->strides[slot])
            continue;
        auto* buffer = resources_.buffer(vertices_[slot]);
        if (!buffer) {
            fail(E_INVALIDARG);
            return false;
        }
        resources_.transition(*buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        const D3D12_VERTEX_BUFFER_VIEW view{buffer->native->GetGPUVirtualAddress(), buffer->desc.sizeBytes,
                                            pipeline->strides[slot]};
        auto& bound = boundVertices_[slot];
        if (bound.BufferLocation != view.BufferLocation || bound.SizeInBytes != view.SizeInBytes ||
            bound.StrideInBytes != view.StrideInBytes) {
            cmd->IASetVertexBuffers(slot, 1, &view);
            bound = view;
        }
    }
    if (indexed) {
        auto* buffer = resources_.buffer(index_);
        resources_.transition(*buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        const D3D12_INDEX_BUFFER_VIEW view{buffer->native->GetGPUVirtualAddress(), buffer->desc.sizeBytes,
                                           indexType_ == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        if (boundIndex_.BufferLocation != view.BufferLocation || boundIndex_.SizeInBytes != view.SizeInBytes ||
            boundIndex_.Format != view.Format) {
            cmd->IASetIndexBuffer(&view);
            boundIndex_ = view;
        }
    }
    fail(descriptors_.bind(pipeline->stages[0], graphics_[0], false));
    if (SUCCEEDED(failure_))
        fail(descriptors_.bind(pipeline->stages[1], graphics_[1], false));
    return SUCCEEDED(failure_);
}
void Commands::draw(u32 vertices, u32 instances, u32 first, u32 firstInstance)
{
    if (prepareGraphics(false))
        context_.commands()->DrawInstanced(vertices, instances, first, firstInstance);
}
void Commands::drawIndexed(u32 indices, u32 instances, u32 first, i32 offset, u32 firstInstance)
{
    if (!prepareGraphics(true))
        return;
    const u32 element = indexType_ == IndexType::U16 ? 2 : 4;
    const auto capacity = resources_.buffer(index_)->desc.sizeBytes / element;
    if (first > capacity || indices > capacity - first) {
        fail(E_INVALIDARG);
        return;
    }
    context_.commands()->DrawIndexedInstanced(indices, instances, first, offset, firstInstance);
}
void Commands::drawIndexedIndirect(BufferHandle handle, u32 offset, u32 count)
{
    if (!prepareGraphics(true))
        return;
    auto* buffer = resources_.buffer(handle);
    const auto bytes = static_cast<u64>(count) * sizeof(DrawIndexedIndirectCommand);
    if (!indexedIndirect_ || !buffer || offset % 4 || offset > buffer->desc.sizeBytes ||
        bytes > buffer->desc.sizeBytes - offset) {
        fail(E_INVALIDARG);
        return;
    }
    resources_.transition(*buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    context_.commands()->ExecuteIndirect(indexedIndirect_.Get(), count, buffer->native.Get(), offset, nullptr, 0);
}

void Commands::upload(BufferHandle buffer, std::span<const std::byte> data, u32 offset)
{
    endRenderPass();
    endComputePass();
    if (recording())
        fail(resources_.upload(buffer, data, offset));
}
void Commands::uploadTexture(TextureHandle texture, std::span<const std::byte> data, u32 mip)
{
    endRenderPass();
    endComputePass();
    if (recording())
        fail(resources_.uploadTexture(texture, data, mip));
}
void Commands::uploadTextureRegion(TextureHandle texture, u32 x, u32 y, u32 width, u32 height,
                                   std::span<const std::byte> data)
{
    endRenderPass();
    endComputePass();
    if (recording())
        fail(resources_.uploadTextureRegion(texture, x, y, width, height, data));
}

void Commands::beginComputePass(std::span<const BufferHandle> writes,
                                std::span<const ComputeTextureWrite> textureWrites)
{
    if (!recording())
        return;
    endRenderPass();
    endComputePass();
    compute_.writes = {};
    compute_.textureWrites = {};
    copy(compute_.writes, 0, writes);
    copy(compute_.textureWrites, 0, textureWrites);
    computePass_ = SUCCEEDED(failure_);
}
void Commands::endComputePass()
{
    if (computePass_ && context_.recording()) {
        // Includes same-state UAV dependencies, which transition barriers alone
        // do not order. A later pass may reuse a buffer without changing state.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        context_.commands()->ResourceBarrier(1, &barrier);
    }
    computePass_ = false;
}
void Commands::setComputePipeline(ComputePipelineHandle pipeline)
{
    computePipeline_ = pipeline;
}
void Commands::bindComputeStorageBuffers(u32 first, std::span<const BufferHandle> buffers)
{
    copy(compute_.buffers, first, buffers);
}
void Commands::bindComputeTextures(u32 first, std::span<const TextureBinding> textures)
{
    copy(compute_.textures, first, textures);
}
void Commands::bindComputeStorageTextures(u32 first, std::span<const TextureHandle> textures)
{
    copy(compute_.storageTextures, first, textures);
}
void Commands::bindComputeUniforms(u32 slot, std::span<const std::byte> data)
{
    if (slot >= compute_.uniforms.size()) {
        fail(E_INVALIDARG);
        return;
    }
    uniform(2, slot, data, compute_.uniforms[slot]);
}
void Commands::dispatch(u32 x, u32 y, u32 z)
{
    if (!recording())
        return;
    auto* pipeline = pipelines_.compute(computePipeline_);
    if (!computePass_ || !pipeline || x > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
        y > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
        z > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
        fail(E_INVALIDARG);
        return;
    }
    auto* cmd = context_.commands();
    if (boundState_ != pipeline->state.Get()) {
        cmd->SetPipelineState(pipeline->state.Get());
        boundState_ = pipeline->state.Get();
    }
    cmd->SetComputeRootSignature(pipeline->root.Get());
    fail(descriptors_.bind(pipeline->stages[0], compute_, true));
    if (SUCCEEDED(failure_)) {
        cmd->Dispatch(x, y, z);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &barrier);
    }
}

void Commands::pushDebugGroup(std::string_view name)
{
    if (!recording())
        return;
    if (name.size() > (std::numeric_limits<UINT>::max)()) {
        fail(E_INVALIDARG);
        return;
    }
    // D3D12 BeginEvent requires PIX's binary event encoding. A raw UTF-8
    // string is not that payload and must not be sent to a console driver.
    // Keep nesting validation; emitting markers requires a proper PIX adapter.
    ++debugDepth_;
}
void Commands::popDebugGroup()
{
    if (!recording())
        return;
    if (!debugDepth_) {
        fail(E_UNEXPECTED);
        return;
    }
    --debugDepth_;
}

HRESULT Commands::initializeBlit(std::span<const std::byte> vertex, std::span<const std::byte> fragment)
{
    if (blitVertex_.valid() || vertex.empty() || fragment.empty())
        return E_INVALIDARG;
    blitVertex_ = pipelines_.createShader({.stage = ShaderStage::Vertex, .format = ShaderFormat::Dxil, .code = vertex});
    blitFragment_ = pipelines_.createShader(
        {.stage = ShaderStage::Fragment, .format = ShaderFormat::Dxil, .code = fragment, .samplerCount = 1});
    blitSampler_ = descriptors_.createSampler({.addressU = AddressMode::ClampToEdge,
                                               .addressV = AddressMode::ClampToEdge,
                                               .addressW = AddressMode::ClampToEdge});
    return blitVertex_.valid() && blitFragment_.valid() && blitSampler_.valid() ? S_OK : E_FAIL;
}

HRESULT Commands::blit(TextureHandle source, u32 sourceMip, u32 sourceLayer, TextureHandle destination,
                       u32 destinationMip, u32 destinationLayer)
{
    auto* src = resources_.texture(source);
    auto* dst = resources_.texture(destination);
    if (!src || !dst || !dst->targets || src->depth || dst->depth ||
        !hasUsage(src->desc.usage, TextureUsage::Sampled) || !hasUsage(dst->desc.usage, TextureUsage::ColorTarget) ||
        sourceMip >= src->desc.mipLevels || destinationMip >= dst->desc.mipLevels || sourceLayer >= src->desc.layers ||
        destinationLayer >= dst->desc.layers ||
        (source == destination && sourceMip == destinationMip && sourceLayer == destinationLayer))
        return E_INVALIDARG;
    if (!blitVertex_.valid() || !blitFragment_.valid())
        return E_UNEXPECTED;
    const auto formatIndex = static_cast<std::size_t>(dst->desc.format);
    if (formatIndex >= blitPipelines_.size())
        return E_INVALIDARG;
    auto& handle = blitPipelines_[formatIndex];
    if (!handle.valid()) {
        const ColorTargetDesc color[] = {{.format = dst->desc.format}};
        handle = pipelines_.createGraphics({.vertexShader = blitVertex_,
                                            .fragmentShader = blitFragment_,
                                            .rasterizer = {.cullMode = CullMode::None},
                                            .colorTargets = color});
    }
    const auto* pipeline = pipelines_.graphics(handle);
    if (!pipeline)
        return E_FAIL;
    resources_.transition(*dst, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          destinationMip + destinationLayer * dst->desc.mipLevels);
    const auto target = dst->target(destinationLayer, destinationMip);
    auto* cmd = context_.commands();
    cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
    applyGraphics(*pipeline);
    const u32 width = (std::max)(1u, dst->desc.width >> destinationMip);
    const u32 height = (std::max)(1u, dst->desc.height >> destinationMip);
    setViewport({0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
    setScissor({0, 0, static_cast<i32>(width), static_cast<i32>(height)});
    Bindings bindings;
    bindings.textures[0] = {source, blitSampler_};
    bindings.textureViews[0] = {sourceMip, sourceLayer, true};
    const HRESULT result = descriptors_.bind(pipeline->stages[1], bindings, false);
    if (SUCCEEDED(result))
        cmd->DrawInstanced(3, 1, 0, 0);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    return result;
}

void Commands::blitTexture(TextureHandle source, TextureHandle destination, u32 layer)
{
    endRenderPass();
    endComputePass();
    if (recording())
        fail(blit(source, 0, 0, destination, 0, layer));
}
void Commands::generateMipmaps(TextureHandle texture)
{
    endRenderPass();
    endComputePass();
    if (!recording())
        return;
    const auto* value = resources_.texture(texture);
    if (!value) {
        fail(E_INVALIDARG);
        return;
    }
    const auto desc = value->desc;
    for (u32 layer = 0; layer < desc.layers && SUCCEEDED(failure_); ++layer)
        for (u32 mip = 1; mip < desc.mipLevels && SUCCEEDED(failure_); ++mip)
            fail(blit(texture, mip - 1, layer, texture, mip, layer));
}

} // namespace engine::rhi::d3d12
