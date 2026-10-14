// A reflection probe's six pictures into one octahedral map (ADR 0202): the
// form the sky's own reflection is kept in, so a surface reads a room the way
// it reads the sky.
//
// Each texel of the map is a direction. The picture that direction falls in
// is the one whose camera it is most in front of, and where in that picture
// is that camera's own projection of it -- the matrices the pictures were
// drawn with, handed over as they were used, so nothing here has to know
// which way a camera's axes run.

#include "engine_fullscreen.hlsli"

cbuffer GpuProbeGatherUniforms : register(b0, space3)
{
    // Camera-relative, and applied to a direction (w of nought): where the
    // camera stood does not enter into it.
    column_major float4x4 FaceViewProjection[6];
};

Texture2D Face0 : register(t0, space2);
SamplerState Face0Sampler : register(s0, space2);
Texture2D Face1 : register(t1, space2);
SamplerState Face1Sampler : register(s1, space2);
Texture2D Face2 : register(t2, space2);
SamplerState Face2Sampler : register(s2, space2);
Texture2D Face3 : register(t3, space2);
SamplerState Face3Sampler : register(s3, space2);
Texture2D Face4 : register(t4, space2);
SamplerState Face4Sampler : register(s4, space2);
Texture2D Face5 : register(t5, space2);
SamplerState Face5Sampler : register(s5, space2);

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

// `render::octahedralDirection`, written a second time: the two have to agree.
float3 octahedralDirection(float2 uv)
{
    const float2 f = uv * 2.0f - 1.0f;
    float3 d = float3(f.x, 1.0f - abs(f.x) - abs(f.y), f.y);
    const float fold = saturate(-d.y);
    d.x += d.x >= 0.0f ? -fold : fold;
    d.z += d.z >= 0.0f ? -fold : fold;
    return normalize(d);
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float3 direction = octahedralDirection(input.Uv);

    // The camera the direction is most in front of: `w` is how far along that
    // camera's own forward axis the direction runs.
    uint face = 0u;
    float4 best = mul(FaceViewProjection[0], float4(direction, 0.0f));
    [unroll] for (uint index = 1u; index < 6u; ++index)
    {
        const float4 clip = mul(FaceViewProjection[index], float4(direction, 0.0f));
        if (clip.w > best.w)
        {
            best = clip;
            face = index;
        }
    }
    // Held a texel's worth inside the picture: its edge is another picture's.
    const float2 ndc = clamp(best.xy / max(best.w, 1e-5f), -1.0f, 1.0f);
    const float2 uv = ndc * float2(0.5f, -0.5f) + 0.5f;

    float3 color;
    if (face == 0u)
    {
        color = Face0.SampleLevel(Face0Sampler, uv, 0.0f).rgb;
    }
    else if (face == 1u)
    {
        color = Face1.SampleLevel(Face1Sampler, uv, 0.0f).rgb;
    }
    else if (face == 2u)
    {
        color = Face2.SampleLevel(Face2Sampler, uv, 0.0f).rgb;
    }
    else if (face == 3u)
    {
        color = Face3.SampleLevel(Face3Sampler, uv, 0.0f).rgb;
    }
    else if (face == 4u)
    {
        color = Face4.SampleLevel(Face4Sampler, uv, 0.0f).rgb;
    }
    else
    {
        color = Face5.SampleLevel(Face5Sampler, uv, 0.0f).rgb;
    }
    // A picture holds what the frame held, and a frame may hold a number no
    // surface should be lit by: one texel of the sun through a window, or a
    // value that is not one.
    color = min(max(color, 0.0f), 64.0f);
    return float4(color, 1.0f);
}
