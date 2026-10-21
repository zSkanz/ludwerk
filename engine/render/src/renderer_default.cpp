#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/asset/surface_shader.h"
#include "engine/asset/texture.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/core/text_key.h"
#include "engine/render/cloth.h"
#include "engine/render/clusters.h"
#include "engine/render/environment.h"
#include "engine/render/particles.h"
#include "engine/render/renderer.h"
#include "engine/render/ribbons.h"
#include "engine/render/settings.h"
#include "engine/render/shader_types.h"
#include "engine/render/shadow.h"
#include "engine/render/surface_source.h"
#include "particle_batches.h"
#include "smaa_tables.h"
#include "terrain_variants.h"

namespace engine::render {
namespace {

using core::f32;
using core::Mat4;
using core::u32;
using core::Vec3;

// Pixels a metre covers at a metre's distance, for choosing a mesh's level of
// detail. Zero -- always the finest level -- under an orthographic camera, where
// how big a thing looks does not depend on how far away it is.
[[nodiscard]] f32 lodPixelsPerUnit(const render::RenderCamera& camera, u32 height) noexcept
{
    if (height == 0 || core::isOrthographic(camera.projection))
        return 0.0f;
    return 0.5f * static_cast<f32>(height) * camera.projection.m[1][1];
}

// Further than any camera in this engine can see, so a cascade boundary set to
// it is never the one a fragment selects.
constexpr f32 kUnreachableDistance = 1.0e9f;

constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::Rgba16Float;
constexpr rhi::TextureFormat kDepthFormat = rhi::TextureFormat::D32Float;
constexpr rhi::TextureFormat kShadowFormat = rhi::TextureFormat::D32Float;
// Tonemapped, sRGB-encoded bytes: the input the anti-aliasing resolve wants.
// `Rgba8Unorm` rather than the sRGB variant because `tonemap.hlsl` has already
// applied the transfer function, and a format that applied it again would be the
// classic double-encode.
constexpr rhi::TextureFormat kLdrFormat = rhi::TextureFormat::Rgba8Unorm;
constexpr rhi::TextureFormat kOcclusionFormat = rhi::TextureFormat::R8Unorm;
// Which pixels are a sprite drawn in its own colours (ADR 0153): a fraction,
// because an anti-aliased edge covers part of a pixel.
constexpr rhi::TextureFormat kSpriteMaskFormat = rhi::TextureFormat::R8Unorm;

// Contact shadows: how far each pixel marches towards the sun, how thick a
// surface is assumed to be behind what the depth buffer shows, and where the
// effect has faded out. Sixty centimetres covers the gap a shadow map's biases
// leave at the base of anything standing on the ground with room to spare; a
// quarter of a metre of thickness stops a pole in front of the ray from
// shadowing the wall a metre behind it; and past sixty metres the gap is
// smaller than a pixel.
constexpr f32 kContactRayMetres = 0.6f;

// The value `drawGeometry` records as the bound material while caves are
// bound, so the next ordinary draw rebinds its own.
constexpr u32 kTerrainBinding = 0xFFFFFFFEu;
// The largest terrain draw the shadow fit takes as a caster, in metres: a
// full-detail node of a few chunks, and nothing coarser.
constexpr f32 kTerrainCasterRadius = 96.0f;
constexpr u32 kVoxelBinding = 0xFFFFFFFDu;

// The block atlas: sixty-four pixel tiles, sixteen to a row, 256 images. Enough
// for any block game's palette, and a quarter of a 2048 atlas's memory. RGBA16F
// because what is drawn into it is the image as the shader SAW it -- already
// linear -- and eight bits of linear crushes the darks of an sRGB image.
constexpr u32 kVoxelTileSize = 64;
constexpr u32 kVoxelTilesPerRow = 16;
constexpr u32 kVoxelAtlasSize = kVoxelTileSize * kVoxelTilesPerRow;
constexpr u32 kVoxelAtlasTiles = kVoxelTilesPerRow * kVoxelTilesPerRow;
constexpr rhi::TextureFormat kVoxelAtlasFormat = rhi::TextureFormat::Rgba16Float;
constexpr f32 kContactThicknessMetres = 0.25f;
// **A mirror ray through the screen** (ADR 0209): how far it reaches at the
// camera -- a room's width; it is longer further off -- and how far behind a
// surface it may land and still have landed on it. Thinner, a ray passes
// behind a table's leg and lands on the wall; thicker, the leg is a slab.
constexpr f32 kScreenReflectionReachMetres = 14.0f;
constexpr f32 kScreenReflectionThicknessMetres = 0.6f;
// And the rays a pixel casts for bounced light: short -- what bounces from
// further is an `IrradianceVolume`'s to say -- and how much the answer of the
// frame before counts in this one's.
constexpr f32 kScreenBounceReachMetres = 3.0f;
constexpr f32 kScreenBounceThicknessMetres = 0.4f;
// Under a camera that moves, ten frames; under a still one, where each
// frame's eight rays are eight more of the same answer, thirty-odd -- a few
// rays beside a lamp are a grain that ten frames' worth
// leaves crawling.
constexpr f32 kScreenBounceHistoryWeight = 0.9f;
constexpr f32 kScreenBounceStillWeight = 0.97f;
// How much the reflection's answer of the frame before counts: under a still
// camera, where it is the same rays started elsewhere, and under one that
// moves, where it is little more than a softening.
constexpr f32 kScreenReflectionStillWeight = 0.9f;
constexpr f32 kScreenReflectionMovingWeight = 0.35f;
constexpr f32 kContactFadeDistance = 60.0f;
constexpr rhi::TextureFormat kLuminanceFormat = rhi::TextureFormat::R32Float;
// A folded eye blitted down for its meter: a 4 by 4 block of it for each
// texel of the first pass's 64 by 64, which that pass's four bilinear taps
// read whole.
constexpr core::u32 kFoldMeterSize = 256;
// `engine_tonemap.hlsli`'s `EngineExposureKey`: the exposure a folded eye
// writes with is worked out here, from the meter read back.
constexpr f32 kExposureKey = 0.45f;

// **FSR 2's passes** (ADR 0164), in the order a frame runs them -- but for the
// two accumulations, of which a frame runs one: the second sharpens after it.
constexpr u32 kFsr2Luminance = 0;
constexpr u32 kFsr2Reconstruct = 1;
constexpr u32 kFsr2DepthClip = 2;
constexpr u32 kFsr2Lock = 3;
constexpr u32 kFsr2Accumulate = 4;
constexpr u32 kFsr2AccumulateSharpen = 5;
constexpr u32 kFsr2Rcas = 6;
constexpr u32 kFsr2PassCount = 7;

// **Frame generation's passes** (ADR 0165): optical flow's seven, then frame
// interpolation's eleven, each in the order a frame first runs it.
constexpr u32 kFgLuma = 0;
constexpr u32 kFgPyramid = 1;
constexpr u32 kFgScdHistogram = 2;
constexpr u32 kFgScdDivergence = 3;
constexpr u32 kFgSearch = 4;
constexpr u32 kFgFilter = 5;
constexpr u32 kFgScale = 6;
constexpr u32 kFgSetup = 7;
constexpr u32 kFgPrepare = 8;
constexpr u32 kFgDepth = 9;
constexpr u32 kFgGameField = 10;
constexpr u32 kFgVectorPyramid = 11;
constexpr u32 kFgPyramidNext = 12;
constexpr u32 kFgFlowField = 13;
constexpr u32 kFgDisocclusion = 14;
constexpr u32 kFgInterpolate = 15;
constexpr u32 kFgColourPyramid = 16;
constexpr u32 kFgInpaint = 17;
constexpr u32 kFgPassCount = 18;
// Optical flow searches seven sizes of the picture, coarsest first, and finds
// one motion a block of eight pixels.
constexpr u32 kFlowLevels = 7;
constexpr u32 kFlowBlock = 8;
// The bins of the histograms a cut is found by: 256 to each of nine parts.
constexpr u32 kFlowHistogramWidth = 256 * 9;
// The most levels an inpainting pyramid is read to.
constexpr u32 kMaxPyramidLevels = 11;

// Five levels, halving from half resolution: the coarsest is a thirty-second of
// the frame, which is where a bloom's tail stops being distinguishable from a
// flat lift.
constexpr u32 kBloomLevels = 5;

// How far towards the frame's measured brightness one FRAME moves. Per frame
// rather than per second, deliberately: a rate driven by elapsed wall-clock time
// would make a screenshot at frame thirty a different picture on a fast machine
// and a slow one.
constexpr f32 kExposureAdaptationRate = 0.05f;

// The bloom threshold, in scene-referred luminance, with a soft knee around it.
// Applied once, on the way into the chain.
constexpr f32 kBloomThreshold = 1.1f;
constexpr f32 kBloomKnee = 0.6f;

// How much of the bloom chain is mixed back in. Small, and it should be: bloom
// that reads as a glow rather than as a haze is mostly threshold and radius, and
// the intensity is what stops it from becoming fog.
constexpr f32 kBloomIntensity = 0.05f;
// `BloomEffect.Size` at which the upsample's tent has its own radius of one
// source texel: the engine's own reach, and the class's default (ADR 0096).
constexpr f32 kBloomSize = 24.0f;
// For a `Sky`'s angular sizes, which are authored in degrees.
constexpr f32 kDegreesToRadians = 0.017453292519943295f;
// How many halvings the look's blur may go down before it runs its Gaussian:
// at five, the smallest level is a thirty-second of the frame, where a blur of
// the whole screen's height is still a handful of texels.
constexpr u32 kLookBlurLevels = 5;
// The Gaussian's width, in texels of the level it runs at, that the level is
// chosen to stay under: wide enough that a bilinear resample back to the frame
// shows no steps, narrow enough that the kernel is a dozen taps.
constexpr f32 kLookBlurLevelSigma = 4.0f;
// The widest circle of confusion `DepthOfFieldEffect` draws, in pixels of a
// 1080-line picture: what `NearIntensity` or `FarIntensity` of 1 reaches.
constexpr f32 kFocusWidestPixels = 16.0f;
// Sun rays: how many taps the gather takes towards the sun, how much each
// counts less than the one before, and how bright the shafts are at an
// `Intensity` of 1 against the sky they come from.
constexpr u32 kRaysTaps = 64;
constexpr f32 kRaysDecay = 0.975f;
constexpr f32 kRaysStrength = 3.0f;
// How far past the screen's edge, in the screen's half-widths, the sun may go
// before its rays have faded away entirely.
constexpr f32 kRaysEdgeFade = 0.6f;
// How far a ray into the open sky is taken to travel through the air, in
// metres. Far enough that a level ray is buried in any air there is, near
// enough that an air which never thins (`Decay` 0) still leaves the zenith a
// little of the sky's own colour at a low `Density`.
constexpr f32 kAirSkyReach = 40000.0f;
// The glare lobe's tightness about the sun, and its strength at `Glare` 1.
constexpr f32 kAirGlareExponent = 12.0f;
constexpr f32 kAirGlareStrength = 0.35f;
// A governed sky's night: how bright the moon's face is, how bright the
// brightest star, and the chance a cell of the star grid holds one.
constexpr f32 kMoonGlow = 1.6f;
constexpr f32 kStarGlow = 1.2f;
constexpr f32 kStarChance = 0.5f;
// The clouds' wind, in layer units a second of game time, and where its
// offset wraps: far enough that a game would have to run for days to see the
// layer jump, near enough that f32 keeps every step of it.
constexpr core::f64 kCloudWind = 0.004;
constexpr core::f64 kCloudWindWrap = 1024.0;
// The share of the air a ray into the open sky counts (`look_air.hlsl`): the
// gradient is already the air above, so the whole integral again drew a grey
// afternoon. The horizon, where the integral is largest, is buried either way.
constexpr f32 kAirSkyShare = 0.3f;

// The most instances one frame may draw through the instanced path, and the one
// vertex buffer they all live in. Five megabytes, allocated once: the alternative
// is a buffer resized mid-frame, which is a stall.
constexpr u32 kMaxInstances = 65536;

// Below this a run is not batched. **Two, measured**: this said three,
// on the reckoning that one instanced call costs a uniform push, a buffer bind
// and a draw, "which is what two ordinary draws cost anyway". It is not what
// they cost: a draw is submitted to the prepass, the forward pass and every
// cascade it casts into, and a run of two is one call in each. Six hundred
// materials on two cubes apiece -- every run a run of two -- were 3,638 draws a
// frame and are 1,828; recording them went from 0.49 ms to 0.31 across the
// three passes, and the frame from 2.40 ms to 2.20.
constexpr u32 kMinInstanceBatch = 2;

constexpr u32 kNoBatch = 0xFFFFFFFFu;

// A frame's skinned instances, and the joints of every palette they read:
// four thousand animated characters in runs, and a quarter of a million joint
// matrices -- sixteen megabytes, made the first frame a skinned run is drawn.
// A run past either is drawn a draw at a time, as before.
constexpr u32 kMaxSkinnedInstances = 4096;
constexpr u32 kMaxPaletteJoints = 262144;

// The occlusion pass's sampling radius in world metres, its self-occlusion bias,
// and how strongly it darkens.
constexpr f32 kOcclusionRadius = 0.6f;
constexpr f32 kOcclusionBias = 0.025f;
constexpr f32 kOcclusionStrength = 1.0f;

// The cofactor matrix of the model transform's rotation-scale block, so a
// non-uniformly scaled mesh lights correctly. The same construction the glTF
// importer uses for baking, and for the same reason: it is det(M) * M^-T, so
// nothing divides by a determinant a degenerate transform makes zero.
[[nodiscard]] Mat4 normalMatrixOf(const Mat4& model) noexcept
{
    const Vec3 a{model.m[0][0], model.m[0][1], model.m[0][2]};
    const Vec3 b{model.m[1][0], model.m[1][1], model.m[1][2]};
    const Vec3 c{model.m[2][0], model.m[2][1], model.m[2][2]};

    const Vec3 cofactor0 = core::cross(b, c);
    const Vec3 cofactor1 = core::cross(c, a);
    const Vec3 cofactor2 = core::cross(a, b);

    Mat4 result;
    result.m[0][0] = cofactor0.x;
    result.m[0][1] = cofactor0.y;
    result.m[0][2] = cofactor0.z;
    result.m[1][0] = cofactor1.x;
    result.m[1][1] = cofactor1.y;
    result.m[1][2] = cofactor1.z;
    result.m[2][0] = cofactor2.x;
    result.m[2][1] = cofactor2.y;
    result.m[2][2] = cofactor2.z;
    return result;
}

[[nodiscard]] std::span<const std::byte> asBytes(const void* data, std::size_t size) noexcept
{
    return std::span<const std::byte>(static_cast<const std::byte*>(data), size);
}

// --- Foliage (ADR 0116) ---------------------------------------------------------
//
// The blocks the foliage shaders read, laid out as `engine_foliage.hlsli` and
// the two compute shaders declare them.

// `GpuFoliageUniforms`, the vertex stage's second block, 64 bytes.
struct GpuFoliageUniforms
{
    f32 wind[4]{};
    f32 windParams[4]{};
    f32 mesh[4]{};
    f32 cameraOrigin[4]{};
};
static_assert(sizeof(GpuFoliageUniforms) == 64);

// The most levels of detail a foliage mesh is drawn at: `FOLIAGE_MAX_LODS`.
constexpr u32 kFoliageMaxLods = 4;

// `GpuFoliageCull`, 192 bytes: the frustum, the run, its mesh and its levels.
struct GpuFoliageCull
{
    f32 planes[6][4]{};
    f32 originRadius[4]{};
    u32 first = 0;
    u32 count = 0;
    u32 bucket = 0;
    u32 bucketBase = 0;
    u32 capacity = 0;
    f32 density = 1.0f;
    f32 drawDistance = 0.0f;
    f32 fadeStart = 0.0f;
    f32 scaleMin = 1.0f;
    f32 scaleMax = 1.0f;
    f32 sink = 0.0f;
    f32 align = 0.0f;
    f32 randomRotation = 1.0f;
    u32 lodCount = 1;
    f32 unused[2]{};
    f32 lodDistances[4]{};
};
static_assert(sizeof(GpuFoliageCull) == 192);

struct GpuFoliageFinalize
{
    u32 commandCount = 0;
    u32 unused[3]{};
};

// One indirect draw's counter -- its bucket's level -- and the most instances
// that level's list holds.
struct GpuFoliageCommand
{
    u32 counter = 0;
    u32 capacity = 0;
};

// The size of one entry of a visible list, `FoliageVisible`.
constexpr u32 kFoliageVisibleBytes = 48;

// A bloom level's size. Level zero is HALF the frame, so the chain starts one
// halving in -- a full-resolution first level would be the frame's cost again
// for a term that is about to be blurred.
[[nodiscard]] u32 bloomLevelSize(u32 base, u32 level) noexcept
{
    const u32 size = base >> (level + 1);
    return size > 0 ? size : 1u;
}

// One fullscreen triangle into one target: the shape every pass in the post
// chain has. Written once because eleven copies of it would be eleven places to
// forget the scissor, and a missing scissor is a pass that draws nothing on a
// backend that requires one.
//
// `secondUniforms` is the fragment stage's slot 1, for a pass that keeps an
// existing block at slot 0 unchanged and adds its own beside it -- the graded
// tonemap, which must leave the plain one's block byte for byte as it was.
// **Ground no layer names draws plain**, a light matte grey, not black: a new
// terrain has no materials (2026-09-29), and its ground is the shape a person
// is sculpting before it is anything.
[[nodiscard]] GpuTerrainLayer plainTerrainLayer() noexcept
{
    GpuTerrainLayer plain;
    for (u32 channel = 0; channel < 3; ++channel) {
        plain.flat[channel] = 0.55f;
        plain.tint[channel] = 1.0f;
    }
    plain.flat[3] = 1.0f;
    plain.surface[0] = 0.9f;
    return plain;
}

void fullscreenPass(rhi::ICmdList& cmd, rhi::PipelineHandle pipeline, rhi::TextureHandle target, u32 width, u32 height,
                    std::string_view name, std::span<const rhi::TextureBinding> textures,
                    std::span<const std::byte> uniforms, rhi::LoadOp loadOp = rhi::LoadOp::Clear,
                    std::span<const std::byte> secondUniforms = {})
{
    const std::array<rhi::ColorAttachment, 1> attachment{rhi::ColorAttachment{
        .texture = target,
        .loadOp = loadOp,
        .storeOp = rhi::StoreOp::Store,
    }};
    cmd.beginRenderPass({.colorAttachments = attachment, .debugName = name});
    cmd.setPipeline(pipeline);
    cmd.setViewport({.width = static_cast<f32>(width), .height = static_cast<f32>(height)});
    cmd.setScissor({.width = static_cast<core::i32>(width), .height = static_cast<core::i32>(height)});
    if (!uniforms.empty())
        cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, uniforms);
    if (!secondUniforms.empty())
        cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, secondUniforms);
    if (!textures.empty())
        cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
}

// A pass that clears a target and draws nothing.
//
// What it is for: a post pass that a setting switched off still has a texture
// downstream of it, and a texture the renderer samples must never hold whatever
// the allocator handed back. Clearing is both cheaper than the pass it replaces
// and the only answer that does not depend on the contents of memory -- the
// alternative, binding it anyway and multiplying by zero, turns an uninitialised
// NaN into a black frame.
void clearPass(rhi::ICmdList& cmd, rhi::TextureHandle target, u32 width, u32 height, std::string_view name,
               rhi::ColorRgba color)
{
    const std::array<rhi::ColorAttachment, 1> attachment{rhi::ColorAttachment{
        .texture = target,
        .loadOp = rhi::LoadOp::Clear,
        .storeOp = rhi::StoreOp::Store,
        .clearColor = color,
    }};
    cmd.beginRenderPass({.colorAttachments = attachment, .debugName = name});
    cmd.setViewport({.width = static_cast<f32>(width), .height = static_cast<f32>(height)});
    cmd.setScissor({.width = static_cast<core::i32>(width), .height = static_cast<core::i32>(height)});
    cmd.endRenderPass();
}

[[nodiscard]] u32 environmentLevelSize(u32 level) noexcept
{
    const u32 size = kEnvironmentBaseSize >> level;
    return size > 0 ? size : 1u;
}

// A run of draws that share a mesh, a section, a material and a level of
// detail, collapsed into one call (ADR 0043).
//
// **Culled as a WHOLE**, and that is the trade this design makes. Building a
// separate instance list per pass would let each cascade reject each object, at
// the cost of six lists per frame; culling whole batches instead keeps one list
// and draws a batch into any pass that any of it reaches. What that spends is
// vertex work on instances that are clipped -- which is the cheap side of a
// frame that was measured to be CPU-bound on submission.
struct InstanceBatch
{
    // The first draw of the run, which is the one that carries its mesh,
    // section and material.
    u32 firstDraw = 0;
    u32 firstInstance = 0;
    u32 count = 0;
    u32 lod = 0;
    // The union of the run's bounds, for the per-pass tests.
    Vec3 boundsCenter;
    f32 boundsRadius = 0.0f;
    // Whether ANY of the run is in the camera's frustum: the camera's passes
    // draw none of it otherwise.
    bool anyVisible = false;
    // **How many of it are** (ADR 0182), staged first: the camera's passes
    // draw these and not the rest. And where its members' bounds begin in
    // `batchBounds_`, in the order staged: what a shadow map's piece of the run
    // is chosen from.
    u32 visibleCount = 0;
    u32 firstBounds = 0;
    // A run of skinned draws: its instances are `GpuSkinnedInstance`s,
    // each posed by its own palette.
    bool skinned = false;
    // **A run of baked parts** (ADR 0253): drawn by the pipeline that reads
    // their lightmap, each instance's rows in the parallel baked stream.
    bool baked = false;
};

// The prefiltered environment's freshness, and the policy that keeps a
// day/night cycle from putting a CPU prefilter in every frame.
//
// **A frame uploads exactly one level, always, whether or not anything
// changed.** That is not the cheapest arrangement and it is the correct one:
// `clock_differential` requires two frames that differ only in `ClockTime` to
// issue the same NUMBER of commands, because what a clock changes is the values
// a frame carries and not its shape. An upload that appeared only when the sky
// had moved made a frame's shape depend on its history, and the gate said so
// the first time it ran. One level per frame is about thirty kilobytes averaged
// over the chain.
//
// Baking is what is conditional. A change marks every level dirty and each is
// rebaked when the cursor reaches it, finest first from wherever the cursor
// happens to be -- so a reflection sharpens over a few frames rather than
// stalling one.
//
// The FIRST bake is whole, because an environment that arrived one level per
// frame would light the first six frames of every run differently from the
// seventh, and that is a difference a golden recorded at frame two and a
// screenshot taken at frame thirty would disagree about.
// How far the clouds may drift, in layer units, before the reflections are
// rebuilt to show where they went: about every five seconds of game time.
constexpr f32 kCloudRebakeDrift = 0.02f;

struct EnvironmentCache
{
    SkyParams target{};
    bool everBaked = false;
    bool dirty[kEnvironmentMipCount]{};
    // Which level this frame uploads. Advances every frame regardless of
    // anything, which is the whole point.
    u32 cursor = 0;
    // What the shader is given, and what the last bake produced. They are two
    // fields because the second one STEPS: it is only recomputed when the sky
    // has drifted past `kEnvironmentRebuildCosine`, which is about once every
    // hundred frames under a fast day cycle, and a diffuse ambient that arrives
    // in steps is a world whose every matte surface pulses at once.
    Vec3 irradiance[9]{};
    Vec3 irradianceTarget[9]{};
    // The sky the target was projected from, which is a different question from
    // the one `target` above answers -- see `irradianceStale`.
    SkyParams irradianceSky;
    // And what LAST FRAME's sky was, which is a third question again: it is how
    // a clock being scrubbed is told apart from a clock running.
    SkyParams previousSky;
    bool hasPreviousSky = false;
    // Kept resident rather than rebuilt into one scratch buffer: the upload
    // happens every frame and the bake does not, so the pixels have to outlive
    // the bake that made them. About 171 KiB for the whole chain.
    std::vector<core::u16> levels[kEnvironmentMipCount];
    // What each level holds, as one number: how the one texture every view
    // shares is known to hold this view's sky or another's (ADR 0202).
    core::u64 hashes[kEnvironmentMipCount]{};
    std::vector<core::u16> lut;

    // True when `params` differs from what the chain was baked from by enough
    // to be worth the work. The sun moving is the common case; the horizon
    // colour changing is a script writing `Lighting.FogColor` and is rare, so it
    // is tested exactly rather than with a threshold.
    // The diffuse half's own test. Same shape as `stale`, four times tighter,
    // and cheap enough to be: see the note where it is called.
    [[nodiscard]] bool irradianceStale(const SkyParams& params) const noexcept
    {
        if (!everBaked)
            return true;
        if (core::dot(params.sunDirection, irradianceSky.sunDirection) < kIrradianceRebuildCosine)
            return true;
        return !(params.horizonColor == irradianceSky.horizonColor && params.zenithColor == irradianceSky.zenithColor &&
                 params.sunColor == irradianceSky.sunColor && params.skybox == irradianceSky.skybox &&
                 params.celestial == irradianceSky.celestial);
    }

    [[nodiscard]] bool stale(const SkyParams& params) const noexcept
    {
        if (!everBaked)
            return true;
        if (core::dot(params.sunDirection, target.sunDirection) < kEnvironmentRebuildCosine)
            return true;
        return !(params.horizonColor == target.horizonColor && params.zenithColor == target.zenithColor &&
                 params.sunColor == target.sunColor && params.specularScale == target.specularScale &&
                 params.skybox == target.skybox && params.celestial == target.celestial &&
                 params.sunAngularRadius == target.sunAngularRadius && params.cloudCover == target.cloudCover &&
                 params.cloudDensity == target.cloudDensity && params.cloudColor == target.cloudColor &&
                 std::abs(params.cloudDriftX - target.cloudDriftX) < kCloudRebakeDrift &&
                 std::abs(params.cloudDriftZ - target.cloudDriftZ) < kCloudRebakeDrift);
    }
};

// **Everything a renderer remembers about ONE view** (ADR 0107): the targets
// sized to it, the exposure it has adapted to, the cascade fit it keeps, the
// environment chain baked for its sky, and the look's images at its size.
//
// A frame used to draw one view, and these were plain members. A camera that
// draws into a texture is a second view in the same frame, and sharing this
// with the first would be two exposures fighting over one history and every
// screen-sized target rebuilt twice a frame as the sizes alternated -- which is
// exactly what a content-browser thumbnail did to the editor's viewport before
// this existed.
//
// **A base the renderer swaps, not a field it indexes.** `DefaultRenderer`
// derives from it privately, so four thousand lines go on naming `hdr_` and
// `exposure_` as they always did, and `render` swaps the view it was asked for
// into the base and the previous one out into `views_`. The main view is view
// 0 and a frame that draws only it never swaps anything, which is what keeps a
// game that uses no views drawing exactly as before.
struct ViewState
{
    rhi::TextureHandle hdr_{};
    rhi::TextureHandle depth_{};
    // A headset's eye drawn multisampled: the samples of its colour and its
    // depth, made the first time and at the size and count asked.
    rhi::TextureHandle msaaColor_{};
    rhi::TextureHandle msaaDepth_{};
    // **The world's interface as the display shows it** (ADR 0250), where the
    // tonemap is a pass of its own: drawn apart from the scene, tested against
    // its depth, and laid over the picture by the tonemap. The samples, where
    // the eye has them, and the layer they resolve into; whether this frame
    // drew it.
    rhi::TextureHandle uiLayer_{};
    rhi::TextureHandle uiLayerSamples_{};
    u32 uiLayerWidth_ = 0;
    u32 uiLayerHeight_ = 0;
    u32 uiLayerSampleCount_ = 0;
    bool uiLayerLive_ = false;
    // The frame's uniforms as the forward pass had them, for what is drawn
    // after it and lit by the same sun: the world's interface lit as a
    // surface, and the particles in its shadow (`GpuParticleShadow`).
    GpuFrameUniforms forwardFrame_{};
    // The format `msaaColor_` was made in: the 16-bit target's, or a headset
    // image's when the tonemap is folded into the pass.
    rhi::TextureFormat msaaFormat_ = rhi::TextureFormat::Undefined;
    // **The exposure this view's forward pass writes with**, the tonemap
    // folded into it (ADR 0250): nought where it is not folded.
    f32 foldExposure_ = 0.0f;
    // Whether what is folded is written encoded, into a plain 8-bit image --
    // a lean window's swapchain -- rather than into an sRGB one: the shaders
    // are given the exposure negative.
    bool foldEncoded_ = false;
    rhi::TextureFormat foldMeterFormat_ = rhi::TextureFormat::Rgba8UnormSrgb;
    // The exposure as the shaders are given it.
    [[nodiscard]] f32 foldForShaders() const noexcept { return foldEncoded_ ? -foldExposure_ : foldExposure_; }
    // A folded eye blitted down for its meter, `kFoldMeterSize` square.
    rhi::TextureHandle foldMeter_{};
    // The fragment density map that shades `msaaColor_`'s passes, and what
    // it was drawn for: the level, and the eye's own middle on the screen.
    rhi::TextureHandle densityMap_{};
    u32 densityLevel_ = 0;
    f32 densityCentre_[2]{};
    u32 msaaSamples_ = 0;
    u32 msaaWidth_ = 0;
    u32 msaaHeight_ = 0;
    // **The depth of what no decal paints** (`BasePart.ReceivesDecals`): the
    // parts with it off, drawn alone, on a frame that has both a decal and one
    // of them in view. A decal's pixel is left alone where this and the
    // scene's depth are the same surface. Made the first frame it is needed.
    rhi::TextureHandle decalMask_{};
    // **The depth of the foliage a decal paints** (ADR 0185), drawn alone on a
    // frame that has a decal and any of it. A decal fades where a surface
    // turns away from it, and a blade of grass is edge-on to every mark laid
    // on the ground under it: where this and the scene's depth are the same
    // surface, the decal lands whole.
    rhi::TextureHandle decalFoliage_{};
    // Set while that mask is being drawn: `drawGeometry` takes only the parts
    // that receive none.
    // **Which of the solid parts a pass of the forward draw takes** (audit c,
    // `BasePart.ReceivesDecals`). On a frame with a decal and a part that takes
    // none, the parts that take decals are drawn, then the decals, then the
    // parts that take none -- over whatever a decal painted where they stand.
    enum class DecalTurn : core::u8
    {
        Every,
        Receiving,
        Refusing,
    };
    DecalTurn decalTurn_ = DecalTurn::Every;
    // Set while the lamps' shadow tiles are drawn, which are the sun's
    // cascades to `drawGeometry` in everything but what they are counted as.
    bool localShadowPass_ = false;
    // Whether this frame drew the depth prepass: what an opaque draw's
    // pipeline assumes (ADR 0174), and whether the forward pass loads the
    // depth or clears it.
    bool prepassDrawn_ = true;
    // **What is cleared and has not been drawn to since** (ADR 0172): the
    // occlusion, the contact shadows and the bloom of a view whose settings
    // have them off are a white, a white and a black picture, cleared the
    // first frame and left alone after. Forgotten when the targets are made
    // again, and with the view.
    bool occlusionOff_ = false;
    bool contactOff_ = false;
    bool bloomOff_ = false;
    // Tonemapped and sRGB-encoded, so the anti-aliasing resolve has an image to
    // find edges in. FXAA works on perceptual luminance, which is what makes it
    // a post-tonemap pass rather than a pre-tonemap one.
    rhi::TextureHandle ldr_{};
    // Half resolution, and the blur's ping-pong partner. Half because sixteen
    // taps at full resolution is four times the cost for a term the blur is
    // about to spread anyway (R16).
    rhi::TextureHandle occlusion_{};
    // **What the screen says** (ADR 0209). The reflection: where each pixel's
    // mirror ray landed in the last frame's lit picture, half the picture's
    // size, with a chain of its own. The bounce: the light gathered from the
    // screen. Neither is there while its setting is off, and then the lit
    // pass is handed a black pixel, which reads as "no answer".
    rhi::TextureHandle screenReflection_{};
    rhi::TextureHandle screenBounce_{};
    // The bounce's answer of the frame before, which this frame's is gathered
    // with; the two change places every frame.
    rhi::TextureHandle screenBounceBefore_{};
    bool screenBounceBeforeValid_ = false;
    // And the reflection's, for the same reason: a march in steps is gathered
    // over frames, or its edges are ragged at rest.
    rhi::TextureHandle screenReflectionBefore_{};
    bool screenReflectionBeforeValid_ = false;
    // The camera the last answer was made under: while it has not moved, the
    // answers are of the same rays and are gathered for longer.
    Mat4 screenLastCamera_{};
    // How many frames each has gathered. **The first counts whole, the second
    // half, the third a third**: a mean from its first frame, at full
    // strength, rather than a fade in from black over the ten frames
    // a fixed weight takes -- a setting switched on drew a different picture
    // from the same setting had from the start, for that long.
    u32 screenReflectionGathered_ = 0;
    u32 screenBounceGathered_ = 0;
    // Whether this frame's pass wrote each: not on the first frame, after a
    // cut, under an orthographic camera, or while off.
    bool screenReflectionReady_ = false;
    bool screenBounceReady_ = false;
    // **The last frame as the lit and blended passes left it** (ADR 0209):
    // what a ray that lands is read from, since this frame's picture has not
    // been lit when the ray is marched. Kept only while something reads the
    // screen -- with both settings off it is not made and nothing is copied.
    // One picture for every anti-aliasing mode: the temporal pass keeps a
    // history only when it is the mode, and the upscaler's is another size.
    rhi::TextureHandle sceneBefore_{};
    // Whether it holds a frame: not on the first, nor after the targets were
    // made again.
    bool sceneBeforeValid_ = false;
    // The camera that frame was drawn from, to find a landing in it again.
    Mat4 sceneBeforeViewProjection_{};
    core::DVec3 sceneBeforeOrigin_{};
    u32 screenReflectionLevels_ = 1;
    core::u64 screenFrame_ = 0;
    // One channel: which pixels belong to something a tool has selected. Only
    // ever written when a draw carries `outlined`, which no game does.
    rhi::TextureHandle outlineMask_{};
    // One channel: which pixels are a sprite drawn in its own colours
    // (ADR 0153). Made the first frame this view has one, and never for a view
    // that has none.
    rhi::TextureHandle spriteMask_{};
    rhi::TextureHandle occlusionBlur_{};
    // Full resolution, one channel: the sun's contact shadows. Full rather than
    // half like the occlusion term, because what it carries is the sharp line
    // where a caster meets the ground.
    rhi::TextureHandle contact_{};
    // The bloom chain, each level its own texture because a `ColorAttachment`
    // names a texture and not a mip level.
    rhi::TextureHandle bloom_[kBloomLevels]{};
    // The automatic exposure's measurement chain, and the two 1x1 targets it
    // ping-pongs between: one frame reads what the last one wrote.
    rhi::TextureHandle luminance64_{};
    rhi::TextureHandle luminance8_{};
    rhi::TextureHandle exposure_[2]{};
    u32 exposureIndex_ = 0;
    bool exposureInitialised_ = false;

    // The prefiltered environment's cache (ADR 0038, environment.h): what the
    // sky's chain holds and when it is rebaked. Per view because a view of
    // another world -- a preview's own sun, a sub-world's sky -- is another
    // sky, and one cache shared between two skies rebakes whole on every
    // alternation. The TEXTURE is one for every view (`environmentMap_`).
    EnvironmentCache environment_;

    // The size the offscreen targets were built for. A window resize rebuilds
    // them rather than stretching, because a stretched HDR target is a bug that
    // looks like a driver problem.
    u32 width_ = 0;
    u32 height_ = 0;

    // What the settings resolve to for this frame: the world is rendered at a
    // fraction of the output and the final resolve upscales it. `width_` above
    // is what the internal chain was BUILT for, and these two are what it is
    // built for now -- the same number until a render scale is set.
    u32 renderWidth_ = 0;
    u32 renderHeight_ = 0;

    // The previous frame's cascade fit, so this frame can keep it.
    ShadowCascades shadowFit_{};
    bool shadowFitted_ = false;

    // The scene behind blended surface shaders (ADR 0091): its depth as
    // distances, and its colour -- each made only in a frame that needs it.
    rhi::TextureHandle sceneDepthCopy_{};
    rhi::TextureHandle sceneColorCopy_{};

    // **The look's own images, made the first frame one is needed** and
    // remade when the render size changes -- never on a frame without the
    // effect that needs them. `lookColor_` is a second full-resolution HDR
    // image, for a pass that reads the frame and has to write a changed one
    // somewhere else; the blur's levels are its downsample chain and each
    // level's ping-pong partner.
    rhi::TextureHandle lookColor_{};
    rhi::TextureHandle blurLevels_[kLookBlurLevels]{};
    rhi::TextureHandle blurPong_[kLookBlurLevels]{};
    // Depth of field at half resolution: the frame with each texel's circle,
    // and what the gather made of it.
    rhi::TextureHandle focusPrepared_{};
    rhi::TextureHandle focusGathered_{};
    // Sun rays at half the frame: what can shine, and the shafts.
    rhi::TextureHandle raysMasked_{};
    rhi::TextureHandle raysGathered_{};
    u32 lookWidth_ = 0;
    u32 lookHeight_ = 0;

    // --- Anti-aliasing and upscaling (ADR 0158) --------------------------------
    //
    // **Made the first frame a mode needs them**, at the render size or, for
    // the upscale's output, the target's -- and none of them on a frame that
    // draws through FXAA alone, which is what keeps that frame's command
    // stream the one it always was. SMAA's edges and weights; the picture an
    // upscale reads; EASU's output, which RCAS reads; and the temporal pass's
    // velocities and the history it ping-pongs.
    rhi::TextureHandle smaaEdges_{};
    rhi::TextureHandle smaaWeights_{};
    rhi::TextureHandle aaResolved_{};
    rhi::TextureHandle upscaled_{};
    rhi::TextureHandle velocity_{};
    rhi::TextureHandle history_[2]{};
    u32 historyIndex_ = 0;
    bool historyValid_ = false;
    u32 aaWidth_ = 0;
    u32 aaHeight_ = 0;
    u32 upscaledWidth_ = 0;
    u32 upscaledHeight_ = 0;
    // **The last frame's camera, unjittered, and the origin its space was
    // relative to**: what a pixel's motion is measured against.
    Mat4 previousViewProjection_{};
    core::DVec3 previousOrigin_{};
    bool previousCamera_ = false;
    // Every part's place a frame ago, by its draws' motion key, in the space of
    // the camera it was drawn with -- and this frame's, filled as it is drawn.
    struct Placed
    {
        Mat4 transform;
        core::DVec3 origin;
    };
    std::unordered_map<u64, Placed> placed_;
    std::unordered_map<u64, Placed> placing_;
    // The same for a headset eye's motion for its runtime (ADR 0252), kept
    // apart: an eye has no temporal pass to keep the others.
    std::unordered_map<u64, Placed> warpPlaced_;
    std::unordered_map<u64, Placed> warpPlacing_;
    // And each canvas of the world's interface, by its run's key.
    std::unordered_map<u64, Placed> warpUiPlaced_;
    std::unordered_map<u64, Placed> warpUiPlacing_;
    // Which of the sample positions this view's next temporal frame takes.
    // The view's own (ADR 0205): one count for every view gave each of four
    // views two of the eight positions, by turns.
    u32 jitterIndex_ = 0;
};

class DefaultRenderer final : public IRenderer, private ViewState
{
public:
    std::optional<core::EngineError> create(rhi::IDevice& device, const ShaderLibrary& shaders,
                                            rhi::TextureFormat colorFormat) override;
    void destroy(rhi::IDevice& device) override;
    void render(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target, const RenderWorld& world,
                const MeshCache& meshes) override;
    void releaseView(rhi::IDevice& device, u32 view) override;
    [[nodiscard]] bool valid() const noexcept override { return valid_; }

    // Scaled with the shadow distance, because that is what it describes: the
    // fit's own radius is derived from how far the sun casts, and a setting that
    // halved the distance while extraction kept every caster within 220 metres
    // would be paying for casters no cascade covers.
    //
    // Zero when the sun casts into no cascades at all, which is what turns the
    // whole caster-retention rule off rather than leaving it running for a
    // shadow map nothing writes.
    [[nodiscard]] f32 shadowRadius() const noexcept override
    {
        if (settings_.shadowCascades == 0)
            return 0.0f;
        return kShadowRadius * (settings_.shadowDistance / kShadowDistance);
    }

    [[nodiscard]] RendererStats stats() const noexcept override { return stats_; }

    void setSettings(const GraphicsSettings& settings) override;
    void setSurfaceSource(ISurfaceSource* source) override { surfaceSource_ = source; }
    void warm(rhi::IDevice& device) override;
    void warmEyes(rhi::IDevice& device, u32 samples, u32 foveation, rhi::TextureFormat eyeFormat) override;
    [[nodiscard]] std::optional<bool> warmSurface(rhi::IDevice& device, std::string_view name) override;
    [[nodiscard]] const GraphicsSettings& settings() const noexcept override { return settings_; }
    [[nodiscard]] core::Vec2 cameraJitter(const RenderWorld& world, const RenderTarget& target) const override;
    void composeViews(rhi::IDevice& device, rhi::ICmdList& cmd, const ViewComposite& views,
                      const RenderTarget& target) override;
    void presentEye(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture, const RenderTarget& target,
                    const PictureCrop& crop) override;
    void presentEyeDepth(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target) override;
    [[nodiscard]] bool generatesFrames(const RenderWorld& world) const noexcept override;
    void showPicture(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture,
                     const RenderTarget& target) override;
    [[nodiscard]] rhi::TextureHandle interpolateFrame(rhi::IDevice& device, rhi::ICmdList& cmd,
                                                      rhi::TextureHandle previous, rhi::TextureHandle current,
                                                      u32 width, u32 height) override;
    // Whether this frame of the main view is a temporal one: the one rule
    // `cameraJitter` and `render` both ask, so a frame is never jittered
    // without the pass that takes the jitter out.
    [[nodiscard]] bool temporalFrame(const RenderWorld& world) const noexcept;
    // Whether the world's main view, this frame, is upscaled by FSR 2
    // (ADR 0164): the setting, a device that made its passes, and a camera
    // with perspective -- the algorithm reads how far each pixel is from the
    // eye out of the depth, which a camera without perspective does not say.
    [[nodiscard]] bool fsr2Frame(const RenderWorld& world) const noexcept;
    // The fraction of the target the world is drawn at: the settings' scale
    // and cap, or the whole of it for a picture of sprites alone -- **and for
    // a view into a texture**: a `ViewportFrame`, a sub-world's or a
    // camera's picture is UI, drawn at its frame's own pixel size as the rest
    // of the UI is. At the world's scale it came out at a fraction of that
    // and the UI showed it in hard blocks.
    //
    // **A part of the window is the window** (ADR 0205): a player's view of
    // a shared screen is scaled as the one picture would be.
    [[nodiscard]] f32 worldRenderScale(const RenderWorld& world, u32 width, u32 height) const noexcept
    {
        return worldRenderScale(world, width, height, activeView_ == 0 || windowPart_);
    }
    [[nodiscard]] f32 worldRenderScale(const RenderWorld& world, u32 width, u32 height, bool ofWindow) const noexcept
    {
        return !ofWindow || spritesOnly(world) ? 1.0f : effectiveRenderScale(settings_, width, height);
    }

private:
    [[nodiscard]] std::optional<core::EngineError> ensureTargets(rhi::IDevice& device, u32 width, u32 height);
    [[nodiscard]] std::optional<core::EngineError> ensureShadowMap(rhi::IDevice& device);
    // Which draws one call submits. `Shadow` takes every item in the list --
    // a caster outside the view still casts into it -- while the two forward
    // selectors take only what the camera can see, each from its own pass.
    enum class Selection
    {
        Shadow,
        // Every draw a tool has selected, into a single-channel mask. Depth-only
        // in the sense that matters here -- it wants a position and nothing else
        // -- which is why it shares the shadow pass's vertex shaders.
        Outline,
        // The same mask for one `Highlight` (ADR 0129): every draw that
        // carries `highlightFilter_`.
        Highlight,
        // Depth only, but filtered like the forward pass: a caster outside the
        // view still casts into it, and a caster outside the view still must not
        // fill the depth buffer the camera reads.
        Prepass,
        Opaque,
        Transparent,
    };

    // One call issued, counted in the total and under what it was for.
    void countDraw(DrawKind kind) noexcept
    {
        ++stats_.drawCalls;
        ++stats_.drawsByKind[static_cast<usize>(kind)];
    }
    // What a mesh's call is counted as: the pass it was submitted into, and
    // in the picture itself whether it was one object or a run.
    [[nodiscard]] DrawKind meshDrawKind(Selection selection, bool run) const noexcept
    {
        switch (selection) {
        case Selection::Shadow:
            return localShadowPass_ ? DrawKind::LocalShadow : DrawKind::SunShadow;
        case Selection::Outline:
            return DrawKind::Outline;
        case Selection::Highlight:
            return DrawKind::Highlight;
        case Selection::Prepass:
            return DrawKind::Prepass;
        case Selection::Transparent:
            return run ? DrawKind::BlendedRun : DrawKind::Blended;
        case Selection::Opaque:
            break;
        }
        return run ? DrawKind::MeshRun : DrawKind::Mesh;
    }

    // `skinned` is the pipeline a draw with a joint palette switches to. The
    // caller sets the static one and this switches at most once per pass,
    // because `extract` sorts by pipeline -- so a world with no skinned draw
    // makes no extra call at all, which is what keeps the first renderer's goldens byte-exact.
    // A cascade's own bounds, so the shadow pass draws into cascade zero only
    // what cascade zero covers. Without it every cascade draws every caster and
    // four cascades cost four times the submission -- which is the exact cost
    // this milestone is also spending instancing to remove.
    struct CullSphere
    {
        Vec3 centre;
        f32 radius = 0.0f;
        // From the centre towards the light, as far as its map's depth reaches
        // (`casterReaches`). Zero for a lamp: its sphere is its reach.
        Vec3 sweep{};
    };

    void drawGeometry(rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes, const Mat4& viewProjection,
                      rhi::PipelineHandle staticPipeline, rhi::PipelineHandle skinnedPipeline, Selection selection,
                      const CullSphere* cull = nullptr);

    // Groups the sorted draw list into instanced runs and fills the staging
    // buffer. Runs before any render pass, because the upload has to.
    void buildInstanceBatches(const RenderWorld& world, const MeshCache& meshes);
    // A skinned draw's place in the frame's palette, at the vertex stage's
    // second block, and the palette at its first storage buffer -- or its
    // second, after a mesh's morph targets.
    void bindSkinPalette(rhi::ICmdList& cmd, const RenderWorld& world, const DrawItem& draw, bool afterMorphs);
    bool ensureSkinPalette(rhi::IDevice& device);

    // --- Surface shaders (ADR 0091) -------------------------------------------
    //
    // **Built the first time a frame names one, and never before** -- the
    // terrain's rule, for the terrain's reason: a renderer that made them in
    // `create` would change the command stream, and every capture golden, of
    // every scene that has none. A surface that cannot be built is tried once,
    // reported once, and drawn as the built-in surface.
    struct SurfaceSet
    {
        std::string name;
        bool ready = false;
        // Its pipelines failed for this revision: it draws as the error
        // surface every frame until the source changes, not only the first.
        bool failed = false;
        // A URN surface's program revision, so a recompile rebuilds.
        core::u64 revision = 0;
        asset::SurfaceReflection reflection;
        std::array<rhi::ShaderHandle, 10> shaders{};
        rhi::PipelineHandle forward{};
        rhi::PipelineHandle blended{};
        rhi::PipelineHandle instanced{};
        // `forward` and `instanced` for a draw the prepass drew (ADR 0174).
        rhi::PipelineHandle forwardPrepassed{};
        rhi::PipelineHandle instancedPrepassed{};
        rhi::PipelineHandle shadow{};
        rhi::PipelineHandle shadowInstanced{};
        rhi::PipelineHandle prepass{};
        rhi::PipelineHandle prepassInstanced{};
        // **Being made off the frame** (ADR 0257), and since when; whether a
        // wait past the project's was said.
        bool making = false;
        std::chrono::steady_clock::time_point askedAt{};
        bool waitSaid = false;
    };
    std::vector<SurfaceSet> surfaces_;
    // What an eye's pass draws with (`warmEyes`): a surface's lit pipelines
    // are made at it too, off the frame.
    u32 eyeSamples_ = 1;
    bool eyeDensity_ = false;
    rhi::TextureFormat eyeFormat_ = rhi::TextureFormat::Undefined;
    // Per material of the frame: its surface's pipelines are being made, and
    // it is drawn in no pass until they are (ADR 0257).
    std::vector<bool> materialWaiting_;
    ISurfaceSource* surfaceSource_ = nullptr;
    // Per material of the frame: whether its surface failed, and draws as the
    // error surface -- loud magenta, the colour no material means.
    std::vector<bool> materialError_;
    // Per material of the frame: its surface (index + 1, 0 for the built-in),
    // its packed block, and its surface textures in declaration order.
    std::vector<u32> materialSurface_;
    std::vector<std::vector<core::u8>> materialBlock_;
    std::vector<std::array<rhi::TextureBinding, asset::MaxSurfaceTextures>> materialSurfaceTextures_;
    // `waiting`: its pipelines are being made off the frame (ADR 0257).
    [[nodiscard]] u32 surfaceFor(rhi::IDevice& device, std::string_view name, bool& failed, bool& waiting);
    // A set being made, asked again: ready once every pipeline is.
    void pollSurface(rhi::IDevice& device, SurfaceSet& set);
    [[nodiscard]] bool buildSurfacePipelines(rhi::IDevice& device, SurfaceSet& set);
    static void releaseSurface(rhi::IDevice& device, SurfaceSet& set);
    void prepareSurfaces(rhi::IDevice& device, const RenderWorld& world);
    void bindSurface(rhi::ICmdList& cmd, u32 material, bool fragment, bool blended) const;
    // Before the blended pass: the scene's depth and, when asked, colour.
    void copySceneForSurfaces(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                              const GpuFrameUniforms& frame);

    // --- Terrain (ADR 0082) --------------------------------------------------
    //
    // **Created the first time a frame has terrain, and never before.** A
    // renderer that made these in `create` would add two pipelines to the
    // command stream of every scene -- and to every capture golden of a scene
    // that has no terrain at all. Deferred, a project that never touches
    // terrain pays nothing and its goldens do not move.
    [[nodiscard]] bool ensureTerrain(rhi::IDevice& device);
    // The block shader's pipeline, on the same lazy terms as the terrain's.
    [[nodiscard]] bool ensureVoxel(rhi::IDevice& device);
    // The particle pipeline and its instance buffer, on the same lazy terms.
    [[nodiscard]] bool ensureParticles(rhi::IDevice& device);
    // The skinned runs' pipelines and buffers, made the first frame a
    // skinned draw is in the world -- so a world without one builds nothing
    // and moves no capture golden.
    [[nodiscard]] bool ensureSkinnedInstancing(rhi::IDevice& device);
    // The morph pipelines (ADR 0196), made the first frame a body has a
    // target above nought -- on the same terms, and for the same reason.
    [[nodiscard]] bool ensureMorph(rhi::IDevice& device);
    // The decal pipeline, on the same lazy terms.
    [[nodiscard]] bool ensureDecals(rhi::IDevice& device);
    // The ribbon pipeline and its vertex buffer (ADR 0129), on the same terms.
    [[nodiscard]] bool ensureRibbons(rhi::IDevice& device);
    // The two mask pipelines of an occluded `Highlight` (ADR 0129), on the
    // same terms: a world with no such highlight makes neither.
    [[nodiscard]] bool ensureHighlightMasks(rhi::IDevice& device);
    // Every `Highlight` of the frame over the finished image: a mask and a
    // composite each.
    void drawHighlights(rhi::ICmdList& cmd, rhi::IDevice& device, const RenderWorld& world, const MeshCache& meshes,
                        const RenderTarget& target);
    // The world UI pipelines and buffer, on the same lazy terms.
    [[nodiscard]] bool ensureWorldUi(rhi::IDevice& device);
    // Which of the world's UI runs a pass draws (ADR 0250): at a brightness
    // of 1 or less, as the display shows them; above it, as light.
    enum class WorldUiRuns : core::u8
    {
        All,
        AsDisplayed,
        AsLight,
    };
    // The world's UI into the open pass: back to front, the ones always on top
    // after all of them.
    void drawWorldUi(rhi::ICmdList& cmd, const RenderWorld& world, WorldUiRuns runs);
    // Whether a run is drawn as the display shows it: its own colours, at a
    // brightness of 1 or less, with none of the scene's light.
    [[nodiscard]] static bool shownAsDisplayed(const WorldUiRun& run) noexcept
    {
        return run.brightness <= 1.0f && run.lightInfluence <= 0.0f;
    }
    // The layer the interface as the display shows it is drawn into apart
    // (`uiLayer_`), at the render size and the pass's samples.
    [[nodiscard]] bool ensureUiLayer(rhi::IDevice& device, u32 samples);
    // The shapes drawn on parts (ADR 0251): their pipelines, meshes and
    // instance buffer, on the same lazy terms as the world's interface; and
    // the ones in the scene or the ones over it into the open pass.
    [[nodiscard]] bool ensureAdornments(rhi::IDevice& device);
    void drawAdornments(rhi::ICmdList& cmd, const RenderWorld& world, bool onTop);
    // The sprite pipeline and its instance buffer (the 2D layer), on the same
    // lazy terms: a world with nothing on the plane builds neither.
    [[nodiscard]] bool ensureSprites(rhi::IDevice& device);
    // **Foliage** (ADR 0116): its cull and draw pipelines, made the first frame
    // a world has any -- so no other world's command stream grows a line.
    [[nodiscard]] bool ensureFoliage(rhi::IDevice& device);
    // The cull, outside any pass: counters cleared, every run culled into its
    // mesh's list, and the indirect draws' instance counts written.
    void cullFoliage(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes);
    // Which of a frame's foliage draws this is: the picture, a cascade, or one
    // of the decal pass's two masks (ADR 0185) -- the depth of the layers a
    // decal paints, and of the ones it does not.
    enum class FoliagePass : core::u8
    {
        Forward,
        Shadow,
        DecalReceiving,
        DecalRefusing,
    };
    // Every bucket's indirect draws, into the open pass.
    void drawFoliage(rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes, const Mat4& viewProjection,
                     FoliagePass pass);

    // **A pipeline the look needs (ADR 0096), made the first frame it is
    // used** -- as the decals' and the particles' are, and for their reason: a
    // world with none of these instances builds none of them, so its command
    // stream is the one it always was, shader for shader.
    struct LookPipeline
    {
        rhi::PipelineHandle handle{};
        bool tried = false;
    };
    // How a look pipeline writes: over what is there, added to it, or laid over
    // it as air -- `dst = src + dst * srcAlpha`, the colour the air adds and
    // the fraction of what is behind it that survives.
    enum class LookBlend : core::u8
    {
        Replace,
        Add,
        Air,
    };
    [[nodiscard]] bool ensureLookPipeline(rhi::IDevice& device, LookPipeline& slot, const char* shader,
                                          rhi::TextureFormat format, LookBlend blend = LookBlend::Replace);

    // Bakes whatever the environment owes this frame and uploads it. Called
    // once per frame, inside the frame, because `uploadTexture` needs a command
    // list and `create` has none.
    void updateEnvironment(rhi::ICmdList& cmd, const SkyParams& params);

    bool valid_ = false;
    rhi::TextureFormat colorFormat_ = rhi::TextureFormat::Undefined;

    // **The views not being drawn right now** (ADR 0107), keyed by the id a
    // `RenderTarget` carries, and which one the `ViewState` base holds. A frame
    // drawing only the main view never touches this.
    std::map<u32, ViewState> parkedViews_;
    u32 activeView_ = 0;
    // Brings a view's state into the base, putting the one there away.
    void useView(u32 view);
    // Frees the textures of the view in the base and forgets its history.
    void releaseActiveView(rhi::IDevice& device);
    // **The environment array** (ADR 0202): one texture for every view. Its
    // first layer is the sky of the view being drawn -- each view keeps its
    // own cache of the sky's levels, and a level is uploaded when what the
    // layer holds is not what this view's cache holds. Then two layers a
    // reflection probe (its reflection by roughness; its diffuse light at one
    // coarse level), and then the slices of the irradiance volumes, four to a
    // layer (ADR 0203). Made again when the settings change how many of
    // either it keeps.
    [[nodiscard]] bool ensureEnvironmentMap(rhi::IDevice& device);
    rhi::TextureHandle environmentMap_{};
    u32 environmentProbeSlots_ = 0;
    u32 environmentVolumeLayers_ = 0;
    core::u64 skyLevelHash_[kEnvironmentMipCount]{};
    bool skyLevelHeld_[kEnvironmentMipCount]{};
    struct ProbeSlot
    {
        core::u64 key = 0;
        // The last frame its map was in the table a frame's surfaces read.
        core::u64 lastUsed = 0;
        bool held = false;
    };
    std::vector<ProbeSlot> probeSlots_;
    struct VolumeSlot
    {
        core::u64 key = 0;
        u32 revision = 0;
        u32 first = 0;
        u32 layers = 0;
        core::u64 lastUsed = 0;
    };
    std::vector<VolumeSlot> volumeSlots_;
    // **The bake's grid of the sun** (ADR 0253 §4), in layers of the same
    // array as a volume's slices and placed among them, and its rows; and
    // whether this cascade of the sun's map leaves the baked parts out.
    f32 sunGridRows_[3][4]{};
    const std::vector<f32>* sunGridHeld_ = nullptr;
    // A frame brought the grid: the environment array keeps layers for it
    // even where it keeps none for volumes -- a standalone headset's.
    bool sunGridWanted_ = false;
    bool shadowLeavesBaked_ = false;
    // A probe's map while it is made: the six pictures gathered, with a plain
    // chain of levels under it; then a target a level, and one for the diffuse
    // light. And a layer of volume slices on its way into the array.
    rhi::TextureHandle probeScratch_{};
    rhi::TextureHandle probeLevels_[kEnvironmentMipCount]{};
    rhi::TextureHandle probeIrradiance_{};
    rhi::TextureHandle volumeScratch_{};
    LookPipeline probeGather_;
    LookPipeline probeFilter_;
    std::vector<core::u16> volumePixels_;
    core::u64 indirectFrame_ = 0;
    // The table this frame's surfaces read: `GpuFrameUniforms`' last rows.
    f32 indirectParams_[4]{};
    f32 probeRows_[kFrameReflectionProbes * 6][4]{};
    f32 volumeRows_[kFrameIrradianceVolumes * kVolumeRows][4]{};
    // Chooses the frame's probes and volumes, and takes a volume's samples
    // into its layers. Before any pass: it uploads.
    void prepareIndirect(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world);

public:
    [[nodiscard]] bool holdsReflectionProbe(core::u64 key) const noexcept override;
    bool storeReflectionProbe(rhi::IDevice& device, rhi::ICmdList& cmd, core::u64 key,
                              std::span<const rhi::TextureHandle, 6> faces,
                              std::span<const core::Mat4, 6> viewProjections) override;
    void dropReflectionProbe(core::u64 key) override;

private:
    // FXAA writing the views' format (`kLdrFormat`), made at `create` only
    // when the main target's format is a different one.
    rhi::PipelineHandle fxaaViewPipeline_{};

    rhi::PipelineHandle shadowPipeline_{};
    rhi::PipelineHandle pbrPipeline_{};
    // **The same pipeline for a draw the depth prepass has already drawn**
    // (ADR 0174): it tests against that depth and writes none. The forward
    // fragment can discard -- its alpha cutoff -- and a pipeline that both
    // discards and writes depth cannot have its depth test run before its
    // fragment: a tile-based GPU then shades every fragment of every triangle,
    // hidden or not, which is the whole of what the prepass was drawn to
    // prevent. With nothing to write, the test is early again. A cutout is not
    // in the prepass and keeps the pipeline above.
    rhi::PipelineHandle pbrPrepassedPipeline_{};
    rhi::PipelineHandle pbrSkinnedPrepassedPipeline_{};
    rhi::PipelineHandle pbrInstancedPrepassedPipeline_{};
    // A run of blended parts: the instanced shaders, blended and writing no
    // depth, as `pbrBlendPipeline_` is to the single draw.
    rhi::PipelineHandle pbrInstancedBlendPipeline_{};
    rhi::PipelineHandle pbrSkinnedInstancedPrepassedPipeline_{};
    // The skinned variants. Same shading, same state; what differs is the vertex
    // input layout and one more uniform block, both of which are pipeline
    // description rather than code.
    rhi::PipelineHandle shadowSkinnedPipeline_{};
    rhi::PipelineHandle pbrSkinnedPipeline_{};
    rhi::PipelineHandle pbrSkinnedBlendPipeline_{};
    // The same shader as `pbrPipeline_`, differing only in state: source-alpha
    // blending and no depth write. A fragment's colour does not depend on which
    // pass drew it; only the order and the state do.
    rhi::PipelineHandle pbrBlendPipeline_{};
    // Depth only, like the shadow pass, but culling BACK faces so the depth it
    // writes is the depth the forward pass will test against. The shadow pass
    // culls front faces on purpose and that would put every surface half a
    // thickness away here.
    rhi::PipelineHandle depthPrepassPipeline_{};
    rhi::PipelineHandle depthPrepassSkinnedPipeline_{};
    // The instanced variants: same shading, same state, a second vertex stream
    // that steps per instance.
    rhi::PipelineHandle shadowInstancedPipeline_{};
    rhi::PipelineHandle depthPrepassInstancedPipeline_{};
    rhi::PipelineHandle pbrInstancedPipeline_{};
    rhi::PipelineHandle skyPipeline_{};
    rhi::PipelineHandle ssaoPipeline_{};
    rhi::PipelineHandle ssaoBlurPipeline_{};
    rhi::PipelineHandle contactPipeline_{};
    rhi::PipelineHandle bloomDownPipeline_{};
    rhi::PipelineHandle bloomUpPipeline_{};
    rhi::PipelineHandle luminanceDownPipeline_{};
    rhi::PipelineHandle luminanceReducePipeline_{};
    rhi::PipelineHandle luminanceAdaptPipeline_{};
    rhi::PipelineHandle tonemapPipeline_{};
    // **The tonemap into the window itself**: with anti-aliasing
    // off the tonemap is the resolve, and it writes the swapchain, whose
    // format is the window's -- B8G8R8A8 -- where the plain pipeline declares
    // `kLdrFormat`. Drawing through a pipeline whose target format is not the
    // pass's is undefined, and fatal under the debug layer.
    rhi::PipelineHandle tonemapWindowPipeline_{};
    rhi::PipelineHandle fxaaPipeline_{};
    // The editor's selection silhouette. Four pipelines because the mask draws
    // the same three geometry variants everything else does, plus the fullscreen
    // pass that turns the mask into a line.
    rhi::PipelineHandle outlinePipeline_{};
    rhi::PipelineHandle outlineSkinnedPipeline_{};
    rhi::PipelineHandle outlineCompositePipeline_{};

    // Every shader handle this renderer created, so `destroy` can release them
    // without a second list. Sized with room: `create` silently stops recording
    // once it is full and the overflow leaks at shutdown, which is a bug that
    // announces itself nowhere.
    rhi::ShaderHandle shaders_[128]{};
    core::usize shaderCount_ = 0;

    rhi::TextureHandle shadowMap_{};
    // The atlas's rows: two by two, or the cascades side by side in one row
    // where a standalone headset's eye draws two of them at 2048 texels.
    u32 shadowRows_ = 2;
    u32 shadowRowsWanted_ = 2;
    // The things near the camera a headset's eye darkens round (frame
    // occluders), kept for their capacity.
    struct OccluderPick
    {
        f32 distanceSq = 0.0f;
        Vec3 centre;
        f32 radius = 0.0f;
    };
    std::vector<OccluderPick> occluderPicks_;
    // The fit a headset frame's first eye drew `shadowMap_` with, for the
    // second (`ShadowView::reuse`). Not a view's: the two eyes are two views
    // and one map.
    ShadowCascades headsetShadowFit_{};
    bool headsetShadowDrawn_ = false;
    // The exposure a headset frame's first eye metered, which the second
    // reads (`ShadowView::reuse`). Owned by that eye's view.
    rhi::TextureHandle headsetExposure_{};
    // **The meter's adapted value, read back a few frames late** for a
    // headset's eyes with the tonemap folded in (ADR 0250): the renderer's,
    // not a view's -- the second eye meters nothing and draws at the first's.
    // Whether one has come, and the tickets asked for.
    f32 foldMeasured_ = 0.0f;
    bool foldMeasuredKnown_ = false;
    std::vector<u32> foldReadbacks_;
    // The local-light atlas and this frame's assignment of its tiles. Held
    // across frames only so the vector behind `localCandidates_` keeps its
    // capacity; nothing in either survives a frame.
    rhi::TextureHandle localShadowMap_{};
    LocalShadows localShadows_{};
    std::vector<LocalShadowCandidate> localCandidates_;
    // 1x1 stand-ins for a material that has no map. `textureFlags` are
    // multipliers rather than branches, so the shader samples every slot
    // whatever the flag says -- and an unbound descriptor read is not a black
    // pixel, it is whatever the backend last left in that slot.
    rhi::TextureHandle whitePixel_{};
    rhi::TextureHandle flatNormalPixel_{};
    rhi::TextureHandle blackPixel_{};
    // **Nothing at all**: black, and not there. The black pixel above is
    // opaque -- "adds nothing" to a colour -- and what stands in for a picture
    // whose alpha is how much of an answer it holds has to say "none" (ADR
    // 0209): handed the opaque one, a blended draw took black for the
    // screen's whole answer and lost the sky's reflection.
    rhi::TextureHandle nothingPixel_{};
    // The terrain's array slots while a terrain's own arrays are not built:
    // an array type there is what the shader declares, and it reads flat
    // colours instead until they are (ADR 0113).
    rhi::TextureHandle whiteArray_{};
    rhi::TextureHandle flatNormalArray_{};
    // The prefiltered environment and the split-sum BRDF table: image-based
    // lighting's two textures (ADR 0038, environment.h). Octahedral rather than
    // a cubemap because the frozen RHI has no cube type, and CPU-prefiltered
    // because it has no compute -- ADR 0043 records what that bought.
    rhi::TextureHandle brdfLut_{};
    // The clustered light tables (clusters.h). Uploaded whole every frame --
    // ninety kilobytes between them -- because `uploadTexture` writes a whole
    // mip and because a frame whose command shape depended on whether the lights
    // moved is exactly what `clock_differential` refuses.
    rhi::TextureHandle clusterGrid_{};
    rhi::TextureHandle lightIndices_{};
    rhi::TextureHandle lightData_{};
    ClusterGrid clusters_;
    // The per-instance vertex stream, and the plan that indexes it. Rebuilt
    // every frame, uploaded once before any render pass -- uploading inside one
    // is what `rhi.err.upload_inside_pass` refuses, and rightly.
    // Scratch for the shadow fit, kept across frames so a frame allocates
    // nothing for it.
    std::vector<ShadowCasterBounds> casterBounds_;

    rhi::BufferHandle instanceBuffer_{};
    std::vector<GpuInstance> instanceStaging_;
    // **The light baked into what stands still** (ADR 0253). A float4 an
    // instance, parallel to `instanceStaging_` -- where its part's rows
    // begin, and whether the mask holds -- read by a baked run alone; the
    // pages, one a layer, the mask in alpha, uploaded when the bake is
    // another; and every baked part's rows, which the vertex stage reads.
    void uploadLightmap(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderLightmap& lightmap);
    void releaseLightmap(rhi::IDevice& device);
    rhi::BufferHandle bakedInstanceBuffer_{};
    std::vector<std::array<f32, 4>> bakedInstanceStaging_;
    bool anyBakedRun_ = false;
    rhi::TextureHandle lightmapPages_{};
    rhi::BufferHandle lightmapRects_{};
    u32 lightmapPageSize_ = 0;
    u32 lightmapPageCount_ = 0;
    // Layers in all, and a page's: two where its mask is the layer after its
    // light (`uploadLightmap`).
    u32 lightmapLayers_ = 0;
    u32 lightmapLayersPerPage_ = 1;
    rhi::TextureFormat lightmapFormat_ = rhi::TextureFormat::Undefined;
    u32 lightmapRectsBytes_ = 0;
    u64 lightmapStamp_ = 0;
    std::vector<core::u16> lightmapPixels_;
    std::vector<std::byte> lightmapBlocks_;
    std::vector<std::byte> lightmapMaskBlocks_;
    rhi::PipelineHandle pbrBakedInstancedPipeline_{};
    rhi::PipelineHandle pbrBakedInstancedPrepassedPipeline_{};
    // Every run's members' bounds, in the order staged (ADR 0182), and the
    // scratch a run is put in order with.
    std::vector<ShadowCasterBounds> batchBounds_;
    std::vector<ShadowCasterBounds> runBounds_;
    std::vector<core::u8> runInFrustum_;
    std::vector<u32> runOrder_;
    // Skinned runs: their instance stream, the frame's palettes, and the
    // three pipelines that read them.
    bool skinnedInstancingTried_ = false;
    rhi::BufferHandle skinnedInstanceBuffer_{};
    rhi::BufferHandle paletteBuffer_{};
    std::vector<GpuSkinnedInstance> skinnedInstanceStaging_;
    rhi::PipelineHandle pbrSkinnedInstancedPipeline_{};
    rhi::PipelineHandle shadowSkinnedInstancedPipeline_{};
    rhi::PipelineHandle depthPrepassSkinnedInstancedPipeline_{};
    // **A body with a morph target above nought** (ADR 0196) is drawn alone
    // through these: the built-in passes with the vertex moved by its
    // targets first. A set for a mesh with no skeleton and one for a skinned
    // one; each the lit pass (writing depth, and testing what the prepass
    // wrote), the blended pass, the shadow maps and the camera's prepass.
    struct MorphPipelines
    {
        rhi::PipelineHandle forward{};
        rhi::PipelineHandle forwardPrepassed{};
        rhi::PipelineHandle blend{};
        rhi::PipelineHandle shadow{};
        rhi::PipelineHandle prepass{};
    };
    std::array<MorphPipelines, 2> morphPipelines_{};
    bool morphTried_ = false;
    // Every pipeline of both sets exists. Until then a body with weights is
    // drawn at rest, in its run, as if it had none.
    bool morphReady_ = false;
    // **The frame's cloth rows** (ADR 0214, `RenderWorld::clothRows`), read by
    // the morph pipelines as a cloth's one target. Made the first frame there
    // is a cloth, at its whole size: `kMaxClothRows` rows.
    rhi::BufferHandle clothBuffer_{};
    std::vector<InstanceBatch> batches_;
    // Per draw: which batch covers it, or `kNoBatch`.
    std::vector<u32> batchOf_;
    // What the frame actually submitted, for the stat that says whether any of
    // this did anything.
    RendererStats stats_;
    rhi::SamplerHandle linearSampler_{};
    rhi::SamplerHandle shadowSampler_{};
    // Trilinear and clamped: the mip index IS the roughness, so filtering
    // between levels is the interpolation the split sum asks for rather than a
    // quality setting.
    rhi::SamplerHandle environmentSampler_{};
    // **What the lit pass is handed of the screen** (ADR 0209): the view's own
    // two pictures, or a black pixel -- "no answer" -- while they are off, on
    // the first frame and after a cut.
    // A blended pass is handed no reflection: what it would read at its pixel
    // is the landing of the opaque surface BEHIND it.
    [[nodiscard]] rhi::TextureBinding screenReflectionBinding(bool blendedPass) const noexcept
    {
        const bool there = screenReflection_.valid() && screenReflectionReady_ && !blendedPass;
        return rhi::TextureBinding{there ? screenReflection_ : nothingPixel_, environmentSampler_};
    }
    [[nodiscard]] rhi::TextureBinding screenBounceBinding() const noexcept
    {
        const bool there = screenBounce_.valid() && screenBounceReady_;
        return rhi::TextureBinding{there ? screenBounce_ : nothingPixel_, linearSampler_};
    }
    // Point and clamped: the cluster tables are looked up by exact texel, and
    // filtering between two light offsets would be a light index that does not
    // exist.
    rhi::SamplerHandle pointSampler_{};

    GraphicsSettings settings_;
    // The tile resolution `shadowMap_` was created for, so a settings change
    // rebuilds it and a repeated one does not.
    u32 shadowTile_ = 0;

    bool defaultsUploaded_ = false;
    bool brdfUploaded_ = false;

    // The terrain's pipelines, made by `ensureTerrain`: the forward pass with
    // the terrain's look, and the shadow pass with no culling and a push away
    // from the light. The depth prepass draws a terrain as the static mesh it
    // is.
    const ShaderLibrary* shaderLibrary_ = nullptr;
    bool terrainTried_ = false;
    bool terrainValid_ = false;
    rhi::PipelineHandle terrainPipeline_{};
    rhi::PipelineHandle terrainNoHexPipeline_{};
    rhi::PipelineHandle terrainTriplanarNoHexPipeline_{};
    bool terrainNoHexTried_ = false;
    bool terrainNoHexNow_ = false;
    bool terrainTriplanarNoHexTried_ = false;
    bool terrainTriplanarNoHexNow_ = false;
    // The ground's compiled variants (ADR 0179): made when a frame first asks
    // for one, and the one a frame's terrain draws go through.
    rhi::PipelineHandle terrainFastPipeline_{};
    rhi::PipelineHandle terrainFlatPipeline_{};
    // **Each of the ground's forward pipelines is made when a frame first
    // draws with it** (ADR 0179), the full one included: its fragment is
    // seven hundred kilobytes compiled, and a second of a phone's loading
    // that a game on the fast ground never needs.
    rhi::ShaderHandle terrainVertex_{};
    rhi::ShaderHandle terrainFragment_{};
    bool terrainFullTried_ = false;
    bool terrainFastTried_ = false;
    bool terrainFlatTried_ = false;
    void ensureTerrainForward(rhi::IDevice& device, const RenderWorld& world);
    [[nodiscard]] rhi::PipelineHandle makeTerrainForward(rhi::IDevice& device, rhi::ShaderHandle vertex,
                                                         rhi::ShaderHandle fragment, const char* debugName);
    // Which of them a frame's terrain draws go through, by the surface asked
    // for; and that one, once made.
    [[nodiscard]] GraphicsSettings::TerrainSurface terrainForwardWanted() const noexcept;
    [[nodiscard]] rhi::PipelineHandle terrainForwardPipeline() const noexcept;
    rhi::PipelineHandle terrainShadowPipeline_{};
    // A terrain's layer arrays drawn from its materials' maps
    // (`terrain_pack.hlsl`): into the sRGB colour array, and into the normal
    // and surface arrays.
    rhi::PipelineHandle terrainPackColorPipeline_{};
    rhi::PipelineHandle terrainPackLinearPipeline_{};
    // The terrain into the depth prepass as it is drawn: slid by its geomorph
    // (ADR 0140), where the static mesh's prepass would write the depth of
    // where it was, and the forward pass then lose to it.
    rhi::PipelineHandle terrainPrepassPipeline_{};
    // How far the shadow pass being drawn pushes the terrain from the light
    // (`terrain_shadow.hlsl`): set per cascade, and for a local light's tile.
    TerrainShadowPush terrainShadowPush_{};
    // The block world's forward pipeline, made the first frame a block is
    // drawn, and the palette it reads, filled each frame from the registry.
    rhi::PipelineHandle voxelPipeline_{};
    // The blocks the prepass drew -- every one but a leaf's cutout -- tested
    // against its depth and writing none (ADR 0174).
    rhi::PipelineHandle voxelPrepassedPipeline_{};
    // The same shader, blended and not writing depth, for glass and water.
    rhi::PipelineHandle voxelBlendPipeline_{};
    // The cutout faces' shadow: depth only, with the forward pass's hole test,
    // so a leaf block casts the leaves rather than a square.
    rhi::PipelineHandle voxelShadowPipeline_{};
    bool voxelTried_ = false;
    // The block registry, by id minus one, and the buffer the vertex stage
    // reads it from, put up when it changes; and the block size.
    std::array<GpuVoxelBlock, kVoxelPaletteSize> voxelBlocks_{};
    std::array<GpuVoxelBlock, kVoxelPaletteSize> voxelBlocksUp_{};
    bool voxelBlocksSent_ = false;
    rhi::BufferHandle voxelBlockBuffer_{};
    GpuVoxelParams voxelParams_{};
    // The block atlas: one tile per distinct block image, filled by
    // drawing each image into its square the first frame it is loaded -- which
    // is what lets it hold compiled images the CPU has no pixels for.
    rhi::TextureHandle voxelAtlas_{};
    rhi::PipelineHandle voxelTilePipeline_{};
    // Which texture is in which tile, by handle, in the order they arrived.
    std::vector<std::pair<u32, u32>> voxelTiles_;
    // Particles: the pipeline, made the first frame one is drawn, and
    // the instance stream this frame uploads into.
    rhi::PipelineHandle particlePipeline_{};
    // Decals, made the first frame one is drawn.
    rhi::PipelineHandle decalPipeline_{};
    // The same decal laid over and added (ADR 0160).
    rhi::PipelineHandle decalAlphaPipeline_{};
    rhi::PipelineHandle decalAddPipeline_{};
    bool decalTried_ = false;
    rhi::BufferHandle particleBuffer_{};
    // World-space UI: one pipeline tested against depth and one that is
    // not, for `AlwaysOnTop`, and a vertex buffer of `MaxWorldUiVertices`.
    rhi::PipelineHandle worldUiPipeline_{};
    rhi::PipelineHandle worldUiOnTopPipeline_{};
    // **The shapes drawn on parts** (ADR 0251): the unit meshes in one buffer,
    // where each shape's indices are, this frame's instances in the order
    // they are drawn and the runs of them one draw each, and the pipelines in
    // the scene and over it. Made the first frame a shape is drawn.
    rhi::PipelineHandle adornmentPipeline_{};
    rhi::PipelineHandle adornmentOnTopPipeline_{};
    rhi::BufferHandle adornmentVertices_{};
    rhi::BufferHandle adornmentIndices_{};
    rhi::BufferHandle adornmentInstances_{};
    bool adornmentsTried_ = false;
    bool adornmentGeometryUp_ = false;
    std::vector<core::Vec3> adornmentCorners_;
    std::vector<core::u16> adornmentCornerIndices_;
    struct AdornmentShape
    {
        u32 firstIndex = 0;
        u32 indexCount = 0;
    };
    std::array<AdornmentShape, 5> adornmentShapes_{};
    struct AdornmentRun
    {
        u32 first = 0;
        u32 count = 0;
        AdornmentMesh mesh = AdornmentMesh::Box;
        rhi::TextureHandle texture;
        bool onTop = false;
    };
    std::vector<AdornmentRun> adornmentRuns_;
    std::vector<GpuAdornment> adornmentStaging_;
    std::vector<u32> adornmentOrder_;
    u32 adornmentCount_ = 0;
    rhi::BufferHandle worldUiBuffer_{};
    bool worldUiTried_ = false;
    u32 worldUiVertexCount_ = 0;
    // Foliage (ADR 0116): the pipelines, and the buffers the cull writes --
    // grown when a frame needs more and never shrunk within a run.
    rhi::ComputePipelineHandle foliageCullPipeline_{};
    rhi::ComputePipelineHandle foliageFinalizePipeline_{};
    rhi::PipelineHandle foliagePipeline_{};
    rhi::PipelineHandle foliageShadowPipeline_{};
    // The shadow's shader into the camera's depth: a decal mask (ADR 0185).
    rhi::PipelineHandle foliageDecalPipeline_{};
    bool foliageTried_ = false;
    rhi::BufferHandle foliageVisible_{};
    u32 foliageVisibleCapacity_ = 0;
    rhi::BufferHandle foliageCounters_{};
    rhi::BufferHandle foliageArguments_{};
    rhi::BufferHandle foliageCommands_{};
    u32 foliageCommandCapacity_ = 0;
    u32 foliageBucketCapacity_ = 0;
    // This frame's: each bucket's first slot in the visible list, how many
    // levels it is drawn at, and each of its levels' first indirect draw
    // (`bucket * kFoliageMaxLods + level`).
    std::vector<u32> foliageBucketBase_;
    std::vector<u32> foliageBucketLods_;
    std::vector<u32> foliageLodCommand_;
    bool foliageCulled_ = false;
    bool particleTried_ = false;
    std::vector<GpuParticle> particleStaging_;
    // Consecutive particles sharing a bounded set of pictures, in depth order.
    std::vector<ParticleBatch> particleRuns_;

    // **Particles simulated on the GPU** (ADR 0160): a buffer an emitter, kept
    // from frame to frame and stepped by a compute pass, and drawn from where
    // it is. `origin` is what its positions are measured from, in doubles
    // here; `head` is the next slot born into, round the buffer.
    struct GpuEmitterBuffer
    {
        rhi::BufferHandle buffer;
        u32 capacity = 0;
        u32 head = 0;
        core::DVec3 origin;
        // The particle system's update it was last stepped for, and the frame
        // it was last asked for: unasked, it is let go.
        u64 serial = 0;
        u64 frame = 0;
    };
    std::map<u64, GpuEmitterBuffer> gpuEmitters_;
    rhi::ComputePipelineHandle particleSimPipeline_{};
    rhi::PipelineHandle particleGpuPipeline_{};
    bool particleGpuTried_ = false;
    // The ground's heights, as the particle system last made them.
    rhi::TextureHandle particleGround_{};
    u32 particleGroundRevision_ = 0;
    u32 particleGroundCells_ = 0;
    // The main view's camera a frame ago: what the depth it left behind was
    // drawn through, for particles that collide with what is seen.
    Mat4 simPrevViewProjection_{};
    core::DVec3 simPrevOrigin_{};
    f32 simPrevDepth_[2]{};
    bool simPrevValid_ = false;
    u64 gpuParticleFrame_ = 0;
    [[nodiscard]] bool ensureGpuParticles(rhi::IDevice& device);
    void simulateGpuParticles(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world);
    void releaseGpuParticles(rhi::IDevice& device);
    u32 particleCount_ = 0;
    // Beams and trails (ADR 0129): the pipeline and the buffer this frame's
    // vertices are uploaded into, made the first frame there is a ribbon.
    rhi::PipelineHandle ribbonPipeline_{};
    rhi::BufferHandle ribbonBuffer_{};
    bool ribbonTried_ = false;
    u32 ribbonVertexCount_ = 0;
    // An occluded `Highlight`'s mask pipelines, and which highlight the mask
    // pass is drawing (one more than its place; a draw carries the same).
    rhi::PipelineHandle highlightMaskPipeline_{};
    rhi::PipelineHandle highlightMaskSkinnedPipeline_{};
    bool highlightTried_ = false;
    core::u8 highlightFilter_ = 0;
    // The 2D layer's sprites: one instance each, drawn in runs that share an
    // image and a filter.
    rhi::PipelineHandle spritePipeline_{};
    rhi::BufferHandle spriteBuffer_{};
    bool spriteTried_ = false;
    std::vector<GpuSprite> spriteStaging_;
    u32 spriteCount_ = 0;
    // **Sprites drawn in their own colours** (ADR 0153): the same instances
    // through a pipeline with a second target -- the view's `spriteMask_` --
    // which the bloom's first level and the resolve then read. Every piece is
    // made the first frame a sprite asks for it, so a world without one builds
    // none of them and draws through the command stream it always had.
    rhi::PipelineHandle spriteExactPipeline_{};
    bool spriteExactTried_ = false;
    [[nodiscard]] bool ensureSpritesExact(rhi::IDevice& device);
    // Whether THIS frame of this view wrote the mask.
    bool spriteExactLive_ = false;
    // **Each terrain's layers** (ADR 0113): three arrays with a slice per
    // layer, built by blitting the layers' own textures when every one has
    // loaded, and rebuilt only when the set of textures changes; and the block
    // its shader reads.
    struct TerrainArrays
    {
        core::InstanceId id;
        std::vector<u32> sources;
        std::array<rhi::TextureHandle, 3> arrays{};
        bool ready = false;
        // The draws in a row that did not have this terrain. **Given back only
        // after many**: a `ViewportFrame` or a sub-world draws a
        // world with no ground between two draws of the main one, and giving
        // the arrays back on the first rebuilt ~17 MB of them -- blits, mips --
        // every frame a preview spun.
        u32 unseen = 0;
        GpuTerrainSurfaceUniforms uniforms{};
        // Its layers (`GpuTerrainLayer`), and the buffer the fragment stage
        // reads them from, put up when they change.
        std::vector<GpuTerrainLayer> layers;
        std::vector<GpuTerrainLayer> layersUp;
        rhi::BufferHandle layerBuffer{};
    };
    static constexpr u32 TerrainArraysKeptUnseen = 240;
    std::vector<TerrainArrays> terrainArrays_;
    void updateTerrainArrays(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world);
    void releaseTerrainArrays(rhi::IDevice& device, TerrainArrays& entry);
    // What a terrain's draws bind for its layers while it has none built: one
    // buffer of plain layers, for every terrain.
    rhi::BufferHandle terrainPlainLayers_{};
    [[nodiscard]] const TerrainArrays* terrainArraysOf(core::InstanceId id) const noexcept;

    // --- The look (ADR 0096) --------------------------------------------------
    //
    // `tonemap.hlsl` with every colour correction folded in, for a frame that
    // has one.
    LookPipeline gradedTonemap_;
    // The same, into the window, for a frame that grades with
    // anti-aliasing off.
    LookPipeline gradedTonemapWindow_;
    // The four resolves again, reading the sprites' mask (ADR 0153), and the
    // bloom's first level leaving those pixels out.
    LookPipeline exactTonemap_;
    LookPipeline exactTonemapWindow_;
    LookPipeline exactGradedTonemap_;
    LookPipeline exactGradedTonemapWindow_;
    LookPipeline bloomDownMasked_;
    // One direction of a separable Gaussian, and a filtered copy from one size
    // to another.
    LookPipeline blur_;
    LookPipeline resample_;
    // Depth of field's three passes.
    LookPipeline focusPrepare_;
    LookPipeline focusGather_;
    LookPipeline focusComposite_;
    // Sun rays' two passes, and the resample again with an additive blend, to
    // lay the shafts over the frame without reading it.
    LookPipeline raysMask_;
    LookPipeline raysGather_;
    LookPipeline raysAdd_;
    // The air, laid over the opaque world and the sky.
    LookPipeline air_;
    // The scene behind blended surface shaders (ADR 0091): its depth as
    // distances, and its colour -- each made only in a frame that needs it.
    LookPipeline surfaceSceneDepth_;
    // Each pixel's mirror ray through the screen (ADR 0209), and the few
    // rays it casts for the light that bounces to it.
    LookPipeline screenTrace_;
    LookPipeline screenBouncePass_;
    // The sky a `Sky` governs. Made by `ensureSkyLook` rather than as a
    // fullscreen pass: it is drawn INSIDE the forward pass, so it declares
    // that pass's depth format as the plain sky's pipeline does.
    LookPipeline skyLook_;
    [[nodiscard]] bool ensureSkyLook(rhi::IDevice& device);

    // --- Anti-aliasing and upscaling (ADR 0158) --------------------------------
    //
    // SMAA's three passes -- its last into a texture or into the window --
    // FSR 1's two, the camera's motion and the temporal resolve: each made the
    // first frame it is drawn, like every look pipeline.
    LookPipeline smaaEdgesPipeline_;
    LookPipeline smaaWeightsPipeline_;
    LookPipeline smaaBlend_;
    LookPipeline smaaBlendWindow_;
    LookPipeline smaaBlendExact_;
    LookPipeline smaaBlendExactWindow_;
    LookPipeline easu_;
    LookPipeline rcas_;
    LookPipeline rcasWindow_;
    LookPipeline taaVelocity_;
    LookPipeline taaResolve_;
    // What moves, drawn with where it was: plain and skinned.
    rhi::PipelineHandle motionPipeline_{};
    rhi::PipelineHandle motionSkinnedPipeline_{};
    // A headset eye's motion for its runtime (ADR 0252), against the depth
    // format of the runtime's images.
    rhi::PipelineHandle warpMotionPipeline_{};
    rhi::PipelineHandle warpMotionSkinnedPipeline_{};
    // The world's interface as solid quads (ADR 0252 §4): in the scene, and
    // over it.
    rhi::PipelineHandle warpMotionUiPipeline_{};
    rhi::PipelineHandle warpMotionUiOnTopPipeline_{};
    rhi::TextureFormat warpMotionDepthFormat_ = rhi::TextureFormat::Undefined;
    bool motionTried_ = false;
    // SMAA's two tables, uploaded once.
    rhi::TextureHandle smaaArea_{};
    rhi::TextureHandle smaaSearch_{};
    bool smaaTablesUploaded_ = false;
    // Whether this frame is a temporal one.
    bool temporalNow_ = false;
    // Whether the view being drawn is a part of the window (ADR 0205): drawn
    // as the main view is, FSR 2 and frame generation apart.
    bool windowPart_ = false;
    // The side of a line a view is seen on, written into the depth before
    // anything is drawn (ADR 0205). Made the first frame a view has a side.
    rhi::PipelineHandle viewSidePipeline_{};
    bool viewSideTried_ = false;
    [[nodiscard]] bool ensureViewSide(rhi::IDevice& device);
    // Several views into one target, and into the window.
    LookPipeline composeViews_;
    LookPipeline composeViewsWindow_;
    // The headset's eyes (ADR 0244): made in the headset's image's format
    // when a headset first draws -- and the mirror's copy of an eye into the
    // window, in the window's. Two pipelines, because the two formats are not
    // one: the mirror once copied through the headset's, and Direct3D 12's
    // debug layer said so every frame.
    LookPipeline xrEye_;
    rhi::TextureFormat xrEyeFormat_ = rhi::TextureFormat::Undefined;
    LookPipeline xrEyeMirror_;
    // The tonemap into a headset's image, in its format where that is not
    // the window's: an eye drawn straight into the runtime's image.
    LookPipeline tonemapEye_;
    LookPipeline gradedTonemapEye_;
    // The eyes' depth copy (ADR 0244 §5), and the depth format it was made for.
    LookPipeline xrDepth_;
    rhi::TextureFormat xrDepthFormat_ = rhi::TextureFormat::Undefined;
    // The two skies, for a view seen on one side of a line.
    rhi::PipelineHandle skySidedPipeline_{};
    LookPipeline skyLookSided_;

    // --- The temporal upscaler (ADR 0164) --------------------------------------
    //
    // **FSR 2**: AMD's algorithm, its passes compiled from its own headers
    // (`shaders/compute/fsr2_*.hlsl`), and the images it keeps from one frame
    // to the next. The main view's alone, as the temporal pass is, and made
    // the first frame the setting asks for it. **It takes the temporal pass's
    // place and the upscale's**: what it writes is the scene at the target's
    // size, still unexposed, and exposure, bloom and the tonemap read that.
    rhi::ComputePipelineHandle fsr2Pipelines_[kFsr2PassCount]{};
    bool fsr2Tried_ = false;
    // A device that could not make the passes or their images, said once.
    bool fsr2Failed_ = false;
    // Whether this frame is upscaled by it.
    bool fsr2Now_ = false;
    // At the render size: the colour as the accumulation reads it, the depth
    // the last frame's geometry would have here, each pixel's motion and
    // depth taken from the nearest of its neighbours -- this frame's motion
    // and the last one's -- the luminance thin features are found in, the
    // masks that say where history is not to be trusted, and the frame's
    // luminance in patches thirty-two pixels a side.
    rhi::TextureHandle fsr2Prepared_{};
    rhi::TextureHandle fsr2PreviousDepth_{};
    rhi::TextureHandle fsr2DilatedMotion_[2]{};
    rhi::TextureHandle fsr2DilatedDepth_{};
    rhi::TextureHandle fsr2LockLuma_{};
    rhi::TextureHandle fsr2Masks_{};
    rhi::TextureHandle fsr2Luminance_{};
    // **And what the passes are told blends**: the scene as it was before
    // anything that does, and how far each pixel of the finished one is from
    // it (`fsr2_reactive.hlsl`). Made on a frame that has something blended,
    // and for a few frames after, each keeping a part of the last one's mask:
    // `fsr2ReactiveLive_` says this frame's mask is this frame's, and
    // `fsr2ReactiveTail_` how many more frames the last one counts for.
    rhi::TextureHandle fsr2Opaque_{};
    rhi::TextureHandle fsr2Reactive_[2]{};
    u32 fsr2ReactiveIndex_ = 0;
    u32 fsr2ReactiveTail_ = 0;
    bool fsr2OpaqueLive_ = false;
    bool fsr2ReactiveLive_ = false;
    LookPipeline fsr2ReactivePipeline_;
    // At the output's: the locks on thin features and the ones this frame
    // found, the history, its luminance over the last four frames, and what
    // the frame reads.
    rhi::TextureHandle fsr2LockStatus_[2]{};
    rhi::TextureHandle fsr2NewLocks_{};
    rhi::TextureHandle fsr2History_[2]{};
    rhi::TextureHandle fsr2LumaHistory_[2]{};
    rhi::TextureHandle fsr2Output_{};
    u32 fsr2RenderWidth_ = 0;
    u32 fsr2RenderHeight_ = 0;
    u32 fsr2OutputWidth_ = 0;
    u32 fsr2OutputHeight_ = 0;
    // Which of each pair is read this frame, how many frames the history
    // holds, and the length of the jitter's sequence as the passes are told
    // it: a step a frame towards what the scale asks for.
    u32 fsr2Parity_ = 0;
    core::i32 fsr2FrameIndex_ = 0;
    f32 fsr2Phases_ = 0.0f;
    bool fsr2Fresh_ = true;
    [[nodiscard]] bool ensureFsr2(rhi::IDevice& device);
    [[nodiscard]] bool ensureFsr2Images(rhi::IDevice& device, u32 outputWidth, u32 outputHeight);
    void releaseFsr2Images(rhi::IDevice& device);
    void failFsr2();
    // The scene before what blends, kept; and, the forward pass over, the mask.
    void copyOpaqueForUpscaler(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                               const GpuFrameUniforms& frame);
    void writeReactiveMask(rhi::IDevice& device, rhi::ICmdList& cmd);
    // This frame and the ones before it, at the target's size -- or nothing,
    // where the images could not be made.
    [[nodiscard]] rhi::TextureHandle upscaleTemporal(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                                     const RenderTarget& target, rhi::TextureHandle scene);
    // What this frame was, for the next one's motion to be measured against.
    void rememberCamera(const RenderCamera& camera);

    // --- Frame generation (ADR 0165) -------------------------------------------
    //
    // **FSR 3's**: AMD's optical flow and frame interpolation, each its
    // passes compiled from its own headers (`shaders/compute/fsr3_*.hlsl`),
    // and the images they keep. The main view's alone. Between the picture
    // of the world the host drew last frame and the one it has just drawn, a
    // third is made: each pixel carried half its motion -- the game's own,
    // which the velocity pass measured, and what optical flow found in the
    // two pictures where the game's says nothing (a shadow, a reflection,
    // what blends).
    rhi::ComputePipelineHandle fgPipelines_[kFgPassCount]{};
    bool fgTried_ = false;
    // Said once: a device that cannot (`failFrameGeneration`).
    bool fgFailed_ = false;
    // What the device is, learnt when the renderer was made.
    bool fgCompute_ = false;
    bool fgAtomics_ = false;
    // Whether this frame's main view is drawn for it, and whether its camera
    // has nothing before it to have moved from.
    bool fgNow_ = false;
    bool motionCut_ = true;
    // The camera's part of the interpolation's constants, kept by `render`
    // for `interpolateFrame`, which has no world.
    GpuFrameInterpolationConstants fgCamera_;
    // Optical flow: the picture as a luminance at seven sizes, this frame's
    // and the last one's; the motion found, two of each size because a pass
    // reads one and writes the other; and the histograms a cut is found by.
    rhi::TextureHandle fgLuma_[2][kFlowLevels]{};
    rhi::TextureHandle fgFlow_[2][kFlowLevels]{};
    rhi::TextureHandle fgScdHistogram_{};
    rhi::TextureHandle fgScdPrevious_{};
    rhi::TextureHandle fgScdTemp_{};
    rhi::TextureHandle fgScdOutput_{};
    // Interpolation, at the render size: each pixel's motion and depth from
    // its nearest neighbour, that depth where the pixel was and where it is
    // half way, the two motion fields as seen from the frame between, and
    // what that frame cannot take from each of its neighbours.
    rhi::TextureHandle fgDilatedMotion_{};
    rhi::TextureHandle fgDilatedDepth_{};
    rhi::TextureHandle fgPreviousDepth_{};
    rhi::TextureHandle fgBetweenDepth_{};
    rhi::TextureHandle fgGameField_[2]{};
    rhi::TextureHandle fgFlowField_[2]{};
    rhi::TextureHandle fgDisocclusion_{};
    // The two pyramids holes are filled from, each an image with every level
    // and a chain of images a level each (`fsr3_fi_vector_pyramid.hlsl`).
    rhi::TextureHandle fgVectorPyramid_{};
    rhi::TextureHandle fgColourPyramid_{};
    std::vector<rhi::TextureHandle> fgVectorLevels_;
    std::vector<rhi::TextureHandle> fgColourLevels_;
    // At the picture's size: the frame between as interpolated, and with its
    // holes filled.
    rhi::TextureHandle fgInterpolated_{};
    rhi::TextureHandle fgOutput_{};
    // Two counts the passes keep: one unused here, and the frames since a cut.
    rhi::BufferHandle fgCounters_{};
    u32 fgRenderWidth_ = 0;
    u32 fgRenderHeight_ = 0;
    u32 fgWidth_ = 0;
    u32 fgHeight_ = 0;
    u32 fgParity_ = 0;
    u32 fgFrameIndex_ = 0;
    bool fgFresh_ = true;
    [[nodiscard]] bool ensureFrameGeneration(rhi::IDevice& device);
    [[nodiscard]] bool ensureFrameGenerationImages(rhi::IDevice& device, u32 width, u32 height);
    void releaseFrameGenerationImages(rhi::IDevice& device);
    void failFrameGeneration(core::TextKey why);
    // A picture copied onto a target: into a texture, and into the window.
    LookPipeline showPicture_;
    LookPipeline showPictureWindow_;
    // `--debug-view=motion`: `velocity_` over the finished picture.
    LookPipeline motionView_;
    LookPipeline motionViewWindow_;
    void showMotion(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target);
    [[nodiscard]] bool ensureMotionPipelines(rhi::IDevice& device);
    [[nodiscard]] bool ensureWarpMotionPipelines(rhi::IDevice& device, rhi::TextureFormat depthFormat);
    // **A headset eye's motion and depth for its runtime** (ADR 0252).
    void drawWarpMotion(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes,
                        const RenderTarget& target);
    [[nodiscard]] bool ensureSmaaTables(rhi::IDevice& device, rhi::ICmdList& cmd);
    [[nodiscard]] static bool aaTexture(rhi::IDevice& device, rhi::TextureHandle& slot, u32 width, u32 height,
                                        rhi::TextureFormat format, const char* name);
    void releaseAaTextures(rhi::IDevice& device);
    // How far every pixel moved since the last frame, into `velocity_`.
    void writeVelocity(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes);
    // This frame blended into its history; the image every pass after reads.
    [[nodiscard]] rhi::TextureHandle resolveTemporal(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                                     rhi::TextureHandle scene);
    // From the tonemapped `ldr_` to the target: the spatial pass, the upscale
    // and the sharpening, as asked.
    void resolvePicture(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target, AntiAliasingMode spatial,
                        bool upscale, bool sharpen);

    // Every look pipeline, for `destroy`.
    [[nodiscard]] std::array<LookPipeline*, 43> lookPipelines() noexcept
    {
        return {&probeGather_,
                &probeFilter_,
                &gradedTonemap_,
                &gradedTonemapWindow_,
                &blur_,
                &resample_,
                &focusPrepare_,
                &focusGather_,
                &focusComposite_,
                &raysMask_,
                &raysGather_,
                &raysAdd_,
                &air_,
                &surfaceSceneDepth_,
                &skyLook_,
                &exactTonemap_,
                &exactTonemapWindow_,
                &exactGradedTonemap_,
                &exactGradedTonemapWindow_,
                &bloomDownMasked_,
                &smaaEdgesPipeline_,
                &smaaWeightsPipeline_,
                &smaaBlend_,
                &smaaBlendWindow_,
                &smaaBlendExact_,
                &smaaBlendExactWindow_,
                &easu_,
                &rcas_,
                &rcasWindow_,
                &taaVelocity_,
                &taaResolve_,
                &fsr2ReactivePipeline_,
                &showPicture_,
                &showPictureWindow_,
                &composeViews_,
                &composeViewsWindow_,
                &xrEye_,
                &xrEyeMirror_,
                &tonemapEye_,
                &gradedTonemapEye_,
                &skyLookSided_,
                &motionView_,
                &motionViewWindow_};
    }

    [[nodiscard]] bool lookTexture(rhi::IDevice& device, rhi::TextureHandle& slot, u32 width, u32 height,
                                   const char* name);
    void releaseLookTextures(rhi::IDevice& device);
    // **What is read from the screen** (ADR 0209). Before the lit pass: each
    // pixel's mirror ray, marched and read off the last frame. After the
    // blended passes: this frame kept for the next one's rays. Both do
    // nothing, and keep nothing, while the settings are off.
    void traceScreen(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world);
    void keepSceneBefore(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world);
    void releaseScreenLight(rhi::IDevice& device);
    // Blurs `image` in place by `size` pixels of a 1080-line picture.
    void blurImage(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle image, f32 size);
    // Focuses `image` by distance into the other full-resolution image, and
    // returns that one -- or `image` itself when the passes cannot be made.
    [[nodiscard]] rhi::TextureHandle focusImage(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                                rhi::TextureHandle image);
    // Adds the sun's shafts onto `image`.
    void sunRaysOnto(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world, const SkyParams& sky,
                     rhi::TextureHandle image);
};

} // namespace

std::optional<core::EngineError> DefaultRenderer::create(rhi::IDevice& device, const ShaderLibrary& shaders,
                                                         rhi::TextureFormat colorFormat)
{
    colorFormat_ = colorFormat;
    shaderLibrary_ = &shaders;
    // What frame generation asks of a device (ADR 0165): compute, and the
    // atomics on an image its passes keep their fields with -- which Metal's
    // shading language lacks.
    fgCompute_ = device.caps().compute;
    fgAtomics_ = device.caps().shaderFormat != rhi::ShaderFormat::Msl;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaders.create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };

    const rhi::ShaderHandle shadowVertex = load("shadow_depth", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle shadowFragment = load("shadow_depth", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle pbrVertex = load("pbr", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle pbrFragment = load("pbr", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle pbrSkinnedVertex = load("pbr_skinned", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle pbrSkinnedFragment = load("pbr_skinned", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle shadowSkinnedVertex = load("shadow_skinned", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle shadowSkinnedFragment = load("shadow_skinned", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle skyVertex = load("sky", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle skyFragment = load("sky", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle tonemapVertex = load("tonemap", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle tonemapFragment = load("tonemap", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle shadowInstancedVertex = load("shadow_instanced", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle shadowInstancedFragment = load("shadow_instanced", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle pbrInstancedVertex = load("pbr_instanced", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle pbrInstancedFragment = load("pbr_instanced", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle pbrBakedInstancedVertex = load("pbr_baked_instanced", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle pbrBakedInstancedFragment = load("pbr_baked_instanced", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle ssaoVertex = load("ssao", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle ssaoFragment = load("ssao", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle ssaoBlurVertex = load("ssao_blur", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle ssaoBlurFragment = load("ssao_blur", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle contactVertex = load("contact_shadow", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle contactFragment = load("contact_shadow", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle bloomDownVertex = load("bloom_down", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle bloomDownFragment = load("bloom_down", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle bloomUpVertex = load("bloom_up", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle bloomUpFragment = load("bloom_up", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle luminanceDownVertex = load("luminance_down", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle luminanceDownFragment = load("luminance_down", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle luminanceReduceVertex = load("luminance_reduce", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle luminanceReduceFragment = load("luminance_reduce", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle luminanceAdaptVertex = load("luminance_adapt", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle luminanceAdaptFragment = load("luminance_adapt", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle fxaaVertex = load("fxaa", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fxaaFragment = load("fxaa", rhi::ShaderStage::Fragment);
    // The editor's selection silhouette. The mask's own vertex stage makes the
    // static pipeline; the skinned and instanced ones pair this fragment with
    // the shadow pass's vertex stages, because "position through a matrix" is
    // the same shader whether it writes depth or a one.
    const rhi::ShaderHandle outlineVertex = load("outline_mask", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle outlineFragment = load("outline_mask", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle outlineCompositeVertex = load("outline_composite", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle outlineCompositeFragment = load("outline_composite", rhi::ShaderStage::Fragment);

    for (const rhi::ShaderHandle handle : {shadowInstancedVertex,
                                           shadowInstancedFragment,
                                           pbrInstancedVertex,
                                           pbrInstancedFragment,
                                           ssaoVertex,
                                           ssaoFragment,
                                           ssaoBlurVertex,
                                           ssaoBlurFragment,
                                           contactVertex,
                                           contactFragment,
                                           bloomDownVertex,
                                           bloomDownFragment,
                                           bloomUpVertex,
                                           bloomUpFragment,
                                           luminanceDownVertex,
                                           luminanceDownFragment,
                                           luminanceReduceVertex,
                                           luminanceReduceFragment,
                                           luminanceAdaptVertex,
                                           luminanceAdaptFragment,
                                           fxaaVertex,
                                           fxaaFragment,
                                           outlineVertex,
                                           outlineFragment,
                                           outlineCompositeVertex,
                                           outlineCompositeFragment}) {
        if (!handle.valid()) {
            destroy(device);
            return error.key.hash != 0 ? error : core::makeError(ENG_TR("render.err.shader_format_unknown"));
        }
    }

    // Every shader made so far goes back with the refusal: a
    // renderer that failed to create left them all on the device.
    if (!shadowSkinnedVertex.valid() || !shadowSkinnedFragment.valid() || !pbrSkinnedVertex.valid() ||
        !pbrSkinnedFragment.valid()) {
        destroy(device);
        return error.key.hash != 0 ? error : core::makeError(ENG_TR("render.err.shader_format_unknown"));
    }
    if (!shadowVertex.valid() || !pbrVertex.valid() || !pbrFragment.valid() || !skyVertex.valid() ||
        !skyFragment.valid() || !tonemapVertex.valid() || !tonemapFragment.valid()) {
        destroy(device);
        return error.key.hash != 0 ? error : core::makeError(ENG_TR("render.err.shader_format_unknown"));
    }

    // The one static-mesh vertex layout, matching `asset::Vertex` exactly. The
    // 48 there and the 48 here are the same number for the same reason, and the
    // static_assert in model.h is what says so.
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };

    // The skinned layouts: the same stream at slot 0 plus `asset::SkinVertex` at
    // slot 1. The joint indices are `Float4` and not an integer format because
    // `rhi::VertexFormat` has none and that enumeration is frozen (ADR 0037);
    // model.h records what it costs.
    const std::array<rhi::VertexBufferLayout, 2> skinnedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
    };
    const std::array<rhi::VertexAttribute, 6> skinnedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    // The shadow pass reads position and the skin stream and nothing else, so
    // its joint and weight attributes are at locations 1 and 2 rather than 4 and
    // 5 -- the numbers are the shader's declaration order, not the vertex's.
    const std::array<rhi::VertexAttribute, 3> shadowSkinnedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };

    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};
    const std::array<rhi::ColorTargetDesc, 1> swapTarget{rhi::ColorTargetDesc{.format = colorFormat}};

    shadowPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowVertex,
        .fragmentShader = shadowFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        // Front faces are culled in the shadow pass rather than back faces, and
        // the measurement behind that was made for the lit band: switching to back faces puts a
        // regular hatched acne across every lit floor in `examples/02-meshes`.
        // What it COSTS is the contact -- the stored depth is the far side of a
        // solid object -- and the answer to that is not the cull mode, it is the
        // two biases below it, which exist to fight an acne this already
        // prevents.
        .rasterizer = {.cullMode = rhi::CullMode::Front},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "shadow",
    });

    pbrPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrVertex,
        .fragmentShader = pbrFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr",
    });
    pbrPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrVertex,
        .fragmentShader = pbrFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_prepassed",
    });

    // The blended pass. Depth-tested against what the opaque pass wrote, and
    // depth-write OFF -- two transparent surfaces must both contribute, so
    // neither may occlude the other. Source-alpha over, which is
    // `BlendState`'s own default and is why nothing in `rhi/descs.h` had to
    // change for this (ADR 0037's freeze holds).
    const std::array<rhi::ColorTargetDesc, 1> hdrBlendTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true},
    }};
    pbrBlendPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrVertex,
        .fragmentShader = pbrFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrBlendTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_blend",
    });

    shadowSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowSkinnedVertex,
        .fragmentShader = shadowSkinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = shadowSkinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Front},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "shadow_skinned",
    });

    pbrSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrSkinnedVertex,
        .fragmentShader = pbrSkinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_skinned",
    });
    pbrSkinnedPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrSkinnedVertex,
        .fragmentShader = pbrSkinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_skinned_prepassed",
    });

    pbrSkinnedBlendPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrSkinnedVertex,
        .fragmentShader = pbrSkinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrBlendTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_skinned_blend",
    });

    // The DEPTH PREPASS, and it is `shadow_depth` compiled into a different
    // pipeline rather than a new shader: `GpuShadowUniforms` is already a
    // view-projection and a model matrix, which is exactly what a depth-only
    // pass of the camera needs. What differs is state -- back-face culling, so
    // the depth it writes is the depth the forward pass will test against.
    //
    // What it does NOT do, said out loud: an alpha-masked material writes depth
    // where its own fragments would have been discarded. The shadow pass has
    // always had the same gap, and closing it means a second fragment shader
    // that samples base colour in a pass whose whole point is not to.
    depthPrepassPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowVertex,
        .fragmentShader = shadowFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "depth_prepass",
    });
    depthPrepassSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowSkinnedVertex,
        .fragmentShader = shadowSkinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = shadowSkinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "depth_prepass_skinned",
    });

    // The sky writes no depth and tests none: it is drawn first and everything
    // else covers it. It declares the depth FORMAT anyway, because the forward
    // pass has a depth attachment and every pipeline used inside it must agree
    // about that format even when it neither reads nor writes one.
    skyPipeline_ = device.createGraphicsPipeline({
        .vertexShader = skyVertex,
        .fragmentShader = skyFragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sky",
    });
    // **For a view seen on one side of a line** (ADR 0205): the sky is drawn
    // at the near plane, which is the depth the other side was marked with,
    // and passes wherever the depth is anything else.
    skySidedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = skyVertex,
        .fragmentShader = skyFragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::NotEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sky_sided",
    });

    const std::array<rhi::ColorTargetDesc, 1> occlusionTarget{rhi::ColorTargetDesc{.format = kOcclusionFormat}};
    const std::array<rhi::ColorTargetDesc, 1> luminanceTarget{rhi::ColorTargetDesc{.format = kLuminanceFormat}};
    const std::array<rhi::ColorTargetDesc, 1> ldrTarget{rhi::ColorTargetDesc{.format = kLdrFormat}};
    // Additive, which is what lets the upsample ADD into the level below rather
    // than read a target it is also writing -- something every backend refuses
    // and which the frozen `BlendState` already makes unnecessary.
    const std::array<rhi::ColorTargetDesc, 1> bloomAddTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::One,
                  .srcAlpha = rhi::BlendFactor::One,
                  .dstAlpha = rhi::BlendFactor::One},
    }};

    const auto fullscreen = [&](rhi::ShaderHandle vertex, rhi::ShaderHandle fragment,
                                std::span<const rhi::ColorTargetDesc> targets, const char* name) {
        return device.createGraphicsPipeline({
            .vertexShader = vertex,
            .fragmentShader = fragment,
            .primitive = rhi::PrimitiveType::TriangleList,
            .rasterizer = {.cullMode = rhi::CullMode::None},
            .colorTargets = targets,
            .debugName = name,
        });
    };

    // The per-INSTANCE stream, at slot 1: the model matrix as four columns and
    // the instance's alpha. `perInstance` is the one field ADR 0043 added to the
    // frozen RHI, and this is its only caller.
    const std::array<rhi::VertexBufferLayout, 2> instancedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = sizeof(GpuInstance), .perInstance = true},
    };
    const std::array<rhi::VertexAttribute, 9> instancedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 6, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 7, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 8, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 64},
    };
    // The depth-only instanced pass reads position and the four model columns
    // and nothing else, so its locations are 0 through 4 -- the numbers are the
    // shader's declaration order, not the vertex's.
    const std::array<rhi::VertexAttribute, 5> shadowInstancedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
    };

    shadowInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowInstancedVertex,
        .fragmentShader = shadowInstancedFragment,
        .vertexBuffers = instancedBuffers,
        .vertexAttributes = shadowInstancedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Front},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "shadow_instanced",
    });
    depthPrepassInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowInstancedVertex,
        .fragmentShader = shadowInstancedFragment,
        .vertexBuffers = instancedBuffers,
        .vertexAttributes = shadowInstancedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "depth_prepass_instanced",
    });
    pbrInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrInstancedVertex,
        .fragmentShader = pbrInstancedFragment,
        .vertexBuffers = instancedBuffers,
        .vertexAttributes = instancedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_instanced",
    });
    pbrInstancedPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrInstancedVertex,
        .fragmentShader = pbrInstancedFragment,
        .vertexBuffers = instancedBuffers,
        .vertexAttributes = instancedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_instanced_prepassed",
    });
    pbrInstancedBlendPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrInstancedVertex,
        .fragmentShader = pbrInstancedFragment,
        .vertexBuffers = instancedBuffers,
        .vertexAttributes = instancedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrBlendTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_instanced_blend",
    });
    // **A baked run** (ADR 0253): what an instanced run reads, and after it
    // each instance's rows at slot 2 and each vertex's place in its lightmap
    // at slot 3 -- the mesh's own stream, which only these pipelines bind.
    const std::array<rhi::VertexBufferLayout, 4> bakedBuffers{
        instancedBuffers[0],
        instancedBuffers[1],
        rhi::VertexBufferLayout{.slot = 2, .strideBytes = 16, .perInstance = true},
        rhi::VertexBufferLayout{.slot = 3, .strideBytes = 8},
    };
    std::array<rhi::VertexAttribute, 11> bakedAttributes{};
    std::copy(instancedAttributes.begin(), instancedAttributes.end(), bakedAttributes.begin());
    bakedAttributes[9] =
        rhi::VertexAttribute{.location = 9, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 0};
    bakedAttributes[10] =
        rhi::VertexAttribute{.location = 10, .bufferSlot = 3, .format = rhi::VertexFormat::Float2, .offsetBytes = 0};
    pbrBakedInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrBakedInstancedVertex,
        .fragmentShader = pbrBakedInstancedFragment,
        .vertexBuffers = bakedBuffers,
        .vertexAttributes = bakedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_baked_instanced",
    });
    pbrBakedInstancedPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = pbrBakedInstancedVertex,
        .fragmentShader = pbrBakedInstancedFragment,
        .vertexBuffers = bakedBuffers,
        .vertexAttributes = bakedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_baked_instanced_prepassed",
    });

    ssaoPipeline_ = fullscreen(ssaoVertex, ssaoFragment, occlusionTarget, "ssao");
    ssaoBlurPipeline_ = fullscreen(ssaoBlurVertex, ssaoBlurFragment, occlusionTarget, "ssao_blur");
    contactPipeline_ = fullscreen(contactVertex, contactFragment, occlusionTarget, "contact_shadow");
    bloomDownPipeline_ = fullscreen(bloomDownVertex, bloomDownFragment, hdrTarget, "bloom_down");
    bloomUpPipeline_ = fullscreen(bloomUpVertex, bloomUpFragment, bloomAddTarget, "bloom_up");
    luminanceDownPipeline_ = fullscreen(luminanceDownVertex, luminanceDownFragment, luminanceTarget, "luminance_down");
    luminanceReducePipeline_ =
        fullscreen(luminanceReduceVertex, luminanceReduceFragment, luminanceTarget, "luminance_reduce");
    luminanceAdaptPipeline_ =
        fullscreen(luminanceAdaptVertex, luminanceAdaptFragment, luminanceTarget, "luminance_adapt");
    tonemapPipeline_ = fullscreen(tonemapVertex, tonemapFragment, ldrTarget, "tonemap");
    if (colorFormat != kLdrFormat)
        tonemapWindowPipeline_ = fullscreen(tonemapVertex, tonemapFragment, swapTarget, "tonemap_window");
    fxaaPipeline_ = fullscreen(fxaaVertex, fxaaFragment, swapTarget, "fxaa");
    // The views draw into `kLdrFormat` textures; when the window's format is
    // another, their resolve needs a pipeline of its own (ADR 0107).
    if (colorFormat != kLdrFormat)
        fxaaViewPipeline_ = fullscreen(fxaaVertex, fxaaFragment, ldrTarget, "fxaa_view");

    // --- The editor's selection silhouette -----------------------------------
    //
    // The mask draws the same three geometry variants as everything else, with
    // the shadow pass's vertex stages: what a mask needs from a vertex is a
    // position through a matrix, which is what a depth-only pass needs too.
    //
    // **Front faces only and no depth at all.** The mask asks whether a pixel is
    // covered by the selected object, not by which part of it, so back-face
    // culling halves the fill for free -- and testing depth would be asking a
    // buffer this pass does not have. The consequence is deliberate and it is
    // the one an editor wants: the outline shows through what is in front of it,
    // so selecting something you cannot see still tells you where it is.
    const std::array<rhi::ColorTargetDesc, 1> maskTarget{rhi::ColorTargetDesc{.format = kOcclusionFormat}};
    outlinePipeline_ = device.createGraphicsPipeline({
        .vertexShader = outlineVertex,
        .fragmentShader = outlineFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = maskTarget,
        .debugName = "outline_mask",
    });
    outlineSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowSkinnedVertex,
        .fragmentShader = outlineFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = shadowSkinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = maskTarget,
        .debugName = "outline_mask_skinned",
    });
    // Straight alpha over whatever the frame already holds. The shader hands
    // back a premultiplied colour with its own coverage in alpha, so the source
    // factor is One.
    const std::array<rhi::ColorTargetDesc, 1> outlineTarget{rhi::ColorTargetDesc{
        .format = colorFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .srcAlpha = rhi::BlendFactor::One,
                  .dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha},
    }};
    outlineCompositePipeline_ =
        fullscreen(outlineCompositeVertex, outlineCompositeFragment, outlineTarget, "outline_composite");

    if (!outlinePipeline_.valid() || !outlineSkinnedPipeline_.valid() || !outlineCompositePipeline_.valid()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.pipeline_create_failed"));
    }

    if (!shadowPipeline_.valid() || !pbrPipeline_.valid() || !pbrBlendPipeline_.valid() || !skyPipeline_.valid() ||
        !tonemapPipeline_.valid() || !shadowSkinnedPipeline_.valid() || !pbrSkinnedPipeline_.valid() ||
        !pbrSkinnedBlendPipeline_.valid() || !depthPrepassPipeline_.valid() || !depthPrepassSkinnedPipeline_.valid() ||
        !ssaoPipeline_.valid() || !ssaoBlurPipeline_.valid() || !contactPipeline_.valid() ||
        !bloomDownPipeline_.valid() || !bloomUpPipeline_.valid() || !luminanceDownPipeline_.valid() ||
        !luminanceReducePipeline_.valid() || !luminanceAdaptPipeline_.valid() || !fxaaPipeline_.valid() ||
        !shadowInstancedPipeline_.valid() || !depthPrepassInstancedPipeline_.valid() ||
        !pbrInstancedPipeline_.valid()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.pipeline_create_failed"));
    }

    linearSampler_ = device.createSampler({.debugName = "material"});
    environmentSampler_ = device.createSampler({
        .addressU = rhi::AddressMode::ClampToEdge,
        .addressV = rhi::AddressMode::ClampToEdge,
        .addressW = rhi::AddressMode::ClampToEdge,
        .debugName = "environment",
    });
    pointSampler_ = device.createSampler({
        .minFilter = rhi::Filter::Nearest,
        .magFilter = rhi::Filter::Nearest,
        .mipmapMode = rhi::MipmapMode::Nearest,
        .addressU = rhi::AddressMode::ClampToEdge,
        .addressV = rhi::AddressMode::ClampToEdge,
        .addressW = rhi::AddressMode::ClampToEdge,
        .debugName = "cluster",
    });
    // Point, not linear: `Gather` fetches the four texels itself and the shader
    // does the bilinear comparison, which is what makes a hardware comparison
    // sampler unnecessary (ADR 0043).
    shadowSampler_ = device.createSampler({
        .minFilter = rhi::Filter::Nearest,
        .magFilter = rhi::Filter::Nearest,
        .mipmapMode = rhi::MipmapMode::Nearest,
        .addressU = rhi::AddressMode::ClampToEdge,
        .addressV = rhi::AddressMode::ClampToEdge,
        .addressW = rhi::AddressMode::ClampToEdge,
        .debugName = "shadow",
    });

    // The three defaults, in the values that make a missing map a no-op rather
    // than a change: white multiplies to itself, (0.5, 0.5, 1) is the tangent-
    // space normal pointing straight out, and black adds nothing.
    const auto onePixel = [&](const char* name, core::u8 r, core::u8 g, core::u8 b) -> rhi::TextureHandle {
        const rhi::TextureHandle handle = device.createTexture({
            .format = rhi::TextureFormat::Rgba8Unorm,
            .usage = rhi::TextureUsage::Sampled,
            .width = 1,
            .height = 1,
            .debugName = name,
        });
        (void)r;
        (void)g;
        (void)b;
        // The pixels are written on the first frame rather than here: `create`
        // runs outside a frame and has no command list, and an RHI call for
        // "upload without one" would be a call added on the eve of the interface
        // freeze that `render` already has a way to avoid.
        return handle;
    };
    whitePixel_ = onePixel("default-white", 0xFF, 0xFF, 0xFF);
    flatNormalPixel_ = onePixel("default-normal", 0x80, 0x80, 0xFF);
    blackPixel_ = onePixel("default-black", 0x00, 0x00, 0x00);
    nothingPixel_ = onePixel("default-nothing", 0x00, 0x00, 0x00);
    if (!whitePixel_.valid() || !flatNormalPixel_.valid() || !blackPixel_.valid() || !nothingPixel_.valid()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    }

    if (ensureShadowMap(device).has_value()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    }

    (void)ensureEnvironmentMap(device);
    brdfLut_ = device.createTexture({
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::Sampled,
        .width = kBrdfLutSize,
        .height = kBrdfLutSize,
        .debugName = "brdf-lut",
    });
    instanceBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = kMaxInstances * static_cast<u32>(sizeof(GpuInstance)),
        .debugName = "instances",
    });
    if (!instanceBuffer_.valid()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    }
    // Without it a baked part is drawn as any other is.
    bakedInstanceBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = kMaxInstances * 16u,
        .debugName = "baked-instances",
    });

    clusterGrid_ = device.createTexture({
        .format = rhi::TextureFormat::R32Float,
        .usage = rhi::TextureUsage::Sampled,
        .width = kClusterGridWidth,
        .height = kClusterGridHeight,
        .debugName = "cluster-grid",
    });
    lightIndices_ = device.createTexture({
        .format = rhi::TextureFormat::R32Float,
        .usage = rhi::TextureUsage::Sampled,
        .width = kLightIndexTextureWidth,
        .height = kLightIndexTextureHeight,
        .debugName = "cluster-light-indices",
    });
    lightData_ = device.createTexture({
        .format = rhi::TextureFormat::Rgba32Float,
        .usage = rhi::TextureUsage::Sampled,
        .width = 3,
        .height = kMaxClusteredLights,
        .debugName = "cluster-light-data",
    });
    if (!environmentMap_.valid() || !brdfLut_.valid() || !clusterGrid_.valid() || !lightIndices_.valid() ||
        !lightData_.valid()) {
        destroy(device);
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    }

    valid_ = true;
    return std::nullopt;
}

// The atlas is always two tiles by two, whatever the cascade count: fewer
// cascades buy submission rather than memory, and `shadowTileResolution` is the
// dial that buys memory (settings.h).
std::optional<core::EngineError> DefaultRenderer::ensureShadowMap(rhi::IDevice& device)
{
    ENG_PROFILE_SCOPE("prepare.shadow_map");
    if (shadowMap_.valid() && shadowTile_ == settings_.shadowTileResolution && shadowRows_ == shadowRowsWanted_)
        return std::nullopt;

    if (shadowMap_.valid())
        device.destroy(shadowMap_);

    const u32 atlas = settings_.shadowTileResolution * 2;
    shadowMap_ = device.createTexture({
        .format = kShadowFormat,
        .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
        .width = atlas,
        .height = settings_.shadowTileResolution * shadowRowsWanted_,
        .debugName = "shadow-atlas",
    });
    shadowRows_ = shadowRowsWanted_;
    if (!shadowMap_.valid())
        return core::makeError(ENG_TR("render.err.target_create_failed"));

    // The LOCAL atlas, for spots and points (`shadow.h`). Fixed size rather than
    // scaled by the shadow settings: its tiles are per light and not per camera
    // slice, so the quality dial that sizes a cascade says nothing about it.
    if (localShadowMap_.valid())
        device.destroy(localShadowMap_);
    localShadowMap_ = device.createTexture({
        .format = kShadowFormat,
        .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
        .width = kLocalShadowAtlasResolution,
        .height = kLocalShadowAtlasResolution,
        .debugName = "local-shadow-atlas",
    });
    if (!localShadowMap_.valid())
        return core::makeError(ENG_TR("render.err.target_create_failed"));

    shadowTile_ = settings_.shadowTileResolution;
    // A new atlas is a new texel size, so last frame's box would be remembered
    // against a lattice that no longer exists.
    shadowFitted_ = false;
    return std::nullopt;
}

void DefaultRenderer::warm(rhi::IDevice& device)
{
    if (!valid_)
        return;
    core::profile::Sections sections;
    // Each makes its family once and answers from then on; one that cannot be
    // made -- a shader the content does not carry -- is tried once here as it
    // would have been in play, and draws nothing either way.
    ENG_PROFILE_NEXT(sections, "warm.particles");
    (void)ensureParticles(device);
    ENG_PROFILE_NEXT(sections, "warm.gpu_particles");
    (void)ensureGpuParticles(device);
    ENG_PROFILE_NEXT(sections, "warm.ribbons");
    (void)ensureRibbons(device);
    ENG_PROFILE_NEXT(sections, "warm.decals");
    (void)ensureDecals(device);
    ENG_PROFILE_NEXT(sections, "warm.world_ui");
    (void)ensureWorldUi(device);
    ENG_PROFILE_NEXT(sections, "warm.highlight");
    (void)ensureHighlightMasks(device);
    ENG_PROFILE_NEXT(sections, "warm.skinning");
    (void)ensureSkinnedInstancing(device);
    ENG_PROFILE_NEXT(sections, "warm.morphs");
    (void)ensureMorph(device);
    // What makes a reflection probe's map (ADR 0202), where the machine keeps
    // any: its first pictures are taken behind this same curtain.
    ENG_PROFILE_NEXT(sections, "warm.probes");
    if (settings_.reflectionProbes > 0) {
        (void)ensureLookPipeline(device, probeGather_, "probe_gather", kHdrFormat);
        (void)ensureLookPipeline(device, probeFilter_, "probe_filter", kHdrFormat);
        (void)ensureLookPipeline(device, resample_, "look_resample", kHdrFormat);
    }
}

void DefaultRenderer::warmEyes(rhi::IDevice& device, u32 samples, u32 foveation, rhi::TextureFormat eyeFormat)
{
    if (!valid_ || samples <= 1)
        return;
    core::profile::Sections sections;
    ENG_PROFILE_NEXT(sections, "warm.eyes");
    eyeSamples_ = samples;
    eyeDensity_ = foveation > 0 && device.caps().densityTexelWidth > 0;
    eyeFormat_ = eyeFormat;
    // What an eye's pass with samples draws (`forwardWhole`): the lit
    // surfaces in every family, the sky, the world's interface, the ground,
    // the blocks and the foliage. Prepassed variants are not among them: a
    // frame with a depth prepass is not drawn with samples. Each that is
    // made by then; the rest are made at their first use, as they were.
    const MorphPipelines& morph = morphPipelines_[0];
    const MorphPipelines& morphSkinned = morphPipelines_[1];
    const std::array<rhi::PipelineHandle, 17> folding{
        pbrBakedInstancedPipeline_,
        pbrPipeline_,
        pbrBlendPipeline_,
        pbrSkinnedPipeline_,
        pbrSkinnedBlendPipeline_,
        pbrInstancedPipeline_,
        pbrInstancedBlendPipeline_,
        pbrSkinnedInstancedPipeline_,
        morph.forward,
        morph.blend,
        morphSkinned.forward,
        morphSkinned.blend,
        skyPipeline_,
        skySidedPipeline_,
        skyLook_.handle,
        worldUiPipeline_,
        worldUiOnTopPipeline_,
    };
    const std::array<rhi::PipelineHandle, 8> lit{
        foliagePipeline_,     terrainPipeline_,     terrainNoHexPipeline_, terrainTriplanarNoHexPipeline_,
        terrainFastPipeline_, terrainFlatPipeline_, voxelPipeline_,        voxelBlendPipeline_,
    };
    const rhi::Capabilities& caps = device.caps();
    const bool density = foveation > 0 && caps.densityTexelWidth > 0;
    // **Twice where the tonemap may be folded in**: into the 16-bit target,
    // and into the eye's image.
    for (const rhi::PipelineHandle pipeline : folding) {
        if (!pipeline.valid())
            continue;
        device.warmPipeline(pipeline, samples, density, rhi::TextureFormat::Undefined);
        if (eyeFormat != rhi::TextureFormat::Undefined)
            device.warmPipeline(pipeline, samples, density, eyeFormat);
    }
    for (const rhi::PipelineHandle pipeline : lit) {
        if (pipeline.valid())
            device.warmPipeline(pipeline, samples, density, rhi::TextureFormat::Undefined);
    }
}

std::optional<bool> DefaultRenderer::warmSurface(rhi::IDevice& device, std::string_view name)
{
    if (!valid_ || shaderLibrary_ == nullptr)
        return std::nullopt;
    bool failed = false;
    // Still being made off the frame is still on its way (ADR 0257): a
    // game's `PreloadAsync` waits for it, and a loading screen covers it.
    bool waiting = false;
    if (surfaceFor(device, name, failed, waiting) != 0)
        return true;
    return failed ? std::optional<bool>{false} : std::nullopt;
}

void DefaultRenderer::setSettings(const GraphicsSettings& settings)
{
    // Clamped here as well as at every source, because this is the last door: a
    // caller that builds a `GraphicsSettings` by hand should not be able to ask
    // for a render scale of zero and get a target of no pixels.
    settings_ = clampSettings(settings);
}

// How many layers of the environment array hold irradiance volumes' slices,
// four slices a layer (ADR 0203): sixty-four slices, which is two volumes at
// their deepest or a house of rooms.
constexpr u32 kVolumeLayers = 32;
// The level of a probe's second layer its diffuse light is kept at: eight
// texels a side.
constexpr u32 kProbeIrradianceLevel = kEnvironmentMipCount - 2;

bool DefaultRenderer::ensureEnvironmentMap(rhi::IDevice& device)
{
    const u32 probes = settings_.reflectionProbes;
    // **And the bake's grid of the sun** (ADR 0253 §4), which takes a
    // volume's slices: where no volume is worked out, layers for it alone.
    const u32 volumeLayers = settings_.indirectLight ? kVolumeLayers : sunGridWanted_ ? kIrradianceVolumeSide / 4u : 0u;
    if (environmentMap_.valid() && probes == environmentProbeSlots_ && volumeLayers == environmentVolumeLayers_)
        return true;
    if (environmentMap_.valid())
        device.destroy(environmentMap_);
    // The environment's mip chain is the roughness chain, so `mipLevels` is the
    // number of roughness steps and not a filtering nicety. The sky's layer is
    // written by `uploadTexture`, which is the one frozen call that takes a
    // level; a probe's and a volume's by a copy from a texture of their own,
    // which is why the array is a target as well.
    //
    // Never fewer than two layers: one is a plain texture, and the shaders
    // read an array.
    environmentMap_ = device.createTexture({
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
        .width = kEnvironmentBaseSize,
        .height = kEnvironmentBaseSize,
        .layers = std::max<u32>(2u, 1u + probes * 2u + volumeLayers),
        .mipLevels = kEnvironmentMipCount,
        .debugName = "environment",
    });
    environmentProbeSlots_ = probes;
    environmentVolumeLayers_ = volumeLayers;
    // A new texture holds nothing: no view's sky, no probe's map, no volume.
    // Each view's cache of its sky is still what it baked, and is uploaded
    // again the next frame that view is drawn.
    for (bool& held : skyLevelHeld_)
        held = false;
    probeSlots_.assign(probes, ProbeSlot{});
    volumeSlots_.clear();
    return environmentMap_.valid();
}

bool DefaultRenderer::holdsReflectionProbe(core::u64 key) const noexcept
{
    return std::any_of(probeSlots_.begin(), probeSlots_.end(),
                       [key](const ProbeSlot& slot) { return slot.held && slot.key == key; });
}

void DefaultRenderer::dropReflectionProbe(core::u64 key)
{
    for (ProbeSlot& slot : probeSlots_) {
        if (slot.held && slot.key == key)
            slot = ProbeSlot{};
    }
}

bool DefaultRenderer::storeReflectionProbe(rhi::IDevice& device, rhi::ICmdList& cmd, core::u64 key,
                                           std::span<const rhi::TextureHandle, 6> faces,
                                           std::span<const core::Mat4, 6> viewProjections)
{
    if (!valid_ || probeSlots_.empty() || !environmentMap_.valid())
        return false;
    // Its own map again, then one nobody has, then the one longest out of any
    // frame's table -- and never one this frame's surfaces are reading.
    ProbeSlot* chosen = nullptr;
    for (ProbeSlot& slot : probeSlots_) {
        if (slot.held && slot.key == key)
            chosen = &slot;
    }
    for (ProbeSlot& slot : probeSlots_) {
        if (chosen == nullptr && !slot.held)
            chosen = &slot;
    }
    if (chosen == nullptr) {
        for (ProbeSlot& slot : probeSlots_) {
            if (slot.lastUsed < indirectFrame_ && (chosen == nullptr || slot.lastUsed < chosen->lastUsed))
                chosen = &slot;
        }
    }
    if (chosen == nullptr)
        return false;

    if (!ensureLookPipeline(device, probeGather_, "probe_gather", kHdrFormat) ||
        !ensureLookPipeline(device, probeFilter_, "probe_filter", kHdrFormat))
        return false;
    const auto target = [&](rhi::TextureHandle& slot, u32 size, u32 levels, const char* name) {
        if (!slot.valid()) {
            slot = device.createTexture({
                .format = kHdrFormat,
                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
                .width = size,
                .height = size,
                .mipLevels = levels,
                .debugName = name,
            });
        }
        return slot.valid();
    };
    if (!target(probeScratch_, kEnvironmentBaseSize, kEnvironmentMipCount, "probe-gathered") ||
        !target(probeIrradiance_, environmentLevelSize(kProbeIrradianceLevel), 1, "probe-irradiance"))
        return false;
    for (u32 level = 1; level < kEnvironmentMipCount; ++level) {
        if (!target(probeLevels_[level], environmentLevelSize(level), 1, "probe-level"))
            return false;
    }

    cmd.pushDebugGroup("reflection-probe");
    // The six pictures into one map, and a plain chain of levels under it for
    // the filter to read wide samples from.
    struct Gather
    {
        Mat4 faces[6];
    } gather;
    std::array<rhi::TextureBinding, 6> pictures;
    for (usize face = 0; face < 6; ++face) {
        gather.faces[face] = viewProjections[face];
        pictures[face] = rhi::TextureBinding{faces[face], environmentSampler_};
    }
    fullscreenPass(cmd, probeGather_.handle, probeScratch_, kEnvironmentBaseSize, kEnvironmentBaseSize, "probe-gather",
                   pictures, asBytes(&gather, sizeof(gather)));
    cmd.generateMipmaps(probeScratch_);

    // Each roughness level through its own lobe, as the sky's are; then the
    // diffuse light through a cosine.
    const std::array<rhi::TextureBinding, 1> gathered{rhi::TextureBinding{probeScratch_, environmentSampler_}};
    for (u32 level = 1; level < kEnvironmentMipCount; ++level) {
        const f32 params[4] = {static_cast<f32>(level) / static_cast<f32>(kEnvironmentMipCount - 1), 32.0f,
                               static_cast<f32>(kEnvironmentBaseSize), 0.0f};
        const u32 size = environmentLevelSize(level);
        fullscreenPass(cmd, probeFilter_.handle, probeLevels_[level], size, size, "probe-filter", gathered,
                       asBytes(params, sizeof(params)));
    }
    {
        const f32 params[4] = {1.0f, 64.0f, static_cast<f32>(kEnvironmentBaseSize), 1.0f};
        const u32 size = environmentLevelSize(kProbeIrradianceLevel);
        fullscreenPass(cmd, probeFilter_.handle, probeIrradiance_, size, size, "probe-irradiance", gathered,
                       asBytes(params, sizeof(params)));
    }

    // And each into its place in the array.
    const u32 layer = 1u + static_cast<u32>(chosen - probeSlots_.data()) * 2u;
    cmd.blitTextureLevel(probeScratch_, 0, environmentMap_, layer, 0);
    for (u32 level = 1; level < kEnvironmentMipCount; ++level)
        cmd.blitTextureLevel(probeLevels_[level], 0, environmentMap_, layer, level);
    cmd.blitTextureLevel(probeIrradiance_, 0, environmentMap_, layer + 1u, kProbeIrradianceLevel);
    cmd.popDebugGroup();

    chosen->key = key;
    chosen->held = true;
    chosen->lastUsed = indirectFrame_;
    return true;
}

void DefaultRenderer::prepareIndirect(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world)
{
    ++indirectFrame_;
    for (f32& value : indirectParams_)
        value = 0.0f;
    indirectParams_[2] = kIrradianceVolumeStep;
    indirectParams_[3] = static_cast<f32>(kProbeIrradianceLevel);
    const auto rowsInto = [](f32 rows[][4], const Mat4& worldToBox) {
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 4; ++column)
                rows[row][column] = worldToBox.m[column][row];
        }
    };

    // **Probes**: those the frame was given that hold a map, the nearest
    // eight, the highest priority first -- among equals the smaller box, which
    // is the one placed for something in particular.
    struct Chosen
    {
        const RenderReflectionProbe* probe = nullptr;
        u32 slot = 0;
    };
    std::array<Chosen, kFrameReflectionProbes> probes;
    u32 probeCount = 0;
    for (const RenderReflectionProbe& probe : world.reflectionProbes) {
        if (probeCount == kFrameReflectionProbes)
            break;
        for (u32 slot = 0; slot < probeSlots_.size(); ++slot) {
            if (probeSlots_[slot].held && probeSlots_[slot].key == probe.key) {
                probes[probeCount++] = Chosen{&probe, slot};
                break;
            }
        }
    }
    const auto volumeOf = [](const RenderReflectionProbe& probe) {
        return probe.halfSize.x * probe.halfSize.y * probe.halfSize.z;
    };
    std::stable_sort(probes.begin(), probes.begin() + probeCount, [&](const Chosen& a, const Chosen& b) {
        if (a.probe->priority != b.probe->priority)
            return a.probe->priority > b.probe->priority;
        return volumeOf(*a.probe) < volumeOf(*b.probe);
    });
    for (u32 index = 0; index < probeCount; ++index) {
        const RenderReflectionProbe& probe = *probes[index].probe;
        f32(*rows)[4] = &probeRows_[index * 6];
        rowsInto(rows, probe.worldToBox);
        rows[3][0] = probe.halfSize.x;
        rows[3][1] = probe.halfSize.y;
        rows[3][2] = probe.halfSize.z;
        rows[3][3] = 1.0f / std::max(probe.blend, 1.0e-3f);
        rows[4][0] = probe.capture.x;
        rows[4][1] = probe.capture.y;
        rows[4][2] = probe.capture.z;
        rows[4][3] = static_cast<f32>(1u + probes[index].slot * 2u);
        rows[5][0] = probe.intensity;
        rows[5][1] = probe.boxProjection ? 1.0f : 0.0f;
        rows[5][2] = probe.presence;
        rows[5][3] = 0.0f;
        probeSlots_[probes[index].slot].lastUsed = indirectFrame_;
    }
    indirectParams_[0] = static_cast<f32>(probeCount);

    // The bake's grid of the sun is placed among the volumes after them: held
    // as used before they look for room, so none of them makes room with it.
    if (world.lightmap.sun && world.lightmap.maskHolds) {
        const core::u64 gridKey = 0xB4CE000000000000ull ^ world.lightmap.stamp;
        for (VolumeSlot& held : volumeSlots_) {
            if (held.key == gridKey)
                held.lastUsed = indirectFrame_;
        }
    }

    // **Volumes**: each in a run of layers of its own, four slices a layer,
    // taken again when its samples changed.
    u32 volumeCount = 0;
    const u32 firstVolumeLayer = 1u + environmentProbeSlots_ * 2u;
    const bool hasLayers = environmentVolumeLayers_ != 0 && environmentMap_.valid();
    for (const RenderIrradianceVolume& volume : world.irradianceVolumes) {
        if (volumeCount == kFrameIrradianceVolumes)
            break;
        const u32 nx = volume.counts[0];
        const u32 ny = volume.counts[1];
        const u32 nz = volume.counts[2];
        // **One colour for the whole box** (ADR 0207): its rows and nothing
        // else -- no layer, no upload, and so drawn where this machine keeps
        // no layers for volumes at all.
        if (nx == 0 && ny == 0 && nz == 0) {
            f32(*rows)[4] = &volumeRows_[volumeCount * kVolumeRows];
            rowsInto(rows, volume.worldToBox);
            rows[3][0] = volume.halfSize.x;
            rows[3][1] = volume.halfSize.y;
            rows[3][2] = volume.halfSize.z;
            rows[3][3] = 1.0f / std::max(volume.blend, 1.0e-3f);
            rows[4][0] = 0.0f;
            rows[4][1] = volume.flat.r;
            rows[4][2] = volume.flat.g;
            rows[4][3] = volume.flat.b;
            rows[5][0] = volume.intensity;
            rows[5][1] = 0.0f;
            rows[5][2] = volume.presence;
            rows[5][3] = 0.0f;
            rows[6][0] = volume.tint.r;
            rows[6][1] = volume.tint.g;
            rows[6][2] = volume.tint.b;
            rows[6][3] = volume.added ? 1.0f : 0.0f;
            ++volumeCount;
            continue;
        }
        if (!hasLayers)
            continue;
        if (nx == 0 || ny == 0 || nz == 0 || nx > kIrradianceVolumeSide || ny > kIrradianceVolumeSide ||
            nz > kIrradianceVolumeSide || volume.samples.size() < static_cast<usize>(nx) * ny * nz * 12)
            continue;
        const u32 layers = (nz + 3u) / 4u;
        auto slot = std::find_if(volumeSlots_.begin(), volumeSlots_.end(),
                                 [&](const VolumeSlot& held) { return held.key == volume.key; });
        if (slot != volumeSlots_.end() && slot->layers != layers) {
            volumeSlots_.erase(slot);
            slot = volumeSlots_.end();
        }
        bool upload = slot != volumeSlots_.end() && slot->revision != volume.revision;
        if (slot == volumeSlots_.end()) {
            // The first run of free layers long enough; and when there is
            // none, the volumes no frame has read for longest make room.
            const auto place = [&]() -> std::optional<u32> {
                std::sort(volumeSlots_.begin(), volumeSlots_.end(),
                          [](const VolumeSlot& a, const VolumeSlot& b) { return a.first < b.first; });
                u32 cursor = 0;
                for (const VolumeSlot& held : volumeSlots_) {
                    if (held.first - cursor >= layers)
                        return cursor;
                    cursor = held.first + held.layers;
                }
                if (environmentVolumeLayers_ - cursor >= layers)
                    return cursor;
                return std::nullopt;
            };
            std::optional<u32> first = place();
            while (!first.has_value()) {
                auto oldest = volumeSlots_.end();
                for (auto held = volumeSlots_.begin(); held != volumeSlots_.end(); ++held) {
                    if (held->lastUsed < indirectFrame_ &&
                        (oldest == volumeSlots_.end() || held->lastUsed < oldest->lastUsed))
                        oldest = held;
                }
                if (oldest == volumeSlots_.end())
                    break;
                volumeSlots_.erase(oldest);
                first = place();
            }
            if (!first.has_value())
                continue;
            volumeSlots_.push_back(VolumeSlot{.key = volume.key, .first = *first, .layers = layers});
            slot = volumeSlots_.end() - 1;
            upload = true;
        }
        slot->lastUsed = indirectFrame_;
        if (upload) {
            if (!volumeScratch_.valid()) {
                volumeScratch_ = device.createTexture({
                    .format = kHdrFormat,
                    .usage = rhi::TextureUsage::Sampled,
                    .width = kEnvironmentBaseSize,
                    .height = kEnvironmentBaseSize,
                    .debugName = "volume-slices",
                });
            }
            if (!volumeScratch_.valid())
                continue;
            slot->revision = volume.revision;
            const usize side = kEnvironmentBaseSize;
            for (u32 layer = 0; layer < layers; ++layer) {
                volumePixels_.assign(side * side * 4, 0);
                for (u32 row = 0; row < 4 && layer * 4 + row < nz; ++row) {
                    const u32 z = layer * 4 + row;
                    for (u32 y = 0; y < ny; ++y) {
                        for (u32 x = 0; x < nx; ++x) {
                            const f32* sample = &volume.samples[((static_cast<usize>(z) * ny + y) * nx + x) * 12];
                            for (u32 tile = 0; tile < 3; ++tile) {
                                core::u16* texel =
                                    &volumePixels_[((static_cast<usize>(row) * kIrradianceVolumeSide + y) * side +
                                                    tile * kIrradianceVolumeSide + x) *
                                                   4];
                                for (u32 channel = 0; channel < 4; ++channel)
                                    texel[channel] = floatToHalf(sample[tile * 4 + channel]);
                            }
                        }
                    }
                }
                cmd.uploadTexture(volumeScratch_,
                                  asBytes(volumePixels_.data(), volumePixels_.size() * sizeof(core::u16)), 0);
                cmd.blitTexture(volumeScratch_, environmentMap_, firstVolumeLayer + slot->first + layer);
            }
        }
        f32(*rows)[4] = &volumeRows_[volumeCount * kVolumeRows];
        rowsInto(rows, volume.worldToBox);
        rows[3][0] = volume.halfSize.x;
        rows[3][1] = volume.halfSize.y;
        rows[3][2] = volume.halfSize.z;
        rows[3][3] = 1.0f / std::max(volume.blend, 1.0e-3f);
        rows[4][0] = static_cast<f32>(nx);
        rows[4][1] = static_cast<f32>(ny);
        rows[4][2] = static_cast<f32>(nz);
        rows[4][3] = static_cast<f32>(firstVolumeLayer + slot->first);
        rows[5][0] = volume.intensity;
        rows[5][1] = 1.0f / static_cast<f32>(kEnvironmentBaseSize);
        rows[5][2] = volume.presence;
        rows[5][3] = volume.margin;
        rows[6][0] = volume.tint.r;
        rows[6][1] = volume.tint.g;
        rows[6][2] = volume.tint.b;
        rows[6][3] = volume.added ? 1.0f : 0.0f;
        ++volumeCount;
    }
    indirectParams_[1] = static_cast<f32>(volumeCount);

    // **The bake's grid of the sun** (ADR 0253 §4): one value a point, in the
    // first tile of a run of layers placed as a volume's, read while the mask
    // holds. Uploaded when the grid is another.
    std::memset(sunGridRows_, 0, sizeof(sunGridRows_));
    const RenderLightmap& baked = world.lightmap;
    const u32 gx = baked.sunCounts[0];
    const u32 gy = baked.sunCounts[1];
    const u32 gz = baked.sunCounts[2];
    if (baked.sun && baked.maskHolds && hasLayers && gx > 0 && gy > 0 && gz > 0 && gx <= kIrradianceVolumeSide &&
        gy <= kIrradianceVolumeSide && gz <= kIrradianceVolumeSide &&
        baked.sun->size() == static_cast<usize>(gx) * gy * gz) {
        const u32 layers = (gz + 3u) / 4u;
        // Its key is its own, past every volume's.
        const core::u64 key = 0xB4CE000000000000ull ^ baked.stamp;
        auto slot = std::find_if(volumeSlots_.begin(), volumeSlots_.end(),
                                 [&](const VolumeSlot& held) { return held.key == key; });
        bool upload = slot == volumeSlots_.end() || sunGridHeld_ != baked.sun.get() || slot->layers != layers;
        if (slot != volumeSlots_.end() && slot->layers != layers) {
            volumeSlots_.erase(slot);
            slot = volumeSlots_.end();
        }
        if (slot == volumeSlots_.end()) {
            std::sort(volumeSlots_.begin(), volumeSlots_.end(),
                      [](const VolumeSlot& a, const VolumeSlot& b) { return a.first < b.first; });
            std::optional<u32> first;
            u32 cursor = 0;
            for (const VolumeSlot& held : volumeSlots_) {
                if (held.first - cursor >= layers) {
                    first = cursor;
                    break;
                }
                cursor = held.first + held.layers;
            }
            if (!first.has_value() && environmentVolumeLayers_ - cursor >= layers)
                first = cursor;
            if (first.has_value()) {
                volumeSlots_.push_back(VolumeSlot{.key = key, .first = *first, .layers = layers});
                slot = volumeSlots_.end() - 1;
            }
        }
        if (slot != volumeSlots_.end()) {
            slot->lastUsed = indirectFrame_;
            if (upload && !volumeScratch_.valid()) {
                volumeScratch_ = device.createTexture({
                    .format = kHdrFormat,
                    .usage = rhi::TextureUsage::Sampled,
                    .width = kEnvironmentBaseSize,
                    .height = kEnvironmentBaseSize,
                    .debugName = "volume-slices",
                });
            }
            if (upload && volumeScratch_.valid()) {
                const usize side = kEnvironmentBaseSize;
                for (u32 layer = 0; layer < layers; ++layer) {
                    volumePixels_.assign(side * side * 4, 0);
                    for (u32 row = 0; row < 4 && layer * 4 + row < gz; ++row) {
                        const u32 z = layer * 4 + row;
                        for (u32 y = 0; y < gy; ++y) {
                            for (u32 x = 0; x < gx; ++x) {
                                const f32 seen = (*baked.sun)[(static_cast<usize>(z) * gy + y) * gx + x];
                                volumePixels_[((static_cast<usize>(row) * kIrradianceVolumeSide + y) * side + x) * 4] =
                                    floatToHalf(seen);
                            }
                        }
                    }
                    cmd.uploadTexture(volumeScratch_,
                                      asBytes(volumePixels_.data(), volumePixels_.size() * sizeof(core::u16)), 0);
                    cmd.blitTexture(volumeScratch_, environmentMap_, firstVolumeLayer + slot->first + layer);
                }
                sunGridHeld_ = baked.sun.get();
            }
            if (sunGridHeld_ == baked.sun.get()) {
                sunGridRows_[0][0] = baked.sunOffset.x;
                sunGridRows_[0][1] = baked.sunOffset.y;
                sunGridRows_[0][2] = baked.sunOffset.z;
                sunGridRows_[0][3] = baked.sunScale;
                sunGridRows_[1][0] = static_cast<f32>(gx);
                sunGridRows_[1][1] = static_cast<f32>(gy);
                sunGridRows_[1][2] = static_cast<f32>(gz);
                sunGridRows_[1][3] = static_cast<f32>(firstVolumeLayer + slot->first);
                sunGridRows_[2][0] = 1.0f / static_cast<f32>(kEnvironmentBaseSize);
                sunGridRows_[2][3] = 1.0f;
            }
        }
    }
}

void DefaultRenderer::useView(u32 view)
{
    if (view == activeView_)
        return;
    ViewState& active = *this;
    ViewState incoming;
    if (const auto found = parkedViews_.find(view); found != parkedViews_.end()) {
        incoming = std::move(found->second);
        parkedViews_.erase(found);
    }
    parkedViews_[activeView_] = std::move(active);
    active = std::move(incoming);
    activeView_ = view;
}

void DefaultRenderer::releaseActiveView(rhi::IDevice& device)
{
    releaseLookTextures(device);
    releaseAaTextures(device);
    const auto release = [&device](rhi::TextureHandle& texture) {
        if (texture.valid())
            device.destroy(texture);
        texture = {};
    };
    for (rhi::TextureHandle* texture : {&hdr_,         &depth_,         &decalMask_,   &decalFoliage_,   &ldr_,
                                        &occlusion_,   &occlusionBlur_, &contact_,     &outlineMask_,    &spriteMask_,
                                        &luminance64_, &luminance8_,    &exposure_[0], &exposure_[1],    &msaaColor_,
                                        &msaaDepth_,   &densityMap_,    &uiLayer_,     &uiLayerSamples_, &foldMeter_})
        release(*texture);
    for (rhi::TextureHandle& level : bloom_)
        release(level);
    static_cast<ViewState&>(*this) = ViewState{};
}

void DefaultRenderer::releaseView(rhi::IDevice& device, u32 view)
{
    // The main view is the renderer's own and goes with `destroy`.
    if (view == 0)
        return;
    const u32 was = activeView_;
    if (view != was && !parkedViews_.contains(view))
        return;
    useView(view);
    releaseActiveView(device);
    parkedViews_.erase(view);
    // Back to what the base held, or to the main view if that was this one.
    activeView_ = view;
    const u32 back = was == view ? 0u : was;
    ViewState incoming;
    if (const auto found = parkedViews_.find(back); found != parkedViews_.end()) {
        incoming = std::move(found->second);
        parkedViews_.erase(found);
    }
    static_cast<ViewState&>(*this) = std::move(incoming);
    activeView_ = back;
}

std::optional<core::EngineError> DefaultRenderer::ensureTargets(rhi::IDevice& device, u32 width, u32 height)
{
    ENG_PROFILE_SCOPE("prepare.targets");
    if (!ensureEnvironmentMap(device))
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    if (hdr_.valid() && width == width_ && height == height_)
        return std::nullopt;

    // `width`/`height` arrive already scaled -- see `render`, which is the one
    // place the settings' render scale is applied, so that nothing downstream
    // has to remember to.

    for (rhi::TextureHandle* texture :
         {&hdr_, &depth_, &decalMask_, &decalFoliage_, &ldr_, &occlusion_, &occlusionBlur_, &contact_, &outlineMask_,
          &spriteMask_, &luminance64_, &luminance8_, &exposure_[0], &exposure_[1]}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    for (rhi::TextureHandle& level : bloom_) {
        if (level.valid())
            device.destroy(level);
        level = {};
    }
    // New targets hold nothing: what was cleared is cleared no longer.
    occlusionOff_ = false;
    contactOff_ = false;
    bloomOff_ = false;

    const auto half = [](u32 value) { return value > 1 ? value / 2 : 1u; };

    hdr_ = device.createTexture({
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = "hdr",
    });
    // **Sampled as well as an attachment**, which is the whole of the roadmap's
    // "the scene depth must be samplable by a later pass". It is written by the
    // prepass, read by ambient occlusion, and then attached again by the forward
    // pass -- never both at once, which is a rule every backend enforces and
    // none has to be asked about.
    depth_ = device.createTexture({
        .format = kDepthFormat,
        .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = "depth",
    });
    ldr_ = device.createTexture({
        .format = kLdrFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = "ldr",
    });
    occlusion_ = device.createTexture({
        .format = kOcclusionFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = half(width),
        .height = half(height),
        .debugName = "occlusion",
    });
    // Full resolution, unlike the occlusion term beside it: what this carries is
    // an EDGE, and half a pixel of it is the difference between an outline and a
    // suggestion.
    outlineMask_ = device.createTexture({
        .format = kOcclusionFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = "outline-mask",
    });
    contact_ = device.createTexture({
        .format = kOcclusionFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = "contact-shadow",
    });
    occlusionBlur_ = device.createTexture({
        .format = kOcclusionFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = half(width),
        .height = half(height),
        .debugName = "occlusion-blur",
    });

    u32 levelWidth = half(width);
    u32 levelHeight = half(height);
    for (u32 level = 0; level < kBloomLevels; ++level) {
        bloom_[level] = device.createTexture({
            .format = kHdrFormat,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = levelWidth,
            .height = levelHeight,
            .debugName = "bloom",
        });
        levelWidth = half(levelWidth);
        levelHeight = half(levelHeight);
    }

    luminance64_ = device.createTexture({
        .format = kLuminanceFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = 64,
        .height = 64,
        .debugName = "luminance-64",
    });
    luminance8_ = device.createTexture({
        .format = kLuminanceFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = 8,
        .height = 8,
        .debugName = "luminance-8",
    });
    for (rhi::TextureHandle& target : exposure_) {
        target = device.createTexture({
            .format = kLuminanceFormat,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = 1,
            .height = 1,
            .debugName = "exposure",
        });
    }
    exposureInitialised_ = false;

    if (!hdr_.valid() || !depth_.valid() || !ldr_.valid() || !occlusion_.valid() || !occlusionBlur_.valid() ||
        !outlineMask_.valid() || !luminance64_.valid() || !luminance8_.valid() || !exposure_[0].valid() ||
        !exposure_[1].valid())
        return core::makeError(ENG_TR("render.err.target_create_failed"));
    for (const rhi::TextureHandle& level : bloom_) {
        if (!level.valid())
            return core::makeError(ENG_TR("render.err.target_create_failed"));
    }

    width_ = width;
    height_ = height;
    return std::nullopt;
}

void DefaultRenderer::destroy(rhi::IDevice& device)
{
    // Every view but the one in the base, which the lists below release.
    while (!parkedViews_.empty()) {
        const u32 view = parkedViews_.begin()->first;
        const u32 keep = activeView_;
        useView(view);
        releaseActiveView(device);
        parkedViews_.erase(view);
        activeView_ = keep;
        if (const auto found = parkedViews_.find(keep); found != parkedViews_.end()) {
            static_cast<ViewState&>(*this) = std::move(found->second);
            parkedViews_.erase(found);
        }
    }
    activeView_ = 0;
    if (fxaaViewPipeline_.valid())
        device.destroy(fxaaViewPipeline_);
    fxaaViewPipeline_ = {};
    for (core::usize index = 0; index < shaderCount_; ++index)
        device.destroy(shaders_[index]);
    shaderCount_ = 0;

    for (SurfaceSet& surface : surfaces_)
        releaseSurface(device, surface);
    surfaces_.clear();

    for (rhi::PipelineHandle* pipeline : {&shadowPipeline_,
                                          &pbrPipeline_,
                                          &pbrPrepassedPipeline_,
                                          &pbrSkinnedPrepassedPipeline_,
                                          &pbrInstancedPrepassedPipeline_,
                                          &pbrInstancedBlendPipeline_,
                                          &pbrBakedInstancedPipeline_,
                                          &pbrBakedInstancedPrepassedPipeline_,
                                          &pbrBlendPipeline_,
                                          &skyPipeline_,
                                          &tonemapPipeline_,
                                          &tonemapWindowPipeline_,
                                          &shadowSkinnedPipeline_,
                                          &pbrSkinnedPipeline_,
                                          &pbrSkinnedBlendPipeline_,
                                          &depthPrepassPipeline_,
                                          &depthPrepassSkinnedPipeline_,
                                          &viewSidePipeline_,
                                          &skySidedPipeline_,
                                          &ssaoPipeline_,
                                          &ssaoBlurPipeline_,
                                          &contactPipeline_,
                                          &bloomDownPipeline_,
                                          &bloomUpPipeline_,
                                          &luminanceDownPipeline_,
                                          &luminanceReducePipeline_,
                                          &luminanceAdaptPipeline_,
                                          &fxaaPipeline_,
                                          &shadowInstancedPipeline_,
                                          &depthPrepassInstancedPipeline_,
                                          &pbrInstancedPipeline_,
                                          &outlinePipeline_,
                                          &outlineSkinnedPipeline_,
                                          &outlineCompositePipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    for (rhi::TextureHandle* texture : {&decalFoliage_,
                                        &hdr_,
                                        &depth_,
                                        &decalMask_,
                                        &ldr_,
                                        &occlusion_,
                                        &occlusionBlur_,
                                        &contact_,
                                        &outlineMask_,
                                        &spriteMask_,
                                        &luminance64_,
                                        &luminance8_,
                                        &exposure_[0],
                                        &exposure_[1],
                                        &shadowMap_,
                                        &localShadowMap_,
                                        &whitePixel_,
                                        &flatNormalPixel_,
                                        &blackPixel_,
                                        &nothingPixel_,
                                        &whiteArray_,
                                        &flatNormalArray_,
                                        &environmentMap_,
                                        &brdfLut_,
                                        &clusterGrid_,
                                        &lightIndices_,
                                        &lightData_,
                                        &probeScratch_,
                                        &probeIrradiance_,
                                        &volumeScratch_}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    for (rhi::TextureHandle& level : probeLevels_) {
        if (level.valid())
            device.destroy(level);
        level = {};
    }
    probeSlots_.clear();
    volumeSlots_.clear();
    environmentProbeSlots_ = 0;
    environmentVolumeLayers_ = 0;
    for (rhi::TextureHandle& level : bloom_) {
        if (level.valid())
            device.destroy(level);
        level = {};
    }
    if (instanceBuffer_.valid())
        device.destroy(instanceBuffer_);
    instanceBuffer_ = {};
    if (bakedInstanceBuffer_.valid())
        device.destroy(bakedInstanceBuffer_);
    bakedInstanceBuffer_ = {};
    releaseLightmap(device);
    if (voxelBlockBuffer_.valid())
        device.destroy(voxelBlockBuffer_);
    voxelBlockBuffer_ = {};
    voxelBlocksSent_ = false;

    terrainFullTried_ = false;
    terrainNoHexTried_ = false;
    terrainNoHexNow_ = false;
    terrainTriplanarNoHexTried_ = false;
    terrainTriplanarNoHexNow_ = false;
    terrainFastTried_ = false;
    terrainFlatTried_ = false;
    terrainVertex_ = {};
    terrainFragment_ = {};
    for (rhi::PipelineHandle* pipeline : {&terrainPipeline_,
                                          &terrainNoHexPipeline_,
                                          &terrainTriplanarNoHexPipeline_,
                                          &terrainFastPipeline_,
                                          &terrainFlatPipeline_,
                                          &terrainShadowPipeline_,
                                          &terrainPrepassPipeline_,
                                          &terrainPackColorPipeline_,
                                          &terrainPackLinearPipeline_,
                                          &voxelPipeline_,
                                          &voxelPrepassedPipeline_,
                                          &particlePipeline_,
                                          &voxelTilePipeline_,
                                          &voxelBlendPipeline_,
                                          &voxelShadowPipeline_,
                                          &decalPipeline_,
                                          &decalAlphaPipeline_,
                                          &decalAddPipeline_,
                                          &worldUiPipeline_,
                                          &adornmentPipeline_,
                                          &adornmentOnTopPipeline_,
                                          &worldUiOnTopPipeline_,
                                          &spritePipeline_,
                                          &spriteExactPipeline_,
                                          &ribbonPipeline_,
                                          &highlightMaskPipeline_,
                                          &highlightMaskSkinnedPipeline_,
                                          &pbrSkinnedInstancedPipeline_,
                                          &pbrSkinnedInstancedPrepassedPipeline_,
                                          &shadowSkinnedInstancedPipeline_,
                                          &depthPrepassSkinnedInstancedPipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    spriteExactTried_ = false;
    terrainTried_ = false;
    terrainValid_ = false;
    for (TerrainArrays& entry : terrainArrays_) {
        releaseTerrainArrays(device, entry);
        if (entry.layerBuffer.valid())
            device.destroy(entry.layerBuffer);
        entry.layerBuffer = {};
    }
    terrainArrays_.clear();
    if (terrainPlainLayers_.valid())
        device.destroy(terrainPlainLayers_);
    terrainPlainLayers_ = {};
    terrainArrays_.clear();
    voxelTried_ = false;
    if (particleBuffer_.valid())
        device.destroy(particleBuffer_);
    particleBuffer_ = {};
    releaseGpuParticles(device);
    for (rhi::BufferHandle* buffer : {&skinnedInstanceBuffer_, &paletteBuffer_, &clothBuffer_}) {
        if (buffer->valid())
            device.destroy(*buffer);
        *buffer = {};
    }
    skinnedInstancingTried_ = false;
    for (MorphPipelines& set : morphPipelines_) {
        for (rhi::PipelineHandle* pipeline :
             {&set.forward, &set.forwardPrepassed, &set.blend, &set.shadow, &set.prepass}) {
            if (pipeline->valid())
                device.destroy(*pipeline);
            *pipeline = {};
        }
    }
    morphTried_ = false;
    morphReady_ = false;
    if (ribbonBuffer_.valid())
        device.destroy(ribbonBuffer_);
    ribbonBuffer_ = {};
    ribbonTried_ = false;
    highlightTried_ = false;
    if (worldUiBuffer_.valid())
        device.destroy(worldUiBuffer_);
    worldUiBuffer_ = {};
    for (rhi::BufferHandle* buffer : {&adornmentVertices_, &adornmentIndices_, &adornmentInstances_}) {
        if (buffer->valid())
            device.destroy(*buffer);
        *buffer = {};
    }
    adornmentsTried_ = false;
    adornmentGeometryUp_ = false;
    if (spriteBuffer_.valid())
        device.destroy(spriteBuffer_);
    spriteBuffer_ = {};
    spriteTried_ = false;
    if (voxelAtlas_.valid())
        device.destroy(voxelAtlas_);
    voxelAtlas_ = {};
    voxelTiles_.clear();
    particleTried_ = false;
    decalTried_ = false;
    worldUiTried_ = false;
    for (rhi::PipelineHandle* pipeline : {&foliagePipeline_, &foliageShadowPipeline_, &foliageDecalPipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    for (rhi::ComputePipelineHandle* pipeline : {&foliageCullPipeline_, &foliageFinalizePipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    releaseFsr2Images(device);
    for (rhi::ComputePipelineHandle& pipeline : fsr2Pipelines_) {
        if (pipeline.valid())
            device.destroy(pipeline);
        pipeline = {};
    }
    fsr2Tried_ = false;
    fsr2Failed_ = false;
    releaseFrameGenerationImages(device);
    for (rhi::ComputePipelineHandle& pipeline : fgPipelines_) {
        if (pipeline.valid())
            device.destroy(pipeline);
        pipeline = {};
    }
    fgTried_ = false;
    fgFailed_ = false;
    for (rhi::BufferHandle* buffer : {&foliageVisible_, &foliageCounters_, &foliageArguments_, &foliageCommands_}) {
        if (buffer->valid())
            device.destroy(*buffer);
        *buffer = {};
    }
    foliageVisibleCapacity_ = 0;
    foliageBucketCapacity_ = 0;
    foliageCommandCapacity_ = 0;
    foliageTried_ = false;
    foliageCulled_ = false;
    for (LookPipeline* look : lookPipelines()) {
        if (look->handle.valid())
            device.destroy(look->handle);
        *look = {};
    }
    for (rhi::PipelineHandle* pipeline :
         {&motionPipeline_, &motionSkinnedPipeline_, &warpMotionPipeline_, &warpMotionSkinnedPipeline_,
          &warpMotionUiPipeline_, &warpMotionUiOnTopPipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    motionTried_ = false;
    for (rhi::TextureHandle* table : {&smaaArea_, &smaaSearch_}) {
        if (table->valid())
            device.destroy(*table);
        *table = {};
    }
    smaaTablesUploaded_ = false;
    releaseLookTextures(device);
    lookWidth_ = 0;
    lookHeight_ = 0;

    for (rhi::SamplerHandle* sampler : {&linearSampler_, &shadowSampler_, &environmentSampler_, &pointSampler_}) {
        if (sampler->valid())
            device.destroy(*sampler);
        *sampler = {};
    }

    width_ = 0;
    height_ = 0;
    defaultsUploaded_ = false;
    brdfUploaded_ = false;
    exposureInitialised_ = false;
    exposureIndex_ = 0;
    environment_ = EnvironmentCache{};
    valid_ = false;
}

void DefaultRenderer::releaseSurface(rhi::IDevice& device, SurfaceSet& set)
{
    for (rhi::PipelineHandle* pipeline :
         {&set.forward, &set.blended, &set.instanced, &set.shadow, &set.shadowInstanced, &set.prepass,
          &set.prepassInstanced, &set.forwardPrepassed, &set.instancedPrepassed}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = rhi::PipelineHandle{};
    }
    for (rhi::ShaderHandle& shader : set.shaders) {
        if (shader.valid())
            device.destroy(shader);
        shader = rhi::ShaderHandle{};
    }
    set.ready = false;
}

void DefaultRenderer::pollSurface(rhi::IDevice& device, SurfaceSet& set)
{
    if (!set.making)
        return;
    bool every = true;
    bool refused = false;
    for (const rhi::PipelineHandle pipeline :
         {set.forward, set.forwardPrepassed, set.instanced, set.instancedPrepassed, set.blended, set.shadow,
          set.shadowInstanced, set.prepass, set.prepassInstanced}) {
        const std::optional<bool> made = device.pipelineMade(pipeline);
        if (!made.has_value())
            every = false;
        else if (!*made)
            refused = true;
    }
    if (!every)
        return;
    set.making = false;
    set.ready = !refused;
    set.failed = refused;
    if (refused) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{set.name}}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.surface_pipeline_failed"), args);
        return;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - set.askedAt);
    const std::array<core::I18nArg, 2> args{core::I18nArg{"urn", std::string_view{set.name}},
                                            core::I18nArg{"milliseconds", std::lround(elapsed.count() * 10.0) / 10.0}};
    core::log(core::LogLevel::Info, ENG_TR("render.info.surface_pipelines_off_frame"), args);
}

u32 DefaultRenderer::surfaceFor(rhi::IDevice& device, std::string_view name, bool& failed, bool& waiting)
{
    failed = false;
    waiting = false;
    // **A user's surface, by URN**: asked for every frame, never waited on.
    if (name.find("://") != std::string_view::npos) {
        const SurfaceProgram* program = nullptr;
        const SurfaceStatus status = surfaceSource_ != nullptr
                                         ? surfaceSource_->find(name, shaderLibrary_->format(), program)
                                         : SurfaceStatus::Pending;
        if (status == SurfaceStatus::Failed || (status == SurfaceStatus::Ready && program == nullptr)) {
            failed = status == SurfaceStatus::Failed;
            return 0;
        }
        if (status == SurfaceStatus::Pending)
            return 0;
        core::usize found = surfaces_.size();
        for (core::usize index = 0; index < surfaces_.size(); ++index) {
            if (surfaces_[index].name == name)
                found = index;
        }
        if (found < surfaces_.size() && surfaces_[found].revision == program->revision) {
            pollSurface(device, surfaces_[found]);
            failed = surfaces_[found].failed;
            waiting = surfaces_[found].making;
            return surfaces_[found].ready ? static_cast<u32>(found) + 1u : 0u;
        }
        if (found == surfaces_.size()) {
            surfaces_.emplace_back().name = std::string(name);
        }
        SurfaceSet& set = surfaces_[found];
        // No wait for the GPU: a destroy releases a pipeline once
        // no frame in flight holds it, as every other mid-run release relies on.
        releaseSurface(device, set);
        // Timed, because it is the one cost a surface adds on the frame that
        // first draws it: ten shaders and every pipeline, on the render thread.
        const auto started = std::chrono::steady_clock::now();
        set.revision = program->revision;
        set.reflection = program->reflection;
        for (core::usize index = 0; index < 5; ++index) {
            for (const bool fragment : {false, true}) {
                const asset::SurfaceResourceCounts counts = asset::surfaceResourceCounts(
                    set.reflection, static_cast<asset::SurfaceVariant>(index),
                    fragment ? asset::SurfaceStage::Fragment : asset::SurfaceStage::Vertex);
                const std::vector<std::byte>& code = program->code[index * 2 + (fragment ? 1 : 0)];
                set.shaders[index * 2 + (fragment ? 1 : 0)] = device.createShader({
                    .stage = fragment ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Vertex,
                    .format = shaderLibrary_->format(),
                    .code = code,
                    .entryPoint = fragment ? "FragmentMain" : "VertexMain",
                    .samplerCount = counts.samplers,
                    .uniformBufferCount = counts.uniformBuffers,
                    .debugName = set.name,
                });
            }
        }
        set.failed = !buildSurfacePipelines(device, set);
        if (set.failed) {
            // Said once per revision, where the magenta alone said nothing.
            const std::array<core::I18nArg, 1> args{core::I18nArg{"name", name}};
            core::log(core::LogLevel::Warn, ENG_TR("render.warn.surface_pipeline_failed"), args);
            failed = true;
            return 0;
        }
        // Made off the frame: asked again until it is (ADR 0257).
        if (set.making) {
            waiting = true;
            return 0;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
        const std::array<core::I18nArg, 2> args{
            core::I18nArg{"urn", name}, core::I18nArg{"milliseconds", std::lround(elapsed.count() * 10.0) / 10.0}};
        core::log(core::LogLevel::Info, ENG_TR("render.info.surface_pipelines"), args);
        return static_cast<u32>(found) + 1u;
    }

    for (core::usize index = 0; index < surfaces_.size(); ++index) {
        if (surfaces_[index].name == name) {
            pollSurface(device, surfaces_[index]);
            failed = surfaces_[index].failed;
            waiting = surfaces_[index].making;
            return surfaces_[index].ready ? static_cast<u32>(index) + 1u : 0u;
        }
    }
    SurfaceSet& set = surfaces_.emplace_back();
    set.name = std::string(name);

    const std::optional<std::string> source =
        shaderLibrary_ != nullptr ? shaderLibrary_->surfaceSource(name) : std::nullopt;
    if (!source.has_value()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", name}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.surface_unavailable"), args);
        return 0;
    }
    set.reflection = asset::reflectSurface(*source);
    if (!set.reflection.ok()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", name}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.surface_unavailable"), args);
        return 0;
    }

    // Every variant's two stages, with the counts the layout decides.
    constexpr std::array<std::pair<asset::SurfaceVariant, std::string_view>, 5> variants{
        std::pair{asset::SurfaceVariant::Forward, std::string_view{"forward"}},
        std::pair{asset::SurfaceVariant::ForwardInstanced, std::string_view{"forward_instanced"}},
        std::pair{asset::SurfaceVariant::ForwardBlended, std::string_view{"forward_blended"}},
        std::pair{asset::SurfaceVariant::Depth, std::string_view{"depth"}},
        std::pair{asset::SurfaceVariant::DepthInstanced, std::string_view{"depth_instanced"}},
    };
    for (core::usize index = 0; index < variants.size(); ++index) {
        const std::string shaderName = "surface_" + set.name + "_" + std::string(variants[index].second);
        for (const auto& [stage, rhiStage] : {std::pair{asset::SurfaceStage::Vertex, rhi::ShaderStage::Vertex},
                                              std::pair{asset::SurfaceStage::Fragment, rhi::ShaderStage::Fragment}}) {
            const asset::SurfaceResourceCounts counts =
                asset::surfaceResourceCounts(set.reflection, variants[index].first, stage);
            set.shaders[index * 2 + (stage == asset::SurfaceStage::Vertex ? 0 : 1)] =
                shaderLibrary_->createCounted(device, shaderName, rhiStage, counts.samplers, counts.uniformBuffers);
        }
    }
    if (!buildSurfacePipelines(device, set)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", name}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.surface_unavailable"), args);
        return 0;
    }
    if (set.making) {
        waiting = true;
        return 0;
    }
    return static_cast<u32>(surfaces_.size());
}

bool DefaultRenderer::buildSurfacePipelines(rhi::IDevice& device, SurfaceSet& set)
{
    if (!std::all_of(set.shaders.begin(), set.shaders.end(), [](rhi::ShaderHandle shader) { return shader.valid(); }))
        return false;

    // The built-in surface's states, with the full vertex layout in every pass:
    // a displaced vertex casts a displaced shadow, and it may displace by its
    // normal or its uv.
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48}};
    const std::array<rhi::VertexBufferLayout, 2> instancedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = sizeof(GpuInstance), .perInstance = true},
    };
    const std::array<rhi::VertexAttribute, 9> instancedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 6, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 7, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 8, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 64},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};
    const std::array<rhi::ColorTargetDesc, 1> hdrBlendTarget{
        rhi::ColorTargetDesc{.format = kHdrFormat, .blend = {.enabled = true}}};
    const rhi::DepthStencilState depthWriting{
        .depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual};
    const auto shader = [&](core::usize variant, bool fragment) {
        return set.shaders[variant * 2 + (fragment ? 1 : 0)];
    };
    // **Made off the frame where the machine may** (ADR 0257): the driver's
    // compile on a job, the lit pipelines with their twins at an eye's pass.
    const bool offFrame = settings_.offFramePipelines;
    std::vector<rhi::PipelineVariant> eyes;
    if (eyeSamples_ > 1 || eyeDensity_) {
        eyes.push_back(rhi::PipelineVariant{.samples = eyeSamples_, .densityShaded = eyeDensity_});
        if (eyeFormat_ != rhi::TextureFormat::Undefined)
            eyes.push_back(
                rhi::PipelineVariant{.samples = eyeSamples_, .densityShaded = eyeDensity_, .colorFormat = eyeFormat_});
    }
    const auto make = [&](const rhi::GraphicsPipelineDesc& desc, bool lit) {
        if (!offFrame)
            return device.createGraphicsPipeline(desc);
        return device.createGraphicsPipelineOffFrame(desc, lit ? std::span<const rhi::PipelineVariant>(eyes)
                                                               : std::span<const rhi::PipelineVariant>{});
    };

    set.forward = make(
        {
            .vertexShader = shader(0, false),
            .fragmentShader = shader(0, true),
            .vertexBuffers = buffers,
            .vertexAttributes = attributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = depthWriting,
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = "surface_forward",
        },
        true);
    const rhi::DepthStencilState depthTesting{
        .depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual};
    set.forwardPrepassed = make(
        {
            .vertexShader = shader(0, false),
            .fragmentShader = shader(0, true),
            .vertexBuffers = buffers,
            .vertexAttributes = attributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = depthTesting,
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = "surface_forward_prepassed",
        },
        true);
    set.instanced = make(
        {
            .vertexShader = shader(1, false),
            .fragmentShader = shader(1, true),
            .vertexBuffers = instancedBuffers,
            .vertexAttributes = instancedAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = depthWriting,
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = "surface_forward_instanced",
        },
        true);
    set.instancedPrepassed = make(
        {
            .vertexShader = shader(1, false),
            .fragmentShader = shader(1, true),
            .vertexBuffers = instancedBuffers,
            .vertexAttributes = instancedAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = depthTesting,
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = "surface_forward_instanced_prepassed",
        },
        true);
    set.blended = make(
        {
            .vertexShader = shader(2, false),
            .fragmentShader = shader(2, true),
            .vertexBuffers = buffers,
            .vertexAttributes = attributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = hdrBlendTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = "surface_forward_blended",
        },
        true);
    const auto depthPipeline = [&](core::usize variant, bool instanced, rhi::CullMode cull, rhi::TextureFormat format,
                                   const char* debugName) {
        return make(
            {
                .vertexShader = shader(variant, false),
                .fragmentShader = shader(variant, true),
                .vertexBuffers = instanced ? std::span<const rhi::VertexBufferLayout>(instancedBuffers)
                                           : std::span<const rhi::VertexBufferLayout>(buffers),
                .vertexAttributes = instanced ? std::span<const rhi::VertexAttribute>(instancedAttributes)
                                              : std::span<const rhi::VertexAttribute>(attributes),
                .rasterizer = {.cullMode = cull, .depthClip = true},
                .depthStencil = depthWriting,
                .colorTargets = {},
                .depthStencilFormat = format,
                .debugName = debugName,
            },
            false);
    };
    set.shadow = depthPipeline(3, false, rhi::CullMode::Front, kShadowFormat, "surface_shadow");
    set.shadowInstanced = depthPipeline(4, true, rhi::CullMode::Front, kShadowFormat, "surface_shadow_instanced");
    set.prepass = depthPipeline(3, false, rhi::CullMode::Back, kDepthFormat, "surface_prepass");
    set.prepassInstanced = depthPipeline(4, true, rhi::CullMode::Back, kDepthFormat, "surface_prepass_instanced");

    const bool made = set.forward.valid() && set.instanced.valid() && set.blended.valid() && set.shadow.valid() &&
                      set.shadowInstanced.valid() && set.prepass.valid() && set.prepassInstanced.valid();
    // Off the frame, every handle is good and nothing is made yet: asked
    // again until it is.
    set.making = offFrame && made;
    set.ready = made && !set.making;
    set.askedAt = std::chrono::steady_clock::now();
    set.waitSaid = false;
    return made;
}

void DefaultRenderer::prepareSurfaces(rhi::IDevice& device, const RenderWorld& world)
{
    ENG_PROFILE_SCOPE("prepare.surfaces");
    materialSurface_.assign(world.materials.size(), 0u);
    materialError_.assign(world.materials.size(), false);
    materialWaiting_.assign(world.materials.size(), false);
    materialBlock_.resize(world.materials.size());
    materialSurfaceTextures_.resize(world.materials.size());

    // The clock and the camera, the same for every surface this frame. An f32
    // of seconds keeps a millisecond for about four and a half hours of play.
    const f32 clock[4] = {static_cast<f32>(world.environment.surfaceTime), static_cast<f32>(world.camera.origin.x),
                          static_cast<f32>(world.camera.origin.y), static_cast<f32>(world.camera.origin.z)};

    for (core::usize index = 0; index < world.materials.size(); ++index) {
        const RenderMaterial& material = world.materials[index];
        const std::string_view name =
            material.surface.empty() ? std::string_view{settings_.forcedSurface} : std::string_view{material.surface};
        if (name.empty())
            continue;
        bool failed = false;
        bool waiting = false;
        const u32 surface = surfaceFor(device, name, failed, waiting);
        materialError_[index] = failed;
        if (surface == 0 && waiting) {
            // **Not drawn while its pipelines are made** (ADR 0257) -- in
            // no pass, so nothing casts a shadow without a body -- for as long
            // as the project waits; drawn with the built-in surface after
            // that, and said once.
            for (SurfaceSet& set : surfaces_) {
                if (set.name != name)
                    continue;
                const auto waited = std::chrono::steady_clock::now() - set.askedAt;
                if (waited < std::chrono::milliseconds(settings_.materialWaitMs)) {
                    materialWaiting_[index] = true;
                }
                else if (!set.waitSaid) {
                    set.waitSaid = true;
                    const std::array<core::I18nArg, 2> args{
                        core::I18nArg{"urn", name},
                        core::I18nArg{"milliseconds", static_cast<core::i64>(settings_.materialWaitMs)}};
                    core::log(core::LogLevel::Info, ENG_TR("render.info.surface_drawn_plain"), args);
                }
                break;
            }
        }
        if (surface == 0)
            continue;
        materialSurface_[index] = surface;
        const asset::SurfaceReflection& reflection = surfaces_[surface - 1].reflection;

        // **The built-in fields first, under their own names**, then what the
        // material says for its shader: a surface that declares `Color` gets
        // the part's colour, and one that declares its own `Color` default
        // gets it only when nothing set one.
        const auto builtIn = [&](std::string_view field) -> std::optional<std::array<f32, 4>> {
            const GpuMaterialUniforms& u = material.uniforms;
            if (field == "Color")
                return std::array<f32, 4>{u.baseColor[0], u.baseColor[1], u.baseColor[2], u.baseColor[3]};
            if (field == "Metalness")
                return std::array<f32, 4>{u.metallicRoughnessNormalCutoff[0], 0.0f, 0.0f, 0.0f};
            if (field == "Roughness")
                return std::array<f32, 4>{u.metallicRoughnessNormalCutoff[1], 0.0f, 0.0f, 0.0f};
            if (field == "NormalScale")
                return std::array<f32, 4>{u.metallicRoughnessNormalCutoff[2], 0.0f, 0.0f, 0.0f};
            if (field == "AlphaCutoff")
                return std::array<f32, 4>{u.metallicRoughnessNormalCutoff[3], 0.0f, 0.0f, 0.0f};
            if (field == "Emissive")
                return std::array<f32, 4>{u.emissive[0], u.emissive[1], u.emissive[2], 0.0f};
            return std::nullopt;
        };
        const auto valueOf = [&](std::string_view field) -> const SurfaceValue* {
            for (const SurfaceValue& value : material.surfaceValues) {
                if (value.name == field)
                    return &value;
            }
            return nullptr;
        };

        std::vector<core::u8>& block = materialBlock_[index];
        block.assign(reflection.blockBytes, 0);
        std::memcpy(block.data(), clock, sizeof(clock));
        // The wind (ADR 0115), after the clock and the texture mask.
        const f32 wind[8] = {world.environment.wind.x,
                             world.environment.wind.y,
                             world.environment.wind.z,
                             world.environment.windGusts,
                             world.environment.windTurbulence,
                             0.0f,
                             0.0f,
                             0.0f};
        std::memcpy(block.data() + 32, wind, sizeof(wind));
        for (const asset::SurfaceParam& param : reflection.params) {
            if (const SurfaceValue* value = valueOf(param.name); value != nullptr && !value->isTexture) {
                asset::writeSurfaceParam(param, value->value, block);
            }
            else if (const std::optional<std::array<f32, 4>> fixed = builtIn(param.name); fixed.has_value()) {
                asset::writeSurfaceParam(param, *fixed, block);
            }
            else {
                asset::writeSurfaceParam(param, {}, block);
            }
        }

        core::u32 textureMask = 0;
        const auto builtInMap = [&](std::string_view field) -> rhi::TextureHandle {
            if (field == "ColorMap")
                return material.baseColor;
            if (field == "NormalMap")
                return material.normal;
            if (field == "MetallicRoughnessMap")
                return material.metallicRoughness;
            if (field == "EmissiveMap")
                return material.emissive;
            return rhi::TextureHandle{};
        };
        for (core::usize slot = 0; slot < reflection.textures.size(); ++slot) {
            const asset::SurfaceTexture& texture = reflection.textures[slot];
            rhi::TextureHandle handle{};
            if (const SurfaceValue* value = valueOf(texture.name); value != nullptr && value->isTexture)
                handle = value->texture;
            if (!handle.valid())
                handle = builtInMap(texture.name);
            if (handle.valid())
                textureMask |= 1u << slot;
            if (!handle.valid()) {
                handle = texture.fallback == asset::SurfaceTextureDefault::Black    ? blackPixel_
                         : texture.fallback == asset::SurfaceTextureDefault::Normal ? flatNormalPixel_
                                                                                    : whitePixel_;
            }
            materialSurfaceTextures_[index][slot] = rhi::TextureBinding{handle, linearSampler_};
        }
        std::memcpy(block.data() + 16, &textureMask, sizeof(textureMask));
    }
}

void DefaultRenderer::bindSurface(rhi::ICmdList& cmd, u32 material, bool fragment, bool blended) const
{
    const std::vector<core::u8>& block = materialBlock_[material];
    const asset::SurfaceReflection& reflection = surfaces_[materialSurface_[material] - 1].reflection;
    const std::span<const rhi::TextureBinding> textures(materialSurfaceTextures_[material].data(),
                                                        reflection.textures.size());
    cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, std::as_bytes(std::span(block)));
    if (!textures.empty())
        cmd.bindTextures(rhi::ShaderStage::Vertex, 0, textures);
    if (fragment) {
        cmd.bindUniforms(rhi::ShaderStage::Fragment, 2, std::as_bytes(std::span(block)));
        const std::size_t low = std::min<std::size_t>(textures.size(), 4);
        if (low > 0)
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures.first(low));
        if (textures.size() > low)
            cmd.bindTextures(rhi::ShaderStage::Fragment, asset::EngineFragmentSamplers, textures.subspan(low));
        if (blended) {
            // A blended surface's stage declares every slot up to the scene's,
            // and SDL_GPU wants each bound: the fifth texture's, when there is
            // no fifth texture, is white.
            if (textures.size() <= 4) {
                const std::array<rhi::TextureBinding, 1> unused{rhi::TextureBinding{whitePixel_, linearSampler_}};
                cmd.bindTextures(rhi::ShaderStage::Fragment, asset::EngineFragmentSamplers, unused);
            }
            const std::array<rhi::TextureBinding, 2> scene{
                rhi::TextureBinding{sceneDepthCopy_.valid() ? sceneDepthCopy_ : whitePixel_, pointSampler_},
                rhi::TextureBinding{sceneColorCopy_.valid() ? sceneColorCopy_ : blackPixel_, linearSampler_},
            };
            cmd.bindTextures(rhi::ShaderStage::Fragment, asset::SceneDepthSlot, scene);
        }
        else {
            // **An opaque surface reads the screen from the same two slots**
            // (ADR 0209), and its stage declares every slot up to them: the
            // fifth texture's is white where there is no fifth.
            if (textures.size() <= 4) {
                const std::array<rhi::TextureBinding, 1> unused{rhi::TextureBinding{whitePixel_, linearSampler_}};
                cmd.bindTextures(rhi::ShaderStage::Fragment, asset::EngineFragmentSamplers, unused);
            }
            const std::array<rhi::TextureBinding, 2> screen{screenReflectionBinding(false), screenBounceBinding()};
            cmd.bindTextures(rhi::ShaderStage::Fragment, asset::ScreenReflectionSlot, screen);
        }
    }
}

void DefaultRenderer::copySceneForSurfaces(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                           const GpuFrameUniforms& frame)
{
    // **Only a frame that has a blended surface shader in view pays for it** --
    // and every other frame's command stream is exactly what it was.
    bool depthWanted = false;
    bool colorWanted = false;
    for (const DrawItem& draw : world.draws) {
        if (!draw.transparent || !draw.inCameraFrustum || draw.material >= materialSurface_.size() ||
            materialSurface_[draw.material] == 0)
            continue;
        depthWanted = true;
        colorWanted = colorWanted || world.materials[draw.material].readsSceneColor;
    }
    if (!depthWanted)
        return;
    if (!ensureLookPipeline(device, surfaceSceneDepth_, "surface_scene_depth", rhi::TextureFormat::R32Float) ||
        (colorWanted && !ensureLookPipeline(device, resample_, "look_resample", kHdrFormat)))
        return;
    if (!sceneDepthCopy_.valid()) {
        sceneDepthCopy_ = device.createTexture({
            .format = rhi::TextureFormat::R32Float,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = renderWidth_,
            .height = renderHeight_,
            .debugName = "surface-scene-depth",
        });
    }
    if (colorWanted && !lookTexture(device, sceneColorCopy_, renderWidth_, renderHeight_, "surface-scene-color"))
        colorWanted = false;
    if (!sceneDepthCopy_.valid())
        return;

    cmd.endRenderPass();
    const Mat4 inverseProjection = core::inverse(world.camera.projection);
    const std::array<rhi::TextureBinding, 1> depth{rhi::TextureBinding{depth_, pointSampler_}};
    fullscreenPass(cmd, surfaceSceneDepth_.handle, sceneDepthCopy_, renderWidth_, renderHeight_, "surface-scene-depth",
                   depth, asBytes(&inverseProjection, sizeof(inverseProjection)));
    if (colorWanted) {
        const std::array<rhi::TextureBinding, 1> color{rhi::TextureBinding{hdr_, linearSampler_}};
        fullscreenPass(cmd, resample_.handle, sceneColorCopy_, renderWidth_, renderHeight_, "surface-scene-color",
                       color, {});
    }

    const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
        .texture = hdr_,
        .loadOp = rhi::LoadOp::Load,
        .storeOp = rhi::StoreOp::Store,
    }};
    cmd.beginRenderPass({
        .colorAttachments = resumeTarget,
        .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
        .debugName = "forward-after-scene-copy",
    });
    cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
    cmd.setScissor({.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
    // The frame block again: the copies bound their own at the same slot.
    cmd.setPipeline(pbrBlendPipeline_);
    cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
}

void DefaultRenderer::releaseLightmap(rhi::IDevice& device)
{
    if (lightmapPages_.valid())
        device.destroy(lightmapPages_);
    lightmapPages_ = {};
    if (lightmapRects_.valid())
        device.destroy(lightmapRects_);
    lightmapRects_ = {};
    lightmapPageSize_ = 0;
    lightmapPageCount_ = 0;
    lightmapLayers_ = 0;
    lightmapLayersPerPage_ = 1;
    lightmapFormat_ = rhi::TextureFormat::Undefined;
    lightmapRectsBytes_ = 0;
    lightmapStamp_ = 0;
}

void DefaultRenderer::uploadLightmap(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderLightmap& lightmap)
{
    ENG_PROFILE_SCOPE("render.lightmap_upload");
    lightmapStamp_ = lightmap.stamp;
    const LightmapPages* pages = lightmap.pages.get();
    const bool encoded = pages != nullptr && !pages->light.empty();
    const bool whole =
        pages != nullptr && pages->size > 0 && pages->count > 0 &&
        (encoded ? pages->light.size() == pages->count && pages->masks.size() == pages->count
                 : pages->texels.size() == static_cast<core::usize>(pages->size) * pages->size * 4 * pages->count);
    if (lightmap.stamp == 0 || !whole || lightmap.rects.empty()) {
        // **No bake, or none whole**: every part is drawn as it was.
        releaseLightmap(device);
        lightmapStamp_ = lightmap.stamp;
        return;
    }
    // **In the blocks the card samples** (ADR 0253 §6): a page kept as UASTC
    // HDR is BC6H's or ASTC's, a byte a texel, with its mask the layer after
    // it -- blocks have no alpha. Half floats with the mask in their alpha
    // where the card samples neither, and for a bake not yet kept.
    const rhi::Capabilities caps = device.caps();
    const asset::HdrBlocks blocks = !encoded               ? asset::HdrBlocks::Half
                                    : caps.bc6hTextures    ? asset::HdrBlocks::Bc6h
                                    : caps.astcHdrTextures ? asset::HdrBlocks::Astc4x4
                                                           : asset::HdrBlocks::Half;
    const rhi::TextureFormat format = blocks == asset::HdrBlocks::Bc6h      ? rhi::TextureFormat::Bc6hRgbUfloat
                                      : blocks == asset::HdrBlocks::Astc4x4 ? rhi::TextureFormat::Astc4x4Float
                                                                            : kHdrFormat;
    const u32 perPage = blocks == asset::HdrBlocks::Half ? 1u : 2u;
    const u32 layers = pages->count * perPage;
    if (pages->size != lightmapPageSize_ || layers != lightmapLayers_ || format != lightmapFormat_ ||
        !lightmapPages_.valid()) {
        if (lightmapPages_.valid())
            device.destroy(lightmapPages_);
        lightmapPages_ = device.createTexture({
            .format = format,
            .usage = rhi::TextureUsage::Sampled,
            .width = pages->size,
            .height = pages->size,
            .layers = std::max(layers, 2u),
            .debugName = "lightmap-pages",
        });
        lightmapPageSize_ = pages->size;
        lightmapPageCount_ = pages->count;
        lightmapLayers_ = layers;
        lightmapFormat_ = format;
    }
    if (!lightmapPages_.valid()) {
        releaseLightmap(device);
        lightmapStamp_ = lightmap.stamp;
        return;
    }
    lightmapLayersPerPage_ = perPage;
    const core::usize texels = static_cast<core::usize>(pages->size) * pages->size;
    for (u32 page = 0; page < pages->count; ++page) {
        if (!encoded) {
            lightmapPixels_.resize(texels * 4);
            const f32* from = pages->texels.data() + page * texels * 4;
            for (core::usize at = 0; at < texels * 4; ++at)
                lightmapPixels_[at] = floatToHalf(from[at]);
            cmd.uploadTextureLayer(lightmapPages_, page,
                                   asBytes(lightmapPixels_.data(), lightmapPixels_.size() * sizeof(core::u16)));
            continue;
        }
        u32 width = 0;
        u32 height = 0;
        u32 maskWidth = 0;
        u32 maskHeight = 0;
        if (!asset::transcodeHdrPicture(pages->light[page], blocks, width, height, lightmapBlocks_) ||
            !asset::transcodeHdrPicture(pages->masks[page], blocks, maskWidth, maskHeight, lightmapMaskBlocks_) ||
            width != pages->size || height != pages->size || maskWidth != pages->size || maskHeight != pages->size) {
            releaseLightmap(device);
            lightmapStamp_ = lightmap.stamp;
            return;
        }
        if (blocks == asset::HdrBlocks::Half) {
            // The mask's grey into the light's alpha: four half floats a
            // texel, each.
            auto* light = reinterpret_cast<core::u16*>(lightmapBlocks_.data());
            const auto* mask = reinterpret_cast<const core::u16*>(lightmapMaskBlocks_.data());
            for (core::usize texel = 0; texel < texels; ++texel)
                light[texel * 4 + 3] = mask[texel * 4];
            cmd.uploadTextureLayer(lightmapPages_, page, lightmapBlocks_);
            continue;
        }
        cmd.uploadTextureLayer(lightmapPages_, page * 2, lightmapBlocks_);
        cmd.uploadTextureLayer(lightmapPages_, page * 2 + 1, lightmapMaskBlocks_);
    }
    {
        // What the light costs the card, said once a bake.
        const core::usize bytes = static_cast<core::usize>(layers) * texels * (perPage == 1 ? 8u : 1u);
        const std::array<core::I18nArg, 4> args{
            core::I18nArg{"pages", static_cast<core::i64>(pages->count)},
            core::I18nArg{"size", static_cast<core::i64>(pages->size)},
            core::I18nArg{"format", std::string_view{blocks == asset::HdrBlocks::Bc6h      ? "BC6H"
                                                     : blocks == asset::HdrBlocks::Astc4x4 ? "ASTC HDR"
                                                                                           : "RGBA16F"}},
            core::I18nArg{"bytes", static_cast<core::i64>(bytes)},
        };
        core::log(core::LogLevel::Info, ENG_TR("render.info.lightmap_uploaded"), args);
    }
    // The rows: a buffer as large as they are, kept while it is large enough.
    const auto bytes = static_cast<u32>(lightmap.rects.size() * sizeof(f32));
    if (bytes > lightmapRectsBytes_ || !lightmapRects_.valid()) {
        if (lightmapRects_.valid())
            device.destroy(lightmapRects_);
        lightmapRects_ = device.createBuffer({
            .usage = rhi::BufferUsage::GraphicsStorageRead,
            .sizeBytes = bytes,
            .debugName = "lightmap-rects",
        });
        lightmapRectsBytes_ = lightmapRects_.valid() ? bytes : 0;
    }
    if (lightmapRects_.valid())
        cmd.upload(lightmapRects_, asBytes(lightmap.rects.data(), bytes), 0);
}

void DefaultRenderer::bindSkinPalette(rhi::ICmdList& cmd, const RenderWorld& world, const DrawItem& draw,
                                      bool afterMorphs)
{
    // Where its joints begin in the frame's palette and how many there are:
    // none, and it is drawn as modelled.
    GpuSkinUniforms skin;
    const bool posed =
        paletteBuffer_.valid() && draw.firstBone < world.bones.size() && world.bones.size() <= kMaxPaletteJoints;
    skin.palette[0] = posed ? draw.firstBone : 0u;
    skin.palette[1] =
        posed ? static_cast<u32>(std::min<usize>(draw.boneCount, world.bones.size() - draw.firstBone)) : 0u;
    cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&skin, sizeof(skin)));
    // The morph table binds itself and this buffer after it.
    if (!afterMorphs && paletteBuffer_.valid()) {
        const std::array<rhi::BufferHandle, 1> palettes{paletteBuffer_};
        cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, palettes);
    }
}

void DefaultRenderer::buildInstanceBatches(const RenderWorld& world, const MeshCache& meshes)
{
    ENG_PROFILE_SCOPE("render.batches");
    batches_.clear();
    instanceStaging_.clear();
    bakedInstanceStaging_.clear();
    anyBakedRun_ = false;
    skinnedInstanceStaging_.clear();
    batchBounds_.clear();
    batchOf_.assign(world.draws.size(), kNoBatch);
    if (!settings_.instancing)
        return;
    // Skinned runs only once their pipelines exist (`ensureSkinnedInstancing`),
    // and only while the palettes fit.
    const bool skinnedRuns = pbrSkinnedInstancedPipeline_.valid() && shadowSkinnedInstancedPipeline_.valid() &&
                             depthPrepassSkinnedInstancedPipeline_.valid() && skinnedInstanceBuffer_.valid() &&
                             paletteBuffer_.valid() && world.bones.size() <= kMaxPaletteJoints;
    if (!world.camera.valid)
        return;

    // The SAME level the submission will choose, from the same function and the
    // same camera -- a batch whose members disagreed about their level of detail
    // would draw one mesh with another's index range.
    const f32 pixelsPerUnit = lodPixelsPerUnit(world.camera, height_);

    // **A blended draw is batched where its order survives it.** Their order
    // is their correctness, and `drawSortKey` sorts them by material and then
    // from the far one to the near one -- so the members of one family that
    // stand next to each other in the list are drawn one after another
    // already, and a call that draws them as instances, in that same order,
    // puts the same fragments down in the same sequence. What a run may not
    // do is reorder, which is why it is staged as it was sorted and not as
    // `orderInstanceRun` would have it. A fight's effects are this: forty
    // shards of one mesh and one look were forty calls.
    //
    // Left alone: a part thinned by `Fade` (it still casts, as the solid part
    // it is), a skinned one, a block world's chunk, a surface shader's
    // material (its blended pipeline is not instanced), and anything thinned
    // to nothing, which the pass skips by its first member.
    const auto blendedRun = [&](const DrawItem& draw) {
        return draw.transparent && pbrInstancedBlendPipeline_.valid() && !draw.fadedOnly && draw.boneCount == 0 &&
               !draw.voxelBlock && !draw.terrain && draw.alpha > 0.0f &&
               !(draw.material < materialSurface_.size() && materialSurface_[draw.material] != 0);
    };
    const auto instanceable = [&](const DrawItem& draw) {
        // Skinned draws were not batched at first -- a joint palette is per
        // draw and there is no room for one in a vertex stream.
        //
        // **Selection is NOT a reason to leave a batch**. The first cut
        // of the outline pass excluded an outlined draw here, which split the
        // run it was in -- and a run of identical meshes is how a whole forest
        // is drawn. Whatever that split disturbed downstream, it took every
        // boulder and every tree canopy in the flagship out of the frame the
        // moment one of them was selected. The batching the entire frame
        // depends on is not the place to solve a tool's problem: the outline
        // pass ignores batching instead, which is where the cost belongs and
        // where it is a handful of draws.
        //
        // **A skinned draw is batched too**, once the palettes can be
        // read by instance: five hundred animated enemies were 1,800 draws.
        //
        // **A body with a morph target above nought is drawn alone** (ADR
        // 0196): its weights are its own. One with every weight at nought has
        // no row and is here like any other -- which is the whole cost of
        // targets to a horde that is not using them.
        if (draw.morph != NoMorph && morphReady_)
            return false;
        // And so is a cloth (ADR 0214): its vertices are its own too.
        if (draw.cloth != NoCloth)
            return false;
        if (draw.transparent)
            return blendedRun(draw);
        return draw.boneCount == 0 || skinnedRuns;
    };

    for (core::usize index = 0; index < world.draws.size();) {
        const DrawItem& first = world.draws[index];
        if (!instanceable(first)) {
            ++index;
            continue;
        }
        const MeshCache::Resolved* resolved = meshes.resolve(first.mesh);
        if (resolved == nullptr || resolved->lods.empty()) {
            ++index;
            continue;
        }
        // A palette with the skin stream to read it, as the single skinned
        // draw requires; a draw with one and not the other is drawn alone.
        const bool skinned = first.boneCount > 0;
        if (skinned && !resolved->skin.valid()) {
            ++index;
            continue;
        }
        const u32 lod =
            selectMeshLod(*resolved, first.transform, pixelsPerUnit, settings_.lodBias, settings_.maximumLodLevel);

        core::usize last = index + 1;
        while (last < world.draws.size()) {
            const DrawItem& next = world.draws[last];
            // By FAMILY, not by material: a run of parts that differ only by
            // colour is one call, each colour in its instance.
            if (!instanceable(next) || !(next.mesh == first.mesh) || next.section != first.section ||
                world.familyOf(next.material) != world.familyOf(first.material) || (next.boneCount > 0) != skinned)
                break;
            // The blended pass and the solid ones are two passes.
            if (next.transparent != first.transparent)
                break;
            // A run is drawn into a shadow map whole or not at all: one that
            // casts and one that does not are two runs (`BasePart.CastShadow`).
            if (next.castShadow != first.castShadow)
                break;
            // And into the decals' mask whole or not at all
            // (`BasePart.ReceivesDecals`).
            if (next.receivesDecals != first.receivesDecals)
                break;
            // A baked part and one that is not are drawn by two pipelines.
            if ((next.lightmap != NoLightmap) != (first.lightmap != NoLightmap))
                break;
            // A surface's colour is in its block, not in the instance's tint:
            // one material per run.
            const bool surfaced = first.material < materialSurface_.size() && materialSurface_[first.material] != 0;
            if (surfaced && next.material != first.material)
                break;
            if (selectMeshLod(*resolved, next.transform, pixelsPerUnit, settings_.lodBias, settings_.maximumLodLevel) !=
                lod)
                break;
            ++last;
        }

        const auto count = static_cast<u32>(last - index);
        const core::usize staged = skinned ? skinnedInstanceStaging_.size() : instanceStaging_.size();
        const core::usize room = skinned ? kMaxSkinnedInstances : kMaxInstances;
        // **A baked part is drawn as a run of its own however few there
        // are** (ADR 0253): its light is read by the instanced pipeline alone.
        const bool baked = first.lightmap != NoLightmap && !skinned && !first.transparent &&
                           resolved->lightmap.valid() && pbrBakedInstancedPipeline_.valid() &&
                           bakedInstanceBuffer_.valid() && lightmapPages_.valid() && lightmapRects_.valid() &&
                           world.lightmap.stamp == lightmapStamp_ && lightmapStamp_ != 0;
        if ((count < kMinInstanceBatch && !baked) || staged + count > room) {
            index = last;
            continue;
        }

        InstanceBatch batch;
        batch.firstDraw = static_cast<u32>(index);
        batch.firstInstance = static_cast<u32>(staged);
        batch.count = count;
        batch.lod = lod;
        batch.skinned = skinned;
        batch.baked = baked;
        anyBakedRun_ = anyBakedRun_ || baked;

        // The union sphere, grown one member at a time. Conservative in the
        // direction that never drops geometry, which is the only direction a
        // cull may be wrong in.
        batch.boundsCenter = world.draws[index].boundsCenter;
        batch.boundsRadius = world.draws[index].boundsRadius;
        for (core::usize member = index; member < last; ++member) {
            const DrawItem& draw = world.draws[member];
            batchOf_[member] = static_cast<u32>(batches_.size());
            batch.anyVisible = batch.anyVisible || draw.inCameraFrustum;

            const Vec3 offset = draw.boundsCenter - batch.boundsCenter;
            const f32 distance = core::length(offset);
            if (distance + draw.boundsRadius > batch.boundsRadius) {
                const f32 grown = 0.5f * (batch.boundsRadius + distance + draw.boundsRadius);
                if (distance > 1e-6f)
                    batch.boundsCenter = batch.boundsCenter + offset * ((grown - batch.boundsRadius) / distance);
                batch.boundsRadius = grown;
            }
        }

        // **Staged in the order a pass can take a piece of** (ADR 0182): the
        // members the camera sees first, as they were ordered, then the rest
        // by distance. The union above is grown in the order it always was --
        // a sphere grown in another order is another sphere.
        runBounds_.clear();
        runInFrustum_.clear();
        for (core::usize member = index; member < last; ++member) {
            runBounds_.push_back({world.draws[member].boundsCenter, world.draws[member].boundsRadius});
            runInFrustum_.push_back(world.draws[member].inCameraFrustum ? core::u8{1} : core::u8{0});
        }
        if (first.transparent) {
            // As sorted, far to near, and all of it: see `blendedRun`.
            runOrder_.resize(count);
            for (u32 place = 0; place < count; ++place)
                runOrder_[place] = place;
            batch.visibleCount = count;
        }
        else {
            batch.visibleCount = orderInstanceRun(runBounds_, runInFrustum_, runOrder_);
        }
        batch.firstBounds = static_cast<u32>(batchBounds_.size());
        for (const u32 place : runOrder_) {
            const DrawItem& draw = world.draws[index + place];
            batchBounds_.push_back(runBounds_[place]);

            GpuInstance instance;
            instance.model = draw.transform;
            instance.alphaTint[0] = draw.alpha;
            const GpuMaterialUniforms& own = world.materials[draw.material].uniforms;
            instance.alphaTint[1] = own.baseColor[0];
            instance.alphaTint[2] = own.baseColor[1];
            instance.alphaTint[3] = own.baseColor[2];
            if (skinned) {
                GpuSkinnedInstance posed;
                posed.model = instance.model;
                std::copy_n(instance.alphaTint, 4, posed.alphaTint);
                posed.palette[0] = static_cast<f32>(draw.firstBone);
                skinnedInstanceStaging_.push_back(posed);
            }
            else {
                instanceStaging_.push_back(instance);
                // Its rows, whether the mask holds, and a page's layers.
                bakedInstanceStaging_.push_back(
                    baked ? std::array<f32, 4>{static_cast<f32>(draw.lightmap), world.lightmap.maskHolds ? 1.0f : 0.0f,
                                               static_cast<f32>(lightmapLayersPerPage_), 0.0f}
                          : std::array<f32, 4>{});
            }
        }

        batches_.push_back(batch);
        index = last;
    }
}

void DefaultRenderer::updateEnvironment(rhi::ICmdList& cmd, const SkyParams& params)
{
    ENG_PROFILE_SCOPE("render.environment");
    core::profile::Sections sections;
    ENG_PROFILE_NEXT(sections, "environment.brdf");
    const auto uploadLevel = [&](u32 level) {
        const std::vector<core::u16>& pixels = environment_.levels[level];
        if (pixels.empty())
            return;
        cmd.uploadTexture(environmentMap_, asBytes(pixels.data(), pixels.size() * sizeof(core::u16)), level);
        skyLevelHash_[level] = environment_.hashes[level];
        skyLevelHeld_[level] = true;
    };
    // **The texture is every view's, and its first layer the sky of the one
    // being drawn** (ADR 0202). Whatever of this view's sky it does not hold
    // -- another view's was drawn since, or the texture is new -- is uploaded
    // before the frame reads it. A game with one view, or several under one
    // sky, uploads nothing here: the same sky is the same numbers.
    const auto uploadOthers = [&](u32 except) {
        for (u32 level = 0; level < kEnvironmentMipCount; ++level) {
            if (level != except && !environment_.levels[level].empty() &&
                (!skyLevelHeld_[level] || skyLevelHash_[level] != environment_.hashes[level]))
                uploadLevel(level);
        }
    };
    const auto bakeLevel = [&](u32 level) {
        const u32 size = environmentLevelSize(level);
        const f32 roughness =
            kEnvironmentMipCount > 1 ? static_cast<f32>(level) / static_cast<f32>(kEnvironmentMipCount - 1) : 0.0f;
        environment_.levels[level].assign(static_cast<core::usize>(size) * size * 4, 0);
        bakeEnvironmentLevel(environment_.target, size, roughness, environmentSampleCount(level),
                             environment_.levels[level]);
        // `Lighting.EnvironmentSpecularScale`, applied where the chain is made
        // and skipped at its default, so a world that never sets it bakes the
        // same halves it always did.
        if (environment_.target.specularScale != 1.0f) {
            for (core::u16& half : environment_.levels[level])
                half = floatToHalf(halfToFloat(half) * environment_.target.specularScale);
        }
        core::u64 hash = 1469598103934665603ull;
        for (const core::u16 half : environment_.levels[level])
            hash = (hash ^ half) * 1099511628211ull;
        environment_.hashes[level] = hash;
        environment_.dirty[level] = false;
    };

    if (!brdfUploaded_) {
        // Independent of the environment -- it is the BRDF integrated against
        // itself -- so it is baked once and never again.
        environment_.lut.assign(static_cast<core::usize>(kBrdfLutSize) * kBrdfLutSize * 4, 0);
        bakeBrdfLut(kBrdfLutSize, environment_.lut);
        cmd.uploadTexture(brdfLut_, asBytes(environment_.lut.data(), environment_.lut.size() * sizeof(core::u16)), 0);
        brdfUploaded_ = true;
    }

    // **The diffuse half has its own staleness, four times tighter than the
    // specular one.** What the specular chain pays for a rebuild is six
    // texture bakes and six uploads; what this pays is one projection of the
    // sky onto nine numbers. They are different prices and they were sharing a
    // threshold, which set the diffuse one by what the expensive half could
    // afford -- and a light every matte surface receives is exactly the one that
    // must not arrive in steps.
    ENG_PROFILE_NEXT(sections, "environment.irradiance");
    if (environment_.irradianceStale(params)) {
        environment_.irradianceSky = params;
        bakeIrradianceSh(params, environment_.irradianceTarget);
    }

    ENG_PROFILE_NEXT(sections, "environment.prefilter");
    if (environment_.stale(params)) {
        environment_.target = params;
        for (bool& level : environment_.dirty)
            level = true;
    }

    // **A sun that jumps is a cut, not a motion, and it is taken whole.**
    //
    // The blend below is sized for a sky the clock walks across. A sky that
    // arrives somewhere else between one frame and the next is a script
    // scrubbing `ClockTime`, a scene loading, or a fixture that steps a quarter
    // of an hour per frame -- and easing into those over a fifth of a second is
    // a fade nobody asked for. The threshold is the specular chain's own: a sun
    // that moves further than that in ONE frame is moving faster than the
    // environment can track at all, so there is nothing to protect.
    const bool cut = !environment_.everBaked || !environment_.hasPreviousSky ||
                     core::dot(params.sunDirection, environment_.previousSky.sunDirection) < kEnvironmentRebuildCosine;
    environment_.previousSky = params;
    environment_.hasPreviousSky = true;

    if (!environment_.everBaked) {
        for (u32 level = 0; level < kEnvironmentMipCount; ++level) {
            bakeLevel(level);
            uploadLevel(level);
        }
        for (u32 index = 0; index < 9; ++index)
            environment_.irradiance[index] = environment_.irradianceTarget[index];
        environment_.everBaked = true;
        environment_.cursor = 0;
        return;
    }

    // **The diffuse ambient walks towards its target rather than arriving at it.
    // ** The bake above happens on one frame in about a hundred; before
    // this, its result was handed to the shader whole on that frame, so the
    // light every matte surface in the world receives held still and then
    // stepped by about a per cent. A human running the flagship reported the
    // ground pulsing, and said it started around eleven in the morning -- which
    // is the hour band where the sky's irradiance changes fastest with the sun
    // in this model, so the accumulated step is largest.
    //
    // A per-frame rate rather than a time constant, which is the same shape the
    // exposure adaptation two hundred lines below uses and keeps a headless run
    // reproducible. At a twelfth per frame a step is spread over about a fifth
    // of a second: slow enough that no frame carries a visible jump, fast
    // enough that the ambient is never more than one bake behind the sky.
    const f32 rate = cut ? 1.0f : kEnvironmentIrradianceRate;
    for (u32 index = 0; index < 9; ++index) {
        environment_.irradiance[index] = environment_.irradiance[index] +
                                         (environment_.irradianceTarget[index] - environment_.irradiance[index]) * rate;
    }

    const u32 level = environment_.cursor;
    environment_.cursor = (environment_.cursor + 1) % kEnvironmentMipCount;
    if (environment_.dirty[level])
        bakeLevel(level);
    uploadLevel(level);
    uploadOthers(level);
}

void DefaultRenderer::drawGeometry(rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes,
                                   const Mat4& viewProjection, rhi::PipelineHandle staticPipeline,
                                   rhi::PipelineHandle skinnedPipeline, Selection selection, const CullSphere* cull)
{
    ENG_PROFILE_SCOPE("render.draws");
    // The two masks -- a tool's selection and a game's highlight -- are one
    // kind of pass and differ only in which draws they take.
    const bool mask = selection == Selection::Outline || selection == Selection::Highlight;
    const bool depthOnly = selection == Selection::Shadow || selection == Selection::Prepass || mask;
    // Which pipeline is currently set. Three variants now rather than two, so a
    // handle is clearer than a bool -- and `extract`'s sort keeps runs of each
    // together, so this switches a handful of times per pass whatever the scene.
    rhi::PipelineHandle currentPipeline = staticPipeline;
    const rhi::PipelineHandle instancedPipeline = selection == Selection::Shadow        ? shadowInstancedPipeline_
                                                  : selection == Selection::Prepass     ? depthPrepassInstancedPipeline_
                                                  : selection == Selection::Transparent ? pbrInstancedBlendPipeline_
                                                                                        : pbrInstancedPipeline_;
    const rhi::PipelineHandle skinnedInstancedPipeline =
        selection == Selection::Shadow    ? shadowSkinnedInstancedPipeline_
        : selection == Selection::Prepass ? depthPrepassSkinnedInstancedPipeline_
                                          : pbrSkinnedInstancedPipeline_;

    // Pixels per world unit at one metre, from the projection itself rather
    // than from a field-of-view nobody stored: `projection[1][1]` IS
    // `1 / tan(fovY / 2)` for `core::perspective`, so half the target height
    // times that is the number a metre subtends at a metre away.
    //
    // Taken from the CAMERA even in the shadow pass, deliberately. A level
    // chosen by how big a thing looks to the LIGHT would change with the sun,
    // so a shadow could be cast by different geometry than the object drawn --
    // which is a shadow that does not match its caster. Choosing once, from the
    // camera, keeps the two the same mesh.
    const f32 pixelsPerUnit = world.camera.valid ? lodPixelsPerUnit(world.camera, height_) : 0.0f;
    // The draws arrive sorted, so this walks them in order and
    // never reorders. Grouping is `extract`'s job and re-deriving it here would
    // be the backend doing work bgfx would have to repeat.
    u32 boundMaterial = 0xFFFFFFFFu;
    core::InstanceId boundTerrain{};

    for (core::usize drawIndex = 0; drawIndex < world.draws.size(); ++drawIndex) {
        const DrawItem& draw = world.draws[drawIndex];

        // A batched run is drawn once, by its first member, and every other
        // member is skipped -- one call cannot be issued in pieces, which is
        // also why a batch is culled as a whole.
        const u32 batchIndex = drawIndex < batchOf_.size() ? batchOf_[drawIndex] : kNoBatch;
        // **The outline pass ignores batching.** A batch is drawn or skipped as
        // a whole, and selecting one of five identical crates does not select
        // the other four -- so an outlined draw inside a batch would outline
        // all five or none. Treating every draw as its own here costs a handful
        // of calls, because only what is selected is drawn at all, and it
        // leaves the batching the rest of the frame is built on untouched.
        const InstanceBatch* batch = mask || batchIndex == kNoBatch ? nullptr : &batches_[batchIndex];
        if (batch != nullptr && batch->firstDraw != drawIndex)
            continue;
        // **A material whose pipelines are being made is drawn in no pass**
        // (ADR 0257): no shadow, no depth, no body, until they are -- a run is
        // one material, so it waits whole.
        if (draw.material < materialWaiting_.size() && materialWaiting_[draw.material])
            continue;

        // The shadow pass takes every solid draw; the forward passes take only
        // what the camera can see. A caster behind the camera still casts into
        // the frame.
        const bool visible = batch != nullptr ? batch->anyVisible : draw.inCameraFrustum;
        if (selection != Selection::Shadow && !visible)
            continue;
        // And what is see-through casts none (`castsShadow`).
        if (selection == Selection::Shadow && !castsShadow(draw))
            continue;
        // Nor, past the nearest cascade, a run drawn with its bake's mask.
        if (selection == Selection::Shadow && shadowLeavesBaked_ && batch != nullptr && batch->baked)
            continue;
        // A part this machine thinned to nothing (`BasePart.Fade`) is in the
        // list for its shadow alone: the camera's blended pass has none of it.
        if (selection == Selection::Transparent && draw.alpha <= 0.0f)
            continue;
        // **A frame with a decal draws its solid parts in two turns**:
        // those a decal paints before the decals, those it does not after.
        if (selection == Selection::Opaque && decalTurn_ != DecalTurn::Every &&
            draw.receivesDecals != (decalTurn_ == DecalTurn::Receiving))
            continue;
        if (selection == Selection::Prepass && draw.cutout)
            continue;
        if ((selection == Selection::Opaque || selection == Selection::Prepass) && draw.transparent)
            continue;
        // A transparent selected part still gets an outline: what is selected
        // is a fact about the tool, not about the material.
        if (selection == Selection::Outline && !draw.outlined)
            continue;
        if (selection == Selection::Highlight && draw.highlight != highlightFilter_)
            continue;
        if (selection == Selection::Transparent && !draw.transparent)
            continue;
        if (cull != nullptr) {
            const Vec3 centre = batch != nullptr ? batch->boundsCenter : draw.boundsCenter;
            const f32 radius = batch != nullptr ? batch->boundsRadius : draw.boundsRadius;
            if (!casterReaches(cull->centre, cull->radius, cull->sweep, centre, radius))
                continue;
        }

        // **The pieces of a run this pass draws** (ADR 0182): what the camera
        // sees of it, for the camera's passes; for a shadow map, the members
        // that reach what the map covers -- a piece of those the camera sees
        // and a piece of those it does not, each a range of the order staged.
        std::array<InstancePiece, 2> pieces{};
        u32 pieceCount = 0;
        if (batch != nullptr) {
            if (selection != Selection::Shadow) {
                pieces[pieceCount++] = InstancePiece{0, batch->visibleCount};
            }
            else if (cull == nullptr) {
                pieces[pieceCount++] = InstancePiece{0, batch->count};
            }
            else {
                const std::span<const ShadowCasterBounds> members{batchBounds_.data() + batch->firstBounds,
                                                                  batch->count};
                for (const auto& [from, to] : {std::pair<u32, u32>{0u, batch->visibleCount},
                                               std::pair<u32, u32>{batch->visibleCount, batch->count}}) {
                    const InstancePiece piece =
                        castersReaching(cull->centre, cull->radius, cull->sweep, members, from, to);
                    if (piece.count == 0)
                        continue;
                    if (pieceCount > 0 && pieces[pieceCount - 1].first + pieces[pieceCount - 1].count == piece.first)
                        pieces[pieceCount - 1].count += piece.count;
                    else
                        pieces[pieceCount++] = piece;
                }
            }
            if (pieceCount == 0 || pieces[0].count == 0)
                continue;
        }

        const MeshCache::Resolved* resolved = meshes.resolve(draw.mesh);
        if (resolved == nullptr || resolved->lods.empty())
            continue;

        const u32 lod = batch != nullptr ? batch->lod
                                         : selectMeshLod(*resolved, draw.transform, pixelsPerUnit, settings_.lodBias,
                                                         settings_.maximumLodLevel);
        const MeshLodRange& level = resolved->lods[lod];
        if (draw.section >= level.sectionCount || level.firstSection + draw.section >= resolved->sections.size())
            continue;
        const MeshSection& section = resolved->sections[level.firstSection + draw.section];
        if (section.indexCount == 0)
            continue;

        // A draw is skinned only if it has a palette AND the mesh carries the
        // second stream. The two can disagree for exactly one frame -- a mesh
        // whose file failed to load has no skin buffer while a track already
        // exists -- and drawing that through the skinned pipeline would read an
        // unbound vertex buffer.
        //
        // **A cloth is not** (ADR 0214): its vertices are where the cloth
        // hangs, in the mesh's own space, and are added to the mesh as it was
        // modelled through the morph pipelines -- a target of weight one,
        // read from this frame's rows. The masks draw it posed, as it was
        // before the cloth: a silhouette, for the reason a face's is.
        const bool clothDraw = !mask && batch == nullptr && morphReady_ && clothBuffer_.valid() &&
                               draw.cloth < world.cloths.size() && resolved->vertexOffset == 0 && !draw.terrain &&
                               !draw.voxelBlock;
        const bool skinnedDraw =
            batch == nullptr && !clothDraw && draw.boneCount > 0 && resolved->skin.valid() && skinnedPipeline.valid();
        // A run of them, posed from the frame's palette buffer.
        const bool skinnedRun = batch != nullptr && batch->skinned;
        // **A body its morph targets move this frame** (ADR 0196). The masks
        // -- a selection's outline, a highlight -- draw it at rest: they want
        // a silhouette, and a face's targets move it by less than the line is
        // wide.
        //
        // **Only a mesh whose vertices start at nought in their buffer.** The
        // vertex stage finds a vertex's deltas by the vertex's number, and
        // with a base vertex Direct3D numbers from the mesh where Vulkan and
        // Metal number from the buffer. `MeshCache::attachMorphs` gives a
        // table to no other mesh; this is the same statement where the
        // pipeline is chosen, so that changing the one cannot quietly break
        // the other.
        const bool morphDraw =
            clothDraw || (!mask && batch == nullptr && morphReady_ && draw.morph < world.morphs.size() &&
                          resolved->morph.valid() && resolved->vertexOffset == 0 && !draw.terrain && !draw.voxelBlock);
        // A terrain mesh in the opaque pass is drawn with the terrain's look,
        // and in the shadow pass with no culling and a push from the light (see
        // the cascade loop); in the prepass it is an ordinary static mesh.
        const bool terrainDraw =
            selection == Selection::Opaque && batch == nullptr && draw.terrain && terrainForwardPipeline().valid();
        const bool terrainShadow =
            selection == Selection::Shadow && batch == nullptr && draw.terrain && terrainShadowPipeline_.valid();
        const bool terrainPrepass =
            selection == Selection::Prepass && batch == nullptr && draw.terrain && terrainPrepassPipeline_.valid();
        const bool voxelDraw = (selection == Selection::Opaque || selection == Selection::Transparent) &&
                               batch == nullptr && draw.voxelBlock && voxelPipeline_.valid() &&
                               voxelBlendPipeline_.valid();
        // A leaf's shadow has the leaf's holes: the cutout faces cast through
        // the hole test rather than as the solid squares their mesh is.
        const bool leafShadow = selection == Selection::Shadow && batch == nullptr && draw.voxelBlock && draw.cutout &&
                                voxelShadowPipeline_.valid();
        // A surface shader's own pipelines, for a plain or instanced mesh (ADR
        // 0091). Skinned, terrain and voxel geometry keep the built-in surface,
        // and so does the outline mask, which wants a position and nothing else.
        //
        // **And so does a mesh that has morph targets**, whatever its weights
        // are this frame: its morphed draw has no surface pipeline, and a face
        // that changed its look the moment it spoke would be worse than one
        // that never had the surface.
        const u32 surfaceId = !mask && !skinnedDraw && !skinnedRun && !clothDraw && !draw.terrain && !draw.voxelBlock &&
                                      !resolved->morph.valid() && draw.material < materialSurface_.size()
                                  ? materialSurface_[draw.material]
                                  : 0u;
        const SurfaceSet* surface = surfaceId != 0 ? &surfaces_[surfaceId - 1] : nullptr;
        // **A run of baked parts in the lit pass** (ADR 0253); in every other
        // pass it is an instanced run like any.
        const bool bakedRun = batch != nullptr && batch->baked && selection == Selection::Opaque && surface == nullptr;
        // A masked surface cuts itself in its fragment, which its depth pass
        // does not run: left in the prepass, its holes would show whatever the
        // prepass depth hid -- the sky -- instead of what is behind them.
        if (selection == Selection::Prepass && surface != nullptr && world.materials[draw.material].masked)
            continue;
        // **What the prepass drew is tested against its depth and writes none**
        // (ADR 0174): every opaque draw but a cutout and a masked surface,
        // which the prepass leaves out above and which write their own.
        const bool prepassed = prepassDrawn_ && selection == Selection::Opaque && !draw.cutout &&
                               !(surface != nullptr && world.materials[draw.material].masked);
        const auto unlessPrepassed = [prepassed](rhi::PipelineHandle writing, rhi::PipelineHandle testing) {
            return prepassed && testing.valid() ? testing : writing;
        };
        const rhi::PipelineHandle surfacePipeline =
            surface == nullptr                    ? rhi::PipelineHandle{}
            : selection == Selection::Shadow      ? (batch != nullptr ? surface->shadowInstanced : surface->shadow)
            : selection == Selection::Prepass     ? (batch != nullptr ? surface->prepassInstanced : surface->prepass)
            : selection == Selection::Transparent ? surface->blended
            : batch != nullptr                    ? unlessPrepassed(surface->instanced, surface->instancedPrepassed)
                                                  : unlessPrepassed(surface->forward, surface->forwardPrepassed);
        const MorphPipelines& morphSet = morphPipelines_[skinnedDraw ? 1 : 0];
        const rhi::PipelineHandle morphPipeline = !morphDraw                        ? rhi::PipelineHandle{}
                                                  : selection == Selection::Shadow  ? morphSet.shadow
                                                  : selection == Selection::Prepass ? morphSet.prepass
                                                  : selection == Selection::Transparent
                                                      ? morphSet.blend
                                                      : unlessPrepassed(morphSet.forward, morphSet.forwardPrepassed);
        const rhi::PipelineHandle wanted =
            surfacePipeline.valid() ? surfacePipeline
            : skinnedRun            ? unlessPrepassed(skinnedInstancedPipeline, pbrSkinnedInstancedPrepassedPipeline_)
            : bakedRun              ? unlessPrepassed(pbrBakedInstancedPipeline_, pbrBakedInstancedPrepassedPipeline_)
            : batch != nullptr      ? unlessPrepassed(instancedPipeline, pbrInstancedPrepassedPipeline_)
            : morphDraw             ? morphPipeline
            : skinnedDraw           ? unlessPrepassed(skinnedPipeline, pbrSkinnedPrepassedPipeline_)
            : terrainDraw           ? terrainForwardPipeline()
            : terrainShadow         ? terrainShadowPipeline_
            : terrainPrepass        ? terrainPrepassPipeline_
            : voxelDraw
                ? (selection == Selection::Transparent ? voxelBlendPipeline_
                                                       : unlessPrepassed(voxelPipeline_, voxelPrepassedPipeline_))
            : leafShadow                     ? voxelShadowPipeline_
            : selection == Selection::Opaque ? unlessPrepassed(staticPipeline, pbrPrepassedPipeline_)
                                             : staticPipeline;
        if (!(wanted == currentPipeline)) {
            cmd.setPipeline(wanted);
            currentPipeline = wanted;
        }

        if (depthOnly) {
            // The instanced path reads its model matrix from the vertex stream,
            // so the block carries only the view-projection -- but it is pushed
            // per batch rather than per pass, because an ordinary draw between
            // two batches overwrites the same slot.
            const GpuShadowUniforms uniforms{viewProjection, batch != nullptr ? Mat4{} : draw.transform};
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&uniforms, sizeof(uniforms)));
            if (surfacePipeline.valid())
                bindSurface(cmd, draw.material, false, false);
            if (terrainShadow || terrainPrepass) {
                // The push, and the geomorph: a shadow is cast by the ground as
                // it is drawn, and the prepass writes where it is.
                std::array<f32, 40> push{};
                if (terrainShadow) {
                    push[0] = terrainShadowPush_.constant;
                    push[1] = terrainShadowPush_.reach;
                    push[2] = terrainShadowPush_.most;
                }
                if (draw.terrainMorph < world.terrainMorphs.size())
                    std::copy_n(world.terrainMorphs[draw.terrainMorph].rows.begin(), 36, push.begin() + 4);
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(push.data(), sizeof(push)));
            }
            if (leafShadow && boundMaterial != kVoxelBinding) {
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&voxelParams_, sizeof(voxelParams_)));
                const std::array<rhi::BufferHandle, 1> blocks{voxelBlockBuffer_};
                cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, blocks);
                const std::array<rhi::TextureBinding, 1> atlas{
                    voxelAtlas_.valid() ? rhi::TextureBinding{voxelAtlas_, pointSampler_}
                                        : rhi::TextureBinding{whitePixel_, pointSampler_},
                };
                cmd.bindTextures(rhi::ShaderStage::Fragment, 0, atlas);
                boundMaterial = kVoxelBinding;
            }
        }
        else {
            GpuObjectUniforms uniforms{viewProjection, batch != nullptr ? Mat4{} : draw.transform,
                                       batch != nullptr ? Mat4{} : normalMatrixOf(draw.transform)};
            uniforms.instanceAlphaUnused[0] = batch != nullptr ? 1.0f : draw.alpha;
            // A terrain node's level, for the debug view of levels.
            uniforms.instanceAlphaUnused[1] = static_cast<f32>(draw.terrainLevel);
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&uniforms, sizeof(uniforms)));
            // And its geomorph (ADR 0140), beside the block every mesh has.
            if (draw.terrain) {
                const TerrainMorph morph = draw.terrainMorph < world.terrainMorphs.size()
                                               ? world.terrainMorphs[draw.terrainMorph]
                                               : TerrainMorph{};
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(morph.rows.data(), sizeof(morph.rows)));
            }

            if (voxelDraw) {
                // The registry's colours at the vertex stage's second slot, and
                // the standard textures bound to their neutral stand-ins -- once
                // per run of chunks.
                if (boundMaterial != kVoxelBinding) {
                    cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&voxelParams_, sizeof(voxelParams_)));
                    const std::array<rhi::BufferHandle, 1> blocks{voxelBlockBuffer_};
                    cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, blocks);
                    const std::array<rhi::TextureBinding, 13> textures{
                        // The block atlas in the base-colour slot, sampled
                        // without smoothing; white until the first image.
                        voxelAtlas_.valid() ? rhi::TextureBinding{voxelAtlas_, pointSampler_}
                                            : rhi::TextureBinding{whitePixel_, pointSampler_},
                        rhi::TextureBinding{flatNormalPixel_, linearSampler_},
                        rhi::TextureBinding{whitePixel_, linearSampler_},
                        rhi::TextureBinding{blackPixel_, linearSampler_},
                        rhi::TextureBinding{shadowMap_, shadowSampler_},
                        rhi::TextureBinding{environmentMap_, environmentSampler_},
                        rhi::TextureBinding{brdfLut_, environmentSampler_},
                        rhi::TextureBinding{clusterGrid_, pointSampler_},
                        rhi::TextureBinding{lightIndices_, pointSampler_},
                        rhi::TextureBinding{lightData_, pointSampler_},
                        rhi::TextureBinding{occlusion_, linearSampler_},
                        rhi::TextureBinding{localShadowMap_, shadowSampler_},
                        rhi::TextureBinding{contact_, pointSampler_},
                    };
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
                    boundMaterial = kVoxelBinding;
                }
            }
            else if (terrainDraw) {
                // The terrain's layers at the material slot, its arrays after
                // the standard textures, and those bound to their neutral
                // stand-ins -- once per run of one terrain's draws.
                if (boundMaterial != kTerrainBinding || !(draw.terrainId == boundTerrain)) {
                    const TerrainArrays* layers = terrainArraysOf(draw.terrainId);
                    static const GpuTerrainSurfaceUniforms flat{};
                    const GpuTerrainSurfaceUniforms& block = layers != nullptr ? layers->uniforms : flat;
                    // **A compiled variant binds what it reads and nothing
                    // else** (ADR 0179): the layers' colours, the sun's shadow
                    // map and the three light tables -- five textures where
                    // the full ground's layout is sixteen -- and `Flat` none.
                    const rhi::PipelineHandle forward = terrainForwardPipeline();
                    if (forward == terrainFastPipeline_ || forward == terrainFlatPipeline_) {
                        cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&block, sizeof(block)));
                        if (forward == terrainFastPipeline_) {
                            const std::array<rhi::TextureBinding, 5> compact{
                                layers != nullptr && layers->ready
                                    ? rhi::TextureBinding{layers->arrays[0], linearSampler_}
                                    : rhi::TextureBinding{whiteArray_, linearSampler_},
                                rhi::TextureBinding{shadowMap_, shadowSampler_},
                                rhi::TextureBinding{clusterGrid_, pointSampler_},
                                rhi::TextureBinding{lightIndices_, pointSampler_},
                                rhi::TextureBinding{lightData_, pointSampler_},
                            };
                            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, compact);
                        }
                        const std::array<rhi::BufferHandle, 1> rows{layers != nullptr && layers->layerBuffer.valid()
                                                                        ? layers->layerBuffer
                                                                        : terrainPlainLayers_};
                        cmd.bindStorageBuffers(rhi::ShaderStage::Fragment, 0, rows);
                    }
                    else {
                        if (settings_.debugView == DebugView::None && !settings_.leanTerrain()) {
                            cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&block, sizeof(block)));
                        }
                        else {
                            // A debug view and the lean ground
                            // (ADR 0175) ride in the block's last row, on a copy:
                            // the terrain's own stays as built.
                            GpuTerrainSurfaceUniforms viewed = block;
                            viewed.debug[0] = static_cast<f32>(settings_.debugView);
                            viewed.debug[1] = settings_.leanTerrain() ? 1.0f : 0.0f;
                            cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&viewed, sizeof(viewed)));
                        }
                        const auto layerArray = [&](usize slot, rhi::TextureHandle fallback) {
                            return layers != nullptr && layers->ready
                                       ? rhi::TextureBinding{layers->arrays[slot], linearSampler_}
                                       : rhi::TextureBinding{fallback, linearSampler_};
                        };
                        const std::array<rhi::TextureBinding, 16> textures{
                            rhi::TextureBinding{whitePixel_, linearSampler_},
                            rhi::TextureBinding{flatNormalPixel_, linearSampler_},
                            rhi::TextureBinding{whitePixel_, linearSampler_},
                            rhi::TextureBinding{blackPixel_, linearSampler_},
                            rhi::TextureBinding{shadowMap_, shadowSampler_},
                            rhi::TextureBinding{environmentMap_, environmentSampler_},
                            rhi::TextureBinding{brdfLut_, environmentSampler_},
                            rhi::TextureBinding{clusterGrid_, pointSampler_},
                            rhi::TextureBinding{lightIndices_, pointSampler_},
                            rhi::TextureBinding{lightData_, pointSampler_},
                            rhi::TextureBinding{occlusion_, linearSampler_},
                            rhi::TextureBinding{localShadowMap_, shadowSampler_},
                            rhi::TextureBinding{contact_, pointSampler_},
                            layerArray(0, whiteArray_),
                            layerArray(1, flatNormalArray_),
                            layerArray(2, whiteArray_),
                        };
                        cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
                        const std::array<rhi::BufferHandle, 1> layerRows{
                            layers != nullptr && layers->layerBuffer.valid() ? layers->layerBuffer
                                                                             : terrainPlainLayers_};
                        cmd.bindStorageBuffers(rhi::ShaderStage::Fragment, 0, layerRows);
                    }
                    boundMaterial = kTerrainBinding;
                    boundTerrain = draw.terrainId;
                }
            }
            else if (draw.material != boundMaterial && draw.material < world.materials.size()) {
                const RenderMaterial& material = world.materials[draw.material];
                if (draw.material < materialError_.size() && materialError_[draw.material]) {
                    // A surface shader that does not compile: the built-in
                    // surface in a colour nobody picks, so it is found.
                    GpuMaterialUniforms error = material.uniforms;
                    const f32 magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                    std::memcpy(error.baseColor, magenta, sizeof(magenta));
                    const f32 glow[4] = {0.6f, 0.0f, 0.6f, 0.0f};
                    std::memcpy(error.emissive, glow, sizeof(glow));
                    std::memset(error.textureFlags, 0, sizeof(error.textureFlags));
                    cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&error, sizeof(error)));
                }
                else {
                    cmd.bindUniforms(rhi::ShaderStage::Fragment, 1,
                                     asBytes(&material.uniforms, sizeof(material.uniforms)));
                }

                // Every slot is bound every time, with the shadow map last.
                // A slot left over from the previous material is the classic
                // way one mesh ends up wearing another's texture.
                const auto orDefault = [](rhi::TextureHandle handle, rhi::TextureHandle fallback) {
                    return handle.valid() ? handle : fallback;
                };
                const std::array<rhi::TextureBinding, 15> textures{
                    rhi::TextureBinding{orDefault(material.baseColor, whitePixel_), linearSampler_},
                    rhi::TextureBinding{orDefault(material.normal, flatNormalPixel_), linearSampler_},
                    rhi::TextureBinding{orDefault(material.metallicRoughness, whitePixel_), linearSampler_},
                    rhi::TextureBinding{orDefault(material.emissive, blackPixel_), linearSampler_},
                    rhi::TextureBinding{shadowMap_, shadowSampler_},
                    rhi::TextureBinding{environmentMap_, environmentSampler_},
                    rhi::TextureBinding{brdfLut_, environmentSampler_},
                    rhi::TextureBinding{clusterGrid_, pointSampler_},
                    rhi::TextureBinding{lightIndices_, pointSampler_},
                    rhi::TextureBinding{lightData_, pointSampler_},
                    rhi::TextureBinding{occlusion_, linearSampler_},
                    rhi::TextureBinding{localShadowMap_, shadowSampler_},
                    rhi::TextureBinding{contact_, pointSampler_},
                    // What the screen says (ADR 0209), or nothing while it
                    // is off -- and no reflection for a blended draw: what is
                    // at its pixel is the landing of the surface behind it.
                    screenReflectionBinding(selection == Selection::Transparent),
                    screenBounceBinding(),
                };
                cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
                boundMaterial = draw.material;
            }
            // Every draw rather than on a change of material: vertex slot 1 is
            // also where a skinned draw's joints and the terrain's block go.
            if (surfacePipeline.valid()) {
                bindSurface(cmd, draw.material, true, selection == Selection::Transparent);
                // Its textures took the built-in maps' slots: whatever draws
                // next with this material binds them again.
                boundMaterial = 0xFFFFFFFFu;
            }
        }

        if (skinnedRun) {
            const std::array<rhi::BufferHandle, 3> vertexBuffers{resolved->vertices, resolved->skin,
                                                                 skinnedInstanceBuffer_};
            cmd.bindVertexBuffers(0, vertexBuffers);
            const std::array<rhi::BufferHandle, 1> palettes{paletteBuffer_};
            cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, palettes);
        }
        else if (bakedRun) {
            const std::array<rhi::BufferHandle, 4> vertexBuffers{resolved->vertices, instanceBuffer_,
                                                                 bakedInstanceBuffer_, resolved->lightmap};
            cmd.bindVertexBuffers(0, vertexBuffers);
            const std::array<rhi::BufferHandle, 1> rows{lightmapRects_};
            cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, rows);
            const std::array<rhi::TextureBinding, 1> pages{rhi::TextureBinding{lightmapPages_, environmentSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 15, pages);
        }
        else if (batch != nullptr) {
            const std::array<rhi::BufferHandle, 2> vertexBuffers{resolved->vertices, instanceBuffer_};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        else if (skinnedDraw) {
            // **Where its palette begins in the frame's**, which is uploaded
            // once: sixteen bytes a draw, whatever the skeleton's size.
            bindSkinPalette(cmd, world, draw, morphDraw);

            const std::array<rhi::BufferHandle, 2> vertexBuffers{resolved->vertices, resolved->skin};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        else {
            const std::array<rhi::BufferHandle, 1> vertexBuffers{resolved->vertices};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        if (morphDraw) {
            // The table at the vertex stage's first storage slot, and the
            // draw's targets in the block after the ones it already has: the
            // object's, and a skinned draw's palette. A cloth's table is the
            // frame's rows, and its one target the run that is its own.
            const std::array<rhi::BufferHandle, 2> table{clothDraw ? clothBuffer_ : resolved->morph, paletteBuffer_};
            cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0,
                                   std::span(table).first(skinnedDraw && paletteBuffer_.valid() ? 2u : 1u));
            GpuMorphUniforms block;
            if (clothDraw) {
                const ClothDraw& rows = world.cloths[draw.cloth];
                block.range[0] = 0;
                block.range[1] = rows.vertexCount;
                block.range[2] = 1;
                block.rows[0] = rows.firstRow;
                block.weights[0] = 1.0f;
            }
            else {
                block = morphUniforms(world.morphs[draw.morph], resolved->morphFirstVertex, resolved->morphVertexCount,
                                      resolved->morphTargetCount);
            }
            cmd.bindUniforms(rhi::ShaderStage::Vertex, skinnedDraw ? 2 : 1, asBytes(&block, sizeof(block)));
            // That slot is the block world's too, bound once a run of chunks:
            // whatever chunk is drawn next binds its own again.
            if (boundMaterial == kVoxelBinding)
                boundMaterial = 0xFFFFFFFFu;
        }
        cmd.bindIndexBuffer(resolved->indices, rhi::IndexType::U32);
        if (batch == nullptr) {
            cmd.drawIndexed(section.indexCount, 1, resolved->firstIndex + section.firstIndex, resolved->vertexOffset,
                            0);
            const bool lit = selection == Selection::Opaque || selection == Selection::Transparent;
            countDraw(morphDraw && lit ? DrawKind::Morph : meshDrawKind(selection, false));
        }
        else {
            for (u32 piece = 0; piece < pieceCount; ++piece) {
                cmd.drawIndexed(section.indexCount, pieces[piece].count, resolved->firstIndex + section.firstIndex,
                                resolved->vertexOffset, batch->firstInstance + pieces[piece].first);
                countDraw(meshDrawKind(selection, true));
                ++stats_.instancedDraws;
                stats_.instances += pieces[piece].count;
            }
        }
    }
}

// A frame's world UI stops here: ten thousand quads, far past any screen of
// name tags, and a fixed buffer rather than one that grows under a frame.
constexpr u32 MaxWorldUiVertices = 60000;

// A frame's sprites stop here (the 2D layer): four megabytes of instances, and
// a screen of 16-pixel tiles at 4K is a little over thirty thousand of them.
constexpr u32 MaxSprites = 65536;

void DefaultRenderer::drawWorldUi(rhi::ICmdList& cmd, const RenderWorld& world, WorldUiRuns runs)
{
    GpuWorldUiView view;
    view.viewProjection = world.camera.viewProjection;
    const std::array<rhi::BufferHandle, 1> uiBuffers{worldUiBuffer_};
    for (const bool onTop : {false, true}) {
        // The shapes drawn on parts (ADR 0251) are drawn as the display shows
        // them: after the interface of their kind, in the scene or over it.
        if (worldUiVertexCount_ == 0) {
            if (runs != WorldUiRuns::AsLight)
                drawAdornments(cmd, world, onTop);
            continue;
        }
        cmd.setPipeline(onTop ? worldUiOnTopPipeline_ : worldUiPipeline_);
        cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&view, sizeof(view)));
        cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&forwardFrame_, sizeof(forwardFrame_)));
        cmd.bindVertexBuffers(0, uiBuffers);
        for (const WorldUiRun& run : world.worldUiRuns) {
            if (run.alwaysOnTop != onTop || run.vertexCount == 0 ||
                run.firstVertex + run.vertexCount > worldUiVertexCount_)
                continue;
            const bool displayed = shownAsDisplayed(run);
            if ((runs == WorldUiRuns::AsDisplayed && !displayed) || (runs == WorldUiRuns::AsLight && displayed))
                continue;
            GpuWorldUiLook look;
            look.params[0] = run.brightness;
            look.params[1] = foldForShaders();
            look.params[2] = displayed ? 1.0f : 0.0f;
            look.params[3] = std::exp2(world.environment.exposureCompensation);
            look.light[0] = run.lightInfluence;
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&look, sizeof(look)));
            // The run's picture and the frame's gradient table (ADR 0110),
            // which the screen's UI reads too; then what lights it as a
            // surface (`LightInfluence`) -- the exposure the frame before was
            // drawn at, and the forward's, the light the screen bounces in a
            // slot a material's map takes.
            const std::array<rhi::TextureBinding, 13> texture{
                rhi::TextureBinding{run.texture.valid() ? run.texture : whitePixel_, environmentSampler_},
                rhi::TextureBinding{world.worldUiGradients.valid() ? world.worldUiGradients : whitePixel_,
                                    environmentSampler_},
                rhi::TextureBinding{exposure_[exposureIndex_], pointSampler_},
                screenBounceBinding(),
                rhi::TextureBinding{shadowMap_, shadowSampler_},
                rhi::TextureBinding{environmentMap_, environmentSampler_},
                rhi::TextureBinding{brdfLut_, environmentSampler_},
                rhi::TextureBinding{clusterGrid_, pointSampler_},
                rhi::TextureBinding{lightIndices_, pointSampler_},
                rhi::TextureBinding{lightData_, pointSampler_},
                rhi::TextureBinding{occlusion_, linearSampler_},
                rhi::TextureBinding{localShadowMap_, shadowSampler_},
                rhi::TextureBinding{contact_, pointSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, texture);
            cmd.draw(run.vertexCount, 1, run.firstVertex, 0);
            countDraw(DrawKind::WorldUi);
        }
        if (runs != WorldUiRuns::AsLight)
            drawAdornments(cmd, world, onTop);
    }
}

void DefaultRenderer::drawAdornments(rhi::ICmdList& cmd, const RenderWorld& world, bool onTop)
{
    if (adornmentCount_ == 0)
        return;
    bool bound = false;
    for (const AdornmentRun& run : adornmentRuns_) {
        if (run.onTop != onTop || run.count == 0)
            continue;
        if (!bound) {
            cmd.setPipeline(onTop ? adornmentOnTopPipeline_ : adornmentPipeline_);
            GpuWorldUiView view;
            view.viewProjection = world.camera.viewProjection;
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&view, sizeof(view)));
            const std::array<rhi::BufferHandle, 2> streams{adornmentVertices_, adornmentInstances_};
            cmd.bindVertexBuffers(0, streams);
            cmd.bindIndexBuffer(adornmentIndices_, rhi::IndexType::U16);
            bound = true;
        }
        const std::array<rhi::TextureBinding, 1> picture{
            rhi::TextureBinding{run.texture.valid() ? run.texture : whitePixel_, linearSampler_}};
        cmd.bindTextures(rhi::ShaderStage::Fragment, 0, picture);
        const AdornmentShape& shape = adornmentShapes_[static_cast<usize>(run.mesh)];
        cmd.drawIndexed(shape.indexCount, run.count, shape.firstIndex, 0, run.first);
        countDraw(DrawKind::WorldUi);
    }
}

namespace {

// How many shapes drawn on parts a frame holds (ADR 0251): beyond it, the
// nearest are drawn.
constexpr u32 MaxAdornments = 8192;

} // namespace

bool DefaultRenderer::ensureAdornments(rhi::IDevice& device)
{
    if (adornmentsTried_)
        return adornmentPipeline_.valid() && adornmentOnTopPipeline_.valid() && adornmentVertices_.valid() &&
               adornmentIndices_.valid() && adornmentInstances_.valid();
    adornmentsTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "adornment", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "adornment", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // **The unit meshes**, positions alone: a box, a sphere, a cylinder and a
    // cone at 24 segments, and a square.
    adornmentCorners_.clear();
    adornmentCornerIndices_.clear();
    constexpr u32 Segments = 24;
    constexpr u32 Rings = 12;
    constexpr f32 Pi = 3.14159265358979f;
    const auto begin = [&](AdornmentMesh mesh) {
        adornmentShapes_[static_cast<usize>(mesh)].firstIndex = static_cast<u32>(adornmentCornerIndices_.size());
    };
    const auto end = [&](AdornmentMesh mesh) {
        AdornmentShape& shape = adornmentShapes_[static_cast<usize>(mesh)];
        shape.indexCount = static_cast<u32>(adornmentCornerIndices_.size()) - shape.firstIndex;
    };
    const auto corner = [&](f32 x, f32 y, f32 z) {
        adornmentCorners_.push_back(core::Vec3{x, y, z});
        return static_cast<core::u16>(adornmentCorners_.size() - 1);
    };
    const auto triangle = [&](core::u16 a, core::u16 b, core::u16 c) {
        adornmentCornerIndices_.insert(adornmentCornerIndices_.end(), {a, b, c});
    };
    begin(AdornmentMesh::Box);
    {
        std::array<core::u16, 8> at{};
        for (u32 index = 0; index < 8; ++index)
            at[index] = corner(index & 1 ? 0.5f : -0.5f, index & 2 ? 0.5f : -0.5f, index & 4 ? 0.5f : -0.5f);
        constexpr std::array<std::array<u32, 4>, 6> faces{
            {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}}};
        for (const auto& face : faces) {
            triangle(at[face[0]], at[face[1]], at[face[2]]);
            triangle(at[face[0]], at[face[2]], at[face[3]]);
        }
    }
    end(AdornmentMesh::Box);
    begin(AdornmentMesh::Sphere);
    {
        const core::u16 first = static_cast<core::u16>(adornmentCorners_.size());
        for (u32 ring = 0; ring <= Rings; ++ring) {
            const f32 phi = Pi * static_cast<f32>(ring) / static_cast<f32>(Rings);
            for (u32 segment = 0; segment <= Segments; ++segment) {
                const f32 theta = 2.0f * Pi * static_cast<f32>(segment) / static_cast<f32>(Segments);
                (void)corner(0.5f * std::sin(phi) * std::cos(theta), 0.5f * std::cos(phi),
                             0.5f * std::sin(phi) * std::sin(theta));
            }
        }
        for (u32 ring = 0; ring < Rings; ++ring) {
            for (u32 segment = 0; segment < Segments; ++segment) {
                const auto a = static_cast<core::u16>(first + ring * (Segments + 1) + segment);
                const auto b = static_cast<core::u16>(a + Segments + 1);
                triangle(a, b, static_cast<core::u16>(a + 1));
                triangle(static_cast<core::u16>(a + 1), b, static_cast<core::u16>(b + 1));
            }
        }
    }
    end(AdornmentMesh::Sphere);
    const auto revolved = [&](f32 nearZ, f32 farZ, f32 farRadius) {
        const core::u16 nearMiddle = corner(0.0f, 0.0f, nearZ);
        const core::u16 farMiddle = corner(0.0f, 0.0f, farZ);
        const core::u16 first = static_cast<core::u16>(adornmentCorners_.size());
        for (u32 segment = 0; segment < Segments; ++segment) {
            const f32 theta = 2.0f * Pi * static_cast<f32>(segment) / static_cast<f32>(Segments);
            (void)corner(0.5f * std::cos(theta), 0.5f * std::sin(theta), nearZ);
            (void)corner(farRadius * std::cos(theta), farRadius * std::sin(theta), farZ);
        }
        for (u32 segment = 0; segment < Segments; ++segment) {
            const auto a = static_cast<core::u16>(first + 2 * segment);
            const auto b = static_cast<core::u16>(first + 2 * ((segment + 1) % Segments));
            triangle(a, b, static_cast<core::u16>(a + 1));
            triangle(static_cast<core::u16>(a + 1), b, static_cast<core::u16>(b + 1));
            triangle(nearMiddle, b, a);
            if (farRadius > 0.0f)
                triangle(farMiddle, static_cast<core::u16>(a + 1), static_cast<core::u16>(b + 1));
        }
    };
    begin(AdornmentMesh::Cylinder);
    revolved(-0.5f, 0.5f, 0.5f);
    end(AdornmentMesh::Cylinder);
    begin(AdornmentMesh::Cone);
    revolved(0.0f, -1.0f, 0.0f);
    end(AdornmentMesh::Cone);
    begin(AdornmentMesh::Quad);
    {
        const core::u16 a = corner(-0.5f, -0.5f, 0.0f);
        const core::u16 b = corner(0.5f, -0.5f, 0.0f);
        const core::u16 c = corner(0.5f, 0.5f, 0.0f);
        const core::u16 d = corner(-0.5f, 0.5f, 0.0f);
        triangle(a, b, c);
        triangle(a, c, d);
    }
    end(AdornmentMesh::Quad);

    adornmentVertices_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(adornmentCorners_.size() * sizeof(core::Vec3)),
        .debugName = "adornment-meshes",
    });
    adornmentIndices_ = device.createBuffer({
        .usage = rhi::BufferUsage::Index,
        .sizeBytes = static_cast<u32>(adornmentCornerIndices_.size() * sizeof(core::u16)),
        .debugName = "adornment-indices",
    });
    adornmentInstances_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(MaxAdornments * sizeof(GpuAdornment)),
        .debugName = "adornment-instances",
    });

    const std::array<rhi::VertexAttribute, 8> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 64},
        rhi::VertexAttribute{.location = 6, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 80},
        rhi::VertexAttribute{.location = 7, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 96},
    };
    const std::array<rhi::VertexBufferLayout, 2> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(core::Vec3)},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = sizeof(GpuAdornment), .perInstance = true},
    };
    // Premultiplied, as the world's interface blends: into the 16-bit
    // target, the interface's layer or a headset's image alike.
    const std::array<rhi::ColorTargetDesc, 1> target{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true, .srcColor = rhi::BlendFactor::One},
    }};
    const auto make = [&](bool tested, const char* name) {
        return device.createGraphicsPipeline({
            .vertexShader = vertex,
            .fragmentShader = fragment,
            .vertexBuffers = buffers,
            .vertexAttributes = attributes,
            // Both sides: an image is seen from behind too, and a shape the
            // camera is inside of still shows its far side.
            .rasterizer = {.cullMode = rhi::CullMode::None},
            .depthStencil = {.depthTest = tested, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = target,
            .depthStencilFormat = kDepthFormat,
            .debugName = name,
        });
    };
    adornmentPipeline_ = make(true, "adornment");
    adornmentOnTopPipeline_ = make(false, "adornment.top");
    return adornmentPipeline_.valid() && adornmentOnTopPipeline_.valid() && adornmentVertices_.valid() &&
           adornmentIndices_.valid() && adornmentInstances_.valid();
}

bool DefaultRenderer::ensureUiLayer(rhi::IDevice& device, u32 samples)
{
    if (uiLayer_.valid() && uiLayerWidth_ == renderWidth_ && uiLayerHeight_ == renderHeight_ &&
        uiLayerSampleCount_ == samples && (samples <= 1 || uiLayerSamples_.valid()))
        return true;
    for (rhi::TextureHandle* texture : {&uiLayer_, &uiLayerSamples_}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    // 8-bit sRGB, so a blend into it is in linear light and what it holds is
    // what the display shows, as a headset's image is.
    uiLayer_ = device.createTexture({
        .format = rhi::TextureFormat::Rgba8UnormSrgb,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = renderWidth_,
        .height = renderHeight_,
        .debugName = "world-ui-layer",
    });
    if (samples > 1)
        uiLayerSamples_ = device.createTexture({
            .format = rhi::TextureFormat::Rgba8UnormSrgb,
            .usage = rhi::TextureUsage::ColorTarget,
            .width = renderWidth_,
            .height = renderHeight_,
            .samples = samples,
            .debugName = "world-ui-layer-samples",
        });
    uiLayerWidth_ = renderWidth_;
    uiLayerHeight_ = renderHeight_;
    uiLayerSampleCount_ = samples;
    return uiLayer_.valid() && (samples <= 1 || uiLayerSamples_.valid());
}

bool DefaultRenderer::ensureWorldUi(rhi::IDevice& device)
{
    if (worldUiTried_)
        return worldUiPipeline_.valid() && worldUiOnTopPipeline_.valid() && worldUiBuffer_.valid();
    worldUiTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "ui_world", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "ui_world", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    const std::array<rhi::VertexAttribute, 9> attributes{
        rhi::VertexAttribute{.location = 0,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float3,
                             .offsetBytes = offsetof(WorldUiVertex, x)},
        rhi::VertexAttribute{.location = 1,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Ubyte4Unorm,
                             .offsetBytes = offsetof(WorldUiVertex, r)},
        rhi::VertexAttribute{.location = 2,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float4,
                             .offsetBytes = offsetof(WorldUiVertex, localX)},
        rhi::VertexAttribute{.location = 3,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float1,
                             .offsetBytes = offsetof(WorldUiVertex, radius)},
        rhi::VertexAttribute{.location = 4,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float2,
                             .offsetBytes = offsetof(WorldUiVertex, u)},
        // The gradient and the stroke (ADR 0110), as the screen's UI reads them.
        rhi::VertexAttribute{.location = 5,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float4,
                             .offsetBytes = offsetof(WorldUiVertex, look) + offsetof(UiVertexAppearance, gradientX)},
        rhi::VertexAttribute{.location = 6,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float4,
                             .offsetBytes = offsetof(WorldUiVertex, look) + offsetof(UiVertexAppearance, gradientRow)},
        rhi::VertexAttribute{.location = 7,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float4,
                             .offsetBytes =
                                 offsetof(WorldUiVertex, look) + offsetof(UiVertexAppearance, gradientOffsetX)},
        rhi::VertexAttribute{.location = 8,
                             .bufferSlot = 0,
                             .format = rhi::VertexFormat::Float1,
                             .offsetBytes = offsetof(WorldUiVertex, look) + offsetof(UiVertexAppearance, strokeJoin)},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(WorldUiVertex)},
    };
    // Premultiplied by the shader, from a straight alpha upstream as the
    // screen's UI blends: the same blend into the 16-bit target, into the
    // interface's own layer and into a headset's image (ADR 0250).
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true, .srcColor = rhi::BlendFactor::One},
    }};
    const auto make = [&](bool tested, const char* name) {
        return device.createGraphicsPipeline({
            .vertexShader = vertex,
            .fragmentShader = fragment,
            .vertexBuffers = buffers,
            .vertexAttributes = attributes,
            // Both sides: a sign is read from in front, and seen edge-on or
            // from behind it is a sign seen from behind.
            .rasterizer = {.cullMode = rhi::CullMode::None},
            // Tested and never written, like every blended surface: hidden by
            // what is in front, and never hiding what is drawn after it.
            .depthStencil = {.depthTest = tested, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = name,
        });
    };
    worldUiPipeline_ = make(true, "ui_world");
    worldUiOnTopPipeline_ = make(false, "ui_world.top");
    worldUiBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(MaxWorldUiVertices * sizeof(WorldUiVertex)),
        .debugName = "ui_world",
    });
    return worldUiPipeline_.valid() && worldUiOnTopPipeline_.valid() && worldUiBuffer_.valid();
}

bool DefaultRenderer::ensureDecals(rhi::IDevice& device)
{
    if (decalTried_)
        return decalPipeline_.valid();
    decalTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "decal", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "decal", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    // **Multiplied into what is there**: the colour this writes is a factor,
    // one where nothing lands. Alpha is left as it was.
    const std::array<rhi::ColorTargetDesc, 1> multiplyTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::DstColor,
                  .dstColor = rhi::BlendFactor::Zero,
                  .srcAlpha = rhi::BlendFactor::Zero,
                  .dstAlpha = rhi::BlendFactor::One},
    }};
    decalPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        // Both sides reach the fragment stage, which keeps the far one: see
        // the shader. No depth attachment -- the pass reads depth instead.
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = multiplyTarget,
        .debugName = "decal",
    });
    // **Laid over what is there, and added to it** (ADR 0160): the same
    // shader, premultiplied for the one and with nothing taken away for the
    // other. Alpha is left as it was in both.
    const std::array<rhi::ColorTargetDesc, 1> alphaTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .srcAlpha = rhi::BlendFactor::Zero,
                  .dstAlpha = rhi::BlendFactor::One},
    }};
    const std::array<rhi::ColorTargetDesc, 1> addTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::One,
                  .srcAlpha = rhi::BlendFactor::Zero,
                  .dstAlpha = rhi::BlendFactor::One},
    }};
    decalAlphaPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = alphaTarget,
        .debugName = "decal-alpha",
    });
    decalAddPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = addTarget,
        .debugName = "decal-additive",
    });
    return decalPipeline_.valid();
}

bool DefaultRenderer::ensureLookPipeline(rhi::IDevice& device, LookPipeline& slot, const char* shader,
                                         rhi::TextureFormat format, LookBlend blend)
{
    ENG_PROFILE_SCOPE("prepare.look_pipeline");
    if (slot.tried)
        return slot.handle.valid();
    slot.tried = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, shader, rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, shader, rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    rhi::ColorTargetDesc target{.format = format};
    if (blend == LookBlend::Add) {
        target.blend = {.enabled = true,
                        .srcColor = rhi::BlendFactor::One,
                        .dstColor = rhi::BlendFactor::One,
                        .srcAlpha = rhi::BlendFactor::Zero,
                        .dstAlpha = rhi::BlendFactor::One};
    }
    else if (blend == LookBlend::Air) {
        target.blend = {.enabled = true,
                        .srcColor = rhi::BlendFactor::One,
                        .dstColor = rhi::BlendFactor::SrcAlpha,
                        .srcAlpha = rhi::BlendFactor::Zero,
                        .dstAlpha = rhi::BlendFactor::One};
    }
    const std::array<rhi::ColorTargetDesc, 1> targets{target};
    slot.handle = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = targets,
        .debugName = shader,
    });
    // **Said once, by name**: a pass made the first frame it is drawn fails
    // where nobody is looking, and the frame simply goes without it.
    if (!slot.handle.valid()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{shader}}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.pipeline_failed"), args);
    }
    return slot.handle.valid();
}

bool DefaultRenderer::lookTexture(rhi::IDevice& device, rhi::TextureHandle& slot, u32 width, u32 height,
                                  const char* name)
{
    if (slot.valid())
        return true;
    slot = device.createTexture({
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = name,
    });
    return slot.valid();
}

// --- Anti-aliasing and upscaling (ADR 0158) ------------------------------------

namespace {

// The radical inverse of `index` in `base`: Halton's sequence, which fills a
// pixel evenly in eight samples where a random one clumps.
[[nodiscard]] f32 halton(u32 index, u32 base) noexcept
{
    f32 fraction = 1.0f;
    f32 result = 0.0f;
    while (index > 0) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

constexpr u32 kJitterSamples = 8;

// **How long the jitter's sequence is under FSR 2** (ADR 0164): eight samples
// for each of the output's pixels a rendered one covers, which is AMD's rule
// (`ffxFsr2GetJitterPhaseCount`) -- an output pixel sees every part of itself
// rendered as often at any scale.
[[nodiscard]] u32 fsr2JitterPhases(u32 renderWidth, u32 outputWidth) noexcept
{
    const f32 ratio = renderWidth > 0 ? static_cast<f32>(outputWidth) / static_cast<f32>(renderWidth) : 1.0f;
    const auto phases = static_cast<u32>(static_cast<f32>(kJitterSamples) * ratio * ratio);
    return phases > kJitterSamples ? phases : kJitterSamples;
}

// A float as the word a constant buffer carries it in.
[[nodiscard]] u32 bitsOf(f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

} // namespace

bool DefaultRenderer::temporalFrame(const RenderWorld& world) const noexcept
{
    // **Not a picture with nothing behind it**, whose alpha a blend would
    // lose; **nor a picture of sprites alone** (`spritesOnly`), which SMAA
    // smooths instead -- a 2D game is not jittered at all. A sprite drawn in
    // its own colours among 3D surfaces is neither jittered nor blended: it
    // is drawn where the camera is without its jitter, and the resolve passes
    // its pixels through (ADR 0158).
    //
    // **And a frame FSR 2 upscales is one** (ADR 0164), whatever the
    // anti-aliasing asked for: it is jittered and measured the same way, and
    // the upscaler is the pass that takes the jitter out.
    return (settings_.antiAliasing == AntiAliasingMode::Taa || fsr2Frame(world)) && world.camera.valid &&
           !world.environment.transparentBackground && !spritesOnly(world);
}

bool DefaultRenderer::fsr2Frame(const RenderWorld& world) const noexcept
{
    return settings_.upscaling == UpscalingMode::Fsr2 && !fsr2Failed_ && world.camera.valid &&
           !core::isOrthographic(world.camera.projection);
}

core::Vec2 DefaultRenderer::cameraJitter(const RenderWorld& world, const RenderTarget& target) const
{
    const u32 targetWidth = target.width;
    const u32 targetHeight = target.height;
    // The main view and the parts of the window (ADR 0205): a picture shown
    // through the interface keeps no history.
    const bool ofWindow = target.view == 0 || target.window;
    if (!ofWindow || !temporalFrame(world) || targetWidth == 0 || targetHeight == 0)
        return {};
    // The render size, as `render` will make it.
    const f32 scale = worldRenderScale(world, targetWidth, targetHeight, true);
    const auto scaled = [scale](u32 value) {
        const auto result = static_cast<u32>(static_cast<f32>(value) * scale + 0.5f);
        return result > 0 ? result : 1u;
    };
    // FSR 2 upscales the main view alone, and its phases are its own.
    const u32 phases =
        target.view == 0 && fsr2Frame(world) ? fsr2JitterPhases(scaled(targetWidth), targetWidth) : kJitterSamples;
    // The view's own count, wherever the view is kept between frames.
    u32 index = jitterIndex_;
    if (target.view != activeView_) {
        const auto parked = parkedViews_.find(target.view);
        index = parked == parkedViews_.end() ? 0u : parked->second.jitterIndex_;
    }
    const u32 sample = index % phases + 1;
    const f32 x = halton(sample, 2) - 0.5f;
    const f32 y = halton(sample, 3) - 0.5f;
    return core::Vec2{2.0f * x / static_cast<f32>(scaled(targetWidth)),
                      2.0f * y / static_cast<f32>(scaled(targetHeight))};
}

bool DefaultRenderer::aaTexture(rhi::IDevice& device, rhi::TextureHandle& slot, u32 width, u32 height,
                                rhi::TextureFormat format, const char* name)
{
    if (slot.valid())
        return true;
    slot = device.createTexture({
        .format = format,
        .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
        .width = width,
        .height = height,
        .debugName = name,
    });
    return slot.valid();
}

void DefaultRenderer::releaseAaTextures(rhi::IDevice& device)
{
    for (rhi::TextureHandle* texture :
         {&smaaEdges_, &smaaWeights_, &aaResolved_, &upscaled_, &velocity_, &history_[0], &history_[1]}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    historyValid_ = false;
    aaWidth_ = 0;
    aaHeight_ = 0;
    upscaledWidth_ = 0;
    upscaledHeight_ = 0;
}

bool DefaultRenderer::ensureSmaaTables(rhi::IDevice& device, rhi::ICmdList& cmd)
{
    if (smaaTablesUploaded_)
        return smaaArea_.valid() && smaaSearch_.valid();
    smaaTablesUploaded_ = true;
    smaaArea_ = device.createTexture({
        .format = rhi::TextureFormat::Rg8Unorm,
        .usage = rhi::TextureUsage::Sampled,
        .width = kSmaaAreaWidth,
        .height = kSmaaAreaHeight,
        .debugName = "smaa-area",
    });
    smaaSearch_ = device.createTexture({
        .format = rhi::TextureFormat::R8Unorm,
        .usage = rhi::TextureUsage::Sampled,
        .width = kSmaaSearchWidth,
        .height = kSmaaSearchHeight,
        .debugName = "smaa-search",
    });
    if (!smaaArea_.valid() || !smaaSearch_.valid())
        return false;
    cmd.uploadTexture(smaaArea_, smaaAreaTable(), 0);
    cmd.uploadTexture(smaaSearch_, smaaSearchTable(), 0);
    return true;
}

bool DefaultRenderer::ensureMotionPipelines(rhi::IDevice& device)
{
    if (motionTried_)
        return motionPipeline_.valid() && motionSkinnedPipeline_.valid();
    motionTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const auto load = [&](const char* name, rhi::ShaderStage stage) {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("motion", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("motion", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle skinnedVertex = load("motion_skinned", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle skinnedFragment = load("motion_skinned", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid() || !skinnedVertex.valid() || !skinnedFragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    // The mesh streams as the shadow pass reads them: a position, and for
    // the skinned twin the joints and weights of the second stream.
    const std::array<rhi::VertexBufferLayout, 1> buffers{rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48}};
    const std::array<rhi::VertexAttribute, 1> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0}};
    const std::array<rhi::VertexBufferLayout, 2> skinnedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
    };
    const std::array<rhi::VertexAttribute, 3> skinnedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    // **Against the depth the prepass drew, never writing it**: a moving
    // thing hidden behind a still one keeps the still one's motion.
    const std::array<rhi::ColorTargetDesc, 1> target{rhi::ColorTargetDesc{.format = rhi::TextureFormat::Rg16Float}};
    const rhi::DepthStencilState depth{.depthTest = true, .depthWrite = false};
    motionPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = depth,
        .colorTargets = target,
        .depthStencilFormat = kDepthFormat,
        .debugName = "motion",
    });
    motionSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = skinnedVertex,
        .fragmentShader = skinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = depth,
        .colorTargets = target,
        .depthStencilFormat = kDepthFormat,
        .debugName = "motion_skinned",
    });
    for (const auto& [pipeline, name] :
         {std::pair{motionPipeline_, "motion"}, std::pair{motionSkinnedPipeline_, "motion_skinned"}}) {
        if (!pipeline.valid()) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("render.warn.pipeline_failed"), args);
        }
    }
    return motionPipeline_.valid() && motionSkinnedPipeline_.valid();
}

bool DefaultRenderer::ensureWarpMotionPipelines(rhi::IDevice& device, rhi::TextureFormat depthFormat)
{
    if (warpMotionDepthFormat_ == depthFormat)
        return warpMotionPipeline_.valid() && warpMotionSkinnedPipeline_.valid();
    for (rhi::PipelineHandle* pipeline :
         {&warpMotionPipeline_, &warpMotionSkinnedPipeline_, &warpMotionUiPipeline_, &warpMotionUiOnTopPipeline_}) {
        if (pipeline->valid())
            device.destroy(*pipeline);
        *pipeline = {};
    }
    warpMotionDepthFormat_ = depthFormat;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const auto load = [&](const char* name, rhi::ShaderStage stage) {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("warp_motion", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("warp_motion", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle skinnedVertex = load("warp_motion_skinned", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle skinnedFragment = load("warp_motion_skinned", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid() || !skinnedVertex.valid() || !skinnedFragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    const std::array<rhi::VertexBufferLayout, 1> buffers{rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48}};
    const std::array<rhi::VertexAttribute, 1> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0}};
    const std::array<rhi::VertexBufferLayout, 2> skinnedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
    };
    const std::array<rhi::VertexAttribute, 3> skinnedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    // **Its own depth, written**: the runtime reads the motion with the
    // depth beside it, and the nearer surface's motion is the one it needs.
    const std::array<rhi::ColorTargetDesc, 1> target{rhi::ColorTargetDesc{.format = rhi::TextureFormat::Rgba16Float}};
    const rhi::DepthStencilState depth{
        .depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual};
    warpMotionPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = depth,
        .colorTargets = target,
        .depthStencilFormat = depthFormat,
        .debugName = "warp_motion",
    });
    warpMotionSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = skinnedVertex,
        .fragmentShader = skinnedFragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = depth,
        .colorTargets = target,
        .depthStencilFormat = depthFormat,
        .debugName = "warp_motion_skinned",
    });
    // **The world's interface**: its vertices' positions, already in the
    // world, both faces, and -- over the scene -- with no test of the depth.
    const std::array<rhi::VertexBufferLayout, 1> uiBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(WorldUiVertex)}};
    for (const bool onTop : {false, true}) {
        (onTop ? warpMotionUiOnTopPipeline_ : warpMotionUiPipeline_) = device.createGraphicsPipeline({
            .vertexShader = vertex,
            .fragmentShader = fragment,
            .vertexBuffers = uiBuffers,
            .vertexAttributes = attributes,
            .rasterizer = {.cullMode = rhi::CullMode::None},
            .depthStencil = onTop ? rhi::DepthStencilState{.depthTest = false, .depthWrite = true} : depth,
            .colorTargets = target,
            .depthStencilFormat = depthFormat,
            .debugName = onTop ? "warp_motion_ui_on_top" : "warp_motion_ui",
        });
    }
    for (const auto& [pipeline, name] : {std::pair{warpMotionPipeline_, "warp_motion"},
                                         std::pair{warpMotionSkinnedPipeline_, "warp_motion_skinned"}}) {
        if (!pipeline.valid()) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("render.warn.pipeline_failed"), args);
        }
    }
    return warpMotionPipeline_.valid() && warpMotionSkinnedPipeline_.valid();
}

// **A headset eye's motion for its runtime** (ADR 0252): every opaque draw,
// with where it is and where it was, through this frame's view at both ends
// (`warp_motion.hlsl` says why), into the runtime's motion image; and its
// depth beside it. What does not move on its own is drawn as still. What
// blends, and what is cut out, is not drawn: the runtime moves it with what
// is behind it.
void DefaultRenderer::drawWarpMotion(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                     const MeshCache& meshes, const RenderTarget& target)
{
    if (!target.motionDepth.valid() || target.motionWidth == 0 || target.motionHeight == 0 ||
        !ensureWarpMotionPipelines(device, target.motionDepthFormat))
        return;
    const RenderCamera& camera = world.camera;
    const Mat4 view = camera.unjitteredViewProjection;
    const f32 pixelsPerUnit = camera.valid ? lodPixelsPerUnit(camera, height_) : 0.0f;
    warpPlacing_.clear();
    cmd.pushDebugGroup("space-warp");
    cmd.beginRenderPass({
        .colorAttachments = std::array<rhi::ColorAttachment, 1>{rhi::ColorAttachment{
            .texture = target.motion,
            .loadOp = rhi::LoadOp::Clear,
            .storeOp = rhi::StoreOp::Store,
            .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
        }},
        .depthStencil = {.texture = target.motionDepth, .loadOp = rhi::LoadOp::Clear, .storeOp = rhi::StoreOp::Store},
        .debugName = "warp-motion",
    });
    cmd.setViewport({.width = static_cast<f32>(target.motionWidth), .height = static_cast<f32>(target.motionHeight)});
    cmd.setScissor(
        {.width = static_cast<core::i32>(target.motionWidth), .height = static_cast<core::i32>(target.motionHeight)});
    rhi::PipelineHandle bound{};
    for (const DrawItem& draw : world.draws) {
        if (draw.transparent || draw.cutout || !draw.inCameraFrustum)
            continue;
        const MeshCache::Resolved* resolved = meshes.resolve(draw.mesh);
        if (resolved == nullptr || resolved->lods.empty())
            continue;
        const u32 lod =
            selectMeshLod(*resolved, draw.transform, pixelsPerUnit, settings_.lodBias, settings_.maximumLodLevel);
        const MeshLodRange& level = resolved->lods[lod];
        if (draw.section >= level.sectionCount || level.firstSection + draw.section >= resolved->sections.size())
            continue;
        const MeshSection& section = resolved->sections[level.firstSection + draw.section];
        if (section.indexCount == 0)
            continue;
        // Where it was, in this frame's camera space: still, where it has no
        // place a frame ago.
        Mat4 before = draw.transform;
        if (draw.motionKey != 0) {
            warpPlacing_[draw.motionKey] = Placed{draw.transform, camera.origin};
            if (const auto found = warpPlaced_.find(draw.motionKey); found != warpPlaced_.end()) {
                const core::DVec3 shift = found->second.origin - camera.origin;
                before = core::translation(
                             Vec3{static_cast<f32>(shift.x), static_cast<f32>(shift.y), static_cast<f32>(shift.z)}) *
                         found->second.transform;
            }
        }
        const bool skinned = draw.boneCount > 0 && resolved->skin.valid();
        const rhi::PipelineHandle pipeline = skinned ? warpMotionSkinnedPipeline_ : warpMotionPipeline_;
        if (!(pipeline == bound)) {
            cmd.setPipeline(pipeline);
            bound = pipeline;
        }
        GpuMotionUniforms motion;
        motion.viewProjection = view;
        motion.model = draw.transform;
        motion.current = view * draw.transform;
        motion.previous = view * before;
        cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&motion, sizeof(motion)));
        if (skinned) {
            bindSkinPalette(cmd, world, draw, false);
            const std::array<rhi::BufferHandle, 2> vertexBuffers{resolved->vertices, resolved->skin};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        else {
            const std::array<rhi::BufferHandle, 1> vertexBuffers{resolved->vertices};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        cmd.bindIndexBuffer(resolved->indices, rhi::IndexType::U32);
        cmd.drawIndexed(section.indexCount, 1, resolved->firstIndex + section.firstIndex, resolved->vertexOffset, 0);
        countDraw(DrawKind::Velocity);
    }
    // **The world's interface, solid** (ADR 0252 §4): blended as it is in
    // the eye, the runtime would move it with what is behind it, and smear
    // the text the player reads -- a panel following the head the worst of
    // all. Each canvas's quads, moved by where its canvas was.
    warpUiPlacing_.clear();
    if (worldUiVertexCount_ > 0 && worldUiBuffer_.valid() && warpMotionUiPipeline_.valid() &&
        warpMotionUiOnTopPipeline_.valid()) {
        const std::array<rhi::BufferHandle, 1> uiBuffers{worldUiBuffer_};
        cmd.bindVertexBuffers(0, uiBuffers);
        for (const bool onTop : {false, true}) {
            cmd.setPipeline(onTop ? warpMotionUiOnTopPipeline_ : warpMotionUiPipeline_);
            for (const WorldUiRun& run : world.worldUiRuns) {
                if (run.vertexCount == 0 || run.alwaysOnTop != onTop ||
                    run.firstVertex + run.vertexCount > worldUiVertexCount_)
                    continue;
                Mat4 before = Mat4{};
                if (run.key != 0) {
                    warpUiPlacing_[run.key] = Placed{run.placement, camera.origin};
                    if (const auto found = warpUiPlaced_.find(run.key); found != warpUiPlaced_.end()) {
                        const core::DVec3 shift = found->second.origin - camera.origin;
                        before = core::translation(Vec3{static_cast<f32>(shift.x), static_cast<f32>(shift.y),
                                                        static_cast<f32>(shift.z)}) *
                                 found->second.transform * core::inverse(run.placement);
                    }
                }
                GpuMotionUniforms motion;
                motion.viewProjection = view;
                motion.model = Mat4{};
                motion.current = view;
                motion.previous = view * before;
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&motion, sizeof(motion)));
                cmd.draw(run.vertexCount, 1, run.firstVertex, 0);
            }
        }
    }
    cmd.endRenderPass();
    cmd.popDebugGroup();
    warpPlaced_.swap(warpPlacing_);
    warpUiPlaced_.swap(warpUiPlacing_);
}

void DefaultRenderer::writeVelocity(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                    const MeshCache& meshes)
{
    if (!aaTexture(device, velocity_, renderWidth_, renderHeight_, rhi::TextureFormat::Rg16Float, "velocity") ||
        !ensureLookPipeline(device, taaVelocity_, "taa_velocity", rhi::TextureFormat::Rg16Float))
        return;
    const RenderCamera& camera = world.camera;
    // A camera that moved further than anything walks in a frame was cut,
    // not moved: nothing of the last frame is this one's.
    if (previousCamera_) {
        const core::DVec3 moved = camera.origin - previousOrigin_;
        if (moved.x * moved.x + moved.y * moved.y + moved.z * moved.z > 100.0 * 100.0) {
            previousCamera_ = false;
            historyValid_ = false;
            placed_.clear();
        }
    }
    // What frame generation is told (ADR 0165): a frame with no camera before
    // it has nothing to be the frame between.
    motionCut_ = !previousCamera_;
    // The last camera, its space moved to this one's origin -- or, with none,
    // this one, which moves nothing.
    const Mat4 previous =
        previousCamera_
            ? previousViewProjection_ * core::translation(Vec3{static_cast<f32>(camera.origin.x - previousOrigin_.x),
                                                               static_cast<f32>(camera.origin.y - previousOrigin_.y),
                                                               static_cast<f32>(camera.origin.z - previousOrigin_.z)})
            : camera.unjitteredViewProjection;
    GpuReprojectUniforms reproject;
    reproject.inverse = core::inverse(camera.viewProjection);
    reproject.previous = previous;
    reproject.jitter[0] = camera.jitter.x * 0.5f;
    reproject.jitter[1] = -camera.jitter.y * 0.5f;
    const std::array<rhi::TextureBinding, 1> depth{rhi::TextureBinding{depth_, pointSampler_}};
    cmd.pushDebugGroup("velocity");
    fullscreenPass(cmd, taaVelocity_.handle, velocity_, renderWidth_, renderHeight_, "velocity-camera", depth,
                   asBytes(&reproject, sizeof(reproject)));

    // **What moved by itself**, over that: every part whose place differs
    // from a frame ago, drawn with both places.
    placing_.clear();
    const bool drawable = ensureMotionPipelines(device);
    const f32 pixelsPerUnit = camera.valid ? lodPixelsPerUnit(camera, height_) : 0.0f;
    bool begun = false;
    rhi::PipelineHandle bound{};
    for (const DrawItem& draw : world.draws) {
        if (draw.motionKey == 0 || draw.transparent)
            continue;
        placing_[draw.motionKey] = Placed{draw.transform, camera.origin};
        const auto found = placed_.find(draw.motionKey);
        if (!drawable || !previousCamera_ || found == placed_.end() || !draw.inCameraFrustum || draw.cutout)
            continue;
        // The same place, to a hair: nothing to draw over the camera's motion.
        const Placed& before = found->second;
        bool moved = false;
        for (int column = 0; column < 3 && !moved; ++column) {
            for (int row = 0; row < 3; ++row) {
                if (std::abs(draw.transform.m[column][row] - before.transform.m[column][row]) > 1e-5f) {
                    moved = true;
                    break;
                }
            }
        }
        for (int axis = 0; axis < 3 && !moved; ++axis) {
            const f64 now = static_cast<f64>(draw.transform.m[3][axis]) + (&camera.origin.x)[axis];
            const f64 then = static_cast<f64>(before.transform.m[3][axis]) + (&before.origin.x)[axis];
            moved = std::abs(now - then) > 1e-4;
        }
        if (!moved)
            continue;
        const MeshCache::Resolved* resolved = meshes.resolve(draw.mesh);
        if (resolved == nullptr || resolved->lods.empty())
            continue;
        const u32 lod =
            selectMeshLod(*resolved, draw.transform, pixelsPerUnit, settings_.lodBias, settings_.maximumLodLevel);
        const MeshLodRange& level = resolved->lods[lod];
        if (draw.section >= level.sectionCount || level.firstSection + draw.section >= resolved->sections.size())
            continue;
        const MeshSection& section = resolved->sections[level.firstSection + draw.section];
        if (section.indexCount == 0)
            continue;
        if (!begun) {
            cmd.beginRenderPass({
                .colorAttachments = std::array<rhi::ColorAttachment, 1>{rhi::ColorAttachment{
                    .texture = velocity_,
                    .loadOp = rhi::LoadOp::Load,
                    .storeOp = rhi::StoreOp::Store,
                }},
                .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                .debugName = "velocity-objects",
            });
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            begun = true;
        }
        const bool skinned = draw.boneCount > 0 && resolved->skin.valid();
        const rhi::PipelineHandle pipeline = skinned ? motionSkinnedPipeline_ : motionPipeline_;
        if (!(pipeline == bound)) {
            cmd.setPipeline(pipeline);
            bound = pipeline;
        }
        GpuMotionUniforms motion;
        motion.viewProjection = camera.viewProjection;
        motion.model = draw.transform;
        motion.current = camera.unjitteredViewProjection * draw.transform;
        motion.previous = previousViewProjection_ * before.transform;
        cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&motion, sizeof(motion)));
        if (skinned) {
            bindSkinPalette(cmd, world, draw, false);
            const std::array<rhi::BufferHandle, 2> vertexBuffers{resolved->vertices, resolved->skin};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        else {
            const std::array<rhi::BufferHandle, 1> vertexBuffers{resolved->vertices};
            cmd.bindVertexBuffers(0, vertexBuffers);
        }
        cmd.bindIndexBuffer(resolved->indices, rhi::IndexType::U32);
        cmd.drawIndexed(section.indexCount, 1, resolved->firstIndex + section.firstIndex, resolved->vertexOffset, 0);
        countDraw(DrawKind::Velocity);
    }
    if (begun)
        cmd.endRenderPass();
    cmd.popDebugGroup();
}

rhi::TextureHandle DefaultRenderer::resolveTemporal(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                                    rhi::TextureHandle scene)
{
    const RenderCamera& camera = world.camera;
    // What this frame was is what the next one is measured against, whatever
    // the resolve below manages.
    const auto remember = [&]() { rememberCamera(camera); };
    if (!velocity_.valid() || !ensureLookPipeline(device, taaResolve_, "taa_resolve", kHdrFormat) ||
        !aaTexture(device, history_[0], renderWidth_, renderHeight_, kHdrFormat, "history-a") ||
        !aaTexture(device, history_[1], renderWidth_, renderHeight_, kHdrFormat, "history-b")) {
        remember();
        return scene;
    }
    const u32 write = historyIndex_ ^ 1u;
    GpuTaaUniforms taa;
    taa.texel[0] = 1.0f / static_cast<f32>(renderWidth_);
    taa.texel[1] = 1.0f / static_cast<f32>(renderHeight_);
    taa.texel[2] = static_cast<f32>(renderWidth_);
    taa.texel[3] = static_cast<f32>(renderHeight_);
    // A sixteenth of this frame at rest -- two cycles of the eight samples
    // in the history -- and a fifth in motion, where history resampled every
    // frame softens.
    taa.blend[0] = 0.0625f;
    taa.blend[1] = 0.2f;
    taa.blend[2] = historyValid_ ? 1.0f : 0.0f;
    // **How far outside this frame's neighbourhood the history may stay**:
    // a box of one and a half standard deviations. One flickered on wires
    // thinner than a pixel -- whether one is in the box changes with the
    // jitter, and the history was cut back to the frame each time -- and a
    // wider box keeps more of what moved away. Measured (`anti_aliasing_flicker`):
    // 1 took 30% of FXAA's crawl away, 1.5 nearly half.
    taa.blend[3] = 1.5f;
    taa.jitter[0] = camera.jitter.x * 0.5f;
    taa.jitter[1] = -camera.jitter.y * 0.5f;
    const std::array<rhi::TextureBinding, 5> bindings{
        rhi::TextureBinding{scene, pointSampler_},
        rhi::TextureBinding{history_[historyIndex_], environmentSampler_},
        rhi::TextureBinding{velocity_, pointSampler_},
        rhi::TextureBinding{depth_, pointSampler_},
        rhi::TextureBinding{spriteExactLive_ ? spriteMask_ : blackPixel_, pointSampler_},
    };
    cmd.pushDebugGroup("taa");
    fullscreenPass(cmd, taaResolve_.handle, history_[write], renderWidth_, renderHeight_, "taa", bindings,
                   asBytes(&taa, sizeof(taa)));
    cmd.popDebugGroup();
    historyIndex_ = write;
    historyValid_ = true;
    remember();
    return history_[write];
}

void DefaultRenderer::rememberCamera(const RenderCamera& camera)
{
    previousViewProjection_ = camera.unjitteredViewProjection;
    previousOrigin_ = camera.origin;
    previousCamera_ = true;
    placed_.swap(placing_);
    placing_.clear();
    jitterIndex_ += 1;
}

// --- Frame generation (ADR 0165) -----------------------------------------------

bool DefaultRenderer::generatesFrames(const RenderWorld& world) const noexcept
{
    // The main view's, of a world with depth and motion to interpolate by: a
    // camera with perspective, something behind it, and more than sprites --
    // FSR 2's reasons (`fsr2Frame`), and the temporal pass's.
    return settings_.frameGeneration && !fgFailed_ && fgCompute_ && fgAtomics_ && world.camera.valid &&
           !core::isOrthographic(world.camera.projection) && !world.environment.transparentBackground &&
           !spritesOnly(world);
}

void DefaultRenderer::showPicture(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture,
                                  const RenderTarget& target)
{
    if (!picture.valid() || !target.color.valid())
        return;
    // Written in the format of what it writes, as the tonemap is.
    const bool intoWindow = target.colorFormat != kLdrFormat;
    LookPipeline& slot = intoWindow ? showPictureWindow_ : showPicture_;
    if (!ensureLookPipeline(device, slot, "look_resample", intoWindow ? target.colorFormat : kLdrFormat))
        return;
    // The same size: the nearest texel is the texel.
    const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{picture, pointSampler_}};
    fullscreenPass(cmd, slot.handle, target.color, target.width, target.height, "show-picture", source, {});
}

bool DefaultRenderer::ensureViewSide(rhi::IDevice& device)
{
    if (viewSideTried_)
        return viewSidePipeline_.valid();
    viewSideTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "view_side", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "view_side", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    // Depth alone, written whatever is there: the pass has just cleared it.
    viewSidePipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::Always},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "view_side",
    });
    if (!viewSidePipeline_.valid()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{"view_side"}}};
        core::log(core::LogLevel::Warn, ENG_TR("render.warn.pipeline_failed"), args);
    }
    return viewSidePipeline_.valid();
}

void DefaultRenderer::composeViews(rhi::IDevice& device, rhi::ICmdList& cmd, const ViewComposite& views,
                                   const RenderTarget& target)
{
    if (!target.color.valid() || target.width == 0 || target.height == 0)
        return;
    // Written in the format of what it writes, as a shown picture is.
    const bool intoWindow = target.colorFormat != kLdrFormat;
    LookPipeline& slot = intoWindow ? composeViewsWindow_ : composeViews_;
    if (!ensureLookPipeline(device, slot, "look_views", intoWindow ? target.colorFormat : kLdrFormat))
        return;
    GpuViewsUniforms uniforms;
    // Every slot bound: a view there is none of reads a white pixel and is
    // given no rectangle.
    std::array<rhi::TextureBinding, 5> pictures;
    const u32 count = std::min<u32>(views.count, 4);
    for (u32 index = 0; index < 4; ++index) {
        const ComposedView& view = views.views[index];
        const bool shown = index < count && view.picture.valid() && view.width > 0 && view.height > 0;
        pictures[index] = rhi::TextureBinding{shown ? view.picture : whitePixel_, pointSampler_};
        if (shown) {
            uniforms.rects[index][0] = static_cast<f32>(view.x);
            uniforms.rects[index][1] = static_cast<f32>(view.y);
            uniforms.rects[index][2] = static_cast<f32>(view.width);
            uniforms.rects[index][3] = static_cast<f32>(view.height);
        }
    }
    const bool whole = views.whole.valid() && views.wholeWeight > 0.0f;
    pictures[4] = rhi::TextureBinding{whole ? views.whole : whitePixel_, pointSampler_};
    uniforms.target[0] = static_cast<f32>(target.width);
    uniforms.target[1] = static_cast<f32>(target.height);
    uniforms.target[2] = static_cast<f32>(count);
    uniforms.target[3] = views.acrossLine ? 1.0f : 0.0f;
    uniforms.line[0] = views.lineNormal.x;
    uniforms.line[1] = views.lineNormal.y;
    uniforms.line[2] = 0.5f * views.lineThickness;
    uniforms.line[3] = whole ? std::clamp(views.wholeWeight, 0.0f, 1.0f) : 0.0f;
    uniforms.lineColor[0] = views.lineColor.r;
    uniforms.lineColor[1] = views.lineColor.g;
    uniforms.lineColor[2] = views.lineColor.b;
    uniforms.lineColor[3] = 1.0f;
    fullscreenPass(cmd, slot.handle, target.color, target.width, target.height, "views", pictures,
                   asBytes(&uniforms, sizeof(uniforms)));
}

void DefaultRenderer::presentEye(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle picture,
                                 const RenderTarget& target, const PictureCrop& crop)
{
    if (!picture.valid() || !target.color.valid() || target.width == 0 || target.height == 0)
        return;
    if (!xrEye_.tried)
        xrEyeFormat_ = target.colorFormat;
    LookPipeline& eyeCopy = target.colorFormat == xrEyeFormat_ ? xrEye_ : xrEyeMirror_;
    if (!ensureLookPipeline(device, eyeCopy, "xr_eye", target.colorFormat))
        return;
    const bool srgb = target.colorFormat == rhi::TextureFormat::Rgba8UnormSrgb ||
                      target.colorFormat == rhi::TextureFormat::Bgra8UnormSrgb;
    const bool seeThrough = crop.seeThrough;
    const std::array<f32, 8> uniforms{
        srgb ? 1.0f : 0.0f, seeThrough ? 1.0f : 0.0f, crop.roomOpacity, 0.0f, crop.x, crop.y, crop.width, crop.height};
    const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{picture, linearSampler_}};
    fullscreenPass(cmd, eyeCopy.handle, target.color, target.width, target.height, "xr-eye", source,
                   asBytes(uniforms.data(), sizeof(uniforms)));
}

void DefaultRenderer::presentEyeDepth(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target)
{
    if (!depth_.valid() || !target.color.valid() || target.width == 0 || target.height == 0)
        return;
    // A pipeline that writes depth and no colour, made for the image's
    // format the first time it is asked for, and again if it changes.
    if (!xrDepth_.tried || xrDepthFormat_ != target.colorFormat) {
        xrDepth_ = LookPipeline{};
        xrDepth_.tried = true;
        xrDepthFormat_ = target.colorFormat;
        if (shaderLibrary_ != nullptr) {
            core::EngineError error;
            const rhi::ShaderHandle vertex =
                shaderLibrary_->create(device, "xr_depth", rhi::ShaderStage::Vertex, &error);
            const rhi::ShaderHandle fragment =
                shaderLibrary_->create(device, "xr_depth", rhi::ShaderStage::Fragment, &error);
            for (const rhi::ShaderHandle handle : {vertex, fragment}) {
                if (handle.valid() && shaderCount_ < std::size(shaders_))
                    shaders_[shaderCount_++] = handle;
            }
            if (vertex.valid() && fragment.valid()) {
                xrDepth_.handle = device.createGraphicsPipeline({
                    .vertexShader = vertex,
                    .fragmentShader = fragment,
                    .primitive = rhi::PrimitiveType::TriangleList,
                    .rasterizer = {.cullMode = rhi::CullMode::None},
                    .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::Always},
                    .depthStencilFormat = target.colorFormat,
                    .debugName = "xr_depth",
                });
            }
            if (!xrDepth_.handle.valid()) {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{"xr_depth"}}};
                core::log(core::LogLevel::Warn, ENG_TR("render.warn.pipeline_failed"), args);
            }
        }
    }
    if (!xrDepth_.handle.valid())
        return;
    cmd.beginRenderPass({.depthStencil = {.texture = target.color,
                                          .loadOp = rhi::LoadOp::Clear,
                                          .storeOp = rhi::StoreOp::Store,
                                          .clearDepth = 1.0f},
                         .debugName = "xr-depth"});
    cmd.setPipeline(xrDepth_.handle);
    cmd.setViewport({.width = static_cast<f32>(target.width), .height = static_cast<f32>(target.height)});
    cmd.setScissor({.width = static_cast<core::i32>(target.width), .height = static_cast<core::i32>(target.height)});
    const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{depth_, pointSampler_}};
    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, source);
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
}

void DefaultRenderer::showMotion(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target)
{
    if (!velocity_.valid() || !target.color.valid())
        return;
    const bool intoWindow = target.colorFormat != kLdrFormat;
    LookPipeline& slot = intoWindow ? motionViewWindow_ : motionView_;
    if (!ensureLookPipeline(device, slot, "motion_view", intoWindow ? target.colorFormat : kLdrFormat))
        return;
    const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{velocity_, pointSampler_}};
    fullscreenPass(cmd, slot.handle, target.color, target.width, target.height, "motion-view", source, {});
}

void DefaultRenderer::failFrameGeneration(core::TextKey why)
{
    if (fgFailed_)
        return;
    fgFailed_ = true;
    fgNow_ = false;
    core::log(core::LogLevel::Warn, why, {});
}

bool DefaultRenderer::ensureFrameGeneration(rhi::IDevice& device)
{
    const auto whole = [this]() {
        for (const rhi::ComputePipelineHandle& pipeline : fgPipelines_) {
            if (!pipeline.valid())
                return false;
        }
        return true;
    };
    if (fgTried_)
        return whole();
    fgTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;
    static constexpr std::array<const char*, kFgPassCount> Names{
        "fsr3_of_luma",           "fsr3_of_pyramid",    "fsr3_of_scd_histogram", "fsr3_of_scd_divergence",
        "fsr3_of_search",         "fsr3_of_filter",     "fsr3_of_scale",         "fsr3_fi_setup",
        "fsr3_fi_prepare",        "fsr3_fi_depth",      "fsr3_fi_game_field",    "fsr3_fi_vector_pyramid",
        "fsr3_fi_pyramid_next",   "fsr3_fi_flow_field", "fsr3_fi_disocclusion",  "fsr3_fi_interpolate",
        "fsr3_fi_colour_pyramid", "fsr3_fi_inpaint",
    };
    core::EngineError error;
    for (u32 pass = 0; pass < kFgPassCount; ++pass) {
        fgPipelines_[pass] = shaderLibrary_->createCompute(device, Names[pass], &error);
        if (!fgPipelines_[pass].valid()) {
            core::logText(core::LogLevel::Warn, error.message);
            return false;
        }
    }
    return true;
}

void DefaultRenderer::releaseFrameGenerationImages(rhi::IDevice& device)
{
    const auto release = [&device](rhi::TextureHandle& texture) {
        if (texture.valid())
            device.destroy(texture);
        texture = {};
    };
    for (auto& family : fgLuma_) {
        for (rhi::TextureHandle& texture : family)
            release(texture);
    }
    for (auto& family : fgFlow_) {
        for (rhi::TextureHandle& texture : family)
            release(texture);
    }
    for (rhi::TextureHandle* texture :
         {&fgScdHistogram_, &fgScdPrevious_, &fgScdTemp_, &fgScdOutput_, &fgDilatedMotion_, &fgDilatedDepth_,
          &fgPreviousDepth_, &fgBetweenDepth_, &fgGameField_[0], &fgGameField_[1], &fgFlowField_[0], &fgFlowField_[1],
          &fgDisocclusion_, &fgVectorPyramid_, &fgColourPyramid_, &fgInterpolated_, &fgOutput_})
        release(*texture);
    for (rhi::TextureHandle& texture : fgVectorLevels_)
        release(texture);
    for (rhi::TextureHandle& texture : fgColourLevels_)
        release(texture);
    fgVectorLevels_.clear();
    fgColourLevels_.clear();
    if (fgCounters_.valid())
        device.destroy(fgCounters_);
    fgCounters_ = {};
    fgRenderWidth_ = 0;
    fgRenderHeight_ = 0;
    fgWidth_ = 0;
    fgHeight_ = 0;
    fgFresh_ = true;
}

bool DefaultRenderer::ensureFrameGenerationImages(rhi::IDevice& device, u32 width, u32 height)
{
    const u32 renderWidth = static_cast<u32>(fgCamera_.renderSize[0]);
    const u32 renderHeight = static_cast<u32>(fgCamera_.renderSize[1]);
    if (fgOutput_.valid() && fgWidth_ == width && fgHeight_ == height && fgRenderWidth_ == renderWidth &&
        fgRenderHeight_ == renderHeight)
        return true;
    // Another size is another pair of frames: nothing of the old ones is kept.
    releaseFrameGenerationImages(device);
    if (renderWidth < 2 || renderHeight < 2 || width < 2 || height < 2)
        return false;
    bool made = true;
    using Usage = rhi::TextureUsage;
    const auto image = [&](rhi::TextureHandle& slot, u32 imageWidth, u32 imageHeight, rhi::TextureFormat format,
                           Usage usage, const char* name, u32 mips = 1) {
        slot = device.createTexture({
            .format = format,
            .usage = usage,
            .width = imageWidth,
            .height = imageHeight,
            .mipLevels = mips,
            .debugName = name,
        });
        made = made && slot.valid();
    };
    constexpr rhi::TextureFormat Integer = rhi::TextureFormat::R32Uint;
    // **An image of integers is loaded, not sampled** (`ComputeStorageRead`);
    // one a pass keeps with an atomic, or reads through the binding it writes
    // by, is both read and written (`ComputeStorageReadWrite`); and the ones a
    // new pair of frames starts from nothing are targets too, to be cleared.
    for (u32 family = 0; family < 2; ++family) {
        u32 flowWidth = (width + kFlowBlock - 1) / kFlowBlock;
        u32 flowHeight = (height + kFlowBlock - 1) / kFlowBlock;
        for (u32 level = 0; level < kFlowLevels; ++level) {
            image(fgLuma_[family][level], std::max(width >> level, 1u), std::max(height >> level, 1u), Integer,
                  Usage::ComputeStorageWrite | Usage::ComputeStorageRead | Usage::ColorTarget, "fg-luma");
            image(fgFlow_[family][level], flowWidth, flowHeight, Integer,
                  Usage::ComputeStorageReadWrite | Usage::ComputeStorageRead, "fg-flow");
            flowWidth = (flowWidth + 1) / 2;
            flowHeight = (flowHeight + 1) / 2;
        }
    }
    image(fgScdHistogram_, kFlowHistogramWidth, 1, Integer, Usage::ComputeStorageReadWrite | Usage::ColorTarget,
          "fg-scd-histogram");
    image(fgScdPrevious_, kFlowHistogramWidth, 1, rhi::TextureFormat::R32Float,
          Usage::ComputeStorageReadWrite | Usage::ColorTarget, "fg-scd-previous");
    image(fgScdTemp_, 3, 1, Integer, Usage::ComputeStorageReadWrite | Usage::ColorTarget, "fg-scd-temp");
    image(fgScdOutput_, 3, 1, Integer, Usage::ComputeStorageReadWrite | Usage::ComputeStorageRead | Usage::ColorTarget,
          "fg-scd-output");

    const Usage sampled = Usage::ComputeStorageWrite | Usage::Sampled;
    const Usage kept = Usage::ComputeStorageReadWrite | Usage::ComputeStorageRead;
    image(fgDilatedMotion_, renderWidth, renderHeight, kHdrFormat, sampled, "fg-dilated-motion");
    image(fgDilatedDepth_, renderWidth, renderHeight, rhi::TextureFormat::R32Float, sampled, "fg-dilated-depth");
    image(fgPreviousDepth_, renderWidth, renderHeight, Integer, kept, "fg-previous-depth");
    image(fgBetweenDepth_, renderWidth, renderHeight, Integer, kept, "fg-between-depth");
    image(fgGameField_[0], renderWidth, renderHeight, Integer, kept, "fg-game-field-x");
    image(fgGameField_[1], renderWidth, renderHeight, Integer, kept, "fg-game-field-y");
    image(fgFlowField_[0], renderWidth, renderHeight, Integer, kept, "fg-flow-field-x");
    image(fgFlowField_[1], renderWidth, renderHeight, Integer, kept, "fg-flow-field-y");
    image(fgDisocclusion_, renderWidth, renderHeight, kLdrFormat, sampled, "fg-disocclusion");
    image(fgInterpolated_, width, height, kLdrFormat, sampled, "fg-interpolated");
    image(fgOutput_, width, height, kLdrFormat, sampled, "fg-output");

    // **A pyramid is half its source at its first level and half again at
    // each after**, down to the last level with a pixel each way.
    const auto pyramid = [&](rhi::TextureHandle& whole, std::vector<rhi::TextureHandle>& levels, u32 sourceWidth,
                             u32 sourceHeight, const char* name) {
        u32 count = 0;
        for (u32 levelWidth = sourceWidth / 2, levelHeight = sourceHeight / 2;
             levelWidth >= 1 && levelHeight >= 1 && count < kMaxPyramidLevels; levelWidth /= 2, levelHeight /= 2)
            ++count;
        image(whole, sourceWidth / 2, sourceHeight / 2, kHdrFormat, sampled, name, count);
        levels.resize(count);
        for (u32 level = 0; level < count; ++level)
            image(levels[level], (sourceWidth / 2) >> level, (sourceHeight / 2) >> level, kHdrFormat, sampled, name);
    };
    pyramid(fgVectorPyramid_, fgVectorLevels_, renderWidth, renderHeight, "fg-vector-pyramid");
    pyramid(fgColourPyramid_, fgColourLevels_, width, height, "fg-colour-pyramid");

    fgCounters_ = device.createBuffer({
        .usage = rhi::BufferUsage::ComputeStorageRead | rhi::BufferUsage::ComputeStorageWrite,
        .sizeBytes = 2 * sizeof(u32),
        .debugName = "fg-counters",
    });
    made = made && fgCounters_.valid();
    if (!made) {
        releaseFrameGenerationImages(device);
        return false;
    }
    fgRenderWidth_ = renderWidth;
    fgRenderHeight_ = renderHeight;
    fgWidth_ = width;
    fgHeight_ = height;
    fgFresh_ = true;
    return true;
}

rhi::TextureHandle DefaultRenderer::interpolateFrame(rhi::IDevice& device, rhi::ICmdList& cmd,
                                                     rhi::TextureHandle previous, rhi::TextureHandle current, u32 width,
                                                     u32 height)
{
    // The main view's depth and motion are what it interpolates by.
    useView(0);
    if (!fgNow_ || fgFailed_ || !velocity_.valid() || !depth_.valid() || !current.valid())
        return {};
    if (!ensureFrameGeneration(device) || !ensureFrameGenerationImages(device, width, height)) {
        failFrameGeneration(ENG_TR("render.warn.frame_generation_unavailable"));
        return {};
    }
    // **Nothing to be the frame between**: new images, a camera with nothing
    // before it, or no picture before this one. The passes that keep
    // something from frame to frame still run, so the next frame has it.
    const bool reset = fgFresh_ || motionCut_ || !previous.valid();
    fgFresh_ = false;

    const u32 renderWidth = fgRenderWidth_;
    const u32 renderHeight = fgRenderHeight_;
    const rhi::SamplerHandle exact = pointSampler_;
    const rhi::SamplerHandle filtered = environmentSampler_;
    using Sampled = std::span<const rhi::TextureBinding>;
    using Loaded = std::span<const rhi::TextureHandle>;
    using Written = std::span<const rhi::ComputeTextureWrite>;
    using Buffers = std::span<const rhi::BufferHandle>;
    const auto run = [&](u32 which, Sampled sampled, Loaded loaded, Written written,
                         std::span<const std::byte> constants, u32 groupsX, u32 groupsY, u32 groupsZ = 1,
                         Buffers writtenBuffers = {}, Buffers readBuffers = {}) {
        cmd.beginComputePass(writtenBuffers, written);
        cmd.setComputePipeline(fgPipelines_[which]);
        if (!sampled.empty())
            cmd.bindComputeTextures(0, sampled);
        if (!loaded.empty())
            cmd.bindComputeStorageTextures(0, loaded);
        if (!readBuffers.empty())
            cmd.bindComputeStorageBuffers(0, readBuffers);
        cmd.bindComputeUniforms(0, constants);
        cmd.dispatch(groupsX, groupsY, groupsZ);
        cmd.endComputePass();
    };
    const auto tiles = [](u32 size, u32 tile) { return (size + tile - 1) / tile; };

    cmd.pushDebugGroup("frame-generation");

    // --- Optical flow, after `dispatch` in AMD's runtime -----------------------
    if (reset) {
        const rhi::ColorRgba none{0.0f, 0.0f, 0.0f, 0.0f};
        for (u32 family = 0; family < 2; ++family) {
            for (u32 level = 0; level < kFlowLevels; ++level)
                clearPass(cmd, fgLuma_[family][level], std::max(width >> level, 1u), std::max(height >> level, 1u),
                          "fg-clear", none);
        }
        clearPass(cmd, fgScdHistogram_, kFlowHistogramWidth, 1, "fg-clear", none);
        clearPass(cmd, fgScdPrevious_, kFlowHistogramWidth, 1, "fg-clear", none);
        clearPass(cmd, fgScdTemp_, 3, 1, "fg-clear", none);
        clearPass(cmd, fgScdOutput_, 3, 1, "fg-clear", none);
    }
    fgFrameIndex_ = reset ? 0u : fgFrameIndex_ + 1u;
    GpuOpticalFlowConstants flow;
    flow.inputLumaResolution[0] = static_cast<core::i32>(width);
    flow.inputLumaResolution[1] = static_cast<core::i32>(height);
    flow.pyramidLevelCount = kFlowLevels;
    flow.frameIndex = fgFrameIndex_;
    // The picture is as the window shows it: eight bits, already encoded.
    flow.backbufferTransferFunction = 0;
    flow.minMaxLuminance[1] = 1.0f;
    const auto flowBytes = [&flow](u32 level) {
        flow.pyramidLevel = level;
        return asBytes(&flow, sizeof(flow));
    };
    const u32 now = fgParity_ & 1u;
    const u32 before = now ^ 1u;
    const auto lumaWidth = [width](u32 level) { return std::max(width >> level, 1u); };
    const auto lumaHeight = [height](u32 level) { return std::max(height >> level, 1u); };
    {
        // The picture as a luminance, each thread four pixels.
        const std::array<rhi::TextureBinding, 1> sampled{rhi::TextureBinding{current, exact}};
        const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgLuma_[now][0]}};
        run(kFgLuma, sampled, {}, written, flowBytes(0), tiles((width + 1) / 2, 16), tiles((height + 1) / 2, 16));
    }
    for (u32 level = 0; level + 1 < kFlowLevels; ++level) {
        const std::array<rhi::TextureHandle, 1> loaded{fgLuma_[now][level]};
        const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgLuma_[now][level + 1]}};
        run(kFgPyramid, {}, loaded, written, flowBytes(level), tiles(lumaWidth(level + 1), 8),
            tiles(lumaHeight(level + 1), 8));
    }
    {
        // Whether the scene was cut: nine histograms, against the last frame's.
        const std::array<rhi::TextureHandle, 1> loaded{fgLuma_[now][0]};
        const std::array<rhi::ComputeTextureWrite, 1> histogram{rhi::ComputeTextureWrite{fgScdHistogram_}};
        run(kFgScdHistogram, {}, loaded, histogram, flowBytes(0), tiles((width / 4) / 3, 32), 16, 9);
        const std::array<rhi::ComputeTextureWrite, 4> written{
            rhi::ComputeTextureWrite{fgScdHistogram_},
            rhi::ComputeTextureWrite{fgScdPrevious_},
            rhi::ComputeTextureWrite{fgScdTemp_},
            rhi::ComputeTextureWrite{fgScdOutput_},
        };
        run(kFgScdDivergence, {}, {}, written, flowBytes(0), 9, 3);
    }
    {
        // The size of the motion's image at each level: a block of eight
        // pixels at the finest, and half as many blocks at each coarser.
        std::array<u32, kFlowLevels> blocksWide{};
        std::array<u32, kFlowLevels> blocksHigh{};
        blocksWide[0] = tiles(width, kFlowBlock);
        blocksHigh[0] = tiles(height, kFlowBlock);
        for (u32 level = 1; level < kFlowLevels; ++level) {
            blocksWide[level] = (blocksWide[level - 1] + 1) / 2;
            blocksHigh[level] = (blocksHigh[level - 1] + 1) / 2;
        }
        // Coarsest first: search, take the middle of each neighbourhood, and
        // hand the result to the next level as its guess. A level reads one
        // of the two images of its size and writes the other.
        for (u32 step = 0; step < kFlowLevels; ++step) {
            const u32 level = kFlowLevels - 1 - step;
            const u32 searched = level & 1u;
            const u32 settled = searched ^ 1u;
            const std::span<const std::byte> constants = flowBytes(level);
            {
                const std::array<rhi::TextureHandle, 2> loaded{fgLuma_[now][level], fgLuma_[before][level]};
                const std::array<rhi::ComputeTextureWrite, 2> written{
                    rhi::ComputeTextureWrite{fgFlow_[searched][level]},
                    rhi::ComputeTextureWrite{fgScdOutput_},
                };
                run(kFgSearch, {}, loaded, written, constants, tiles(lumaWidth(level), 16),
                    tiles(lumaHeight(level), 16));
            }
            {
                const std::array<rhi::TextureHandle, 1> loaded{fgFlow_[searched][level]};
                const std::array<rhi::ComputeTextureWrite, 1> written{
                    rhi::ComputeTextureWrite{fgFlow_[settled][level]}};
                run(kFgFilter, {}, loaded, written, constants, tiles(blocksWide[level], 16),
                    tiles(blocksHigh[level], 4));
            }
            if (level > 0) {
                const std::array<rhi::TextureHandle, 3> loaded{fgLuma_[now][level], fgLuma_[before][level],
                                                               fgFlow_[settled][level]};
                const std::array<rhi::ComputeTextureWrite, 2> written{
                    rhi::ComputeTextureWrite{fgFlow_[settled][level - 1]},
                    rhi::ComputeTextureWrite{fgScdOutput_},
                };
                run(kFgScale, {}, loaded, written, constants, tiles(blocksWide[level - 1], 4),
                    tiles(blocksHigh[level - 1], 4));
            }
        }
    }
    // The finest level's, after its filter: level zero is even, so it settled
    // in the second of the two.
    const rhi::TextureHandle motion = fgFlow_[1][0];

    // --- Frame interpolation, after `ffxFrameInterpolationDispatch` ------------
    GpuFrameInterpolationConstants constants = fgCamera_;
    constants.displaySize[0] = static_cast<core::i32>(width);
    constants.displaySize[1] = static_cast<core::i32>(height);
    constants.displaySizeRcp[0] = 1.0f / static_cast<f32>(width);
    constants.displaySizeRcp[1] = 1.0f / static_cast<f32>(height);
    constants.upscalerTargetSize[0] = constants.displaySize[0];
    constants.upscalerTargetSize[1] = constants.displaySize[1];
    constants.interpolationRectSize[0] = constants.displaySize[0];
    constants.interpolationRectSize[1] = constants.displaySize[1];
    constants.reset = reset ? 1 : 0;
    // Optical flow's motion is in pixels of the picture.
    constants.opticalFlowScale[0] = constants.displaySizeRcp[0];
    constants.opticalFlowScale[1] = constants.displaySizeRcp[1];
    constants.opticalFlowBlockSize = static_cast<core::i32>(kFlowBlock);
    constants.minMaxLuminance[1] = 1.0f;
    // Read by nothing the engine builds: a constant, so a frame's commands
    // do not depend on the clock.
    constants.deltaTime = 1000.0f / 60.0f;
    const std::span<const std::byte> frame = asBytes(&constants, sizeof(constants));
    const u32 renderX = tiles(renderWidth, 8);
    const u32 renderY = tiles(renderHeight, 8);
    const u32 pictureX = tiles(width, 8);
    const u32 pictureY = tiles(height, 8);
    {
        // The fields the passes keep with atomics, emptied; the two depths
        // set to the furthest there is; the count of frames since a cut.
        const std::array<rhi::TextureHandle, 1> loaded{fgScdOutput_};
        const std::array<rhi::ComputeTextureWrite, 7> written{
            rhi::ComputeTextureWrite{fgGameField_[0]},  rhi::ComputeTextureWrite{fgGameField_[1]},
            rhi::ComputeTextureWrite{fgFlowField_[0]},  rhi::ComputeTextureWrite{fgFlowField_[1]},
            rhi::ComputeTextureWrite{fgDisocclusion_},  rhi::ComputeTextureWrite{fgBetweenDepth_},
            rhi::ComputeTextureWrite{fgPreviousDepth_},
        };
        const std::array<rhi::BufferHandle, 1> counters{fgCounters_};
        run(kFgSetup, {}, loaded, written, frame, renderX, renderY, 1, counters);
    }
    {
        // Each pixel's motion and depth from its nearest neighbour, and that
        // depth carried to where the pixel was.
        const std::array<rhi::TextureBinding, 2> sampled{
            rhi::TextureBinding{velocity_, exact},
            rhi::TextureBinding{depth_, exact},
        };
        const std::array<rhi::ComputeTextureWrite, 3> written{
            rhi::ComputeTextureWrite{fgPreviousDepth_},
            rhi::ComputeTextureWrite{fgDilatedMotion_},
            rhi::ComputeTextureWrite{fgDilatedDepth_},
        };
        run(kFgPrepare, sampled, {}, written, frame, renderX, renderY);
    }
    rhi::TextureHandle made{};
    if (!reset) {
        // A pyramid: its first level from its source, each after from the
        // level before.
        const auto pyramid = [&](rhi::TextureHandle whole, const std::vector<rhi::TextureHandle>& levels,
                                 u32 sourceWidth, u32 sourceHeight, bool colours) {
            for (u32 level = 1; level < levels.size(); ++level) {
                GpuPyramidConstants next;
                next.sourceSize[0] = static_cast<core::i32>((sourceWidth / 2) >> (level - 1));
                next.sourceSize[1] = static_cast<core::i32>((sourceHeight / 2) >> (level - 1));
                next.colours = colours ? 1 : 0;
                const std::array<rhi::TextureBinding, 1> sampled{rhi::TextureBinding{levels[level - 1], exact}};
                const std::array<rhi::ComputeTextureWrite, 2> written{
                    rhi::ComputeTextureWrite{levels[level]},
                    rhi::ComputeTextureWrite{whole, level},
                };
                run(kFgPyramidNext, sampled, {}, written, asBytes(&next, sizeof(next)),
                    tiles(static_cast<u32>(next.sourceSize[0]) / 2, 8),
                    tiles(static_cast<u32>(next.sourceSize[1]) / 2, 8));
            }
        };
        // No lens distortion: a texel of nothing.
        const rhi::TextureBinding undistorted{blackPixel_, exact};
        {
            // The depth of the frame between.
            const std::array<rhi::TextureBinding, 3> sampled{
                rhi::TextureBinding{fgDilatedMotion_, exact},
                rhi::TextureBinding{fgDilatedDepth_, exact},
                undistorted,
            };
            const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgBetweenDepth_}};
            run(kFgDepth, sampled, {}, written, frame, renderX, renderY);
        }
        {
            // The game's motion, as seen from the frame between.
            const std::array<rhi::TextureBinding, 5> sampled{
                rhi::TextureBinding{fgDilatedMotion_, exact},
                rhi::TextureBinding{fgDilatedDepth_, exact},
                rhi::TextureBinding{previous, filtered},
                rhi::TextureBinding{current, filtered},
                undistorted,
            };
            const std::array<rhi::ComputeTextureWrite, 2> written{
                rhi::ComputeTextureWrite{fgGameField_[0]},
                rhi::ComputeTextureWrite{fgGameField_[1]},
            };
            run(kFgGameField, sampled, {}, written, frame, renderX, renderY);
        }
        if (!fgVectorLevels_.empty()) {
            // And that field at every coarser size, for the places no vector
            // reached.
            const std::array<rhi::TextureHandle, 2> loaded{fgGameField_[0], fgGameField_[1]};
            const std::array<rhi::ComputeTextureWrite, 2> written{
                rhi::ComputeTextureWrite{fgVectorLevels_[0]},
                rhi::ComputeTextureWrite{fgVectorPyramid_, 0},
            };
            run(kFgVectorPyramid, {}, loaded, written, frame, tiles(renderWidth / 2, 8), tiles(renderHeight / 2, 8));
            pyramid(fgVectorPyramid_, fgVectorLevels_, renderWidth, renderHeight, false);
        }
        {
            // Optical flow's motion, as seen from the frame between.
            const std::array<rhi::TextureBinding, 2> sampled{
                rhi::TextureBinding{previous, filtered},
                rhi::TextureBinding{current, filtered},
            };
            const std::array<rhi::TextureHandle, 1> loaded{motion};
            const std::array<rhi::ComputeTextureWrite, 2> written{
                rhi::ComputeTextureWrite{fgFlowField_[0]},
                rhi::ComputeTextureWrite{fgFlowField_[1]},
            };
            run(kFgFlowField, sampled, loaded, written, frame, tiles(width / kFlowBlock, 8),
                tiles(height / kFlowBlock, 8));
        }
        {
            // What the frame between cannot take from each of its neighbours.
            const std::array<rhi::TextureBinding, 3> sampled{
                rhi::TextureBinding{fgDilatedDepth_, exact},
                rhi::TextureBinding{fgVectorPyramid_, exact},
                undistorted,
            };
            const std::array<rhi::TextureHandle, 4> loaded{fgGameField_[0], fgGameField_[1], fgPreviousDepth_,
                                                           fgBetweenDepth_};
            const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgDisocclusion_}};
            run(kFgDisocclusion, sampled, loaded, written, frame, renderX, renderY);
        }
        {
            // The frame between.
            const std::array<rhi::TextureBinding, 4> sampled{
                rhi::TextureBinding{previous, filtered},
                rhi::TextureBinding{current, filtered},
                rhi::TextureBinding{fgDisocclusion_, filtered},
                rhi::TextureBinding{fgVectorPyramid_, exact},
            };
            const std::array<rhi::TextureHandle, 4> loaded{fgGameField_[0], fgGameField_[1], fgFlowField_[0],
                                                           fgFlowField_[1]};
            const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgInterpolated_}};
            const std::array<rhi::BufferHandle, 1> counters{fgCounters_};
            run(kFgInterpolate, sampled, loaded, written, frame, pictureX, pictureY, 1, {}, counters);
        }
        if (!fgColourLevels_.empty()) {
            // It at every coarser size, for its holes.
            const std::array<rhi::TextureBinding, 1> sampled{rhi::TextureBinding{fgInterpolated_, exact}};
            const std::array<rhi::ComputeTextureWrite, 2> written{
                rhi::ComputeTextureWrite{fgColourLevels_[0]},
                rhi::ComputeTextureWrite{fgColourPyramid_, 0},
            };
            run(kFgColourPyramid, sampled, {}, written, frame, tiles(width / 2, 8), tiles(height / 2, 8));
            pyramid(fgColourPyramid_, fgColourLevels_, width, height, true);
        }
        {
            // The holes filled. The picture has no interface on it, so "the
            // picture as presented" is the picture.
            const std::array<rhi::TextureBinding, 4> sampled{
                rhi::TextureBinding{fgColourPyramid_, exact},
                rhi::TextureBinding{current, exact},
                rhi::TextureBinding{current, exact},
                rhi::TextureBinding{fgInterpolated_, exact},
            };
            const std::array<rhi::TextureHandle, 1> loaded{fgScdOutput_};
            const std::array<rhi::ComputeTextureWrite, 1> written{rhi::ComputeTextureWrite{fgOutput_}};
            run(kFgInpaint, sampled, loaded, written, frame, pictureX, pictureY);
        }
        made = fgOutput_;
    }
    cmd.popDebugGroup();
    fgParity_ = before;
    return made;
}

// --- The temporal upscaler (ADR 0164) ------------------------------------------

void DefaultRenderer::failFsr2()
{
    if (fsr2Failed_)
        return;
    fsr2Failed_ = true;
    core::log(core::LogLevel::Warn, ENG_TR("render.warn.fsr2_unavailable"), {});
}

bool DefaultRenderer::ensureFsr2(rhi::IDevice& device)
{
    ENG_PROFILE_SCOPE("prepare.fsr2");
    const auto whole = [this]() {
        for (const rhi::ComputePipelineHandle& pipeline : fsr2Pipelines_) {
            if (!pipeline.valid())
                return false;
        }
        return true;
    };
    if (fsr2Tried_)
        return whole();
    fsr2Tried_ = true;
    if (shaderLibrary_ == nullptr || !device.caps().compute)
        return false;
    static constexpr std::array<const char*, kFsr2PassCount> Names{
        "fsr2_luminance",  "fsr2_reconstruct",        "fsr2_depth_clip", "fsr2_lock",
        "fsr2_accumulate", "fsr2_accumulate_sharpen", "fsr2_rcas",
    };
    core::EngineError error;
    for (u32 pass = 0; pass < kFsr2PassCount; ++pass) {
        fsr2Pipelines_[pass] = shaderLibrary_->createCompute(device, Names[pass], &error);
        if (!fsr2Pipelines_[pass].valid()) {
            core::logText(core::LogLevel::Warn, error.message);
            return false;
        }
    }
    return true;
}

void DefaultRenderer::releaseFsr2Images(rhi::IDevice& device)
{
    for (rhi::TextureHandle* texture :
         {&fsr2Prepared_, &fsr2PreviousDepth_, &fsr2DilatedMotion_[0], &fsr2DilatedMotion_[1], &fsr2DilatedDepth_,
          &fsr2LockLuma_, &fsr2Masks_, &fsr2Luminance_, &fsr2LockStatus_[0], &fsr2LockStatus_[1], &fsr2NewLocks_,
          &fsr2History_[0], &fsr2History_[1], &fsr2LumaHistory_[0], &fsr2LumaHistory_[1], &fsr2Output_, &fsr2Opaque_,
          &fsr2Reactive_[0], &fsr2Reactive_[1]}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    fsr2RenderWidth_ = 0;
    fsr2RenderHeight_ = 0;
    fsr2OutputWidth_ = 0;
    fsr2OutputHeight_ = 0;
    fsr2Fresh_ = true;
    fsr2OpaqueLive_ = false;
    fsr2ReactiveLive_ = false;
    fsr2ReactiveTail_ = 0;
}

void DefaultRenderer::copyOpaqueForUpscaler(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                            const GpuFrameUniforms& frame)
{
    // **Only a frame with something blended in it pays for the copy**, and
    // the mask after it.
    bool blends = particleCount_ > 0 || ribbonVertexCount_ > 0 || !world.gpuEmitters.empty() || worldUiVertexCount_ > 0;
    for (usize index = 0; index < world.draws.size() && !blends; ++index)
        blends = world.draws[index].transparent && world.draws[index].inCameraFrustum;
    if (!blends || !ensureLookPipeline(device, resample_, "look_resample", kHdrFormat) ||
        !aaTexture(device, fsr2Opaque_, renderWidth_, renderHeight_, kHdrFormat, "fsr2-opaque"))
        return;
    cmd.endRenderPass();
    const std::array<rhi::TextureBinding, 1> color{rhi::TextureBinding{hdr_, pointSampler_}};
    fullscreenPass(cmd, resample_.handle, fsr2Opaque_, renderWidth_, renderHeight_, "fsr2-opaque", color, {});
    fsr2OpaqueLive_ = true;

    const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
        .texture = hdr_,
        .loadOp = rhi::LoadOp::Load,
        .storeOp = rhi::StoreOp::Store,
    }};
    cmd.beginRenderPass({
        .colorAttachments = resumeTarget,
        .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
        .debugName = "forward-after-opaque-copy",
    });
    cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
    cmd.setScissor({.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
    // The frame block again: the copy bound its own at the same slot.
    cmd.setPipeline(pbrBlendPipeline_);
    cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
}

void DefaultRenderer::writeReactiveMask(rhi::IDevice& device, rhi::ICmdList& cmd)
{
    // A frame with something blended, or one of the few after it: what the
    // last mask held is still in the history (`fsr2_reactive.hlsl`).
    const bool tail = fsr2ReactiveTail_ > 0;
    if ((!fsr2OpaqueLive_ && !tail) ||
        !ensureLookPipeline(device, fsr2ReactivePipeline_, "fsr2_reactive", kSpriteMaskFormat) ||
        !aaTexture(device, fsr2Reactive_[0], renderWidth_, renderHeight_, kSpriteMaskFormat, "fsr2-reactive-a") ||
        !aaTexture(device, fsr2Reactive_[1], renderWidth_, renderHeight_, kSpriteMaskFormat, "fsr2-reactive-b"))
        return;
    const u32 write = fsr2ReactiveIndex_ ^ 1u;
    const std::array<rhi::TextureBinding, 4> bindings{
        // With nothing blended this frame the scene is its own "before".
        rhi::TextureBinding{fsr2OpaqueLive_ ? fsr2Opaque_ : hdr_, pointSampler_},
        rhi::TextureBinding{hdr_, pointSampler_},
        rhi::TextureBinding{spriteExactLive_ ? spriteMask_ : blackPixel_, pointSampler_},
        rhi::TextureBinding{tail ? fsr2Reactive_[fsr2ReactiveIndex_] : blackPixel_, pointSampler_},
    };
    fullscreenPass(cmd, fsr2ReactivePipeline_.handle, fsr2Reactive_[write], renderWidth_, renderHeight_,
                   "fsr2-reactive", bindings, {});
    fsr2ReactiveIndex_ = write;
    fsr2ReactiveLive_ = true;
    // Four frames at six tenths each is an eighth of the mask: nothing.
    fsr2ReactiveTail_ = fsr2OpaqueLive_ ? 4u : fsr2ReactiveTail_ - 1u;
}

bool DefaultRenderer::ensureFsr2Images(rhi::IDevice& device, u32 outputWidth, u32 outputHeight)
{
    // At another size they were released when the frame began (`render`):
    // another size is another history, and nothing of the old one is kept.
    if (fsr2Output_.valid())
        return true;
    bool made = true;
    // **Each a target as well as an image a pass writes**: a new history is
    // cleared, and a clear is a render pass. `readBack` for the one a pass
    // reads through the binding it writes it by.
    const auto image = [&](rhi::TextureHandle& slot, u32 width, u32 height, rhi::TextureFormat format, bool readBack,
                           const char* name) {
        slot = device.createTexture({
            .format = format,
            .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget |
                     (readBack ? rhi::TextureUsage::ComputeStorageReadWrite : rhi::TextureUsage::ComputeStorageWrite),
            .width = width,
            .height = height,
            .debugName = name,
        });
        made = made && slot.valid();
    };
    // **The previous depth is the one image that is neither**: an integer,
    // kept by an atomic minimum, read only through the binding it is written
    // by, and never cleared from here -- the lock pass leaves it at the
    // furthest there is, for the next frame's reconstruction to bring nearer.
    fsr2PreviousDepth_ = device.createTexture({
        .format = rhi::TextureFormat::R32Uint,
        .usage = rhi::TextureUsage::ComputeStorageReadWrite,
        .width = renderWidth_,
        .height = renderHeight_,
        .debugName = "fsr2-previous-depth",
    });
    made = made && fsr2PreviousDepth_.valid();
    // **Three formats and no others** (the callbacks header says why): what
    // AMD keeps in two channels or one of sixteen bits is kept in four here,
    // or in one of thirty-two.
    const u32 lumaWidth = std::max(renderWidth_ / 32u, 1u);
    const u32 lumaHeight = std::max(renderHeight_ / 32u, 1u);
    image(fsr2Prepared_, renderWidth_, renderHeight_, kHdrFormat, false, "fsr2-prepared-colour");
    image(fsr2DilatedMotion_[0], renderWidth_, renderHeight_, kHdrFormat, false, "fsr2-dilated-motion-a");
    image(fsr2DilatedMotion_[1], renderWidth_, renderHeight_, kHdrFormat, false, "fsr2-dilated-motion-b");
    image(fsr2DilatedDepth_, renderWidth_, renderHeight_, rhi::TextureFormat::R32Float, false, "fsr2-dilated-depth");
    image(fsr2LockLuma_, renderWidth_, renderHeight_, rhi::TextureFormat::R32Float, false, "fsr2-lock-luma");
    image(fsr2Masks_, renderWidth_, renderHeight_, kLdrFormat, false, "fsr2-reactive-masks");
    image(fsr2Luminance_, lumaWidth, lumaHeight, kHdrFormat, false, "fsr2-luminance");
    image(fsr2LockStatus_[0], outputWidth, outputHeight, kHdrFormat, false, "fsr2-lock-status-a");
    image(fsr2LockStatus_[1], outputWidth, outputHeight, kHdrFormat, false, "fsr2-lock-status-b");
    image(fsr2NewLocks_, outputWidth, outputHeight, rhi::TextureFormat::R32Float, true, "fsr2-new-locks");
    image(fsr2History_[0], outputWidth, outputHeight, kHdrFormat, false, "fsr2-history-a");
    image(fsr2History_[1], outputWidth, outputHeight, kHdrFormat, false, "fsr2-history-b");
    image(fsr2LumaHistory_[0], outputWidth, outputHeight, kLdrFormat, false, "fsr2-luma-history-a");
    image(fsr2LumaHistory_[1], outputWidth, outputHeight, kLdrFormat, false, "fsr2-luma-history-b");
    image(fsr2Output_, outputWidth, outputHeight, kHdrFormat, false, "fsr2-output");
    if (!made) {
        releaseFsr2Images(device);
        return false;
    }
    fsr2RenderWidth_ = renderWidth_;
    fsr2RenderHeight_ = renderHeight_;
    fsr2OutputWidth_ = outputWidth;
    fsr2OutputHeight_ = outputHeight;
    fsr2Fresh_ = true;
    return true;
}

rhi::TextureHandle DefaultRenderer::upscaleTemporal(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                                    const RenderTarget& target, rhi::TextureHandle scene)
{
    const RenderCamera& camera = world.camera;
    if (!velocity_.valid())
        return {};
    if (!ensureFsr2Images(device, target.width, target.height)) {
        failFsr2();
        return {};
    }
    const u32 outputWidth = target.width;
    const u32 outputHeight = target.height;
    // **A history that is not this view's any more is begun again**: new
    // images, or a camera that was cut (`writeVelocity`).
    const bool reset = fsr2Fresh_ || !historyValid_;

    // --- The frame's constants, after `fsr2Dispatch` in AMD's runtime ---------
    GpuFsr2Constants constants;
    constants.renderSize[0] = static_cast<core::i32>(renderWidth_);
    constants.renderSize[1] = static_cast<core::i32>(renderHeight_);
    constants.maxRenderSize[0] = constants.renderSize[0];
    constants.maxRenderSize[1] = constants.renderSize[1];
    constants.displaySize[0] = static_cast<core::i32>(outputWidth);
    constants.displaySize[1] = static_cast<core::i32>(outputHeight);
    constants.inputColorResourceDimensions[0] = constants.renderSize[0];
    constants.inputColorResourceDimensions[1] = constants.renderSize[1];
    constants.lumaMipDimensions[0] = static_cast<core::i32>(std::max(renderWidth_ / 32u, 1u));
    constants.lumaMipDimensions[1] = static_cast<core::i32>(std::max(renderHeight_ / 32u, 1u));
    // The level of AMD's chain the luminance image stands for; it is one
    // image here and the number is only carried.
    constants.lumaMipLevelToUse = 4;
    fsr2FrameIndex_ = reset ? 0 : fsr2FrameIndex_ + 1;
    constants.frameIndex = fsr2FrameIndex_;
    // **From a depth to a distance, and from a pixel to a direction**: the
    // projection's own four numbers, read out of the matrix the frame was
    // drawn with -- AMD's runtime derives the same four from a near plane, a
    // far one and an angle.
    const Mat4& projection = camera.projection;
    constants.deviceToViewDepth[0] = -projection.m[2][2];
    constants.deviceToViewDepth[1] = projection.m[3][2];
    constants.deviceToViewDepth[2] = 1.0f / projection.m[0][0];
    constants.deviceToViewDepth[3] = 1.0f / projection.m[1][1];
    constants.tanHalfFov = 1.0f / projection.m[0][0];
    // **The jitter in pixels, down the screen**: the camera's is in clip
    // space, where up is positive.
    constants.jitterOffset[0] = camera.jitter.x * 0.5f * static_cast<f32>(renderWidth_);
    constants.jitterOffset[1] = -camera.jitter.y * 0.5f * static_cast<f32>(renderHeight_);
    // **`velocity_` is how far a pixel moved, in the picture's own unit, and
    // where it was is where it is less that**; the algorithm adds.
    constants.motionVectorScale[0] = -1.0f;
    constants.motionVectorScale[1] = -1.0f;
    constants.downscaleFactor[0] = static_cast<f32>(renderWidth_) / static_cast<f32>(outputWidth);
    constants.downscaleFactor[1] = static_cast<f32>(renderHeight_) / static_cast<f32>(outputHeight);
    // The sequence's length follows the scale a step a frame, as AMD's does.
    const auto phases = static_cast<f32>(fsr2JitterPhases(renderWidth_, outputWidth));
    if (reset || fsr2Phases_ == 0.0f)
        fsr2Phases_ = phases;
    else if (phases > fsr2Phases_)
        fsr2Phases_ += 1.0f;
    else if (phases < fsr2Phases_)
        fsr2Phases_ -= 1.0f;
    constants.jitterPhaseCount = fsr2Phases_;
    // Read by AMD's own exposure alone, which the engine's replaces: a
    // constant, so a frame's commands do not depend on the clock.
    constants.deltaTime = 1.0f / 60.0f;

    // --- Which of each pair -----------------------------------------------------
    const u32 read = fsr2Parity_ & 1u;
    const u32 write = read ^ 1u;
    // This frame's dilated motion is the one the next frame calls the last.
    const rhi::TextureHandle motion = fsr2DilatedMotion_[read];
    const rhi::TextureHandle motionBefore = fsr2DilatedMotion_[write];

    cmd.pushDebugGroup("fsr2");
    if (reset) {
        const rhi::ColorRgba none{0.0f, 0.0f, 0.0f, 0.0f};
        for (const rhi::TextureHandle texture :
             {fsr2Prepared_, fsr2DilatedMotion_[0], fsr2DilatedMotion_[1], fsr2Masks_, fsr2LockLuma_})
            clearPass(cmd, texture, renderWidth_, renderHeight_, "fsr2-clear", none);
        for (const rhi::TextureHandle texture : {fsr2LockStatus_[0], fsr2LockStatus_[1], fsr2NewLocks_, fsr2History_[0],
                                                 fsr2History_[1], fsr2LumaHistory_[0], fsr2LumaHistory_[1]})
            clearPass(cmd, texture, outputWidth, outputHeight, "fsr2-clear", none);
        clearPass(cmd, fsr2Luminance_, static_cast<u32>(constants.lumaMipDimensions[0]),
                  static_cast<u32>(constants.lumaMipDimensions[1]), "fsr2-clear", none);
    }

    // **The last frame's exposure**, which this frame's is made from after
    // this pass -- or white, a luminance of one, with nothing metered yet.
    const rhi::TextureHandle exposure = exposureInitialised_ ? exposure_[exposureIndex_] : whitePixel_;
    // **Exactly the texel wherever a pass reads one, and filtered where it
    // samples between them**: each texture has its own sampler here, and a
    // thirty-two-bit float is not filtered on every device.
    const rhi::SamplerHandle exact = pointSampler_;
    const rhi::SamplerHandle filtered = environmentSampler_;
    const auto pass = [&](u32 which, std::span<const rhi::TextureBinding> reads,
                          std::span<const rhi::ComputeTextureWrite> writes, u32 width, u32 height, u32 tile,
                          std::span<const std::byte> second = {}) {
        cmd.beginComputePass(std::span<const rhi::BufferHandle>{}, writes);
        cmd.setComputePipeline(fsr2Pipelines_[which]);
        cmd.bindComputeTextures(0, reads);
        cmd.bindComputeUniforms(0, asBytes(&constants, sizeof(constants)));
        if (!second.empty())
            cmd.bindComputeUniforms(1, second);
        cmd.dispatch((width + tile - 1) / tile, (height + tile - 1) / tile, 1);
        cmd.endComputePass();
    };

    // The frame's luminance in patches, which the accumulation compares with
    // what a pixel's history remembers of it.
    {
        const std::array<rhi::TextureBinding, 1> reads{rhi::TextureBinding{scene, filtered}};
        const std::array<rhi::ComputeTextureWrite, 1> writes{rhi::ComputeTextureWrite{fsr2Luminance_}};
        pass(kFsr2Luminance, reads, writes, static_cast<u32>(constants.lumaMipDimensions[0]),
             static_cast<u32>(constants.lumaMipDimensions[1]), 8);
    }
    // **A new history's previous depth is whatever the image was made
    // with**, and the lock pass is what clears it: run once first, on the
    // images just cleared, where it finds nothing to lock.
    const auto lock = [&]() {
        const std::array<rhi::TextureBinding, 1> reads{rhi::TextureBinding{fsr2LockLuma_, exact}};
        const std::array<rhi::ComputeTextureWrite, 2> writes{
            rhi::ComputeTextureWrite{fsr2NewLocks_},
            rhi::ComputeTextureWrite{fsr2PreviousDepth_},
        };
        pass(kFsr2Lock, reads, writes, renderWidth_, renderHeight_, 8);
    };
    if (reset)
        lock();
    // Each pixel's motion and depth from its nearest neighbour, and that depth
    // carried to where the pixel was.
    {
        const std::array<rhi::TextureBinding, 4> reads{
            rhi::TextureBinding{velocity_, exact},
            rhi::TextureBinding{depth_, exact},
            rhi::TextureBinding{scene, exact},
            rhi::TextureBinding{exposure, exact},
        };
        const std::array<rhi::ComputeTextureWrite, 4> writes{
            rhi::ComputeTextureWrite{fsr2PreviousDepth_},
            rhi::ComputeTextureWrite{motion},
            rhi::ComputeTextureWrite{fsr2DilatedDepth_},
            rhi::ComputeTextureWrite{fsr2LockLuma_},
        };
        pass(kFsr2Reconstruct, reads, writes, renderWidth_, renderHeight_, 8);
    }
    // What came out from behind something, and the colour prepared.
    {
        // **What blends, and a sprite drawn in its own colours**: neither
        // has a motion of its own, and where one is the history is not to be
        // trusted (`fsr2_reactive.hlsl`, which has the sprites in it).
        const rhi::TextureHandle reactive = fsr2ReactiveLive_  ? fsr2Reactive_[fsr2ReactiveIndex_]
                                            : spriteExactLive_ ? spriteMask_
                                                               : blackPixel_;
        const std::array<rhi::TextureBinding, 8> reads{
            rhi::TextureBinding{motion, exact},          rhi::TextureBinding{fsr2DilatedDepth_, exact},
            rhi::TextureBinding{reactive, exact},        rhi::TextureBinding{blackPixel_, exact},
            rhi::TextureBinding{motionBefore, filtered}, rhi::TextureBinding{velocity_, exact},
            rhi::TextureBinding{scene, exact},           rhi::TextureBinding{exposure, exact},
        };
        // The previous depth among what it "writes": it is read through
        // that binding, an integer image being nothing a sampler reads.
        const std::array<rhi::ComputeTextureWrite, 3> writes{
            rhi::ComputeTextureWrite{fsr2Masks_},
            rhi::ComputeTextureWrite{fsr2Prepared_},
            rhi::ComputeTextureWrite{fsr2PreviousDepth_},
        };
        pass(kFsr2DepthClip, reads, writes, renderWidth_, renderHeight_, 8);
    }
    // The features thinner than a rendered pixel, locked so they stay -- and
    // the previous depth left at the furthest there is, for the next frame.
    lock();
    // This frame into the history, at the output's size -- into the output
    // itself with no sharpening after it.
    const bool sharpen = settings_.sharpness > 0.0f;
    {
        const std::array<rhi::TextureBinding, 8> reads{
            rhi::TextureBinding{exposure, exact},
            rhi::TextureBinding{fsr2Masks_, filtered},
            rhi::TextureBinding{motion, exact},
            rhi::TextureBinding{fsr2History_[read], exact},
            rhi::TextureBinding{fsr2LockStatus_[read], filtered},
            rhi::TextureBinding{fsr2Prepared_, filtered},
            rhi::TextureBinding{fsr2Luminance_, filtered},
            rhi::TextureBinding{fsr2LumaHistory_[read], filtered},
        };
        const std::array<rhi::ComputeTextureWrite, 5> writes{
            rhi::ComputeTextureWrite{fsr2History_[write]}, rhi::ComputeTextureWrite{fsr2LockStatus_[write]},
            rhi::ComputeTextureWrite{fsr2NewLocks_},       rhi::ComputeTextureWrite{fsr2LumaHistory_[write]},
            rhi::ComputeTextureWrite{fsr2Output_},
        };
        pass(sharpen ? kFsr2AccumulateSharpen : kFsr2Accumulate, reads,
             std::span<const rhi::ComputeTextureWrite>{writes.data(), sharpen ? usize{4} : usize{5}}, outputWidth,
             outputHeight, 8);
    }
    if (sharpen) {
        // **AMD's own sharpening, on the scene before it is exposed**, with
        // the setting as it means it: 0 to 1, and two stops of attenuation at
        // nothing down to none at 1 (`FsrRcasCon`). The second word is the
        // same in halves, which only a build with them reads.
        GpuFsr2RcasConstants rcas;
        rcas.config[0] = bitsOf(std::exp2(-(2.0f - 2.0f * std::clamp(settings_.sharpness, 0.0f, 1.0f))));
        const std::array<rhi::TextureBinding, 2> reads{
            rhi::TextureBinding{exposure, exact},
            rhi::TextureBinding{fsr2History_[write], exact},
        };
        const std::array<rhi::ComputeTextureWrite, 1> writes{rhi::ComputeTextureWrite{fsr2Output_}};
        pass(kFsr2Rcas, reads, writes, outputWidth, outputHeight, 16, asBytes(&rcas, sizeof(rcas)));
    }
    cmd.popDebugGroup();

    fsr2Parity_ = write;
    fsr2Fresh_ = false;
    historyValid_ = true;
    rememberCamera(camera);
    return fsr2Output_;
}

void DefaultRenderer::resolvePicture(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target,
                                     AntiAliasingMode spatial, bool upscale, bool sharpen)
{
    const bool intoWindow = target.colorFormat != kLdrFormat;
    // The spatial pass writes the target when nothing comes after it.
    const bool last = !upscale && !sharpen;
    rhi::TextureHandle picture = ldr_;
    if (!last && !aaTexture(device, aaResolved_, renderWidth_, renderHeight_, kLdrFormat, "aa-resolved"))
        return;

    if (spatial == AntiAliasingMode::Fxaa) {
        GpuFxaaUniforms fxaa;
        // The SOURCE's texel, not the target's. FXAA walks an edge in the image
        // it is reading, and at a reduced render scale that image is smaller
        // than what it writes -- a step sized in output texels would look for
        // edges at the wrong spacing and find none.
        fxaa.texel[0] = 1.0f / static_cast<f32>(renderWidth_);
        fxaa.texel[1] = 1.0f / static_cast<f32>(renderHeight_);
        const std::array<rhi::TextureBinding, 1> ldrBinding{rhi::TextureBinding{ldr_, linearSampler_}};
        cmd.pushDebugGroup("fxaa");
        // A view's texture is `kLdrFormat` whatever the window's format is.
        const bool ldrFormat = !last || target.colorFormat != colorFormat_;
        const rhi::PipelineHandle fxaaPass = ldrFormat && fxaaViewPipeline_.valid() ? fxaaViewPipeline_ : fxaaPipeline_;
        if (last)
            fullscreenPass(cmd, fxaaPass, target.color, target.width, target.height, "fxaa", ldrBinding,
                           asBytes(&fxaa, sizeof(fxaa)));
        else
            fullscreenPass(cmd, fxaaPass, aaResolved_, renderWidth_, renderHeight_, "fxaa", ldrBinding,
                           asBytes(&fxaa, sizeof(fxaa)));
        cmd.popDebugGroup();
        picture = last ? rhi::TextureHandle{} : aaResolved_;
    }
    else if (spatial == AntiAliasingMode::Smaa) {
        // **A frame with sprites drawn in their own colours blends through the
        // twin that leaves them be** (ADR 0153).
        const bool exact = spriteExactLive_;
        LookPipeline& blendSlot = exact ? (last && intoWindow ? smaaBlendExactWindow_ : smaaBlendExact_)
                                        : (last && intoWindow ? smaaBlendWindow_ : smaaBlend_);
        const bool ready =
            ensureLookPipeline(device, smaaEdgesPipeline_, "smaa_edges", rhi::TextureFormat::Rg8Unorm) &&
            ensureLookPipeline(device, smaaWeightsPipeline_, "smaa_weights", rhi::TextureFormat::Rgba8Unorm) &&
            ensureLookPipeline(device, blendSlot, exact ? "smaa_blend_exact" : "smaa_blend",
                               last && intoWindow ? target.colorFormat : kLdrFormat) &&
            ensureSmaaTables(device, cmd) &&
            aaTexture(device, smaaEdges_, renderWidth_, renderHeight_, rhi::TextureFormat::Rg8Unorm, "smaa-edges") &&
            aaTexture(device, smaaWeights_, renderWidth_, renderHeight_, rhi::TextureFormat::Rgba8Unorm,
                      "smaa-weights");
        if (ready) {
            GpuSmaaUniforms smaa;
            smaa.metrics[0] = 1.0f / static_cast<f32>(renderWidth_);
            smaa.metrics[1] = 1.0f / static_cast<f32>(renderHeight_);
            smaa.metrics[2] = static_cast<f32>(renderWidth_);
            smaa.metrics[3] = static_cast<f32>(renderHeight_);
            const auto uniforms = asBytes(&smaa, sizeof(smaa));
            // **Linear and clamped, every one** -- its authors' requirement.
            const std::array<rhi::TextureBinding, 1> colour{rhi::TextureBinding{ldr_, environmentSampler_}};
            const std::array<rhi::TextureBinding, 3> tables{
                rhi::TextureBinding{smaaEdges_, environmentSampler_},
                rhi::TextureBinding{smaaArea_, environmentSampler_},
                rhi::TextureBinding{smaaSearch_, environmentSampler_},
            };
            const std::array<rhi::TextureBinding, 3> blendTextures{
                rhi::TextureBinding{ldr_, environmentSampler_}, rhi::TextureBinding{smaaWeights_, environmentSampler_},
                rhi::TextureBinding{exact ? spriteMask_ : blackPixel_, environmentSampler_}};
            const std::span<const rhi::TextureBinding> blend{blendTextures.data(), exact ? usize{3} : usize{2}};
            cmd.pushDebugGroup("smaa");
            fullscreenPass(cmd, smaaEdgesPipeline_.handle, smaaEdges_, renderWidth_, renderHeight_, "smaa-edges",
                           colour, uniforms);
            fullscreenPass(cmd, smaaWeightsPipeline_.handle, smaaWeights_, renderWidth_, renderHeight_, "smaa-weights",
                           tables, uniforms);
            if (last)
                fullscreenPass(cmd, blendSlot.handle, target.color, target.width, target.height, "smaa-blend", blend,
                               uniforms);
            else
                fullscreenPass(cmd, blendSlot.handle, aaResolved_, renderWidth_, renderHeight_, "smaa-blend", blend,
                               uniforms);
            cmd.popDebugGroup();
            picture = last ? rhi::TextureHandle{} : aaResolved_;
        }
        else if (last) {
            // Nothing to smooth with: the picture as it is, upscaled as the
            // tonemap upscales.
            const std::array<rhi::TextureBinding, 1> ldrBinding{rhi::TextureBinding{ldr_, linearSampler_}};
            GpuFxaaUniforms fxaa;
            fxaa.texel[0] = 1.0f / static_cast<f32>(renderWidth_);
            fxaa.texel[1] = 1.0f / static_cast<f32>(renderHeight_);
            const rhi::PipelineHandle fxaaPass =
                target.colorFormat != colorFormat_ && fxaaViewPipeline_.valid() ? fxaaViewPipeline_ : fxaaPipeline_;
            fullscreenPass(cmd, fxaaPass, target.color, target.width, target.height, "fxaa", ldrBinding,
                           asBytes(&fxaa, sizeof(fxaa)));
            return;
        }
    }
    if (last)
        return;

    LookPipeline& rcasSlot = intoWindow ? rcasWindow_ : rcas_;
    if (!ensureLookPipeline(device, rcasSlot, "fsr_rcas", target.colorFormat))
        return;
    // **RCAS's strength in stops**: 0 the most it sharpens, 2 the least we
    // ask of it -- `Sharpness` 1 and 0.
    GpuRcasUniforms rcas;
    rcas.con[0] = bitsOf(std::exp2(-2.0f * (1.0f - std::clamp(settings_.sharpness, 0.0f, 1.0f))));
    rhi::TextureHandle sharpened = picture;

    if (upscale) {
        if (upscaledWidth_ != target.width || upscaledHeight_ != target.height) {
            if (upscaled_.valid())
                device.destroy(upscaled_);
            upscaled_ = {};
            upscaledWidth_ = target.width;
            upscaledHeight_ = target.height;
        }
        if (!ensureLookPipeline(device, easu_, "fsr_easu", kLdrFormat) ||
            !aaTexture(device, upscaled_, target.width, target.height, kLdrFormat, "upscaled"))
            return;
        // `FsrEasuCon`, for a picture the size of the render into the target.
        const f32 inWidth = static_cast<f32>(renderWidth_);
        const f32 inHeight = static_cast<f32>(renderHeight_);
        const f32 outWidth = static_cast<f32>(target.width);
        const f32 outHeight = static_cast<f32>(target.height);
        GpuEasuUniforms easu;
        easu.con[0][0] = bitsOf(inWidth / outWidth);
        easu.con[0][1] = bitsOf(inHeight / outHeight);
        easu.con[0][2] = bitsOf(0.5f * inWidth / outWidth - 0.5f);
        easu.con[0][3] = bitsOf(0.5f * inHeight / outHeight - 0.5f);
        easu.con[1][0] = bitsOf(1.0f / inWidth);
        easu.con[1][1] = bitsOf(1.0f / inHeight);
        easu.con[1][2] = bitsOf(1.0f / inWidth);
        easu.con[1][3] = bitsOf(-1.0f / inHeight);
        easu.con[2][0] = bitsOf(-1.0f / inWidth);
        easu.con[2][1] = bitsOf(2.0f / inHeight);
        easu.con[2][2] = bitsOf(1.0f / inWidth);
        easu.con[2][3] = bitsOf(2.0f / inHeight);
        easu.con[3][0] = bitsOf(0.0f);
        easu.con[3][1] = bitsOf(4.0f / inHeight);
        // With the sprites' mask, which the upscale takes the nearest texel
        // under (ADR 0158): pixel art is never filtered.
        const std::array<rhi::TextureBinding, 2> source{
            rhi::TextureBinding{picture, environmentSampler_},
            rhi::TextureBinding{spriteExactLive_ ? spriteMask_ : blackPixel_, pointSampler_}};
        cmd.pushDebugGroup("fsr1");
        fullscreenPass(cmd, easu_.handle, upscaled_, target.width, target.height, "fsr-easu", source,
                       asBytes(&easu, sizeof(easu)));
        cmd.popDebugGroup();
        sharpened = upscaled_;
    }
    // And under the mask, the picture unsharpened.
    const std::array<rhi::TextureBinding, 2> source{
        rhi::TextureBinding{sharpened, pointSampler_},
        rhi::TextureBinding{spriteExactLive_ ? spriteMask_ : blackPixel_, pointSampler_}};
    cmd.pushDebugGroup("rcas");
    fullscreenPass(cmd, rcasSlot.handle, target.color, target.width, target.height, "fsr-rcas", source,
                   asBytes(&rcas, sizeof(rcas)));
    cmd.popDebugGroup();
}

void DefaultRenderer::releaseScreenLight(rhi::IDevice& device)
{
    for (rhi::TextureHandle* texture :
         {&sceneBefore_, &screenReflection_, &screenReflectionBefore_, &screenBounce_, &screenBounceBefore_}) {
        if (texture->valid())
            device.destroy(*texture);
        *texture = {};
    }
    sceneBeforeValid_ = false;
    screenReflectionReady_ = false;
    screenBounceReady_ = false;
    screenBounceBeforeValid_ = false;
    screenReflectionBeforeValid_ = false;
}

void DefaultRenderer::traceScreen(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world)
{
    screenReflectionReady_ = false;
    screenBounceReady_ = false;
    const bool wanted = settings_.screenReflectionSteps > 0 || settings_.screenBounceRays > 0;
    if (!wanted) {
        // **Off is nothing kept** (ADR 0209): a scene without it is the
        // frame it was before there was any.
        if (sceneBefore_.valid() || screenReflection_.valid() || screenBounce_.valid())
            releaseScreenLight(device);
        return;
    }
    // A position is rebuilt from depth as a perspective camera made it: under
    // an orthographic one (the 2D layer) the pass is off rather than wrong.
    if (!world.camera.valid || core::isOrthographic(world.camera.projection)) {
        screenBounceBeforeValid_ = false;
        screenReflectionBeforeValid_ = false;
        return;
    }
    const u32 width = renderWidth_ > 1 ? renderWidth_ / 2 : 1;
    const u32 height = renderHeight_ > 1 ? renderHeight_ / 2 : 1;
    // **Made on the frame that first wants them**, which has no last frame
    // to read and so runs neither pass: a run's first frame is where
    // everything it draws with is made, and the frames after it are alike
    // (`clock_differential` holds them to that).
    const bool reflecting =
        settings_.screenReflectionSteps > 0 && ensureLookPipeline(device, screenTrace_, "screen_trace", kHdrFormat);
    const bool bouncing = settings_.screenBounceRays > 0 &&
                          ensureLookPipeline(device, screenBouncePass_, "screen_bounce", kHdrFormat) &&
                          lookTexture(device, screenBounce_, width, height, "screen-bounce") &&
                          lookTexture(device, screenBounceBefore_, width, height, "screen-bounce-before");
    if (reflecting && (!screenReflection_.valid() || !screenReflectionBefore_.valid())) {
        // A chain down to a few pixels: a rougher surface reads further down.
        u32 levels = 1;
        for (u32 side = std::min(width, height); side > 8 && levels < 6; side /= 2)
            ++levels;
        screenReflectionLevels_ = levels;
        for (rhi::TextureHandle* picture : {&screenReflection_, &screenReflectionBefore_}) {
            if (picture->valid())
                continue;
            *picture = device.createTexture({
                .format = kHdrFormat,
                .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
                .width = width,
                .height = height,
                .mipLevels = levels,
                .debugName = "screen-reflection",
            });
        }
        screenReflectionBeforeValid_ = false;
    }
    if (!sceneBeforeValid_) {
        screenBounceBeforeValid_ = false;
        screenReflectionBeforeValid_ = false;
        return;
    }

    GpuScreenTraceUniforms trace;
    trace.projection[0] = world.camera.projection.m[0][0] != 0.0f ? 1.0f / world.camera.projection.m[0][0] : 1.0f;
    trace.projection[1] = world.camera.projection.m[1][1] != 0.0f ? 1.0f / world.camera.projection.m[1][1] : 1.0f;
    trace.projection[2] = world.camera.nearPlane;
    trace.projection[3] = world.camera.farPlane;
    // **The view's middle off its axis** (an eye's projection): rebuilt
    // positions and the pixels they land on are wrong by it otherwise.
    trace.center[0] = world.camera.projection.m[2][0];
    trace.center[1] = world.camera.projection.m[2][1];
    // This frame's view space, to the world as this frame's camera has it at
    // its origin, to the last frame's -- whose origin was the camera then --
    // and through the camera that drew it.
    const Vec3 moved{static_cast<f32>(world.camera.origin.x - sceneBeforeOrigin_.x),
                     static_cast<f32>(world.camera.origin.y - sceneBeforeOrigin_.y),
                     static_cast<f32>(world.camera.origin.z - sceneBeforeOrigin_.z)};
    trace.toLast = sceneBeforeViewProjection_ * core::translation(moved) * core::inverse(world.camera.view);
    trace.params[0] = kScreenReflectionReachMetres;
    trace.params[1] = kScreenReflectionThicknessMetres;
    trace.params[2] = static_cast<f32>(settings_.screenReflectionSteps);
    trace.frame[0] = 1.0f;
    // A start that differs every frame, gathered by the pass's own answer of
    // the frame before -- for longer while the camera is where it was, when
    // the answers are of the same rays; under a camera that moves a
    // reflection moves as nothing else on its surface does, and an old
    // answer is a smear.
    trace.frame[1] = static_cast<f32>(screenFrame_ % 64u);
    const bool still = std::memcmp(&screenLastCamera_, &world.camera.unjitteredViewProjection, sizeof(Mat4)) == 0 &&
                       moved.x == 0.0f && moved.y == 0.0f && moved.z == 0.0f;
    screenLastCamera_ = world.camera.unjitteredViewProjection;
    trace.frame[2] = screenReflectionBeforeValid_ ? 1.0f : 0.0f;
    if (!screenReflectionBeforeValid_)
        screenReflectionGathered_ = 0;
    if (!screenBounceBeforeValid_)
        screenBounceGathered_ = 0;
    const auto gathered = [](u32 frames, f32 most) {
        return std::min(most, static_cast<f32>(frames) / static_cast<f32>(frames + 1u));
    };
    trace.frame[3] =
        gathered(screenReflectionGathered_, still ? kScreenReflectionStillWeight : kScreenReflectionMovingWeight);

    cmd.pushDebugGroup("screen-light");
    if (reflecting && screenReflection_.valid() && screenReflectionBefore_.valid()) {
        // The two pictures change places: the one written is never the one read.
        std::swap(screenReflection_, screenReflectionBefore_);
        const std::array<rhi::TextureBinding, 3> reads{
            rhi::TextureBinding{depth_, pointSampler_},
            rhi::TextureBinding{sceneBefore_, linearSampler_},
            rhi::TextureBinding{screenReflectionBefore_, linearSampler_},
        };
        fullscreenPass(cmd, screenTrace_.handle, screenReflection_, width, height, "screen-trace", reads,
                       asBytes(&trace, sizeof(trace)));
        if (screenReflectionLevels_ > 1)
            cmd.generateMipmaps(screenReflection_);
        screenReflectionReady_ = true;
        screenReflectionBeforeValid_ = true;
        screenReflectionGathered_ = std::min(screenReflectionGathered_ + 1u, 1024u);
    }
    else {
        screenReflectionBeforeValid_ = false;
    }
    // **And the light that bounces** (`screen_bounce.hlsl`): a few short rays
    // a pixel, gathered with this pass's answer of the frame before. The two
    // pictures change places, so the one written is never the one read.
    if (bouncing) {
        std::swap(screenBounce_, screenBounceBefore_);
        GpuScreenTraceUniforms bounce = trace;
        // Its own answer of the frame before, by its own weight.
        bounce.params[0] = kScreenBounceReachMetres;
        bounce.params[1] = kScreenBounceThicknessMetres;
        bounce.params[2] = static_cast<f32>(settings_.screenBounceRays);
        // Every frame a different turn: it is gathered over frames whatever
        // the anti-aliasing is, by its own history.
        bounce.frame[1] = static_cast<f32>(screenFrame_ % 64u);
        bounce.frame[2] = screenBounceBeforeValid_ ? 1.0f : 0.0f;
        bounce.frame[3] =
            gathered(screenBounceGathered_, still ? kScreenBounceStillWeight : kScreenBounceHistoryWeight);
        const std::array<rhi::TextureBinding, 3> reads{
            rhi::TextureBinding{depth_, pointSampler_},
            rhi::TextureBinding{sceneBefore_, linearSampler_},
            rhi::TextureBinding{screenBounceBefore_, linearSampler_},
        };
        fullscreenPass(cmd, screenBouncePass_.handle, screenBounce_, width, height, "screen-bounce", reads,
                       asBytes(&bounce, sizeof(bounce)));
        screenBounceReady_ = true;
        screenBounceBeforeValid_ = true;
        screenBounceGathered_ = std::min(screenBounceGathered_ + 1u, 1024u);
    }
    else {
        screenBounceBeforeValid_ = false;
    }
    cmd.popDebugGroup();
}

void DefaultRenderer::keepSceneBefore(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world)
{
    if (settings_.screenReflectionSteps == 0 && settings_.screenBounceRays == 0)
        return;
    if (!world.camera.valid || !ensureLookPipeline(device, resample_, "look_resample", kHdrFormat) ||
        !lookTexture(device, sceneBefore_, renderWidth_, renderHeight_, "scene-before")) {
        sceneBeforeValid_ = false;
        return;
    }
    const std::array<rhi::TextureBinding, 1> color{rhi::TextureBinding{hdr_, linearSampler_}};
    fullscreenPass(cmd, resample_.handle, sceneBefore_, renderWidth_, renderHeight_, "scene-before", color, {});
    sceneBeforeViewProjection_ = world.camera.viewProjection;
    sceneBeforeOrigin_ = world.camera.origin;
    sceneBeforeValid_ = true;
    ++screenFrame_;
}

void DefaultRenderer::releaseLookTextures(rhi::IDevice& device)
{
    const auto release = [&device](rhi::TextureHandle& texture) {
        if (texture.valid())
            device.destroy(texture);
        texture = {};
    };
    release(lookColor_);
    release(sceneDepthCopy_);
    release(sceneColorCopy_);
    // The targets are another size now: what was kept of the last frame is
    // not this view's any more.
    releaseScreenLight(device);
    for (rhi::TextureHandle& level : blurLevels_)
        release(level);
    for (rhi::TextureHandle& level : blurPong_)
        release(level);
    release(focusPrepared_);
    release(focusGathered_);
    release(raysMasked_);
    release(raysGathered_);
}

void DefaultRenderer::sunRaysOnto(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                  const SkyParams& sky, rhi::TextureHandle image)
{
    // **Where the sun is on the screen**: its direction projected as a point at
    // infinity -- w of zero, so only the view's rotation and the lens act on it.
    // Behind the camera there is nothing to stream from.
    const Mat4& viewProjection = world.camera.viewProjection;
    const Vec3 sun = sky.sunDirection;
    f32 clip[4]{};
    for (u32 row = 0; row < 4; ++row) {
        clip[row] =
            viewProjection.m[0][row] * sun.x + viewProjection.m[1][row] * sun.y + viewProjection.m[2][row] * sun.z;
    }
    if (!(clip[3] > 1e-4f))
        return;
    const f32 ndcX = clip[0] / clip[3];
    const f32 ndcY = clip[1] / clip[3];

    // **Present only while the sun is.** Past the screen's edge the shafts fade
    // over `kRaysEdgeFade` half-widths, and below the horizon they go with the
    // day -- so a sunset takes its rays with it rather than cutting them off.
    const f32 outside = std::max(std::abs(ndcX), std::abs(ndcY)) - 1.0f;
    const f32 onScreen = std::clamp(1.0f - outside / kRaysEdgeFade, 0.0f, 1.0f);
    const f32 presence = onScreen * sky.dayFactor * world.look.sunRaysIntensity * kRaysStrength;
    if (!(presence > 0.0f))
        return;

    if (!ensureLookPipeline(device, raysMask_, "look_rays_mask", kHdrFormat) ||
        !ensureLookPipeline(device, raysGather_, "look_rays_gather", kHdrFormat) ||
        !ensureLookPipeline(device, raysAdd_, "look_resample", kHdrFormat, LookBlend::Add))
        return;
    // Half resolution: at a quarter, a post or a branch in front of the sun was
    // a texel or two of the mask, and its shaft drowned in the glow around it.
    const u32 raysWidth = std::max(renderWidth_ / 2, 1u);
    const u32 raysHeight = std::max(renderHeight_ / 2, 1u);
    if (!lookTexture(device, raysMasked_, raysWidth, raysHeight, "look-rays-mask") ||
        !lookTexture(device, raysGathered_, raysWidth, raysHeight, "look-rays"))
        return;

    GpuLookRaysUniforms rays;
    rays.sun[0] = ndcX * 0.5f + 0.5f;
    rays.sun[1] = 0.5f - ndcY * 0.5f;
    rays.sun[2] = presence;
    rays.sun[3] = static_cast<f32>(renderWidth_) / static_cast<f32>(renderHeight_);
    // `Spread` is how much of the way to the sun a texel looks: a quarter at
    // 0, a halo round the sun; all of it at 1, shafts across the screen.
    rays.gather[0] = 0.25f + 0.75f * world.look.sunRaysSpread;
    rays.gather[1] = static_cast<f32>(kRaysTaps);
    rays.gather[2] = kRaysDecay;
    rays.gather[3] = raysLobe(world.look.sunRaysSpread);

    cmd.pushDebugGroup("sun-rays");
    const std::array<rhi::TextureBinding, 2> mask{rhi::TextureBinding{image, environmentSampler_},
                                                  rhi::TextureBinding{depth_, pointSampler_}};
    fullscreenPass(cmd, raysMask_.handle, raysMasked_, raysWidth, raysHeight, "rays-mask", mask,
                   asBytes(&rays, sizeof(rays)));
    const std::array<rhi::TextureBinding, 1> gather{rhi::TextureBinding{raysMasked_, environmentSampler_}};
    fullscreenPass(cmd, raysGather_.handle, raysGathered_, raysWidth, raysHeight, "rays-gather", gather,
                   asBytes(&rays, sizeof(rays)));
    const std::array<rhi::TextureBinding, 1> shafts{rhi::TextureBinding{raysGathered_, environmentSampler_}};
    fullscreenPass(cmd, raysAdd_.handle, image, renderWidth_, renderHeight_, "rays-add", shafts, {}, rhi::LoadOp::Load);
    cmd.popDebugGroup();
}

rhi::TextureHandle DefaultRenderer::focusImage(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                               rhi::TextureHandle image)
{
    if (!ensureLookPipeline(device, focusPrepare_, "look_focus_prepare", kHdrFormat) ||
        !ensureLookPipeline(device, focusGather_, "look_focus_gather", kHdrFormat) ||
        !ensureLookPipeline(device, focusComposite_, "look_focus_composite", kHdrFormat))
        return image;
    const u32 halfWidth = renderWidth_ > 1 ? renderWidth_ / 2 : 1;
    const u32 halfHeight = renderHeight_ > 1 ? renderHeight_ / 2 : 1;
    rhi::TextureHandle& output = image == lookColor_ ? hdr_ : lookColor_;
    if (!lookTexture(device, focusPrepared_, halfWidth, halfHeight, "look-focus") ||
        !lookTexture(device, focusGathered_, halfWidth, halfHeight, "look-focus-gathered") ||
        !lookTexture(device, output, renderWidth_, renderHeight_, "look-color"))
        return image;

    const RenderLook& look = world.look;
    const f32 widest = kFocusWidestPixels * static_cast<f32>(renderHeight_) / 1080.0f;
    GpuLookFocusUniforms focus;
    focus.band[0] = look.focusDistance;
    focus.band[1] = look.inFocusRadius;
    focus.band[2] = look.nearIntensity;
    focus.band[3] = look.farIntensity;
    focus.lens[0] = world.camera.nearPlane;
    focus.lens[1] = world.camera.farPlane;
    focus.lens[2] = widest * 0.5f;
    focus.lens[3] = widest;
    focus.texel[0] = 1.0f / static_cast<f32>(renderWidth_);
    focus.texel[1] = 1.0f / static_cast<f32>(renderHeight_);
    focus.texel[2] = 1.0f / static_cast<f32>(halfWidth);
    focus.texel[3] = 1.0f / static_cast<f32>(halfHeight);

    cmd.pushDebugGroup("depth-of-field");
    const std::array<rhi::TextureBinding, 2> prepare{rhi::TextureBinding{image, environmentSampler_},
                                                     rhi::TextureBinding{depth_, pointSampler_}};
    fullscreenPass(cmd, focusPrepare_.handle, focusPrepared_, halfWidth, halfHeight, "focus-prepare", prepare,
                   asBytes(&focus, sizeof(focus)));
    // Point-sampled: a circle of confusion averaged with its neighbour's is a
    // circle nobody's depth has.
    const std::array<rhi::TextureBinding, 1> gather{rhi::TextureBinding{focusPrepared_, pointSampler_}};
    fullscreenPass(cmd, focusGather_.handle, focusGathered_, halfWidth, halfHeight, "focus-gather", gather,
                   asBytes(&focus, sizeof(focus)));
    const std::array<rhi::TextureBinding, 3> composite{rhi::TextureBinding{image, pointSampler_},
                                                       rhi::TextureBinding{focusGathered_, environmentSampler_},
                                                       rhi::TextureBinding{depth_, pointSampler_}};
    fullscreenPass(cmd, focusComposite_.handle, output, renderWidth_, renderHeight_, "focus-composite", composite,
                   asBytes(&focus, sizeof(focus)));
    cmd.popDebugGroup();
    return output;
}

void DefaultRenderer::blurImage(rhi::IDevice& device, rhi::ICmdList& cmd, rhi::TextureHandle image, f32 size)
{
    // `Size` is where most of a pixel's light lands, which for a Gaussian is two
    // standard deviations -- in pixels of a picture 1,080 lines tall, so a blur
    // looks the same at every window size.
    const f32 sigma = 0.5f * size * static_cast<f32>(renderHeight_) / 1080.0f;
    // Below a quarter of a pixel the kernel's side taps weigh nothing.
    if (!(sigma > 0.25f))
        return;
    if (!ensureLookPipeline(device, blur_, "look_blur", kHdrFormat) ||
        !ensureLookPipeline(device, resample_, "look_resample", kHdrFormat))
        return;

    // Down the chain until the Gaussian is a few texels wide there.
    u32 levels = 0;
    f32 levelSigma = sigma;
    while (levelSigma > kLookBlurLevelSigma && levels < kLookBlurLevels) {
        levelSigma *= 0.5f;
        ++levels;
    }

    cmd.pushDebugGroup("blur");
    rhi::TextureHandle current = image;
    u32 width = renderWidth_;
    u32 height = renderHeight_;
    for (u32 level = 0; level < levels; ++level) {
        const u32 levelWidth = bloomLevelSize(renderWidth_, level);
        const u32 levelHeight = bloomLevelSize(renderHeight_, level);
        if (!lookTexture(device, blurLevels_[level], levelWidth, levelHeight, "look-blur")) {
            cmd.popDebugGroup();
            return;
        }
        // The bloom chain's own downsample, with no threshold: a thirteen-tap
        // box that does not alias the way one bilinear read per texel would.
        GpuBloomUniforms down;
        down.texelRadius[0] = 1.0f / static_cast<f32>(width);
        down.texelRadius[1] = 1.0f / static_cast<f32>(height);
        down.texelRadius[2] = 1.0f;
        const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{current, environmentSampler_}};
        fullscreenPass(cmd, bloomDownPipeline_, blurLevels_[level], levelWidth, levelHeight, "blur-down", source,
                       asBytes(&down, sizeof(down)));
        current = blurLevels_[level];
        width = levelWidth;
        height = levelHeight;
    }

    // The partner the two directions ping-pong through: the level's own, or at
    // full size whichever full-resolution image `image` is not.
    rhi::TextureHandle& partner = levels > 0 ? blurPong_[levels - 1] : (image == lookColor_ ? hdr_ : lookColor_);
    if (!lookTexture(device, partner, width, height, "look-blur-pong")) {
        cmd.popDebugGroup();
        return;
    }
    GpuLookBlurUniforms pass;
    pass.stepSigma[2] = levelSigma;
    pass.stepSigma[3] = std::min(std::ceil(3.0f * levelSigma), 16.0f);
    pass.stepSigma[0] = 1.0f / static_cast<f32>(width);
    const std::array<rhi::TextureBinding, 1> across{rhi::TextureBinding{current, environmentSampler_}};
    fullscreenPass(cmd, blur_.handle, partner, width, height, "blur-x", across, asBytes(&pass, sizeof(pass)));
    pass.stepSigma[0] = 0.0f;
    pass.stepSigma[1] = 1.0f / static_cast<f32>(height);
    const std::array<rhi::TextureBinding, 1> vertical{rhi::TextureBinding{partner, environmentSampler_}};
    fullscreenPass(cmd, blur_.handle, current, width, height, "blur-y", vertical, asBytes(&pass, sizeof(pass)));

    // And back to the frame's size, into the image it came from.
    if (levels > 0) {
        const std::array<rhi::TextureBinding, 1> result{rhi::TextureBinding{current, environmentSampler_}};
        fullscreenPass(cmd, resample_.handle, image, renderWidth_, renderHeight_, "blur-up", result, {});
    }
    cmd.popDebugGroup();
}

bool DefaultRenderer::ensureSkyLook(rhi::IDevice& device)
{
    ENG_PROFILE_SCOPE("prepare.sky_pipeline");
    if (skyLook_.tried)
        return skyLook_.handle.valid();
    skyLook_.tried = true;
    if (shaderLibrary_ == nullptr)
        return false;
    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "sky_look", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "sky_look", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    const std::array<rhi::ColorTargetDesc, 1> target{rhi::ColorTargetDesc{.format = kHdrFormat}};
    skyLook_.handle = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = target,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sky_look",
    });
    // And for a view seen on one side of a line, as `sky_sided` is.
    skyLookSided_.tried = true;
    skyLookSided_.handle = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .primitive = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::NotEqual},
        .colorTargets = target,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sky_look_sided",
    });
    return skyLook_.handle.valid();
}

bool DefaultRenderer::ensureMorph(rhi::IDevice& device)
{
    if (morphTried_)
        return morphReady_;
    // Timed by name: what a frame statistic shows the making of these cost,
    // the once it happens.
    ENG_PROFILE_SCOPE("prepare.morph_pipelines");
    morphTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };

    // The layouts of the passes these stand in for (`create`): the mesh at
    // slot 0, and for a skinned one its joints and weights at slot 1.
    const std::array<rhi::VertexBufferLayout, 1> plainBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };
    const std::array<rhi::VertexBufferLayout, 2> skinnedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
    };
    const std::array<rhi::VertexAttribute, 4> plainForward{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexAttribute, 6> skinnedForward{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    // Depth only: the position, and a skinned mesh's joints and weights.
    const std::array<rhi::VertexAttribute, 1> plainDepth{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
    };
    const std::array<rhi::VertexAttribute, 3> skinnedDepth{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};
    const std::array<rhi::ColorTargetDesc, 1> hdrBlendTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true},
    }};

    struct Family
    {
        const char* forwardShader;
        const char* depthShader;
        std::span<const rhi::VertexBufferLayout> buffers;
        std::span<const rhi::VertexAttribute> forwardAttributes;
        std::span<const rhi::VertexAttribute> depthAttributes;
        // The five names, for a capture of the frame to tell them apart by.
        std::array<const char*, 5> names;
    };
    const std::array<Family, 2> families{
        Family{"pbr_morph",
               "shadow_morph",
               plainBuffers,
               plainForward,
               plainDepth,
               {"pbr_morph", "pbr_morph_prepassed", "pbr_morph_blend", "shadow_morph", "depth_prepass_morph"}},
        Family{"pbr_morph_skinned",
               "shadow_morph_skinned",
               skinnedBuffers,
               skinnedForward,
               skinnedDepth,
               {"pbr_morph_skinned", "pbr_morph_skinned_prepassed", "pbr_morph_skinned_blend", "shadow_morph_skinned",
                "depth_prepass_morph_skinned"}},
    };

    bool ready = true;
    for (core::usize index = 0; index < families.size(); ++index) {
        const Family& family = families[index];
        const rhi::ShaderHandle forwardVertex = load(family.forwardShader, rhi::ShaderStage::Vertex);
        const rhi::ShaderHandle forwardFragment = load(family.forwardShader, rhi::ShaderStage::Fragment);
        const rhi::ShaderHandle depthVertex = load(family.depthShader, rhi::ShaderStage::Vertex);
        const rhi::ShaderHandle depthFragment = load(family.depthShader, rhi::ShaderStage::Fragment);
        if (!forwardVertex.valid() || !forwardFragment.valid() || !depthVertex.valid() || !depthFragment.valid()) {
            // Content exported before these shaders were: its bodies are
            // drawn at rest, and the log says why.
            core::logText(core::LogLevel::Warn, error.message);
            return false;
        }

        // Each the state of the pass it stands in for, word for word: the
        // lit pass, the lit pass over a prepass, the blended pass, a shadow
        // map (front faces culled) and the camera's own depth.
        MorphPipelines& set = morphPipelines_[index];
        set.forward = device.createGraphicsPipeline({
            .vertexShader = forwardVertex,
            .fragmentShader = forwardFragment,
            .vertexBuffers = family.buffers,
            .vertexAttributes = family.forwardAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = family.names[0],
        });
        set.forwardPrepassed = device.createGraphicsPipeline({
            .vertexShader = forwardVertex,
            .fragmentShader = forwardFragment,
            .vertexBuffers = family.buffers,
            .vertexAttributes = family.forwardAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = hdrTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = family.names[1],
        });
        set.blend = device.createGraphicsPipeline({
            .vertexShader = forwardVertex,
            .fragmentShader = forwardFragment,
            .vertexBuffers = family.buffers,
            .vertexAttributes = family.forwardAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = hdrBlendTarget,
            .depthStencilFormat = kDepthFormat,
            .debugName = family.names[2],
        });
        set.shadow = device.createGraphicsPipeline({
            .vertexShader = depthVertex,
            .fragmentShader = depthFragment,
            .vertexBuffers = family.buffers,
            .vertexAttributes = family.depthAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Front},
            .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = {},
            .depthStencilFormat = kShadowFormat,
            .debugName = family.names[3],
        });
        set.prepass = device.createGraphicsPipeline({
            .vertexShader = depthVertex,
            .fragmentShader = depthFragment,
            .vertexBuffers = family.buffers,
            .vertexAttributes = family.depthAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
            .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = {},
            .depthStencilFormat = kDepthFormat,
            .debugName = family.names[4],
        });
        ready = ready && set.forward.valid() && set.forwardPrepassed.valid() && set.blend.valid() &&
                set.shadow.valid() && set.prepass.valid();
    }
    morphReady_ = ready;
    return morphReady_;
}

bool DefaultRenderer::ensureSkinnedInstancing(rhi::IDevice& device)
{
    if (skinnedInstancingTried_)
        return pbrSkinnedInstancedPipeline_.valid() && shadowSkinnedInstancedPipeline_.valid() &&
               depthPrepassSkinnedInstancedPipeline_.valid() && skinnedInstanceBuffer_.valid() &&
               paletteBuffer_.valid();
    skinnedInstancingTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle forwardVertex = load("pbr_skinned_instanced", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle forwardFragment = load("pbr_skinned_instanced", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle depthVertex = load("shadow_skinned_instanced", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle depthFragment = load("shadow_skinned_instanced", rhi::ShaderStage::Fragment);
    if (!forwardVertex.valid() || !forwardFragment.valid() || !depthVertex.valid() || !depthFragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // The mesh at slot 0, the skin stream at slot 1 and the instances at slot
    // 2: `skinnedBuffers` and `instancedBuffers` (in `create`) side by side.
    const std::array<rhi::VertexBufferLayout, 3> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
        rhi::VertexBufferLayout{.slot = 2, .strideBytes = sizeof(GpuSkinnedInstance), .perInstance = true},
    };
    const std::array<rhi::VertexAttribute, 12> forwardAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 6, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 7, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 8, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 9, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 10, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 64},
        rhi::VertexAttribute{.location = 11, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 80},
    };
    // Depth only: position, the skin stream, the model and the palette start.
    const std::array<rhi::VertexAttribute, 8> depthAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 5, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 6, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 7, .bufferSlot = 2, .format = rhi::VertexFormat::Float4, .offsetBytes = 80},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};

    pbrSkinnedInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = forwardVertex,
        .fragmentShader = forwardFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = forwardAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_skinned_instanced",
    });
    pbrSkinnedInstancedPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = forwardVertex,
        .fragmentShader = forwardFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = forwardAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "pbr_skinned_instanced_prepassed",
    });
    shadowSkinnedInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = depthVertex,
        .fragmentShader = depthFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = depthAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Front},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "shadow_skinned_instanced",
    });
    depthPrepassSkinnedInstancedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = depthVertex,
        .fragmentShader = depthFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = depthAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "depth_prepass_skinned_instanced",
    });
    skinnedInstanceBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = kMaxSkinnedInstances * static_cast<u32>(sizeof(GpuSkinnedInstance)),
        .debugName = "skinned-instances",
    });
    (void)ensureSkinPalette(device);
    return pbrSkinnedInstancedPipeline_.valid() && shadowSkinnedInstancedPipeline_.valid() &&
           depthPrepassSkinnedInstancedPipeline_.valid() && skinnedInstanceBuffer_.valid() && paletteBuffer_.valid();
}

bool DefaultRenderer::ensureSkinPalette(rhi::IDevice& device)
{
    if (!paletteBuffer_.valid()) {
        paletteBuffer_ = device.createBuffer({
            .usage = rhi::BufferUsage::GraphicsStorageRead,
            .sizeBytes = kMaxPaletteJoints * static_cast<u32>(sizeof(Mat4)),
            .debugName = "skin-palettes",
        });
    }
    return paletteBuffer_.valid();
}

bool DefaultRenderer::ensureParticles(rhi::IDevice& device)
{
    if (particleTried_)
        return particlePipeline_.valid() && particleBuffer_.valid();
    particleTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("particle", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("particle", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // **Instances only**: the six corners come from the vertex index, so the
    // one stream is per instance and there is no per-vertex buffer at all.
    const std::array<rhi::VertexAttribute, 5> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
        rhi::VertexAttribute{.location = 4, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 64},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(GpuParticle), .perInstance = true},
    };
    // Premultiplied, which is what lets one pipeline blend and add: see the
    // shader's header.
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .srcAlpha = rhi::BlendFactor::One,
                  .dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha},
    }};
    particlePipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        // No depth attachment: the shader reads the scene's depth, tests
        // against it and fades near it (soft particles), and never writes it.
        .colorTargets = hdrTarget,
        .debugName = "particle",
    });
    particleBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(ParticleSystem::MaxDrawn * sizeof(GpuParticle)),
        .debugName = "particles",
    });
    return particlePipeline_.valid() && particleBuffer_.valid();
}

bool DefaultRenderer::ensureGpuParticles(rhi::IDevice& device)
{
    if (particleGpuTried_)
        return particleSimPipeline_.valid() && particleGpuPipeline_.valid();
    particleGpuTried_ = true;
    if (shaderLibrary_ == nullptr || !device.caps().compute)
        return false;
    core::EngineError error;
    particleSimPipeline_ = shaderLibrary_->createCompute(device, "particle_sim", &error);
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("particle_gpu", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("particle_gpu", rhi::ShaderStage::Fragment);
    if (!particleSimPipeline_.valid() || !vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    // Premultiplied, as the CPU's particles are, and with no vertex stream at
    // all: a particle is a slot of the buffer, found by its instance number.
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .srcAlpha = rhi::BlendFactor::One,
                  .dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha},
    }};
    particleGpuPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = hdrTarget,
        .debugName = "particle-gpu",
    });
    return particleSimPipeline_.valid() && particleGpuPipeline_.valid();
}

void DefaultRenderer::releaseGpuParticles(rhi::IDevice& device)
{
    for (auto& [key, held] : gpuEmitters_) {
        if (held.buffer.valid())
            device.destroy(held.buffer);
    }
    gpuEmitters_.clear();
    if (particleGround_.valid())
        device.destroy(particleGround_);
    particleGround_ = {};
    particleGroundRevision_ = 0;
    particleGroundCells_ = 0;
    if (particleSimPipeline_.valid())
        device.destroy(particleSimPipeline_);
    particleSimPipeline_ = {};
    if (particleGpuPipeline_.valid())
        device.destroy(particleGpuPipeline_);
    particleGpuPipeline_ = {};
    particleGpuTried_ = false;
    simPrevValid_ = false;
}

void DefaultRenderer::simulateGpuParticles(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world)
{
    // The camera the depth still in `depth_` was drawn through, and this
    // frame's for the next.
    const Mat4 prevViewProjection = simPrevViewProjection_;
    const core::DVec3 prevOrigin = simPrevOrigin_;
    const f32 prevNear = simPrevDepth_[0];
    const f32 prevFar = simPrevDepth_[1];
    const bool prevValid = simPrevValid_ && depth_.valid();
    simPrevViewProjection_ = world.camera.viewProjection;
    simPrevOrigin_ = world.camera.origin;
    simPrevDepth_[0] = core::isOrthographic(world.camera.projection) ? -world.camera.nearPlane : world.camera.nearPlane;
    simPrevDepth_[1] = world.camera.farPlane;
    simPrevValid_ = world.camera.valid;

    gpuParticleFrame_ += 1;
    if (!world.gpuEmitters.empty() && ensureGpuParticles(device)) {
        // The ground's heights, when the particle system has made them again.
        const RenderParticleGround* ground = world.particleGround;
        if (ground != nullptr && ground->cells > 0 && ground->revision != particleGroundRevision_ &&
            ground->heights.size() == static_cast<usize>(ground->cells) * ground->cells) {
            if (!particleGround_.valid() || particleGroundCells_ != ground->cells) {
                if (particleGround_.valid())
                    device.destroy(particleGround_);
                particleGround_ = device.createTexture({
                    .format = rhi::TextureFormat::R32Float,
                    .usage = rhi::TextureUsage::Sampled,
                    .width = ground->cells,
                    .height = ground->cells,
                    .debugName = "particle-ground",
                });
                particleGroundCells_ = ground->cells;
            }
            if (particleGround_.valid()) {
                cmd.uploadTexture(particleGround_,
                                  asBytes(ground->heights.data(), ground->heights.size() * sizeof(f32)), 0);
                particleGroundRevision_ = ground->revision;
            }
        }
        const bool groundThere = ground != nullptr && particleGround_.valid() && ground->side > 0.0f;

        // Each emitter's buffer, made or grown before any pass opens: a grown
        // one starts again from nothing, as a new one does.
        for (const RenderGpuEmitter& emitter : world.gpuEmitters) {
            GpuEmitterBuffer& held = gpuEmitters_[(static_cast<u64>(emitter.id.index) << 32) | emitter.id.generation];
            held.frame = gpuParticleFrame_;
            if (held.buffer.valid() && held.capacity >= emitter.capacity)
                continue;
            if (held.buffer.valid())
                device.destroy(held.buffer);
            held.capacity = emitter.capacity;
            held.head = 0;
            held.serial = 0;
            held.origin = emitter.frame.position;
            held.buffer = device.createBuffer({
                .usage = rhi::BufferUsage::GraphicsStorageRead | rhi::BufferUsage::ComputeStorageWrite,
                .sizeBytes = held.capacity * static_cast<u32>(sizeof(GpuSimParticle)),
                .debugName = "particles.gpu",
            });
            if (held.buffer.valid()) {
                const std::vector<GpuSimParticle> nobody(held.capacity);
                cmd.upload(held.buffer, asBytes(nobody.data(), nobody.size() * sizeof(GpuSimParticle)), 0);
            }
        }

        cmd.pushDebugGroup("particles-gpu");
        for (const RenderGpuEmitter& emitter : world.gpuEmitters) {
            GpuEmitterBuffer& held = gpuEmitters_[(static_cast<u64>(emitter.id.index) << 32) | emitter.id.generation];
            // Stepped once for each update of the particle system.
            if (!held.buffer.valid() || held.serial == emitter.serial)
                continue;
            held.serial = emitter.serial;

            GpuParticleSim sim;
            sim.prevViewProjection = prevViewProjection;
            sim.prevInverseViewProjection = core::inverse(prevViewProjection);
            sim.emitterPlace[0] = static_cast<f32>(emitter.frame.position.x - held.origin.x);
            sim.emitterPlace[1] = static_cast<f32>(emitter.frame.position.y - held.origin.y);
            sim.emitterPlace[2] = static_cast<f32>(emitter.frame.position.z - held.origin.z);
            sim.emitterPlace[3] = emitter.step;
            const Vec3 up = core::normalize(emitter.frame.rotation * Vec3{0.0f, 1.0f, 0.0f});
            const Vec3 side = core::normalize(emitter.frame.rotation * Vec3{1.0f, 0.0f, 0.0f});
            sim.emitterUp[0] = up.x;
            sim.emitterUp[1] = up.y;
            sim.emitterUp[2] = up.z;
            sim.emitterUp[3] = emitter.speed;
            sim.emitterSide[0] = side.x;
            sim.emitterSide[1] = side.y;
            sim.emitterSide[2] = side.z;
            sim.emitterSide[3] = emitter.spread;
            sim.emitterExtent[0] = emitter.extent.x;
            sim.emitterExtent[1] = emitter.extent.y;
            sim.emitterExtent[2] = emitter.extent.z;
            sim.emitterExtent[3] = emitter.lifetime;
            sim.acceleration[0] = emitter.acceleration.x;
            sim.acceleration[1] = emitter.acceleration.y;
            sim.acceleration[2] = emitter.acceleration.z;
            sim.acceleration[3] = emitter.drag;
            sim.wind[0] = emitter.wind.x;
            sim.wind[1] = emitter.wind.y;
            sim.wind[2] = emitter.wind.z;
            sim.turn[0] = emitter.rotation;
            sim.turn[1] = emitter.rotationSpread;
            sim.turn[2] = emitter.spin;
            sim.turn[3] = emitter.spinSpread;
            // What is seen needs a frame to have been drawn; the ground, its map.
            core::i32 meets = emitter.collision;
            if (!prevValid)
                meets &= ~1;
            if (!groundThere)
                meets &= ~2;
            sim.collide[0] = static_cast<f32>(meets);
            sim.collide[1] = static_cast<f32>(emitter.response);
            sim.collide[2] = emitter.bounce;
            sim.collide[3] = emitter.friction;
            sim.touch[0] = emitter.radius;
            sim.touch[1] = emitter.restSpeed;
            sim.touch[2] = static_cast<f32>(emitter.columns * emitter.rows);
            sim.originFromCamera[0] = static_cast<f32>(held.origin.x - prevOrigin.x);
            sim.originFromCamera[1] = static_cast<f32>(held.origin.y - prevOrigin.y);
            sim.originFromCamera[2] = static_cast<f32>(held.origin.z - prevOrigin.z);
            sim.depthParams[0] = prevNear;
            sim.depthParams[1] = prevFar;
            sim.depthParams[2] = 1.0f / static_cast<f32>(std::max(renderWidth_, 1u));
            sim.depthParams[3] = 1.0f / static_cast<f32>(std::max(renderHeight_, 1u));
            if (groundThere) {
                sim.ground[0] = static_cast<f32>(ground->corner.x - held.origin.x);
                sim.ground[1] = static_cast<f32>(ground->corner.z - held.origin.z);
                sim.ground[2] = 1.0f / ground->side;
                sim.ground[3] = static_cast<f32>(held.origin.y);
            }
            const u32 born = std::min(emitter.spawn, held.capacity);
            sim.spawn[0] = held.head;
            sim.spawn[1] = born;
            sim.spawn[2] = held.capacity;
            sim.spawn[3] = emitter.seed;
            sim.frame[0] = static_cast<u32>(emitter.serial);
            sim.frame[1] = groundThere ? 1u : 0u;
            held.head = (held.head + born) % held.capacity;

            const std::array<rhi::BufferHandle, 1> written{held.buffer};
            cmd.beginComputePass(written);
            cmd.setComputePipeline(particleSimPipeline_);
            const std::array<rhi::TextureBinding, 2> read{
                rhi::TextureBinding{prevValid ? depth_ : whitePixel_, pointSampler_},
                rhi::TextureBinding{groundThere ? particleGround_ : blackPixel_, environmentSampler_},
            };
            cmd.bindComputeTextures(0, read);
            cmd.bindComputeUniforms(0, asBytes(&sim, sizeof(sim)));
            cmd.dispatch((held.capacity + 63u) / 64u, 1, 1);
            cmd.endComputePass();
        }
        cmd.popDebugGroup();
    }

    // An emitter nobody asked for this frame has gone: its buffer with it.
    for (auto at = gpuEmitters_.begin(); at != gpuEmitters_.end();) {
        if (at->second.frame == gpuParticleFrame_) {
            ++at;
            continue;
        }
        if (at->second.buffer.valid())
            device.destroy(at->second.buffer);
        at = gpuEmitters_.erase(at);
    }
}

bool DefaultRenderer::ensureRibbons(rhi::IDevice& device)
{
    if (ribbonTried_)
        return ribbonPipeline_.valid() && ribbonBuffer_.valid();
    ribbonTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("ribbon", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("ribbon", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // A corner at a time: where it is, its colour, and where on the ribbon.
    const std::array<rhi::VertexAttribute, 3> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(RenderRibbonVertex)},
    };
    // Premultiplied, as the particles are and for their reason.
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true,
                  .srcColor = rhi::BlendFactor::One,
                  .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .srcAlpha = rhi::BlendFactor::One,
                  .dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha},
    }};
    ribbonPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        // Both sides: a ribbon that does not face the camera has two.
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = hdrTarget,
        .debugName = "ribbon",
    });
    ribbonBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(RibbonSystem::MaxVertices * sizeof(RenderRibbonVertex)),
        .debugName = "ribbons",
    });
    return ribbonPipeline_.valid() && ribbonBuffer_.valid();
}

bool DefaultRenderer::ensureHighlightMasks(rhi::IDevice& device)
{
    if (highlightTried_)
        return highlightMaskPipeline_.valid() && highlightMaskSkinnedPipeline_.valid();
    highlightTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("highlight_mask", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("highlight_mask", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle skinnedVertex = load("shadow_skinned", rhi::ShaderStage::Vertex);
    if (!vertex.valid() || !fragment.valid() || !skinnedVertex.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // The outline mask's two layouts, said again: the static mesh's stream,
    // and the same with the skin stream beside it.
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };
    const std::array<rhi::VertexBufferLayout, 2> skinnedBuffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
        rhi::VertexBufferLayout{.slot = 1, .strideBytes = 32},
    };
    const std::array<rhi::VertexAttribute, 3> skinnedAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
    };
    const std::array<rhi::ColorTargetDesc, 1> maskTarget{rhi::ColorTargetDesc{.format = kOcclusionFormat}};
    highlightMaskPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        // No depth attachment: the fragment reads the scene's depth and tests
        // against it by hand (`highlight_mask.hlsl`).
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = maskTarget,
        .debugName = "highlight_mask",
    });
    highlightMaskSkinnedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = skinnedVertex,
        .fragmentShader = fragment,
        .vertexBuffers = skinnedBuffers,
        .vertexAttributes = skinnedAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = {.depthTest = false, .depthWrite = false},
        .colorTargets = maskTarget,
        .debugName = "highlight_mask_skinned",
    });
    return highlightMaskPipeline_.valid() && highlightMaskSkinnedPipeline_.valid();
}

// **Every `Highlight`, over the finished image** (ADR 0129): for each, the
// draws that carry it into the mask, and the mask's edge and its fill
// composited in the colours it was given.
//
// After the tone curve and not before it, for the reason the editor's
// silhouette is: a highlight is a mark on the picture -- "this one", "an
// ally", "you can pick this up" -- and its colour must be the colour the game
// said at noon and at midnight. One mask and one composite a highlight, which
// is what the budget (`[render] max_highlights`) is a budget of.
void DefaultRenderer::drawHighlights(rhi::ICmdList& cmd, rhi::IDevice& device, const RenderWorld& world,
                                     const MeshCache& meshes, const RenderTarget& target)
{
    if (world.highlights.empty() || !world.camera.valid)
        return;
    cmd.pushDebugGroup("highlights");
    for (usize index = 0; index < world.highlights.size(); ++index) {
        const RenderHighlight& highlight = world.highlights[index];
        const bool occluded = highlight.occluded && ensureHighlightMasks(device);
        const rhi::PipelineHandle still = occluded ? highlightMaskPipeline_ : outlinePipeline_;
        const rhi::PipelineHandle skinned = occluded ? highlightMaskSkinnedPipeline_ : outlineSkinnedPipeline_;

        cmd.beginRenderPass({
            .colorAttachments = std::array<rhi::ColorAttachment, 1>{rhi::ColorAttachment{
                .texture = outlineMask_,
                .loadOp = rhi::LoadOp::Clear,
                .storeOp = rhi::StoreOp::Store,
            }},
            .debugName = "highlight-mask",
        });
        cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
        cmd.setScissor(
            {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
        cmd.setPipeline(still);
        if (occluded) {
            const std::array<rhi::TextureBinding, 1> sceneDepth{rhi::TextureBinding{depth_, pointSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, sceneDepth);
        }
        highlightFilter_ = static_cast<core::u8>(index + 1);
        // Unjittered (ADR 0158): over the finished picture, after the
        // temporal pass took the jitter out of it.
        drawGeometry(cmd, world, meshes, world.camera.unjitteredViewProjection, still, skinned, Selection::Highlight);
        highlightFilter_ = 0;
        cmd.endRenderPass();

        GpuOutlineUniforms outline;
        outline.texelWidth[0] = 1.0f / static_cast<f32>(renderWidth_);
        outline.texelWidth[1] = 1.0f / static_cast<f32>(renderHeight_);
        outline.texelWidth[2] = 2.0f;
        for (usize channel = 0; channel < 4; ++channel) {
            outline.color[channel] = highlight.outline[channel];
            outline.fillColor[channel] = highlight.fill[channel];
        }
        const std::array<rhi::TextureBinding, 1> maskBinding{rhi::TextureBinding{outlineMask_, linearSampler_}};
        fullscreenPass(cmd, outlineCompositePipeline_, target.color, target.width, target.height, "highlight-composite",
                       maskBinding, asBytes(&outline, sizeof(outline)), rhi::LoadOp::Load);
        countDraw(DrawKind::Highlight);
    }
    cmd.popDebugGroup();
}

bool DefaultRenderer::ensureFoliage(rhi::IDevice& device)
{
    if (foliageTried_)
        return foliageCullPipeline_.valid() && foliageFinalizePipeline_.valid() && foliagePipeline_.valid() &&
               foliageShadowPipeline_.valid();
    foliageTried_ = true;
    if (shaderLibrary_ == nullptr || !device.caps().compute)
        return false;

    core::EngineError error;
    foliageCullPipeline_ = shaderLibrary_->createCompute(device, "foliage_cull", &error);
    foliageFinalizePipeline_ = shaderLibrary_->createCompute(device, "foliage_finalize", &error);
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "foliage", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "foliage", rhi::ShaderStage::Fragment, &error);
    const rhi::ShaderHandle shadowVertex =
        shaderLibrary_->create(device, "foliage_shadow", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle shadowFragment =
        shaderLibrary_->create(device, "foliage_shadow", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment, shadowVertex, shadowFragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!foliageCullPipeline_.valid() || !foliageFinalizePipeline_.valid() || !vertex.valid() || !fragment.valid() ||
        !shadowVertex.valid() || !shadowFragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // The mesh's own vertex, as every static mesh is drawn; the instance comes
    // from the list the cull wrote. Both sides of a leaf, since a blade of grass
    // is usually one quad.
    const std::array<rhi::VertexBufferLayout, 1> buffers{rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48}};
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    // The position and the UV, the UV at location 1 (`foliage_shadow.hlsl`):
    // a card's image cuts its shadow as it cuts it.
    const std::array<rhi::VertexAttribute, 2> shadowAttributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};
    foliagePipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::None, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "foliage",
    });
    foliageShadowPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowVertex,
        .fragmentShader = shadowFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = shadowAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "foliage_shadow",
    });
    // **The shadow's shader through the camera** (ADR 0185): the depth the
    // forward pass wrote for the same instances, with the same holes -- the
    // card's image and the fade's dither -- so a decal can tell foliage from
    // what it stands on. Clipped as the forward pass clips, or the two depths
    // differ at the near plane. A frame with no decal never binds it.
    foliageDecalPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowVertex,
        .fragmentShader = shadowFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = shadowAttributes,
        .rasterizer = {.cullMode = rhi::CullMode::None, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "foliage_decal",
    });
    return foliagePipeline_.valid() && foliageShadowPipeline_.valid();
}

void DefaultRenderer::cullFoliage(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world,
                                  const MeshCache& meshes)
{
    foliageCulled_ = false;
    if (world.foliageRuns.empty() || world.foliageBuckets.empty() || !world.camera.valid)
        return;
    if (!ensureFoliage(device))
        return;

    // Each bucket's slice of the visible list -- a list per level of detail,
    // `capacity` long each -- and its indirect draws: one per section of each
    // level. The level an instance is drawn at is the cull's choice, by where
    // the coarser level's error stops showing as a pixel.
    const auto bucketCount = static_cast<u32>(world.foliageBuckets.size());
    foliageBucketBase_.assign(bucketCount, 0);
    foliageBucketLods_.assign(bucketCount, 0);
    foliageLodCommand_.assign(static_cast<usize>(bucketCount) * kFoliageMaxLods, 0);
    std::vector<std::array<f32, 4>> lodDistances(bucketCount, std::array<f32, 4>{});
    const f32 pixelsPerUnit = lodPixelsPerUnit(world.camera, height_);
    std::vector<rhi::DrawIndexedIndirectCommand> arguments;
    std::vector<GpuFoliageCommand> commands;
    u32 total = 0;
    for (u32 bucket = 0; bucket < bucketCount; ++bucket) {
        const RenderFoliageBucket& entry = world.foliageBuckets[bucket];
        foliageBucketBase_[bucket] = total;
        const MeshCache::Resolved* resolved = meshes.resolve(entry.mesh);
        if (resolved == nullptr || resolved->lods.empty())
            continue;
        const u32 levels = std::min(static_cast<u32>(resolved->lods.size()), kFoliageMaxLods);
        foliageBucketLods_[bucket] = levels;
        total += entry.capacity * levels;
        for (u32 lod = 0; lod < levels; ++lod) {
            // Where level `lod + 1` takes over, for a unit-scale instance.
            if (lod + 1 < levels)
                lodDistances[bucket][lod] =
                    resolved->lods[lod + 1].error * pixelsPerUnit * settings_.lodBias / LodPixelError;
            foliageLodCommand_[bucket * kFoliageMaxLods + lod] = static_cast<u32>(arguments.size());
            const MeshLodRange& level = resolved->lods[lod];
            for (u32 section = 0; section < level.sectionCount; ++section) {
                const MeshSection& geometry = resolved->sections[level.firstSection + section];
                arguments.push_back(rhi::DrawIndexedIndirectCommand{
                    .indexCount = geometry.indexCount,
                    .instanceCount = 0,
                    .firstIndex = resolved->firstIndex + geometry.firstIndex,
                    .vertexOffset = resolved->vertexOffset,
                    .firstInstance = 0,
                });
                commands.push_back(GpuFoliageCommand{bucket * kFoliageMaxLods + lod, entry.capacity});
            }
        }
    }
    if (total == 0 || arguments.empty())
        return;

    // Grown, never shrunk: a field walked through keeps its high-water mark.
    const auto grow = [&device](rhi::BufferHandle& buffer, u32& capacity, u32 needed, u32 stride,
                                rhi::BufferUsage usage, const char* name) {
        if (buffer.valid() && capacity >= needed)
            return;
        if (buffer.valid())
            device.destroy(buffer);
        capacity = std::max(needed, capacity + capacity / 2);
        buffer = device.createBuffer({.usage = usage, .sizeBytes = capacity * stride, .debugName = name});
    };
    grow(foliageVisible_, foliageVisibleCapacity_, total, kFoliageVisibleBytes,
         rhi::BufferUsage::GraphicsStorageRead | rhi::BufferUsage::ComputeStorageWrite, "foliage.visible");
    u32 bucketCapacity = foliageBucketCapacity_;
    grow(foliageCounters_, bucketCapacity, bucketCount * kFoliageMaxLods, 4,
         rhi::BufferUsage::ComputeStorageRead | rhi::BufferUsage::ComputeStorageWrite, "foliage.counters");
    foliageBucketCapacity_ = bucketCapacity;
    const auto commandCount = static_cast<u32>(arguments.size());
    u32 argumentCapacity = foliageCommandCapacity_;
    grow(foliageArguments_, argumentCapacity, commandCount, sizeof(rhi::DrawIndexedIndirectCommand),
         rhi::BufferUsage::Indirect | rhi::BufferUsage::ComputeStorageWrite, "foliage.arguments");
    u32 infoCapacity = foliageCommandCapacity_;
    grow(foliageCommands_, infoCapacity, commandCount, sizeof(GpuFoliageCommand), rhi::BufferUsage::ComputeStorageRead,
         "foliage.commands");
    foliageCommandCapacity_ = std::min(argumentCapacity, infoCapacity);
    if (!foliageVisible_.valid() || !foliageCounters_.valid() || !foliageArguments_.valid() ||
        !foliageCommands_.valid())
        return;

    // The counters from zero, the draws' fixed words, and which bucket each is.
    const std::vector<u32> zeros(static_cast<usize>(bucketCount) * kFoliageMaxLods, 0u);
    cmd.upload(foliageCounters_, asBytes(zeros.data(), zeros.size() * sizeof(u32)), 0);
    cmd.upload(foliageArguments_, asBytes(arguments.data(), arguments.size() * sizeof(arguments[0])), 0);
    cmd.upload(foliageCommands_, asBytes(commands.data(), commands.size() * sizeof(commands[0])), 0);

    cmd.pushDebugGroup("foliage-cull");
    const std::array<rhi::BufferHandle, 2> written{foliageVisible_, foliageCounters_};
    cmd.beginComputePass(written);
    cmd.setComputePipeline(foliageCullPipeline_);
    GpuFoliageCull cull;
    for (u32 plane = 0; plane < core::Frustum::SideCount; ++plane) {
        const core::Plane& side = world.camera.frustum.planes[plane];
        cull.planes[plane][0] = side.normal.x;
        cull.planes[plane][1] = side.normal.y;
        cull.planes[plane][2] = side.normal.z;
        cull.planes[plane][3] = side.distance;
    }
    cull.density = world.foliageDensity;
    for (const RenderFoliageRun& run : world.foliageRuns) {
        if (run.count == 0 || run.bucket >= bucketCount || foliageBucketLods_[run.bucket] == 0)
            continue;
        const RenderFoliageBucket& bucket = world.foliageBuckets[run.bucket];
        cull.lodCount = foliageBucketLods_[run.bucket];
        for (u32 lod = 0; lod < 4; ++lod)
            cull.lodDistances[lod] = lodDistances[run.bucket][lod];
        cull.originRadius[0] = static_cast<f32>(run.origin.x - world.camera.origin.x);
        cull.originRadius[1] = static_cast<f32>(run.origin.y - world.camera.origin.y);
        cull.originRadius[2] = static_cast<f32>(run.origin.z - world.camera.origin.z);
        cull.originRadius[3] = bucket.radius;
        cull.first = run.first;
        cull.count = run.count;
        cull.bucket = run.bucket;
        cull.bucketBase = foliageBucketBase_[run.bucket];
        cull.capacity = bucket.capacity;
        cull.drawDistance = run.drawDistance;
        cull.fadeStart = std::max(0.0f, run.drawDistance - run.fadeDistance);
        cull.scaleMin = run.scaleMin;
        cull.scaleMax = run.scaleMax;
        cull.sink = run.sink;
        cull.align = run.alignToNormal;
        cull.randomRotation = run.randomRotation ? 1.0f : 0.0f;
        const std::array<rhi::BufferHandle, 1> read{run.instances};
        cmd.bindComputeStorageBuffers(0, read);
        cmd.bindComputeUniforms(0, asBytes(&cull, sizeof(cull)));
        cmd.dispatch((run.count + 63u) / 64u, 1, 1);
    }
    cmd.endComputePass();

    const std::array<rhi::BufferHandle, 1> finalWrites{foliageArguments_};
    cmd.beginComputePass(finalWrites);
    cmd.setComputePipeline(foliageFinalizePipeline_);
    const std::array<rhi::BufferHandle, 2> finalReads{foliageCommands_, foliageCounters_};
    cmd.bindComputeStorageBuffers(0, finalReads);
    const GpuFoliageFinalize finalize{commandCount, {}};
    cmd.bindComputeUniforms(0, asBytes(&finalize, sizeof(finalize)));
    cmd.dispatch((commandCount + 63u) / 64u, 1, 1);
    cmd.endComputePass();
    cmd.popDebugGroup();
    foliageCulled_ = true;
}

void DefaultRenderer::drawFoliage(rhi::ICmdList& cmd, const RenderWorld& world, const MeshCache& meshes,
                                  const Mat4& viewProjection, FoliagePass pass)
{
    if (!foliageCulled_)
        return;
    // A decal's mask is drawn as a shadow is -- depth alone, with the card's
    // holes -- and differs in which buckets it takes and how far it reaches.
    const bool decal = pass == FoliagePass::DecalReceiving || pass == FoliagePass::DecalRefusing;
    const bool shadow = pass != FoliagePass::Forward;
    if (decal && !foliageDecalPipeline_.valid())
        return;
    cmd.setPipeline(decal ? foliageDecalPipeline_ : (shadow ? foliageShadowPipeline_ : foliagePipeline_));
    const std::array<rhi::BufferHandle, 1> list{foliageVisible_};
    cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, list);

    for (u32 index = 0; index < world.foliageBuckets.size(); ++index) {
        const RenderFoliageBucket& bucket = world.foliageBuckets[index];
        if (pass == FoliagePass::Shadow && !bucket.castShadow)
            continue;
        if (decal && bucket.receivesDecals != (pass == FoliagePass::DecalReceiving))
            continue;
        const MeshCache::Resolved* resolved = meshes.resolve(bucket.mesh);
        if (resolved == nullptr || resolved->lods.empty())
            continue;

        if (shadow) {
            const GpuShadowUniforms uniforms{viewProjection, Mat4{}};
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&uniforms, sizeof(uniforms)));
        }
        else {
            GpuObjectUniforms uniforms{viewProjection, Mat4{}, Mat4{}};
            uniforms.instanceAlphaUnused[0] = 1.0f;
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&uniforms, sizeof(uniforms)));
        }
        GpuFoliageUniforms foliage;
        foliage.wind[0] = world.environment.wind.x;
        foliage.wind[1] = world.environment.wind.y;
        foliage.wind[2] = world.environment.wind.z;
        foliage.wind[3] = static_cast<f32>(world.environment.surfaceTime);
        foliage.windParams[0] = world.environment.windGusts;
        foliage.windParams[1] = world.environment.windTurbulence;
        foliage.windParams[2] = bucket.windResponse / std::max(bucket.stiffness, 1e-3f);
        foliage.mesh[0] = bucket.meshMinY;
        foliage.mesh[1] = bucket.meshHeight;
        // The shadow's shader draws nothing past this. A decal's mask ends
        // where the decals do, and an instance's root is no further from any
        // of it than the mesh reaches at its largest.
        f32 reach = world.foliageShadowDistance;
        if (decal) {
            f32 largest = 1.0f;
            for (const RenderFoliageRun& run : world.foliageRuns) {
                if (run.bucket == index)
                    largest = std::max(largest, run.scaleMax);
            }
            reach = decalReach(world.decals) + bucket.radius * largest;
        }
        foliage.mesh[2] = reach;
        foliage.cameraOrigin[0] = static_cast<f32>(world.camera.origin.x);
        foliage.cameraOrigin[1] = static_cast<f32>(world.camera.origin.y);
        foliage.cameraOrigin[2] = static_cast<f32>(world.camera.origin.z);

        const std::array<rhi::BufferHandle, 1> vertexBuffers{resolved->vertices};
        cmd.bindVertexBuffers(0, vertexBuffers);
        cmd.bindIndexBuffer(resolved->indices, rhi::IndexType::U32);

        const u32 levels = index < foliageBucketLods_.size() ? foliageBucketLods_[index] : 0u;
        for (u32 lod = 0; lod < levels; ++lod) {
            // This level's list, after the bucket's base.
            foliage.windParams[3] = static_cast<f32>(foliageBucketBase_[index] + lod * bucket.capacity);
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&foliage, sizeof(foliage)));
            const u32 sections = resolved->lods[lod].sectionCount;
            const u32 firstCommand = foliageLodCommand_[index * kFoliageMaxLods + lod];
            for (u32 section = 0; section < sections; ++section) {
                // **The shadow takes the card's holes**: the material's
                // alpha, its cutoff, and its image.
                if (shadow) {
                    const RenderMaterial* material = section < bucket.sectionMaterials.size() &&
                                                             bucket.sectionMaterials[section] < world.materials.size()
                                                         ? &world.materials[bucket.sectionMaterials[section]]
                                                         : nullptr;
                    const std::array<f32, 4> cut{
                        material != nullptr ? material->uniforms.baseColor[3] : 1.0f,
                        material != nullptr ? material->uniforms.metallicRoughnessNormalCutoff[3] : 0.0f,
                        material != nullptr ? material->uniforms.textureFlags[0] : 0.0f, 0.0f};
                    cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(cut.data(), sizeof(cut)));
                    const std::array<rhi::TextureBinding, 1> image{rhi::TextureBinding{
                        material != nullptr && material->baseColor.valid() ? material->baseColor : whitePixel_,
                        linearSampler_}};
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, image);
                }
                if (!shadow && section < bucket.sectionMaterials.size() &&
                    bucket.sectionMaterials[section] < world.materials.size()) {
                    const RenderMaterial& material = world.materials[bucket.sectionMaterials[section]];
                    cmd.bindUniforms(rhi::ShaderStage::Fragment, 1,
                                     asBytes(&material.uniforms, sizeof(material.uniforms)));
                    const auto orDefault = [](rhi::TextureHandle handle, rhi::TextureHandle fallback) {
                        return handle.valid() ? handle : fallback;
                    };
                    const std::array<rhi::TextureBinding, 15> textures{
                        rhi::TextureBinding{orDefault(material.baseColor, whitePixel_), linearSampler_},
                        rhi::TextureBinding{orDefault(material.normal, flatNormalPixel_), linearSampler_},
                        rhi::TextureBinding{orDefault(material.metallicRoughness, whitePixel_), linearSampler_},
                        rhi::TextureBinding{orDefault(material.emissive, blackPixel_), linearSampler_},
                        rhi::TextureBinding{shadowMap_, shadowSampler_},
                        rhi::TextureBinding{environmentMap_, environmentSampler_},
                        rhi::TextureBinding{brdfLut_, environmentSampler_},
                        rhi::TextureBinding{clusterGrid_, pointSampler_},
                        rhi::TextureBinding{lightIndices_, pointSampler_},
                        rhi::TextureBinding{lightData_, pointSampler_},
                        rhi::TextureBinding{occlusion_, linearSampler_},
                        rhi::TextureBinding{localShadowMap_, shadowSampler_},
                        rhi::TextureBinding{contact_, pointSampler_},
                        // What the screen says (ADR 0209), or nothing while it is off.
                        screenReflectionBinding(true),
                        screenBounceBinding(),
                    };
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
                }
                cmd.drawIndexedIndirect(
                    foliageArguments_,
                    (firstCommand + section) * static_cast<u32>(sizeof(rhi::DrawIndexedIndirectCommand)), 1);
                countDraw(DrawKind::Foliage);
            }
        }
    }
}

// **The sprite pipeline again, with the mask as a second target** (ADR 0153).
// Both targets blend the same way -- straight alpha, each by its own output's
// alpha -- so no backend is asked for a blend state per target.
bool DefaultRenderer::ensureSpritesExact(rhi::IDevice& device)
{
    if (spriteExactTried_)
        return spriteExactPipeline_.valid();
    spriteExactTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "sprite_exact", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment =
        shaderLibrary_->create(device, "sprite_exact", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(GpuSprite), .perInstance = true},
    };
    const std::array<rhi::ColorTargetDesc, 2> targets{
        rhi::ColorTargetDesc{.format = kHdrFormat, .blend = {.enabled = true}},
        rhi::ColorTargetDesc{.format = kSpriteMaskFormat, .blend = {.enabled = true}},
    };
    spriteExactPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::None},
        // As the plain sprite pipeline's: tested, never written.
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = targets,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sprite_exact",
    });
    return spriteExactPipeline_.valid();
}

bool DefaultRenderer::ensureSprites(rhi::IDevice& device)
{
    if (spriteTried_)
        return spritePipeline_.valid() && spriteBuffer_.valid();
    spriteTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const rhi::ShaderHandle vertex = shaderLibrary_->create(device, "sprite", rhi::ShaderStage::Vertex, &error);
    const rhi::ShaderHandle fragment = shaderLibrary_->create(device, "sprite", rhi::ShaderStage::Fragment, &error);
    for (const rhi::ShaderHandle handle : {vertex, fragment}) {
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
    }
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 16},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 32},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 48},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = sizeof(GpuSprite), .perInstance = true},
    };
    // Straight alpha, as a picture's transparent pixels are stored.
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true},
    }};
    spritePipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        // A flipped sprite is still a sprite, and the plane is seen from
        // either side.
        .rasterizer = {.cullMode = rhi::CullMode::None},
        // Tested and never written, like every blended surface: a 3D part in
        // front of the plane hides a sprite, and sprites order among
        // themselves by `ZIndex`, which is the order they are drawn in.
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "sprite",
    });
    spriteBuffer_ = device.createBuffer({
        .usage = rhi::BufferUsage::Vertex,
        .sizeBytes = static_cast<u32>(MaxSprites * sizeof(GpuSprite)),
        .debugName = "sprites",
    });
    return spritePipeline_.valid() && spriteBuffer_.valid();
}

bool DefaultRenderer::ensureVoxel(rhi::IDevice& device)
{
    if (voxelTried_)
        return voxelPipeline_.valid();
    voxelTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("voxel", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("voxel", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }
    // `asset::Vertex`, all four attributes: the block shader reads the UV.
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};

    // The atlas and the pipeline that fills it, beside the block pipeline that
    // reads it: a block world with images needs all three and one without
    // pays for the atlas's memory only when the first image arrives.
    const rhi::ShaderHandle tileVertex = load("voxel_tile", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle tileFragment = load("voxel_tile", rhi::ShaderStage::Fragment);
    if (tileVertex.valid() && tileFragment.valid()) {
        const std::array<rhi::ColorTargetDesc, 1> atlasTarget{rhi::ColorTargetDesc{.format = kVoxelAtlasFormat}};
        voxelTilePipeline_ = device.createGraphicsPipeline({
            .vertexShader = tileVertex,
            .fragmentShader = tileFragment,
            .rasterizer = {.cullMode = rhi::CullMode::None, .depthClip = true},
            .colorTargets = atlasTarget,
            .debugName = "voxel-tile",
        });
    }

    voxelPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "voxel",
    });
    voxelPrepassedPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "voxel_prepassed",
    });
    // **Both sides, blended, depth tested and not written**: from under water
    // the surface is seen from below, and a pane of glass is a pane from either
    // side.
    const std::array<rhi::ColorTargetDesc, 1> blendTarget{rhi::ColorTargetDesc{
        .format = kHdrFormat,
        .blend = {.enabled = true},
    }};
    voxelBlendPipeline_ = device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::None, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = false, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = blendTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = "voxel-blend",
    });

    // The shadow pipeline's state is `shadow_depth`'s -- front faces culled,
    // for that same reason -- and only its fragment stage differs.
    const rhi::ShaderHandle shadowVertex = load("voxel_shadow", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle shadowFragment = load("voxel_shadow", rhi::ShaderStage::Fragment);
    if (shadowVertex.valid() && shadowFragment.valid()) {
        const std::span<const rhi::VertexAttribute> shadowAttributes{attributes.data(), 3};
        voxelShadowPipeline_ = device.createGraphicsPipeline({
            .vertexShader = shadowVertex,
            .fragmentShader = shadowFragment,
            .vertexBuffers = buffers,
            .vertexAttributes = shadowAttributes,
            .rasterizer = {.cullMode = rhi::CullMode::Front},
            .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
            .colorTargets = {},
            .depthStencilFormat = kShadowFormat,
            .debugName = "voxel-shadow",
        });
    }
    return voxelPipeline_.valid();
}

void DefaultRenderer::releaseTerrainArrays(rhi::IDevice& device, TerrainArrays& entry)
{
    for (rhi::TextureHandle& array : entry.arrays) {
        if (array.valid())
            device.destroy(array);
        array = {};
    }
    entry.ready = false;
    entry.sources.clear();
}

const DefaultRenderer::TerrainArrays* DefaultRenderer::terrainArraysOf(core::InstanceId id) const noexcept
{
    for (const TerrainArrays& entry : terrainArrays_) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

void DefaultRenderer::updateTerrainArrays(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderWorld& world)
{
    ENG_PROFILE_SCOPE("prepare.terrain_arrays");
    for (TerrainArrays& entry : terrainArrays_)
        ++entry.unseen;

    // The stand-ins, the first frame a terrain is drawn rather than at
    // creation: a world with no ground never makes them. Two layers, because
    // one is a plain 2D texture to a backend.
    if (!world.terrains.empty() && !whiteArray_.valid()) {
        const auto oneArray = [&](const char* name) {
            return device.createTexture({
                .format = rhi::TextureFormat::Rgba8Unorm,
                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
                .width = 1,
                .height = 1,
                .layers = 2,
                .debugName = name,
            });
        };
        whiteArray_ = oneArray("terrain-white-array");
        flatNormalArray_ = oneArray("terrain-normal-array");
        terrainPlainLayers_ =
            device.createBuffer({.usage = rhi::BufferUsage::GraphicsStorageRead,
                                 .sizeBytes = static_cast<u32>(sizeof(GpuTerrainLayer)) * kTerrainLayerSlots,
                                 .debugName = "terrain-plain-layers"});
        if (terrainPlainLayers_.valid()) {
            const std::vector<GpuTerrainLayer> plain(kTerrainLayerSlots, plainTerrainLayer());
            cmd.upload(terrainPlainLayers_, asBytes(plain.data(), plain.size() * sizeof(GpuTerrainLayer)), 0);
        }
        // **Cleared to what they stand for**: made without contents,
        // they held whatever the allocator handed back, and a slot the shader
        // samples must never.
        const std::array<std::pair<rhi::TextureHandle, rhi::ColorRgba>, 2> standIns{
            std::pair{whiteArray_, rhi::ColorRgba{1.0f, 1.0f, 1.0f, 1.0f}},
            std::pair{flatNormalArray_, rhi::ColorRgba{0.5f, 0.5f, 1.0f, 1.0f}},
        };
        for (const auto& [array, colour] : standIns) {
            for (u32 layer = 0; layer < 2 && array.valid(); ++layer) {
                const std::array<rhi::ColorAttachment, 1> attachment{rhi::ColorAttachment{
                    .texture = array, .loadOp = rhi::LoadOp::Clear, .clearColor = colour, .layer = layer}};
                cmd.beginRenderPass({.colorAttachments = attachment, .debugName = "terrain-stand-in"});
                cmd.endRenderPass();
            }
        }
    }

    for (const RenderTerrain& terrain : world.terrains) {
        auto found = std::find_if(terrainArrays_.begin(), terrainArrays_.end(),
                                  [&](const TerrainArrays& entry) { return entry.id == terrain.id; });
        if (found == terrainArrays_.end()) {
            terrainArrays_.emplace_back();
            found = terrainArrays_.end() - 1;
            found->id = terrain.id;
        }
        TerrainArrays& entry = *found;
        entry.unseen = 0;

        // The block, every frame: a material's colour is cheap to change and
        // must show at once, textures or not.
        const auto layerCount = static_cast<u32>(std::min<usize>(terrain.layers.size(), kTerrainLayerSlots - 1));
        GpuTerrainSurfaceUniforms& block = entry.uniforms;
        block = GpuTerrainSurfaceUniforms{};
        entry.layers.assign(kTerrainLayerSlots, plainTerrainLayer());
        bool waiting = false;
        std::vector<u32> sources;
        sources.reserve(static_cast<usize>(layerCount) * 4u);
        for (u32 index = 0; index < layerCount; ++index) {
            const RenderTerrainLayer& layer = terrain.layers[index];
            GpuTerrainLayer& row = entry.layers[index + 1];
            for (u32 channel = 0; channel < 4; ++channel) {
                row.flat[channel] = layer.flat[channel];
                row.tint[channel] = layer.tint[channel];
                row.surface[channel] = layer.surface[channel];
                row.tiling[channel] = layer.tiling[channel];
                row.emissive[channel] = layer.emissive[channel];
            }
            waiting = waiting || layer.waiting;
            for (const rhi::TextureHandle map : layer.maps)
                sources.push_back(map.id);
        }
        const auto ruleCount = static_cast<u32>(std::min<usize>(terrain.rules.size(), kTerrainRuleSlots));
        for (u32 index = 0; index < ruleCount; ++index) {
            const asset::TerrainRuleShape& rule = terrain.rules[index];
            for (u32 channel = 0; channel < 4; ++channel) {
                block.ruleSlope[index][channel] = rule.slope[channel];
                block.ruleHeight[index][channel] = rule.height[channel];
            }
            block.ruleMisc[index][0] = rule.material;
            block.ruleMisc[index][1] = rule.noise;
            block.ruleMisc[index][2] = rule.heightJitter;
            block.ruleMisc[index][3] = rule.enabled;
            for (u32 word = 0; word < 8; ++word)
                block.ruleApplies[index][word] = rule.appliesTo[word];
        }
        // **The slope under which no rule covers anything**: a rule's cover is
        // nothing until the slope, its noise added, passes where its band
        // begins -- and the noise adds half its strength at most. Ground
        // flatter than the flattest rule's is ground the rules need not be
        // asked about.
        f32 flattest = 2.0f;
        for (u32 index = 0; index < ruleCount; ++index) {
            const asset::TerrainRuleShape& rule = terrain.rules[index];
            if (rule.enabled >= 0.5f)
                flattest = std::min(flattest, rule.slope[0] - 0.5f * std::abs(rule.noise));
        }
        block.debug[3] = flattest;
        block.params[1] = static_cast<f32>(ruleCount);
        block.params[2] = static_cast<f32>(layerCount);
        block.params[3] = static_cast<f32>(terrain.origin.y);
        // The layers, put up only when a material changed: a copy pass, before
        // any other.
        if (!entry.layerBuffer.valid()) {
            entry.layerBuffer =
                device.createBuffer({.usage = rhi::BufferUsage::GraphicsStorageRead,
                                     .sizeBytes = static_cast<u32>(sizeof(GpuTerrainLayer)) * kTerrainLayerSlots,
                                     .debugName = "terrain-layers"});
        }
        if (entry.layerBuffer.valid() && entry.layers.size() == kTerrainLayerSlots &&
            (entry.layersUp.size() != entry.layers.size() ||
             std::memcmp(entry.layersUp.data(), entry.layers.data(), entry.layers.size() * sizeof(GpuTerrainLayer)) !=
                 0)) {
            cmd.upload(entry.layerBuffer, asBytes(entry.layers.data(), entry.layers.size() * sizeof(GpuTerrainLayer)),
                       0);
            entry.layersUp = entry.layers;
        }

        // **The arrays, when every layer's maps have loaded and something
        // changed**: a blit per map into its slice, then the mips. Until then
        // the terrain draws flat, as it did before it had textures.
        if (!waiting && layerCount > 0 && terrainValid_ && (!entry.ready || entry.sources != sources)) {
            releaseTerrainArrays(device, entry);
            constexpr std::array<rhi::TextureFormat, 3> Formats{
                rhi::TextureFormat::Rgba8UnormSrgb, rhi::TextureFormat::Rgba8Unorm, rhi::TextureFormat::Rgba8Unorm};
            constexpr std::array<const char*, 3> Names{"terrain-color", "terrain-normal", "terrain-surface"};
            constexpr u32 Size = 512;
            constexpr u32 Mips = 10;
            bool made = true;
            for (usize slot = 0; slot < 3; ++slot) {
                entry.arrays[slot] = device.createTexture({
                    .format = Formats[slot],
                    .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
                    .width = Size,
                    .height = Size,
                    // At least two: one layer is a plain 2D texture to a
                    // backend, and the shader reads an array.
                    .layers = std::max(layerCount, 2u),
                    .mipLevels = Mips,
                    .debugName = Names[slot],
                });
                made = made && entry.arrays[slot].valid();
            }
            if (made) {
                // **Drawn, a layer at a time** (`terrain_pack.hlsl`): each
                // map read at the mip that fits the layer, and the surface
                // layer packed from the material's metallic-roughness and
                // height maps. A copy of mip 0 shimmered on a map over twice
                // the layer's size, and could not pack.
                const std::array<rhi::TextureHandle, 3> neutral{whitePixel_, flatNormalPixel_, whitePixel_};
                for (u32 index = 0; index < layerCount; ++index) {
                    const RenderTerrainLayer& layer = terrain.layers[index];
                    const rhi::TextureHandle height = layer.maps[3];
                    for (usize slot = 0; slot < 3; ++slot) {
                        const rhi::TextureHandle source = layer.maps[slot];
                        const std::array<f32, 4> uniforms{slot == 2 ? 1.0f : 0.0f, height.valid() ? 1.0f : 0.0f,
                                                          static_cast<f32>(Size), 0.0f};
                        const std::array<rhi::TextureBinding, 2> maps{
                            rhi::TextureBinding{source.valid() ? source : neutral[slot], linearSampler_},
                            rhi::TextureBinding{height.valid() ? height : whitePixel_, linearSampler_},
                        };
                        const std::array<rhi::ColorAttachment, 1> attachment{rhi::ColorAttachment{
                            .texture = entry.arrays[slot], .loadOp = rhi::LoadOp::DontCare, .layer = index}};
                        cmd.beginRenderPass({.colorAttachments = attachment, .debugName = "terrain-pack"});
                        cmd.setPipeline(slot == 0 ? terrainPackColorPipeline_ : terrainPackLinearPipeline_);
                        cmd.setViewport({.width = static_cast<f32>(Size), .height = static_cast<f32>(Size)});
                        cmd.setScissor({.width = static_cast<core::i32>(Size), .height = static_cast<core::i32>(Size)});
                        cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(uniforms.data(), sizeof(uniforms)));
                        cmd.bindTextures(rhi::ShaderStage::Fragment, 0, maps);
                        cmd.draw(3, 1, 0, 0);
                        cmd.endRenderPass();
                    }
                }
                for (const rhi::TextureHandle array : entry.arrays)
                    cmd.generateMipmaps(array);
                entry.sources = std::move(sources);
                entry.ready = true;
            }
            else {
                releaseTerrainArrays(device, entry);
            }
        }
        block.params[0] = entry.ready && !waiting ? 1.0f : 0.0f;
    }

    // A terrain gone from every world drawn for a while gives its arrays back.
    const auto gone = [](const TerrainArrays& entry) { return entry.unseen > TerrainArraysKeptUnseen; };
    for (TerrainArrays& entry : terrainArrays_) {
        if (gone(entry)) {
            releaseTerrainArrays(device, entry);
            if (entry.layerBuffer.valid())
                device.destroy(entry.layerBuffer);
            entry.layerBuffer = {};
        }
    }
    std::erase_if(terrainArrays_, gone);
}

bool DefaultRenderer::ensureTerrain(rhi::IDevice& device)
{
    ENG_PROFILE_SCOPE("prepare.terrain_pipeline");
    if (terrainTried_)
        return terrainValid_;
    terrainTried_ = true;
    if (shaderLibrary_ == nullptr)
        return false;

    core::EngineError error;
    const auto load = [&](std::string_view name, rhi::ShaderStage stage) -> rhi::ShaderHandle {
        const rhi::ShaderHandle handle = shaderLibrary_->create(device, name, stage, &error);
        if (handle.valid() && shaderCount_ < std::size(shaders_))
            shaders_[shaderCount_++] = handle;
        return handle;
    };
    const rhi::ShaderHandle vertex = load("terrain", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle fragment = load("terrain", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle depthVertex = load("terrain_depth", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle depthFragment = load("terrain_depth", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle shadowVertex = load("terrain_shadow", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle shadowFragment = load("terrain_shadow", rhi::ShaderStage::Fragment);
    const rhi::ShaderHandle packVertex = load("terrain_pack", rhi::ShaderStage::Vertex);
    const rhi::ShaderHandle packFragment = load("terrain_pack", rhi::ShaderStage::Fragment);
    if (!vertex.valid() || !fragment.valid() || !depthVertex.valid() || !depthFragment.valid() ||
        !shadowVertex.valid() || !shadowFragment.valid() || !packVertex.valid() || !packFragment.valid()) {
        core::logText(core::LogLevel::Warn, error.message);
        return false;
    }

    // `asset::Vertex`, 48 bytes, the layout every static mesh has -- a terrain
    // node is filed in `MeshCache` like one. All four attributes: the UV is
    // where the mesher puts a triangle's three layers and the corner (ADR 0113).
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };
    // The forward pipelines are made when a frame draws with one
    // (`ensureTerrainForward`); the full ground's shaders are kept for it.
    terrainVertex_ = vertex;
    terrainFragment_ = fragment;
    // **Back faces culled in the shadow pass**, where every mesh culls its
    // FRONT faces so the depth stored is a solid's far side. The ground
    // is a surface with no far side: culling its front faces would cull all of
    // it, and the sun would shine through the hills. So it stores the side the
    // light falls on, pushed past what a receiver's filter reads by the slope
    // (`terrain_shadow.hlsl`); a face turned from the light is behind that
    // side, and drawn too it was only more ground to acne.
    // All four attributes: the geomorph's offset rides in three of them.
    terrainShadowPipeline_ = device.createGraphicsPipeline({
        .vertexShader = shadowVertex,
        .fragmentShader = shadowFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kShadowFormat,
        .debugName = "terrain_shadow",
    });

    terrainPrepassPipeline_ = device.createGraphicsPipeline({
        .vertexShader = depthVertex,
        .fragmentShader = depthFragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = {},
        .depthStencilFormat = kDepthFormat,
        .debugName = "terrain_prepass",
    });

    const auto pack = [&](rhi::TextureFormat format, const char* name) {
        const std::array<rhi::ColorTargetDesc, 1> target{rhi::ColorTargetDesc{.format = format}};
        return device.createGraphicsPipeline({
            .vertexShader = packVertex,
            .fragmentShader = packFragment,
            .primitive = rhi::PrimitiveType::TriangleList,
            .rasterizer = {.cullMode = rhi::CullMode::None},
            .colorTargets = target,
            .debugName = name,
        });
    };
    terrainPackColorPipeline_ = pack(rhi::TextureFormat::Rgba8UnormSrgb, "terrain_pack_color");
    terrainPackLinearPipeline_ = pack(rhi::TextureFormat::Rgba8Unorm, "terrain_pack_linear");

    terrainValid_ = terrainShadowPipeline_.valid() && terrainPrepassPipeline_.valid() &&
                    terrainPackColorPipeline_.valid() && terrainPackLinearPipeline_.valid();
    return terrainValid_;
}

GraphicsSettings::TerrainSurface DefaultRenderer::terrainForwardWanted() const noexcept
{
    using Surface = GraphicsSettings::TerrainSurface;
    // A debug view is the full ground's to draw: the variants have none. And
    // a variant that could not be made is drawn as the full ground.
    if (settings_.debugView != DebugView::None)
        return Surface::Full;
    const Surface asked = settings_.terrainSurfaceNow();
    if (asked == Surface::Fast && !(terrainFastTried_ && !terrainFastPipeline_.valid()))
        return Surface::Fast;
    if (asked == Surface::Flat && !(terrainFlatTried_ && !terrainFlatPipeline_.valid()))
        return Surface::Flat;
    return Surface::Full;
}

rhi::PipelineHandle DefaultRenderer::terrainForwardPipeline() const noexcept
{
    using Surface = GraphicsSettings::TerrainSurface;
    const Surface wanted = terrainForwardWanted();
    if (wanted == Surface::Full && terrainTriplanarNoHexNow_ && terrainTriplanarNoHexPipeline_.valid())
        return terrainTriplanarNoHexPipeline_;
    if (wanted == Surface::Full && terrainNoHexNow_ && terrainNoHexPipeline_.valid())
        return terrainNoHexPipeline_;
    return wanted == Surface::Fast   ? terrainFastPipeline_
           : wanted == Surface::Flat ? terrainFlatPipeline_
                                     : terrainPipeline_;
}

rhi::PipelineHandle DefaultRenderer::makeTerrainForward(rhi::IDevice& device, rhi::ShaderHandle vertex,
                                                        rhi::ShaderHandle fragment, const char* debugName)
{
    // `asset::Vertex`, 48 bytes, the layout every static mesh has, all four
    // attributes (`ensureTerrain`): a variant is another fragment over the
    // same draw.
    const std::array<rhi::VertexAttribute, 4> attributes{
        rhi::VertexAttribute{.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 0},
        rhi::VertexAttribute{.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offsetBytes = 12},
        rhi::VertexAttribute{.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offsetBytes = 24},
        rhi::VertexAttribute{.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offsetBytes = 40},
    };
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = 48},
    };
    const std::array<rhi::ColorTargetDesc, 1> hdrTarget{rhi::ColorTargetDesc{.format = kHdrFormat}};
    return device.createGraphicsPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .rasterizer = {.cullMode = rhi::CullMode::Back, .depthClip = true},
        .depthStencil = {.depthTest = true, .depthWrite = true, .depthCompare = rhi::CompareOp::LessOrEqual},
        .colorTargets = hdrTarget,
        .depthStencilFormat = kDepthFormat,
        .debugName = debugName,
    });
}

void DefaultRenderer::ensureTerrainForward(rhi::IDevice& device, const RenderWorld& world)
{
    ENG_PROFILE_SCOPE("prepare.terrain_forward");
    if (!terrainValid_ || shaderLibrary_ == nullptr)
        return;
    using Surface = GraphicsSettings::TerrainSurface;
    const auto variant = [&](std::string_view name, const char* debugName) -> rhi::PipelineHandle {
        core::EngineError error;
        const rhi::ShaderHandle vertex = shaderLibrary_->create(device, name, rhi::ShaderStage::Vertex, &error);
        const rhi::ShaderHandle fragment = shaderLibrary_->create(device, name, rhi::ShaderStage::Fragment, &error);
        for (const rhi::ShaderHandle handle : {vertex, fragment})
            if (handle.valid() && shaderCount_ < std::size(shaders_))
                shaders_[shaderCount_++] = handle;
        if (!vertex.valid() || !fragment.valid()) {
            // Said, and the full ground draws in its place.
            core::logText(core::LogLevel::Warn, error.message);
            return {};
        }
        return makeTerrainForward(device, vertex, fragment, debugName);
    };
    // The one asked for; and where it cannot be made, the full one, which
    // `terrainForwardWanted` then answers.
    if (terrainForwardWanted() == Surface::Fast && !terrainFastTried_) {
        terrainFastTried_ = true;
        terrainFastPipeline_ = variant("terrain_fast", "terrain_fast");
    }
    if (terrainForwardWanted() == Surface::Flat && !terrainFlatTried_) {
        terrainFlatTried_ = true;
        terrainFlatPipeline_ = variant("terrain_flat", "terrain_flat");
    }
    // A material-dependent permutation, not a quality tier. None of the
    // world's layers can enter the omitted branch. Re-evaluate every frame
    // so edits and views with another terrain retain the complete feature.
    terrainNoHexNow_ = !terrainNeedsHexSampling(world);
    terrainTriplanarNoHexNow_ = terrainNoHexNow_ && terrainIsTriplanarOnly(world);
    if (terrainForwardWanted() == Surface::Full && terrainTriplanarNoHexNow_) {
        if (!terrainTriplanarNoHexTried_) {
            terrainTriplanarNoHexTried_ = true;
            terrainTriplanarNoHexPipeline_ = variant("terrain_triplanar_no_hex", "terrain_triplanar_no_hex");
        }
        if (terrainTriplanarNoHexPipeline_.valid())
            return;
    }
    if (terrainForwardWanted() == Surface::Full && terrainNoHexNow_) {
        if (!terrainNoHexTried_) {
            terrainNoHexTried_ = true;
            terrainNoHexPipeline_ = variant("terrain_no_hex", "terrain_no_hex");
        }
        if (terrainNoHexPipeline_.valid())
            return;
    }
    if (terrainForwardWanted() == Surface::Full && !terrainFullTried_) {
        terrainFullTried_ = true;
        terrainPipeline_ = makeTerrainForward(device, terrainVertex_, terrainFragment_, "terrain");
    }
}

void DefaultRenderer::render(rhi::IDevice& device, rhi::ICmdList& cmd, const RenderTarget& target,
                             const RenderWorld& world, const MeshCache& meshes)
{
    if (!valid_ || !target.color.valid() || target.width == 0 || target.height == 0)
        return;
    useView(target.view);
    windowPart_ = target.window && target.view != 0;
    // **A headset's eye draws no screen-space shading** unless asked
    // (`RenderWorld::headsetEye`): the settings this picture is drawn with,
    // put back when it is drawn.
    struct SettingsBack
    {
        GraphicsSettings& live;
        GraphicsSettings saved;
        ~SettingsBack() { live = saved; }
    } settingsBack{settings_, settings_};
    if (world.headsetEye && !world.headsetScreenEffects) {
        settings_.ambientOcclusion = false;
        settings_.contactShadows = false;
        settings_.screenReflectionSteps = 0;
        settings_.screenBounceRays = 0;
    }
    // **An eye is drawn whole and sharp**: at its own resolution, never
    // scaled up from a smaller one -- a handheld's resolution cap drew a
    // Quest's eye below its pixels and FSR brought it up, and the player saw
    // every edge in steps -- and with no temporal anti-aliasing, whose jitter
    // and history are one view's and no pair of eyes': the spatial one
    // instead.
    if (world.headsetEye) {
        if (settings_.antiAliasing == AntiAliasingMode::Taa)
            settings_.antiAliasing = AntiAliasingMode::Smaa;
        settings_.upscaling = UpscalingMode::None;
        // At its own resolution: `[xr] render_scale` makes the runtime's
        // images smaller instead, and its compositor brings them up. Only a
        // headset that cannot leaves the scale here, and the tonemap's own
        // filtering brings the picture up.
        settings_.renderScale = std::clamp(world.headsetQuality.renderScale, 0.5f, 1.0f);
        settings_.renderResolutionCap = 0;
        settings_.frameGeneration = false;
    }
    // **And lean on a standalone headset** (`RenderWorld::headsetLean`):
    // two eyes at seventy-two frames a second on a phone's GPU leave no room
    // for what a flat game on it can afford.
    if (world.headsetEye && world.headsetLean) {
        settings_.bloom = false;
        settings_.sunRays = false;
        settings_.depthOfField = false;
        settings_.shadowCascades = std::min(settings_.shadowCascades, 2u);
        // **Shadows that hold up at arm's length**: a handheld's 512-texel
        // tiles over seventy metres were what a Quest's player saw of a cup
        // on the table, a smear. Tiles of 1024 -- the two cascades in a 2048
        // atlas -- over twenty metres, the first of them two metres deep.
        // **2048 texels a tile, the two side by side in one row**: a
        // first cascade two metres deep at a millimetre or two a texel, whose
        // edge a player sees at arm's length without steps -- and no atlas of
        // four tiles, two of them empty, cleared and stored each frame.
        settings_.shadowTileResolution = std::max(settings_.shadowTileResolution, 2048u);
        settings_.shadowDistance = std::min(settings_.shadowDistance, 20.0f);
        // **No spatial anti-aliasing pass**: a pass over each eye's three
        // million pixels, 0.9 ms of a Quest 3's frame. The eye is drawn
        // straight into the runtime's image by the tonemap.
        settings_.antiAliasing = AntiAliasingMode::Off;
    }
    // **A lean window** (`leanPath`, the mobile lean path): what a tile GPU
    // pays a pass over every pixel for, left out -- each such pass leaves the
    // tile and comes back, in bandwidth and heat. The anti-aliasing is the
    // forward pass's samples where the frame allows them (below).
    const bool leanWindow = !world.headsetEye && settings_.leanPath;
    if (leanWindow) {
        settings_.bloom = false;
        settings_.sunRays = false;
        settings_.depthOfField = false;
        settings_.ambientOcclusion = false;
        settings_.contactShadows = false;
        settings_.screenReflectionSteps = 0;
        settings_.screenBounceRays = 0;
    }
    // **And the project's word over both** (`[xr]`, `HeadsetQuality`): what
    // it sets, in place of the level's or the numbers above.
    if (world.headsetEye) {
        const HeadsetQuality& quality = world.headsetQuality;
        if (quality.shadowResolution != 0) {
            u32 tile = 256;
            while (tile * 2 <= std::min(quality.shadowResolution, 2048u))
                tile *= 2;
            settings_.shadowTileResolution = tile;
        }
        if (quality.shadowDistance > 0.0f)
            settings_.shadowDistance = std::clamp(quality.shadowDistance, 5.0f, 500.0f);
        if (quality.shadowCascades != 0)
            settings_.shadowCascades = std::min(quality.shadowCascades, 4u);
    }
    // Each pass's time on the CPU, recording it: what was submitted is
    // the GPU's, which `wait.*` measures.
    core::profile::Sections passes;
    ENG_PROFILE_NEXT(passes, "render.prepare");

    // **The one place the render scale is applied.** Everything below draws the
    // WORLD at `renderWidth_` by `renderHeight_` and only the final resolve
    // writes `target`, so a reduced scale costs every per-pixel pass at once
    // and costs the 2D pass -- which the host draws afterwards, at the target's
    // own size -- nothing at all.
    // The scale, and the cap on a handheld's resolution (`effectiveRenderScale`)
    // -- and neither for a picture of sprites alone (`spritesOnly`).
    const f32 worldScale = worldRenderScale(world, target.width, target.height);
    // A view that brings no bake leaves the layers as the last that did.
    if (world.lightmap.sun)
        sunGridWanted_ = true;
    const auto scaled = [&](u32 value) {
        const auto result = static_cast<u32>(static_cast<f32>(value) * worldScale + 0.5f);
        return result > 0 ? result : 1u;
    };
    renderWidth_ = scaled(target.width);
    renderHeight_ = scaled(target.height);

    shadowRowsWanted_ = world.headsetEye && world.headsetLean && settings_.shadowCascades <= 2 ? 1u : 2u;
    if (ensureShadowMap(device).has_value())
        return;
    if (ensureTargets(device, renderWidth_, renderHeight_).has_value())
        return;
    // The look's images are remade lazily at the new size, by whichever effect
    // next needs one -- a frame without one releases nothing it never made.
    if (lookWidth_ != renderWidth_ || lookHeight_ != renderHeight_) {
        releaseLookTextures(device);
        lookWidth_ = renderWidth_;
        lookHeight_ = renderHeight_;
    }
    // And the anti-aliasing's, with whatever history they held.
    if (aaWidth_ != renderWidth_ || aaHeight_ != renderHeight_) {
        releaseAaTextures(device);
        aaWidth_ = renderWidth_;
        aaHeight_ = renderHeight_;
    }
    // **The temporal pass is the world's main view's** (ADR 0158): a view
    // into a texture -- a `ViewportFrame`, a sub-world -- is drawn once
    // whenever it changes and keeps no history, and smooths by SMAA -- and
    // see `temporalFrame` for the rest.
    //
    // **And a part of the window's** (ADR 0205): a player's view of a shared
    // screen is drawn every frame and keeps a history of its own.
    temporalNow_ = (activeView_ == 0 || windowPart_) && temporalFrame(world);
    // **And whether a frame is made between this one and the last** (ADR
    // 0165), which needs each pixel's motion as the temporal pass does,
    // whatever smooths the frame.
    if (activeView_ == 0) {
        fgNow_ = generatesFrames(world);
        if (settings_.frameGeneration && !fgFailed_) {
            if (!fgCompute_)
                failFrameGeneration(ENG_TR("render.warn.frame_generation_unavailable"));
            else if (!fgAtomics_)
                failFrameGeneration(ENG_TR("render.warn.frame_generation_metal"));
        }
        if (!fgNow_ && fgOutput_.valid())
            releaseFrameGenerationImages(device);
    }
    // The instrument that shows each pixel's motion asks for it written, as
    // the two that read it do.
    const bool motionShown = activeView_ == 0 && settings_.debugView == DebugView::Motion && world.camera.valid;
    const bool motionNow = temporalNow_ || (fgNow_ && activeView_ == 0) || motionShown;
    if (!temporalNow_)
        historyValid_ = false;
    if (!motionNow) {
        previousCamera_ = false;
        placed_.clear();
    }
    if (fgNow_ && activeView_ == 0) {
        // The camera's part of the interpolation's constants, as FSR 2's are
        // read out of the projection (`upscaleTemporal`).
        const RenderCamera& camera = world.camera;
        fgCamera_ = GpuFrameInterpolationConstants{};
        fgCamera_.renderSize[0] = static_cast<core::i32>(renderWidth_);
        fgCamera_.renderSize[1] = static_cast<core::i32>(renderHeight_);
        fgCamera_.maxRenderSize[0] = fgCamera_.renderSize[0];
        fgCamera_.maxRenderSize[1] = fgCamera_.renderSize[1];
        fgCamera_.cameraNear = camera.nearPlane;
        fgCamera_.cameraFar = camera.farPlane;
        fgCamera_.deviceToViewDepth[0] = -camera.projection.m[2][2];
        fgCamera_.deviceToViewDepth[1] = camera.projection.m[3][2];
        fgCamera_.deviceToViewDepth[2] = 1.0f / camera.projection.m[0][0];
        fgCamera_.deviceToViewDepth[3] = 1.0f / camera.projection.m[1][1];
        fgCamera_.tanHalfFov = 1.0f / camera.projection.m[0][0];
        fgCamera_.jitter[0] = camera.jitter.x * 0.5f * static_cast<f32>(renderWidth_);
        fgCamera_.jitter[1] = -camera.jitter.y * 0.5f * static_cast<f32>(renderHeight_);
        fgCamera_.motionVectorScale[0] = -1.0f;
        fgCamera_.motionVectorScale[1] = -1.0f;
    }
    // **And the upscaler's, where the setting asks for it and the device made
    // its passes** (ADR 0164). One that cannot says so once, and the frame is
    // resolved and upscaled as it is without it from then on.
    if (activeView_ == 0) {
        fsr2Now_ = temporalNow_ && fsr2Frame(world);
        if (fsr2Now_ && !ensureFsr2(device)) {
            failFsr2();
            fsr2Now_ = false;
        }
        // Its images are the size of several frames: with the setting off
        // they are given back -- and at another size they are another
        // history's, made again where each is first needed.
        const bool held = fsr2Output_.valid() || fsr2Opaque_.valid();
        const bool resized =
            fsr2Output_.valid() && (fsr2RenderWidth_ != renderWidth_ || fsr2RenderHeight_ != renderHeight_ ||
                                    fsr2OutputWidth_ != target.width || fsr2OutputHeight_ != target.height);
        if (held && (!fsr2Now_ || resized))
            releaseFsr2Images(device);
        fsr2OpaqueLive_ = false;
        fsr2ReactiveLive_ = false;
    }

    if (!defaultsUploaded_) {
        // White multiplies to itself, (0.5, 0.5, 1) is the tangent-space normal
        // pointing straight out, and black adds nothing -- so a material with no
        // map gets a sample that changes nothing rather than an unbound read.
        const std::array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        const std::array<std::byte, 4> flat{std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}};
        const std::array<std::byte, 4> black{std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}};
        cmd.uploadTexture(whitePixel_, white, 0);
        cmd.uploadTexture(flatNormalPixel_, flat, 0);
        cmd.uploadTexture(blackPixel_, black, 0);
        const std::array<std::byte, 4> nothing{};
        cmd.uploadTexture(nothingPixel_, nothing, 0);
        defaultsUploaded_ = true;
    }

    // The sky, resolved once, and the one place its derived colours come from.
    // Both the sky pass and the prefiltered environment read this struct, which
    // is what stops a reflection from disagreeing with what it reflects.
    //
    // **With an `Atmosphere`, the horizon is the air's colour** (ADR 0096): it
    // takes `FogColor`'s place in the derivation, so the air is tinted by the
    // hour exactly as the horizon was -- warm at dusk, dark at night -- and the
    // sky, the reflections and the air laid over the world all agree on it.
    const bool air = world.look.atmosphere.present;
    SkyParams sky =
        skyParamsFor(world.environment.sunDirection, air ? world.look.atmosphere.color : world.environment.fogColor);
    sky.specularScale = world.environment.environmentSpecularScale;
    // **A `Sky` governs the sun's look and, with pictures, the sky itself**
    // (ADR 0096) -- never where the sun is.
    const RenderSky& skyLook = world.look.sky;
    if (skyLook.present) {
        setSunAngularRadius(sky, skyLook.sunAngularSize * 0.5f * kDegreesToRadians);
        sky.celestial = skyLook.celestialBodiesShown;
        if (skyLook.image.valid())
            sky.skybox = skyLook.radiance;
        // The clouds, carried by a wind of about sixteen metres a second at
        // the layer's scale, on the game's clock, folded in f64 before it
        // becomes f32 so an hour-long game drifts as smoothly as a new one.
        sky.cloudCover = skyLook.cloudCover;
        sky.cloudDensity = skyLook.cloudDensity;
        sky.cloudColor = skyLook.cloudColor;
        const core::f64 drift = std::fmod(world.environment.simTime * kCloudWind, kCloudWindWrap);
        sky.cloudDriftX = static_cast<f32>(drift);
        sky.cloudDriftZ = static_cast<f32>(drift * 0.37);
    }
    updateEnvironment(cmd, sky);
    prepareIndirect(device, cmd, world);

    // The clustered light assignment, and its three tables. Built on the CPU
    // because the frozen RHI has no compute, and uploaded unconditionally
    // because a frame's command shape must not depend on whether the lights
    // moved -- the same rule the environment upload obeys, for the same reason.
    // Grouped and uploaded BEFORE any render pass begins, because
    // `rhi.err.upload_inside_pass` refuses the alternative and is right to: the
    // GPU is rasterizing into the target by then.
    stats_ = RendererStats{};
    // The air's pipeline the first frame there is air -- before any pass,
    // because the air is laid INSIDE the forward pass's span, and a pipeline
    // made mid-pass is not.
    const bool airDrawn =
        air && world.camera.valid && ensureLookPipeline(device, air_, "look_air", kHdrFormat, LookBlend::Air);
    // And the governed sky's, for the same reason.
    // The holes and shadow views draw a plain sky, one colour: a `Sky`'s
    // pictures would put other colours where the check reads sky.
    const bool skyGoverned = skyLook.present && world.camera.valid && settings_.debugView != DebugView::Holes &&
                             !blackSky(settings_.debugView) && ensureSkyLook(device);
    prepareSurfaces(device, world);
    // The skinned runs' pipelines, the first frame there are skinned draws
    // enough to make one: a world with a character or two builds nothing, and
    // its capture goldens do not move.
    if (!skinnedInstancingTried_ && settings_.instancing &&
        std::count_if(world.draws.begin(), world.draws.end(), [](const DrawItem& draw) {
            return draw.boneCount > 0 && !draw.transparent;
        }) >= static_cast<std::ptrdiff_t>(kMinInstanceBatch))
        (void)ensureSkinnedInstancing(device);
    // And the morph pipelines (ADR 0196), **when a mesh that has targets is
    // loaded** and not the frame one of its weights first leaves nought: a
    // face that blinks for the first time in the middle of a fight would
    // otherwise pay for ten pipelines there. A mesh arrives behind whatever
    // its load is behind -- a loading curtain, `ContentProvider:PreloadAsync`
    // -- and so do these. A game with no such mesh builds nothing.
    if (!morphTried_ && (meshes.morphMeshCount() > 0 || !world.morphs.empty() || !world.cloths.empty()))
        (void)ensureMorph(device);
    // This frame's cloths (ADR 0214), up before any pass for the reason the
    // palettes are.
    if (!world.clothRows.empty()) {
        if (!clothBuffer_.valid())
            clothBuffer_ = device.createBuffer({
                .usage = rhi::BufferUsage::GraphicsStorageRead,
                .sizeBytes = kMaxClothRows * static_cast<u32>(sizeof(GpuMorphDelta)),
                .debugName = "cloth-rows",
            });
        if (clothBuffer_.valid())
            cmd.upload(clothBuffer_,
                       asBytes(world.clothRows.data(),
                               std::min<usize>(world.clothRows.size(), kMaxClothRows) * sizeof(GpuMorphDelta)),
                       0);
    }
    // The bake's pages and rows when the bake is another (ADR 0253): before
    // the runs, which are baked only against the pages this frame holds.
    // A view that carries none -- a picture of something else -- leaves the
    // pages as they are for the views that do.
    if (world.lightmap.stamp != 0 && world.lightmap.stamp != lightmapStamp_)
        uploadLightmap(device, cmd, world.lightmap);
    buildInstanceBatches(world, meshes);
    if (anyBakedRun_ && !bakedInstanceStaging_.empty()) {
        cmd.upload(
            bakedInstanceBuffer_,
            asBytes(bakedInstanceStaging_.data(), bakedInstanceStaging_.size() * sizeof(bakedInstanceStaging_[0])), 0);
    }
    if (!instanceStaging_.empty()) {
        cmd.upload(instanceBuffer_, asBytes(instanceStaging_.data(), instanceStaging_.size() * sizeof(GpuInstance)), 0);
    }
    // And every palette, once, for every skinned run of every pass to read.
    if (!skinnedInstanceStaging_.empty()) {
        cmd.upload(skinnedInstanceBuffer_,
                   asBytes(skinnedInstanceStaging_.data(), skinnedInstanceStaging_.size() * sizeof(GpuSkinnedInstance)),
                   0);
    }
    // **Every skeleton's palette, once a frame**, whatever draws it: a run, or
    // one object at a time (`bindSkinPalette`).
    if (!world.bones.empty() && world.bones.size() <= kMaxPaletteJoints && ensureSkinPalette(device))
        cmd.upload(paletteBuffer_, asBytes(world.bones.data(), world.bones.size() * sizeof(Mat4)), 0);

    // This frame's particles, up before any pass for the reason the instances
    // are. The pipeline is made the first frame there is one, so a world
    // without particles builds nothing and moves no golden.
    particleCount_ = 0;
    particleRuns_.clear();
    if (!world.particles.empty() && ensureParticles(device)) {
        particleStaging_.clear();
        const usize count = std::min(world.particles.size(), ParticleSystem::MaxDrawn);
        particleStaging_.reserve(count);
        for (usize at = 0; at < count; ++at) {
            const RenderParticle& particle = world.particles[at];
            GpuParticle gpu;
            gpu.positionSize[0] = particle.position.x;
            gpu.positionSize[1] = particle.position.y;
            gpu.positionSize[2] = particle.position.z;
            gpu.positionSize[3] = particle.size;
            for (usize channel = 0; channel < 4; ++channel)
                gpu.color[channel] = particle.color[channel];
            gpu.params[0] = particle.emission;
            gpu.params[1] = static_cast<f32>(particle.shape);
            gpu.params[2] = particle.rotation;
            gpu.params[3] = static_cast<f32>(appendParticle(particleRuns_, particle.texture, static_cast<u32>(at)));
            for (usize corner = 0; corner < 4; ++corner)
                gpu.uv[corner] = particle.uv[corner];
            gpu.light[0] = particle.lightInfluence;
            particleStaging_.push_back(gpu);
        }
        cmd.upload(particleBuffer_, asBytes(particleStaging_.data(), particleStaging_.size() * sizeof(GpuParticle)), 0);
        particleCount_ = static_cast<u32>(count);
    }

    // This frame's beams and trails (ADR 0129), on the same terms: the
    // vertices are the stream, so they go up as they are.
    ribbonVertexCount_ = 0;
    if (!world.ribbonVertices.empty() && !world.ribbonRuns.empty() && ensureRibbons(device)) {
        const usize count = std::min(world.ribbonVertices.size(), RibbonSystem::MaxVertices);
        cmd.upload(ribbonBuffer_, asBytes(world.ribbonVertices.data(), count * sizeof(RenderRibbonVertex)), 0);
        ribbonVertexCount_ = static_cast<u32>(count);
    }

    // This frame's sprites (the 2D layer), up before any pass for the same
    // reason. Past `MaxSprites` the rest are not drawn: the order is ZIndex
    // first, so what goes is the front-most, which is loud rather than subtle.
    spriteCount_ = 0;
    spriteExactLive_ = false;
    if (!world.sprites.empty() && ensureSprites(device)) {
        spriteStaging_.clear();
        const usize count = std::min<usize>(world.sprites.size(), MaxSprites);
        spriteStaging_.reserve(count);
        bool anyExact = false;
        for (usize at = 0; at < count; ++at) {
            const RenderSprite& sprite = world.sprites[at];
            anyExact = anyExact || sprite.exact;
            GpuSprite gpu;
            for (usize axis = 0; axis < 4; ++axis) {
                gpu.rect[axis] = sprite.rect[axis];
                gpu.uv[axis] = sprite.uv[axis];
                gpu.color[axis] = sprite.color[axis];
            }
            gpu.turn[0] = sprite.cosine;
            gpu.turn[1] = sprite.sine;
            gpu.turn[2] = sprite.z;
            // The shape, and four more for a sprite drawn in its own colours.
            gpu.turn[3] = static_cast<f32>(sprite.shape + (sprite.exact ? 4 : 0));
            spriteStaging_.push_back(gpu);
        }
        cmd.upload(spriteBuffer_, asBytes(spriteStaging_.data(), spriteStaging_.size() * sizeof(GpuSprite)), 0);
        spriteCount_ = static_cast<u32>(count);

        // **A frame with one exact sprite draws every sprite through the
        // pipeline that writes the mask** (ADR 0153), so they keep their order
        // among themselves. Where the pipeline or the mask cannot be made, the
        // sprites are drawn lit, as they were before there was a choice.
        if (anyExact && ensureSpritesExact(device)) {
            if (!spriteMask_.valid()) {
                spriteMask_ = device.createTexture({
                    .format = kSpriteMaskFormat,
                    .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
                    .width = renderWidth_,
                    .height = renderHeight_,
                    .debugName = "sprite-mask",
                });
            }
            spriteExactLive_ = spriteMask_.valid();
        }
    }

    // **This frame's shapes drawn on parts** (ADR 0251), up before any pass:
    // those in the scene from far to near, then those over it by `ZIndex`
    // and from far to near, in runs of one mesh and one picture.
    adornmentCount_ = 0;
    adornmentRuns_.clear();
    if (!world.adornments.empty() && ensureAdornments(device)) {
        if (!adornmentGeometryUp_) {
            cmd.upload(adornmentVertices_,
                       asBytes(adornmentCorners_.data(), adornmentCorners_.size() * sizeof(core::Vec3)), 0);
            cmd.upload(adornmentIndices_,
                       asBytes(adornmentCornerIndices_.data(), adornmentCornerIndices_.size() * sizeof(core::u16)), 0);
            adornmentGeometryUp_ = true;
        }
        adornmentOrder_.resize(world.adornments.size());
        for (u32 index = 0; index < adornmentOrder_.size(); ++index)
            adornmentOrder_[index] = index;
        std::sort(adornmentOrder_.begin(), adornmentOrder_.end(), [&](u32 a, u32 b) {
            const RenderAdornment& first = world.adornments[a];
            const RenderAdornment& second = world.adornments[b];
            if (first.onTop != second.onTop)
                return !first.onTop;
            if (first.onTop && first.zIndex != second.zIndex)
                return first.zIndex < second.zIndex;
            if (first.distance != second.distance)
                return first.distance > second.distance;
            return a < b;
        });
        if (adornmentOrder_.size() > MaxAdornments)
            adornmentOrder_.erase(adornmentOrder_.begin(),
                                  adornmentOrder_.begin() +
                                      static_cast<std::ptrdiff_t>(adornmentOrder_.size() - MaxAdornments));
        adornmentStaging_.clear();
        for (const u32 index : adornmentOrder_) {
            const RenderAdornment& shape = world.adornments[index];
            GpuAdornment gpu;
            gpu.model = shape.model;
            for (usize channel = 0; channel < 4; ++channel) {
                gpu.color[channel] = shape.color[channel];
                gpu.inner[channel] = shape.inner[channel];
                gpu.params[channel] = shape.params[channel];
            }
            const u32 at = static_cast<u32>(adornmentStaging_.size());
            if (adornmentRuns_.empty() || adornmentRuns_.back().mesh != shape.mesh ||
                adornmentRuns_.back().texture != shape.texture || adornmentRuns_.back().onTop != shape.onTop)
                adornmentRuns_.push_back(AdornmentRun{at, 0, shape.mesh, shape.texture, shape.onTop});
            adornmentRuns_.back().count += 1;
            adornmentStaging_.push_back(gpu);
        }
        adornmentCount_ = static_cast<u32>(adornmentStaging_.size());
        cmd.upload(adornmentInstances_, asBytes(adornmentStaging_.data(), adornmentCount_ * sizeof(GpuAdornment)), 0);
    }

    // This frame's world UI, up before any pass for the same reason.
    worldUiVertexCount_ = 0;
    if (!world.worldUiVertices.empty() && ensureWorldUi(device)) {
        worldUiVertexCount_ = static_cast<u32>(std::min<usize>(world.worldUiVertices.size(), MaxWorldUiVertices));
        cmd.upload(worldUiBuffer_, asBytes(world.worldUiVertices.data(), worldUiVertexCount_ * sizeof(WorldUiVertex)),
                   0);
    }

    // The block world's palette, and its pipeline the first time a block is
    // drawn -- before any pass, because a pipeline made mid-pass is not.
    if (!world.voxelColors.empty() ||
        std::any_of(world.draws.begin(), world.draws.end(), [](const DrawItem& draw) { return draw.voxelBlock; })) {
        const RenderWorld::VoxelColors missing{Color3{1.0f, 0.0f, 1.0f}, Color3{1.0f, 0.0f, 1.0f},
                                               Color3{1.0f, 0.0f, 1.0f}};
        for (u32 id = 0; id < kVoxelPaletteSize; ++id) {
            const RenderWorld::VoxelColors& colors = id < world.voxelColors.size() ? world.voxelColors[id] : missing;
            const auto put = [](f32(&slot)[4], const Color3& color) {
                slot[0] = color.r;
                slot[1] = color.g;
                slot[2] = color.b;
                slot[3] = 1.0f;
            };
            put(voxelBlocks_[id].top, colors.top);
            put(voxelBlocks_[id].side, colors.side);
            put(voxelBlocks_[id].bottom, colors.bottom);
        }
        voxelParams_.params[0] = world.voxelBlockSize;
        voxelParams_.params[1] = static_cast<f32>(kVoxelTilesPerRow);
        voxelParams_.params[2] = 1.0f / static_cast<f32>(kVoxelTilesPerRow);
        voxelParams_.params[3] = 0.5f / static_cast<f32>(kVoxelTileSize);
        (void)ensureVoxel(device);

        // **Each image into its tile, the first frame it is loaded**, and not
        // again while a block still shows it. **A full atlas takes back the
        // tile of an image no block shows any more**: a game that
        // swapped its block images went on drawing new blocks untextured once
        // it had seen 256 of them in all.
        std::vector<u32> shown;
        shown.reserve(world.voxelTextures.size() * 3);
        for (const RenderWorld::VoxelTextures& images : world.voxelTextures) {
            for (const rhi::TextureHandle texture : {images.top, images.side, images.bottom}) {
                if (texture.valid())
                    shown.push_back(texture.id);
            }
        }
        std::sort(shown.begin(), shown.end());
        const auto tileOf = [&](rhi::TextureHandle texture) -> f32 {
            if (!texture.valid())
                return -1.0f;
            for (const auto& [handle, tile] : voxelTiles_) {
                if (handle == texture.id)
                    return static_cast<f32>(tile);
            }
            if (!voxelTilePipeline_.valid())
                return -1.0f;
            auto reclaimed = voxelTiles_.end();
            if (voxelTiles_.size() >= kVoxelAtlasTiles) {
                reclaimed = std::find_if(voxelTiles_.begin(), voxelTiles_.end(), [&](const auto& entry) {
                    return !std::binary_search(shown.begin(), shown.end(), entry.first);
                });
                if (reclaimed == voxelTiles_.end())
                    return -1.0f;
            }
            const bool freshAtlas = !voxelAtlas_.valid();
            if (!voxelAtlas_.valid()) {
                voxelAtlas_ = device.createTexture({
                    .format = kVoxelAtlasFormat,
                    .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
                    .width = kVoxelAtlasSize,
                    .height = kVoxelAtlasSize,
                    .debugName = "voxel-atlas",
                });
                if (!voxelAtlas_.valid())
                    return -1.0f;
            }
            const auto tile = reclaimed != voxelTiles_.end() ? reclaimed->second : static_cast<u32>(voxelTiles_.size());
            const std::array<rhi::ColorAttachment, 1> atlasAttachment{rhi::ColorAttachment{
                .texture = voxelAtlas_,
                // The first image clears the atlas; every later one draws over
                // its own square and leaves the rest as it was.
                .loadOp = freshAtlas ? rhi::LoadOp::Clear : rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({.colorAttachments = atlasAttachment, .debugName = "voxel-tile"});
            const auto column = static_cast<f32>(tile % kVoxelTilesPerRow);
            const auto row = static_cast<f32>(tile / kVoxelTilesPerRow);
            const auto size = static_cast<f32>(kVoxelTileSize);
            cmd.setViewport({.x = column * size, .y = row * size, .width = size, .height = size});
            cmd.setScissor({.x = static_cast<core::i32>(column * size),
                            .y = static_cast<core::i32>(row * size),
                            .width = static_cast<core::i32>(kVoxelTileSize),
                            .height = static_cast<core::i32>(kVoxelTileSize)});
            cmd.setPipeline(voxelTilePipeline_);
            const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{texture, pointSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, source);
            cmd.draw(3, 1, 0, 0);
            cmd.endRenderPass();
            if (reclaimed != voxelTiles_.end())
                reclaimed->first = texture.id;
            else
                voxelTiles_.emplace_back(texture.id, tile);
            return static_cast<f32>(tile);
        };
        for (u32 id = 0; id < kVoxelPaletteSize; ++id) {
            const RenderWorld::VoxelTextures images =
                id < world.voxelTextures.size() ? world.voxelTextures[id] : RenderWorld::VoxelTextures{};
            voxelBlocks_[id].tiles[0] = tileOf(images.top);
            voxelBlocks_[id].tiles[1] = tileOf(images.side);
            voxelBlocks_[id].tiles[2] = tileOf(images.bottom);
            voxelBlocks_[id].tiles[3] = id < world.voxelColors.size() ? world.voxelColors[id].alpha : 1.0f;
        }
        voxelParams_.params[0] = world.voxelBlockSize;
        (void)ensureVoxel(device);
        // The registry, put up only when it changed: a copy pass, before any
        // other.
        if (!voxelBlockBuffer_.valid()) {
            voxelBlockBuffer_ = device.createBuffer({.usage = rhi::BufferUsage::GraphicsStorageRead,
                                                     .sizeBytes = static_cast<u32>(sizeof(voxelBlocks_)),
                                                     .debugName = "voxel-blocks"});
            voxelBlocksSent_ = false;
        }
        if (voxelBlockBuffer_.valid() &&
            (!voxelBlocksSent_ || std::memcmp(voxelBlocksUp_.data(), voxelBlocks_.data(), sizeof(voxelBlocks_)) != 0)) {
            cmd.upload(voxelBlockBuffer_, asBytes(voxelBlocks_.data(), sizeof(voxelBlocks_)), 0);
            voxelBlocksUp_ = voxelBlocks_;
            voxelBlocksSent_ = true;
        }
    }

    // The pipelines the first frame a terrain is drawn, and each terrain's
    // layers -- before any pass, because a pipeline made mid-pass is not, and
    // drawing a layer is a pass of its own. The pipelines first: the layers
    // are drawn with two of them.
    if (!world.terrains.empty()) {
        (void)ensureTerrain(device);
        ensureTerrainForward(device, world);
    }
    updateTerrainArrays(device, cmd, world);

    // The light budget, applied where the lights enter the frame. Truncation
    // rather than selection: extraction order is deterministic (R10), so which
    // lights survive a budget is the same answer on every machine and in every
    // replay of the same world.
    const std::span<const RenderLight> budgetedLights =
        world.lights.size() > settings_.lightBudget
            ? std::span<const RenderLight>(world.lights.data(), settings_.lightBudget)
            : std::span<const RenderLight>(world.lights);
    buildClusters(world.camera, budgetedLights, clusters_);

    // --- Local shadows: which lights get a tile ------------------------------
    ENG_PROFILE_NEXT(passes, "render.lights");
    //
    // **This is `PointLight.Shadows` and `SpotLight.Shadows` finally meaning
    // something** (`shadow.h`). Done
    // here rather than in `buildClusters` because it is a question about the
    // ATLAS and not about the clusters, and because the answer has to be written
    // into the light table before that table is uploaded a few lines below.
    localCandidates_.clear();
    for (const RenderLight& light : budgetedLights) {
        LocalShadowCandidate candidate;
        if (light.shadows) {
            candidate.position = light.position;
            candidate.direction = light.direction;
            candidate.range = light.range;
            candidate.cosHalfAngle = light.kind == LightKind::Spot ? light.spotCosHalfAngle : -1.0f;
        }
        // A light that casts nothing still occupies an INDEX, because the fit
        // answers with candidate indices and those have to be the light table's
        // own. Its range is zero, which the fit refuses -- so it costs a slot in
        // a vector and nothing else.
        localCandidates_.push_back(candidate);
    }
    localShadows_ = fitLocalShadows(localCandidates_);

    // The tile goes in `Color.w`, which was genuinely unused. -1 means "casts
    // nothing", which is what every light starts as.
    for (u32 index = 0; index < clusters_.lightCount; ++index)
        clusters_.lightData[static_cast<usize>(index) * 12 + 7] = -1.0f;
    for (u32 entry = 0; entry < localShadows_.count; ++entry) {
        const LocalShadow& shadow = localShadows_.entries[entry];
        if (shadow.candidate < clusters_.lightCount)
            clusters_.lightData[static_cast<usize>(shadow.candidate) * 12 + 7] = static_cast<f32>(shadow.firstTile);
    }

    cmd.uploadTexture(clusterGrid_, asBytes(clusters_.grid.data(), clusters_.grid.size() * sizeof(f32)), 0);
    cmd.uploadTexture(lightIndices_, asBytes(clusters_.indices.data(), clusters_.indices.size() * sizeof(f32)), 0);
    cmd.uploadTexture(lightData_, asBytes(clusters_.lightData.data(), clusters_.lightData.size() * sizeof(f32)), 0);

    // The cascade fit, from the camera basis. `inverse(view)` is the camera's own
    // frame; the camera sits at the origin of this space, so only its axes are
    // read out of it.
    // A headset's eyes fit from the head (`ShadowView`).
    const Mat4 cameraFrame = core::inverse(world.shadowView.has_value() ? world.shadowView->view : world.camera.view);
    ShadowFit fit;
    // From where the light comes: the moon's shadows at night (see `SkyParams`).
    fit.sunDirection = sky.lightDirection;
    fit.right = Vec3{cameraFrame.m[0][0], cameraFrame.m[0][1], cameraFrame.m[0][2]};
    fit.up = Vec3{cameraFrame.m[1][0], cameraFrame.m[1][1], cameraFrame.m[1][2]};
    // The camera looks down -Z, so its forward is the negated third axis.
    fit.forward = Vec3{-cameraFrame.m[2][0], -cameraFrame.m[2][1], -cameraFrame.m[2][2]};
    fit.spread = world.shadowView.has_value() ? world.shadowView->spread : core::viewSpread(world.camera.projection);
    fit.nearPlane = world.shadowView.has_value() ? world.shadowView->nearPlane : world.camera.nearPlane;
    fit.firstSplit = world.shadowView.has_value() ? world.shadowView->firstSplit : 0.0f;
    fit.origin = world.camera.origin;

    // What can cast, handed to the fit so a cascade can be sized to it. The two
    // numbers are already on every `DrawItem`; nothing is computed here that the
    // extraction did not compute.
    casterBounds_.clear();
    casterBounds_.reserve(world.draws.size());
    for (const DrawItem& draw : world.draws) {
        // **A coarse terrain node is not a caster the fit can use.** It is
        // hundreds of metres across, and handing it to the fit inflated every
        // cascade's depth range to its size -- the depth bias is a fraction of
        // that range, and thin casters' shadows vanished into it. Ground that
        // near is in the finer nodes; the coarse ones still draw into the map.
        if (draw.terrain && draw.boundsRadius > kTerrainCasterRadius)
            continue;
        casterBounds_.push_back(ShadowCasterBounds{draw.boundsCenter, draw.boundsRadius});
    }
    fit.casters = casterBounds_;
    fit.distance = settings_.shadowDistance;
    fit.tileResolution = settings_.shadowTileResolution;
    fit.cascadeCount = settings_.shadowCascades;

    // Last frame's fit is handed back in, which is what lets a cascade keep the
    // same box -- and therefore the same texel lattice -- while nothing has left
    // it. Held per renderer rather than globally, for the reason the
    // exposure and the environment chain are: this is the history of one view,
    // and two views in one process are two histories.
    // The cascades' residual depth bias: a headset's eye's own.
    const f32 depthBias = world.headsetEye ? kHeadsetShadowDepthBiasMetres : kShadowDepthBiasMetres;
    // The second eye of a headset frame takes the first eye's fit and map.
    const bool shadowReused = world.shadowView.has_value() && world.shadowView->reuse && headsetShadowDrawn_;
    const ShadowCascades cascades =
        shadowReused ? headsetShadowFit_ : fitShadowCascades(fit, shadowFitted_ ? &shadowFit_ : nullptr);
    shadowFit_ = cascades;
    shadowFitted_ = true;
    headsetShadowFit_ = cascades;
    headsetShadowDrawn_ = world.shadowView.has_value();

    // The foliage cull (ADR 0116), before any pass reads what it writes.
    cullFoliage(device, cmd, world, meshes);
    // Particles on the GPU (ADR 0160), stepped once a frame by the main view
    // -- a view into a texture draws the same buffers and steps nothing.
    // Or by the window's first part, in a frame that draws no main view
    // (ADR 0205).
    if (activeView_ == 0 || (windowPart_ && target.stepsFrame))
        simulateGpuParticles(device, cmd, world);

    // --- Shadow pass --------------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.shadows");
    //
    // One pass, four viewports into one 2x2 atlas -- `shadow.h` says why an
    // atlas rather than an array. Runs even with no draws, so the map is cleared
    // rather than carrying last frame's depths into a frame that samples it.
    //
    // Each cascade also culls against its OWN sphere, swept towards the sun.
    // Without that, four cascades cost four times the submission, which is the
    // exact price the instanced path elsewhere in this milestone exists to
    // remove.
    // **Not at all when no cascade is drawn** (ADR 0172): the shader looks
    // nothing up in a cascade whose far plane is out of reach, and with none
    // in reach nothing reads the atlas -- so nothing has to clear it, which on
    // a phone was a depth target of a million texels cleared and stored for
    // every frame of a game with its shadows off.
    const u32 cascadesDrawn = world.environment.globalShadows ? settings_.shadowCascades : 0u;
    // A map another eye of this frame drew is read, not drawn again.
    const u32 cascadesRendered = shadowReused ? 0u : cascadesDrawn;
    cmd.pushDebugGroup("shadow");
    if (cascadesRendered != 0)
        cmd.beginRenderPass({
            .colorAttachments = {},
            .depthStencil = {.texture = shadowMap_, .loadOp = rhi::LoadOp::Clear, .storeOp = rhi::StoreOp::Store},
            .debugName = "shadow",
        });
    if (world.camera.valid && cascadesRendered != 0) {
        f32 splits[kShadowCascadeCount + 1]{};
        shadowSplits(world.camera.nearPlane, settings_.shadowDistance, kShadowSplitLambda, splits);

        // **A cascade nothing renders into is a cascade that is cleared, and a
        // cleared depth of 1 reads as lit.** That is the whole mechanism behind
        // `shadowCascades` being a setting: the sampler needs no idea how many
        // there are, because a fragment that selects a tile nobody drew into
        // gets the same answer as one that falls outside a cascade entirely.
        // `Lighting.GlobalShadows` off (ADR 0096) is no cascade drawn, and
        // every far plane out of reach.
        for (u32 index = 0; index < cascadesRendered; ++index) {
            const auto tile = static_cast<f32>(settings_.shadowTileResolution);
            const f32 x = static_cast<f32>(index & 1u) * tile;
            const f32 y = static_cast<f32>(index >> 1u) * tile;
            cmd.setPipeline(shadowPipeline_);
            cmd.setViewport({.x = x, .y = y, .width = tile, .height = tile});
            cmd.setScissor({.x = static_cast<core::i32>(x),
                            .y = static_cast<core::i32>(y),
                            .width = static_cast<core::i32>(settings_.shadowTileResolution),
                            .height = static_cast<core::i32>(settings_.shadowTileResolution)});

            // The cascade's sphere, in the same camera-relative space the fit
            // used and the draws are in. It comes BACK from the fit now: the
            // box is sized to the casters and the sphere is sized to what the
            // camera can see, and those stopped being the same number when the
            // box learned to be smaller than the slice.
            // Shrunk by the filter's reach, the way Unity's
            // `cullingSphere.w -= filterSize` is (U-58): a fragment at the very
            // edge of a cascade has taps that step outside it, and a caster
            // culled because its centre was outside would leave those taps
            // reading empty depth.
            const f32 filterReach = cascades.texelWorld[index] * kShadowFilterMaxTexels;
            // And swept towards the sun as far as the map's depth goes:
            // what stands between the sphere and the sun casts into it.
            const CullSphere cull{cascades.cullCentre[index], cascades.cullRadius[index] + filterReach,
                                  core::normalize(fit.sunDirection) * cascades.casterReach[index]};
            // **The terrain has no far side to store.** Every mesh here culls
            // its front faces, so the depth in the map is the back of a solid
            // and a lit surface never shadows itself. The ground is one
            // surface: drawn as it is, the map holds exactly the depth the
            // ground is then compared against. So it is pushed away from the
            // light by its slope times this cascade's filter reach
            // (`terrain_shadow.hlsl`). Six texels for every slope, which this
            // replaced, speckled ground at a low sun and detached shadows by
            // metres in the far cascade.
            terrainShadowPush_ = terrainCascadePush(cascades.texelWorld[index], cascades.depthRange[index],
                                                    shadowPenumbra(world.environment.shadowSoftness));
            // **What is baked casts into the nearest cascade alone** while the
            // mask holds (ADR 0253 §4): there the map is what what moves near
            // the player is shadowed by, sharp; further out the bake's grid is.
            shadowLeavesBaked_ = world.lightmap.maskHolds && sunGridRows_[2][3] > 0.5f &&
                                 (settings_.measuredSkip & MeasureSkip::BakedCasters) == 0u &&
                                 (index > 0 || (settings_.measuredSkip & MeasureSkip::BakedNear) != 0u);
            drawGeometry(cmd, world, meshes, cascades.viewProjection[index], shadowPipeline_, shadowSkinnedPipeline_,
                         Selection::Shadow, &cull);
            shadowLeavesBaked_ = false;
            // Foliage casts only near the camera: the shader drops what lies
            // past `foliage_shadow_distance`, so the far cascades get none.
            if (splits[index] < world.foliageShadowDistance)
                drawFoliage(cmd, world, meshes, cascades.viewProjection[index], FoliagePass::Shadow);
        }
    }
    if (cascadesRendered != 0)
        cmd.endRenderPass();
    cmd.popDebugGroup();

    // --- Local shadow pass ---------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.local_shadows");
    //
    // The same shape as the pass above and a different fit: one target, one
    // viewport per tile. **It runs even with nothing to draw**, for the reason
    // the cascade pass does -- a cleared depth of 1 reads as lit, so a tile
    // nobody rendered into is a light that shadows nothing rather than a light
    // that shadows everything with last frame's depths.
    //
    // Each tile culls against its own light's sphere. Without that, a scene with
    // six casting lights would submit its whole geometry six times, which is the
    // cost that makes a tile budget necessary rather than nice.
    //
    // **And not at all with no light casting** (ADR 0172): the shader skips
    // the lookup when no tile is live, so an atlas nobody reads was being
    // cleared and stored -- four million texels of depth -- every frame of
    // every scene without a casting lamp, which is most of them.
    cmd.pushDebugGroup("local-shadow");
    if (localShadows_.count != 0)
        cmd.beginRenderPass({
            .colorAttachments = {},
            .depthStencil = {.texture = localShadowMap_, .loadOp = rhi::LoadOp::Clear, .storeOp = rhi::StoreOp::Store},
            .debugName = "local-shadow",
        });
    localShadowPass_ = true;
    for (u32 entry = 0; entry < localShadows_.count; ++entry) {
        const LocalShadow& shadow = localShadows_.entries[entry];
        const LocalShadowCandidate& candidate = localCandidates_[shadow.candidate];
        for (u32 face = 0; face < shadow.tileCount; ++face) {
            const LocalShadowTileRect rect = localShadowTileRect(shadow.firstTile + face);
            cmd.setPipeline(shadowPipeline_);
            cmd.setViewport({.x = static_cast<f32>(rect.x),
                             .y = static_cast<f32>(rect.y),
                             .width = static_cast<f32>(rect.width),
                             .height = static_cast<f32>(rect.height)});
            cmd.setScissor({.x = static_cast<core::i32>(rect.x),
                            .y = static_cast<core::i32>(rect.y),
                            .width = static_cast<core::i32>(rect.width),
                            .height = static_cast<core::i32>(rect.height)});
            const CullSphere cull{candidate.position, candidate.range};
            // The terrain pushed by its slope here too: a lamp over the ground
            // meets most of it at a grazing angle.
            terrainShadowPush_ = terrainLocalPush();
            drawGeometry(cmd, world, meshes, shadow.viewProjection[face], shadowPipeline_, shadowSkinnedPipeline_,
                         Selection::Shadow, &cull);
        }
    }
    localShadowPass_ = false;
    if (localShadows_.count != 0)
        cmd.endRenderPass();
    cmd.popDebugGroup();

    // --- Depth prepass -------------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.prepass");
    //
    // **This is the roadmap's design constraint, answered.** The scene's depth
    // has to be samplable by a later pass, and a prepass is what makes that
    // possible without asking the frozen RHI for a read-only depth state: depth
    // is written here, sampled by the occlusion pass, and attached again by the
    // forward pass -- never a texture and an attachment at the same time.
    //
    // What it costs is a second geometry submission, which is CPU work in the
    // exact place the instanced path exists to reduce. What it buys, besides the
    // constraint, is early-Z rejection for the forward pass.
    //
    // **`[debug] skip = "depth_prepass"`** (ADR 0171) leaves it out on a frame
    // where nothing reads the depth before the forward pass -- no ambient
    // occlusion, no contact shadow, no motion written: the forward pass then
    // clears the depth and writes its own. On a tile-based GPU a pass is a
    // load and a store and the scene's triangles binned once more, and
    // whether early rejection pays that back is a phone's to say.
    const bool orthographicView = core::isOrthographic(world.camera.projection);
    const bool depthRead = (settings_.ambientOcclusion && !orthographicView) || motionNow ||
                           (settings_.contactShadows && world.camera.valid && settings_.shadowCascades != 0 &&
                            !orthographicView && world.environment.globalShadows);
    // **A view seen on one side of a line** (ADR 0205) always has it: the
    // prepass is where the other side is taken out, before anything is lit.
    const bool sided = (target.sideNormal.x != 0.0f || target.sideNormal.y != 0.0f) && ensureViewSide(device);
    // **Never on a standalone headset's eye** where nothing reads it: on a
    // tiled GPU the prepass is the scene binned once more and a depth target
    // of three million texels stored, an eye. The forward pass clears and
    // writes the depth itself, which is the depth the runtime is given.
    // Nor in a lean window, for the same reason.
    const bool leanEye = (world.headsetEye && world.headsetLean) || (!world.headsetEye && settings_.leanPath);
    prepassDrawn_ = depthRead || sided || (!leanEye && (settings_.measuredSkip & MeasureSkip::DepthPrepass) == 0u);
    if (prepassDrawn_) {
        cmd.pushDebugGroup("depth-prepass");
        cmd.beginRenderPass({
            .colorAttachments = {},
            .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Clear, .storeOp = rhi::StoreOp::Store},
            .debugName = "depth-prepass",
        });
        cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
        cmd.setScissor(
            {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
        if (sided) {
            // The near plane over the side nobody sees: every surface, the
            // sky and everything blended fails its depth test there, in this
            // pass and in every pass after it.
            GpuViewSideUniforms side;
            side.line[0] = target.sideNormal.x;
            side.line[1] = target.sideNormal.y;
            side.line[2] = target.sideMargin;
            side.size[0] = static_cast<f32>(target.width);
            side.size[1] = static_cast<f32>(target.height);
            cmd.setPipeline(viewSidePipeline_);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&side, sizeof(side)));
            cmd.draw(3, 1, 0, 0);
        }
        if (world.camera.valid) {
            cmd.setPipeline(depthPrepassPipeline_);
            drawGeometry(cmd, world, meshes, world.camera.viewProjection, depthPrepassPipeline_,
                         depthPrepassSkinnedPipeline_, Selection::Prepass);
        }
        cmd.endRenderPass();
        cmd.popDebugGroup();
    }

    // **How far each pixel moved**, for the temporal pass (ADR 0158): from the
    // depth just drawn, and what moved by itself over it.
    if (motionNow) {
        ENG_PROFILE_NEXT(passes, "render.velocity");
        writeVelocity(device, cmd, world, meshes);
    }

    // --- Ambient occlusion ---------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.ao");
    //
    // Half resolution, sixteen taps, then a depth-aware blur in two separable
    // passes. The result multiplies the environment and the ambient and nothing
    // else -- the sun has a shadow map that answers whether IT reaches a surface.
    cmd.pushDebugGroup("occlusion");
    const u32 occlusionWidth = renderWidth_ > 1 ? renderWidth_ / 2 : 1;
    const u32 occlusionHeight = renderHeight_ > 1 ? renderHeight_ / 2 : 1;
    // The screen-space passes rebuild a position from depth as a perspective
    // camera made it, and an orthographic one (the 2D layer) is not that --
    // so under one they are off rather than wrong.
    const bool orthographic = core::isOrthographic(world.camera.projection);
    if (!settings_.ambientOcclusion || orthographic) {
        // White is "nothing is occluded", which is what the forward pass
        // multiplies its ambient term by when this one is switched off.
        // **Once, not every frame** (ADR 0172): a target nothing has drawn to
        // since it was cleared is still clear.
        if (!occlusionOff_) {
            clearPass(cmd, occlusion_, occlusionWidth, occlusionHeight, "occlusion-off",
                      rhi::ColorRgba{1.0f, 1.0f, 1.0f, 1.0f});
            occlusionOff_ = true;
        }
    }
    else {
        occlusionOff_ = false;
        GpuSsaoUniforms ssao;
        ssao.projection[0] = world.camera.projection.m[0][0] != 0.0f ? 1.0f / world.camera.projection.m[0][0] : 1.0f;
        ssao.projection[1] = world.camera.projection.m[1][1] != 0.0f ? 1.0f / world.camera.projection.m[1][1] : 1.0f;
        ssao.projection[2] = world.camera.nearPlane;
        ssao.projection[3] = world.camera.farPlane;
        // **The view's middle off its axis** (an eye's projection, whose two
        // sides are at different angles): positions rebuilt from depth as if
        // it were centred are beside where they are, and a headset's eye drew
        // its occlusion and its contact shadows as columns down the walls.
        ssao.center[0] = world.camera.projection.m[2][0];
        ssao.center[1] = world.camera.projection.m[2][1];
        ssao.viewport[0] = static_cast<f32>(occlusionWidth);
        ssao.viewport[1] = static_cast<f32>(occlusionHeight);
        ssao.viewport[2] = 1.0f / static_cast<f32>(occlusionWidth);
        ssao.viewport[3] = 1.0f / static_cast<f32>(occlusionHeight);
        ssao.params[0] = kOcclusionRadius;
        ssao.params[1] = kOcclusionBias;
        ssao.params[2] = kOcclusionStrength;

        const std::array<rhi::TextureBinding, 1> depthBinding{rhi::TextureBinding{depth_, pointSampler_}};
        fullscreenPass(cmd, ssaoPipeline_, occlusion_, occlusionWidth, occlusionHeight, "ssao", depthBinding,
                       asBytes(&ssao, sizeof(ssao)));

        GpuBlurUniforms blur;
        blur.texelDirection[0] = 1.0f / static_cast<f32>(occlusionWidth);
        blur.texelDirection[1] = 1.0f / static_cast<f32>(occlusionHeight);
        blur.texelDirection[2] = 1.0f;
        blur.texelDirection[3] = 0.0f;
        const std::array<rhi::TextureBinding, 2> horizontal{rhi::TextureBinding{occlusion_, linearSampler_},
                                                            rhi::TextureBinding{depth_, pointSampler_}};
        fullscreenPass(cmd, ssaoBlurPipeline_, occlusionBlur_, occlusionWidth, occlusionHeight, "ssao-blur-x",
                       horizontal, asBytes(&blur, sizeof(blur)));

        blur.texelDirection[2] = 0.0f;
        blur.texelDirection[3] = 1.0f;
        const std::array<rhi::TextureBinding, 2> vertical{rhi::TextureBinding{occlusionBlur_, linearSampler_},
                                                          rhi::TextureBinding{depth_, pointSampler_}};
        fullscreenPass(cmd, ssaoBlurPipeline_, occlusion_, occlusionWidth, occlusionHeight, "ssao-blur-y", vertical,
                       asBytes(&blur, sizeof(blur)));
    }
    cmd.popDebugGroup();

    // --- Contact shadows -----------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.contact_shadows");
    //
    // The sun's last few centimetres, from the prepass depth (contact_shadow.hlsl).
    // After the occlusion pass because it reads the same depth, and before the
    // forward pass because the forward pass samples what it writes. Off, it is
    // a white clear, which the forward pass's `min` then ignores.
    cmd.pushDebugGroup("contact-shadow");
    if (!settings_.contactShadows || !world.camera.valid || settings_.shadowCascades == 0 || orthographic ||
        !world.environment.globalShadows) {
        if (!contactOff_) {
            clearPass(cmd, contact_, renderWidth_, renderHeight_, "contact-off",
                      rhi::ColorRgba{1.0f, 1.0f, 1.0f, 1.0f});
            contactOff_ = true;
        }
    }
    else {
        contactOff_ = false;
        GpuContactUniforms contact;
        contact.projection[0] = world.camera.projection.m[0][0] != 0.0f ? 1.0f / world.camera.projection.m[0][0] : 1.0f;
        contact.projection[1] = world.camera.projection.m[1][1] != 0.0f ? 1.0f / world.camera.projection.m[1][1] : 1.0f;
        contact.projection[2] = world.camera.nearPlane;
        contact.projection[3] = world.camera.farPlane;
        contact.center[0] = world.camera.projection.m[2][0];
        contact.center[1] = world.camera.projection.m[2][1];
        // Towards the sun, turned into the camera's view space: the view
        // matrix's rotation, applied to a direction.
        const Vec3 sun = sky.lightDirection;
        const Mat4& view = world.camera.view;
        const Vec3 viewSun{view.m[0][0] * sun.x + view.m[1][0] * sun.y + view.m[2][0] * sun.z,
                           view.m[0][1] * sun.x + view.m[1][1] * sun.y + view.m[2][1] * sun.z,
                           view.m[0][2] * sun.x + view.m[1][2] * sun.y + view.m[2][2] * sun.z};
        const f32 sunLength = core::length(viewSun);
        contact.sun[0] = sunLength > 0.0f ? viewSun.x / sunLength : 0.0f;
        contact.sun[1] = sunLength > 0.0f ? viewSun.y / sunLength : 1.0f;
        contact.sun[2] = sunLength > 0.0f ? viewSun.z / sunLength : 0.0f;
        contact.sun[3] = kContactRayMetres;
        contact.params[0] = kContactThicknessMetres;
        // A light that lights nothing shadows nothing: a sun below the horizon
        // before the moon has come up.
        contact.params[1] = sky.lightPresence;
        contact.params[2] = kContactFadeDistance;
        contact.params[3] = 1.0f;
        // How far each cascade's bias reaches along the light: its
        // constant, its slope term at its steepest, and the filter's radius
        // twice -- the kernel reads that far to the side of the pixel, and on
        // a wall the sun grazes a texel to the side is two along the light.
        const f32 penumbra = shadowPenumbra(world.environment.shadowSoftness);
        for (u32 index = 0; index < 4; ++index) {
            const bool drawn = index < cascadesDrawn;
            const f32 texel = cascades.texelWorld[index];
            const f32 kernel =
                texel > 0.0f ? std::clamp(penumbra / texel, kShadowFilterMinTexels, kShadowFilterMaxTexels) : 0.0f;
            contact.cascadeFar[index] = drawn ? cascades.farDistance[index] : kUnreachableDistance;
            contact.biasReach[index] =
                drawn ? depthBias + texel * (kShadowSlopeTexels * kShadowMostSlope + 2.0f * kernel) : 0.0f;
        }
        const std::array<rhi::TextureBinding, 1> depthBinding{rhi::TextureBinding{depth_, pointSampler_}};
        fullscreenPass(cmd, contactPipeline_, contact_, renderWidth_, renderHeight_, "contact-shadow", depthBinding,
                       asBytes(&contact, sizeof(contact)));
    }
    cmd.popDebugGroup();

    // --- What is read from the screen (ADR 0209) -----------------------------
    ENG_PROFILE_NEXT(passes, "render.screen_light");
    traceScreen(device, cmd, world);

    // --- Sky and forward PBR ------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.forward");

    // Cleared to nothing -- zero coverage -- for a view with no sky behind it;
    // the sky covers every other view's background whatever this is.
    const bool clearBehind = world.environment.transparentBackground;
    const std::array<rhi::ColorAttachment, 1> hdrAttachment{rhi::ColorAttachment{
        .texture = hdr_,
        .loadOp = rhi::LoadOp::Clear,
        .storeOp = rhi::StoreOp::Store,
        .clearColor = {0.0f, 0.0f, 0.0f, clearBehind ? 0.0f : 1.0f},
    }};

    // **A headset's eye drawn multisampled** (`RenderWorld::headsetSamples`):
    // the forward pass into the samples and resolved into `hdr_` on the tile,
    // the samples and their depth never stored. Only where that pass can be
    // the whole of the forward group -- nothing it draws reads the depth or
    // the picture partway through, which would close and reopen it on a
    // depth of one sample -- and nothing reads the eye's depth after it.
    bool readsScene = false;
    for (const DrawItem& draw : world.draws) {
        if (draw.transparent && draw.inCameraFrustum && draw.material < materialSurface_.size() &&
            materialSurface_[draw.material] != 0) {
            readsScene = true;
            break;
        }
    }
    const bool forwardWhole = world.decals.empty() && !airDrawn && !spriteExactLive_ && particleCount_ == 0 &&
                              ribbonVertexCount_ == 0 && world.gpuEmitters.empty() && !readsScene && !prepassDrawn_ &&
                              !(world.look.depthOfField && settings_.depthOfField);
    u32 samples = world.headsetEye && !world.headsetDepthRead && forwardWhole ? world.headsetSamples
                  : leanWindow && forwardWhole ? std::clamp(settings_.leanSamples, 1u, 8u)
                                               : 1u;
    // **The tonemap folded into the pass** (ADR 0250): a headset's eye drawn
    // with samples straight into its runtime's 8-bit image -- the forward
    // shaders expose, tonemap and dither what they write, the samples resolve
    // into the image, and neither the 16-bit target nor the tonemap pass is
    // in the frame (1.1 to 1.5 ms an eye on a Quest 3). Only where nothing
    // drawn needs the frame's light after the pass -- no grade, blur, bloom
    // or rays -- and only what knows how to fold is drawn: the lit surfaces,
    // the sky and the world's UI, not ground, blocks, foliage, sprites or a
    // surface shader. The exposure is the meter's, read back a few frames
    // late, or the fixed one; until a reading has come, not folded.
    {
        // Taken by a frame's first eye only: the second is drawn at the
        // exposure the first was, as it is when not folded.
        const bool secondEye = world.shadowView.has_value() && world.shadowView->reuse;
        std::array<std::byte, 4> reading{};
        while (!secondEye && !foldReadbacks_.empty() && device.takeReadback(foldReadbacks_.front(), reading)) {
            foldReadbacks_.erase(foldReadbacks_.begin());
            f32 measured = 0.0f;
            std::memcpy(&measured, reading.data(), sizeof(measured));
            if (measured > 0.0f && std::isfinite(measured)) {
                foldMeasured_ = measured;
                foldMeasuredKnown_ = true;
            }
        }
        const bool metering = settings_.autoExposure && world.environment.autoExposure;
        const f32 compensation = std::exp2(world.environment.exposureCompensation);
        const f32 exposure = !metering            ? kExposureKey * compensation
                             : foldMeasuredKnown_ ? kExposureKey / std::max(foldMeasured_, 1e-4f) * compensation
                                                  : 0.0f;
        // The samples resolve into the image itself, so at its own size: an
        // eye drawn smaller (`[xr] render_scale`) is brought up by the
        // tonemap pass, and is not folded.
        const bool srgbImage = target.colorFormat == rhi::TextureFormat::Rgba8UnormSrgb ||
                               target.colorFormat == rhi::TextureFormat::Bgra8UnormSrgb;
        // A lean window's swapchain is plain 8-bit: written encoded.
        const bool plainImage = leanWindow && (target.colorFormat == rhi::TextureFormat::Rgba8Unorm ||
                                               target.colorFormat == rhi::TextureFormat::Bgra8Unorm);
        // Metered, the image is read back for the next frames' exposure, so
        // it must be one that can be: a window's swapchain is not, and is
        // folded only at a fixed exposure.
        const bool intoImage = target.color.valid() && (target.colorReadable || !metering) &&
                               (srgbImage || plainImage) && renderWidth_ == target.width &&
                               renderHeight_ == target.height;
        foldEncoded_ = plainImage;
        bool foldable = world.terrains.empty() && world.foliageRuns.empty() && world.sprites.empty();
        for (const DrawItem& draw : world.draws) {
            if (!foldable)
                break;
            if (draw.terrain || draw.voxelBlock ||
                (draw.material < materialSurface_.size() && materialSurface_[draw.material] != 0))
                foldable = false;
        }
        const bool fold = (settings_.measuredSkip & MeasureSkip::Fold) == 0u && samples > 1 &&
                          ((world.headsetEye && world.headsetLean) || leanWindow) &&
                          !world.environment.transparentBackground && intoImage && exposure > 0.0f && foldable &&
                          !world.look.graded && world.look.blurSize <= 0.0f &&
                          !(settings_.bloom && world.look.bloomEnabled) && !(settings_.sunRays && world.look.sunRays);
        foldExposure_ = fold ? exposure : 0.0f;
    }
    const rhi::TextureFormat samplesFormat = foldExposure_ > 0.0f ? target.colorFormat : kHdrFormat;
    // **Shaded less away from where the eye looks** (`headsetFoveation`), on
    // a device with a fragment density map: the map made at its texel size,
    // filled from the eye's own middle -- where its view's axis meets the
    // screen, off the picture's centre for an eye that sees further out
    // than in.
    const rhi::Capabilities caps = device.caps();
    const u32 foveation = samples > 1 && caps.densityTexelWidth > 0 && caps.densityTexelHeight > 0
                              ? std::min(world.headsetFoveation, 3u)
                              : 0u;
    const f32 centreU = 0.5f * (1.0f - world.camera.projection.m[2][0]);
    const f32 centreV = 0.5f * (1.0f + world.camera.projection.m[2][1]);
    bool densityRemade = false;
    if (foveation == 0) {
        if (densityMap_.valid()) {
            device.destroy(densityMap_);
            densityMap_ = {};
            densityRemade = true;
        }
    }
    else {
        const u32 mapWidth = (renderWidth_ + caps.densityTexelWidth - 1) / caps.densityTexelWidth;
        const u32 mapHeight = (renderHeight_ + caps.densityTexelHeight - 1) / caps.densityTexelHeight;
        const bool resized = msaaWidth_ != renderWidth_ || msaaHeight_ != renderHeight_;
        if (!densityMap_.valid() || resized) {
            if (densityMap_.valid())
                device.destroy(densityMap_);
            densityMap_ = device.createTexture({
                .format = rhi::TextureFormat::Rg8Unorm,
                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::DensityMap,
                .width = mapWidth,
                .height = mapHeight,
                .debugName = "eye-density",
            });
            densityRemade = true;
            densityLevel_ = 0;
        }
        if (densityMap_.valid() &&
            (densityRemade || densityLevel_ != foveation || std::fabs(densityCentre_[0] - centreU) > 0.01f ||
             std::fabs(densityCentre_[1] - centreV) > 0.01f)) {
            // Full density inside the first ring, a fragment for two pixels
            // each way out to the second, and for four beyond it -- the
            // rings in the eye's own proportion, wider with the level.
            const f32 inner = foveation == 3 ? 0.30f : foveation == 2 ? 0.38f : 0.45f;
            const f32 outer = foveation == 3 ? 0.48f : foveation == 2 ? 0.58f : 2.0f;
            std::vector<std::byte> texels(static_cast<usize>(mapWidth) * mapHeight * 2u);
            for (u32 y = 0; y < mapHeight; ++y) {
                for (u32 x = 0; x < mapWidth; ++x) {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(mapWidth) - centreU;
                    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(mapHeight) - centreV;
                    const f32 reach = std::sqrt(u * u + v * v);
                    const auto density = static_cast<std::byte>(reach < inner ? 255 : reach < outer ? 128 : 64);
                    const usize at = (static_cast<usize>(y) * mapWidth + x) * 2u;
                    texels[at] = density;
                    texels[at + 1] = density;
                }
            }
            cmd.uploadTexture(densityMap_, texels, 0);
            densityLevel_ = foveation;
            densityCentre_[0] = centreU;
            densityCentre_[1] = centreV;
        }
    }
    // The samples are remade with the map that shades them, or without one.
    const bool densityChanged = densityRemade;
    if (samples > 1 &&
        (!msaaColor_.valid() || !msaaDepth_.valid() || msaaSamples_ != samples || msaaWidth_ != renderWidth_ ||
         msaaHeight_ != renderHeight_ || densityChanged || msaaFormat_ != samplesFormat)) {
        for (rhi::TextureHandle* texture : {&msaaColor_, &msaaDepth_}) {
            if (texture->valid())
                device.destroy(*texture);
            *texture = {};
        }
        msaaColor_ = device.createTexture({
            .format = samplesFormat,
            .usage = rhi::TextureUsage::ColorTarget,
            .width = renderWidth_,
            .height = renderHeight_,
            .samples = samples,
            .densityMap = densityMap_,
            .debugName = "eye-samples",
        });
        msaaFormat_ = samplesFormat;
        msaaDepth_ = device.createTexture({
            .format = kDepthFormat,
            .usage = rhi::TextureUsage::DepthStencilTarget,
            .width = renderWidth_,
            .height = renderHeight_,
            .samples = samples,
            .debugName = "eye-depth-samples",
        });
        msaaSamples_ = samples;
        msaaWidth_ = renderWidth_;
        msaaHeight_ = renderHeight_;
    }
    if (!msaaColor_.valid() || !msaaDepth_.valid()) {
        samples = 1;
        foldExposure_ = 0.0f;
    }
    // A lean window's samples are its anti-aliasing: no pass after them.
    if (leanWindow && samples > 1)
        settings_.antiAliasing = AntiAliasingMode::Off;
    // **The world's interface, as the display shows it or as light** (ADR
    // 0250): a run at a brightness of 1 or less is drawn as authored. Folded,
    // every run is drawn in the pass, in the order it came; otherwise those
    // are drawn apart, into their own layer, after the forward group -- the
    // samples' depth kept for it -- and the brighter ones in the scene.
    bool uiAsDisplayed = false;
    bool uiAsLight = false;
    if (worldUiVertexCount_ > 0) {
        for (const WorldUiRun& run : world.worldUiRuns) {
            if (run.vertexCount == 0)
                continue;
            (shownAsDisplayed(run) ? uiAsDisplayed : uiAsLight) = true;
        }
    }
    // And the shapes drawn on parts (ADR 0251), as the display shows them.
    if (adornmentCount_ > 0)
        uiAsDisplayed = true;
    const bool uiApart = uiAsDisplayed && foldExposure_ <= 0.0f;
    const bool uiInForward = uiAsLight || (uiAsDisplayed && !uiApart);
    const std::array<rhi::ColorAttachment, 1> samplesAttachment{rhi::ColorAttachment{
        .texture = msaaColor_,
        .loadOp = rhi::LoadOp::Clear,
        .storeOp = rhi::StoreOp::Resolve,
        .clearColor = {0.0f, 0.0f, 0.0f, clearBehind ? 0.0f : 1.0f},
        .resolveTexture = foldExposure_ > 0.0f ? target.color : hdr_,
    }};

    // Whether the depth of field ran inside the forward pass, before what
    // blends; the look's own passes below do not run it a second time.
    bool focusedBeforeBlended = false;
    // Whether a pass of the forward group is open, for its end to close: the
    // particles leave none open when nothing is drawn after them.
    bool forwardOpen = true;
    cmd.pushDebugGroup("forward");
    if (samples > 1)
        cmd.beginRenderPass({
            .colorAttachments = samplesAttachment,
            .depthStencil = {.texture = msaaDepth_,
                             .loadOp = rhi::LoadOp::Clear,
                             .storeOp = uiApart ? rhi::StoreOp::Store : rhi::StoreOp::DontCare},
            .debugName = "forward-samples",
        });
    else
        cmd.beginRenderPass({
            .colorAttachments = hdrAttachment,
            // LOADED, not cleared: the prepass wrote this depth and the occlusion
            // pass has already read it -- unless a measurement left the prepass
            // out, and this pass is the first to draw depth. Stored, because the
            // blended pass tests against it.
            .depthStencil = {.texture = depth_,
                             .loadOp = prepassDrawn_ ? rhi::LoadOp::Load : rhi::LoadOp::Clear,
                             .storeOp = rhi::StoreOp::Store},
            .debugName = "forward",
        });
    cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
    cmd.setScissor({.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});

    if (world.camera.valid) {
        GpuSkyUniforms skyUniforms;
        // The sky shader turns a screen position back into a world direction,
        // so it needs the inverse. Computed once per frame rather than per
        // pixel, which is the only reason it is a uniform rather than a
        // derivation.
        skyUniforms.inverseViewProjection = core::inverse(world.camera.skyViewProjection);
        skyUniforms.sunDirectionSize[0] = sky.sunDirection.x;
        skyUniforms.sunDirectionSize[1] = sky.sunDirection.y;
        skyUniforms.sunDirectionSize[2] = sky.sunDirection.z;
        skyUniforms.sunDirectionSize[3] = sky.sunAngularRadius;
        skyUniforms.horizonColor[0] = sky.horizonColor.r;
        skyUniforms.horizonColor[1] = sky.horizonColor.g;
        skyUniforms.horizonColor[2] = sky.horizonColor.b;
        skyUniforms.zenithColor[0] = sky.zenithColor.r;
        skyUniforms.zenithColor[1] = sky.zenithColor.g;
        skyUniforms.zenithColor[2] = sky.zenithColor.b;
        // w: the tonemap folded into this pass (`foldOutput`).
        skyUniforms.zenithColor[3] = foldForShaders();
        skyUniforms.sunColor[0] = sky.sunColor.r;
        skyUniforms.sunColor[1] = sky.sunColor.g;
        skyUniforms.sunColor[2] = sky.sunColor.b;
        // The disc's brightness relative to the sky around it, scaled by the day
        // factor so a sun below the horizon leaves no disc behind.
        skyUniforms.sunColor[3] = kSunDiscIntensity * sky.dayFactor;
        // **Magenta, for the holes view**: a colour no
        // ground is, so every pixel of it in a picture is sky. Black for the
        // shadow, bend, albedo and material views, where magenta is a colour
        // ground is (`sky.hlsl`).
        skyUniforms.horizonColor[3] = settings_.debugView == DebugView::Holes ? 1.0f
                                      : blackSky(settings_.debugView)         ? 2.0f
                                                                              : 0.0f;
        if (clearBehind) {
            // No sky behind a view that shows only its instances (ADR 0107):
            // what nothing draws stays clear.
        }
        else if (skyGoverned) {
            // **The sky a `Sky` governs** (ADR 0096): its pictures or the
            // gradient, and its sun, moon and stars, through its own pipeline.
            GpuLookSkyUniforms lookSky;
            lookSky.flags[0] = skyLook.image.valid() ? 1.0f : 0.0f;
            lookSky.flags[1] = skyLook.celestialBodiesShown ? 1.0f : 0.0f;
            lookSky.flags[2] = skyLook.sunImage.valid() ? 1.0f : 0.0f;
            lookSky.flags[3] = skyLook.moonImage.valid() ? 1.0f : 0.0f;
            // The moon stands opposite the sun, as the light model has it.
            lookSky.moon[0] = -sky.sunDirection.x;
            lookSky.moon[1] = -sky.sunDirection.y;
            lookSky.moon[2] = -sky.sunDirection.z;
            lookSky.moon[3] = skyLook.moonAngularSize * 0.5f * kDegreesToRadians;
            // Night is what shows the moon and the stars: none of either while
            // the day factor is up, all of them once it has gone.
            const f32 night = 1.0f - sky.dayFactor;
            lookSky.moonColor[0] = 0.78f * kMoonGlow;
            lookSky.moonColor[1] = 0.82f * kMoonGlow;
            lookSky.moonColor[2] = 0.9f * kMoonGlow;
            lookSky.moonColor[3] = night;
            // About StarCount stars over the whole sphere: six faces of cells,
            // half of them holding one.
            const f32 cells = std::sqrt(static_cast<f32>(skyLook.starCount) / (6.0f * kStarChance));
            lookSky.stars[0] = skyLook.starCount > 0 ? std::max(cells, 1.0f) : 0.0f;
            lookSky.stars[1] = kStarChance;
            lookSky.stars[2] = night * night * kStarGlow;
            lookSky.clouds[0] = sky.cloudCover;
            lookSky.clouds[1] = sky.cloudDensity;
            lookSky.clouds[2] = sky.cloudDriftX;
            lookSky.clouds[3] = sky.cloudDriftZ;
            lookSky.cloudColor[0] = sky.cloudColor.r;
            lookSky.cloudColor[1] = sky.cloudColor.g;
            lookSky.cloudColor[2] = sky.cloudColor.b;
            lookSky.cloudColor[3] = kCloudSunLight;
            cmd.setPipeline(sided && skyLookSided_.handle.valid() ? skyLookSided_.handle : skyLook_.handle);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&skyUniforms, sizeof(skyUniforms)));
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 1, asBytes(&lookSky, sizeof(lookSky)));
            const std::array<rhi::TextureBinding, 3> skyTextures{
                rhi::TextureBinding{skyLook.image.valid() ? skyLook.image : whitePixel_, environmentSampler_},
                rhi::TextureBinding{skyLook.sunImage.valid() ? skyLook.sunImage : whitePixel_, environmentSampler_},
                rhi::TextureBinding{skyLook.moonImage.valid() ? skyLook.moonImage : whitePixel_, environmentSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, skyTextures);
            cmd.draw(3, 1, 0, 0);
        }
        else {
            cmd.setPipeline(sided && skySidedPipeline_.valid() ? skySidedPipeline_ : skyPipeline_);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&skyUniforms, sizeof(skyUniforms)));
            cmd.draw(3, 1, 0, 0);
        }

        GpuFrameUniforms frame;
        frame.sunDirectionBrightness[0] = sky.lightDirection.x;
        frame.sunDirectionBrightness[1] = sky.lightDirection.y;
        frame.sunDirectionBrightness[2] = sky.lightDirection.z;
        // The day factor is folded in here rather than tested in the shader: a
        // sun below the horizon is a sun that lights nothing, and before the lighting work it
        // went on lighting every upward-facing surface from underneath.
        // The moon by night, at its own small fraction (see `SkyParams`).
        frame.sunDirectionBrightness[3] = world.environment.sunBrightness * sky.lightFactor;
        frame.sunColorUnused[0] = sky.lightColor.r * world.environment.lightTint.r;
        frame.sunColorUnused[1] = sky.lightColor.g * world.environment.lightTint.g;
        frame.sunColorUnused[2] = sky.lightColor.b * world.environment.lightTint.b;
        // w: the cascades in one row of the atlas rather than two by two.
        frame.sunColorUnused[3] = shadowRows_ == 1 ? 1.0f : 0.0f;
        // **What rests near a surface** (`nearbyOcclusion`), in a headset's
        // eye: the small things nearest the camera, each a sphere as big as
        // its smallest half -- a cube's half side, a ball's radius -- and no
        // bigger than eight tenths of its bounds. Large things are what
        // things rest on, and things far off shade nothing anyone sees.
        frame.occluderParams[0] = 0.0f;
        frame.occluderParams[1] = 0.0f;
        if (world.headsetEye && (settings_.measuredSkip & MeasureSkip::Nearby) == 0u) {
            occluderPicks_.clear();
            for (const DrawItem& draw : world.draws) {
                if (draw.transparent || !draw.castShadow || draw.terrain || draw.voxelBlock ||
                    draw.boundsRadius < 0.02f || draw.boundsRadius > 0.6f)
                    continue;
                const f32 distanceSq = core::dot(draw.boundsCenter, draw.boundsCenter);
                if (distanceSq > 16.0f)
                    continue;
                f32 smallest = draw.boundsRadius;
                for (int column = 0; column < 3; ++column) {
                    const Vec3 axis{draw.transform.m[column][0], draw.transform.m[column][1],
                                    draw.transform.m[column][2]};
                    smallest = std::min(smallest, 0.5f * core::length(axis));
                }
                occluderPicks_.push_back(
                    OccluderPick{distanceSq, draw.boundsCenter, std::min(smallest, 0.8f * draw.boundsRadius)});
            }
            std::stable_sort(occluderPicks_.begin(), occluderPicks_.end(),
                             [](const OccluderPick& a, const OccluderPick& b) { return a.distanceSq < b.distanceSq; });
            const usize count = std::min<usize>(occluderPicks_.size(), kFrameOccluders);
            for (usize index = 0; index < count; ++index) {
                const OccluderPick& pick = occluderPicks_[index];
                frame.occluders[index][0] = pick.centre.x;
                frame.occluders[index][1] = pick.centre.y;
                frame.occluders[index][2] = pick.centre.z;
                frame.occluders[index][3] = pick.radius;
            }
            frame.occluderParams[0] = static_cast<f32>(count);
            frame.occluderParams[1] = 1.0f;
        }
        frame.forwardFold[0] = foldForShaders();
        frame.ambient[0] = world.environment.ambient.r;
        frame.ambient[1] = world.environment.ambient.g;
        frame.ambient[2] = world.environment.ambient.b;
        frame.outdoorAmbient[0] = world.environment.outdoorAmbient.r;
        frame.outdoorAmbient[1] = world.environment.outdoorAmbient.g;
        frame.outdoorAmbient[2] = world.environment.outdoorAmbient.b;
        // **With an `Atmosphere` the linear fog is not the world's any more**
        // (ADR 0096): `FogStart`, `FogEnd` and `FogColor` are kept and not
        // used. The opaque world and the sky are covered by the air pass below;
        // what still reads these three -- the blended surfaces and the
        // particles, which the air pass cannot see behind -- gets a linear
        // stand-in for the same air at the camera's height: its colour, from
        // the camera out to where nineteen parts in twenty are hidden.
        Color3 fogColor = world.environment.fogColor;
        f32 fogStart = world.environment.fogStart;
        f32 fogEnd = world.environment.fogEnd;
        if (air) {
            const AirMedium medium = airMediumOf(world.look.atmosphere, world.camera.origin.y);
            const f32 hidden = airOpticalDepth(medium, 0.0f, 1.0f);
            fogColor = sky.horizonColor;
            fogStart = 0.0f;
            fogEnd = hidden > 0.0f ? 3.0f / hidden : 0.0f;
        }
        frame.fogColor[0] = fogColor.r;
        frame.fogColor[1] = fogColor.g;
        frame.fogColor[2] = fogColor.b;
        frame.fogRange[0] = fogStart;
        frame.fogRange[1] = fogEnd;
        // Precomputed here so a fragment shader does not divide per pixel, and
        // zero when fog is off -- which makes the fog factor zero without the
        // shader needing to know that `end <= start` means anything.
        frame.fogRange[2] = fogEnd > fogStart ? 1.0f / (fogEnd - fogStart) : 0.0f;
        for (u32 index = 0; index < kShadowCascadeCount; ++index) {
            frame.cascadeViewProjection[index] = cascades.viewProjection[index];
            // A cascade past the setting's count was never rendered into, so its
            // boundary is pushed past anything a frame can hold: the selection
            // loop then stops at the last cascade that WAS rendered, and a
            // fragment beyond it selects the empty tile and comes back lit. The
            // blend band makes that a fade rather than a plane, which is what a
            // shadow distance ending should look like anyway.
            frame.cascadeFar[index] = index < cascadesDrawn ? cascades.farDistance[index] : kUnreachableDistance;
            frame.cascadeTexelWorld[index] = cascades.texelWorld[index];
            frame.cascadeDepthRange[index] = cascades.depthRange[index];
        }
        // `Lighting.ShadowSoftness` (ADR 0096).
        frame.shadowParams[0] = shadowPenumbra(world.environment.shadowSoftness);
        frame.shadowParams[1] = kShadowNormalOffsetTexels;
        frame.shadowParams[2] = kShadowCascadeBlend;
        frame.shadowParams[3] = depthBias;

        // The local atlas: one matrix per TILE, and how many of them are live.
        // Zero live tiles is what a scene with no casting spot or point gets,
        // and the shader skips the whole lookup on it.
        for (u32 entry = 0; entry < localShadows_.count; ++entry) {
            const LocalShadow& shadow = localShadows_.entries[entry];
            for (u32 face = 0; face < shadow.tileCount; ++face) {
                const u32 tile = shadow.firstTile + face;
                if (tile < kLocalShadowTileCount)
                    frame.localShadowViewProjection[tile] = shadow.viewProjection[face];
            }
        }
        frame.localShadowParams[0] = static_cast<f32>(localShadows_.tilesUsed);
        frame.localShadowParams[1] = 1.0f / static_cast<f32>(kLocalShadowAtlasResolution);
        frame.localShadowParams[2] = kLocalShadowDepthBias;
        frame.localShadowParams[3] = kLocalShadowFilterTexels;

        frame.environmentParams[0] = static_cast<f32>(kEnvironmentMipCount);
        frame.environmentParams[1] = 1.0f;
        frame.environmentParams[2] = 1.0f;
        // The shadow filter's taps: the level's (ADR 0172), or a
        // measurement's over it (`[debug] shadow_taps`, ADR 0171). Zero is
        // sixteen, and the bytes High always sent.
        frame.environmentParams[3] =
            static_cast<f32>(settings_.measuredShadowTaps != 0 ? settings_.measuredShadowTaps : settings_.shadowTaps);
        // **A standalone headset's tent** (minus one): four bilinear
        // comparisons a texel and a half across. The disc's sixteen taps
        // over a five-centimetre penumbra were a soft smear under a cube a
        // player looks at from half a metre, and the most of a Quest's
        // frame its shading took. A measurement's taps still say.
        if (world.headsetEye && world.headsetLean && settings_.measuredShadowTaps == 0)
            frame.environmentParams[3] = -1.0f;
        // `[debug] skip` (ADR 0171): zero unless a measurement asked.
        frame.clusterParams[3] =
            static_cast<f32>(settings_.measuredSkip | (settings_.shadowUnderCasters ? 0u : MeasureSkip::ShadowUnder));
        // `Lighting.EnvironmentDiffuseScale` (ADR 0096) on the nine
        // coefficients -- linear in them, so the sky's diffuse light scales
        // with no shader knowing. One is one, and the bytes are unchanged.
        const f32 diffuseScale = world.environment.environmentDiffuseScale;
        for (u32 index = 0; index < 9; ++index) {
            frame.irradianceSh[index][0] = environment_.irradiance[index].x * diffuseScale;
            frame.irradianceSh[index][1] = environment_.irradiance[index].y * diffuseScale;
            frame.irradianceSh[index][2] = environment_.irradiance[index].z * diffuseScale;
            frame.irradianceSh[index][3] = 0.0f;
        }

        // The lights themselves are in the tables uploaded above; what the block
        // carries is how to find them.
        frame.lightCountUnused[0] = static_cast<f32>(clusters_.lightCount);
        // **The camera's forward axis, under an orthographic projection**:
        // a fragment's distance in front of the camera is what the cascades and
        // the clusters are stated in, and there the clip position's w -- which
        // the vertex stages hand over for it -- is 1 for every fragment.
        if (core::isOrthographic(world.camera.projection)) {
            const Vec3 forward =
                core::normalize(core::transformDirection(core::inverse(world.camera.view), Vec3{0.0f, 0.0f, -1.0f}));
            frame.lightCountUnused[1] = forward.x;
            frame.lightCountUnused[2] = forward.y;
            frame.lightCountUnused[3] = forward.z;
        }
        frame.clusterParams[0] = clusters_.sliceScale;
        frame.clusterParams[1] = clusters_.sliceBias;
        frame.viewportParams[0] = static_cast<f32>(renderWidth_);
        frame.viewportParams[1] = static_cast<f32>(renderHeight_);
        frame.viewportParams[2] = 1.0f / static_cast<f32>(renderWidth_);
        frame.viewportParams[3] = 1.0f / static_cast<f32>(renderHeight_);
        // What a room reflects and the light it gives back (`prepareIndirect`).
        std::memcpy(frame.indirectParams, indirectParams_, sizeof(indirectParams_));
        // What the screen may give this frame (`traceScreen`): nothing where
        // its pass did not run.
        frame.screenLightParams[0] = screenReflectionReady_ ? 1.0f : 0.0f;
        frame.screenLightParams[1] = screenBounceReady_ ? 1.0f : 0.0f;
        frame.screenLightParams[2] = static_cast<f32>(screenReflectionLevels_ - 1u);
        // And the steps a blended surface's own march may take
        // (`sceneReflectionAt`): the setting's, whatever the pass did -- that
        // march reads this frame's scene and waits for no frame before it.
        // None under an orthographic camera, as the pass: the march finds
        // depth as a perspective one made it.
        frame.screenLightParams[3] =
            core::isOrthographic(world.camera.projection) ? 0.0f : static_cast<f32>(settings_.screenReflectionSteps);
        frame.screenViewProjection = world.camera.viewProjection;
        std::memcpy(frame.probeRows, probeRows_, sizeof(probeRows_));
        std::memcpy(frame.volumeRows, volumeRows_, sizeof(volumeRows_));
        std::memcpy(frame.sunGridRows, sunGridRows_, sizeof(sunGridRows_));

        // **What no decal paints** (`BasePart.ReceivesDecals`). A
        // decal is projected onto the depth the picture holds, which is of
        // everything, and the frozen RHI has no stencil to mark a part out
        // with. Those parts used to be drawn a second time, alone, into a
        // depth of their own for the decal to compare against -- a horde that
        // takes no decals, submitted twice a frame. Now they are drawn ONCE,
        // after the decals: a decal paints the pixel where one of them will
        // stand, and the part is then drawn over it. A cutout's holes show
        // the decal on what is behind, as they should.
        const bool decalsNow = !world.decals.empty() && ensureDecals(device);
        bool refusing = false;
        if (decalsNow) {
            for (const DrawItem& draw : world.draws) {
                if (!draw.receivesDecals && !draw.transparent && draw.inCameraFrustum) {
                    refusing = true;
                    break;
                }
            }
        }
        cmd.setPipeline(pbrPipeline_);
        cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
        forwardFrame_ = frame;
        decalTurn_ = refusing ? DecalTurn::Receiving : DecalTurn::Every;
        drawGeometry(cmd, world, meshes, world.camera.viewProjection, pbrPipeline_, pbrSkinnedPipeline_,
                     Selection::Opaque);
        decalTurn_ = DecalTurn::Every;
        // The foliage the cull kept (ADR 0116), with the opaque surfaces.
        drawFoliage(cmd, world, meshes, world.camera.viewProjection, FoliagePass::Forward);

        // **Decals, between the opaque surfaces and the transparent ones**.
        // They read the depth the opaque surfaces wrote, which a pass
        // that has it attached cannot, so the forward pass is closed around
        // them and reopened after -- only on a frame that has any, so every
        // other frame's command stream is what it always was.
        if (decalsNow) {
            cmd.endRenderPass();
            // The mask is the foliage's alone now: a layer of grass that
            // takes no decal is a compute-culled run with no draw to defer.
            bool masked = false;
            // And the foliage: the layers a decal does not paint go into the
            // same mask as those parts, and the ones it does into a depth of
            // their own, where the decal lands whichever way a blade faces
            // (ADR 0185). Each only on a frame that has a decal in the picture
            // and such a layer drawn, and only under the decals: a mask is
            // read nowhere else.
            const rhi::Rect decalsCover =
                decalCoverage(world.decals, world.camera.viewProjection, renderWidth_, renderHeight_);
            bool foliageRefuses = false;
            bool foliageReceives = false;
            // Neither where the setting says a decal does not paint foliage
            // (`GraphicsSettings::foliageDecals`): no blade is drawn again.
            if (settings_.foliageDecals && foliageCulled_ && foliageDecalPipeline_.valid() && decalsCover.width > 0 &&
                decalsCover.height > 0) {
                for (const RenderFoliageBucket& bucket : world.foliageBuckets) {
                    if (bucket.capacity == 0)
                        continue;
                    (bucket.receivesDecals ? foliageReceives : foliageRefuses) = true;
                }
            }
            masked = masked || foliageRefuses;
            if (masked && !decalMask_.valid()) {
                decalMask_ = device.createTexture({
                    .format = kDepthFormat,
                    .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
                    .width = renderWidth_,
                    .height = renderHeight_,
                    .debugName = "decal-mask",
                });
            }
            if (foliageReceives && !decalFoliage_.valid()) {
                decalFoliage_ = device.createTexture({
                    .format = kDepthFormat,
                    .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
                    .width = renderWidth_,
                    .height = renderHeight_,
                    .debugName = "decal-foliage",
                });
            }
            masked = masked && decalMask_.valid();
            foliageReceives = foliageReceives && decalFoliage_.valid();
            if (foliageReceives) {
                cmd.beginRenderPass({
                    .colorAttachments = {},
                    .depthStencil = {.texture = decalFoliage_,
                                     .loadOp = rhi::LoadOp::Clear,
                                     .storeOp = rhi::StoreOp::Store},
                    .debugName = "decal-foliage",
                });
                cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
                cmd.setScissor(decalsCover);
                drawFoliage(cmd, world, meshes, world.camera.viewProjection, FoliagePass::DecalReceiving);
                cmd.endRenderPass();
            }
            if (masked) {
                cmd.beginRenderPass({
                    .colorAttachments = {},
                    .depthStencil = {.texture = decalMask_,
                                     .loadOp = rhi::LoadOp::Clear,
                                     .storeOp = rhi::StoreOp::Store},
                    .debugName = "decal-mask",
                });
                cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
                cmd.setScissor(
                    {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
                if (foliageRefuses) {
                    cmd.setScissor(decalsCover);
                    drawFoliage(cmd, world, meshes, world.camera.viewProjection, FoliagePass::DecalRefusing);
                }
                cmd.endRenderPass();
            }
            const std::array<rhi::ColorAttachment, 1> decalTarget{rhi::ColorAttachment{
                .texture = hdr_,
                .loadOp = rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({.colorAttachments = decalTarget, .debugName = "decals"});
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            const Mat4 inverseViewProjection = core::inverse(world.camera.viewProjection);
            // What lights an Alpha decal: the ambient and the sun, as a
            // particle is lit -- a decal has no normal of its own to shade by.
            const f32 decalSun = world.environment.sunBrightness * sky.lightFactor;
            const f32 decalLight[3]{world.environment.outdoorAmbient.r + sky.lightColor.r * decalSun,
                                    world.environment.outdoorAmbient.g + sky.lightColor.g * decalSun,
                                    world.environment.outdoorAmbient.b + sky.lightColor.b * decalSun};
            core::i32 boundMode = -1;
            for (const RenderDecal& decal : world.decals) {
                // In pool order, each through its own blend (ADR 0160).
                const core::i32 mode = decal.blendMode == 1 && decalAlphaPipeline_.valid()
                                           ? 1
                                           : (decal.blendMode == 2 && decalAddPipeline_.valid() ? 2 : 0);
                if (mode != boundMode) {
                    cmd.setPipeline(mode == 1 ? decalAlphaPipeline_ : (mode == 2 ? decalAddPipeline_ : decalPipeline_));
                    boundMode = mode;
                }
                GpuDecalUniforms vertex;
                vertex.boxToWorld = decal.boxToWorld;
                vertex.viewProjection = world.camera.viewProjection;
                GpuDecalFragment fragment;
                fragment.worldToBox = decal.worldToBox;
                fragment.inverseViewProjection = inverseViewProjection;
                fragment.color[0] = decal.color.r;
                fragment.color[1] = decal.color.g;
                fragment.color[2] = decal.color.b;
                fragment.color[3] = decal.opacity;
                fragment.params[0] = 1.0f / static_cast<f32>(renderWidth_);
                fragment.params[1] = 1.0f / static_cast<f32>(renderHeight_);
                fragment.params[2] = decal.texture.valid() ? 1.0f : 0.0f;
                fragment.params[3] = static_cast<f32>(mode);
                fragment.light[0] = decalLight[0];
                fragment.light[1] = decalLight[1];
                fragment.light[2] = decalLight[2];
                fragment.light[3] = decal.emissive;
                fragment.axis[0] = decal.axis.x;
                fragment.axis[1] = decal.axis.y;
                fragment.axis[2] = decal.axis.z;
                // Which of the two masks there is to read: 1 for the parts
                // and layers no decal paints, 2 for the foliage one does.
                fragment.axis[3] = (masked ? 1.0f : 0.0f) + (foliageReceives ? 2.0f : 0.0f);
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&vertex, sizeof(vertex)));
                cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&fragment, sizeof(fragment)));
                // A mask where there is one; the depth again where there is
                // none, bound and not read.
                const std::array<rhi::TextureBinding, 4> textures{
                    rhi::TextureBinding{decal.texture.valid() ? decal.texture : whitePixel_, linearSampler_},
                    rhi::TextureBinding{depth_, pointSampler_},
                    rhi::TextureBinding{masked ? decalMask_ : depth_, pointSampler_},
                    rhi::TextureBinding{foliageReceives ? decalFoliage_ : depth_, pointSampler_},
                };
                cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
                cmd.draw(36, 1, 0, 0);
                countDraw(DrawKind::Decal);
            }
            cmd.endRenderPass();

            const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
                .texture = hdr_,
                .loadOp = rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({
                .colorAttachments = resumeTarget,
                .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                .debugName = "forward-blended",
            });
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            // **The parts no decal paints, now**: solid, tested
            // against the depth as the rest were, over whatever a decal put
            // where they stand.
            if (refusing) {
                cmd.setPipeline(pbrPipeline_);
                cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
                decalTurn_ = DecalTurn::Refusing;
                drawGeometry(cmd, world, meshes, world.camera.viewProjection, pbrPipeline_, pbrSkinnedPipeline_,
                             Selection::Opaque);
                decalTurn_ = DecalTurn::Every;
            }
            // The frame block again: the decal pass bound its own at the same
            // slot, and the blended surfaces below read this one.
            cmd.setPipeline(pbrBlendPipeline_);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
        }

        // **The 2D layer's sprites**, after everything opaque and before the
        // blended 3D surfaces: a picture on the plane is hidden by a solid part
        // in front of it, and a pane of glass in front of it is drawn over it.
        // In runs of one image and one filter, which a tilemap makes long.
        if (spriteCount_ > 0) {
            // **With an exact sprite in the frame, a pass of their own**: the
            // forward pass is closed, the sprites are drawn into the scene and
            // the mask at once, and the forward pass is reopened for what
            // blends over them -- as it is around the air and the decals.
            if (spriteExactLive_) {
                cmd.endRenderPass();
                const std::array<rhi::ColorAttachment, 2> spriteTargets{
                    rhi::ColorAttachment{.texture = hdr_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                    rhi::ColorAttachment{.texture = spriteMask_,
                                         .loadOp = rhi::LoadOp::Clear,
                                         .storeOp = rhi::StoreOp::Store,
                                         .clearColor = rhi::ColorRgba{0.0f, 0.0f, 0.0f, 0.0f}},
                };
                cmd.beginRenderPass({
                    .colorAttachments = spriteTargets,
                    .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                    .debugName = "sprites-exact",
                });
                cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
                cmd.setScissor(
                    {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            }
            cmd.setPipeline(spriteExactLive_ ? spriteExactPipeline_ : spritePipeline_);
            if (spriteExactLive_) {
                GpuSpriteExactView exactView;
                exactView.viewProjection = world.camera.viewProjection;
                exactView.unjitteredViewProjection = world.camera.unjitteredViewProjection;
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&exactView, sizeof(exactView)));
            }
            else {
                GpuWorldUiView spriteView;
                spriteView.viewProjection = world.camera.viewProjection;
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&spriteView, sizeof(spriteView)));
            }
            const std::array<rhi::BufferHandle, 1> spriteBuffers{spriteBuffer_};
            cmd.bindVertexBuffers(0, spriteBuffers);
            u32 first = 0;
            while (first < spriteCount_) {
                const RenderSprite& lead = world.sprites[first];
                u32 end = first + 1;
                while (end < spriteCount_ && world.sprites[end].texture == lead.texture &&
                       world.sprites[end].nearest == lead.nearest) {
                    ++end;
                }
                const std::array<rhi::TextureBinding, 1> texture{
                    rhi::TextureBinding{lead.texture.valid() ? lead.texture : whitePixel_,
                                        lead.nearest ? pointSampler_ : environmentSampler_}};
                cmd.bindTextures(rhi::ShaderStage::Fragment, 0, texture);
                cmd.draw(6, end - first, 0, first);
                countDraw(DrawKind::Sprite);
                first = end;
            }
            if (spriteExactLive_) {
                cmd.endRenderPass();
                const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
                    .texture = hdr_,
                    .loadOp = rhi::LoadOp::Load,
                    .storeOp = rhi::StoreOp::Store,
                }};
                cmd.beginRenderPass({
                    .colorAttachments = resumeTarget,
                    .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                    .debugName = "forward-after-sprites",
                });
                cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
                cmd.setScissor(
                    {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
                cmd.setPipeline(pbrBlendPipeline_);
            }
            // The frame block again, for the blended surfaces below.
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
        }

        // **The air** (`Atmosphere`, ADR 0096), over everything opaque and
        // over the sky, before anything blended: it reads the depth the forward
        // pass has attached, so the pass is closed around it and reopened after,
        // as it is for the decals -- only on a frame that has air.
        if (airDrawn) {
            cmd.endRenderPass();
            const AirMedium medium = airMediumOf(world.look.atmosphere, world.camera.origin.y);
            GpuLookAirUniforms airBlock;
            airBlock.inverseViewProjection = core::inverse(world.camera.skyViewProjection);
            airBlock.density[0] = medium.extinction;
            airBlock.density[1] = medium.falloff;
            airBlock.density[2] = medium.height;
            airBlock.density[3] = medium.haze;
            airBlock.light[0] = sky.horizonColor.r;
            airBlock.light[1] = sky.horizonColor.g;
            airBlock.light[2] = sky.horizonColor.b;
            airBlock.light[3] = kAirSkyReach;
            const f32 glare = world.look.atmosphere.glare * kAirGlareStrength * sky.dayFactor;
            airBlock.glare[0] = sky.sunColor.r * glare;
            airBlock.glare[1] = sky.sunColor.g * glare;
            airBlock.glare[2] = sky.sunColor.b * glare;
            airBlock.glare[3] = kAirSkyShare;
            airBlock.sun[0] = sky.sunDirection.x;
            airBlock.sun[1] = sky.sunDirection.y;
            airBlock.sun[2] = sky.sunDirection.z;
            airBlock.sun[3] = kAirGlareExponent;
            const std::array<rhi::TextureBinding, 1> sceneDepth{rhi::TextureBinding{depth_, pointSampler_}};
            fullscreenPass(cmd, air_.handle, hdr_, renderWidth_, renderHeight_, "atmosphere", sceneDepth,
                           asBytes(&airBlock, sizeof(airBlock)), rhi::LoadOp::Load);

            const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
                .texture = hdr_,
                .loadOp = rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({
                .colorAttachments = resumeTarget,
                .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                .debugName = "forward-after-air",
            });
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            cmd.setPipeline(pbrBlendPipeline_);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
        }

        // **With a depth of field, the lens first**. What blends writes
        // no depth -- a pane of glass, a flame, a spark -- so the focus pass
        // read the depth of what was BEHIND it and blurred it as that: a
        // window at the focus distance was smeared with the far hills seen
        // through it, and a fire was sharp over the floor and smeared over the
        // sky. So the opaque world is focused here, and what blends is drawn
        // over the focused picture, sharp, as those engines do by default.
        // The focused image becomes the frame (`hdr_` and `lookColor_` are two
        // of a kind and change places), and the forward pass resumes on it.
        if (world.look.depthOfField && settings_.depthOfField && !orthographic) {
            cmd.endRenderPass();
            if (focusImage(device, cmd, world, hdr_) != hdr_) {
                std::swap(hdr_, lookColor_);
                focusedBeforeBlended = true;
            }
            const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
                .texture = hdr_,
                .loadOp = rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({
                .colorAttachments = resumeTarget,
                .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                .debugName = "forward-after-focus",
            });
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            cmd.setPipeline(pbrBlendPipeline_);
            cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&frame, sizeof(frame)));
        }

        // Blended, after the opaque pass has filled depth, back to front. The
        // frame uniforms are still bound -- same block, same slot, same values
        // -- so only the pipeline changes.
        //
        // What this does NOT buy, and the deliverable should not imply
        // otherwise: sorting is per draw, so two transparent surfaces that
        // intersect each other sort wrongly at the pixels where they cross.
        // Order-independent transparency is not on the v1 list.
        //
        // **And the upscaler is told what does** (ADR 0164): the scene as it
        // is now, kept, for the mask made when the pass is over.
        if (fsr2Now_ && activeView_ == 0)
            copyOpaqueForUpscaler(device, cmd, world, frame);
        copySceneForSurfaces(device, cmd, world, frame);
        cmd.setPipeline(pbrBlendPipeline_);
        drawGeometry(cmd, world, meshes, world.camera.viewProjection, pbrBlendPipeline_, pbrSkinnedBlendPipeline_,
                     Selection::Transparent);

        // Particles last: over every surface, transparent ones included,
        // which is the one ordering error this accepts -- a particle behind a
        // pane of glass draws over it. Sorting the two together would put a
        // per-particle draw into a per-draw sort, and the whole point of a
        // particle is that it is not a draw of its own.
        const bool gpuParticles = !world.gpuEmitters.empty() && particleGpuPipeline_.valid();
        if (particleCount_ > 0 || ribbonVertexCount_ > 0 || gpuParticles) {
            GpuParticleUniforms particleUniforms;
            particleUniforms.viewProjection = world.camera.viewProjection;
            const Mat4 cameraToWorld = core::inverse(world.camera.view);
            const Vec3 right = core::transformDirection(cameraToWorld, Vec3{1.0f, 0.0f, 0.0f});
            const Vec3 up = core::transformDirection(cameraToWorld, Vec3{0.0f, 1.0f, 0.0f});
            particleUniforms.cameraRight[0] = right.x;
            particleUniforms.cameraRight[1] = right.y;
            particleUniforms.cameraRight[2] = right.z;
            particleUniforms.cameraUp[0] = up.x;
            particleUniforms.cameraUp[1] = up.y;
            particleUniforms.cameraUp[2] = up.z;

            // A particle carries no sky term, so it is lit as the open air is
            // (ADR 0084): `OutdoorAmbient`, not the enclosed `Ambient`.
            GpuParticleLighting lighting;
            lighting.ambient[0] = world.environment.outdoorAmbient.r;
            lighting.ambient[1] = world.environment.outdoorAmbient.g;
            lighting.ambient[2] = world.environment.outdoorAmbient.b;
            const f32 sun = world.environment.sunBrightness * sky.lightFactor;
            lighting.sunLight[0] = sky.lightColor.r * sun;
            lighting.sunLight[1] = sky.lightColor.g * sun;
            lighting.sunLight[2] = sky.lightColor.b * sun;
            lighting.fogColor[0] = frame.fogColor[0];
            lighting.fogColor[1] = frame.fogColor[1];
            lighting.fogColor[2] = frame.fogColor[2];
            lighting.fogRange[0] = frame.fogRange[0];
            lighting.fogRange[1] = frame.fogRange[1];
            lighting.fogRange[2] = frame.fogRange[2];
            // Negative for an orthographic camera: `particle.hlsl` reads the
            // sign to know its depth is linear.
            lighting.depth[0] =
                core::isOrthographic(world.camera.projection) ? -world.camera.nearPlane : world.camera.nearPlane;
            lighting.depth[1] = world.camera.farPlane;
            lighting.depth[2] = 1.0f / static_cast<f32>(renderWidth_);
            lighting.depth[3] = 1.0f / static_cast<f32>(renderHeight_);

            // **Soft particles read the depth the forward pass has attached**,
            // so it is closed around them and reopened after, as it is for the
            // decals -- only on a frame that has particles.
            cmd.endRenderPass();
            const std::array<rhi::ColorAttachment, 1> particleTarget{rhi::ColorAttachment{
                .texture = hdr_,
                .loadOp = rhi::LoadOp::Load,
                .storeOp = rhi::StoreOp::Store,
            }};
            cmd.beginRenderPass({.colorAttachments = particleTarget, .debugName = "particles"});
            cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
            cmd.setScissor(
                {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
            // **Beams and trails first** (ADR 0129), then the particles over
            // them: a ribbon is the larger and the further thing more often
            // than not, and each kind is in order within itself. A run is one
            // texture.
            if (ribbonVertexCount_ > 0) {
                cmd.setPipeline(ribbonPipeline_);
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&particleUniforms, sizeof(particleUniforms)));
                cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&lighting, sizeof(lighting)));
                const std::array<rhi::BufferHandle, 1> ribbonBuffers{ribbonBuffer_};
                cmd.bindVertexBuffers(0, ribbonBuffers);
                for (const RenderRibbonRun& run : world.ribbonRuns) {
                    if (run.firstVertex >= ribbonVertexCount_)
                        break;
                    const u32 count = std::min(run.vertexCount, ribbonVertexCount_ - run.firstVertex);
                    const std::array<rhi::TextureBinding, 2> ribbonTextures{
                        rhi::TextureBinding{depth_, pointSampler_},
                        rhi::TextureBinding{run.texture.valid() ? run.texture : whitePixel_, linearSampler_},
                    };
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, ribbonTextures);
                    cmd.draw(count, 1, run.firstVertex, 0);
                    countDraw(DrawKind::Ribbon);
                }
            }
            // **The sun's cascades for the shadow at a particle**
            // (`LightInfluence`), the vertex stage's own copy, and its map.
            GpuParticleShadow particleShadow;
            for (usize index = 0; index < 4; ++index) {
                particleShadow.viewProjection[index] = forwardFrame_.cascadeViewProjection[index];
                particleShadow.far[index] = forwardFrame_.cascadeFar[index];
            }
            for (usize axis = 0; axis < 3; ++axis)
                particleShadow.sun[axis] = forwardFrame_.sunDirectionBrightness[axis];
            particleShadow.sun[3] = forwardFrame_.sunDirectionBrightness[3];
            particleShadow.layout[0] = forwardFrame_.sunColorUnused[3];
            for (usize index = 0; index < 9; ++index) {
                for (usize channel = 0; channel < 4; ++channel)
                    particleShadow.sky[index][channel] = forwardFrame_.irradianceSh[index][channel];
            }
            particleShadow.skyParams[0] = forwardFrame_.environmentParams[1];
            const std::array<rhi::TextureBinding, 1> particleShadowMap{rhi::TextureBinding{shadowMap_, shadowSampler_}};
            if (particleCount_ > 0) {
                cmd.setPipeline(particlePipeline_);
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&particleUniforms, sizeof(particleUniforms)));
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&particleShadow, sizeof(particleShadow)));
                cmd.bindTextures(rhi::ShaderStage::Vertex, 0, particleShadowMap);
                cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&lighting, sizeof(lighting)));
                const std::array<rhi::BufferHandle, 1> particleBuffers{particleBuffer_};
                cmd.bindVertexBuffers(0, particleBuffers);
                for (const ParticleBatch& run : particleRuns_) {
                    if (run.first >= particleCount_)
                        break;
                    std::array<rhi::TextureBinding, MaxParticleTextures + 1> particleTextures;
                    particleTextures[0] = {depth_, pointSampler_};
                    for (u32 index = 0; index < MaxParticleTextures; ++index)
                        particleTextures[index + 1] = {index < run.textureCount ? run.textures[index] : whitePixel_,
                                                       linearSampler_};
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, particleTextures);
                    cmd.draw(6, std::min(run.count, particleCount_ - run.first), 0, run.first);
                    countDraw(DrawKind::Particle);
                }
            }
            // **And the ones simulated on the GPU** (ADR 0160): an emitter a
            // draw, every slot of its buffer an instance -- an empty slot is a
            // square of no size. Among themselves in no order: sorting a
            // hundred thousand a frame is what the GPU was chosen to avoid,
            // and light added to light does not show it.
            if (gpuParticles) {
                cmd.setPipeline(particleGpuPipeline_);
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, asBytes(&particleUniforms, sizeof(particleUniforms)));
                cmd.bindUniforms(rhi::ShaderStage::Vertex, 2, asBytes(&particleShadow, sizeof(particleShadow)));
                cmd.bindTextures(rhi::ShaderStage::Vertex, 0, particleShadowMap);
                cmd.bindUniforms(rhi::ShaderStage::Fragment, 0, asBytes(&lighting, sizeof(lighting)));
                for (const RenderGpuEmitter& emitter : world.gpuEmitters) {
                    const auto found =
                        gpuEmitters_.find((static_cast<u64>(emitter.id.index) << 32) | emitter.id.generation);
                    if (found == gpuEmitters_.end() || !found->second.buffer.valid())
                        continue;
                    const GpuEmitterBuffer& held = found->second;
                    GpuParticleLook look;
                    look.originEmission[0] = static_cast<f32>(held.origin.x - world.camera.origin.x);
                    look.originEmission[1] = static_cast<f32>(held.origin.y - world.camera.origin.y);
                    look.originEmission[2] = static_cast<f32>(held.origin.z - world.camera.origin.z);
                    look.originEmission[3] = emitter.emission;
                    look.flipbook[0] = static_cast<f32>(emitter.columns);
                    look.flipbook[1] = static_cast<f32>(emitter.rows);
                    look.flipbook[2] = static_cast<f32>(emitter.flipbookMode);
                    look.flipbook[3] = emitter.framerate;
                    look.kind[0] = static_cast<f32>(emitter.shape);
                    look.kind[1] = emitter.texture.valid() ? 1.0f : 0.0f;
                    look.kind[2] = emitter.lightInfluence;
                    for (usize at = 0; at < 16; ++at) {
                        for (usize channel = 0; channel < 4; ++channel)
                            look.colorOverLife[at][channel] = emitter.colorOverLife[at][channel];
                        look.sizeOverLife[at / 4][at % 4] = emitter.sizeOverLife[at];
                    }
                    cmd.bindUniforms(rhi::ShaderStage::Vertex, 1, asBytes(&look, sizeof(look)));
                    const std::array<rhi::BufferHandle, 1> slots{held.buffer};
                    cmd.bindStorageBuffers(rhi::ShaderStage::Vertex, 0, slots);
                    const std::array<rhi::TextureBinding, 2> particleTextures{
                        rhi::TextureBinding{depth_, pointSampler_},
                        rhi::TextureBinding{emitter.texture.valid() ? emitter.texture : whitePixel_, linearSampler_},
                    };
                    cmd.bindTextures(rhi::ShaderStage::Fragment, 0, particleTextures);
                    cmd.draw(6, held.capacity, 0, 0);
                    countDraw(DrawKind::Particle);
                }
            }
            cmd.endRenderPass();
            forwardOpen = false;

            // **Reopened for the world's UI, and only for it** (ADR 0172):
            // nothing else is drawn after the particles, and a pass opened to
            // draw nothing still loads the picture and the depth and stores
            // them again -- every frame with a particle in it.
            if (uiInForward) {
                const std::array<rhi::ColorAttachment, 1> resumeTarget{rhi::ColorAttachment{
                    .texture = hdr_,
                    .loadOp = rhi::LoadOp::Load,
                    .storeOp = rhi::StoreOp::Store,
                }};
                cmd.beginRenderPass({
                    .colorAttachments = resumeTarget,
                    .depthStencil = {.texture = depth_, .loadOp = rhi::LoadOp::Load, .storeOp = rhi::StoreOp::Store},
                    .debugName = "forward-after-particles",
                });
                cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
                cmd.setScissor(
                    {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
                forwardOpen = true;
            }
        }

        // **World UI after the particles**: the trees arrive back to
        // front, and the ones that are always on top come after all of them.
        if (uiInForward)
            drawWorldUi(cmd, world, uiApart ? WorldUiRuns::AsLight : WorldUiRuns::All);
    }

    if (forwardOpen)
        cmd.endRenderPass();
    // **And the interface as the display shows it, apart** (ADR 0250): into
    // its own layer, tested against the scene's depth -- the samples', where
    // the eye has them -- for the tonemap to lay over the picture.
    uiLayerLive_ = false;
    if (uiApart && ensureUiLayer(device, samples)) {
        const std::array<rhi::ColorAttachment, 1> layerAttachment{rhi::ColorAttachment{
            .texture = samples > 1 ? uiLayerSamples_ : uiLayer_,
            .loadOp = rhi::LoadOp::Clear,
            .storeOp = samples > 1 ? rhi::StoreOp::Resolve : rhi::StoreOp::Store,
            .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
            .resolveTexture = samples > 1 ? uiLayer_ : rhi::TextureHandle{},
        }};
        cmd.beginRenderPass({
            .colorAttachments = layerAttachment,
            .depthStencil = {.texture = samples > 1 ? msaaDepth_ : depth_,
                             .loadOp = rhi::LoadOp::Load,
                             .storeOp = samples > 1 ? rhi::StoreOp::DontCare : rhi::StoreOp::Store},
            .debugName = "world-ui",
        });
        cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
        cmd.setScissor(
            {.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
        drawWorldUi(cmd, world, WorldUiRuns::AsDisplayed);
        cmd.endRenderPass();
        uiLayerLive_ = true;
    }
    // Before the look's passes: a shaft of light or a blur over the whole
    // picture is not something that blended.
    if (fsr2Now_ && activeView_ == 0)
        writeReactiveMask(device, cmd);
    cmd.popDebugGroup();

    // This frame, lit and blended and nothing else yet, kept for the next
    // one's rays (ADR 0209) -- where anything reads the screen.
    keepSceneBefore(device, cmd, world);

    // What the headset's runtime makes the frames between from (ADR 0252).
    if (target.motion.valid())
        drawWarpMotion(device, cmd, world, meshes, target);

    // --- The look's scene passes (ADR 0096) ------------------------------------
    ENG_PROFILE_NEXT(passes, "render.look");
    //
    // What every pass below reads as "the frame". `hdr_` on every frame without
    // one of these effects, which is what keeps that frame's command stream the
    // one it always was.
    const RenderLook& look = world.look;
    // Folded, the frame is the headset's image, already tonemapped.
    rhi::TextureHandle sceneColor = foldExposure_ > 0.0f ? target.color : hdr_;
    // **What the meter reads of a folded eye**: the image blitted down into
    // one of the renderer's own. A runtime's image is never read where it is
    // -- it rests as a colour target, the layout the runtime takes it back
    // in, and a view to sample it through is one no backend makes.
    rhi::TextureHandle meteredColor = sceneColor;
    u32 meteredWidth = renderWidth_;
    u32 meteredHeight = renderHeight_;
    if (foldExposure_ > 0.0f && target.colorReadable) {
        // In the image's own kind: an encoded picture copied as it is, and
        // decoded by the meter.
        const rhi::TextureFormat meterFormat =
            foldEncoded_ ? rhi::TextureFormat::Rgba8Unorm : rhi::TextureFormat::Rgba8UnormSrgb;
        if (foldMeter_.valid() && foldMeterFormat_ != meterFormat) {
            device.destroy(foldMeter_);
            foldMeter_ = {};
        }
        foldMeterFormat_ = meterFormat;
        if (!foldMeter_.valid())
            foldMeter_ = device.createTexture({
                .format = meterFormat,
                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
                .width = kFoldMeterSize,
                .height = kFoldMeterSize,
                .debugName = "eye-metered",
            });
        if (foldMeter_.valid()) {
            cmd.blitTexture(target.color, foldMeter_, 0);
            meteredColor = foldMeter_;
            meteredWidth = kFoldMeterSize;
            meteredHeight = kFoldMeterSize;
        }
    }

    // **A picture for a reflection probe stops here** (ADR 0202): the frame as
    // it is lit, before anything that is a camera's own -- the look, the
    // exposure, bloom, the tone curve, anti-aliasing.
    if (world.capture) {
        if (ensureLookPipeline(device, resample_, "look_resample", kHdrFormat)) {
            const std::array<rhi::TextureBinding, 1> lit{rhi::TextureBinding{hdr_, environmentSampler_}};
            fullscreenPass(cmd, resample_.handle, target.color, target.width, target.height, "probe-picture", lit, {});
        }
        return;
    }

    // **Depth of field**, first: it reads the depth the opaque world wrote,
    // and a blur or a shaft of light added before it would be focused as though
    // it stood where the surface behind it does. Not under an orthographic
    // camera, whose depth is not a distance a lens focuses by -- the 2D layer's
    // view -- and not on a machine that has turned it off (ADR 0044).
    //
    // **Already done, on a frame with a camera**: inside the forward
    // pass, before what blends, so glass and flames are drawn over the focused
    // picture and not blurred as what is behind them.
    if (look.depthOfField && settings_.depthOfField && world.camera.valid && !orthographic && !focusedBeforeBlended)
        sceneColor = focusImage(device, cmd, world, sceneColor);

    // **Sun rays**, after the focus -- a shaft is light in the air between the
    // camera and everything, which no lens focuses away -- and before the blur,
    // which softens them with the rest.
    if (look.sunRays && settings_.sunRays && world.camera.valid && !orthographic)
        sunRaysOnto(device, cmd, world, sky, sceneColor);

    // **The blur**, last of them: it softens whatever the others made. On the
    // HDR image and before exposure, so a highlight blurs as light does -- a
    // bright lamp spreads into a glow rather than into a grey smear -- and the
    // interface, which the host draws after all of this, stays sharp.
    if (look.blurSize > 0.0f && world.camera.valid)
        blurImage(device, cmd, sceneColor, look.blurSize);

    // **The temporal pass** (ADR 0158), last of the passes on the scene and
    // before exposure and bloom: they read the picture it settled.
    //
    // **Or the upscaler in its place** (ADR 0164), after which "the frame" is
    // the size of the target: `sceneWidth` by `sceneHeight` is what the
    // passes below measure a texel of it by.
    u32 sceneWidth = renderWidth_;
    u32 sceneHeight = renderHeight_;
    bool upscaledNow = false;
    if (temporalNow_) {
        ENG_PROFILE_NEXT(passes, "render.taa");
        rhi::TextureHandle upscaled{};
        if (fsr2Now_ && activeView_ == 0)
            upscaled = upscaleTemporal(device, cmd, world, target, sceneColor);
        if (upscaled.valid()) {
            sceneColor = upscaled;
            sceneWidth = target.width;
            sceneHeight = target.height;
            upscaledNow = true;
        }
        else {
            sceneColor = resolveTemporal(device, cmd, world, sceneColor);
        }
    }
    else if (motionNow) {
        // No temporal pass to remember what this frame was: frame generation
        // measures the next one's motion against it all the same.
        rememberCamera(world.camera);
    }

    // --- Automatic exposure -------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.exposure");
    //
    // Three passes down to one texel, and the last of them carries state: it
    // reads the exposure the LAST frame wrote and writes this frame's into the
    // other of two 1x1 targets. That ping-pong is how a value survives a frame
    // in a renderer with no compute and no readback a frame could afford.
    const u32 previousExposure = exposureIndex_;
    const u32 nextExposure = 1u - exposureIndex_;
    exposureIndex_ = nextExposure;

    cmd.pushDebugGroup("exposure");
    // **A headset frame is metered once**, by its first eye, and its second
    // eye is drawn at that exposure: two eyes metered apart are two
    // exposures, one eye brighter than the other, and three passes more --
    // the first reading the whole eye.
    const bool exposureShared = world.shadowView.has_value() && world.shadowView->reuse && headsetExposure_.valid();
    rhi::TextureHandle metered = exposureShared ? headsetExposure_ : exposure_[nextExposure];
    if (exposureShared) {
        // Nothing to meter: the first eye has.
    }
    // The machine's switch and the world's, and either turns it off: a scene
    // that wants a fixed exposure gets one, and a machine that cannot afford
    // the metering does not pay for it (ADR 0044, ADR 0096).
    else if (!settings_.autoExposure || !world.environment.autoExposure) {
        // A neutral gain, written directly. Skipping the three passes is the
        // point of the setting -- what is left is `ExposureCompensation` and
        // `Lighting.Brightness`, which is exactly the fixed exposure the engine
        // had before the lighting work metered anything.
        clearPass(cmd, exposure_[nextExposure], 1, 1, "exposure-fixed", rhi::ColorRgba{1.0f, 1.0f, 1.0f, 1.0f});
        // So that switching metering back on adapts instantly from the frame it
        // measures rather than from the neutral value it finds.
        exposureInitialised_ = false;
    }
    else {
        GpuLuminanceUniforms luminance;
        // **The world's own limits** (`Lighting.ExposureMin`/`ExposureMax`):
        // the meter is held to the averages those gains answer to.
        {
            const ExposureRange range = exposureRange(world.environment.exposureMin, world.environment.exposureMax);
            luminance.range[0] = range.lowest;
            luminance.range[1] = range.highest;
        }
        const bool meteredApart = meteredColor != sceneColor;
        luminance.texelRate[0] = 1.0f / static_cast<f32>(meteredApart ? meteredWidth : sceneWidth);
        luminance.texelRate[1] = 1.0f / static_cast<f32>(meteredApart ? meteredHeight : sceneHeight);
        // Folded: the meter undoes the curve and this frame's exposure.
        luminance.texelRate[3] = foldExposure_ > 0.0f ? (foldEncoded_ ? -1.0f : 1.0f) / foldExposure_ : 0.0f;
        // **A view seen on one side of a line is metered on that side** (ADR
        // 0205): the other is black, and half a picture of black read as a
        // dark room and opened the exposure. The line goes through the
        // middle, so the meter reads the kept side's mirror image there.
        luminance.range[2] = target.sideNormal.x * static_cast<f32>(target.width);
        luminance.range[3] = target.sideNormal.y * static_cast<f32>(target.height);
        // The world's interface metered as part of the picture (ADR 0250),
        // over it at the exposure the frame before was drawn at; and a
        // picture with nothing behind it metered where something was drawn.
        luminance.first[0] = std::exp2(world.environment.exposureCompensation);
        luminance.first[1] = world.environment.transparentBackground ? 1.0f : 0.0f;
        const std::array<rhi::TextureBinding, 3> hdrBinding{
            rhi::TextureBinding{meteredColor, linearSampler_},
            rhi::TextureBinding{uiLayerLive_ ? uiLayer_ : nothingPixel_, linearSampler_},
            rhi::TextureBinding{exposure_[previousExposure], pointSampler_}};
        fullscreenPass(cmd, luminanceDownPipeline_, luminance64_, 64, 64, "luminance-down", hdrBinding,
                       asBytes(&luminance, sizeof(luminance)));
        luminance.range[2] = 0.0f;
        luminance.range[3] = 0.0f;

        luminance.texelRate[0] = 1.0f / 64.0f;
        luminance.texelRate[1] = 1.0f / 64.0f;
        luminance.texelRate[3] = 0.0f;
        const std::array<rhi::TextureBinding, 1> coarse{rhi::TextureBinding{luminance64_, linearSampler_}};
        fullscreenPass(cmd, luminanceReducePipeline_, luminance8_, 8, 8, "luminance-reduce", coarse,
                       asBytes(&luminance, sizeof(luminance)));

        luminance.texelRate[0] = 1.0f / 8.0f;
        luminance.texelRate[1] = 1.0f / 8.0f;
        // The first frame after a resize adapts instantly rather than from
        // whatever the freshly created target happens to hold: a run whose first
        // frames faded in from black is a run whose golden at frame two and
        // screenshot at frame thirty disagree.
        luminance.texelRate[2] = exposureInitialised_ ? kExposureAdaptationRate : 1.0f;
        const std::array<rhi::TextureBinding, 2> adapt{
            rhi::TextureBinding{luminance8_, linearSampler_},
            rhi::TextureBinding{exposure_[previousExposure], linearSampler_}};
        fullscreenPass(cmd, luminanceAdaptPipeline_, exposure_[nextExposure], 1, 1, "luminance-adapt", adapt,
                       asBytes(&luminance, sizeof(luminance)));
        exposureInitialised_ = true;
        // **And read back for a folded eye's next frames**, a few frames
        // late: never more than a handful asked for at once.
        if (((world.headsetEye && world.headsetLean) || (!world.headsetEye && settings_.leanPath)) &&
            foldReadbacks_.size() < 4) {
            if (const u32 ticket = device.requestReadback(exposure_[nextExposure], 1, 1); ticket != 0)
                foldReadbacks_.push_back(ticket);
        }
    }
    cmd.popDebugGroup();
    headsetExposure_ = world.shadowView.has_value() ? metered : rhi::TextureHandle{};

    // --- Bloom ---------------------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.bloom");
    //
    // Down with a thirteen-tap box, up with a tent, each level its own texture
    // because a `ColorAttachment` names a texture and not a mip level. The
    // threshold is applied once, on the way in.
    // **A `BloomEffect` governs bloom when one counts** (ADR 0096): its numbers
    // replace the engine's own, and one that is disabled turns bloom off. With
    // none, every number below is the constant it always was. The machine's
    // switch still wins over the world's (ADR 0044).
    const bool bloomOn = settings_.bloom && look.bloomEnabled;
    cmd.pushDebugGroup("bloom");
    if (!bloomOn) {
        // Black adds nothing, and the tonemap adds `bloom_[0]` unconditionally.
        // Cheaper than the eight passes it replaces and, unlike leaving the
        // chain's textures alone, does not depend on what was in them. Once:
        // black stays black until bloom draws again.
        if (!bloomOff_) {
            clearPass(cmd, bloom_[0], bloomLevelSize(renderWidth_, 0), bloomLevelSize(renderHeight_, 0), "bloom-off",
                      rhi::ColorRgba{0.0f, 0.0f, 0.0f, 1.0f});
            bloomOff_ = true;
        }
    }
    else {
        bloomOff_ = false;
        // As many levels as the settings give it (ADR 0172): five on a desk,
        // three on a handheld.
        const u32 bloomLevels = std::clamp(settings_.bloomLevels, 2u, kBloomLevels);
        u32 sourceWidth = sceneWidth;
        u32 sourceHeight = sceneHeight;
        for (u32 level = 0; level < bloomLevels; ++level) {
            GpuBloomUniforms bloom;
            bloom.texelRadius[0] = 1.0f / static_cast<f32>(sourceWidth);
            bloom.texelRadius[1] = 1.0f / static_cast<f32>(sourceHeight);
            bloom.texelRadius[2] = 1.0f;
            if (level == 0) {
                bloom.threshold[0] = look.bloomGoverned ? look.bloomThreshold : kBloomThreshold;
                bloom.threshold[1] = kBloomKnee;
            }
            // **Clamped at the edges, never wrapped**: the material
            // sampler repeats, and a kernel this wide at a coarse level reached
            // round the screen -- the ground's glow at the bottom edge drew a
            // band along the top.
            // **A sprite drawn in its own colours starts no glow** (ADR 0153):
            // the first level, in such a frame, reads the mask beside the scene
            // and leaves those pixels out. The levels below read this one.
            const bool masked = level == 0 && spriteExactLive_ &&
                                ensureLookPipeline(device, bloomDownMasked_, "bloom_down_masked", kHdrFormat);
            const std::array<rhi::TextureBinding, 2> source{
                rhi::TextureBinding{level == 0 ? sceneColor : bloom_[level - 1], environmentSampler_},
                rhi::TextureBinding{masked ? spriteMask_ : whitePixel_, environmentSampler_}};
            sourceWidth = bloomLevelSize(renderWidth_, level);
            sourceHeight = bloomLevelSize(renderHeight_, level);
            fullscreenPass(cmd, masked ? bloomDownMasked_.handle : bloomDownPipeline_, bloom_[level], sourceWidth,
                           sourceHeight, "bloom-down",
                           std::span<const rhi::TextureBinding>{source.data(), masked ? usize{2} : usize{1}},
                           asBytes(&bloom, sizeof(bloom)));
        }

        for (u32 level = bloomLevels - 1; level > 0; --level) {
            GpuBloomUniforms bloom;
            bloom.texelRadius[0] = 1.0f / static_cast<f32>(bloomLevelSize(renderWidth_, level));
            bloom.texelRadius[1] = 1.0f / static_cast<f32>(bloomLevelSize(renderHeight_, level));
            // The tent's radius is how far the glow reaches: every level's
            // kernel widens together, so the falloff keeps its shape.
            bloom.texelRadius[2] = look.bloomGoverned ? look.bloomSize / kBloomSize : 1.0f;
            const std::array<rhi::TextureBinding, 1> source{rhi::TextureBinding{bloom_[level], environmentSampler_}};
            // `LoadOp::Load`, because the pipeline blends ADDITIVELY into what
            // the downsample already put there -- reading and writing one target
            // in one pass is what every backend refuses.
            fullscreenPass(cmd, bloomUpPipeline_, bloom_[level - 1], bloomLevelSize(renderWidth_, level - 1),
                           bloomLevelSize(renderHeight_, level - 1), "bloom-up", source, asBytes(&bloom, sizeof(bloom)),
                           rhi::LoadOp::Load);
        }
    }
    cmd.popDebugGroup();

    // --- Tonemap ------------------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.tonemap");
    //
    // A separate pass rather than writing the swapchain directly from the
    // forward one: the HDR target has to be complete before it can be sampled,
    // and a backend is entitled to enforce that. It writes an LDR TEXTURE rather
    // than the swapchain now, because the anti-aliasing resolve needs a
    // tonemapped image to find edges in.
    GpuTonemapUniforms tonemap;
    tonemap.exposureBloom[0] = world.environment.exposureCompensation;
    tonemap.exposureBloom[1] = look.bloomGoverned ? kBloomIntensity * look.bloomIntensity : kBloomIntensity;
    tonemap.exposureBloom[2] = world.environment.transparentBackground ? 1.0f : 0.0f;
    tonemap.exposureBloom[3] = 0.0f;

    // **Every colour correction, a stage each, in the graded twin of the
    // tonemap** -- chosen only on a frame that has one, so a world without
    // draws through the plain pipeline with the plain block.
    GpuGradeUniforms grade;
    // A view with nothing behind it goes straight to its target: the
    // anti-aliasing resolve writes an opaque picture, and would lose the alpha.
    //
    // **What follows the tonemap** (ADR 0158): a spatial anti-aliasing pass --
    // FXAA, or SMAA, which a temporal frame needs no more of and a view that
    // asked for TAA takes in its place -- then, for a world drawn smaller than
    // its target, FSR 1's upscale, and RCAS's sharpening after it or after the
    // temporal pass. Any of them, and the tonemap writes `ldr_` for them.
    const bool opaquePicture = !world.environment.transparentBackground;
    //
    // **And none of them after FSR 2** (ADR 0164), which smoothed, upscaled
    // and sharpened already: the tonemap writes the target. Where FSR 2 was
    // asked for and this frame is not its -- a device without compute, a
    // camera without perspective -- FSR 1 upscales in its place.
    const AntiAliasingMode spatial = upscaledNow ? AntiAliasingMode::Off
                                     : settings_.antiAliasing == AntiAliasingMode::Taa
                                         ? (temporalNow_ ? AntiAliasingMode::Off : AntiAliasingMode::Smaa)
                                         : settings_.antiAliasing;
    const bool smaller = !upscaledNow && (renderWidth_ < target.width || renderHeight_ < target.height);
    const bool upscale = opaquePicture && settings_.upscaling != UpscalingMode::None && smaller;
    const bool sharpen =
        opaquePicture && !upscaledNow && !upscale && temporalNow_ && !smaller && settings_.sharpness > 0.0f;
    const bool resolve = opaquePicture && (spatial != AntiAliasingMode::Off || upscale || sharpen);
    // **Written in the format of what it writes**: the LDR texture
    // when a resolve follows, the target itself otherwise -- a window's
    // swapchain, or a view's `kLdrFormat` texture.
    const bool intoWindow = !resolve && target.colorFormat != kLdrFormat;
    // An sRGB target encodes on its own: a headset's runtime image.
    if (intoWindow && (target.colorFormat == rhi::TextureFormat::Rgba8UnormSrgb ||
                       target.colorFormat == rhi::TextureFormat::Bgra8UnormSrgb))
        tonemap.exposureBloom[3] = 1.0f;
    // A target in another format than the window's: a headset's image.
    const bool intoEye = intoWindow && target.colorFormat != colorFormat_;
    LookPipeline& gradedSlot = intoEye ? gradedTonemapEye_ : intoWindow ? gradedTonemapWindow_ : gradedTonemap_;
    const bool graded = look.graded && ensureLookPipeline(device, gradedSlot, "tonemap_graded",
                                                          intoWindow ? target.colorFormat : kLdrFormat);
    if (graded) {
        const u32 stages = std::min<u32>(look.gradeCount, static_cast<u32>(MaxGradeStages));
        for (u32 stage = 0; stage < stages; ++stage) {
            for (u32 row = 0; row < 3; ++row) {
                for (u32 column = 0; column < 3; ++column)
                    grade.rows[stage][row][column] = look.grades[stage].mix[row][column];
            }
            grade.rows[stage][0][3] = look.grades[stage].power;
            grade.rows[stage][1][3] = look.grades[stage].lift;
        }
        grade.count[0] = static_cast<f32>(stages);
    }
    // **A frame with sprites drawn in their own colours resolves through the
    // twin that reads their mask** (ADR 0153) -- plain or graded, into the
    // texture or the window, four in all and each made when first needed. The
    // mask, and the sprite's colour under it, through a point sampler: a world
    // drawn smaller than the window is scaled up here, and pixel art by the
    // nearest texel (ADR 0158).
    LookPipeline* exactSlot = nullptr;
    // Not into a headset's image, whose format the window's twins were not
    // made for: sprites in their own colours are a flat game's.
    if (spriteExactLive_ && !intoEye) {
        LookPipeline& slot = graded ? (intoWindow ? exactGradedTonemapWindow_ : exactGradedTonemap_)
                                    : (intoWindow ? exactTonemapWindow_ : exactTonemap_);
        if (ensureLookPipeline(device, slot, graded ? "tonemap_graded_exact" : "tonemap_exact",
                               intoWindow ? target.colorFormat : kLdrFormat))
            exactSlot = &slot;
    }
    // The world's interface as the display shows it (ADR 0250), last: after
    // the sprites' mask in the twins that read one, in its place otherwise.
    const rhi::TextureBinding uiLayer{uiLayerLive_ ? uiLayer_ : nothingPixel_, linearSampler_};
    std::array<rhi::TextureBinding, 5> tonemapTextures{
        rhi::TextureBinding{sceneColor, environmentSampler_}, rhi::TextureBinding{bloom_[0], environmentSampler_},
        rhi::TextureBinding{metered, linearSampler_},
        rhi::TextureBinding{exactSlot != nullptr ? spriteMask_ : whitePixel_, pointSampler_}, uiLayer};
    if (exactSlot == nullptr)
        tonemapTextures[3] = uiLayer;
    const std::span<const rhi::TextureBinding> tonemapBindings{tonemapTextures.data(),
                                                               exactSlot != nullptr ? usize{5} : usize{4}};

    // With anti-aliasing on, this writes the LDR texture the resolve reads and
    // the resolve is what reaches the target. With it off, this IS the resolve
    // -- and it is also where a reduced render scale is upscaled, because a
    // fullscreen pass into a larger target sampling a smaller source is exactly
    // a bilinear upscale.
    cmd.pushDebugGroup("tonemap");
    const rhi::PipelineHandle plain =
        intoEye ? (ensureLookPipeline(device, tonemapEye_, "tonemap", target.colorFormat) ? tonemapEye_.handle
                                                                                          : rhi::PipelineHandle{})
        : intoWindow && tonemapWindowPipeline_.valid() ? tonemapWindowPipeline_
                                                       : tonemapPipeline_;
    // Folded into the forward pass (ADR 0250), the picture is already there.
    if (foldExposure_ <= 0.0f)
        fullscreenPass(cmd,
                       exactSlot != nullptr ? exactSlot->handle
                       : graded             ? gradedSlot.handle
                                            : plain,
                       resolve ? ldr_ : target.color, resolve ? renderWidth_ : target.width,
                       resolve ? renderHeight_ : target.height, "tonemap", tonemapBindings,
                       asBytes(&tonemap, sizeof(tonemap)), rhi::LoadOp::Clear,
                       graded ? asBytes(&grade, sizeof(grade)) : std::span<const std::byte>{});
    cmd.popDebugGroup();

    // --- Anti-aliasing -------------------------------------------------------
    ENG_PROFILE_NEXT(passes, "render.aa");
    //
    // The spatial pass, the upscale and the sharpening (`resolvePicture`).
    if (resolve)
        resolvePicture(device, cmd, target, spatial, upscale, sharpen);
    if (motionShown)
        showMotion(device, cmd, target);

    // A game's highlights (ADR 0129), over the finished image and under the
    // editor's own mark.
    drawHighlights(cmd, device, world, meshes, target);

    // --- The editor's selection silhouette -----------------------------------
    ENG_PROFILE_NEXT(passes, "render.selection");
    //
    // **Last, over the finished image, and only when something is selected.**
    // A game's draw list never carries `outlined`, so a packaged build walks
    // this loop once and does nothing -- which is what keeps the pass out of
    // every golden in the repository without the renderer knowing what an
    // editor is.
    //
    // After tonemapping rather than before it: the outline is a tool's mark on
    // a picture, not a thing in the world, and running it through exposure and
    // a tone curve would make its colour depend on how bright the scene is.
    bool anyOutlined = false;
    for (const DrawItem& draw : world.draws) {
        if (draw.outlined) {
            anyOutlined = true;
            break;
        }
    }
    if (!anyOutlined)
        return;

    cmd.pushDebugGroup("outline");
    cmd.beginRenderPass({
        .colorAttachments = std::array<rhi::ColorAttachment, 1>{rhi::ColorAttachment{
            .texture = outlineMask_,
            .loadOp = rhi::LoadOp::Clear,
            .storeOp = rhi::StoreOp::Store,
        }},
        .debugName = "outline-mask",
    });
    cmd.setViewport({.width = static_cast<f32>(renderWidth_), .height = static_cast<f32>(renderHeight_)});
    cmd.setScissor({.width = static_cast<core::i32>(renderWidth_), .height = static_cast<core::i32>(renderHeight_)});
    if (world.camera.valid) {
        cmd.setPipeline(outlinePipeline_);
        drawGeometry(cmd, world, meshes, world.camera.unjitteredViewProjection, outlinePipeline_,
                     outlineSkinnedPipeline_, Selection::Outline);
    }
    cmd.endRenderPass();

    GpuOutlineUniforms outline;
    outline.texelWidth[0] = 1.0f / static_cast<f32>(renderWidth_);
    outline.texelWidth[1] = 1.0f / static_cast<f32>(renderHeight_);
    // In mask texels rather than in output pixels, because that is what the
    // taps step in. At a reduced render scale the line thins with everything
    // else, which is what a person who asked for a smaller image meant.
    outline.texelWidth[2] = 2.0f;
    // Orange, because nothing in a PBR scene is and it stays legible against
    // both the lit and the shadowed halves of a frame -- the same colour and the
    // same reason the wire box had.
    outline.color[0] = 1.0f;
    outline.color[1] = 0.45f;
    outline.color[2] = 0.05f;
    outline.color[3] = 1.0f;
    // Faint on purpose. This is what separates a selected object from an
    // unselected one standing in front of it, and it must not hide the colour
    // somebody selected the part in order to change.
    outline.fillColor[0] = 1.0f;
    outline.fillColor[1] = 0.45f;
    outline.fillColor[2] = 0.05f;
    outline.fillColor[3] = 0.12f;

    const std::array<rhi::TextureBinding, 1> maskBinding{rhi::TextureBinding{outlineMask_, linearSampler_}};
    fullscreenPass(cmd, outlineCompositePipeline_, target.color, target.width, target.height, "outline-composite",
                   maskBinding, asBytes(&outline, sizeof(outline)), rhi::LoadOp::Load);
    cmd.popDebugGroup();
}

std::unique_ptr<IRenderer> createDefaultRenderer()
{
    return std::make_unique<DefaultRenderer>();
}

} // namespace engine::render
