// **The shapes drawn on parts** (ADR 0251): boxes, spheres, cylinders, cones,
// lines, images and the outlines of a selection, each an instance of one of
// the renderer's unit meshes.
//
// **Drawn as the display shows them**, as the world's interface is (ADR 0250
// §7): the colour as it was set, not lit, not exposed and not through the
// curve, premultiplied for a blend in the display's linear light -- into a
// headset eye's image where the tonemap is folded into its pass, into the
// interface's own layer otherwise.

#include "engine_fullscreen.hlsli"

cbuffer AdornmentView : register(b0, space1)
{
    column_major float4x4 ViewProjection;
};

// An image's picture; a white pixel for the rest.
Texture2D AdornmentTexture : register(t0, space2);
SamplerState AdornmentSampler : register(s0, space2);

struct VertexInput
{
    // The unit mesh's corner, per vertex.
    float3 Position : TEXCOORD0;
    // Per instance: the model matrix's columns, the colour as it was set
    // and its opacity, a disc's inside, and how it is drawn.
    float4 Model0 : TEXCOORD1;
    float4 Model1 : TEXCOORD2;
    float4 Model2 : TEXCOORD3;
    float4 Model3 : TEXCOORD4;
    float4 Color : TEXCOORD5;
    float4 Inner : TEXCOORD6;
    float4 Params : TEXCOORD7;
};

struct Interpolants
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    nointerpolation float4 Color : TEXCOORD1;
    nointerpolation float4 Inner : TEXCOORD2;
    nointerpolation float4 Params : TEXCOORD3;
};

Interpolants VertexMain(VertexInput input)
{
    const float4 world = input.Model0 * input.Position.x + input.Model1 * input.Position.y +
                         input.Model2 * input.Position.z + input.Model3;
    Interpolants output;
    output.Position = mul(ViewProjection, float4(world.xyz, 1.0f));
    output.Local = input.Position.xy;
    output.Color = input.Color;
    output.Inner = input.Inner;
    output.Params = input.Params;
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    float4 color = float4(decodeSrgb(input.Color.rgb), input.Color.a);
    const uint mode = uint(input.Params.x + 0.5f);
    if (mode == 1u)
    {
        // An image: the square's corner is its picture's, v down.
        const float4 picture =
            AdornmentTexture.Sample(AdornmentSampler, float2(input.Local.x + 0.5f, 0.5f - input.Local.y));
        color *= picture;
    }
    else if (mode == 2u)
    {
        // A disc, its rim the colour and its inside the inner one.
        const float radius = length(input.Local) * 2.0f;
        if (radius > 1.0f)
            discard;
        if (radius < 1.0f - input.Params.y)
            color = float4(decodeSrgb(input.Inner.rgb), input.Inner.a);
    }
    clip(color.a - 1.0f / 255.0f);
    return float4(color.rgb * color.a, color.a);
}
