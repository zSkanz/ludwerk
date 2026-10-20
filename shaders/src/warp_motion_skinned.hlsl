// `warp_motion.hlsl` for a skinned mesh: the pose this frame. A joint's own
// motion between frames is not in it -- the previous pose is not kept -- so
// an arm swinging is made between as its body moves; measured on the device
// before more is kept (ADR 0252 §8).

#define ENG_UNIFORMS_MOTION
#define ENG_UNIFORMS_SKIN
#include "engine_aa.hlsli"
#include "engine_pbr.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    // `float4` and not `uint4`: see `pbr_skinned.hlsl`.
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
    output.Position = mul(MotionViewProjection, mul(MotionModel, posed));
    output.Now = mul(MotionCurrent, posed);
    output.Before = mul(MotionPrevious, posed);
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float3 now = input.Now.xyz / max(input.Now.w, 1e-6f);
    const float3 before = input.Before.xyz / max(input.Before.w, 1e-6f);
    return float4(now - before, 0.0f);
}
