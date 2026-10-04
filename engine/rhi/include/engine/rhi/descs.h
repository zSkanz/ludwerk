// Resource and pipeline descriptors.
//
// Every desc is a plain aggregate with defaults that describe the common case,
// so a call site names only what it actually cares about. That is also what
// makes `rhi_capture` able to serialise a desc field by field.
#pragma once

#include <span>
#include <string_view>

#include "engine/rhi/types.h"

namespace engine::rhi {

struct BufferDesc
{
    BufferUsage usage = BufferUsage::None;
    u32 sizeBytes = 0;
    // Shows up in RenderDoc and in the backend validation layer. Developer
    // text, never shown to a player, so it is a plain string (R3 does not
    // apply); empty is fine.
    std::string_view debugName{};
};

struct TextureDesc
{
    TextureFormat format = TextureFormat::Undefined;
    TextureUsage usage = TextureUsage::None;
    u32 width = 0;
    u32 height = 0;
    // More than one makes an ARRAY texture, sampled as `Texture2DArray`.
    u32 layers = 1;
    u32 mipLevels = 1;
    std::string_view debugName{};
};

struct SamplerDesc
{
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipmapMode mipmapMode = MipmapMode::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    std::string_view debugName{};
};

struct ShaderDesc
{
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::Unknown;
    std::span<const std::byte> code{};
    std::string_view entryPoint = "main";

    // Backends need the counts up front to build their descriptor layouts;
    // SDL_GPU rejects a shader whose declared counts do not match its bindings.
    // The shader compiler emits them into the shader manifest, so nothing hand
    // counts these.
    u32 samplerCount = 0;
    u32 uniformBufferCount = 0;
    // Storage buffers the stage reads (`StructuredBuffer`), after its
    // textures in the binding order (ADR 0116).
    u32 storageBufferCount = 0;

    std::string_view debugName{};
};

// **A compute pipeline** (ADR 0116): one shader, the counts its layout needs,
// and the thread-group size it was compiled with -- all from reflection, as a
// graphics shader's counts are.
struct ComputePipelineDesc
{
    ShaderFormat format = ShaderFormat::Unknown;
    std::span<const std::byte> code{};
    std::string_view entryPoint = "ComputeMain";
    u32 samplerCount = 0;
    u32 readonlyStorageBufferCount = 0;
    u32 readwriteStorageBufferCount = 0;
    // The textures it writes (ADR 0164), bound when its pass begins.
    u32 readwriteStorageTextureCount = 0;
    u32 uniformBufferCount = 0;
    u32 threadCountX = 1;
    u32 threadCountY = 1;
    u32 threadCountZ = 1;
    std::string_view debugName{};
};

// **A texture a compute pass writes** (ADR 0164): one mip of it.
struct ComputeTextureWrite
{
    TextureHandle texture{};
    u32 mipLevel = 0;
};

// One indexed draw read from a buffer by `drawIndexedIndirect`: five words,
// laid out as every backend reads them, so a compute shader writes it as a
// `uint` array.
struct DrawIndexedIndirectCommand
{
    u32 indexCount = 0;
    u32 instanceCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 firstInstance = 0;
};
static_assert(sizeof(DrawIndexedIndirectCommand) == 20, "an indirect draw is five 32-bit words");

struct VertexAttribute
{
    // Matches the shader input location, not the field order.
    u32 location = 0;
    // Which bound vertex buffer this attribute reads from.
    u32 bufferSlot = 0;
    VertexFormat format = VertexFormat::Float3;
    u32 offsetBytes = 0;
};

struct VertexBufferLayout
{
    u32 slot = 0;
    u32 strideBytes = 0;
    // When true this stream advances once per INSTANCE rather than once per
    // vertex, which is what lets one call draw a run of objects that share a
    // mesh and a material. False is the default and is what every stream that
    // existed before instanced rendering was (ADR 0043).
    bool perInstance = false;
};

struct DepthStencilState
{
    bool depthTest = false;
    bool depthWrite = false;
    CompareOp depthCompare = CompareOp::LessOrEqual;
};

struct BlendState
{
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::SrcAlpha;
    BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::OneMinusSrcAlpha;
    BlendOp alphaOp = BlendOp::Add;
};

struct ColorTargetDesc
{
    TextureFormat format = TextureFormat::Undefined;
    BlendState blend{};
};

struct RasterizerState
{
    FillMode fillMode = FillMode::Solid;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    // **Clip what lies outside the depth range instead of clamping it.** Off,
    // a triangle nearer than the near plane is flattened onto it -- which a
    // shadow map wants, so a caster behind the light still casts. On, it is
    // cut away -- which an oblique near plane needs (ADR 0107), or a mirror's
    // camera would see the wall behind the glass pressed flat over the room.
    bool depthClip = false;
};

struct GraphicsPipelineDesc
{
    ShaderHandle vertexShader{};
    ShaderHandle fragmentShader{};

    std::span<const VertexBufferLayout> vertexBuffers{};
    std::span<const VertexAttribute> vertexAttributes{};

    PrimitiveType primitive = PrimitiveType::TriangleList;
    RasterizerState rasterizer{};
    DepthStencilState depthStencil{};

    std::span<const ColorTargetDesc> colorTargets{};
    // Undefined means the pipeline renders without a depth attachment.
    TextureFormat depthStencilFormat = TextureFormat::Undefined;

    std::string_view debugName{};
};

struct ColorAttachment
{
    TextureHandle texture{};
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    ColorRgba clearColor{};
    // **Which layer of an array is drawn into** (terrain audit T3): a
    // terrain's layer arrays are drawn one layer at a time from their
    // materials' maps, each read at the mip that fits. Mip 0 always.
    u32 layer = 0;
};

struct DepthStencilAttachment
{
    TextureHandle texture{};
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::DontCare;
    f32 clearDepth = 1.0f;
};

struct RenderPassDesc
{
    std::span<const ColorAttachment> colorAttachments{};
    // An invalid texture means no depth attachment this pass.
    DepthStencilAttachment depthStencil{};
    std::string_view debugName{};
};

// A texture is always sampled through a sampler; binding them as a pair is
// what every backend's descriptor model expects, and it makes forgetting one
// impossible rather than a black screen.
struct TextureBinding
{
    TextureHandle texture{};
    SamplerHandle sampler{};
};

} // namespace engine::rhi
