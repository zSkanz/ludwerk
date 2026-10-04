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
        settings.shadowDistance = 70.0f;
        settings.lightBudget = 32;
        settings.bloom = false;
        settings.ambientOcclusion = false;
        settings.contactShadows = false;
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
        settings.shadowDistance = 100.0f;
        settings.lightBudget = 96;
        settings.bloom = true;
        settings.ambientOcclusion = false;
        settings.contactShadows = true;
        settings.terrainPixelError = 3.0f;
        settings.antiAliasing = AntiAliasingMode::Smaa;
        settings.autoExposure = true;
        settings.depthOfField = false;
        break;

    case QualityLevel::High:
        // The M7.5 defaults, to the value. See the header for why that is a
        // requirement and not an accident.
        settings.renderScale = 1.0f;
        settings.shadowTileResolution = kShadowTileResolution;
        settings.shadowCascades = kShadowCascadeCount;
        settings.shadowDistance = kShadowDistance;
        settings.lightBudget = kMaxClusteredLights;
        settings.bloom = true;
        settings.ambientOcclusion = true;
        settings.contactShadows = true;
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
        // **160 metres rather than 220, and D052 is why.** A cascade's texel is
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
        settings.bloom = true;
        settings.ambientOcclusion = true;
        settings.contactShadows = true;
        settings.terrainPixelError = 1.5f;
        // The temporal pass: what still crawls in motion under SMAA -- a fence,
        // a wire, the far grass -- is sampled at eight places over eight frames.
        settings.antiAliasing = AntiAliasingMode::Taa;
        settings.autoExposure = true;
        break;
    }

    return settings;
}

QualityLevel defaultQuality(bool handheld) noexcept
{
    return handheld ? QualityLevel::Medium : QualityLevel::High;
}

GraphicsSettings handheldSettings(GraphicsSettings settings) noexcept
{
    // **A phone's world is drawn below its display and brought up by FSR 1**
    // (ADR 0158): every level caps the resolution but Ultra, and a picture
    // upscaled is a picture whose edges FSR keeps. FXAA rather than SMAA below
    // High, for the two passes it saves on a phone's GPU; never the temporal
    // pass, whose history is a full-resolution image more to keep.
    settings.upscaling = UpscalingMode::Fsr1;
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
