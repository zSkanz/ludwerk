// The resolve: the `Rgba16Float` HDR target to the swapchain, with exposure.
// The first fullscreen pass in the engine and the first time the swapchain is
// not the only colour target.
//
// **The operator is Khronos PBR Neutral** (KhronosGroup/ToneMapping, the glTF
// viewer's default since 2024), and the reason is what the first renderer is for. This is the
// milestone where an imported glTF file first appears on screen, and every
// question asked of the result -- does this material look like the file says it
// looks, did the importer read baseColorFactor correctly, is the golden image
// showing a bug or a look -- is a question about hue. ACES's filmic fit answers
// it badly: it is cheap and it is the industry default, but it rotates
// saturated hues towards orange as they brighten, so a red that is wrong and a
// red that is merely bright arrive at the same pixel. PBR Neutral is the same
// order of cost, is monotone in luminance, and desaturates only in the
// highlight roll-off, which leaves a material's colour recognisable right up to
// the point it clips. Reinhard was rejected for the opposite reason: it never
// really reaches white, so every bright surface reads as washed out.
//
// **sRGB encoding happens here and nowhere else.** The swapchain is claimed with
// SDL_GPU's default SDR composition, which is a plain `B8G8R8A8_UNORM`
// (third_party/sdl3/src/gpu/vulkan/SDL_gpu_vulkan.c:245-246): the display wants
// sRGB-encoded bytes and that format applies no transfer function, so this
// shader applies it. Its counterpart, the decode, is the texture format's job --
// base colour and emissive are `Rgba8UnormSrgb` and the sampler linearises them;
// normal and metallic-roughness maps stay `Rgba8Unorm` because their contents
// were never colours. If the swapchain ever becomes `SDR_LINEAR`, the hardware
// starts encoding and `encodeSrgb` below must go, or it happens twice and the
// image turns pale.

#define ENG_UNIFORMS_TONEMAP
#include "engine_pbr.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D HdrTexture : register(t0, space2);
SamplerState HdrSampler : register(s0, space2);
// The bloom chain's finest level, already the sum of every coarser one.
Texture2D BloomTexture : register(t1, space2);
SamplerState BloomSampler : register(s1, space2);
// One texel: the frame's adapted average luminance (`luminance_adapt.hlsl`).
Texture2D<float> ExposureTexture : register(t2, space2);
SamplerState ExposureSampler : register(s2, space2);

#ifdef ENG_TONEMAP_EXACT
// **Which pixels are a sprite drawn in its own colours** (ADR 0153), one
// channel, written by `sprite_exact.hlsl`. Declared only in the twins that
// define the symbol -- `tonemap_exact.hlsl`, `tonemap_graded_exact.hlsl` --
// so a frame without such a sprite resolves through the bytes it always did.
Texture2D<float> ExactTexture : register(t3, space2);
SamplerState ExactSampler : register(s3, space2);
// **The world's interface as the display shows it** (ADR 0250): linear,
// premultiplied, over this pass's picture (`finishOutput`). Transparent where
// there is none.
Texture2D UiLayerTexture : register(t4, space2);
SamplerState UiLayerSampler : register(s4, space2);
#else
Texture2D UiLayerTexture : register(t3, space2);
SamplerState UiLayerSampler : register(s3, space2);
#endif

struct Interpolants
{
    float2 Uv : TEXCOORD0;
    float4 Position : SV_Position;
};

Interpolants VertexMain(uint vertexId : SV_VertexID)
{
    Interpolants output;
    // Depth 0: this pass has no depth attachment, and 0 is the near plane in the
    // engine's [0, 1] convention, so it is in front of whatever a future pass
    // might put behind it.
    fullscreenTriangle(vertexId, 0.0f, output.Position, output.Uv);
    return output;
}

#include "engine_tonemap.hlsli"

#ifdef ENG_TONEMAP_EXACT
// **A sprite's pixel is already a colour for a screen.** What the HDR target
// holds there is the picture decoded from sRGB, so encoding it again gives the
// bytes of the file: no exposure, no curve, no grade, no glow added, and no
// dither -- half a step of noise is the difference between the colour and its
// neighbour. Where a sprite half covers a pixel -- an anti-aliased edge, a
// transparent texel -- the two answers mix by how much of it is sprite.
//
// **Through a point sampler** (ADR 0158), the mask and the sprite's colour
// alike: a world drawn smaller than the window is scaled up by this pass, and
// pixel art is scaled by the nearest texel, never filtered. A one-to-one pass
// reads the same texel either way.
float4 resolveExact(float4 resolved, float2 uv)
{
    const float exact = ExactTexture.SampleLevel(ExactSampler, uv, 0.0f);
    const float3 hdr = HdrTexture.SampleLevel(ExactSampler, uv, 0.0f).rgb;
    return float4(lerp(resolved.rgb, encodeSrgb(saturate(hdr)), exact), resolved.a);
}
#endif

// **What this pass writes, from the picture it tonemapped**: the world's
// interface laid over it where it has one -- in linear light, as a blend into
// a headset's sRGB image would have -- and, into an sRGB image (w above a
// half: a headset's eye drawn straight into the runtime's image, which
// encodes what it is given), decoded after the dither, so the bytes that land
// are the ones a copy wrote. Where there is no interface, exactly what it
// always wrote.
float4 finishOutput(float4 written, float2 uv)
{
    const float4 layer = UiLayerTexture.SampleLevel(UiLayerSampler, uv, 0.0f);
    if (layer.a > 0.0f)
    {
        const float alpha = layer.a + written.a * (1.0f - layer.a);
        const float3 composed = (layer.rgb + decodeSrgb(written.rgb) * written.a * (1.0f - layer.a)) / alpha;
        return ExposureBloom.w > 0.5f ? float4(composed, alpha) : float4(encodeSrgb(composed), alpha);
    }
    return ExposureBloom.w > 0.5f ? float4(decodeSrgb(written.rgb), written.a) : written;
}

float4 FragmentMain(Interpolants input) : SV_Target0
{
    // SampleLevel because the HDR target has one mip and this pass is a 1:1
    // blit; an implicit derivative would only compute a level that is already
    // known to be zero.
    const float4 hdrTexel = HdrTexture.SampleLevel(HdrSampler, input.Uv, 0.0f);
    const float3 hdr = hdrTexel.rgb;
    // **The scene's coverage as alpha, for a view with no sky behind it**
    // (ADR 0107, `ViewportFrame`): z set, and what nothing drew stays clear.
    // Every other view is opaque, as it always was.
    const float alpha = ExposureBloom.z > 0.5f ? saturate(hdrTexel.a) : 1.0f;

    // Bloom is added in scene-referred light, BEFORE the curve. Adding it after
    // would put a glow on top of an already-compressed image, which is the look
    // of a screen-space filter rather than of light.
    const float3 bloom = BloomTexture.SampleLevel(BloomSampler, input.Uv, 0.0f).rgb;
    const float3 scene = hdr + bloom * ExposureBloom.y;

    // **Exposure is automatic, with the artist control on top.** The measured
    // value is the frame's adapted geometric-mean luminance; dividing by it maps
    // that mean onto the key below, which is what an exposure meter does. The
    // compensation is in EV stops, which is the unit a person who has used a
    // camera already knows -- and it is `Lighting.ExposureCompensation`.
    const float measured = max(ExposureTexture.SampleLevel(ExposureSampler, float2(0.5f, 0.5f), 0.0f), 1e-4f);
    const float exposure = (EngineExposureKey / measured) * exp2(ExposureBloom.x);
    const float3 exposed = max(scene * exposure, float3(0.0f, 0.0f, 0.0f));

#ifdef ENG_TONEMAP_EXACT
    const float4 written =
        resolveExact(float4(ditherOutput(encodeSrgb(tonemapPbrNeutral(exposed)), input.Position.xy), alpha), input.Uv);
#else
    const float4 written = float4(ditherOutput(encodeSrgb(tonemapPbrNeutral(exposed)), input.Position.xy), alpha);
#endif
    return finishOutput(written, input.Uv);
}
