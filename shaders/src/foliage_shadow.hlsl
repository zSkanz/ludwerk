// Foliage into the shadow cascades (ADR 0116): the same placement as
// `foliage.hlsl`, and only within the shadow distance -- a vertex of an
// instance past it is collapsed so the instance draws nothing.
//
// **And with the holes the drawn instance has** (the terrain audit's T3): a
// card's image cuts it where its alpha is under the material's cutoff, and an
// instance fading out at its draw distance is dithered away -- the forward
// pass does both, and a shadow that did neither was a solid rectangle for
// every card, and stayed whole while its instance faded.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"
#include "engine_foliage.hlsli"

// x: the material's alpha, y: its cutoff (0 for a material that is not
// `Mask`, which cuts nothing), z: 1 when it has a base-colour image.
cbuffer GpuFoliageShadowCut : register(b0, space3)
{
    float4 Cut;
};

Texture2D BaseColorTexture : register(t0, space2);
SamplerState BaseColorSampler : register(s0, space2);

// The position and the UV, at locations 0 and 1 -- the renderer binds the
// UV's bytes there. Inputs are numbered in order, so the UV cannot keep the
// forward pass's third slot with the two before it unread.
struct VertexInput
{
    float3 Position : TEXCOORD0;
    float2 Uv : TEXCOORD1;
    uint Instance : SV_InstanceID;
};

struct Interpolants
{
    float2 Uv : TEXCOORD0;
    nointerpolation float Fade : TEXCOORD1;
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const FoliageVisible visible = foliageInstance(input.Instance);
    output.Uv = input.Uv;
    output.Fade = visible.Fade;
    if (length(visible.Position) > FoliageMesh.z) {
        output.Position = float4(0.0f, 0.0f, -1.0f, 1.0f);
        return output;
    }
    const float3 position = foliagePosition(visible, input.Position, foliageBasis(visible));
    output.Position = mul(LightViewProjection, float4(position, 1.0f));
    return output;
}

// `foliage.hlsl`'s 4x4 ordered dither, in the shadow map's own texels.
float foliageShadowDither(float2 pixel)
{
    const uint2 p = uint2(pixel) & 3u;
    const uint index = p.x + p.y * 4u;
    const float table[16] = {0.0f,   8.0f,  2.0f,  10.0f, 12.0f, 4.0f,  14.0f, 6.0f,
                             3.0f,   11.0f, 1.0f,  9.0f,  15.0f, 7.0f,  13.0f, 5.0f};
    return (table[index] + 0.5f) / 16.0f;
}

void FragmentMain(Interpolants input)
{
    if (input.Fade < foliageShadowDither(input.Position.xy))
        discard;
    const float image = BaseColorTexture.Sample(BaseColorSampler, input.Uv).a;
    clip(Cut.x * lerp(1.0f, image, Cut.z) - Cut.y);
}
