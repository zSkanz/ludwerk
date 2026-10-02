// The resolve with a colour grade in it: `tonemap.hlsl`, plus every enabled
// `ColorCorrectionEffect` of the frame, a stage each, in order (ADR 0096,
// `render::resolveLook`, `render::applyGrade`).
//
// **A second pipeline rather than a branch in the first**, and the reason is
// every golden in the repository. A world with no colour correction must draw
// through exactly the shader it always drew through -- the same bytes, the same
// uniform block -- so this file includes that one whole, renames its entry
// point out of the way, and adds its own. The renderer makes this pipeline the
// first frame a grade exists, as it does the decals', so a world without one
// never builds it.
//
// The grade sits where the class's documentation says: on the exposed light,
// before the curve, so a highlight it brightens still rolls off rather than
// clipping.

#define FragmentMain tonemapUngradedFragment
// Through the include directory, because the compiler reads this file from
// memory and has no directory of its own to resolve a sibling against.
#include "../src/tonemap.hlsl"
#undef FragmentMain

// `render::GpuGradeUniforms`, 400 bytes, at the fragment stage's SECOND slot:
// the first is `GpuTonemapUniforms`, unchanged.
cbuffer GpuGradeUniforms : register(b1, space3)
{
    // Eight stages of three rows. A row's xyz is the mix of (r, g, b) -- tint,
    // then saturation; the first row's w is the stage's power (its contrast)
    // and the second's its lift (its brightness).
    float4 GradeRows[24];
    // x how many stages count.
    float4 GradeCount;
};

// Where contrast turns: the exposure's key (`render::GradePivot`).
static const float EngineGradePivot = 0.45f;

float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float4 hdrTexel = HdrTexture.SampleLevel(HdrSampler, input.Uv, 0.0f);
    const float3 hdr = hdrTexel.rgb;
    // **The scene's coverage as alpha, for a view with no sky behind it**
    // (ADR 0107, `ViewportFrame`): z set, and what nothing drew stays clear.
    // Every other view is opaque, as it always was.
    const float alpha = ExposureBloom.z > 0.5f ? saturate(hdrTexel.a) : 1.0f;
    const float3 bloom = BloomTexture.SampleLevel(BloomSampler, input.Uv, 0.0f).rgb;
    const float3 scene = hdr + bloom * ExposureBloom.y;

    const float measured = max(ExposureTexture.SampleLevel(ExposureSampler, float2(0.5f, 0.5f), 0.0f), 1e-4f);
    const float exposure = (EngineExposureKey / measured) * exp2(ExposureBloom.x);
    float3 graded = max(scene * exposure, float3(0.0f, 0.0f, 0.0f));

    // **A stage an effect, in order.** Contrast is a power about the pivot and
    // not a line through it (D427): a line took everything darker than a
    // third of the pivot below zero -- a night sky to black, grass to neon --
    // where a power darkens a channel without ever reaching black, and keeps
    // the three in the order they were. Nothing goes below zero at any step:
    // the curve has no answer for negative light.
    const int stages = (int)GradeCount.x;
    for (int stage = 0; stage < stages; ++stage)
    {
        const float4 red = GradeRows[stage * 3 + 0];
        const float4 green = GradeRows[stage * 3 + 1];
        const float4 blue = GradeRows[stage * 3 + 2];
        float3 mixed = max(float3(dot(red.xyz, graded), dot(green.xyz, graded), dot(blue.xyz, graded)),
                           float3(0.0f, 0.0f, 0.0f));
        const float power = red.w;
        if (power != 1.0f)
        {
            mixed = EngineGradePivot * pow(max(mixed, float3(1.0e-6f, 1.0e-6f, 1.0e-6f)) / EngineGradePivot,
                                           float3(power, power, power));
        }
        graded = max(mixed + green.w, float3(0.0f, 0.0f, 0.0f));
    }

#ifdef ENG_TONEMAP_EXACT
    return resolveExact(float4(ditherOutput(encodeSrgb(tonemapPbrNeutral(graded)), input.Position.xy), alpha), hdr,
                        input.Uv);
#else
    return float4(ditherOutput(encodeSrgb(tonemapPbrNeutral(graded)), input.Position.xy), alpha);
#endif
}
