// World-space UI: a `BillboardGui` or a `SurfaceGui`'s quads, drawn in the
// world rather than over it.
//
// **The same quads `ui2d.hlsl` draws**, laid out by the same layout and built by
// the same draw list, with one difference that is the whole feature: the host
// has already placed each corner in the world, so this stage projects it like
// any other surface and it is hidden by whatever is in front of it. What comes
// out goes into the HDR target before tone mapping -- it is part of the world's
// picture, which is why it is not the screen pass the frame-generation
// constraint says must be composited last.
//
// A UI colour is written for a screen, in sRGB. Here it is converted to linear
// light and scaled by the gui's `Brightness`, so white at one is about as
// bright as a lit white wall and above one it blooms.

// **Lit as the surfaces are** (`LightInfluence`): the forward's own lighting,
// with the four slots a material's maps take left to this shader, and the
// light the screen bounces moved into one of them -- SDL_GPU binds a stage's
// samplers as one run from the first, so the run has no gap. The screen's
// reflection, which no blended surface reads, goes after the run, unused.
#define ENG_UNIFORMS_FRAME
#define ENG_SURFACE_WRAPPER
#define ENG_SCREEN_REFLECTION_REGISTERS t13, s13
#define ENG_SCREEN_BOUNCE_REGISTERS t3, s3
#include "engine_forward.hlsli"
#include "engine_ui.hlsli"
#include "engine_fullscreen.hlsli"
#include "engine_tonemap.hlsli"

cbuffer UiWorldView : register(b0, space1)
{
    column_major float4x4 ViewProjection;
};

// The frame's own uniforms are the first block (`GpuFrameUniforms`): the sun,
// its shadow's cascades, the sky's light -- for an interface lit as a surface.
cbuffer UiWorldLook : register(b1, space3)
{
    // x: brightness; y: the tonemap folded into the pass, its exposure; z
    // above a half: drawn as the display shows it (`FragmentMain`); w: the
    // exposure compensation's gain.
    float4 Look;
    // x: how much the scene's light decides the colours (`LightInfluence`).
    float4 LookLight;
};

Texture2D<float4> UiTexture : register(t0, space2);
SamplerState UiSampler : register(s0, space2);
// The frame's gradient table (ADR 0110), the screen's UI's own.
Texture2D<float4> GradientTable : register(t1, space2);
SamplerState GradientSampler : register(s1, space2);
// The exposure the frame before was drawn at: what the interface's own
// colours are worth in the scene.
Texture2D<float> ExposureTexture : register(t2, space2);
SamplerState ExposureSampler : register(s2, space2);

struct VertexInput
{
    // Camera-relative metres, as every f32 position the renderer draws is.
    float3 Position : TEXCOORD0;
    float4 Color : TEXCOORD1;
    // xy: the vertex's offset from its quad's centre; zw: the quad's
    // half-extent -- in canvas pixels, for the rounded corner.
    float4 LocalHalf : TEXCOORD2;
    float Radius : TEXCOORD3;
    float2 Uv : TEXCOORD4;
    // A gradient and a stroke (ADR 0110), as `ui2d.hlsl` reads them.
    float4 GradientFrame : TEXCOORD5;
    float4 GradientShape : TEXCOORD6;
    float4 GradientOffsetBand : TEXCOORD7;
    float StrokeJoin : TEXCOORD8;
};

struct UiInterpolants
{
    float4 Color : TEXCOORD0;
    float4 LocalHalf : TEXCOORD1;
    float Radius : TEXCOORD2;
    float2 Uv : TEXCOORD3;
    float4 GradientFrame : TEXCOORD4;
    nointerpolation float4 GradientShape : TEXCOORD5;
    nointerpolation float4 GradientOffsetBand : TEXCOORD6;
    nointerpolation float StrokeJoin : TEXCOORD7;
    // Camera-relative metres, and the distance along the camera's axis.
    float3 ShadingPosition : TEXCOORD8;
    float ViewDepth : TEXCOORD9;
    float4 Position : SV_Position;
};

UiInterpolants VertexMain(VertexInput input)
{
    UiInterpolants output;
    output.Position = mul(ViewProjection, float4(input.Position, 1.0f));
    output.ShadingPosition = input.Position;
    output.ViewDepth = output.Position.w;
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

float3 srgbToLinear(float3 color)
{
    return lerp(pow((color + 0.055f) / 1.055f, 2.4f), color / 12.92f, step(color, 0.04045f));
}

// A part's surface, unless a material says otherwise: dielectric and fairly
// rough (`render_world.cpp`'s plain part), so an interface lit as a surface
// reads as a part of its colours does.
static const float EngineUiRoughness = 0.7f;

// **The interface lit as the surface it is on** (`LightInfluence` 1): the
// plane's normal, turned to the camera, lit by what lights a part of these
// colours there -- the sun in the shadows things cast, the sky, the lamps,
// what the probes and the screen bounce -- and fogged as a part is.
float3 litAsSurface(float3 albedo, float3 position, float viewDepth, float2 pixel)
{
    float3 normal = normalize(cross(ddx(position), ddy(position)));
    if (dot(normal, position) > 0.0f)
        normal = -normal;
    const Surface surface = makeSurface(position, normal, albedo, 0.0f, EngineUiRoughness);
    EngineScreenFlatness = screenFlatness(normal, position);
    EngineGeometricNormal = normal;
    float3 color = lightSurface(surface, position, normal, viewDepth, pixel);
    if ((uint(ClusterParams.w) & EngineSkipFog) == 0u)
        color = applyFog(color, FogColor.rgb, FogRange, length(position));
    return color;
}

float4 FragmentMain(UiInterpolants input) : SV_Target0
{
    float4 color = input.Color * UiTexture.Sample(UiSampler, input.Uv);
    const float2 band = input.GradientOffsetBand.zw;
    if (band.y > band.x) {
        const float distance =
            uiStrokeDistance(input.LocalHalf.xy, input.LocalHalf.zw, input.Radius, input.StrokeJoin);
        color.a *= uiStrokeCoverage(distance, band.x, band.y);
    }
    else if (input.Radius > 0.0f) {
        const float distance = uiRoundedRectDistance(input.LocalHalf.xy, input.LocalHalf.zw, input.Radius);
        const float edge = fwidth(distance);
        color.a *= 1.0f - smoothstep(-edge, edge, distance);
    }
    color *= uiGradient(GradientTable, GradientSampler, input.GradientFrame.xy, input.GradientFrame.zw,
                        input.GradientShape.y, input.GradientShape.x, input.GradientShape.z, input.GradientShape.w,
                        input.GradientOffsetBand.xy);
    // Nothing to blend is nothing to write, which keeps a label's empty corners
    // from greying the depth-sorted surfaces behind them at the edges.
    clip(color.a - 1.0f / 255.0f);
    const float3 albedo = srgbToLinear(color.rgb);
    const float3 own = albedo * Look.x;
    // **As authored** (ADR 0250), with no light of the scene's and a
    // brightness of 1 or less: the colours the interface was given are the
    // ones the display shows -- not exposed, not through the curve -- and it
    // blends over the finished picture, into a headset eye's image where the
    // tonemap is folded into its pass, or into a layer of its own the tonemap
    // lays over the picture.
    float3 shown = saturate(own);
    if (Look.z <= 0.5f)
    {
        // **Otherwise it is light in the scene**, exposed and through the
        // curve: its own light -- above a brightness of 1 a glow that blooms,
        // at or below it what the display would have shown, worth that in the
        // scene at the exposure the frame is drawn at -- mixed with the
        // scene's light on a surface of its colours, by `LightInfluence`.
        float exposure = Look.y;
        if (exposure <= 0.0f)
        {
            const float measured =
                max(ExposureTexture.SampleLevel(ExposureSampler, float2(0.5f, 0.5f), 0.0f), 1e-4f);
            exposure = EngineExposureKey / measured * Look.w;
        }
        const float3 ownInScene = Look.x > 1.0f ? own : untonemapPbrNeutral(saturate(own)) / max(exposure, 1e-4f);
        const float influence = saturate(LookLight.x);
        float3 scene = ownInScene;
        [branch]
        if (influence > 0.0f)
        {
            // Its colours as a part takes them (`render_world.cpp`'s
            // `tintBy`): the numbers themselves, as the surface's albedo.
            const float3 lit = litAsSurface(saturate(color.rgb), input.ShadingPosition, input.ViewDepth,
                                            input.Position.xy);
            scene = lerp(ownInScene, lit, influence);
        }
        shown = foldOutput(float4(scene, 1.0f), Look.y, input.Position.xy).rgb;
    }
    // Premultiplied: the pipeline blends One and OneMinusSrcAlpha, into a
    // layer that is laid over the picture as it was blended.
    return float4(shown * color.a, color.a);
}
