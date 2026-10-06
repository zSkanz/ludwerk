// The uniform layouts the renderer and its shaders both agree on (ADR 0006).
//
// This file and `shaders/include/engine_pbr.hlsli` describe the same bytes twice,
// in two languages, and nothing in the toolchain checks that they still match:
// SDL_GPU takes a uniform block as an opaque span and a shader that disagrees
// about the layout reads whatever happens to be there. The failure is not a
// build error, it is a scene lit by garbage.
//
// So the rule is: **change one, change the other, in the same commit**, and the
// `static_assert`s below are what turn a size mismatch into a compile error on
// this side at least.
//
// **SDL_GPU fixes the register spaces per stage** and a shader that ignores the
// contract binds nothing at all (SDL_gpu.h:2702-2730, read at the pinned
// version):
//
//   vertex   -- textures and samplers in `space0`, uniform buffers `b[n] space1`
//   fragment -- textures and samplers in `space2`, uniform buffers `b[n] space3`
//
// And vertex inputs use `TEXCOORDn` semantics, because that is what
// SDL_shadercross maps to SPIR-V input locations; `POSITION`/`NORMAL` compile
// and then bind nothing.
#pragma once

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/render/shadow.h"

namespace engine::render {

using core::f32;
using core::u32;

// **The shading space is camera-relative, and nothing in these blocks says so
// except this paragraph.** There is no camera position anywhere below, because
// the eye is at the origin: `extract` subtracts the camera's world position from
// every transform and every light before the snapshot is built (ADR 0014, the
// M4 brief's Decision 8). Two consequences a shader author cannot see from the
// struct definitions, and which silently produce a wrong image rather than an
// error if they are broken: `GpuObjectUniforms::model` must carry a
// camera-relative translation, and `sunViewProjection` must be built in the same
// space.
//
// The clustered pass replaced `kMaxForwardLights` at M7.5 (clusters.h). What is
// left of the old shape is `GpuLight` itself, which the shader now rebuilds from
// three texels of a light table instead of reading out of this block -- the
// frozen RHI has no storage buffer, so a fragment shader's only route to bulk
// data is a sampled texture.
//
// A light as the fragment shader reads it. Four-float rows because a constant
// buffer packs to 16 bytes and a struct that ignores that is a struct whose
// second element is at the wrong offset on some backend.
struct GpuLight
{
    // xyz position (camera-relative), w range.
    f32 positionRange[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // rgb colour premultiplied by brightness, w unused.
    f32 color[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // xyz spot direction, w cosine of the half angle. **-1 for a point light**,
    // which is the value that admits every direction and so costs no branch; 1
    // would be the narrowest cone expressible, which is the opposite.
    f32 directionCosAngle[4]{0.0f, -1.0f, 0.0f, -1.0f};
};

static_assert(sizeof(GpuLight) == 48, "GpuLight is a cbuffer layout; see engine_pbr.hlsli");

// Vertex stage, `b0 space1`.
struct GpuObjectUniforms
{
    core::Mat4 viewProjection;
    core::Mat4 model;
    // The cofactor matrix of `model`'s rotation-scale block, so normals survive
    // a non-uniform scale. Stored as a Mat4 rather than a Mat3 because a
    // float3x3 in a constant buffer is three float4 rows anyway, and spelling
    // that out is how the two sides stop disagreeing about the padding.
    core::Mat4 normalMatrix;
    // x is `1 - BasePart.Transparency` for this draw, and it is HERE rather than
    // in `GpuMaterialUniforms` for a reason the material block's own comment
    // gives: materials are deduplicated per frame precisely so the sort key can
    // group draws that share a bind set, and a per-instance alpha written there
    // would split one material into as many as there are distinct values.
    //
    // This block is per draw and already bound per draw, so carrying it costs
    // one more 16-byte row and no RHI call -- which is what keeps it possible
    // after ADR 0037 froze the interface.
    //
    // The fragment stage cannot read this block (SDL_GPU gives the vertex stage
    // space1 and the fragment stage space3), so the vertex shader passes it down
    // as an interpolant. That is not an optimization, it is the only route.
    f32 instanceAlphaUnused[4]{1.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuObjectUniforms) == 208, "GpuObjectUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`.
struct GpuFrameUniforms
{
    // xyz points from the world TOWARDS the sun, w is its brightness. The
    // brightness already carries the day factor (`SkyParams::dayFactor`), so a
    // sun below the horizon lights nothing and the shader needs no test for it.
    f32 sunDirectionBrightness[4]{0.0f, 1.0f, 0.0f, 2.0f};
    // rgb the sun's OWN colour, w unused. Derived from its elevation rather
    // than authored (environment.h), and new at M7.5: until then the sun cast
    // white light while the sky's disc was tinted, so a sunset lit a scene
    // exactly like noon did.
    f32 sunColorUnused[4]{1.0f, 0.96f, 0.9f, 0.0f};
    // `Lighting.Ambient`, the flat light of enclosed spaces, and below it
    // `Lighting.OutdoorAmbient`, that of open ones; a surface takes a blend by
    // how much sky it sees (ADR 0084). w unused in both.
    f32 ambient[4]{0.15f, 0.16f, 0.2f, 0.0f};
    f32 outdoorAmbient[4]{0.15f, 0.16f, 0.2f, 0.0f};
    f32 fogColor[4]{0.6f, 0.7f, 0.85f, 0.0f};
    // x start, y end, z one over (end - start) precomputed because a fragment
    // shader would otherwise divide per pixel, w unused. When fog is off
    // (`end <= start`) z is zero, which makes the fog factor zero without the
    // shader needing to know why.
    f32 fogRange[4]{200.0f, 0.0f, 0.0f, 0.0f};
    // x is how many lights the frame carries at all. The shader does not loop to
    // it -- it loops to its own cluster's count -- but a scene with none skips
    // the cluster lookup entirely.
    f32 lightCountUnused[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // x how many mip levels the prefiltered environment has, y how strongly it
    // contributes, z how strongly ambient occlusion darkens it. `y` and `z`
    // are multipliers rather than switches so that a scene can dial either
    // down without the shader gaining a branch nothing else needs. w is how
    // many taps the sun's shadow filter takes where a measurement asked for
    // fewer than its sixteen (ADR 0171), and zero otherwise.
    f32 environmentParams[4]{6.0f, 1.0f, 1.0f, 0.0f};
    // x and y are the exponential depth slicing's scale and bias, so a fragment
    // turns its own view depth into a cluster slice with one multiply-add and a
    // logarithm (clusters.h). z unused; w is `GraphicsSettings::measuredSkip`,
    // the lighting terms a measurement leaves out, and zero otherwise.
    f32 clusterParams[4]{};
    // x width in pixels, y height, z 1/width, w 1/height. The fragment stage
    // needs it to turn `SV_Position` into a cluster tile, and it is the first
    // time this block has carried anything about the target's size.
    f32 viewportParams[4]{};
    // Per cascade, one lane each: where the cascade stops in view-space metres,
    // one of its shadow texels in world metres, and its orthographic depth range
    // in metres. The last two are what let a filter radius and a depth bias be
    // stated in metres and mean the same thing in every cascade.
    f32 cascadeFar[4]{};
    f32 cascadeTexelWorld[4]{};
    f32 cascadeDepthRange[4]{};
    // x the filter radius in world metres, y the normal offset in shadow texels,
    // z the fraction of a cascade that blends into the next, w the residual
    // depth bias in world metres.
    f32 shadowParams[4]{};
    core::Mat4 cascadeViewProjection[4];
    // Irradiance as nine spherical-harmonic coefficients, cosine-convolved and
    // already divided by pi, so the shader multiplies the evaluation straight
    // by the diffuse albedo (environment.h). This is what replaces a flat
    // ambient on the diffuse lobe; `ambient` above is ADDED to it rather than
    // replaced by it, so `Lighting.Ambient` still means what it documents.
    f32 irradianceSh[9][4]{};
    // **One matrix per LOCAL shadow tile, not per light** (shadow.h). A spot
    // occupies one tile and a point occupies six, so sixteen matrices is the
    // whole atlas whatever mix of lights filled it -- and the fragment shader
    // indexes them with the same number the light table hands it, which is what
    // keeps the two from ever disagreeing about which tile belongs to whom.
    //
    // A kilobyte a frame. The alternative was packing them into the light data
    // texture beside the light rows, which would have cost four texture rows per
    // spot and twenty-four per point and put a matrix where nothing else is one.
    core::Mat4 localShadowViewProjection[kLocalShadowTileCount];
    // x how many tiles are live this frame, y one over the atlas resolution in
    // texels, z the depth bias in normalised units, w the filter radius in
    // texels. Zero live tiles is what a scene with no casting local light gets,
    // and the shader skips the whole lookup on it.
    f32 localShadowParams[4]{};
};

static_assert(sizeof(GpuFrameUniforms) == 224 + 256 + 144 + 64 * kLocalShadowTileCount + 16,
              "GpuFrameUniforms is a cbuffer layout; see engine_forward.hlsli");

// Fragment stage, `b1 space3`. Per material rather than per frame, because it
// changes with the bind set and the sort key already groups draws by material.
struct GpuMaterialUniforms
{
    // rgb base colour factor, w alpha.
    f32 baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    // rgb emissive factor, w unused.
    f32 emissive[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // x metallic, y roughness, z normal-map scale, w alpha cutoff.
    //
    // **The cutoff is zero unless the material is `Mask`** (D423): the shader
    // clips at it whatever the mode, so a block that began at glTF's default
    // of a half -- which the plain part's block did, being made from nothing
    // -- was an alpha test at 0.5 on a material nobody had called a mask. A
    // pane of glass at `Transparency` 0.55 was not drawn at all, and at
    // exactly 0.5 it was stripes. Whoever means a mask writes its cutoff.
    f32 metallicRoughnessNormalCutoff[4]{1.0f, 1.0f, 1.0f, 0.0f};
    // x is 1 when the material has a base-colour texture, y normal, z
    // metallic-roughness, w emissive. Floats rather than a bitmask because a
    // shader multiplies by them and a branch per texture per fragment is worse
    // than a multiply by one.
    //
    // **Because they are multipliers rather than branches, the sample happens
    // either way** -- so every one of the five fragment texture slots must have
    // something bound, and a material with no base-colour map gets a 1x1
    // default rather than an invalid handle. An unbound descriptor read is not
    // a black pixel, it is whatever the backend last left there.
    f32 textureFlags[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuMaterialUniforms) == 64, "GpuMaterialUniforms is a cbuffer layout");

// One entry of the per-INSTANCE vertex stream (ADR 0043), at slot 1.
//
// The model matrix as four columns, the instance's own alpha and its own base
// colour, and NOT the cofactor normal matrix: that would be four more `float4`
// attributes, and a vertex layout is capped at sixteen on the weakest
// conforming device. The instanced shaders derive it with three cross products
// per vertex instead, which is the cheaper half of the trade.
struct GpuInstance
{
    core::Mat4 model;
    // x is `1 - BasePart.Transparency`, for the same reason
    // `GpuObjectUniforms` carries it: a per-instance alpha written into the
    // material block would split one material into as many as there are values.
    //
    // **yzw is the instance's base colour** (D184), for the same reason again:
    // parts that differ only by `Color` were each their own material, so a
    // snake of two hundred differently tinted segments was two hundred draws.
    // A run now shares a material FAMILY (`RenderWorld::materialFamilies`) and
    // each member brings its own colour in the three floats this already had
    // spare -- the stride and the pipelines did not change.
    f32 alphaTint[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

static_assert(sizeof(GpuInstance) == 80, "GpuInstance is a vertex stride; see pbr_instanced.hlsl");

// One entry of a SKINNED run's per-instance stream (H2), at slot 2: what
// `GpuInstance` carries, and where in the frame's palette buffer this
// instance's joints begin -- a float, because the vertex formats have no
// integer one (ADR 0037), exact to sixteen million joints.
struct GpuSkinnedInstance
{
    core::Mat4 model;
    f32 alphaTint[4]{1.0f, 1.0f, 1.0f, 1.0f};
    f32 palette[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuSkinnedInstance) == 96,
              "GpuSkinnedInstance is a vertex stride; see pbr_skinned_instanced.hlsl");

// Vertex stage, `b0 space1`, for the shadow pass. Depth only, so there is
// nothing else it needs.
struct GpuShadowUniforms
{
    core::Mat4 lightViewProjection;
    core::Mat4 model;
};

static_assert(sizeof(GpuShadowUniforms) == 128, "GpuShadowUniforms is a cbuffer layout");

// The most joints one skinned draw can be posed by. A budget rather than a limit
// of the design, like `kMaxForwardLights`: the palette is a constant buffer and
// a constant buffer has a size, so a rig with more joints than this draws in
// bind pose for the ones past it. Sixty-four is generous for a humanoid -- a
// game character is typically thirty to fifty -- and 4 KB is a comfortable push
// on every backend. Raising it is one number here and one in `engine_pbr.hlsli`.
inline constexpr u32 kMaxSkinJoints = 64;

// Vertex stage, `b1 space1`, for both skinned passes. `joint * inverseBind`
// already combined, because that product is what a vertex multiplies by and
// sending the two halves separately would send twice the bytes to do the same
// multiply on the GPU.
struct GpuSkinUniforms
{
    core::Mat4 jointMatrices[kMaxSkinJoints];
};

static_assert(sizeof(GpuSkinUniforms) == 64 * 64, "GpuSkinUniforms is a cbuffer layout; see engine_pbr.hlsli");

// Fragment stage, `b0 space3`, for the tonemap pass.
struct GpuTonemapUniforms
{
    // x exposure compensation in EV stops -- the artist control, on top of the
    // automatic exposure the 1x1 target carries. y how much bloom is mixed in.
    // z above one half: the output's alpha is the scene's coverage rather than
    // one (a view with no sky behind it, ADR 0107). w unused.
    f32 exposureBloom[4]{0.0f, 0.04f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuTonemapUniforms) == 16, "GpuTonemapUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the screen-space ambient occlusion pass.
//
// It reads the depth the PREPASS wrote and nothing else: a forward renderer that
// grew a normal target would have grown half a deferred one, so the normal is
// reconstructed from depth derivatives (M7.5 brief, Decision 13).
struct GpuSsaoUniforms
{
    // x tan of half the horizontal field of view, y vertical, z near plane,
    // w far plane -- everything needed to turn a depth sample back into a
    // view-space position.
    f32 projection[4]{1.0f, 0.5f, 0.1f, 400.0f};
    // x width in pixels, y height, z 1/width, w 1/height, of the AO target.
    f32 viewport[4]{};
    // x the sampling radius in world metres, y the depth bias that keeps a
    // surface from occluding itself, z the strength, w unused.
    f32 params[4]{0.5f, 0.02f, 1.0f, 0.0f};
};

static_assert(sizeof(GpuSsaoUniforms) == 48, "GpuSsaoUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the depth-aware blur that follows it.
struct GpuBlurUniforms
{
    // x and y are one texel of the source, z and w the blur direction in texels
    // -- so one pipeline serves both the horizontal and the vertical pass.
    f32 texelDirection[4]{};
};

static_assert(sizeof(GpuBlurUniforms) == 16, "GpuBlurUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the editor's selection outline.
struct GpuOutlineUniforms
{
    // x and y are one mask texel, z the outline's half-width in texels, w
    // unused. The width scales the tap step rather than the tap count, so a
    // thicker line costs nothing more.
    f32 texelWidth[4]{};
    // The line. Alpha is how opaque it is over the frame.
    f32 color[4]{};
    // The tint over the selected shape, and its alpha is how much of it there
    // is. Small on purpose -- see the shader.
    f32 fillColor[4]{};
};

static_assert(sizeof(GpuOutlineUniforms) == 48, "GpuOutlineUniforms is a cbuffer layout");

// `tonemap_graded.hlsl`'s second block (ADR 0096): every enabled
// `ColorCorrectionEffect` composed into one affine map of exposed linear colour,
// as three rows -- `out.r = dot(rows[0], (r, g, b, 1))`. At the fragment stage's
// slot 1, so the plain tonemap's block at slot 0 is left exactly as it was.
//
// **Stages, since contrast is a power** (D427): eight of three rows each --
// a row's xyz the mix of (r, g, b), and its w the stage's power (first row)
// and its lift (second) -- and how many of them count.
struct GpuGradeUniforms
{
    f32 rows[8][3][4]{};
    // x how many stages; yzw unused.
    f32 count[4]{};
};
static_assert(sizeof(GpuGradeUniforms) == 400, "GpuGradeUniforms is mirrored by tonemap_graded.hlsl");

// `look_blur.hlsl`'s block (ADR 0096): one direction of `BlurEffect`'s
// separable Gaussian.
struct GpuLookBlurUniforms
{
    // xy one source texel along this pass's direction, z the standard deviation
    // in texels, w how many texels either side the kernel reaches.
    f32 stepSigma[4]{};
    f32 reserved[4]{};
};
static_assert(sizeof(GpuLookBlurUniforms) == 32, "GpuLookBlurUniforms is mirrored by look_blur.hlsl");

// `engine_look.hlsli`'s focus block (ADR 0096), shared by depth of field's three
// passes.
struct GpuLookFocusUniforms
{
    // The sharpest distance, how far either side stays sharp, the near side's
    // softness and the far side's.
    f32 band[4]{};
    // Near plane, far plane, the widest circle in half-resolution texels, and
    // the same in full-resolution pixels.
    f32 lens[4]{};
    // One full-resolution texel, then one half-resolution texel.
    f32 texel[4]{};
};
static_assert(sizeof(GpuLookFocusUniforms) == 48, "GpuLookFocusUniforms is mirrored by engine_look.hlsli");

// `engine_look.hlsli`'s rays block (ADR 0096), shared by sun rays' two passes.
struct GpuLookRaysUniforms
{
    // Where the sun is on the screen in texture space, how present it is with
    // the intensity folded in, and the screen's width over its height.
    f32 sun[4]{};
    // How much of the way to the sun the gather reaches, how many taps, and how
    // much each counts less than the last.
    f32 gather[4]{};
};
static_assert(sizeof(GpuLookRaysUniforms) == 32, "GpuLookRaysUniforms is mirrored by engine_look.hlsli");

// `engine_look.hlsli`'s air block (ADR 0096), for `look_air.hlsl`.
struct GpuLookAirUniforms
{
    core::Mat4 inverseViewProjection;
    // `render::AirMedium`: extinction, falloff, the camera's height above
    // `Offset`, and haze.
    f32 density[4]{};
    // The air's own light, and how far a ray into the sky is taken to go.
    f32 light[4]{};
    // The glare's light towards the sun.
    f32 glare[4]{};
    // Towards the sun, and the glare lobe's tightness.
    f32 sun[4]{};
};
static_assert(sizeof(GpuLookAirUniforms) == 128, "GpuLookAirUniforms is mirrored by engine_look.hlsli");

// `sky_look.hlsl`'s second block (ADR 0096), beside `GpuSkyUniforms`.
struct GpuLookSkyUniforms
{
    // Pictures drawn, sun/moon/stars drawn, sun a picture, moon a picture.
    f32 flags[4]{};
    // Towards the moon, and its angular radius in radians.
    f32 moon[4]{};
    // The moon's light, and how much of it shows.
    f32 moonColor[4]{};
    // Star cells per cube face, the chance a cell holds one, how much shows.
    f32 stars[4]{};
    // Cloud cover, density, and the wind's offset (Stage 9).
    f32 clouds[4]{};
    // The clouds' lit colour.
    f32 cloudColor[4]{1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(GpuLookSkyUniforms) == 96, "GpuLookSkyUniforms is mirrored by engine_look.hlsli");

// Fragment stage, `b0 space3`, shared by the bloom chain's two pipelines.
struct GpuBloomUniforms
{
    // x and y are one texel of the SOURCE, z the filter radius in source texels,
    // w unused.
    f32 texelRadius[4]{};
    // x the threshold in scene-referred luminance, y the soft knee's width, and
    // both are zero on every pass but the first: the threshold is applied once,
    // on the way into the chain, or a surface pops as it crosses it at every
    // level. z and w unused.
    f32 threshold[4]{};
};

static_assert(sizeof(GpuBloomUniforms) == 32, "GpuBloomUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the three passes that measure the frame's
// brightness and adapt to it.
struct GpuLuminanceUniforms
{
    // x and y are one texel of the source, z how far towards the measured value
    // one frame moves, w unused.
    //
    // **`z` is per FRAME rather than per second**, and that is deliberate: a
    // rate driven by elapsed wall-clock time would make a screenshot at frame
    // thirty a different picture on a fast machine and a slow one, and the
    // goldens are the reason that matters (R10 in spirit).
    f32 texelRate[4]{};
    // x the lowest and y the highest average luminance the automatic exposure
    // will accept.
    //
    // **The lower bound is what keeps midnight from looking like noon**, and it
    // is the whole reason this is a clamp rather than a guard. A meter with no
    // floor exposes a night scene up until it matches a day one, which removes
    // the day/night cycle the automatic exposure was added to serve -- the exact
    // failure the day strip showed on its first run. Against a key of 0.45 this
    // allows a gain between 0.15 and 3, so a night scene lands about a stop and
    // a half below a day one instead of level with it.
    f32 range[4]{0.15f, 3.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuLuminanceUniforms) == 32, "GpuLuminanceUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the anti-aliasing resolve.
struct GpuFxaaUniforms
{
    // x and y are one texel of the source; z and w unused.
    f32 texel[4]{};
};

static_assert(sizeof(GpuFxaaUniforms) == 16, "GpuFxaaUniforms is a cbuffer layout");

// --- Anti-aliasing and upscaling (ADR 0158), `engine_aa.hlsli` -----------------

// Fragment `b0 space3`: SMAA's `SMAA_RT_METRICS` -- one texel, and the size,
// of the picture it reads.
struct GpuSmaaUniforms
{
    f32 metrics[4]{};
};
static_assert(sizeof(GpuSmaaUniforms) == 16, "GpuSmaaUniforms is a cbuffer layout");

// Fragment `b0 space3`: `FsrEasuCon`'s four constants, float bits as words.
struct GpuEasuUniforms
{
    u32 con[4][4]{};
};
static_assert(sizeof(GpuEasuUniforms) == 64, "GpuEasuUniforms is a cbuffer layout");

// Fragment `b0 space3`: `FsrRcasCon`'s constant.
struct GpuRcasUniforms
{
    u32 con[4]{};
};
static_assert(sizeof(GpuRcasUniforms) == 16, "GpuRcasUniforms is a cbuffer layout");

// Fragment `b0 space3`: the camera's motion, for `taa_velocity.hlsl`.
struct GpuReprojectUniforms
{
    core::Mat4 inverse;
    core::Mat4 previous;
    f32 jitter[4]{};
};
static_assert(sizeof(GpuReprojectUniforms) == 144, "GpuReprojectUniforms is a cbuffer layout");

// Vertex `b0 space1`: one moving draw, now jittered, now and a frame ago.
// The jittered place is the two matrices the depth pass is given, multiplied
// where it multiplies them: its depth is what this draw is tested against
// (D548).
struct GpuMotionUniforms
{
    core::Mat4 viewProjection;
    core::Mat4 model;
    core::Mat4 current;
    core::Mat4 previous;
};
static_assert(sizeof(GpuMotionUniforms) == 256, "GpuMotionUniforms is a cbuffer layout");

// Fragment `b0 space3`: the temporal resolve.
struct GpuTaaUniforms
{
    f32 texel[4]{};
    f32 blend[4]{};
    f32 jitter[4]{};
};
static_assert(sizeof(GpuTaaUniforms) == 48, "GpuTaaUniforms is a cbuffer layout");

// Compute `b0 space2`: FSR 2's constants (ADR 0164) -- `cbFSR2` in the
// callbacks header, field for field, which is `Fsr2Constants` in AMD's own
// runtime.
struct GpuFsr2Constants
{
    core::i32 renderSize[2]{};
    core::i32 maxRenderSize[2]{};
    core::i32 displaySize[2]{};
    core::i32 inputColorResourceDimensions[2]{};
    core::i32 lumaMipDimensions[2]{};
    core::i32 lumaMipLevelToUse = 0;
    core::i32 frameIndex = 0;
    f32 deviceToViewDepth[4]{};
    f32 jitterOffset[2]{};
    f32 motionVectorScale[2]{};
    f32 downscaleFactor[2]{};
    f32 motionVectorJitterCancellation[2]{};
    f32 preExposure = 1.0f;
    f32 previousFramePreExposure = 1.0f;
    f32 tanHalfFov = 0.0f;
    f32 jitterPhaseCount = 0.0f;
    f32 deltaTime = 0.0f;
    f32 dynamicResChangeFactor = 0.0f;
    f32 viewSpaceToMetersFactor = 1.0f;
    f32 pad = 0.0f;
};
static_assert(sizeof(GpuFsr2Constants) == 128, "GpuFsr2Constants is a cbuffer layout");

// Compute `b1 space2`: how much FSR 2's own RCAS sharpens.
struct GpuFsr2RcasConstants
{
    core::u32 config[4]{};
};
static_assert(sizeof(GpuFsr2RcasConstants) == 16, "GpuFsr2RcasConstants is a cbuffer layout");

// Compute `b0 space2`: optical flow's constants (ADR 0165) -- `cbOF` in the
// callbacks header, field for field.
struct GpuOpticalFlowConstants
{
    core::i32 inputLumaResolution[2]{};
    core::u32 pyramidLevel = 0;
    core::u32 pyramidLevelCount = 0;
    core::u32 frameIndex = 0;
    core::u32 backbufferTransferFunction = 0;
    f32 minMaxLuminance[2]{};
};
static_assert(sizeof(GpuOpticalFlowConstants) == 32, "GpuOpticalFlowConstants is a cbuffer layout");

// Compute `b0 space2`: frame interpolation's constants (ADR 0165) -- `cbFI`
// in the callbacks header, field for field, which is
// `FrameInterpolationConstants` in AMD's own runtime.
struct GpuFrameInterpolationConstants
{
    core::i32 renderSize[2]{};
    core::i32 displaySize[2]{};
    f32 displaySizeRcp[2]{};
    f32 cameraNear = 0.0f;
    f32 cameraFar = 0.0f;
    core::i32 upscalerTargetSize[2]{};
    core::i32 mode = 0;
    core::i32 reset = 0;
    f32 deviceToViewDepth[4]{};
    f32 deltaTime = 0.0f;
    core::i32 hudLessAttachedFactor = 0;
    core::i32 distortionFieldSize[2]{1, 1};
    f32 opticalFlowScale[2]{};
    core::i32 opticalFlowBlockSize = 8;
    core::u32 dispatchFlags = 0;
    core::i32 maxRenderSize[2]{};
    core::i32 opticalFlowHalfResMode = 0;
    core::i32 numInstances = 0;
    core::i32 interpolationRectBase[2]{};
    core::i32 interpolationRectSize[2]{};
    f32 debugBarColor[3]{};
    core::u32 backBufferTransferFunction = 0;
    f32 minMaxLuminance[2]{};
    f32 tanHalfFov = 0.0f;
    core::i32 pad = 0;
    f32 jitter[2]{};
    f32 motionVectorScale[2]{};
};
static_assert(sizeof(GpuFrameInterpolationConstants) == 176, "GpuFrameInterpolationConstants is a cbuffer layout");

// Compute `b0 space2`: a level of an inpainting pyramid from the one before
// (`fsr3_fi_pyramid_next.hlsl`).
struct GpuPyramidConstants
{
    core::i32 sourceSize[2]{};
    core::i32 colours = 0;
    core::i32 pad = 0;
};
static_assert(sizeof(GpuPyramidConstants) == 16, "GpuPyramidConstants is a cbuffer layout");

// Fragment stage, `b0 space3`, for the sky pass. The sky is drawn as a
// fullscreen triangle before any geometry, so it needs the inverse view
// projection to turn a screen position back into a direction.
struct GpuSkyUniforms
{
    core::Mat4 inverseViewProjection;
    f32 sunDirectionSize[4]{0.0f, 1.0f, 0.0f, 0.02f};
    f32 horizonColor[4]{0.6f, 0.7f, 0.85f, 0.0f};
    f32 zenithColor[4]{0.15f, 0.3f, 0.7f, 0.0f};
    f32 sunColor[4]{1.0f, 0.95f, 0.85f, 0.0f};
};

static_assert(sizeof(GpuSkyUniforms) == 64 + 64, "GpuSkyUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for the contact-shadow pass.
struct GpuContactUniforms
{
    // x tan(fovX / 2), y tan(fovY / 2), z near, w far.
    f32 projection[4]{1.0f, 0.5f, 0.1f, 400.0f};
    // xyz towards the sun in view space, w the ray length in metres.
    f32 sun[4]{0.0f, 1.0f, 0.0f, 0.6f};
    // x thickness, y strength, z fade distance, w enabled.
    f32 params[4]{0.3f, 1.0f, 60.0f, 1.0f};
};

static_assert(sizeof(GpuContactUniforms) == 48, "GpuContactUniforms is a cbuffer layout");

// --- Terrain (ADR 0082) ------------------------------------------------------

// One entry per material id a terrain can hold: 0 (air, unused) to 255.
inline constexpr u32 kTerrainLayerSlots = 256;
// The rules a terrain paints with (ADR 0113 §2), at most.
inline constexpr u32 kTerrainRuleSlots = 16;

// `b1` of the fragment stage, for `terrain` (ADR 0113): each layer's look,
// by material id, for one terrain.
// **One terrain layer, as the fragment stage reads it**: a storage buffer of
// `kTerrainLayerSlots` of these at `t16 space2`, after the terrain's sixteen
// textures, indexed by material id (0 is air and unused).
//
// **A storage buffer and not the uniform block they were in** (D379): SDL_GPU
// binds a uniform block to Vulkan with a range of 4 KiB
// (`MAX_UBO_SECTION_SIZE`), and three of these rows by 256 layers are 12 KiB.
// On Vulkan -- Linux, Android -- everything past the first 4 KiB of the block
// was read as nothing: no layer drew its textures, and the terrain's params,
// past the cut, said it had none.
struct GpuTerrainLayer
{
    // What the layer is before its textures arrive: its colour, flat; in a,
    // how hard its paint meets what is under it (`BlendSharpness`, ADR 0114).
    f32 flat[4]{};
    // The material's colour factor, and one over its repeat in metres.
    f32 tint[4]{};
    // Roughness factor, metalness factor, normal scale, and flags: 1
    // triplanar, 2 a height map.
    f32 surface[4]{};
    // How the repeat is broken up (ADR 0113's amendment): the colour's
    // large-scale variation, the second sample's scale against the first (1
    // none), 1 for hex tiling, and a spare.
    f32 tiling[4]{0.0f, 1.0f, 0.0f, 0.0f};
    // The light the layer gives off (`Material.Emissive`), added to the lit
    // ground as a mesh's is; a spare.
    f32 emissive[4]{};
};

static_assert(sizeof(GpuTerrainLayer) == 80, "GpuTerrainLayer is a structured buffer's stride");

// Fragment stage, `b1 space3`, for `terrain`.
struct GpuTerrainSurfaceUniforms
{
    // Each rule's slope band and height band (`asset::TerrainRuleShape`), its
    // layer, noise, height jitter and whether it is on, and the layers it
    // covers as 256 bits.
    f32 ruleSlope[kTerrainRuleSlots][4]{};
    f32 ruleHeight[kTerrainRuleSlots][4]{};
    f32 ruleMisc[kTerrainRuleSlots][4]{};
    u32 ruleApplies[kTerrainRuleSlots][8]{};
    // 1 when the arrays hold every layer; the rule count; the layer count; the
    // terrain's height in the world.
    f32 params[4]{};
    // x: the debug view drawn instead of the ground (`DebugView`, terrain
    // audit T0), zero for none. y: 1 for the lean ground (ADR 0175). z: never
    // set; the shader names its unread slots behind it. w: the slope under
    // which no rule covers anything, whatever its noise: ground the fast
    // ground asks no rule about (ADR 0179).
    f32 debug[4]{};
};

static_assert(sizeof(GpuTerrainSurfaceUniforms) == 3 * 16 * 16 + 16 * 32 + 16 + 16,
              "GpuTerrainSurfaceUniforms is a cbuffer layout");

// **The most a uniform block may be** (D379): SDL_GPU binds one to Vulkan with
// a range of 4 KiB, and a shader reads past it as nothing.
inline constexpr core::usize kMaxUniformBlockBytes = 4096;
static_assert(sizeof(GpuFrameUniforms) <= kMaxUniformBlockBytes);
static_assert(sizeof(GpuSkinUniforms) <= kMaxUniformBlockBytes);
static_assert(sizeof(GpuTerrainSurfaceUniforms) <= kMaxUniformBlockBytes);

// Vertex stage, `b0 space1`, for `decal` (F2).
struct GpuDecalUniforms
{
    core::Mat4 boxToWorld;
    core::Mat4 viewProjection;
};

static_assert(sizeof(GpuDecalUniforms) == 128, "GpuDecalUniforms is a cbuffer layout");

// Vertex stage, `b0 space1`, for `ui_world` (F3).
struct GpuWorldUiView
{
    core::Mat4 viewProjection;
};

static_assert(sizeof(GpuWorldUiView) == 64, "GpuWorldUiView is a cbuffer layout");

// Vertex stage, `b0 space1`, for `sprite_exact` (ADR 0153, 0158): the view, and
// the same view without the temporal pass's jitter -- where a sprite drawn in
// its own colours is placed, so pixel art never moves by a fraction of one.
struct GpuSpriteExactView
{
    core::Mat4 viewProjection;
    core::Mat4 unjitteredViewProjection;
};

static_assert(sizeof(GpuSpriteExactView) == 128, "GpuSpriteExactView is a cbuffer layout");

// Fragment stage, `b0 space3`, for `ui_world`: x is the brightness.
struct GpuWorldUiLook
{
    f32 params[4]{1.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuWorldUiLook) == 16, "GpuWorldUiLook is a cbuffer layout");

// Fragment stage, `b0 space3`, for `decal`.
struct GpuDecalFragment
{
    core::Mat4 worldToBox;
    core::Mat4 inverseViewProjection;
    f32 color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    // xy: 1 / render size; z: 1 when there is an image; w: the blend mode
    // (ADR 0160) -- 0 Multiply, 1 Alpha, 2 Additive.
    f32 params[4]{};
    f32 axis[4]{0.0f, 0.0f, 1.0f, 0.0f};
    // What lights an Alpha decal -- the ambient and the sun, as a particle is
    // lit -- and, in the fourth, how brightly it glows.
    f32 light[4]{1.0f, 1.0f, 1.0f, 0.0f};
};

static_assert(sizeof(GpuDecalFragment) == 192, "GpuDecalFragment is a cbuffer layout");

// One particle, as `particle.hlsl` reads its instance stream (F2).
struct GpuParticle
{
    // xyz camera-relative position, w width in metres.
    f32 positionSize[4]{};
    // Linear colour times brightness, and opacity.
    f32 color[4]{};
    // x emission, y shape, z rotation in radians, w 1 when it is drawn from
    // its picture (ADR 0160).
    f32 params[4]{};
    // The picture's frame: left, top, right, bottom in texture space.
    f32 uv[4]{0.0f, 0.0f, 1.0f, 1.0f};
};

static_assert(sizeof(GpuParticle) == 64, "GpuParticle is a vertex stride; see particle.hlsl");

// One particle simulated on the GPU, as `particle_sim.hlsl` keeps it in its
// buffer and `particle_gpu.hlsl` reads it (ADR 0160).
struct GpuSimParticle
{
    f32 position[3]{};
    f32 age = 0.0f;
    f32 velocity[3]{};
    // Zero for a slot never born.
    f32 lifetime = 0.0f;
    f32 rotation = 0.0f;
    f32 spin = 0.0f;
    f32 frame = 0.0f;
    f32 stuck = 0.0f;
};

static_assert(sizeof(GpuSimParticle) == 48, "GpuSimParticle is a structured buffer's stride");

// Compute `b0 space2`, for `particle_sim`: one emitter's step.
struct GpuParticleSim
{
    core::Mat4 prevViewProjection;
    core::Mat4 prevInverseViewProjection;
    f32 emitterPlace[4]{};
    f32 emitterUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    f32 emitterSide[4]{1.0f, 0.0f, 0.0f, 0.0f};
    f32 emitterExtent[4]{};
    f32 acceleration[4]{};
    f32 wind[4]{};
    f32 turn[4]{};
    f32 collide[4]{};
    f32 touch[4]{};
    f32 originFromCamera[4]{};
    f32 depthParams[4]{};
    f32 ground[4]{};
    core::u32 spawn[4]{};
    core::u32 frame[4]{};
};

static_assert(sizeof(GpuParticleSim) == 352, "GpuParticleSim is a cbuffer layout");

// Vertex `b1 space1`, for `particle_gpu`: how one emitter's particles look.
struct GpuParticleLook
{
    f32 originEmission[4]{};
    f32 flipbook[4]{1.0f, 1.0f, 0.0f, 0.0f};
    f32 kind[4]{};
    f32 colorOverLife[16][4]{};
    f32 sizeOverLife[4][4]{};
};

static_assert(sizeof(GpuParticleLook) == 368, "GpuParticleLook is a cbuffer layout");

// One sprite (the 2D layer), per instance, for `sprite`.
struct GpuSprite
{
    // Camera-relative low x, low y, high x, high y.
    f32 rect[4]{};
    // x cosine, y sine, z the plane's depth, w `Enum.Shape2D`.
    f32 turn[4]{1.0f, 0.0f, 0.0f, 0.0f};
    // Left, top, right, bottom in texture space.
    f32 uv[4]{0.0f, 0.0f, 1.0f, 1.0f};
    // sRGB colour and opacity.
    f32 color[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

static_assert(sizeof(GpuSprite) == 64, "GpuSprite is a vertex stride; see sprite.hlsl");

// Vertex stage, `b0 space1`, for `particle`.
struct GpuParticleUniforms
{
    core::Mat4 viewProjection;
    f32 cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    f32 cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuParticleUniforms) == 96, "GpuParticleUniforms is a cbuffer layout");

// Fragment stage, `b0 space3`, for `particle`.
struct GpuParticleLighting
{
    f32 ambient[4]{};
    f32 sunLight[4]{};
    f32 fogColor[4]{};
    f32 fogRange[4]{};
    // Near plane, far plane, and one over the target's width and height: what
    // the shader needs to read the scene's depth and make it linear.
    f32 depth[4]{};
};

static_assert(sizeof(GpuParticleLighting) == 80, "GpuParticleLighting is a cbuffer layout");

// How many block types the block shader has colours for; ids past it wrap.
inline constexpr u32 kVoxelPaletteSize = 256;

// **One block type, as the vertex stage reads it**, by id minus one: a storage
// buffer of `kVoxelPaletteSize` of these at `t0 space0`, for `voxel` and
// `voxel_shadow`.
//
// **A storage buffer and not the uniform block it was** (D380): SDL_GPU binds a
// uniform block to Vulkan 4 KiB at a time, and the palette was 16 KiB -- on
// Linux and Android every block's sides and underside drew black, with none of
// its images, because only the tops were in the first 4 KiB.
struct GpuVoxelBlock
{
    f32 top[4]{};
    f32 side[4]{};
    f32 bottom[4]{};
    // The atlas tile of each face's image -- top, sides, bottom -- or -1; and
    // in the fourth, the alpha a translucent type draws at.
    f32 tiles[4]{};
};

static_assert(sizeof(GpuVoxelBlock) == 64, "GpuVoxelBlock is a structured buffer's stride");

// Vertex stage, `b1 space1`, for `voxel` and `voxel_shadow`.
struct GpuVoxelParams
{
    // x: block size in metres; y: tiles per atlas row; z: a tile in atlas UV;
    // w: half a texel in a tile's own UV.
    f32 params[4]{1.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(GpuVoxelParams) == 16, "GpuVoxelParams is a cbuffer layout");
static_assert(sizeof(GpuVoxelParams) <= kMaxUniformBlockBytes);

} // namespace engine::render
