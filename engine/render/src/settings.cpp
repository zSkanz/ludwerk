#include "engine/render/settings.h"

#include <algorithm>

#include "engine/render/clusters.h"
#include "engine/render/shadow.h"

namespace engine::render {

GraphicsSettings settingsFor(QualityLevel quality) noexcept
{
    GraphicsSettings settings;
    settings.quality = quality;

    switch (quality) {
    case QualityLevel::Low:
        // Everything that costs fragments, turned down or off. Render scale is
        // the first dial rather than the last because it is the only one that
        // reduces EVERY per-pixel pass at once, and a machine that needs this
        // preset is a machine that is fragment-bound.
        settings.renderScale = 0.75f;
        settings.shadowTileResolution = 512;
        settings.shadowCascades = 2;
        // Half the rate and half as far: a cape on a phone is the hero's.
        settings.secondaryMotionRate = 60;
        settings.secondaryMotionDistance = 30.0f;
        settings.shadowTaps = shadowTapsFor(QualityLevel::Low);
        settings.terrainLean = true;
        settings.shadowDistance = 70.0f;
        settings.lightBudget = 32;
        settings.reflectionProbes = 0;
        settings.indirectLight = false;
        settings.bloom = false;
        settings.ambientOcclusion = false;
        settings.contactShadows = false;
        settings.foliageDecals = false;
        settings.terrainPixelError = 4.0f;
        settings.antiAliasing = AntiAliasingMode::Fxaa;
        // The world at three quarters, brought up by FSR 1 rather than
        // filtered: the edges a reduced scale softens, kept (ADR 0158).
        settings.upscaling = UpscalingMode::Fsr1;
        settings.autoExposure = true;
        settings.depthOfField = false;
        settings.sunRays = false;
        break;

    case QualityLevel::Medium:
        settings.renderScale = 1.0f;
        settings.shadowTileResolution = 1024;
        settings.shadowCascades = 3;
        settings.shadowTaps = shadowTapsFor(QualityLevel::Medium);
        settings.shadowDistance = 100.0f;
        settings.lightBudget = 96;
        settings.reflectionProbes = 8;
        settings.reflectionProbeSize = 64;
        settings.indirectLight = false;
        settings.bloom = true;
        settings.ambientOcclusion = false;
        settings.contactShadows = true;
        settings.foliageDecals = true;
        settings.terrainPixelError = 3.0f;
        settings.antiAliasing = AntiAliasingMode::Smaa;
        settings.autoExposure = true;
        settings.depthOfField = false;
        break;

    case QualityLevel::High:
        // The lighting work's defaults, to the value. See the header for why that is a
        // requirement and not an accident.
        settings.renderScale = 1.0f;
        settings.shadowTileResolution = kShadowTileResolution;
        settings.shadowCascades = kShadowCascadeCount;
        settings.shadowDistance = kShadowDistance;
        settings.lightBudget = kMaxClusteredLights;
        settings.bloom = true;
        settings.ambientOcclusion = true;
        settings.contactShadows = true;
        settings.foliageDecals = true;
        settings.terrainPixelError = 2.0f;
        // SMAA rather than FXAA (ADR 0158): an edge found by its shape and
        // blended by the area it covers, which keeps what FXAA blurs.
        settings.antiAliasing = AntiAliasingMode::Smaa;
        settings.autoExposure = true;
        break;

    case QualityLevel::Ultra:
        // The one preset that spends MORE than the engine's default. Its whole
        // content is shadow quality, because that is where this renderer's
        // remaining headroom visibly goes: four times the atlas, spent on
        // DENSITY rather than on range.
        //
        // **160 metres rather than 220, and an earlier defect is why.** A cascade's texel is
        // its box divided by its tile, and the far cascade's box is set by the
        // frustum's cross-section at the shadow distance -- so pushing the
        // distance out costs resolution in proportion. At 220 metres the far
        // cascade measured 0.32 m per texel against High's 0.35: four times the
        // atlas bought nine per cent, and a preset called Ultra was, for
        // anything past thirty metres, exactly as blocky as the one below it.
        // At 160 it measures 0.23 -- half again finer than High, and still a
        // third further out.
        settings.renderScale = 1.0f;
        settings.shadowTileResolution = 2048;
        settings.shadowCascades = 4;
        settings.shadowDistance = 160.0f;
        settings.lightBudget = kMaxClusteredLights;
        settings.reflectionProbes = 32;
        // **What is read from the screen is this level's alone** (ADR
        // 0209), decided on pictures of three real scenes: at `High` two of
        // them changed little or nothing and paid the passes all the same.
        // Nothing a game shipped at `High` changes its look or its frame
        // time; a game that wants either below `Ultra` sets it.
        settings.screenReflectionSteps = 48;
        settings.screenBounceRays = 8;
        settings.bloom = true;
        settings.ambientOcclusion = true;
        settings.contactShadows = true;
        settings.foliageDecals = true;
        settings.terrainPixelError = 1.5f;
        // The temporal pass: what still crawls in motion under SMAA -- a fence,
        // a wire, the far grass -- is sampled at eight places over eight frames.
        settings.antiAliasing = AntiAliasingMode::Taa;
        settings.autoExposure = true;
        break;
    }

    return settings;
}

u32 shadowTapsFor(QualityLevel level) noexcept
{
    switch (level) {
    case QualityLevel::Low:
        return 4;
    case QualityLevel::Medium:
        return 8;
    case QualityLevel::High:
    case QualityLevel::Ultra:
        break;
    }
    return 0;
}

QualityLevel defaultQuality(bool handheld) noexcept
{
    return handheld ? QualityLevel::Medium : QualityLevel::High;
}

ReflectionLevel reflectionsFor(core::i32 level, bool handheld) noexcept
{
    // `Enum.GraphicsLevel`: Low, Medium, High, Ultra, Cinematic.
    static constexpr ReflectionLevel Levels[]{{0, 64}, {8, 64}, {16, 128}, {32, 128}, {32, 128}};
    ReflectionLevel chosen = Levels[std::clamp(level, 0, 4)];
    // A phone keeps half as many, and none below `High` (ADR 0202): a pixel
    // inside a probe's box costs a fifth more of the forward pass on a small
    // GPU -- 1.3 ms of 7.1 at 1080 lines on an integrated one -- and a
    // phone's default level has no room for it until one says it has.
    if (handheld)
        chosen.probes = level >= 2 ? chosen.probes / 2 : 0;
    return chosen;
}

bool indirectLightFor(core::i32 level) noexcept
{
    return level >= 2;
}

u32 screenReflectionStepsFor(core::i32 level) noexcept
{
    // `High` and `Ultra` (ADR 0209): what a mirror ray needs to cross a room
    // without stepping through a chair's leg, and twice that.
    return level >= 3 ? 48u : level == 2 ? 24u : 0u;
}

u32 screenBounceRaysFor(core::i32 level) noexcept
{
    return level >= 3 ? 8u : level == 2 ? 4u : 0u;
}

GraphicsSettings handheldSettings(GraphicsSettings settings) noexcept
{
    // **A phone's world is drawn below its display and brought up by FSR 1**
    // (ADR 0158): every level caps the resolution but Ultra, and a picture
    // upscaled is a picture whose edges FSR keeps. FXAA rather than SMAA below
    // High, for the two passes it saves on a phone's GPU; never the temporal
    // pass, whose history is a full-resolution image more to keep.
    settings.upscaling = UpscalingMode::Fsr1;
    // **What a phone's GPU is spent on** (ADR 0172). Three levels of bloom
    // rather than five: five passes where there were nine. And below High no
    // contact shadows -- a pass over every pixel, twenty-one depth reads each,
    // for a few centimetres of shadow under what a phone's screen shows a
    // finger wide.
    settings.bloomLevels = 3;
    // Half the reflection probes and none below High, and no bounced light,
    // until each is measured on a phone to fit (ADRs 0202, 0203).
    settings.reflectionProbes /= 2;
    settings.indirectLight = false;
    // And nothing read from the screen at any level (ADR 0209): not measured
    // on a phone, which is a statement about the measuring.
    settings.screenReflectionSteps = 0;
    settings.screenBounceRays = 0;
    // Nor is a receiver under a caster told from one in the clear, at
    // any level, for the same reason: `--shadow-under-casters` times it.
    settings.shadowUnderCasters = false;
    if (settings.quality == QualityLevel::Low || settings.quality == QualityLevel::Medium) {
        settings.reflectionProbes = 0;
        settings.contactShadows = false;
        // And no second pass over the grass a decal covers (ADR 0185): the
        // decal is on the ground under it.
        settings.foliageDecals = false;
    }
    // And the lean ground at Medium as well as at Low (ADR 0175): the terrain's
    // fragment was two thirds of a phone's frame.
    if (settings.quality == QualityLevel::Medium)
        settings.terrainLean = true;
    // **And at both, the fast ground over it** (ADR 0179). Measured on the
    // phone this was written for: the lean ground held fifty-six frames a
    // second at Low and forty-one at Medium, the fast one sixty at both and
    // at Medium drawn at the display's own size. A project that wants the
    // other says so (`[graphics.android] terrain_surface`).
    if (settings.quality == QualityLevel::Low || settings.quality == QualityLevel::Medium)
        settings.terrainFast = true;
    if (settings.quality == QualityLevel::Low || settings.quality == QualityLevel::Medium)
        settings.antiAliasing = AntiAliasingMode::Fxaa;
    else if (settings.antiAliasing == AntiAliasingMode::Taa)
        settings.antiAliasing = AntiAliasingMode::Smaa;
    switch (settings.quality) {
    case QualityLevel::Low:
        settings.renderResolutionCap = 720;
        break;
    case QualityLevel::Medium:
        settings.renderResolutionCap = 900;
        break;
    case QualityLevel::High:
        settings.renderResolutionCap = 1080;
        break;
    case QualityLevel::Ultra:
        settings.renderResolutionCap = 0;
        break;
    }
    return settings;
}

f32 effectiveRenderScale(const GraphicsSettings& settings, u32 width, u32 height) noexcept
{
    f32 scale = settings.renderScale;
    const u32 shorter = width < height ? width : height;
    if (settings.renderResolutionCap > 0 && shorter > 0) {
        const f32 capped = static_cast<f32>(settings.renderResolutionCap) / static_cast<f32>(shorter);
        if (capped < scale)
            scale = capped;
    }
    return scale;
}

GraphicsSettings clampSettings(GraphicsSettings settings) noexcept
{
    // A cap below 360 is a picture of blocks; zero is "none".
    if (settings.renderResolutionCap != 0)
        settings.renderResolutionCap = std::clamp(settings.renderResolutionCap, 360u, 4320u);

    // **A floor of a third** (ADR 0164): the furthest FSR 2 is made to bring a
    // picture up from -- its "ultra performance", three to one a side. It was a
    // half while the only upscales read one frame, and under those a third
    // still looks like what it is; the scale is the game's to choose.
    settings.renderScale = std::clamp(settings.renderScale, 1.0f / 3.0f, 1.0f);

    // Powers of two between 256 and 4096, and 4096 is the ceiling because it is
    // the 2D texture size the weakest conforming device is required to support
    // -- an atlas is two tiles across, so 2048 is the largest tile that fits it.
    settings.shadowTileResolution = std::clamp(settings.shadowTileResolution, 256u, 2048u);
    u32 rounded = 256;
    while (rounded * 2 <= settings.shadowTileResolution)
        rounded *= 2;
    settings.shadowTileResolution = rounded;

    settings.shadowCascades = std::min(settings.shadowCascades, kShadowCascadeCount);
    settings.shadowDistance = std::clamp(settings.shadowDistance, 10.0f, 1000.0f);
    settings.lightBudget = std::min(settings.lightBudget, kMaxClusteredLights);
    // Sixteen and more is the filter as it ships, which zero says.
    if (settings.shadowTaps >= 16)
        settings.shadowTaps = 0;
    settings.measuredShadowTaps = std::min(settings.measuredShadowTaps, 16u);
    settings.bloomLevels = std::clamp(settings.bloomLevels, 2u, 5u);
    return settings;
}

std::optional<DebugView> parseDebugView(std::string_view name) noexcept
{
    if (name == "none")
        return DebugView::None;
    if (name == "holes")
        return DebugView::Holes;
    if (name == "level")
        return DebugView::Level;
    if (name == "sky")
        return DebugView::Sky;
    if (name == "shadow")
        return DebugView::Shadow;
    if (name == "occlusion")
        return DebugView::Occlusion;
    if (name == "bend")
        return DebugView::Bend;
    if (name == "albedo")
        return DebugView::Albedo;
    if (name == "material")
        return DebugView::Material;
    if (name == "motion")
        return DebugView::Motion;
    if (name == "side")
        return DebugView::Side;
    return std::nullopt;
}

std::optional<QualityLevel> parseQuality(std::string_view name) noexcept
{
    if (name == "low")
        return QualityLevel::Low;
    if (name == "medium")
        return QualityLevel::Medium;
    if (name == "high")
        return QualityLevel::High;
    if (name == "ultra")
        return QualityLevel::Ultra;
    return std::nullopt;
}

std::string_view qualityName(QualityLevel quality) noexcept
{
    switch (quality) {
    case QualityLevel::Low:
        return "low";
    case QualityLevel::Medium:
        return "medium";
    case QualityLevel::High:
        return "high";
    case QualityLevel::Ultra:
        return "ultra";
    }
    return "high";
}

} // namespace engine::render
