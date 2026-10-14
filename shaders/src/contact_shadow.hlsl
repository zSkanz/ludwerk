// Screen-space contact shadows for the sun.
//
// **What a shadow map cannot draw, drawn from the depth buffer.** Every bias a
// shadow map needs -- the normal offset, the depth bias, the filter's reach --
// is a few centimetres by which a shadow stops short of the thing casting it,
// and at a low sun a few centimetres at the base of a crate is a visible sliver
// of lit ground. The reference renderers close that sliver the same way: march
// a short ray from each visible point towards the sun through the depth buffer
// the camera already has, and call the point shadowed if the ray passes behind
// something. It knows nothing about what is off screen, which is why it is a
// supplement to the shadow map and never a replacement: the forward pass takes
// the darker of the two.
//
// Reads the depth prepass, writes one channel -- 1 lit, 0 in contact shadow --
// that `engine_forward.hlsli` multiplies into the sun and nothing else.

#define ENG_UNIFORMS_CONTACT
#include "engine_pbr.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D<float> DepthTexture : register(t0, space2);
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

static const int EngineContactSteps = 16;

float linearDepthOf(float deviceDepth)
{
    const float near = ContactProjection.z;
    const float far = ContactProjection.w;
    return (near * far) / max(far - deviceDepth * (far - near), 1e-6f);
}

float3 viewPositionOf(float2 uv, float linearDepth)
{
    const float x = (uv.x * 2.0f - 1.0f) * ContactProjection.x * linearDepth;
    const float y = (1.0f - uv.y * 2.0f) * ContactProjection.y * linearDepth;
    return float3(x, y, -linearDepth);
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float deviceDepth = DepthTexture.SampleLevel(DepthSampler, input.Uv, 0.0f);
    // The near plane is the side of a line a view is not seen on (ADR 0205,
    // `view_side.hlsl`): no ray is marched for it.
    if (deviceDepth >= 1.0f || deviceDepth <= 0.0f || ContactParams.w <= 0.0f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const float linearDepth = linearDepthOf(deviceDepth);
    // Contact detail is a near-field effect: past this it is sub-pixel, and a
    // ray a metre long at two hundred metres would shadow whole hillsides.
    const float distanceFade = saturate((ContactParams.z - linearDepth) / (ContactParams.z * 0.25f));
    if (distanceFade <= 0.0f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const float3 origin = viewPositionOf(input.Uv, linearDepth);
    const float3 toSun = ContactSun.xyz;
    // **As far as the shadow map's bias reaches here, the mask is whole**.
    // The map leaves lit, under whatever casts, a stretch along the
    // light as long as its bias: a few centimetres in the nearest cascade and
    // eight texels of a far one on a wall the sun grazes -- a third of a
    // metre. The mask faded from its ray's first step, so over that stretch
    // it let a fifth of the sun through, and a fifth of the sun on the wall
    // of a closed room is a pale line under its ceiling. By the cascade this
    // depth is in, as the lit pass chooses it.
    float biasReach = ContactBiasReach.w;
    if (linearDepth <= ContactCascadeFar.x)
        biasReach = ContactBiasReach.x;
    else if (linearDepth <= ContactCascadeFar.y)
        biasReach = ContactBiasReach.y;
    else if (linearDepth <= ContactCascadeFar.z)
        biasReach = ContactBiasReach.z;
    // The ray is longer further away, so it covers a similar number of pixels.
    // It is not made longer for a cascade whose bias outreaches it: sixteen
    // steps over a longer ray are steps that miss the contact this pass is
    // for. Its last quarter always fades, so its length is never an edge.
    const float rayLength = ContactSun.w * (1.0f + linearDepth * 0.02f);
    biasReach = min(biasReach, rayLength * 0.75f);
    const float thickness = ContactParams.x * (1.0f + linearDepth * 0.02f);
    // A surface must not shadow itself: a hit must be deeper than this to
    // count.
    const float bias = 0.01f + linearDepth * 0.0015f;

    // **Nor be the surface the ray starts on**. The buffer
    // is read at pixel centres, with no filter, and a marched point lands
    // anywhere in a pixel: on a surface the camera sees at a slant -- ground
    // forty metres off -- the depth read is up to half a pixel's change of
    // depth from the depth under the point, tens of centimetres against the
    // bias's few, and at a low sun the ray runs along that surface. The ground
    // stippled itself dark in a band before the fade. So the plane the ray
    // starts on is carried to every pixel it reads -- one over depth is affine
    // across the screen for a plane -- and a pixel on that plane is not a hit.
    // A bias grown by the slant instead lost the contact of whatever stood on
    // such ground, which is what this pass is for. Each slope is the smaller
    // of its two neighbours', so the edge of something standing beside the
    // pixel is not taken for the ground's.
    uint width = 0;
    uint height = 0;
    DepthTexture.GetDimensions(width, height);
    const float2 size = float2(float(max(width, 1u)), float(max(height, 1u)));
    const float2 texel = 1.0f / size;
    const float inverse = 1.0f / linearDepth;
    const float left = 1.0f / linearDepthOf(DepthTexture.SampleLevel(DepthSampler, input.Uv - float2(texel.x, 0.0f), 0.0f));
    const float right = 1.0f / linearDepthOf(DepthTexture.SampleLevel(DepthSampler, input.Uv + float2(texel.x, 0.0f), 0.0f));
    const float up = 1.0f / linearDepthOf(DepthTexture.SampleLevel(DepthSampler, input.Uv - float2(0.0f, texel.y), 0.0f));
    const float down = 1.0f / linearDepthOf(DepthTexture.SampleLevel(DepthSampler, input.Uv + float2(0.0f, texel.y), 0.0f));
    const float slopeX = abs(right - inverse) < abs(inverse - left) ? right - inverse : inverse - left;
    const float slopeY = abs(down - inverse) < abs(inverse - up) ? down - inverse : inverse - up;
    const float2 originPixel = floor(input.Uv * size) + 0.5f;

    // Interleaved gradient noise jitters where the steps fall, so sixteen of
    // them read as a smooth edge rather than sixteen bands.
    const float jitter = frac(52.9829189f * frac(dot(input.Position.xy, float2(0.06711056f, 0.00583715f))));

    float shadow = 0.0f;
    [unroll]
    for (int i = 0; i < EngineContactSteps; ++i)
    {
        const float t = (float(i) + jitter) / float(EngineContactSteps);
        const float3 marched = origin + toSun * (rayLength * t);
        const float sampleDepth = -marched.z;
        if (sampleDepth <= ContactProjection.z)
            break;

        float2 uv;
        uv.x = (marched.x / (ContactProjection.x * sampleDepth)) * 0.5f + 0.5f;
        uv.y = 0.5f - (marched.y / (ContactProjection.y * sampleDepth)) * 0.5f;
        if (any(uv < 0.0f) || any(uv > 1.0f))
            break;

        const float sceneDepth = linearDepthOf(DepthTexture.SampleLevel(DepthSampler, uv, 0.0f));
        // The starting plane at the centre of the pixel read, which is where
        // the depth read was drawn.
        const float2 across = floor(uv * size) + 0.5f - originPixel;
        const float plane = 1.0f / max(inverse + slopeX * across.x + slopeY * across.y, 1e-6f);
        if (abs(sceneDepth - plane) <= bias)
            continue;
        const float behind = sampleDepth - sceneDepth;
        // Behind something, but not so far behind that the something is a
        // different object in front of the ray rather than a caster it passes
        // under.
        if (behind > bias && behind < thickness)
        {
            // Hits within the map's own bias are the contact the shadow map
            // lost, whole; hits past it fade to the ray's end, so the ray's
            // length is not an edge.
            const float past = saturate((rayLength * t - biasReach) / max(rayLength - biasReach, 1e-4f));
            shadow = max(shadow, 1.0f - past * past);
        }
    }

    const float visibility = 1.0f - shadow * ContactParams.y * distanceFade;
    return float4(visibility, visibility, visibility, 1.0f);
}
