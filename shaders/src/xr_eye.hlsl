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
    // y above a half: the room is seen through (ADR 0247), and a pixel's
    // alpha says how much of the game is over it -- the coverage the picture
    // was drawn with, times 1 less z.
    float4 XrEye;
    // The part of the picture copied: its corner, then its width and height,
    // in parts of the picture's -- the whole of it into a headset, the middle
    // of it into a window of another shape.
    float4 XrEyeCrop;
};

Texture2D EyeTexture : register(t0, space2);
SamplerState EyeSampler : register(s0, space2);
// The eye's depth, read only when the room is seen through.

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

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float2 uv = XrEyeCrop.xy + input.Uv * XrEyeCrop.zw;
    const float4 picture = EyeTexture.SampleLevel(EyeSampler, uv, 0.0f);
    float3 color = XrEye.x > 0.5f ? decodeSrgb(picture.rgb) : picture.rgb;
    float alpha = 1.0f;
    if (XrEye.y > 0.5f) {
        // **The coverage the picture was drawn with** (a view with nothing
        // behind it): what blends covers as much as it is opaque, and what
        // writes no depth -- the world's interface, glass, particles -- is
        // over the room as much as a wall is. The runtime takes the colour
        // unpremultiplied, and the picture is premultiplied by its coverage.
        const float covered = saturate(picture.a);
        color = covered > 0.0f ? color / covered : float3(0.0f, 0.0f, 0.0f);
        alpha = covered * (1.0f - XrEye.z);
    }
    return float4(color, alpha);
}
