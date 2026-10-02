// A `Highlight`'s mask where the shape is SEEN (ADR 0129,
// `Enum.HighlightDepthMode.Occluded`): which pixels belong to the highlighted
// object and are not behind something else.
//
// `outline_mask` is the other mode -- every pixel the object covers, through
// whatever is in front of it. This one asks one more question, by hand: is
// this fragment in front of what the scene drew here? The mask pass has no
// depth attachment (it is drawn after the frame is finished, into a target of
// its own), so the scene's depth is read as a texture and compared.
//
// **With room for the surface itself.** The object being highlighted is IN
// that depth buffer, drawn by another vertex stage, and two stages do not land
// on the same depth to the last bit: an exact test would speckle the mask. The
// room is the fragment's own depth slope across a pixel and a half, plus a
// small constant, which is less than any gap between two things a person
// would call separate.
//
// The vertex stage is `shadow_depth`'s for the reason `outline_mask`'s is:
// this file's own entry makes the static pipeline, and the same fragment is
// paired with the skinned shadow stage for the other.

#define ENG_UNIFORMS_SHADOW
#include "engine_pbr.hlsli"

Texture2D SceneDepth : register(t0, space2);
SamplerState SceneDepthSampler : register(s0, space2);

struct VertexInput
{
    float3 Position : TEXCOORD0;
};

struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    output.Position = mul(LightViewProjection, mul(ShadowModel, float4(input.Position, 1.0f)));
    return output;
}

float FragmentMain(Interpolants input) : SV_Target0
{
    float width = 1.0f;
    float height = 1.0f;
    SceneDepth.GetDimensions(width, height);
    const float scene = SceneDepth.SampleLevel(SceneDepthSampler, input.Position.xy / float2(width, height), 0.0f).r;
    const float own = input.Position.z;
    const float room = 1.5f * (abs(ddx(own)) + abs(ddy(own))) + 2e-5f;
    if (own > scene + room)
        discard;
    return 1.0f;
}
