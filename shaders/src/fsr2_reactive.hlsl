// **Where the upscaler's history is not to be trusted** (ADR 0164): what
// blends -- a flame, smoke, a pane of glass, a label in the world -- writes no
// depth and has no motion of its own, so FSR 2 follows the surface behind it
// and drags the thing itself into a streak. This is the mask it is told by:
// how far each pixel is from the scene as it was before anything blended.
//
// After the pass AMD's runtime offers for the same (`ffxFsr2ContextGenerate
// ReactiveMask`): the difference of the two pictures, each compressed first so
// a bright pixel does not count for more than it shows. Twice the difference
// and not a threshold: a thing blended at an alpha moves a pixel by less than
// that alpha, and a mask in steps flickers where the difference crosses one.
// And never more than nine tenths, which is AMD's advice -- at one, a pixel
// has no history at all and is as jagged as it was rendered.
//
// A sprite drawn in its own colours (ADR 0153) is in the mask whole: it is
// not jittered, and what the history holds under it is.
//
// **And where something blended a frame ago, the mask is held a while**: the
// pixel a spark has just left has the spark in its history and nothing in
// this frame to say so, and it is the trail behind every spark. Each frame
// keeps six tenths of the last one's mask, so a pixel is distrusted for three
// or four frames after what blended over it has gone.

#include "engine_fullscreen.hlsli"

Texture2D OpaqueTexture : register(t0, space2);
SamplerState OpaqueSampler : register(s0, space2);
Texture2D SceneTexture : register(t1, space2);
SamplerState SceneSampler : register(s1, space2);
Texture2D<float> ExactTexture : register(t2, space2);
SamplerState ExactSampler : register(s2, space2);
Texture2D<float> PreviousTexture : register(t3, space2);
SamplerState PreviousSampler : register(s3, space2);

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

float3 compress(float3 colour)
{
    return colour / (max(max(0.0f, colour.r), max(colour.g, colour.b)) + 1.0f);
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float3 before = compress(OpaqueTexture.SampleLevel(OpaqueSampler, input.Uv, 0.0f).rgb);
    const float3 after = compress(SceneTexture.SampleLevel(SceneSampler, input.Uv, 0.0f).rgb);
    const float3 moved = abs(after - before);
    const float blended = min(max(moved.r, max(moved.g, moved.b)) * 2.0f, 0.9f);
    const float exact = ExactTexture.SampleLevel(ExactSampler, input.Uv, 0.0f) > 0.5f ? 0.9f : 0.0f;
    const float held = PreviousTexture.SampleLevel(PreviousSampler, input.Uv, 0.0f) * 0.6f;
    return float4(max(max(blended, exact), held), 0.0f, 0.0f, 1.0f);
}
