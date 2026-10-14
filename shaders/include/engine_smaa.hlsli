// SMAA 1x, its authors' reference shader (third_party/smaa, MIT), wired to
// the engine's bindings (ADR 0158).
//
// `SMAA_CUSTOM_SL`: the reference's own HLSL port names two global samplers in
// the effect framework's syntax, which no compiler here reads -- and SDL_GPU
// pairs every texture with ITS sampler, slot for slot. So every sample a pass
// takes goes through the sampler declared beside the texture it reads,
// `<name>Sampler`; the reference's functions name their textures `colorTex`,
// `edgesTex`, `areaTex`, `searchTex` and `blendTex`, and a pass declares those
// it reads. Point or linear is the binding's: SMAA samples a texture point-wise
// only at texel centres, where the two are the same.
#ifndef ENG_SMAA_HLSLI
#define ENG_SMAA_HLSLI

#define ENG_UNIFORMS_SMAA
#include "engine_aa.hlsli"

#define SMAA_RT_METRICS SmaaMetrics
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(tex##Sampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(tex##Sampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(tex##Sampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(tex##Sampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(tex##Sampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(tex##Sampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(tex##Sampler, coord, 0)

// **Every function of the reference is compiled in every pass**, and each
// names the sampler of the texture it reads -- so a pass declares the textures
// it binds, with their samplers, and these stand for the rest. Never sampled
// by the pass that compiles them, they are not in its bindings.
#if !defined(ENG_SMAA_BINDS_COLOR)
SamplerState colorTexSampler;
#endif
#if !defined(ENG_SMAA_BINDS_EDGES)
SamplerState edgesTexSampler;
#endif
#if !defined(ENG_SMAA_BINDS_AREA)
SamplerState areaTexSampler;
#endif
#if !defined(ENG_SMAA_BINDS_SEARCH)
SamplerState searchTexSampler;
#endif
#if !defined(ENG_SMAA_BINDS_BLEND)
SamplerState blendTexSampler;
#endif
SamplerState velocityTexSampler;
SamplerState currentColorTexSampler;
SamplerState previousColorTexSampler;
SamplerState texSampler;

#include "../../third_party/smaa/SMAA.hlsl"

#include "engine_fullscreen.hlsli"

#endif // ENG_SMAA_HLSLI
