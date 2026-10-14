// Light that bounces off what is on the screen (ADR 0209).
//
// **Below the grain of a volume.** An `IrradianceVolume` holds bounced light at
// points metres apart: the red of a wall on the floor at its foot is smaller
// than that. It is on the screen, where the screen has it. Each pixel casts a
// few rays over the hemisphere it faces, a short way, through the depth the
// prepass wrote; where one lands, the LAST frame's lit picture says what light
// comes from there.
//
// Writes the light gathered -- the mean of what the landed rays saw, TIMES the
// share of the rays that landed -- and that share in alpha. The lit shader
// puts it in place of that share of what a volume or the ambient light gave
// it: the same light is never counted twice.
//
// **It feeds itself, and must not gain.** Last frame's picture holds last
// frame's bounce, so a ray lands on light that bounced before: that is a
// second bounce and it is right, and each landing is on a surface that gave
// back less than fell on it. What is read is held to what a lamp is and
// taken a little short, so a rounding upward cannot compound.
//
// Few rays a pixel is noise. It is gathered over frames: each pixel's last
// answer, found again where the pixel was, counts nine parts in ten.

#define ENG_UNIFORMS_SCREEN_TRACE
#include "engine_pbr.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D<float> DepthTexture : register(t0, space2);
SamplerState DepthSampler : register(s0, space2);
Texture2D SceneBeforeTexture : register(t1, space2);
SamplerState SceneBeforeSampler : register(s1, space2);
// This pass's own answer of the frame before.
Texture2D HistoryTexture : register(t2, space2);
SamplerState HistorySampler : register(s2, space2);

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

static const int EngineBounceMaxRays = 8;
static const int EngineBounceSteps = 8;

float linearDepthOf(float deviceDepth)
{
    const float near = TraceProjection.z;
    const float far = TraceProjection.w;
    return (near * far) / max(far - deviceDepth * (far - near), 1e-6f);
}

float3 viewPositionOf(float2 uv, float linearDepth)
{
    const float x = (uv.x * 2.0f - 1.0f) * TraceProjection.x * linearDepth;
    const float y = (1.0f - uv.y * 2.0f) * TraceProjection.y * linearDepth;
    return float3(x, y, -linearDepth);
}

float2 screenOf(float3 position)
{
    const float depth = max(-position.z, 1e-6f);
    float2 uv;
    uv.x = (position.x / (TraceProjection.x * depth)) * 0.5f + 0.5f;
    uv.y = 0.5f - (position.y / (TraceProjection.y * depth)) * 0.5f;
    return uv;
}

float depthAt(float2 uv)
{
    return linearDepthOf(DepthTexture.SampleLevel(DepthSampler, uv, 0.0f));
}

// A view-space point as a place in the last frame's picture; outside it, x
// is below nought.
float2 lastScreenOf(float3 position)
{
    const float4 clip = mul(TraceToLast, float4(position, 1.0f));
    if (clip.w <= 1e-5f)
    {
        return float2(-1.0f, -1.0f);
    }
    const float2 ndc = clip.xy / clip.w;
    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return any(uv < 0.0f) || any(uv > 1.0f) ? float2(-1.0f, -1.0f) : uv;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float4 nothing = float4(0.0f, 0.0f, 0.0f, 0.0f);
    // At a texel's centre, for the reason `screen_trace.hlsl` gives.
    uint depthWidth = 0;
    uint depthHeight = 0;
    DepthTexture.GetDimensions(depthWidth, depthHeight);
    const float2 depthSize = float2(float(max(depthWidth, 1u)), float(max(depthHeight, 1u)));
    const float2 centre = (floor(input.Uv * depthSize - 0.25f) + 0.5f) / depthSize;
    const float deviceDepth = DepthTexture.SampleLevel(DepthSampler, centre, 0.0f);
    if (deviceDepth >= 1.0f || deviceDepth <= 0.0f || TraceFrame.x <= 0.0f)
    {
        return nothing;
    }

    const float linearDepth = linearDepthOf(deviceDepth);
    const float3 position = viewPositionOf(centre, linearDepth);
    const float2 texel = 1.0f / depthSize;
    const float3 left = viewPositionOf(centre - float2(texel.x, 0.0f), depthAt(centre - float2(texel.x, 0.0f)));
    const float3 right = viewPositionOf(centre + float2(texel.x, 0.0f), depthAt(centre + float2(texel.x, 0.0f)));
    const float3 up = viewPositionOf(centre - float2(0.0f, texel.y), depthAt(centre - float2(0.0f, texel.y)));
    const float3 down = viewPositionOf(centre + float2(0.0f, texel.y), depthAt(centre + float2(0.0f, texel.y)));
    const float3 across = abs(right.z - position.z) < abs(position.z - left.z) ? right - position : position - left;
    const float3 along = abs(down.z - position.z) < abs(position.z - up.z) ? down - position : position - up;
    float3 normal = normalize(cross(across, along));
    if (dot(normal, -position) < 0.0f)
    {
        normal = -normal;
    }
    const float3 tangent =
        normalize(abs(normal.z) < 0.99f ? cross(float3(0.0f, 0.0f, 1.0f), normal) : float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = cross(normal, tangent);

    const int rays = clamp(int(TraceParams.z + 0.5f), 1, EngineBounceMaxRays);
    const float reach = TraceParams.x * (1.0f + linearDepth * 0.03f);
    const float thickness = TraceParams.y * (1.0f + linearDepth * 0.03f);
    const float bias = 0.03f + linearDepth * 0.003f;
    const float3 origin = position + normal * bias;
    // A turn that differs by pixel and by frame: the same few directions
    // every frame would be the same few answers gathered for ever.
    const float turn =
        frac(52.9829189f * frac(dot(input.Position.xy + TraceFrame.y * float2(31.0f, 59.0f), float2(0.06711056f, 0.00583715f))));

    float3 gathered = float3(0.0f, 0.0f, 0.0f);
    float answered = 0.0f;
    [loop]
    for (int ray = 0; ray < EngineBounceMaxRays; ++ray)
    {
        if (ray >= rays)
            break;
        // Cosine-weighted over the hemisphere: more rays where more of a
        // surface's light comes from, so the mean of what they see is the
        // light, with nothing to weigh.
        const float u = (float(ray) + 0.5f) / float(rays);
        const float angle = (float(ray) * 0.61803399f + turn) * 6.28318530718f;
        const float radius = sqrt(u);
        const float3 direction =
            normalize(tangent * (cos(angle) * radius) + bitangent * (sin(angle) * radius) + normal * sqrt(1.0f - u));

        float before = 0.0f;
        [loop]
        for (int i = 0; i < EngineBounceSteps; ++i)
        {
            const float fraction = (float(i) + frac(turn + float(ray) * 0.37f)) / float(EngineBounceSteps);
            const float t = reach * fraction * fraction;
            const float3 marched = origin + direction * t;
            const float sampleDepth = -marched.z;
            if (sampleDepth <= TraceProjection.z)
                break;
            const float2 uv = screenOf(marched);
            if (any(uv < 0.0f) || any(uv > 1.0f))
                break;
            const float sceneDevice = DepthTexture.SampleLevel(DepthSampler, uv, 0.0f);
            const float behind = sampleDepth - linearDepthOf(sceneDevice);
            if (behind > 0.0f && behind < thickness + (t - before))
            {
                // On the sky, or the far side of a view's line: no light of
                // the scene's is there to bounce.
                if (sceneDevice < 1.0f && sceneDevice > 0.0f)
                {
                    const float2 lastUv = lastScreenOf(marched);
                    if (lastUv.x >= 0.0f)
                    {
                        gathered += min(SceneBeforeTexture.SampleLevel(SceneBeforeSampler, lastUv, 0.0f).rgb, 16.0f);
                        answered += 1.0f;
                    }
                }
                break;
            }
            before = t;
        }
    }
    // The mean of what the landed rays saw, times the share that landed --
    // and a little short of it, so what feeds itself cannot grow.
    float4 now = float4(gathered * (0.94f / float(rays)), answered / float(rays));

    // Gathered over frames: where this pixel was, what it answered then --
    // **and what its neighbours on the same surface answered.** Eight rays a
    // pixel beside something very bright are an answer of "all of it" here
    // and "none" a pixel over, and gathered pixel by pixel that is a grain
    // that never settles: a speckled halo round every lamp. Read with its
    // four neighbours each frame, the answer spreads over what it lies on,
    // and not across an edge: a neighbour at another depth is another
    // surface.
    //
    // **Four neighbours that turn, and stand nearer or further, by pixel and
    // by frame.** At four fixed places -- the corners, a texel and a half
    // out -- there is a pattern the reading leaves exactly as it found it:
    // one that repeats every three texels both ways, each corner landing on
    // its like. The grain settled into that lattice and stayed, a field of
    // dots round every lamp however long it was gathered.
    if (TraceFrame.z > 0.0f)
    {
        // Where the PIXEL was, not where the depth's texel was -- a quarter
        // of a pixel aside, which gathered is a creep (`screen_trace.hlsl`).
        const float2 lastUv = lastScreenOf(viewPositionOf(input.Uv, linearDepth));
        if (lastUv.x >= 0.0f)
        {
            float4 was = HistoryTexture.SampleLevel(HistorySampler, lastUv, 0.0f);
            float weight = 1.0f;
            const float spin = frac(turn * 3.7f + 0.13f) * 6.28318530718f;
            // This pass's own texel is two of the depth's: from one of its
            // own to three.
            const float2 arm = float2(cos(spin), sin(spin)) * (2.0f + 4.0f * frac(turn * 7.31f));
            const float2 corners[4] = {arm, -arm, float2(-arm.y, arm.x), float2(arm.y, -arm.x)};
            [unroll]
            for (int corner = 0; corner < 4; ++corner)
            {
                const float2 offset = corners[corner] * texel;
                const float beside = depthAt(centre + offset);
                if (abs(beside - linearDepth) < 0.05f + linearDepth * 0.03f)
                {
                    was += HistoryTexture.SampleLevel(HistorySampler, lastUv + offset, 0.0f);
                    weight += 1.0f;
                }
            }
            now = lerp(now, was / weight, TraceFrame.w);
        }
    }
    return now;
}
