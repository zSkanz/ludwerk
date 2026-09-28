#include "engine/app/project_config.h"

#include <fstream>
#include <span>
#include <sstream>

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
void applyFile(const core::TomlDocument& document, GraphicsSettings& settings)
{
    if (const std::optional<f64> value = document.number("graphics.render_scale"))
        settings.renderScale = static_cast<f32>(*value);
    if (const std::optional<f64> value = document.number("graphics.shadow_resolution"))
        settings.shadowTileResolution = static_cast<u32>(*value);
    if (const std::optional<f64> value = document.number("graphics.shadow_cascades"))
        settings.shadowCascades = static_cast<u32>(*value);
    if (const std::optional<f64> value = document.number("graphics.shadow_distance"))
        settings.shadowDistance = static_cast<f32>(*value);
    if (const std::optional<f64> value = document.number("graphics.light_budget"))
        settings.lightBudget = static_cast<u32>(*value);
    if (const std::optional<bool> value = document.boolean("graphics.bloom"))
        settings.bloom = *value;
    if (const std::optional<bool> value = document.boolean("graphics.ambient_occlusion"))
        settings.ambientOcclusion = *value;
    if (const std::optional<bool> value = document.boolean("graphics.anti_aliasing"))
        settings.antiAliasing = *value;
    if (const std::optional<bool> value = document.boolean("graphics.auto_exposure"))
        settings.autoExposure = *value;
    if (const std::optional<bool> value = document.boolean("graphics.depth_of_field"))
        settings.depthOfField = *value;
    if (const std::optional<bool> value = document.boolean("graphics.sun_rays"))
        settings.sunRays = *value;
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
    if (overrides.forcedSurface)
        settings.forcedSurface = *overrides.forcedSurface;
}

} // namespace

render::GraphicsSettings resolveGraphics(const GraphicsOverrides& overrides)
{
    GraphicsSettings settings = render::settingsFor(overrides.quality.value_or(render::QualityLevel::High));
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

ProjectConfig loadProjectConfig(const std::filesystem::path& projectRoot, const GraphicsOverrides& overrides,
                                std::string* diagnostic)
{
    ProjectConfig config;

    std::string text;
    if (projectRoot.empty() || !readFile(projectRoot / "project.toml", text)) {
        config.graphics = resolveGraphics(overrides);
        return config;
    }

    core::TomlDocument document;
    const core::TomlDocument::ParseResult parsed = document.parse(text, (projectRoot / "project.toml").string());
    if (!parsed) {
        if (diagnostic != nullptr)
            *diagnostic = parsed.diagnostic;
        config.graphics = resolveGraphics(overrides);
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
    if (const std::optional<f64> value = document.number("scene.close_grace_seconds");
        value.has_value() && *value >= 0.0 && *value <= 60.0)
        config.sceneCloseGrace = *value;
    if (const std::optional<f64> value = document.number("save.max_slot_bytes");
        value.has_value() && *value >= 1024.0 && *value <= 1024.0 * 1024.0 * 1024.0)
        config.saveMaxSlotBytes = static_cast<core::u64>(*value);
    if (const std::optional<std::string_view> value = document.string("project.company"))
        config.company = *value;
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
            *diagnostic = std::string(projectRoot.string()) + "/project.toml: [project] version \"" +
                          std::string(*value) + "\" is not X.Y.Z";
        }
    }

    const std::span<const f64> size = document.numbers("window.size");
    if (size.size() == 2) {
        config.windowWidth = static_cast<i32>(size[0]);
        config.windowHeight = static_cast<i32>(size[1]);
    }
    // **`width` and `height` are `size` too**, which ten examples wrote and
    // nothing read. `size` wins when a file says both.
    else if (const std::optional<f64> width = document.number("window.width"),
             height = document.number("window.height");
             width.has_value() && height.has_value()) {
        config.windowWidth = static_cast<i32>(*width);
        config.windowHeight = static_cast<i32>(*height);
    }

    // The preset the FILE names, so `quality = "low"` plus `bloom = true` is a
    // preset with one thing turned back on rather than a set of eleven numbers
    // the author has to remember.
    render::QualityLevel level = render::QualityLevel::High;
    if (const std::optional<std::string_view> named = document.string("graphics.quality")) {
        if (const std::optional<render::QualityLevel> parsedLevel = render::parseQuality(*named))
            level = *parsedLevel;
    }
    // And the command line's preset wins over the file's, because it is the
    // outer layer: `--quality=low` on a project that asks for ultra is somebody
    // saying "not on this machine".
    config.graphics = render::settingsFor(overrides.quality.value_or(level));

    // **And when it wins, it wins whole (D052).** A file's per-key graphics
    // entries are refinements OF the level it names -- "high, but the shadows
    // reach further, because this world's landmarks are far away". A player who
    // answers `--quality=low` is saying that level is not available on this
    // machine, so its refinements are not either: keeping them would hand a weak
    // machine the single heaviest dial in the file while everything around it
    // was turned down, which is the opposite of what was asked for. The
    // command line's own per-key flags still apply, because those were typed by
    // the same person as the preset.
    if (!overrides.quality)
        applyFile(document, config.graphics);
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
        return fail("could not read " + file.string());

    const std::optional<std::string> edited = core::setTomlValue(text, key, rendered);
    if (!edited.has_value())
        return fail("could not place " + std::string(key) + " in " + file.string());

    // **Parsed before it is written.** The edit is textual, so a value somebody
    // typed can produce a file the reader refuses -- and the failure mode of
    // writing it anyway is a project the engine will not open, from a dialog
    // whose whole job is to be safe to poke at.
    core::TomlDocument check;
    if (const core::TomlDocument::ParseResult result = check.parse(*edited, file.string()); !result.ok) {
        return fail("that would leave " + file.filename().string() + " unreadable: " + result.diagnostic);
    }

    if (!platform::writeTextFile(file, *edited))
        return fail("could not write " + file.string());
    return true;
}

} // namespace engine::app
