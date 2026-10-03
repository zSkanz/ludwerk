// FSR 1's sharpening (ADR 0158): RCAS, robust contrast-adaptive sharpening,
// from AMD's portable header (third_party/fidelityfx_fsr1, MIT). After FSR 1's
// upscale, and after temporal anti-aliasing, which softens -- both into the
// target. `GraphicsService.Sharpness` is its strength.
//
// It reads the picture texel for texel, so it reads one of the size it
// writes; a picture smaller than the target is upscaled first.

#define ENG_UNIFORMS_RCAS
#include "engine_aa.hlsli"
#include "engine_fullscreen.hlsli"

Texture2D SourceTexture : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);

#define A_GPU 1
#define A_HLSL 1
#include "../../third_party/fidelityfx_fsr1/ffx-fsr/ffx_a.h"

// Through the sampler -- point and clamped -- at the texel's centre rather
// than by `Load`: a texture read without one is a storage texture to SDL_GPU,
// and this one is bound as a sampled texture like every other.
AF4 FsrRcasLoadF(ASU2 p)
{
    uint width;
    uint height;
    SourceTexture.GetDimensions(width, height);
    return SourceTexture.SampleLevel(SourceSampler, (float2(p) + 0.5f) / float2(width, height), 0.0f);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b)
{
}

#define FSR_RCAS_F 1
#include "../../third_party/fidelityfx_fsr1/ffx-fsr/ffx_fsr1.h"

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

float4 FragmentMain(Interpolants input) : SV_Target0
{
    AF1 r;
    AF1 g;
    AF1 b;
    // **The pixel from the coordinate rather than from `SV_Position`**: this
    // pass, the upscale and the temporal resolve each failed to make a
    // pipeline on Direct3D 12 while they read the position, and made one
    // once they did not (measured; why is not known -- the tonemap reads it
    // and is made). The picture read is the size of the one written.
    uint width;
    uint height;
    SourceTexture.GetDimensions(width, height);
    FsrRcasF(r, g, b, AU2(input.Uv * float2(width, height)), RcasCon);
    return float4(r, g, b, 1.0f);
}
