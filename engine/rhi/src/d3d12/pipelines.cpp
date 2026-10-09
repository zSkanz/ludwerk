#include "pipelines.h"

#include <algorithm>
#include <limits>

#include "engine/core/profile.h"
#include "resources.h"

namespace engine::rhi::d3d12 {
namespace {
DXGI_FORMAT vertexFormat(VertexFormat value) noexcept
{
    switch (value) {
    case VertexFormat::Float1:
        return DXGI_FORMAT_R32_FLOAT;
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::Ubyte4Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}
D3D12_BLEND blend(BlendFactor value) noexcept
{
    switch (value) {
    case BlendFactor::Zero:
        return D3D12_BLEND_ZERO;
    case BlendFactor::One:
        return D3D12_BLEND_ONE;
    case BlendFactor::SrcAlpha:
        return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstAlpha:
        return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::SrcColor:
        return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:
        return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::DstColor:
        return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::OneMinusDstColor:
        return D3D12_BLEND_INV_DEST_COLOR;
    }
    return D3D12_BLEND_ZERO;
}
D3D_PRIMITIVE_TOPOLOGY topology(PrimitiveType value) noexcept
{
    switch (value) {
    case PrimitiveType::TriangleList:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveType::TriangleStrip:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveType::LineList:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveType::LineStrip:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveType::PointList:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}
} // namespace

Pipelines::~Pipelines()
{
    (void)context_.waitIdle();
}

ShaderHandle Pipelines::createShader(const ShaderDesc& desc)
{
    if (desc.format != ShaderFormat::Dxil || desc.code.empty() || desc.samplerCount > 16 ||
        desc.uniformBufferCount > 16 || desc.storageBufferCount > 16 || FAILED(context_.status()))
        return {};
    shaders_.push_back({desc.stage,
                        {desc.code.begin(), desc.code.end()},
                        desc.samplerCount,
                        desc.uniformBufferCount,
                        desc.storageBufferCount});
    return {static_cast<u32>(shaders_.size())};
}

HRESULT Pipelines::root(NativePipeline& pipeline, std::span<const Shader> shaders, const ComputePipelineDesc* compute)
{
    std::vector<D3D12_ROOT_PARAMETER> parameters;
    // Each table's range must remain stable while later parameters are appended.
    std::array<D3D12_DESCRIPTOR_RANGE, 6> ranges{};
    u32 rangeCount = 0;
    const auto table = [&](StageLayout& stage, u32& slot, D3D12_DESCRIPTOR_RANGE_TYPE type, u32 count, u32 space,
                           D3D12_SHADER_VISIBILITY visibility) {
        if (!count)
            return;
        auto& range = ranges[rangeCount++];
        range.RangeType = type;
        range.NumDescriptors = count;
        range.RegisterSpace = space;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = visibility;
        slot = static_cast<u32>(parameters.size());
        parameters.push_back(parameter);
        (void)stage;
    };
    const u32 count = compute ? 1u : static_cast<u32>(shaders.size());
    for (u32 index = 0; index < count; ++index) {
        auto& layout = pipeline.stages[index];
        const auto visibility = compute      ? D3D12_SHADER_VISIBILITY_ALL
                                : index == 0 ? D3D12_SHADER_VISIBILITY_VERTEX
                                             : D3D12_SHADER_VISIBILITY_PIXEL;
        const u32 space = compute ? 0u : index * 2;
        layout.samplerCount = compute ? compute->samplerCount : shaders[index].samplers;
        layout.readCount =
            layout.samplerCount + (compute ? compute->readonlyStorageTextureCount + compute->readonlyStorageBufferCount
                                           : shaders[index].storage);
        layout.writeCount = compute ? compute->readwriteStorageTextureCount + compute->readwriteStorageBufferCount : 0;
        layout.textureReads = compute ? compute->readonlyStorageTextureCount : 0;
        layout.bufferReads = compute ? compute->readonlyStorageBufferCount : shaders[index].storage;
        layout.textureWrites = compute ? compute->readwriteStorageTextureCount : 0;
        layout.bufferWrites = compute ? compute->readwriteStorageBufferCount : 0;
        table(layout, layout.reads, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, layout.readCount, space, visibility);
        table(layout, layout.samplers, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, layout.samplerCount, space, visibility);
        table(layout, layout.writes, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, layout.writeCount, 1, visibility);
        const u32 uniforms = compute ? compute->uniformBufferCount : shaders[index].uniforms;
        for (u32 uniform = 0; uniform < uniforms; ++uniform) {
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor = {uniform, compute ? 2u : space + 1};
            parameter.ShaderVisibility = visibility;
            layout.uniforms.push_back(static_cast<u32>(parameters.size()));
            parameters.push_back(parameter);
        }
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.Flags =
        compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> bytes, diagnostic;
    HRESULT result = context_.serializeRootSignature(desc, &bytes, &diagnostic);
    if (FAILED(result))
        return result;
    return context_.device()->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                                  IID_PPV_ARGS(&pipeline.root));
}

PipelineHandle Pipelines::createGraphics(const GraphicsPipelineDesc& desc)
{
    ENG_PROFILE_SCOPE("rhi.graphics_pipeline");
    if (!context_.device() || FAILED(context_.status()) || desc.colorTargets.size() > 8 || !desc.vertexShader.id ||
        desc.vertexShader.id > shaders_.size() || !desc.fragmentShader.id || desc.fragmentShader.id > shaders_.size())
        return {};
    const auto& vertex = shaders_[desc.vertexShader.id - 1];
    const auto& fragment = shaders_[desc.fragmentShader.id - 1];
    if (vertex.stage != ShaderStage::Vertex || fragment.stage != ShaderStage::Fragment || vertex.code.empty() ||
        fragment.code.empty())
        return {};
    NativePipeline pipeline;
    const std::array<Shader, 2> shaders{vertex, fragment};
    if (FAILED(root(pipeline, shaders, nullptr)))
        return {};
    std::array<bool, 16> perInstance{};
    for (const auto& layout : desc.vertexBuffers) {
        if (layout.slot >= pipeline.strides.size() || !layout.strideBytes)
            return {};
        pipeline.strides[layout.slot] = layout.strideBytes;
        perInstance[layout.slot] = layout.perInstance;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> attributes;
    for (const auto& attribute : desc.vertexAttributes) {
        if (attribute.bufferSlot >= pipeline.strides.size() || pipeline.strides[attribute.bufferSlot] == 0)
            return {};
        const bool instance = perInstance[attribute.bufferSlot];
        attributes.push_back(
            {"TEXCOORD", attribute.location, vertexFormat(attribute.format), attribute.bufferSlot,
             attribute.offsetBytes,
             instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
             instance ? 1u : 0u});
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = pipeline.root.Get();
    native.VS = {vertex.code.data(), vertex.code.size()};
    native.PS = {fragment.code.data(), fragment.code.size()};
    native.BlendState.IndependentBlendEnable = TRUE;
    native.NumRenderTargets = static_cast<UINT>(desc.colorTargets.size());
    for (std::size_t index = 0; index < desc.colorTargets.size(); ++index) {
        native.RTVFormats[index] = format(desc.colorTargets[index].format);
        const auto& source = desc.colorTargets[index].blend;
        auto& target = native.BlendState.RenderTarget[index];
        target.BlendEnable = source.enabled;
        target.SrcBlend = blend(source.srcColor);
        target.DestBlend = blend(source.dstColor);
        target.BlendOp = static_cast<D3D12_BLEND_OP>(static_cast<unsigned>(source.colorOp) + 1);
        target.SrcBlendAlpha = blend(source.srcAlpha);
        target.DestBlendAlpha = blend(source.dstAlpha);
        target.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(static_cast<unsigned>(source.alphaOp) + 1);
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    native.SampleMask = (std::numeric_limits<UINT>::max)();
    native.RasterizerState.FillMode =
        desc.rasterizer.fillMode == FillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    native.RasterizerState.CullMode = desc.rasterizer.cullMode == CullMode::None    ? D3D12_CULL_MODE_NONE
                                      : desc.rasterizer.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                                    : D3D12_CULL_MODE_BACK;
    native.RasterizerState.FrontCounterClockwise = desc.rasterizer.frontFace == FrontFace::CounterClockwise;
    native.RasterizerState.DepthClipEnable = desc.rasterizer.depthClip;
    native.DepthStencilState.DepthEnable = desc.depthStencil.depthTest;
    native.DepthStencilState.DepthWriteMask =
        desc.depthStencil.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthStencilState.DepthFunc =
        static_cast<D3D12_COMPARISON_FUNC>(static_cast<unsigned>(desc.depthStencil.depthCompare) + 1);
    native.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    native.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    native.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
    native.DepthStencilState.BackFace = native.DepthStencilState.FrontFace;
    native.DSVFormat = format(desc.depthStencilFormat);
    native.InputLayout = {attributes.data(), static_cast<UINT>(attributes.size())};
    pipeline.topology = topology(desc.primitive);
    native.PrimitiveTopologyType =
        desc.primitive == PrimitiveType::PointList ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
        : desc.primitive == PrimitiveType::LineList || desc.primitive == PrimitiveType::LineStrip
            ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    native.SampleDesc.Count = 1;
    if (FAILED(context_.device()->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&pipeline.state))))
        return {};
    graphics_.push_back(std::move(pipeline));
    return {static_cast<u32>(graphics_.size())};
}

ComputePipelineHandle Pipelines::createCompute(const ComputePipelineDesc& desc)
{
    ENG_PROFILE_SCOPE("rhi.compute_pipeline");
    if (!context_.device() || FAILED(context_.status()) || desc.format != ShaderFormat::Dxil || desc.code.empty() ||
        desc.samplerCount > 16 || desc.uniformBufferCount > 16 || desc.readonlyStorageTextureCount > 16 ||
        desc.readonlyStorageBufferCount > 16 || desc.readwriteStorageBufferCount > 16 ||
        desc.readwriteStorageTextureCount > 16)
        return {};
    NativePipeline pipeline;
    if (FAILED(root(pipeline, {}, &desc)))
        return {};
    D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
    native.pRootSignature = pipeline.root.Get();
    native.CS = {desc.code.data(), desc.code.size()};
    if (FAILED(context_.device()->CreateComputePipelineState(&native, IID_PPV_ARGS(&pipeline.state))))
        return {};
    compute_.push_back(std::move(pipeline));
    return {static_cast<u32>(compute_.size())};
}

NativePipeline* Pipelines::graphics(PipelineHandle handle) noexcept
{
    return handle.id && handle.id <= graphics_.size() && graphics_[handle.id - 1].state ? &graphics_[handle.id - 1]
                                                                                        : nullptr;
}
NativePipeline* Pipelines::compute(ComputePipelineHandle handle) noexcept
{
    return handle.id && handle.id <= compute_.size() && compute_[handle.id - 1].state ? &compute_[handle.id - 1]
                                                                                      : nullptr;
}
void Pipelines::destroy(ShaderHandle handle)
{
    if (handle.id && handle.id <= shaders_.size())
        shaders_[handle.id - 1].code.clear();
}
void Pipelines::retire(NativePipeline& pipeline)
{
    const auto fence = context_.retirementValue();
    retired_.push_back({pipeline.root, fence});
    retired_.push_back({pipeline.state, fence});
    pipeline.root.Reset();
    pipeline.state.Reset();
}
void Pipelines::destroy(PipelineHandle handle)
{
    if (auto* value = graphics(handle))
        retire(*value);
}
void Pipelines::destroy(ComputePipelineHandle handle)
{
    if (auto* value = compute(handle))
        retire(*value);
}
HRESULT Pipelines::collect()
{
    if (context_.recording())
        return E_UNEXPECTED;
    std::uint64_t completed = 0;
    const HRESULT result = context_.completed(completed);
    if (SUCCEEDED(result))
        std::erase_if(retired_, [completed](const auto& value) { return value.fence <= completed; });
    return result;
}
} // namespace engine::rhi::d3d12
