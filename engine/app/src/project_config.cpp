#include "engine/app/project_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <span>
#include <sstream>

#include "engine/core/i18n.h"
#include "engine/core/toml.h"
#include "engine/core/toml_edit.h"
#include "engine/platform/file.h"
#include "engine/scene/localization.h"

namespace engine::app {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::usize;
using render::GraphicsSettings;

[[nodiscard]] bool readFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// The middle layer: what the project file says, over the preset and under the
// command line. Every key is optional and an absent one leaves the preset's
// value, which is what makes `[graphics] quality = "low"` plus one override a
// two-line section rather than a full recital.
// **A number the file gives, if it is one this key can hold** (audit F14):
// checked as the double it was read as, before any cast -- a negative, a huge
// or a NaN `u32` conversion is undefined behaviour, not a large number. One
// outside the range is ignored, as a key the file did not give.
[[nodiscard]] std::optional<f64> numberIn(const core::TomlDocument& document, const char* key, f64 lowest, f64 highest)
{
    const std::optional<f64> value = document.number(key);
    if (!value.has_value() || !std::isfinite(*value) || *value < lowest || *value > highest)
        return std::nullopt;
    return value;
}

// `table` is `graphics`, or a platform's own: `graphics.android`.
void applyFile(const core::TomlDocument& document, std::string_view table, GraphicsSettings& settings)
{
    const auto key = [table](std::string_view name) {
        std::string full(table);
        full += '.';
        full += name;
        return full;
    };
    const auto number = [&](std::string_view name, f64 lowest, f64 highest) {
        return numberIn(document, key(name).c_str(), lowest, highest);
    };
    const auto flag = [&](std::string_view name) { return document.boolean(key(name)); };

    if (const std::optional<f64> value = number("render_scale", 0.05, 4.0))
        settings.renderScale = static_cast<f32>(*value);
    // Zero is "no cap": the display's own resolution.
    if (const std::optional<f64> value = number("render_cap", 0.0, 16384.0))
        settings.renderResolutionCap = static_cast<u32>(*value);
    if (const std::optional<f64> value = number("shadow_resolution", 0.0, 16384.0))
        settings.shadowTileResolution = static_cast<u32>(*value);
    if (const std::optional<f64> value = number("shadow_cascades", 0.0, 16.0))
        settings.shadowCascades = static_cast<u32>(*value);
    if (const std::optional<f64> value = number("shadow_distance", 0.0, 1.0e6))
        settings.shadowDistance = static_cast<f32>(*value);
    if (const std::optional<f64> value = number("light_budget", 0.0, 65536.0))
        settings.lightBudget = static_cast<u32>(*value);
    if (const std::optional<bool> value = flag("bloom"))
        settings.bloom = *value;
    if (const std::optional<bool> value = flag("ambient_occlusion"))
        settings.ambientOcclusion = *value;
    if (const std::optional<bool> value = flag("anti_aliasing"))
        settings.antiAliasing = *value;
    // So each of the audit's suspects for the dots on distant terrain can be
    // turned off alone (terrain audit T0).
    if (const std::optional<bool> value = flag("contact_shadows"))
        settings.contactShadows = *value;
    if (const std::optional<bool> value = flag("auto_exposure"))
        settings.autoExposure = *value;
    if (const std::optional<bool> value = flag("depth_of_field"))
        settings.depthOfField = *value;
    if (const std::optional<bool> value = flag("sun_rays"))
        settings.sunRays = *value;
}

// **The table a platform has to itself** (ADR 0147 section 2): `[graphics.android]`
// over `[graphics]`, as those engines' quality matrices give each platform
// its own column. A handheld is Android; a desktop is the system it was built
// for.
[[nodiscard]] std::string_view platformTable(bool handheld) noexcept
{
    if (handheld)
        return "graphics.android";
#if defined(_WIN32)
    return "graphics.windows";
#elif defined(__APPLE__)
    return "graphics.macos";
#else
    return "graphics.linux";
#endif
}

// A preset, and what the machine's kind adds to it.
[[nodiscard]] GraphicsSettings presetFor(render::QualityLevel level, bool handheld) noexcept
{
    const GraphicsSettings settings = render::settingsFor(level);
    return handheld ? render::handheldSettings(settings) : settings;
}

void applyOverrides(const GraphicsOverrides& overrides, GraphicsSettings& settings)
{
    if (overrides.renderScale)
        settings.renderScale = *overrides.renderScale;
    if (overrides.shadowResolution)
        settings.shadowTileResolution = *overrides.shadowResolution;
    if (overrides.shadowCascades)
        settings.shadowCascades = *overrides.shadowCascades;
    if (overrides.shadowDistance)
        settings.shadowDistance = *overrides.shadowDistance;
    if (overrides.lightBudget)
        settings.lightBudget = *overrides.lightBudget;
    if (overrides.bloom)
        settings.bloom = *overrides.bloom;
    if (overrides.ambientOcclusion)
        settings.ambientOcclusion = *overrides.ambientOcclusion;
    if (overrides.antiAliasing)
        settings.antiAliasing = *overrides.antiAliasing;
    if (overrides.autoExposure)
        settings.autoExposure = *overrides.autoExposure;
    if (overrides.contactShadows)
        settings.contactShadows = *overrides.contactShadows;
    if (overrides.forcedSurface)
        settings.forcedSurface = *overrides.forcedSurface;
    if (overrides.debugView) {
        settings.debugView = *overrides.debugView;
        // The sky and the ground must stay two colours to the last pixel: a
        // crack a pixel wide blended half into the ground is a crack missed.
        // And a shadow's, a bend's, a colour's or a layer's value must be the
        // shader's, not an exposure's.
        if (settings.debugView == render::DebugView::Holes || render::blackSky(settings.debugView)) {
            settings.antiAliasing = false;
            settings.bloom = false;
            settings.ambientOcclusion = false;
            settings.autoExposure = false;
            settings.depthOfField = false;
            settings.sunRays = false;
        }
    }
}

// --- The same, as a model's layers (ADR 0147) ----------------------------------

using scene::GraphicsLayer;
using scene::GraphicsSetting;

// A value as its setting holds it: inside the setting's range.
void say(GraphicsLayer& layer, GraphicsSetting setting, f64 value)
{
    const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(setting);
    layer.put(setting, std::clamp(value, info.lowest, info.highest));
}

// Everything a `GraphicsSettings` holds, said to a layer.
void saySettings(GraphicsLayer& layer, const GraphicsSettings& settings)
{
    say(layer, GraphicsSetting::RenderScale, static_cast<f64>(settings.renderScale));
    say(layer, GraphicsSetting::ShadowResolution, static_cast<f64>(settings.shadowTileResolution));
    say(layer, GraphicsSetting::ShadowCascades, static_cast<f64>(settings.shadowCascades));
    say(layer, GraphicsSetting::ShadowDistance, static_cast<f64>(settings.shadowDistance));
    say(layer, GraphicsSetting::LightBudget, static_cast<f64>(settings.lightBudget));
    say(layer, GraphicsSetting::Bloom, settings.bloom ? 1.0 : 0.0);
    say(layer, GraphicsSetting::AmbientOcclusion, settings.ambientOcclusion ? 1.0 : 0.0);
    say(layer, GraphicsSetting::ContactShadows, settings.contactShadows ? 1.0 : 0.0);
    say(layer, GraphicsSetting::AntiAliasing, settings.antiAliasing ? 1.0 : 0.0);
    say(layer, GraphicsSetting::AutoExposure, settings.autoExposure ? 1.0 : 0.0);
    say(layer, GraphicsSetting::DepthOfField, settings.depthOfField ? 1.0 : 0.0);
    say(layer, GraphicsSetting::SunRays, settings.sunRays ? 1.0 : 0.0);
    // The ground's detail is a scale on `High`'s two pixels a cell.
    say(layer, GraphicsSetting::TerrainDetail, 2.0 / static_cast<f64>(settings.terrainPixelError));
    say(layer, GraphicsSetting::RenderResolutionCap, static_cast<f64>(settings.renderResolutionCap));
}

// What each level gives the settings the renderer has no number for yet, and
// the two it has that a preset never varied.
struct LevelExtras
{
    f64 foliageDensity;
    f64 textureQuality;
    f64 anisotropicFiltering;
    f64 lodBias;
    f64 maximumLodLevel;
    f64 particleBudget;
    f64 softParticles;
    f64 skinWeights;
    f64 level;
};
constexpr std::array<LevelExtras, scene::kQualityPresets> Extras{{
    {0.5, 0.0, 2.0, 0.5, 1.0, 16384.0, 0.0, 2.0, 0.0},
    {0.75, 1.0, 4.0, 0.75, 0.0, 32768.0, 1.0, 4.0, 1.0},
    {1.0, 2.0, 8.0, 1.0, 0.0, 65536.0, 1.0, 4.0, 2.0},
    {1.0, 2.0, 16.0, 1.5, 0.0, 131072.0, 1.0, 4.0, 3.0},
}};

void seedPresets(scene::GraphicsModel& model, bool handheld)
{
    for (usize index = 0; index < scene::kQualityPresets; ++index) {
        GraphicsLayer& layer = model.presets[index];
        layer = GraphicsLayer{};
        saySettings(layer, render::clampSettings(presetFor(static_cast<render::QualityLevel>(index), handheld)));
        const LevelExtras& extras = Extras[index];
        // A shadow quality is one past the level: zero is off.
        say(layer, GraphicsSetting::ShadowQuality, static_cast<f64>(index) + 1.0);
        say(layer, GraphicsSetting::FoliageDensity, extras.foliageDensity);
        say(layer, GraphicsSetting::TextureQuality, extras.textureQuality);
        say(layer, GraphicsSetting::AnisotropicFiltering, extras.anisotropicFiltering);
        say(layer, GraphicsSetting::LODBias, extras.lodBias);
        say(layer, GraphicsSetting::MaximumLODLevel, extras.maximumLodLevel);
        say(layer, GraphicsSetting::ParticleBudget, extras.particleBudget);
        say(layer, GraphicsSetting::SoftParticles, extras.softParticles);
        say(layer, GraphicsSetting::SkinWeights, extras.skinWeights);
        say(layer, GraphicsSetting::FogQuality, extras.level);
        say(layer, GraphicsSetting::GlobalIllumination, extras.level);
        say(layer, GraphicsSetting::Reflections, extras.level);
    }
    model.defaultLevel = static_cast<core::i32>(render::defaultQuality(handheld));
    model.autoLevel = model.defaultLevel;
}

void seedCommandLine(scene::GraphicsModel& model, const GraphicsOverrides& overrides)
{
    GraphicsLayer& layer = model.commandLine;
    layer = GraphicsLayer{};
    if (overrides.quality)
        say(layer, GraphicsSetting::QualityLevel, static_cast<f64>(*overrides.quality));
    if (overrides.renderScale)
        say(layer, GraphicsSetting::RenderScale, static_cast<f64>(*overrides.renderScale));
    if (overrides.shadowResolution)
        say(layer, GraphicsSetting::ShadowResolution, static_cast<f64>(*overrides.shadowResolution));
    if (overrides.shadowCascades)
        say(layer, GraphicsSetting::ShadowCascades, static_cast<f64>(*overrides.shadowCascades));
    if (overrides.shadowDistance)
        say(layer, GraphicsSetting::ShadowDistance, static_cast<f64>(*overrides.shadowDistance));
    if (overrides.lightBudget)
        say(layer, GraphicsSetting::LightBudget, static_cast<f64>(*overrides.lightBudget));
    if (overrides.bloom)
        say(layer, GraphicsSetting::Bloom, *overrides.bloom ? 1.0 : 0.0);
    if (overrides.ambientOcclusion)
        say(layer, GraphicsSetting::AmbientOcclusion, *overrides.ambientOcclusion ? 1.0 : 0.0);
    if (overrides.antiAliasing)
        say(layer, GraphicsSetting::AntiAliasing, *overrides.antiAliasing ? 1.0 : 0.0);
    if (overrides.autoExposure)
        say(layer, GraphicsSetting::AutoExposure, *overrides.autoExposure ? 1.0 : 0.0);
    if (overrides.contactShadows)
        say(layer, GraphicsSetting::ContactShadows, *overrides.contactShadows ? 1.0 : 0.0);
    if (overrides.vsync)
        say(layer, GraphicsSetting::VSync, *overrides.vsync ? 1.0 : 0.0);
    if (overrides.maxFrameRate)
        say(layer, GraphicsSetting::MaxFrameRate, static_cast<f64>(*overrides.maxFrameRate));
    if (overrides.backgroundFrameRate)
        say(layer, GraphicsSetting::BackgroundFrameRate, static_cast<f64>(*overrides.backgroundFrameRate));
}

// A setting's key in `project.toml`: its name in snake case, a run of capitals
// one word -- `MaximumLODLevel` is `maximum_lod_level`.
[[nodiscard]] std::string keyOf(std::string_view name)
{
    std::string key;
    for (usize index = 0; index < name.size(); ++index) {
        const char c = name[index];
        const bool upper = c >= 'A' && c <= 'Z';
        if (upper && index > 0) {
            const bool previousLower = name[index - 1] >= 'a' && name[index - 1] <= 'z';
            const bool nextLower = index + 1 < name.size() && name[index + 1] >= 'a' && name[index + 1] <= 'z';
            const bool previousUpper = name[index - 1] >= 'A' && name[index - 1] <= 'Z';
            if (previousLower || (previousUpper && nextLower))
                key += '_';
        }
        key += upper ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return key;
}

// A setting's key inside its table. Three are older than the rule.
[[nodiscard]] std::string nameInFile(GraphicsSetting setting)
{
    if (setting == GraphicsSetting::VSync)
        return "vsync";
    if (setting == GraphicsSetting::RenderResolutionCap)
        return "render_cap";
    if (setting == GraphicsSetting::QualityLevel)
        return "quality";
    return keyOf(scene::graphicsSettingInfo(setting).name);
}

// The items of the enums a setting is one of, lowercase, as a file names them.
[[nodiscard]] std::span<const std::string_view> choicesOf(GraphicsSetting setting) noexcept
{
    static constexpr std::array<std::string_view, 5> Shadow{"off", "low", "medium", "high", "ultra"};
    static constexpr std::array<std::string_view, 2> Smoothing{"off", "fxaa"};
    static constexpr std::array<std::string_view, 3> Texture{"low", "medium", "high"};
    static constexpr std::array<std::string_view, 3> Window{"windowed", "borderless", "fullscreen"};
    static constexpr std::array<std::string_view, 5> Level{"low", "medium", "high", "ultra", "cinematic"};
    switch (setting) {
    case GraphicsSetting::ShadowQuality:
        return Shadow;
    case GraphicsSetting::AntiAliasing:
        return Smoothing;
    case GraphicsSetting::TextureQuality:
        return Texture;
    case GraphicsSetting::WindowMode:
        return Window;
    case GraphicsSetting::FogQuality:
    case GraphicsSetting::GlobalIllumination:
    case GraphicsSetting::Reflections:
        return Level;
    default:
        return {};
    }
}

// What a table of the file says about one setting, as the setting's value.
[[nodiscard]] std::optional<f64> fileValue(const core::TomlDocument& document, const std::string& key,
                                           GraphicsSetting setting)
{
    const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(setting);
    if (info.kind == scene::GraphicsValueKind::Flag) {
        if (const std::optional<bool> value = document.boolean(key))
            return *value ? 1.0 : 0.0;
        return std::nullopt;
    }
    if (info.kind == scene::GraphicsValueKind::Choice) {
        // By name; and a switch where the choice is off or the first way on,
        // which is how `anti_aliasing = true` has always been written.
        if (const std::optional<std::string_view> named = document.string(key)) {
            const std::span<const std::string_view> names = choicesOf(setting);
            for (usize index = 0; index < names.size(); ++index) {
                if (names[index] == *named)
                    return static_cast<f64>(index);
            }
            return std::nullopt;
        }
        if (const std::optional<bool> value = document.boolean(key))
            return *value ? 1.0 : 0.0;
        return std::nullopt;
    }
    const std::optional<f64> value = document.number(key);
    if (!value.has_value() || !std::isfinite(*value))
        return std::nullopt;
    return value;
}

// One table of the file -- `graphics`, a platform's own, or `display` -- into
// the project's layer: the quality settings from the first two, the display's
// from the third.
void sayTable(const core::TomlDocument& document, std::string_view table, bool display, GraphicsLayer& layer)
{
    for (usize index = 0; index < scene::kGraphicsSettingCount; ++index) {
        const auto setting = static_cast<GraphicsSetting>(index);
        const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(setting);
        if (info.quality == display || setting == GraphicsSetting::QualityLevel ||
            setting == GraphicsSetting::ResolutionWidth || setting == GraphicsSetting::ResolutionHeight)
            continue;
        std::string key(table);
        key += '.';
        key += nameInFile(setting);
        if (const std::optional<f64> value = fileValue(document, key, setting))
            say(layer, setting, *value);
    }
}

} // namespace

std::string projectKeyOf(scene::GraphicsSetting setting)
{
    const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(setting);
    const bool display = !info.quality && setting != GraphicsSetting::QualityLevel;
    return std::string(display ? "display." : "graphics.") + nameInFile(setting);
}

std::span<const std::string_view> projectChoicesOf(scene::GraphicsSetting setting) noexcept
{
    static constexpr std::array<std::string_view, 6> Quality{"low", "medium", "high", "ultra", "custom", "auto"};
    if (setting == GraphicsSetting::QualityLevel)
        return Quality;
    return choicesOf(setting);
}

void seedGraphicsModel(scene::GraphicsModel& model, const GraphicsOverrides& overrides, bool handheld)
{
    seedPresets(model, handheld);
    seedCommandLine(model, overrides);
}

render::GraphicsSettings graphicsSettingsOf(const scene::GraphicsModel& model, bool handheld,
                                            const render::GraphicsSettings* instruments)
{
    GraphicsSettings settings = presetFor(static_cast<render::QualityLevel>(model.preset()), handheld);
    const auto value = [&model](GraphicsSetting setting) { return model.effective(setting); };
    settings.renderScale = static_cast<f32>(value(GraphicsSetting::RenderScale));
    settings.shadowTileResolution = static_cast<u32>(value(GraphicsSetting::ShadowResolution));
    settings.shadowCascades = static_cast<u32>(value(GraphicsSetting::ShadowCascades));
    settings.shadowDistance = static_cast<f32>(value(GraphicsSetting::ShadowDistance));
    settings.lightBudget = static_cast<u32>(value(GraphicsSetting::LightBudget));
    settings.bloom = value(GraphicsSetting::Bloom) != 0.0;
    settings.ambientOcclusion = value(GraphicsSetting::AmbientOcclusion) != 0.0;
    settings.contactShadows = value(GraphicsSetting::ContactShadows) != 0.0;
    settings.antiAliasing = value(GraphicsSetting::AntiAliasing) != 0.0;
    settings.autoExposure = value(GraphicsSetting::AutoExposure) != 0.0;
    settings.depthOfField = value(GraphicsSetting::DepthOfField) != 0.0;
    settings.sunRays = value(GraphicsSetting::SunRays) != 0.0;
    settings.terrainPixelError = static_cast<f32>(2.0 / value(GraphicsSetting::TerrainDetail));
    settings.renderResolutionCap = static_cast<u32>(value(GraphicsSetting::RenderResolutionCap));
    if (instruments != nullptr) {
        GraphicsOverrides carried;
        if (!instruments->forcedSurface.empty())
            carried.forcedSurface = instruments->forcedSurface;
        if (instruments->debugView != render::DebugView::None)
            carried.debugView = instruments->debugView;
        applyOverrides(carried, settings);
    }
    return render::clampSettings(settings);
}

FramePacing pacingOf(const scene::GraphicsModel& model, bool handheld) noexcept
{
    FramePacing pacing;
    pacing.vsync = handheld || model.effective(GraphicsSetting::VSync) != 0.0;
    pacing.maxFrameRate = static_cast<core::u32>(model.effective(GraphicsSetting::MaxFrameRate));
    pacing.backgroundFrameRate = static_cast<core::u32>(model.effective(GraphicsSetting::BackgroundFrameRate));
    return pacing;
}

render::GraphicsSettings resolveGraphics(const GraphicsOverrides& overrides, bool handheld)
{
    GraphicsSettings settings = presetFor(overrides.quality.value_or(render::defaultQuality(handheld)), handheld);
    applyOverrides(overrides, settings);
    return render::clampSettings(settings);
}

namespace {

// `X.Y.Z`, three runs of digits and two dots (ADR 0104 §1).
[[nodiscard]] bool isVersion(std::string_view text) noexcept
{
    int parts = 0;
    std::size_t digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            ++digits;
            continue;
        }
        if (c != '.' || digits == 0)
            return false;
        ++parts;
        digits = 0;
    }
    return parts == 2 && digits > 0;
}

} // namespace

FramePacing pacingWith(FramePacing file, const GraphicsOverrides& overrides) noexcept
{
    if (overrides.vsync.has_value())
        file.vsync = *overrides.vsync;
    if (overrides.maxFrameRate.has_value())
        file.maxFrameRate = *overrides.maxFrameRate;
    if (overrides.backgroundFrameRate.has_value())
        file.backgroundFrameRate = *overrides.backgroundFrameRate;
    return file;
}

ProjectConfig loadProjectConfig(const std::filesystem::path& projectRoot, const GraphicsOverrides& overrides,
                                std::string* diagnostic, bool handheld)
{
    ProjectConfig config;
    seedGraphicsModel(config.graphicsModel, overrides, handheld);

    std::string text;
    if (projectRoot.empty() || !readFile(projectRoot / "project.toml", text)) {
        config.graphics = resolveGraphics(overrides, handheld);
        return config;
    }

    core::TomlDocument document;
    const core::TomlDocument::ParseResult parsed = document.parse(text, (projectRoot / "project.toml").string());
    if (!parsed) {
        if (diagnostic != nullptr)
            *diagnostic = parsed.diagnostic;
        config.graphics = resolveGraphics(overrides, handheld);
        return config;
    }

    if (const std::optional<std::string_view> value = document.string("project.name"))
        config.name = *value;
    if (const std::optional<std::string_view> value = document.string("project.id"))
        config.id = *value;
    if (const std::optional<std::string_view> value = document.string("project.icon"))
        config.icon = *value;
    if (const std::optional<std::string_view> value = document.string("project.scene"))
        config.scene = *value;
    if (const std::optional<std::string_view> value = document.string("window.title"))
        config.windowTitle = *value;
    if (const std::optional<std::string_view> value = document.string("network.server"))
        config.networkServer = *value;
    if (const std::optional<std::string_view> value = document.string("network.role"))
        config.serverRole = *value == "server";
    // Whole numbers in a sane range; anything else keeps the default.
    const auto count = [&document](const char* key, core::u32 lowest, core::u32 highest, core::u32& out) {
        if (const std::optional<f64> value = document.number(key);
            value.has_value() && *value >= static_cast<f64>(lowest) && *value <= static_cast<f64>(highest))
            out = static_cast<core::u32>(*value);
    };
    count("render.max_views_per_frame", 0, 64, config.maxViewsPerFrame);
    count("render.max_view_resolution", 16, 4096, config.maxViewResolution);
    count("render.max_sub_worlds", 0, 8, config.maxSubWorlds);
    count("render.max_highlights", 0, 255, config.maxHighlights);
    count("save.max_slots", 1, 4096, config.saveMaxSlots);
    count("network.timeout", 1, 120, config.networkTimeoutSeconds);
    count("script.max_memory_mb", 16, 65536, config.scriptMemoryMb);
    if (const std::optional<f64> value = document.number("render.foliage_density");
        value.has_value() && *value >= 0.0 && *value <= 1.0)
        config.foliageDensity = static_cast<core::f32>(*value);
    if (const std::optional<f64> value = document.number("render.foliage_shadow_distance");
        value.has_value() && *value >= 0.0 && *value <= 1000.0)
        config.foliageShadowDistance = static_cast<core::f32>(*value);
    if (const std::optional<f64> value = document.number("debug.frame_report_seconds");
        value.has_value() && *value >= 0.0 && *value <= 3600.0)
        config.frameReportSeconds = *value;
    if (const std::optional<std::string_view> value = document.string("debug.overlay_key"))
        config.overlayKey = std::string(*value);
    if (const std::optional<f64> value = document.number("scene.close_grace_seconds");
        value.has_value() && *value >= 0.0 && *value <= 60.0)
        config.sceneCloseGrace = *value;
    if (const std::optional<f64> value = document.number("save.max_slot_bytes");
        value.has_value() && *value >= 1024.0 && *value <= 1024.0 * 1024.0 * 1024.0)
        config.saveMaxSlotBytes = static_cast<core::u64>(*value);
    if (const std::optional<std::string_view> value = document.string("project.company"))
        config.company = *value;
    if (const std::optional<bool> value = document.boolean("display.vsync"))
        config.pacing.vsync = *value;
    if (const std::optional<f64> value = numberIn(document, "display.max_frame_rate", 0.0, 1000.0))
        config.pacing.maxFrameRate = static_cast<core::u32>(*value);
    if (const std::optional<f64> value = numberIn(document, "display.background_frame_rate", 0.0, 1000.0))
        config.pacing.backgroundFrameRate = static_cast<core::u32>(*value);
    if (const std::optional<bool> value = document.boolean("window.fullscreen"))
        config.fullscreen = *value;
    if (const std::optional<bool> value = document.boolean("window.resizable"))
        config.resizable = *value;
    // **Three whole numbers**, which is what every export can stamp: a file
    // version, an Android `versionName`, a `.desktop` entry.
    if (const std::optional<std::string_view> value = document.string("project.version")) {
        if (isVersion(*value)) {
            config.version = *value;
        }
        else if (diagnostic != nullptr) {
            *diagnostic = core::tr(ENG_TR("engine.editor.project_settings.version_not_semver"),
                                   {{"project", projectRoot.string()}, {"version", *value}});
        }
    }

    // A window from a pixel to sixteen thousand, checked before the cast.
    const auto side = [](f64 value) { return std::isfinite(value) && value >= 1.0 && value <= 16384.0; };
    const std::span<const f64> size = document.numbers("window.size");
    if (size.size() == 2 && side(size[0]) && side(size[1])) {
        config.windowWidth = static_cast<i32>(size[0]);
        config.windowHeight = static_cast<i32>(size[1]);
    }
    // **`width` and `height` are `size` too**, which ten examples wrote and
    // nothing read. `size` wins when a file says both.
    else if (const std::optional<f64> width = numberIn(document, "window.width", 1.0, 16384.0),
             height = numberIn(document, "window.height", 1.0, 16384.0);
             size.size() != 2 && width.has_value() && height.has_value()) {
        config.windowWidth = static_cast<i32>(*width);
        config.windowHeight = static_cast<i32>(*height);
    }

    // The preset the FILE names, so `quality = "low"` plus `bloom = true` is a
    // preset with one thing turned back on rather than a set of eleven numbers
    // the author has to remember.
    //
    // **A machine's kind decides where nobody did** (`defaultQuality`), and a
    // platform's own table decides over `[graphics]`: `[graphics.android]
    // quality = "low"` is "high everywhere, low on a phone".
    const std::string_view platform = platformTable(handheld);
    const std::string platformQuality = std::string(platform) + ".quality";
    render::QualityLevel level = render::defaultQuality(handheld);
    if (const std::optional<std::string_view> named = document.string("graphics.quality")) {
        if (const std::optional<render::QualityLevel> parsedLevel = render::parseQuality(*named))
            level = *parsedLevel;
    }
    if (const std::optional<std::string_view> named = document.string(platformQuality)) {
        if (const std::optional<render::QualityLevel> parsedLevel = render::parseQuality(*named))
            level = *parsedLevel;
    }
    // And the command line's preset wins over the file's, because it is the
    // outer layer: `--quality=low` on a project that asks for ultra is somebody
    // saying "not on this machine".
    config.graphics = presetFor(overrides.quality.value_or(level), handheld);

    // **And when it wins, it wins whole (D052).** A file's per-key graphics
    // entries are refinements OF the level it names -- "high, but the shadows
    // reach further, because this world's landmarks are far away". A player who
    // answers `--quality=low` is saying that level is not available on this
    // machine, so its refinements are not either: keeping them would hand a weak
    // machine the single heaviest dial in the file while everything around it
    // was turned down, which is the opposite of what was asked for. The
    // command line's own per-key flags still apply, because those were typed by
    // the same person as the preset.
    if (!overrides.quality) {
        applyFile(document, "graphics", config.graphics);
        applyFile(document, platform, config.graphics);
    }
    applyOverrides(overrides, config.graphics);
    config.graphics = render::clampSettings(config.graphics);

    // **The same, as the project's layer of the model** (ADR 0147): the level
    // the file names and each key it gives -- its refinements left out under a
    // command line's level, by the rule above -- and what `[display]` says.
    {
        GraphicsLayer& layer = config.graphicsModel.project;
        if (document.string("graphics.quality").has_value() || document.string(platformQuality).has_value())
            say(layer, GraphicsSetting::QualityLevel, static_cast<f64>(level));
        if (!overrides.quality) {
            sayTable(document, "graphics", false, layer);
            sayTable(document, platform, false, layer);
        }
        sayTable(document, "display", true, layer);
        // `[window] fullscreen` is the whole display as a window: borderless.
        if (config.fullscreen && !layer.says(GraphicsSetting::WindowMode))
            say(layer, GraphicsSetting::WindowMode, 1.0);
        const std::span<const f64> resolution = document.numbers("display.resolution");
        if (resolution.size() == 2 && side(resolution[0]) && side(resolution[1])) {
            say(layer, GraphicsSetting::ResolutionWidth, std::floor(resolution[0]));
            say(layer, GraphicsSetting::ResolutionHeight, std::floor(resolution[1]));
        }
        if (const std::optional<bool> value = document.boolean("display.remember_player_settings"))
            config.rememberPlayerSettings = *value;
        if (const std::optional<std::string_view> value = document.string("project.default_locale");
            value.has_value() && !scene::canonicalLocale(*value).empty())
            config.defaultLocale = scene::canonicalLocale(*value);
    }
    return config;
}

bool writeProjectSetting(const std::filesystem::path& projectRoot, std::string_view key, std::string_view rendered,
                         std::string* diagnostic)
{
    const auto fail = [&](std::string message) {
        if (diagnostic != nullptr)
            *diagnostic = std::move(message);
        return false;
    };

    const std::filesystem::path file = projectRoot / "project.toml";

    // **A project with no file yet gets one**, because the alternative is a
    // Settings dialog that works on some projects and silently does nothing on
    // the rest -- and "the rest" is every project before somebody first names
    // its window.
    std::string text;
    if (std::filesystem::exists(file) && !readFile(file, text))
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_read"), {{"path", file.string()}}));

    const std::optional<std::string> edited = core::setTomlValue(text, key, rendered);
    if (!edited.has_value())
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_place"),
                             {{"key", key}, {"path", file.string()}}));

    // **Parsed before it is written.** The edit is textual, so a value somebody
    // typed can produce a file the reader refuses -- and the failure mode of
    // writing it anyway is a project the engine will not open, from a dialog
    // whose whole job is to be safe to poke at.
    core::TomlDocument check;
    if (const core::TomlDocument::ParseResult result = check.parse(*edited, file.string()); !result.ok) {
        return fail(core::tr(ENG_TR("engine.editor.project_settings.would_be_unreadable"),
                             {{"file", file.filename().string()}, {"reason", result.diagnostic}}));
    }

    if (!platform::writeTextFile(file, *edited))
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_write"), {{"path", file.string()}}));
    return true;
}

bool removeProjectSetting(const std::filesystem::path& projectRoot, std::string_view key, std::string* diagnostic)
{
    const std::filesystem::path file = projectRoot / "project.toml";
    std::string text;
    // No file has no key: nothing to take out.
    if (!std::filesystem::exists(file))
        return true;
    const auto fail = [&](std::string message) {
        if (diagnostic != nullptr)
            *diagnostic = std::move(message);
        return false;
    };
    if (!readFile(file, text))
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_read"), {{"path", file.string()}}));
    const std::optional<std::string> edited = core::removeTomlValue(text, key);
    if (!edited.has_value())
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_place"),
                             {{"key", key}, {"path", file.string()}}));
    if (*edited == text)
        return true;
    if (!platform::writeTextFile(file, *edited))
        return fail(core::tr(ENG_TR("engine.editor.project_settings.could_not_write"), {{"path", file.string()}}));
    return true;
}

} // namespace engine::app
