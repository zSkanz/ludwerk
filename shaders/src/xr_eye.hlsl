// **An eye's picture, into the headset's image** (ADR 0244). Each eye is drawn
// as a view is -- into a picture whose bytes are already display colour, as a
// window's are -- and this is the copy into the runtime's swapchain. A runtime
// reads an sRGB swapchain's values as linear light and encodes them itself, so
// into one of those the picture is decoded first and comes out as it went in;
// into a plain one it is copied as it is.

#include "engine_fullscreen.hlsli"

cbuffer GpuXrEyeUniforms : register(b0, space3)
{
    // x above a half: the target is an sRGB image, so the picture is decoded.
    float4 XrEye;
};

Texture2D EyeTexture : register(t0, space2);
SamplerState EyeSampler : register(s0, space2);

struct Interpolants
{
    float2 Uv : TEXCOORD0;
    float4 Position : SV_Position;
};

Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    Interpolants output;
    fullscreenTriangle(vertexId, 0.0f, output.Position, output.Uv);
    return output;
}

float3 decodeSrgb(float3 encoded)
{
    const float3 low = encoded / 12.92f;
    const float3 high = pow(max((encoded + 0.055f) / 1.055f, 0.0f), 2.4f);
    return lerp(high, low, step(encoded, 0.04045f));
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float4 picture = EyeTexture.SampleLevel(EyeSampler, input.Uv, 0.0f);
    if (XrEye.x > 0.5f)
        return float4(decodeSrgb(picture.rgb), 1.0f);
    return float4(picture.rgb, 1.0f);
}
