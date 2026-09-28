// The forward pass for foliage (ADR 0116): an instance comes from the list the
// GPU cull wrote, not from a vertex stream, and the fragment stage is the
// forward one every mesh shares -- after a dithered fade, so a field thins
// away at its draw distance instead of ending at a line.

#define ENG_UNIFORMS_OBJECT
#define ENG_UNIFORMS_FRAME
#define ENG_UNIFORMS_MATERIAL
#include "engine_forward.hlsli"
#include "engine_foliage.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    uint Instance : SV_InstanceID;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const FoliageVisible visible = foliageInstance(input.Instance);
    const float3x3 basis = foliageBasis(visible);
    const float3 position = foliagePosition(visible, input.Position, basis);

    output.ShadingPosition = position;
    output.Position = mul(ViewProjection, float4(position, 1.0f));
    output.ViewDepth = output.Position.w;
    // A rotation and a uniform scale, so the normal rides on the rotation.
    output.Normal = mul(basis, input.Normal);
    output.Tangent = float4(mul(basis, input.Tangent.xyz), input.Tangent.w);
    output.Uv = input.Uv;
    output.UvMetres = input.Uv * visible.Scale;
    // The fade, carried down for the fragment stage's dither.
    output.InstanceAlpha = visible.Fade;
    return output;
}

// A 4x4 ordered-dither threshold: which pixels of a half-faded instance go.
float foliageDither(float2 pixel)
{
    const uint2 p = uint2(pixel) & 3u;
    const uint index = p.x + p.y * 4u;
    const float table[16] = {0.0f,   8.0f,  2.0f,  10.0f, 12.0f, 4.0f,  14.0f, 6.0f,
                             3.0f,   11.0f, 1.0f,  9.0f,  15.0f, 7.0f,  13.0f, 5.0f};
    return (table[index] + 0.5f) / 16.0f;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    if (input.InstanceAlpha < foliageDither(input.Position.xy))
        discard;
    input.InstanceAlpha = 1.0f;
    return shadeForward(input);
}
