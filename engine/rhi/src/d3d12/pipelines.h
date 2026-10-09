#pragma once

#include <array>
#include <vector>

#include "context.h"
#include "engine/rhi/descs.h"

namespace engine::rhi::d3d12 {

struct StageLayout
{
    static constexpr u32 Absent = ~u32{0};
    u32 reads = Absent, samplers = Absent, writes = Absent;
    u32 readCount = 0, samplerCount = 0, writeCount = 0;
    u32 textureReads = 0, bufferReads = 0, textureWrites = 0, bufferWrites = 0;
    std::vector<u32> uniforms;
};

struct NativePipeline
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    std::array<StageLayout, 2> stages;
    std::array<u32, 16> strides{};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

// Reflective resource counts remain the same contract as the shipping shader
// compiler. Native root layouts use the existing stage/register-space mapping.
class Pipelines final
{
public:
    explicit Pipelines(Context& context) noexcept : context_(context) {}
    ~Pipelines();
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;
    ShaderHandle createShader(const ShaderDesc& desc);
    PipelineHandle createGraphics(const GraphicsPipelineDesc& desc);
    ComputePipelineHandle createCompute(const ComputePipelineDesc& desc);
    NativePipeline* graphics(PipelineHandle handle) noexcept;
    NativePipeline* compute(ComputePipelineHandle handle) noexcept;
    void destroy(ShaderHandle handle);
    void destroy(PipelineHandle handle);
    void destroy(ComputePipelineHandle handle);
    HRESULT collect();

private:
    struct Shader
    {
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<std::byte> code;
        u32 samplers = 0, uniforms = 0, storage = 0;
    };
    HRESULT root(NativePipeline& pipeline, std::span<const Shader> shaders, const ComputePipelineDesc* compute);
    void retire(NativePipeline& pipeline);
    Context& context_;
    std::vector<Shader> shaders_;
    std::vector<NativePipeline> graphics_, compute_;
    struct Retired
    {
        Microsoft::WRL::ComPtr<IUnknown> native;
        std::uint64_t fence = 0;
    };
    std::vector<Retired> retired_;
};

} // namespace engine::rhi::d3d12
