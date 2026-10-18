// **An eye's depth, into the headset's depth image** (ADR 0244 §5). The
// runtime reprojects the last picture to where the head is when it is shown,
// and with each pixel's depth it moves near things further than far ones --
// so it is copied from the depth the eye was drawn with, as it is: depth in
// [0, 1], 0 at the near plane, which `setDepthRange` names to the runtime.
// The eye may have been drawn smaller than its image (a render scale); the
// copy reads it by place, not by pixel.

#include "engine_fullscreen.hlsli"

Texture2D DepthTexture : register(t0, space2);
SamplerState DepthSampler : register(s0, space2);

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

float FragmentMain(Interpolants input) : SV_Depth
{
    return DepthTexture.SampleLevel(DepthSampler, input.Uv, 0.0f).r;
}
