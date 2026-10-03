// Temporal anti-aliasing (ADR 0158): this frame, drawn a fraction of a pixel
// off from the last one, blended into what the frames before it made of the
// same surface -- found where it was through the motion vectors. Over eight
// frames every pixel is sampled at eight places, which is how a thin line
// stops crawling and a fence stops shimmering when the camera pans.
//
// In linear HDR, before exposure and bloom, as the engines that do this put
// it; blended in a tonemapped space, so one bright pixel cannot outweigh the
// history (Karis, "High Quality Temporal Supersampling", 2014). What history
// does not belong to this pixel any more -- a surface uncovered, a light
// switched on -- is clipped to the colours this frame's neighbourhood has
// (Salvi, "An Excursion in Temporal Supersampling", 2016): a variance box in
// YCoCg, clipped toward its centre rather than clamped per channel.

#define ENG_UNIFORMS_TAA
#include "engine_aa.hlsli"
#include "engine_fullscreen.hlsli"

// The current frame, the velocities and the depth are read texel for texel,
// through point samplers; the history between texels, through a linear one.
Texture2D CurrentTexture : register(t0, space2);
SamplerState CurrentSampler : register(s0, space2);
Texture2D HistoryTexture : register(t1, space2);
SamplerState HistorySampler : register(s1, space2);
Texture2D VelocityTexture : register(t2, space2);
SamplerState VelocitySampler : register(s2, space2);
Texture2D DepthTexture : register(t3, space2);
SamplerState DepthSampler : register(s3, space2);

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

float lumaOf(float3 colour)
{
    return dot(colour, float3(0.2126f, 0.7152f, 0.0722f));
}

// Reversible: what the blend works in, and back.
float3 compress(float3 colour)
{
    return colour / (1.0f + max(colour.r, max(colour.g, colour.b)));
}
float3 expand(float3 colour)
{
    return colour / max(1.0f - max(colour.r, max(colour.g, colour.b)), 1e-4f);
}

float3 toYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25f, 0.5f, 0.25f)), dot(c, float3(0.5f, 0.0f, -0.5f)),
                  dot(c, float3(-0.25f, 0.5f, -0.25f)));
}
float3 fromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The history between texels, by a Catmull-Rom spline in five bilinear taps
// (Jimenez's arrangement): sharper than one bilinear tap, which would blur
// the picture a little more each frame it is carried.
float3 sampleHistory(float2 uv)
{
    const float2 size = TaaTexel.zw;
    const float2 position = uv * size;
    const float2 centre = floor(position - 0.5f) + 0.5f;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);
    const float2 w12 = w1 + w2;
    const float2 tc12 = (centre + w2 / w12) * TaaTexel.xy;
    const float2 tc0 = (centre - 1.0f) * TaaTexel.xy;
    const float2 tc3 = (centre + 2.0f) * TaaTexel.xy;
    float3 sum = HistoryTexture.SampleLevel(HistorySampler, float2(tc12.x, tc0.y), 0.0f).rgb * (w12.x * w0.y);
    sum += HistoryTexture.SampleLevel(HistorySampler, float2(tc0.x, tc12.y), 0.0f).rgb * (w0.x * w12.y);
    sum += HistoryTexture.SampleLevel(HistorySampler, float2(tc12.x, tc12.y), 0.0f).rgb * (w12.x * w12.y);
    sum += HistoryTexture.SampleLevel(HistorySampler, float2(tc3.x, tc12.y), 0.0f).rgb * (w3.x * w12.y);
    sum += HistoryTexture.SampleLevel(HistorySampler, float2(tc12.x, tc3.y), 0.0f).rgb * (w12.x * w3.y);
    const float weight =
        w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / max(weight, 1e-4f), 0.0f);
}

// Toward the box's centre until inside it, as a ray would leave it.
float3 clipToBox(float3 history, float3 low, float3 high)
{
    const float3 centre = 0.5f * (high + low);
    const float3 extent = 0.5f * (high - low) + 1e-5f;
    const float3 offset = history - centre;
    const float3 units = abs(offset / extent);
    const float most = max(units.x, max(units.y, units.z));
    return most > 1.0f ? centre + offset / most : history;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    // From the coordinate, not `SV_Position`: see `fsr_rcas.hlsl`.
    const int2 pixel = int2(input.Uv * TaaTexel.zw);
    const int2 last = int2(TaaTexel.zw) - 1;

    // The nearest surface round the pixel lends it its motion: an edge's
    // pixels move with what is in front, which keeps a moving outline from
    // trailing the background's history behind it.
    float nearest = 1.0f;
    int2 nearestPixel = pixel;
    float3 sum = 0.0f;
    float3 squares = 0.0f;
    float3 centre = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 at = clamp(pixel + int2(x, y), int2(0, 0), last);
            const float2 atUv = (float2(at) + 0.5f) * TaaTexel.xy;
            const float depth = DepthTexture.SampleLevel(DepthSampler, atUv, 0.0f).r;
            if (depth < nearest) {
                nearest = depth;
                nearestPixel = at;
            }
            const float3 colour =
                toYCoCg(compress(max(CurrentTexture.SampleLevel(CurrentSampler, atUv, 0.0f).rgb, 0.0f)));
            sum += colour;
            squares += colour * colour;
            if (x == 0 && y == 0)
                centre = colour;
        }
    }
    const float3 mean = sum / 9.0f;
    const float3 deviation = sqrt(max(squares / 9.0f - mean * mean, 0.0f));
    const float3 low = mean - TaaBlend.w * deviation;
    const float3 high = mean + TaaBlend.w * deviation;

    const float2 velocity =
        VelocityTexture.SampleLevel(VelocitySampler, (float2(nearestPixel) + 0.5f) * TaaTexel.xy, 0.0f).xy;
    const float2 before = input.Uv - velocity;
    const bool outside = any(before < 0.0f) || any(before > 1.0f);
    if (TaaBlend.z < 0.5f || outside)
        return float4(expand(fromYCoCg(centre)), 1.0f);

    const float3 history = clipToBox(toYCoCg(compress(sampleHistory(before))), low, high);

    // More of this frame the faster the pixel moves: history resampled every
    // frame softens, and a moving thing shows less of the aliasing it hides.
    const float pixelsMoved = length(velocity * TaaTexel.zw);
    const float alpha = lerp(TaaBlend.x, TaaBlend.y, saturate(pixelsMoved / 4.0f));
    const float currentWeight = alpha / (1.0f + centre.x);
    const float historyWeight = (1.0f - alpha) / (1.0f + history.x);
    const float3 blended = (centre * currentWeight + history * historyWeight) / (currentWeight + historyWeight);
    const float3 result = expand(max(fromYCoCg(blended), 0.0f));
    // Nothing that is not a number is carried into the next frame.
    return float4(any(isnan(result)) ? 0.0f.xxx : result, 1.0f);
}
