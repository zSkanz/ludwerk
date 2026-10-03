// The forward pass for a RUN of skinned objects that share a mesh (H2): one
// draw for a crowd, each member posed by its own palette.
//
// `pbr_skinned.hlsl`'s blend and `pbr_instanced.hlsl`'s per-instance stream,
// together. What neither could carry is the palette: a uniform block holds one
// skeleton and is scoped to one draw, so every animated character was a draw of
// its own in every pass -- 1,800 draws for five hundred enemies. Here the
// frame's palettes are ONE storage buffer, and each instance says where its
// own begins.
//
// Slot 0 is `asset::Vertex`, slot 1 the skin stream, slot 2
// `render::GpuSkinnedInstance` (96 bytes): the model matrix as four columns,
// the alpha and tint, and the first joint of the instance's palette.
//
// Register spaces are fixed per stage by SDL_GPU (SDL_gpu.h:2699-2730): a
// vertex stage's storage buffers are `t` registers in space 0.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#define ENG_UNIFORMS_MATERIAL
#define ENG_INSTANCE_TINT
#include "engine_forward.hlsli"
#include "engine_skin_palettes.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    // `float4`, not `uint4`: see `pbr_skinned.hlsl` and D042.
    float4 Joints : TEXCOORD4;
    float4 Weights : TEXCOORD5;
    float4 ModelColumn0 : TEXCOORD6;
    float4 ModelColumn1 : TEXCOORD7;
    float4 ModelColumn2 : TEXCOORD8;
    float4 ModelColumn3 : TEXCOORD9;
    float4 InstanceAlphaTint : TEXCOORD10;
    // x: where this instance's palette starts in `SkinPalettes`, as a float
    // because the vertex formats have no integer one (ADR 0037) -- exact up to
    // sixteen million joints.
    float4 InstancePalette : TEXCOORD11;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;

    // Transposed, as `pbr_instanced.hlsl` says: the attributes are the
    // matrix's columns and HLSL's constructor fills rows (D043).
    const float4x4 model =
        transpose(float4x4(input.ModelColumn0, input.ModelColumn1, input.ModelColumn2, input.ModelColumn3));
    const float4x4 skin = paletteSkin(uint(input.InstancePalette.x), uint4(input.Joints), input.Weights);
    const float4 posed = mul(skin, float4(input.Position, 1.0f));
    const float4 shadingPosition = mul(model, posed);
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;

    // The model's cofactor block from its columns, as the instanced pass does,
    // over the skin's rotation, as the skinned pass does.
    const float3 a = input.ModelColumn0.xyz;
    const float3 b = input.ModelColumn1.xyz;
    const float3 c = input.ModelColumn2.xyz;
    const float3x3 normalMatrix = float3x3(cross(b, c), cross(c, a), cross(a, b));
    output.Normal = mul(normalMatrix, mul((float3x3)skin, input.Normal));
    output.Tangent = float4(mul((float3x3)model, mul((float3x3)skin, input.Tangent.xyz)), input.Tangent.w);
    output.Uv = input.Uv;
    // A skinned mesh is a `MeshPart`'s, which keeps its file's UVs.
    output.UvMetres = input.Uv;
    output.InstanceAlpha = input.InstanceAlphaTint.x;
    output.InstanceTint = input.InstanceAlphaTint.yzw;
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    return shadeForward(input);
}
