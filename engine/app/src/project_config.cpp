#include "engine/app/project_config.h"

#include <cmath>
#include <fstream>
#include <span>
#include <sstream>

#include "engine/core/i18n.h"
#include "engine/core/toml.h"
#include "engine/core/toml_edit.h"
#include "engine/platform/file.h"

namespace engine::app {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
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

} // namespace

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

} // namespace engine::app
