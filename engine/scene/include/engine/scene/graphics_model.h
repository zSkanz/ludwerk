// Graphics and display settings as one model (ADR 0147).
//
// A setting's value is the first of these that says it: the command line, what
// a script wrote this session, the player's saved choice, the project's file,
// the preset those chose, and the engine's own default. The model is that
// stack and nothing else: plain numbers, a layer each, and the rule for which
// one is read. It draws nothing and opens no window -- the host reads the
// effective values once a frame and applies what changed.
//
// **Here rather than beside the renderer** because three things write it and
// only one of them is the host: `GraphicsService` (a script), the player's
// file, and the project's. A script reaches a world, and the world is `scene`.
//
// **Nothing here reaches the simulation.** It is not hashed, a trace recorded
// at one quality replays at another, and a world restored to an earlier tick
// keeps the settings a script has written since only because the host puts
// its own layers back every frame -- which is also what makes the editor's
// Stop undo what a game's options menu did during Play.
#pragma once

#include <array>
#include <bitset>
#include <optional>
#include <span>
#include <string_view>

#include "engine/core/types.h"

namespace engine::scene {

// Every setting, in the order `GraphicsService` lists them.
//
//   X(Name, kind, lowest, highest, engine default, quality, applied)
//
// `quality` is whether the setting is part of the quality level: writing one
// makes `QualityLevel` read `Custom`. `applied` is whether anything draws by it
// yet -- the rest are kept, saved and reported, and read when their feature
// lands, so a game's menu does not change shape (ADR 0147 section 2).
#define ENG_GRAPHICS_SETTINGS(X)                                                                                       \
    X(QualityLevel, Choice, 0.0, 5.0, 2.0, false, true)                                                                \
    X(RenderScale, Number, 1.0 / 3.0, 1.0, 1.0, true, true)                                                            \
    X(ShadowQuality, Choice, 0.0, 4.0, 3.0, true, true)                                                                \
    X(ShadowResolution, Whole, 256.0, 2048.0, 2048.0, true, true)                                                      \
    X(ShadowCascades, Whole, 0.0, 4.0, 4.0, true, true)                                                                \
    X(ShadowDistance, Number, 10.0, 1000.0, 120.0, true, true)                                                         \
    X(AntiAliasing, Choice, 0.0, 3.0, 2.0, true, true)                                                                 \
    X(Upscaling, Choice, 0.0, 2.0, 0.0, true, true)                                                                    \
    X(Sharpness, Number, 0.0, 1.0, 0.2, true, true)                                                                    \
    X(AmbientOcclusion, Flag, 0.0, 1.0, 1.0, true, true)                                                               \
    X(ContactShadows, Flag, 0.0, 1.0, 1.0, true, true)                                                                 \
    X(Bloom, Flag, 0.0, 1.0, 1.0, true, true)                                                                          \
    X(DepthOfField, Flag, 0.0, 1.0, 1.0, true, true)                                                                   \
    X(SunRays, Flag, 0.0, 1.0, 1.0, true, true)                                                                        \
    X(AutoExposure, Flag, 0.0, 1.0, 1.0, true, true)                                                                   \
    X(LightBudget, Whole, 0.0, 65536.0, 256.0, true, true)                                                             \
    X(TerrainDetail, Number, 0.25, 4.0, 1.0, true, true)                                                               \
    X(FoliageDensity, Number, 0.0, 1.0, 1.0, true, true)                                                               \
    X(ViewDistance, Number, 0.25, 4.0, 1.0, true, false)                                                               \
    X(TextureQuality, Choice, 0.0, 2.0, 2.0, true, false)                                                              \
    X(AnisotropicFiltering, Whole, 1.0, 16.0, 8.0, true, false)                                                        \
    X(LODBias, Number, 0.25, 4.0, 1.0, true, false)                                                                    \
    X(MaximumLODLevel, Whole, 0.0, 8.0, 0.0, true, false)                                                              \
    X(ParticleBudget, Whole, 0.0, 1048576.0, 65536.0, true, false)                                                     \
    X(SoftParticles, Flag, 0.0, 1.0, 1.0, true, false)                                                                 \
    X(SkinWeights, Whole, 1.0, 4.0, 4.0, true, false)                                                                  \
    X(TextureStreamingBudget, Whole, 0.0, 65536.0, 0.0, true, false)                                                   \
    X(AsyncUploadBudget, Number, 0.0, 33.0, 2.0, true, false)                                                          \
    X(MotionBlur, Flag, 0.0, 1.0, 0.0, true, false)                                                                    \
    X(FogQuality, Choice, 0.0, 4.0, 2.0, true, false)                                                                  \
    X(GlobalIllumination, Choice, 0.0, 4.0, 2.0, true, false)                                                          \
    X(Reflections, Choice, 0.0, 4.0, 2.0, true, false)                                                                 \
    X(RenderResolutionCap, Whole, 0.0, 4320.0, 0.0, true, true)                                                        \
    X(WindowMode, Choice, 0.0, 2.0, 0.0, false, true)                                                                  \
    X(ResolutionWidth, Whole, 0.0, 16384.0, 0.0, false, true)                                                          \
    X(ResolutionHeight, Whole, 0.0, 16384.0, 0.0, false, true)                                                         \
    X(Monitor, Whole, 0.0, 15.0, 0.0, false, true)                                                                     \
    X(VSync, Flag, 0.0, 1.0, 1.0, false, true)                                                                         \
    X(MaxFrameRate, Whole, 0.0, 1000.0, 0.0, false, true)                                                              \
    X(BackgroundFrameRate, Whole, 0.0, 1000.0, 10.0, false, true)                                                      \
    X(FrameGeneration, Flag, 0.0, 1.0, 0.0, false, true)                                                               \
    X(Brightness, Number, -1.0, 1.0, 0.0, false, false)

enum class GraphicsSetting : core::u8
{
#define ENG_GRAPHICS_SETTING_ENUM(Name, ...) Name,
    ENG_GRAPHICS_SETTINGS(ENG_GRAPHICS_SETTING_ENUM)
#undef ENG_GRAPHICS_SETTING_ENUM
        Count,
};

inline constexpr core::usize kGraphicsSettingCount = static_cast<core::usize>(GraphicsSetting::Count);

// What a setting's value is: a switch, a number, a whole number, or one of an
// enum's items by its value.
enum class GraphicsValueKind : core::u8
{
    Flag,
    Number,
    Whole,
    Choice,
};

struct GraphicsSettingInfo
{
    std::string_view name;
    GraphicsValueKind kind = GraphicsValueKind::Number;
    core::f64 lowest = 0.0;
    core::f64 highest = 0.0;
    core::f64 engineDefault = 0.0;
    bool quality = false;
    bool applied = false;
};

[[nodiscard]] const GraphicsSettingInfo& graphicsSettingInfo(GraphicsSetting setting) noexcept;
[[nodiscard]] std::optional<GraphicsSetting> graphicsSettingNamed(std::string_view name) noexcept;

// Who said a value: `Enum.SettingSource`, in its order.
enum class GraphicsSource : core::u8
{
    CommandLine,
    Player,
    Script,
    Project,
    Preset,
    Engine,
};

// `Enum.GraphicsQuality`: the four presets, and the two that are not one.
inline constexpr core::i32 kQualityLow = 0;
inline constexpr core::i32 kQualityMedium = 1;
inline constexpr core::i32 kQualityHigh = 2;
inline constexpr core::i32 kQualityUltra = 3;
inline constexpr core::i32 kQualityCustom = 4;
inline constexpr core::i32 kQualityAuto = 5;
inline constexpr core::usize kQualityPresets = 4;

// `Enum.GraphicsGroup`: the coarse levels a player's menu shows.
enum class GraphicsGroup : core::u8
{
    ViewDistance,
    AntiAliasing,
    PostProcessing,
    Shadows,
    GlobalIllumination,
    Reflections,
    Textures,
    Effects,
    Foliage,
    Shading,
    Count,
};

// `Enum.GraphicsLevel`: `Cinematic` is `Ultra` until something is finer.
inline constexpr core::i32 kLevelCinematic = 4;
inline constexpr core::usize kGraphicsLevels = 5;

// One source's word: the settings it says, and what it says for each.
struct GraphicsLayer
{
    std::array<core::f64, kGraphicsSettingCount> value{};
    std::bitset<kGraphicsSettingCount> said;

    void put(GraphicsSetting setting, core::f64 number) noexcept
    {
        value[static_cast<core::usize>(setting)] = number;
        said.set(static_cast<core::usize>(setting));
    }
    void clear(GraphicsSetting setting) noexcept { said.reset(static_cast<core::usize>(setting)); }
    [[nodiscard]] bool says(GraphicsSetting setting) const noexcept
    {
        return said.test(static_cast<core::usize>(setting));
    }
    [[nodiscard]] core::f64 at(GraphicsSetting setting) const noexcept
    {
        return value[static_cast<core::usize>(setting)];
    }
};

class GraphicsModel
{
public:
    // The command line's, the project file's and the player's saved layers,
    // and what each preset gives every quality setting: the host's, put back
    // every frame.
    GraphicsLayer commandLine;
    GraphicsLayer project;
    GraphicsLayer player;
    std::array<GraphicsLayer, kQualityPresets> presets;
    // What a script wrote this session.
    GraphicsLayer script;
    // The preset `Auto` stands for on this machine, until it is measured.
    core::i32 autoLevel = kQualityHigh;
    // The level a machine starts at when nobody named one: `High` on a
    // desktop, a step lower in the hand.
    core::i32 defaultLevel = kQualityHigh;
    // A script asked for the player's choices to be written, read again, or
    // (`ResetToDefaults`) forgotten: the host's to do, at the frame's end.
    bool saveRequested = false;
    bool loadRequested = false;
    bool forgetPlayer = false;

    // The value in force, and who said it.
    [[nodiscard]] core::f64 effective(GraphicsSetting setting) const noexcept;
    [[nodiscard]] GraphicsSource source(GraphicsSetting setting) const noexcept;

    // **The preset in force**: `QualityLevel` as it was last said, with `Auto`
    // read as the level this machine was given. Never `Custom`.
    [[nodiscard]] core::i32 preset() const noexcept;
    // `QualityLevel` as it reads: the preset, `Auto` when that is what was
    // asked for, or `Custom` when any quality setting is not the preset's.
    [[nodiscard]] core::i32 qualityLevel() const noexcept;
    // What `preset` gives a setting: the preset's word, or the engine's.
    [[nodiscard]] core::f64 presetValue(GraphicsSetting setting, core::i32 level) const noexcept;

    // A script's write. The value is checked against the setting's kind and
    // clamped into its range; false is a value that is not that kind at all
    // (a fraction for a whole number, not a number). A setting the command
    // line pinned is written and does not take effect.
    [[nodiscard]] bool write(GraphicsSetting setting, core::f64 value) noexcept;
    // `ApplyPreset`: the level, and every quality setting as that level has it
    // -- whatever a script, the player or the project said before.
    void applyPreset(core::i32 level) noexcept;
    // `ResetToDefaults`: what a script and the player said are forgotten, and
    // the project's defaults stand.
    void resetToDefaults() noexcept;

    // One of the coarse groups, at a level: its settings as the preset of that
    // level has them. And the level a group is at now, when its settings are
    // exactly one level's -- the lowest that matches.
    void setGroupLevel(GraphicsGroup group, core::i32 level) noexcept;
    [[nodiscard]] std::optional<core::i32> groupLevel(GraphicsGroup group) const noexcept;

    // What the player's file holds after a save: their saved choices under
    // what a script has written since.
    [[nodiscard]] GraphicsLayer playerChoices() const noexcept;

    // The host's layers from `host` -- the player's saved choices among them
    // -- leaving what a script wrote: once a frame, so a restored world does
    // not bring back an old command line.
    void takeHostLayers(const GraphicsModel& host) noexcept;

    // Every effective value, hashed: what the host compares to know whether
    // anything is to be applied.
    [[nodiscard]] core::u64 revision() const noexcept;
};

// The settings a group sets, for `setGroupLevel` and for a menu that shows
// which they are.
[[nodiscard]] std::span<const GraphicsSetting> graphicsGroupSettings(GraphicsGroup group) noexcept;

} // namespace engine::scene
