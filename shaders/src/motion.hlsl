// How far a thing that moves moved since the last frame (ADR 0158), over the
// camera's own motion `taa_velocity.hlsl` wrote: a part, a character, a car --
// drawn with where it is and where it was, against the depth already there.
//
// The vertex stage takes the shadow pass's inputs -- a position, and for the
// skinned twin the joints -- because that is all a motion needs.

#define ENG_UNIFORMS_MOTION
#include "engine_aa.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
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
    const float4 local = float4(input.Position, 1.0f);
    output.Position = mul(MotionPosition, local);
    output.Now = mul(MotionCurrent, local);
    output.Before = mul(MotionPrevious, local);
    return output;
}

// Divided here, per pixel, and not in the vertex stage: a triangle that runs
// behind the camera interpolates its clip positions correctly and its
// projected ones not at all.
float2 FragmentMain(Interpolants input) : SV_Target0
{
    const float2 now = input.Now.xy / max(input.Now.w, 1e-6f);
    const float2 before = input.Before.xy / max(input.Before.w, 1e-6f);
    return (now - before) * float2(0.5f, -0.5f);
}
