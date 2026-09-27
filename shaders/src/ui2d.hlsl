// The 2D pass: screen-space coloured quads, drawn over the world.
//
// `UICorner` is rounded HERE and nowhere else (D030): the geometry stays four
// vertices and the shape is a signed distance on the fragment, so a rounded
// panel costs the same draw as a square one. Layout and hit-testing do not see
// it, which is what `UICorner`'s own doc promises.
//
// Everything the UI draws is one of these. A `Frame`'s background is a quad, a
// `TextLabel`'s glyphs are quads sampling a glyph atlas, an `ImageLabel` is a
// quad sampling a picture, and a nine-slice patch is nine of them.
//
// ONE PIPELINE, and the texture is what makes that possible: an untextured quad
// samples a one-pixel white texture and multiplies by one. A textured pipeline
// beside an untextured one would be two pipelines, two sorts and a state change
// per element, to save a fetch that hits the same texel every time.
//
// Register spaces are SDL_GPU's, not a style choice: a vertex shader's uniform
// buffers live at b[n] in space1 (SDL_gpu.h, SDL_CreateGPUShader), which is why
// the projection is read here rather than in the fragment stage. Vertex inputs
// use TEXCOORDn because that is what SDL_shadercross maps to SPIR-V input
// locations; POSITION would compile and then bind nothing.

#include "engine_ui.hlsli"

cbuffer Ui2dProjection : register(b0, space1)
{
    // Pixels to clip space, as a scale and a bias rather than a matrix: the
    // transform is diagonal, and four floats say so where sixteen would hide it.
    //
    // x, y are 2/width and -2/height -- the sign is what flips the UI's y-down
    // convention onto the API's y-up clip space, and it is the ONE place that
    // conversion happens.
    float4 ScreenToClip;
};

Texture2D<float4> UiTexture : register(t0, space2);
SamplerState UiSampler : register(s0, space2);
// A row per gradient of the frame (ADR 0110), read by a quad that has one.
Texture2D<float4> GradientTable : register(t1, space2);
SamplerState GradientSampler : register(s1, space2);

struct VertexInput
{
    float2 Position : TEXCOORD0;
    float4 Color : TEXCOORD1;
    // xy: this vertex's offset from the quad's centre. zw: the quad's
    // half-extent. Together they let the fragment stage measure a corner
    // without knowing where on the screen the quad is.
    float4 LocalHalf : TEXCOORD2;
    float Radius : TEXCOORD3;
    float2 Uv : TEXCOORD4;
    // The gradient and the stroke (ADR 0110): where this vertex is in the
    // gradient's box and the box's half-size; the row, kind, angle and scale;
    // the offset and the stroke's band; the join.
    float4 GradientFrame : TEXCOORD5;
    float4 GradientShape : TEXCOORD6;
    float4 GradientOffsetBand : TEXCOORD7;
    float StrokeJoin : TEXCOORD8;
};

struct Interpolants
{
    float4 Color : TEXCOORD0;
    float4 LocalHalf : TEXCOORD1;
    float Radius : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    float4 GradientFrame : TEXCOORD4;
    nointerpolation float4 GradientShape : TEXCOORD5;
    nointerpolation float4 GradientOffsetBand : TEXCOORD6;
    nointerpolation float StrokeJoin : TEXCOORD7;
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    Interpolants output;
    output.Position = float4(input.Position.x * ScreenToClip.x + ScreenToClip.z,
                             input.Position.y * ScreenToClip.y + ScreenToClip.w, 0.0f, 1.0f);
    output.Color = input.Color;
    output.LocalHalf = input.LocalHalf;
    output.Radius = input.Radius;
    output.Uv = input.Uv;
    output.GradientFrame = input.GradientFrame;
    output.GradientShape = input.GradientShape;
    output.GradientOffsetBand = input.GradientOffsetBand;
    output.StrokeJoin = input.StrokeJoin;
    return output;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    // The tint TIMES the texture, including its alpha. A glyph atlas is
    // coverage in alpha and white in colour, so a label's colour comes from the
    // vertex; a picture carries its own colour, so a white tint leaves it alone.
    // One multiplication serves both, which is why there is one shader.
    float4 color = input.Color * UiTexture.Sample(UiSampler, input.Uv);

    // A stroke's band (ADR 0110): what lies between its inner and outer
    // distance from its element's edge, measured round the element's corner.
    const float2 band = input.GradientOffsetBand.zw;
    if (band.y > band.x) {
        const float distance =
            uiStrokeDistance(input.LocalHalf.xy, input.LocalHalf.zw, input.Radius, input.StrokeJoin);
        color.a *= uiStrokeCoverage(distance, band.x, band.y);
    }
    // A radius of zero is a square corner and the overwhelmingly common case, so
    // it costs one compare rather than a second pipeline.
    else if (input.Radius > 0.0f) {
        const float distance = uiRoundedRectDistance(input.LocalHalf.xy, input.LocalHalf.zw, input.Radius);
        // Antialiased over one pixel of the distance's own gradient rather than
        // clipped: a hard cut on a curve is a staircase, and `fwidth` is what
        // makes the edge one pixel wide whatever the UI is scaled to.
        const float edge = fwidth(distance);
        color.a *= 1.0f - smoothstep(-edge, edge, distance);
    }

    // The gradient multiplies the colour and the opacity (ADR 0110).
    color *= uiGradient(GradientTable, GradientSampler, input.GradientFrame.xy, input.GradientFrame.zw,
                        input.GradientShape.y, input.GradientShape.x, input.GradientShape.z, input.GradientShape.w,
                        input.GradientOffsetBand.xy);
    return color;
}
