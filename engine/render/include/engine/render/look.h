// The look of a world, resolved (ADR 0096).
//
// Atmosphere, a sky and the post effects are INSTANCES, and where one sits says
// whose it is: directly under `Lighting` it is the world's, directly under the
// current camera it is its viewer's, and anywhere else it does nothing. Several
// of one kind combine by rules the ADR writes down. Every one of those rules is
// applied here and nowhere else, into one POD block the renderer reads -- and the
// editor asks the same function why an instance does not count, so the marker
// it shows and the picture the renderer draws cannot disagree.
//
// **A world with none of these instances resolves to `RenderLook{}`**, and
// `RenderLook{}` is the picture the engine drew before any of them existed: the
// engine's own bloom, the linear fog, the analytic sky and nothing else. That
// is asserted rather than hoped (`look_tests.cpp`), because every golden in the
// repository rests on it.
#pragma once

#include <array>
#include <memory>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/rhi/types.h"

namespace engine::scene {
class World;
}

namespace engine::render {

struct SkyRadiance;

// `Atmosphere`, as drawn. Meaningful only when `present`.
struct RenderAtmosphere
{
    bool present = false;
    core::f32 density = 0.35f;
    // Metres.
    core::f32 offset = 0.0f;
    core::Color3 color{196.0f / 255.0f, 210.0f / 255.0f, 230.0f / 255.0f};
    core::f32 decay = 0.1f;
    core::f32 glare = 0.0f;
    core::f32 haze = 0.0f;

    [[nodiscard]] bool operator==(const RenderAtmosphere&) const noexcept = default;
};

// The six faces of a skybox, in the order the properties are declared.
enum class SkyFace : core::u8
{
    Back,
    Down,
    Front,
    Left,
    Right,
    Up,
};
inline constexpr core::u32 kSkyFaceCount = 6;

// `Sky`, as drawn. Meaningful only when `present`; a sky whose six faces are all
// empty still governs the sun, the moon, the stars and the clouds over the
// engine's own gradient.
struct RenderSky
{
    bool present = false;
    core::NameAtom faces[kSkyFaceCount]{};
    // Degrees about X, Y and Z.
    core::Vec3 orientation{0.0f, 0.0f, 0.0f};
    core::NameAtom sunTexture;
    core::NameAtom moonTexture;
    // Degrees across.
    core::f32 sunAngularSize = 2.3f;
    core::f32 moonAngularSize = 2.0f;
    core::u32 starCount = 3000;
    bool celestialBodiesShown = true;
    core::f32 cloudCover = 0.0f;
    core::f32 cloudDensity = 0.5f;
    core::Color3 cloudColor{1.0f, 1.0f, 1.0f};

    // **What is drawn, handed over after the extract**: the six pictures
    // resampled (`SkyLoader::append`), and the sun's and moon's pictures from
    // the texture library. Invalid for none, or for a picture still loading --
    // which draws the plain disc, or the previous sky, meanwhile.
    rhi::TextureHandle image;
    std::shared_ptr<const SkyRadiance> radiance;
    rhi::TextureHandle sunImage;
    rhi::TextureHandle moonImage;

    // Whether any face names an image. None is the engine's own sky.
    [[nodiscard]] bool hasImages() const noexcept
    {
        for (const core::NameAtom face : faces) {
            if (face.valid())
                return true;
        }
        return false;
    }
    [[nodiscard]] bool operator==(const RenderSky&) const noexcept = default;
};

// One `ColorCorrectionEffect`, as the tonemap applies it to exposed linear
// light (`tonemap_graded.hlsl`, and `applyGrade` below, which is the same
// arithmetic for a test to hold):
//   1. `mix`: its tint, then its saturation -- three rows of (r, g, b);
//   2. nothing below zero;
//   3. `power`: its contrast, as `pivot * (c / pivot) ^ power`, the pivot being
//      the brightness the exposure puts the frame's average at -- so what is
//      at the pivot stays, what is brighter goes further and what is darker
//      goes darker without ever reaching black, each channel keeping its sign
//      and so its hue. One is no contrast, and is skipped exactly;
//   4. `lift`: its brightness, added; nothing below zero.
struct GradeStage
{
    core::f32 mix[3][3]{
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    core::f32 power = 1.0f;
    core::f32 lift = 0.0f;

    [[nodiscard]] bool operator==(const GradeStage&) const noexcept = default;
};

// How many stages one frame grades by.
inline constexpr core::usize MaxGradeStages = 8;
// Where contrast turns: the exposure's key, the frame's own middle.
inline constexpr core::f32 GradePivot = 0.45f;

// **The automatic exposure's limits** (`Lighting.ExposureMin`/`ExposureMax`,
// D460), as what the meter is held to: the lowest and the highest average
// luminance it accepts. An exposure of E stops is the key over that average,
// so the most it may BRIGHTEN is the lowest average, and the most it may
// darken the highest; whichever way round the two were written, the smaller
// is the floor.
//
// The defaults are the limits the engine had before a world could say --
// 0.15 and 3 -- and answer exactly those, so no picture moved when the
// properties arrived.
inline constexpr core::f32 DefaultExposureMin = -2.7369655f;
inline constexpr core::f32 DefaultExposureMax = 1.5849625f;
struct ExposureRange
{
    core::f32 lowest = 0.15f;
    core::f32 highest = 3.0f;
};
[[nodiscard]] ExposureRange exposureRange(core::f32 exposureMin, core::f32 exposureMax) noexcept;

struct RenderLook
{
    // --- Bloom ---------------------------------------------------------------
    //
    // **Governed only when a `BloomEffect` counts.** With none, the engine's own
    // bloom applies exactly as it always has -- every scene and golden in the
    // repository has bloom and none has the instance. With one that counts, the
    // first ENABLED one supplies the numbers; with only disabled ones, bloom is
    // off, which is what a 2D game whose white sprites should not glow wants.
    bool bloomGoverned = false;
    bool bloomEnabled = true;
    // A multiple of the engine's own strength.
    core::f32 bloomIntensity = 1.0f;
    // 24 is the engine's own reach.
    core::f32 bloomSize = 24.0f;
    core::f32 bloomThreshold = 1.1f;

    // --- Colour correction ----------------------------------------------------
    //
    // Every enabled `ColorCorrectionEffect` that changes anything, in order,
    // each as one stage (`GradeStage`): the tonemap applies them one after
    // another to the exposed light. `graded` false is the identity, and the
    // plain tonemap: an effect at its defaults is no stage at all.
    //
    // **They were one affine map** -- every operation was, contrast included,
    // as a line through the pivot -- and that line took the dark half of a
    // picture below zero: at +0.12 every channel under about sRGB 62 was black,
    // so grass went neon and a navy sky went black (D427). Contrast is a power
    // about the pivot now, which never leaves the positive, and a power does
    // not fold into a sum: hence stages. `MaxGradeStages` at once; a ninth
    // effect that changes something is left out.
    bool graded = false;
    std::array<GradeStage, MaxGradeStages> grades{};
    core::u32 gradeCount = 0;

    // --- Blur -------------------------------------------------------------------
    //
    // Pixels of a 1080-line picture, every enabled `BlurEffect` combined by
    // their squares. Zero is no blur.
    core::f32 blurSize = 0.0f;

    // --- Depth of field ----------------------------------------------------------
    bool depthOfField = false;
    core::f32 focusDistance = 25.0f;
    core::f32 inFocusRadius = 10.0f;
    core::f32 nearIntensity = 0.5f;
    core::f32 farIntensity = 0.5f;

    // --- Sun rays -------------------------------------------------------------------
    bool sunRays = false;
    core::f32 sunRaysIntensity = 0.25f;
    core::f32 sunRaysSpread = 0.5f;

    RenderAtmosphere atmosphere;
    RenderSky sky;

    [[nodiscard]] bool operator==(const RenderLook&) const noexcept = default;
};

// **The air as numbers** (`Atmosphere`, ADR 0096): what `look_air.hlsl`
// integrates along every ray, derived from the instance's properties once and
// here, so the pass, the linear fog the blended surfaces approximate it with,
// and the tests that pin the documented distances all read one answer.
struct AirMedium
{
    // How much of the light it crosses the air hides per metre, at the height
    // of `Offset`: `Density` squared, times a fiftieth -- so the default hides
    // half of what stands about 280 metres away, and a density of 1 half of
    // what stands 35 metres away.
    core::f32 extinction = 0.0f;
    // How fast that falls per metre of height: `Decay` of 1 halves it every ten
    // metres, 0.1 every hundred, 0 never.
    core::f32 falloff = 0.0f;
    // The camera's height above `Offset`, in metres.
    core::f32 height = 0.0f;
    // How much thicker the air grows towards the horizon, as a multiple of
    // `extinction`.
    core::f32 haze = 0.0f;
};
[[nodiscard]] AirMedium airMediumOf(const RenderAtmosphere& atmosphere, core::f64 cameraHeight) noexcept;

// How much air a ray crosses from the camera: `reach` metres along a direction
// whose vertical component is `rise`. The CPU half of `airOpticalDepth` in
// `engine_look.hlsli`; what survives is `exp(-depth)`.
[[nodiscard]] core::f32 airOpticalDepth(const AirMedium& air, core::f32 rise, core::f32 reach) noexcept;

// Why an instance of one of these classes changes nothing, or `Counts` when it
// does. The editor turns each into a sentence (R3); the renderer never needs to.
enum class LookStanding : core::u8
{
    // It takes part in the picture.
    Counts,
    // Not one of the classes this file is about.
    NotALook,
    // An effect whose `Enabled` is off. Not a mistake -- for a bloom it is how
    // bloom is turned off -- but the editor still says so.
    Disabled,
    // Not directly under `Lighting` or the current camera; or, for an
    // `Atmosphere` or a `Sky`, not directly under `Lighting`.
    WrongParent,
    // A second `Atmosphere` or `Sky` under `Lighting`: the first one is used.
    NotFirst,
    // A bloom, depth of field or sun rays that an earlier enabled one of its
    // kind outranks.
    Outranked,
};

// **How tight the lobe of sky round the sun that can shine is**, by the
// effect's `Spread`: the mask weights the sky by `exp(-d * d * lobe)`, `d` the
// distance from the sun in screen heights. 24 up to the default spread of a
// half -- the look it always had -- and down to 1.5 at 1, a lobe four times as
// wide: with the disc itself behind one tree's crown the tight lobe held
// nothing that could shine, and the rays went out.
[[nodiscard]] constexpr core::f32 raysLobe(core::f32 spread) noexcept
{
    const core::f32 past = spread > 0.5f ? spread - 0.5f : 0.0f;
    const core::f32 widen = 1.0f + 6.0f * past;
    return 24.0f / (widen * widen);
}

// What the graded tonemap makes of one exposed linear colour, before the tone
// curve: every stage of `look`, in order. The shader's arithmetic, on the CPU.
[[nodiscard]] core::Color3 applyGrade(const RenderLook& look, core::Color3 exposed) noexcept;

// Resolves every instance under `lightingHost` and under `camera` into `out`,
// in document order -- `Lighting`'s children first, then the camera's. Either id
// may be invalid, which contributes nothing.
void resolveLook(const scene::World& world, core::InstanceId lightingHost, core::InstanceId camera, RenderLook& out);

// Whether `id` takes part, by exactly the rules `resolveLook` applies.
[[nodiscard]] LookStanding lookStanding(const scene::World& world, core::InstanceId id, core::InstanceId lightingHost,
                                        core::InstanceId camera);

} // namespace engine::render
