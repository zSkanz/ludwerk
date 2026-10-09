#pragma once

#include <algorithm>

#include "descriptors.h"

namespace engine::rhi::d3d12 {

// Private until every operation is implemented and the device adapter can
// surface failures. The owning frame MUST check status() before submission.
class Commands final : public ICmdList
{
public:
    Commands(Context& context, Resources& resources, Pipelines& pipelines, Descriptors& descriptors) noexcept;
    HRESULT initialize();
    // Internal shaders come from the normal host-compiled pack; no compiler or
    // filesystem is needed by the backend or the console runtime.
    HRESULT initializeBlit(std::span<const std::byte> vertex, std::span<const std::byte> fragment);
    void reset() noexcept;
    [[nodiscard]] HRESULT status() const noexcept { return failure_; }
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setPipeline(PipelineHandle pipeline) override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect& scissor) override;
    void bindVertexBuffers(u32 firstSlot, std::span<const BufferHandle> buffers) override;
    void bindIndexBuffer(BufferHandle buffer, IndexType type) override;
    void bindUniforms(ShaderStage stage, u32 slot, std::span<const std::byte> data) override;
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
    void bindComputeStorageTextures(u32 firstSlot, std::span<const TextureHandle> textures) override;
    void bindComputeUniforms(u32 slot, std::span<const std::byte> data) override;
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
    void pushDebugGroup(std::string_view name) override;
    void popDebugGroup() override;

private:
    void fail(HRESULT result) noexcept;
    bool recording();
    bool prepareGraphics(bool indexed);
    void applyGraphics(const NativePipeline& pipeline);
    void uniform(u32 stage, u32 slot, std::span<const std::byte> data, D3D12_GPU_VIRTUAL_ADDRESS& address);
    HRESULT blit(TextureHandle source, u32 sourceMip, u32 sourceLayer, TextureHandle destination, u32 destinationMip,
                 u32 destinationLayer);
    Bindings* stage(ShaderStage value);
    template <typename T, std::size_t N>
    void copy(std::array<T, N>& target, u32 first, std::span<const T> values)
    {
        if (first > N || values.size() > N - first) {
            fail(E_INVALIDARG);
            return;
        }
        std::copy(values.begin(), values.end(), target.begin() + first);
    }
    Context& context_;
    Resources& resources_;
    Pipelines& pipelines_;
    Descriptors& descriptors_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> indexedIndirect_;
    ShaderHandle blitVertex_{}, blitFragment_{};
    SamplerHandle blitSampler_{};
    std::array<PipelineHandle, static_cast<std::size_t>(TextureFormat::R32Uint) + 1> blitPipelines_{};
    std::array<Bindings, 2> graphics_{};
    Bindings compute_{};
    std::array<BufferHandle, 16> vertices_{};
    BufferHandle index_{};
    IndexType indexType_ = IndexType::U16;
    PipelineHandle pipeline_{};
    ComputePipelineHandle computePipeline_{};
    ID3D12PipelineState* boundState_ = nullptr;
    ID3D12RootSignature* boundGraphicsRoot_ = nullptr;
    D3D_PRIMITIVE_TOPOLOGY boundTopology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<D3D12_VERTEX_BUFFER_VIEW, 16> boundVertices_{};
    D3D12_INDEX_BUFFER_VIEW boundIndex_{};
    struct UniformSnapshot
    {
        std::vector<std::byte> bytes;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    };
    std::array<std::array<UniformSnapshot, 16>, 3> uniformSnapshots_{};
    HRESULT failure_ = S_OK;
    u32 debugDepth_ = 0;
    bool renderPass_ = false, computePass_ = false;
};

} // namespace engine::rhi::d3d12
