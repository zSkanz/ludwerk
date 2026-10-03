// The depth-only pass for a run of skinned objects that share a mesh (H2): the
// shadow cascades and the depth prepass, each member posed by its own palette.
//
// `shadow_skinned.hlsl`'s blend with `shadow_instanced.hlsl`'s per-instance
// model, the palette read from the frame's storage buffer at the place the
// instance names. Position, the skin stream and the instance's model and
// palette start, and nothing else -- the alpha row is stepped over by the
// stride, as `shadow_instanced.hlsl` steps over it.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"
#include "engine_skin_palettes.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    // `float4`, not `uint4`: see `pbr_skinned.hlsl` and D042.
    float4 Joints : TEXCOORD1;
    float4 Weights : TEXCOORD2;
    float4 ModelColumn0 : TEXCOORD3;
    float4 ModelColumn1 : TEXCOORD4;
    float4 ModelColumn2 : TEXCOORD5;
    float4 ModelColumn3 : TEXCOORD6;
    float4 InstancePalette : TEXCOORD7;
};

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const float4x4 model =
        transpose(float4x4(input.ModelColumn0, input.ModelColumn1, input.ModelColumn2, input.ModelColumn3));
    const float4 posed = mul(paletteSkin(uint(input.InstancePalette.x), uint4(input.Joints), input.Weights),
                             float4(input.Position, 1.0f));
    output.Position = mul(LightViewProjection, mul(model, posed));
    return output;
}

void FragmentMain()
{
}
