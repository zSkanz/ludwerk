// **Several players' views, put into one window** (ADR 0205). Each was drawn
// into a picture the size of its part of the window; this is the pass that
// makes the window of them, and it is the only thing a split screen adds to a
// frame besides the views themselves.
//
// Two forms. **Rectangles**: up to four views, each copied to its own, with a
// line along every edge two of them share and black where there is none (the
// fourth quarter of three in a grid). **Across a line**: two views the size of
// the window, the first on one side of a line through its middle and the
// second on the other, the line as thick as it is asked to be.
//
// And over either, the whole window's one picture by a weight: how a screen
// that parts into a grid fades into it and back, where a cut would be seen.

#include "engine_fullscreen.hlsli"

cbuffer GpuViewsUniforms : register(b0, space3)
{
    // A view's rectangle in pixels of the target: xy its corner, zw its size.
    // A size of nought is no view.
    float4 ViewRects[4];
    // xy the target in pixels, z how many views, w above a half for two views
    // across a line.
    float4 ViewsTarget;
    // xy the line's normal, from the first view's side to the second's, y
    // down; z half the line's thickness in pixels; w how much of the whole
    // picture shows over the views, 0 to 1.
    float4 ViewsLine;
    // rgb the line's colour.
    float4 ViewsLineColor;
};

Texture2D View0Texture : register(t0, space2);
SamplerState View0Sampler : register(s0, space2);
Texture2D View1Texture : register(t1, space2);
SamplerState View1Sampler : register(s1, space2);
Texture2D View2Texture : register(t2, space2);
SamplerState View2Sampler : register(s2, space2);
Texture2D View3Texture : register(t3, space2);
SamplerState View3Sampler : register(s3, space2);
Texture2D WholeTexture : register(t4, space2);
SamplerState WholeSampler : register(s4, space2);

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

bool inside(float4 rect, float2 pixel)
{
    return rect.z > 0.0f && pixel.x >= rect.x && pixel.y >= rect.y && pixel.x < rect.x + rect.z &&
           pixel.y < rect.y + rect.w;
}

// Whether `pixel` is on one of the rectangle's edges that is not the
// window's own: the line between two views, and between a view and the
// part of the window nobody has.
bool onEdge(float4 rect, float2 pixel, float halfWidth)
{
    if (rect.z <= 0.0f || halfWidth <= 0.0f)
        return false;
    const float2 low = rect.xy;
    const float2 high = rect.xy + rect.zw;
    const bool alongX = pixel.x > low.x - halfWidth && pixel.x < high.x + halfWidth;
    const bool alongY = pixel.y > low.y - halfWidth && pixel.y < high.y + halfWidth;
    bool edge = false;
    if (low.x > 0.5f)
        edge = edge || (abs(pixel.x - low.x) < halfWidth && alongY);
    if (high.x < ViewsTarget.x - 0.5f)
        edge = edge || (abs(pixel.x - high.x) < halfWidth && alongY);
    if (low.y > 0.5f)
        edge = edge || (abs(pixel.y - low.y) < halfWidth && alongX);
    if (high.y < ViewsTarget.y - 0.5f)
        edge = edge || (abs(pixel.y - high.y) < halfWidth && alongX);
    return edge;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float2 pixel = input.Uv * ViewsTarget.xy;
    const float halfWidth = ViewsLine.z;
    float3 color = float3(0.0f, 0.0f, 0.0f);
    bool onLine = false;
    if (ViewsTarget.w > 0.5f)
    {
        const float side = dot(pixel - 0.5f * ViewsTarget.xy, ViewsLine.xy);
        if (side < 0.0f)
            color = View0Texture.SampleLevel(View0Sampler, input.Uv, 0.0f).rgb;
        else
            color = View1Texture.SampleLevel(View1Sampler, input.Uv, 0.0f).rgb;
        onLine = abs(side) < halfWidth;
    }
    else
    {
        if (inside(ViewRects[0], pixel))
            color = View0Texture.SampleLevel(View0Sampler, (pixel - ViewRects[0].xy) / ViewRects[0].zw, 0.0f).rgb;
        else if (inside(ViewRects[1], pixel))
            color = View1Texture.SampleLevel(View1Sampler, (pixel - ViewRects[1].xy) / ViewRects[1].zw, 0.0f).rgb;
        else if (inside(ViewRects[2], pixel))
            color = View2Texture.SampleLevel(View2Sampler, (pixel - ViewRects[2].xy) / ViewRects[2].zw, 0.0f).rgb;
        else if (inside(ViewRects[3], pixel))
            color = View3Texture.SampleLevel(View3Sampler, (pixel - ViewRects[3].xy) / ViewRects[3].zw, 0.0f).rgb;
        onLine = onEdge(ViewRects[0], pixel, halfWidth) || onEdge(ViewRects[1], pixel, halfWidth) ||
               onEdge(ViewRects[2], pixel, halfWidth) || onEdge(ViewRects[3], pixel, halfWidth);
    }
    if (onLine)
        color = ViewsLineColor.rgb;
    if (ViewsLine.w > 0.0f)
        color = lerp(color, WholeTexture.SampleLevel(WholeSampler, input.Uv, 0.0f).rgb, saturate(ViewsLine.w));
    return float4(color, 1.0f);
}
