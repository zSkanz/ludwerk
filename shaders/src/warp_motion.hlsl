// **What a headset's runtime makes the frames between from** (ADR 0252): each
// opaque surface's motion since the frame before, in the eye's normalized
// device coordinates, and its depth.
//
// Through THIS frame's view at both ends -- `MotionPrevious` is this view
// with the surface where it was -- so a still thing moves by nothing: the
// head's own motion is the runtime's, which reprojects every frame from the
// depth, and given here as well it would move every still thing twice.
// Now minus before, x, y and depth, as the runtime reads it.

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
    output.Position = mul(MotionViewProjection, mul(MotionModel, local));
    output.Now = mul(MotionCurrent, local);
    output.Before = mul(MotionPrevious, local);
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float3 now = input.Now.xyz / max(input.Now.w, 1e-6f);
    const float3 before = input.Before.xyz / max(input.Before.w, 1e-6f);
    return float4(now - before, 0.0f);
}
