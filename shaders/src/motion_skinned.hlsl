// `motion.hlsl` for a skinned mesh (ADR 0158): posed by this frame's palette
// at both ends, so it carries how the whole moved and not how its joints
// turned -- the temporal pass's neighbourhood clamp takes the rest.

#define ENG_UNIFORMS_MOTION
#define ENG_UNIFORMS_SKIN
#include "engine_aa.hlsli"
#include "engine_pbr.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    // `float4` and not `uint4`: see `pbr_skinned.hlsl` and D042.
    float4 Joints : TEXCOORD1;
    float4 Weights : TEXCOORD2;
};

struct Interpolants
{
    float4 Now : TEXCOORD0;
    float4 Before : TEXCOORD1;
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    const float4 posed = mul(skinMatrix(uint4(input.Joints), input.Weights), float4(input.Position, 1.0f));
    output.Position = mul(MotionPosition, posed);
    output.Now = mul(MotionCurrent, posed);
    output.Before = mul(MotionPrevious, posed);
    return output;
}

float2 FragmentMain(Interpolants input) : SV_Target0
{
    const float2 now = input.Now.xy / max(input.Now.w, 1e-6f);
    const float2 before = input.Before.xy / max(input.Before.w, 1e-6f);
    return (now - before) * float2(0.5f, -0.5f);
}
