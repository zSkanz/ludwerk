#include "engine/scene/graphics_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::scene {
namespace {

using core::f64;
using core::i32;
using core::usize;

// One row of the table below.
#define ENG_GRAPHICS_SETTING_INFO(Name, Kind, Lowest, Highest, Default, Quality, Applied)                              \
    GraphicsSettingInfo{#Name, GraphicsValueKind::Kind, Lowest, Highest, Default, Quality, Applied},

constexpr std::array<GraphicsSettingInfo, kGraphicsSettingCount> Settings{
    {ENG_GRAPHICS_SETTINGS(ENG_GRAPHICS_SETTING_INFO)}};

#undef ENG_GRAPHICS_SETTING_INFO

// `Enum.ShadowQuality`'s `Off`.
constexpr i32 ShadowsOff = 0;

using S = GraphicsSetting;

constexpr std::array ViewDistanceGroup{S::ViewDistance, S::TerrainDetail, S::LODBias, S::MaximumLODLevel};
constexpr std::array AntiAliasingGroup{S::AntiAliasing, S::Upscaling, S::Sharpness};
constexpr std::array PostProcessingGroup{S::Bloom, S::DepthOfField, S::SunRays, S::AutoExposure, S::MotionBlur};
constexpr std::array ShadowsGroup{S::ShadowQuality, S::ShadowResolution, S::ShadowCascades, S::ShadowDistance,
                                  S::ContactShadows};
constexpr std::array GlobalIlluminationGroup{S::AmbientOcclusion, S::GlobalIllumination};
constexpr std::array ReflectionsGroup{S::Reflections};
constexpr std::array TexturesGroup{S::TextureQuality, S::AnisotropicFiltering, S::TextureStreamingBudget};
constexpr std::array EffectsGroup{S::ParticleBudget, S::SoftParticles};
constexpr std::array FoliageGroup{S::FoliageDensity};
constexpr std::array ShadingGroup{S::LightBudget, S::SkinWeights, S::AnimationDetail, S::FogQuality};

[[nodiscard]] bool same(f64 a, f64 b) noexcept
{
    // Values are written as doubles and read back as the floats the renderer
    // keeps: equal means equal to a float's worth.
    return std::fabs(a - b) <= 1.0e-5 * std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
}

} // namespace

const GraphicsSettingInfo& graphicsSettingInfo(GraphicsSetting setting) noexcept
{
    return Settings[std::min(static_cast<usize>(setting), kGraphicsSettingCount - 1)];
}

std::optional<GraphicsSetting> graphicsSettingNamed(std::string_view name) noexcept
{
    for (usize index = 0; index < kGraphicsSettingCount; ++index) {
        if (Settings[index].name == name)
            return static_cast<GraphicsSetting>(index);
    }
    return std::nullopt;
}

std::span<const GraphicsSetting> graphicsGroupSettings(GraphicsGroup group) noexcept
{
    switch (group) {
    case GraphicsGroup::ViewDistance:
        return ViewDistanceGroup;
    case GraphicsGroup::AntiAliasing:
        return AntiAliasingGroup;
    case GraphicsGroup::PostProcessing:
        return PostProcessingGroup;
    case GraphicsGroup::Shadows:
        return ShadowsGroup;
    case GraphicsGroup::GlobalIllumination:
        return GlobalIlluminationGroup;
    case GraphicsGroup::Reflections:
        return ReflectionsGroup;
    case GraphicsGroup::Textures:
        return TexturesGroup;
    case GraphicsGroup::Effects:
        return EffectsGroup;
    case GraphicsGroup::Foliage:
        return FoliageGroup;
    case GraphicsGroup::Shading:
        return ShadingGroup;
    case GraphicsGroup::Count:
        break;
    }
    return {};
}

i32 GraphicsModel::preset() const noexcept
{
    // `QualityLevel` has no preset of its own to fall to: the layers, then the
    // engine's.
    f64 asked = static_cast<f64>(defaultLevel);
    for (const GraphicsLayer* layer : {&commandLine, &script, &player, &project}) {
        if (layer->says(S::QualityLevel)) {
            asked = layer->at(S::QualityLevel);
            break;
        }
    }
    const i32 level = static_cast<i32>(asked);
    if (level == kQualityAuto)
        return std::clamp(autoLevel, kQualityLow, kQualityUltra);
    return std::clamp(level, kQualityLow, kQualityUltra);
}

f64 GraphicsModel::presetValue(GraphicsSetting setting, i32 level) const noexcept
{
    const GraphicsLayer& values = presets[static_cast<usize>(std::clamp(level, kQualityLow, kQualityUltra))];
    return values.says(setting) ? values.at(setting) : graphicsSettingInfo(setting).engineDefault;
}

f64 GraphicsModel::effective(GraphicsSetting setting) const noexcept
{
    for (const GraphicsLayer* layer : {&commandLine, &script, &player, &project}) {
        if (layer->says(setting))
            return layer->at(setting);
    }
    if (setting == S::QualityLevel)
        return static_cast<f64>(defaultLevel);
    return presetValue(setting, preset());
}

GraphicsSource GraphicsModel::source(GraphicsSetting setting) const noexcept
{
    if (commandLine.says(setting))
        return GraphicsSource::CommandLine;
    if (script.says(setting))
        return GraphicsSource::Script;
    if (player.says(setting))
        return GraphicsSource::Player;
    if (project.says(setting))
        return GraphicsSource::Project;
    if (setting != S::QualityLevel && presets[static_cast<usize>(preset())].says(setting))
        return GraphicsSource::Preset;
    return GraphicsSource::Engine;
}

i32 GraphicsModel::qualityLevel() const noexcept
{
    const i32 level = preset();
    for (usize index = 0; index < kGraphicsSettingCount; ++index) {
        const auto setting = static_cast<GraphicsSetting>(index);
        if (Settings[index].quality && !same(effective(setting), presetValue(setting, level)))
            return kQualityCustom;
    }
    return static_cast<i32>(effective(S::QualityLevel)) == kQualityAuto ? kQualityAuto : level;
}

bool GraphicsModel::write(GraphicsSetting setting, f64 value) noexcept
{
    const GraphicsSettingInfo& info = graphicsSettingInfo(setting);
    if (!std::isfinite(value))
        return false;
    switch (info.kind) {
    case GraphicsValueKind::Flag:
        if (value != 0.0 && value != 1.0)
            return false;
        break;
    case GraphicsValueKind::Choice:
        if (value != std::floor(value) || value < info.lowest || value > info.highest)
            return false;
        break;
    case GraphicsValueKind::Whole:
        if (value != std::floor(value))
            return false;
        value = std::clamp(value, info.lowest, info.highest);
        break;
    case GraphicsValueKind::Number:
        value = std::clamp(value, info.lowest, info.highest);
        break;
    }

    if (setting == S::QualityLevel) {
        // `Custom` is what a level reads as, never one that is chosen.
        const i32 level = static_cast<i32>(value);
        if (level == kQualityCustom)
            return false;
        applyPreset(level);
        return true;
    }

    script.put(setting, value);
    // **A shadow quality is its resolution and its cascades**: choosing a
    // level writes both as that level has them, and `Off` is no cascades.
    if (setting == S::ShadowQuality) {
        const i32 level = static_cast<i32>(value);
        if (level == ShadowsOff) {
            script.put(S::ShadowCascades, 0.0);
        }
        else {
            script.put(S::ShadowResolution, presetValue(S::ShadowResolution, level - 1));
            script.put(S::ShadowCascades, presetValue(S::ShadowCascades, level - 1));
        }
    }
    return true;
}

void GraphicsModel::applyPreset(i32 level) noexcept
{
    if (level == kQualityCustom)
        return;
    script.put(S::QualityLevel, static_cast<f64>(level));
    const i32 chosen = preset();
    for (usize index = 0; index < kGraphicsSettingCount; ++index) {
        if (!Settings[index].quality)
            continue;
        const auto setting = static_cast<GraphicsSetting>(index);
        script.clear(setting);
        // Where the player's file or the project's still says otherwise, the
        // preset is said over it: "apply" means the level is what is drawn.
        const f64 wanted = presetValue(setting, chosen);
        if (!same(effective(setting), wanted))
            script.put(setting, wanted);
    }
}

void GraphicsModel::resetToDefaults() noexcept
{
    script = GraphicsLayer{};
    player = GraphicsLayer{};
    forgetPlayer = true;
}

void GraphicsModel::setGroupLevel(GraphicsGroup group, i32 level) noexcept
{
    // `Cinematic` is `Ultra` until something is finer.
    const i32 from = std::clamp(level, kQualityLow, kQualityUltra);
    for (const GraphicsSetting setting : graphicsGroupSettings(group))
        script.put(setting, presetValue(setting, from));
}

std::optional<i32> GraphicsModel::groupLevel(GraphicsGroup group) const noexcept
{
    const std::span<const GraphicsSetting> settings = graphicsGroupSettings(group);
    const auto at = [&](i32 level) {
        return std::all_of(settings.begin(), settings.end(), [&](GraphicsSetting setting) {
            return same(effective(setting), presetValue(setting, level));
        });
    };
    // The preset's own level first -- two levels can give a group the same
    // values, and then the one the player chose is the one it is at.
    if (at(preset()))
        return preset();
    for (i32 level = kQualityLow; level <= kQualityUltra; ++level) {
        if (at(level))
            return level;
    }
    return std::nullopt;
}

GraphicsLayer GraphicsModel::playerChoices() const noexcept
{
    GraphicsLayer choices = player;
    for (usize index = 0; index < kGraphicsSettingCount; ++index) {
        const auto setting = static_cast<GraphicsSetting>(index);
        if (script.says(setting))
            choices.put(setting, script.at(setting));
    }
    return choices;
}

void GraphicsModel::takeHostLayers(const GraphicsModel& host) noexcept
{
    commandLine = host.commandLine;
    project = host.project;
    player = host.player;
    presets = host.presets;
    autoLevel = host.autoLevel;
    defaultLevel = host.defaultLevel;
}

core::u64 GraphicsModel::revision() const noexcept
{
    core::u64 hash = 1469598103934665603ull;
    for (usize index = 0; index < kGraphicsSettingCount; ++index) {
        const f64 value = effective(static_cast<GraphicsSetting>(index));
        core::u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ull;
    }
    return hash;
}

} // namespace engine::scene
