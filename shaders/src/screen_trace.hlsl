// Reflections from the screen: where each pixel's mirror ray lands (ADR 0209).
//
// **What a probe cannot say, read off the picture.** A `ReflectionProbe` is a
// room seen from one point: a polished floor reflects the room and not the
// character standing on it, nor anything that moved since. Both are on the
// screen already, where the screen has them. So before the lit pass, each
// pixel's mirror ray is marched through the depth the prepass wrote, and where
// it lands is looked up in the LAST frame's lit picture -- the picture there
// is: this frame's has not been lit yet -- and written down with how sure the
// landing is. The lit shader reads that at its own pixel, as it reads the
// occlusion texture, a level further down this picture's own chain the
// rougher its surface is.
//
// **There is no G-buffer, and this pass is why that is affordable.** The
// normal is rebuilt from depth, as the occlusion pass rebuilds it; roughness is
// in no target, so the march is for the mirror direction and the blur is the
// lit shader's, which knows how rough its surface is. What is lost is said in
// the ADR: a normal map does not bend the ray.
//
// **Never black.** A ray that leaves the screen, turns back to the camera,
// lands on the sky or lands nowhere is a miss: confidence nought, and the lit
// shader keeps what the probe or the sky says.
//
// Writes the colour the ray landed on TIMES how sure the landing is, and how
// sure in alpha: so the chain made of this picture blurs a landing into a
// miss without darkening it, and a miss is nothing at all.

#define ENG_UNIFORMS_SCREEN_TRACE
#include "engine_pbr.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D<float> DepthTexture : register(t0, space2);
SamplerState DepthSampler : register(s0, space2);
// The last frame as the lit and blended passes left it: linear, before bloom,
// tone and the interface.
Texture2D SceneBeforeTexture : register(t1, space2);
SamplerState SceneBeforeSampler : register(s1, space2);
// This pass's own answer of the frame before. **A ray is marched in steps,
// and a thing a step can straddle is landed on by some pixels and stepped
// over by their neighbours**: the far end of every reflection -- the rays that
// pass just over the top of what is reflected -- was a ragged fringe a few
// pixels deep, and a still picture is what a player sees when the camera
// rests. So the start of the march differs every frame and the answers are
// gathered: at rest the fringe is the soft edge it should be, whatever the
// frame's own anti-aliasing is.
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

// The most steps any level takes; a level takes `TraceParams.z` of them.
static const int EngineTraceMaxSteps = 48;
static const int EngineTraceRefineSteps = 4;

float linearDepthOf(float deviceDepth)
{
    const float near = TraceProjection.z;
    const float far = TraceProjection.w;
    return (near * far) / max(far - deviceDepth * (far - near), 1e-6f);
}

float3 viewPositionOf(float2 uv, float linearDepth)
{
    const float x = (uv.x * 2.0f - 1.0f + TraceCenter.x) * TraceProjection.x * linearDepth;
    const float y = (1.0f - uv.y * 2.0f + TraceCenter.y) * TraceProjection.y * linearDepth;
    return float3(x, y, -linearDepth);
}

// A view-space point as a place on this frame's screen.
float2 screenOf(float3 position)
{
    const float depth = max(-position.z, 1e-6f);
    float2 uv;
    uv.x = (position.x / (TraceProjection.x * depth) - TraceCenter.x) * 0.5f + 0.5f;
    uv.y = 0.5f - (position.y / (TraceProjection.y * depth) - TraceCenter.y) * 0.5f;
    return uv;
}

float depthAt(float2 uv)
{
    return linearDepthOf(DepthTexture.SampleLevel(DepthSampler, uv, 0.0f));
}

// How near a screen place is to the picture's edge, as a fade: a reflection
// that is cut off at the edge of the screen fades into the probe's instead.
float edgeFade(float2 uv)
{
    const float2 inside = min(uv, 1.0f - uv);
    return saturate(min(inside.x, inside.y) * 12.0f);
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    float4 miss = float4(0.0f, 0.0f, 0.0f, 0.0f);
    // **Read at the centre of a texel of the depth, never between four.**
    // This pass is half the picture's size, so its own pixel centres are the
    // corners of the depth's texels: read there, a point sampler picks one of
    // the four by the last bit of the coordinate, and the position rebuilt
    // from that depth is not under the place it is said to be. On a floor
    // seen at a slant that tilted the normal on whole rows of the screen, and
    // every ray of those rows went into the floor: lines across the
    // reflection where nothing landed.
    uint depthWidth = 0;
    uint depthHeight = 0;
    DepthTexture.GetDimensions(depthWidth, depthHeight);
    const float2 depthSize = float2(float(max(depthWidth, 1u)), float(max(depthHeight, 1u)));
    // **A quarter of a texel short, so every GPU picks the same one.** At
    // half the picture's size this pixel's centre is exactly ON the line
    // between two of the depth's texels, and which side `floor` falls is
    // the last bit of an interpolated coordinate: one graphics card took
    // the texel to the right and another the one to the left.
    const float2 centre = (floor(input.Uv * depthSize - 0.25f) + 0.5f) / depthSize;
    const float deviceDepth = DepthTexture.SampleLevel(DepthSampler, centre, 0.0f);
    // The sky reflects nothing, and the near plane is the side of a line a
    // view is not seen on (ADR 0205, `view_side.hlsl`). And a frame with no
    // last frame -- the first, or the one after a cut -- has nothing to read.
    if (deviceDepth >= 1.0f || deviceDepth <= 0.0f || TraceFrame.x <= 0.0f)
    {
        return miss;
    }

    const float linearDepth = linearDepthOf(deviceDepth);
    const float3 position = viewPositionOf(centre, linearDepth);

    // What this pixel answered the frame before, found where the pixel was.
    // A miss this frame is gathered with it like a landing: both are answers.
    //
    // **Where the PIXEL was, not where the depth's texel was.** The texel is
    // a quarter of this pass's pixel to one side of the pixel's own centre.
    // Found from there, under a camera that had not moved, the answer of the
    // frame before was read a quarter of a pixel off -- and so was the one
    // before that: everything gathered crept sideways a quarter of a pixel a
    // frame, for as many frames as were gathered. The glow at a wall's foot
    // stood eight pixels to one side of the wall, and to the other side on
    // another graphics card.
    float4 was = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float gather = 0.0f;
    if (TraceFrame.z > 0.0f)
    {
        const float4 lastPlace = mul(TraceToLast, float4(viewPositionOf(input.Uv, linearDepth), 1.0f));
        if (lastPlace.w > 1e-5f)
        {
            const float2 lastNdc = lastPlace.xy / lastPlace.w;
            const float2 lastUv = float2(lastNdc.x * 0.5f + 0.5f, 0.5f - lastNdc.y * 0.5f);
            if (all(lastUv >= 0.0f) && all(lastUv <= 1.0f))
            {
                was = HistoryTexture.SampleLevel(HistorySampler, lastUv, 0.0f);
                gather = TraceFrame.w;
            }
        }
    }
    miss = was * gather;

    // The plane the surface lies in, from its neighbours on the same surface:
    // the occlusion pass's reconstruction, for the reason it gives.
    // One texel of the depth apart, each at a texel's centre.
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

    const float3 incoming = normalize(position);
    const float3 ray = reflect(incoming, normal);
    // A ray that comes back towards the camera lands on what the camera sees
    // the FRONT of, and its back is what a mirror would show: a miss, faded
    // in so the line where it starts is not seen.
    const float away = saturate(1.0f - ray.z * 2.5f);
    if (away <= 0.0f)
    {
        return miss;
    }

    const int steps = clamp(int(TraceParams.z + 0.5f), 1, EngineTraceMaxSteps);
    // Longer further off, so a ray covers a like number of pixels.
    const float reach = TraceParams.x * (1.0f + linearDepth * 0.05f);
    const float thickness = TraceParams.y * (1.0f + linearDepth * 0.05f);
    const float bias = 0.02f + linearDepth * 0.002f;
    // Lifted off the surface it starts on, so the first step does not land in
    // it; and a start that differs by pixel and by frame, which the frame's
    // temporal pass -- or the blur after this one -- makes a smooth edge of.
    const float3 origin = position + normal * bias;
    const float jitter =
        frac(52.9829189f * frac(dot(input.Position.xy + TraceFrame.y * float2(47.0f, 17.0f), float2(0.06711056f, 0.00583715f))));

    float before = 0.0f;
    float landed = -1.0f;
    [loop]
    for (int i = 0; i < EngineTraceMaxSteps; ++i)
    {
        if (i >= steps)
            break;
        // Steps that grow: near the surface is where a contact reflection is,
        // and where a coarse step would walk through a thin thing.
        const float fraction = (float(i) + jitter) / float(steps);
        const float t = reach * fraction * fraction;
        const float3 marched = origin + ray * t;
        const float sampleDepth = -marched.z;
        if (sampleDepth <= TraceProjection.z)
            break;
        const float2 uv = screenOf(marched);
        if (any(uv < 0.0f) || any(uv > 1.0f))
            break;
        const float behind = sampleDepth - depthAt(uv);
        if (behind > 0.0f && behind < thickness + (t - before))
        {
            landed = t;
            break;
        }
        before = t;
    }
    if (landed < 0.0f)
    {
        return miss;
    }

    // Between the last step in front and the first behind, halved a few times.
    float near = before;
    float far = landed;
    [unroll]
    for (int refine = 0; refine < EngineTraceRefineSteps; ++refine)
    {
        const float middle = 0.5f * (near + far);
        const float3 marched = origin + ray * middle;
        if (-marched.z - depthAt(screenOf(marched)) > 0.0f)
            far = middle;
        else
            near = middle;
    }
    const float3 hit = origin + ray * far;
    const float2 hitUv = screenOf(hit);
    const float hitScene = DepthTexture.SampleLevel(DepthSampler, hitUv, 0.0f);
    // Landed on the sky, or on the far side of a line (the near plane): the
    // probe's and the sky's own answer is better than a pixel of either.
    if (hitScene >= 1.0f || hitScene <= 0.0f)
    {
        return miss;
    }
    // **And a landing is ON something.** Halved, the ray's place and the
    // depth under it meet where the ray crosses a surface -- or, where it
    // only passed BEHIND a thing, at that thing's edge, with the depth a long
    // way in front of the ray. That is no landing: it is the colour of
    // whatever the ray went behind, smeared over what is really reflected.
    if (abs(-hit.z - linearDepthOf(hitScene)) > 0.15f + far * 0.03f)
    {
        return miss;
    }

    // Where that point was in the LAST frame's picture, which is the picture
    // there is: this frame's has not been lit yet.
    const float4 lastClip = mul(TraceToLast, float4(hit, 1.0f));
    if (lastClip.w <= 1e-5f)
    {
        return miss;
    }
    const float2 lastNdc = lastClip.xy / lastClip.w;
    const float2 lastUv = float2(lastNdc.x * 0.5f + 0.5f, 0.5f - lastNdc.y * 0.5f);
    if (any(lastUv < 0.0f) || any(lastUv > 1.0f))
    {
        return miss;
    }

    // How sure: not near the screen's edge in either frame, not at the end of
    // its reach, and not a ray that all but turned back.
    const float fraction = saturate(far / reach);
    const float sure = edgeFade(hitUv) * edgeFade(lastUv) * (1.0f - fraction * fraction) * away;
    // One very bright pixel -- the sun's glint on a wet stone -- would be a
    // sparkle that jumps with the jitter: held to what a lamp is.
    const float3 colour = min(SceneBeforeTexture.SampleLevel(SceneBeforeSampler, lastUv, 0.0f).rgb, 32.0f);
    return lerp(float4(colour * sure, sure), was, gather);
}
