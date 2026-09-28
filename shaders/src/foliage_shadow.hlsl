// Foliage into the shadow cascades (ADR 0116): the same placement as
// `foliage.hlsl`, and only within the shadow distance -- a vertex of an
// instance past it is collapsed so the instance draws nothing.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"
#include "engine_foliage.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    uint Instance : SV_InstanceID;
};

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const FoliageVisible visible = foliageInstance(input.Instance);
    if (length(visible.Position) > FoliageMesh.z) {
        output.Position = float4(0.0f, 0.0f, -1.0f, 1.0f);
        return output;
    }
    const float3 position = foliagePosition(visible, input.Position, foliageBasis(visible));
    output.Position = mul(LightViewProjection, float4(position, 1.0f));
    return output;
}

void FragmentMain()
{
}
