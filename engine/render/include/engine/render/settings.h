// The graphics settings family (roadmap M8; ADR 0038 §3, ADR 0044).
//
// Every number in here was `constexpr` until M8. They are **engine** settings
// and not `Lighting` properties, and the distinction is the whole design:
// `Lighting` describes the world and travels with the scene, while these
// describe the machine the scene is being shown on. A game that shipped a
// 4096-texel shadow map as scene state would be deciding how a stranger's
// laptop spends its frame.
//
// Three sources, each overriding the one before: a quality preset, the
// project's `[graphics]` table, and the host's own flags -- and, since ADR
// 0147, a script's write and the player's saved choice between the last two.
// Those two are not this module's: `scene::GraphicsModel` holds every layer,
// the host resolves it, and what arrives here is still one `GraphicsSettings`.
//
// **Nothing here reaches the simulation.** A world hashes identically at every
// quality level, which is asserted rather than asserted-in-prose: the M8 gate
// runs the determinism replay twice at two settings.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/types.h"

namespace engine::render {

using core::f32;
using core::u32;

enum class QualityLevel : core::u8
{
    Low,
    Medium,
    High,
    Ultra,
};

// **How edges are smoothed** (ADR 0158), as `Enum.AntiAliasingMode` numbers
// them. FXAA and SMAA read the finished picture; TAA blends each frame into
// the ones before it, drawn a fraction of a pixel apart.
enum class AntiAliasingMode : core::u8
{
    Off,
    Fxaa,
    Smaa,
    Taa,
};

// **How a world drawn below the output's resolution is brought up to it**
// (ADR 0158), as `Enum.UpscalingMode` numbers them: filtered, by FSR 1, which
// reads the one finished picture, or by FSR 2 (ADR 0164), which builds the
// output's every pixel from the frames before it and is the anti-aliasing as
// well -- at a render scale of 1 that is all it is.
enum class UpscalingMode : core::u8
{
    None,
    Fsr1,
    Fsr2,
};

// **What the renderer draws in place of the picture** (terrain audit T0): a
// test instrument, like `forcedSurface`, and never a setting a player has.
//   Holes      the sky magenta and every terrain flat white, nothing else lit:
//              `imgholes` counts sky seen through the ground.
//   Level      each terrain node in the colour of its level of detail.
//   Sky        the terrain's baked sky term, black to white.
//   Shadow     the sun's shadow on terrain: the map in red, the contact mask
//              in green, blue where the ground faces the sun, and the sky
//              black. `imgshadow` counts faces to the sun either one darkens.
//   Occlusion  the screen-space occlusion on terrain.
//   Bend       how far the normal the terrain is shaded with is bent from
//              its mesh's -- the grain's nudge and the layers' normal maps --
//              four times over, and the sky black: `imgsteps` counts where it
//              steps between two pixels, a crease the ground does not have.
//   Albedo     the colour the terrain is lit as, before any light, and the
//              sky black: plain ground is one grey, and `imgsteps` measures
//              how far its colours spread.
//   Material   the layer each pixel is drawn as, a colour a layer, and the
//              sky black: the corner that weighs most, then the paint, then
//              the rules, each from half -- what the CPU says is there
//              (`asset::drawnMaterial`).
//   Motion     how far each pixel moved since the last frame, over the
//              finished picture (D548): red and green the motion along x and
//              y, mid grey for none and a channel's whole range for sixteen
//              pixels either way, and blue where it moved at all. What the
//              temporal pass and frame generation are told, seen.
enum class DebugView : core::u8
{
    None,
    Holes,
    Level,
    Sky,
    Shadow,
    Occlusion,
    Bend,
    Albedo,
    Material,
    Motion,
};

// Whether a debug view draws the sky black, for a check to pass it over.
[[nodiscard]] constexpr bool blackSky(DebugView view) noexcept
{
    return view == DebugView::Shadow || view == DebugView::Bend || view == DebugView::Albedo ||
           view == DebugView::Material;
}

struct GraphicsSettings
{
    // Which preset these started from. Carried so the overlay and the log can
    // say "High, with two overrides" rather than reciting eleven numbers.
    QualityLevel quality = QualityLevel::High;

    // The fraction of the output resolution the world is rendered at. The post
    // chain and the UI are unaffected: the final resolve upscales the world
    // image into the target, and the 2D pass draws at full resolution on top of
    // it, which is the whole reason a render scale is worth having at all.
    f32 renderScale = 1.0f;

    // **The most pixels the world's shorter side is rendered at**, or zero for
    // no limit (the mobile ledger). A phone's display is 1440 pixels tall at
    // five hundred to the inch: a world rendered at every one of them is four
    // and a half million pixels through every per-pixel pass, for detail no
    // eye resolves at that density -- and the heat of it halves the frame
    // rate inside a minute. The world is rendered at this height and resolved
    // up, exactly as `renderScale` does; the interface is drawn at the
    // display's own resolution on top. Zero on a desktop, where a pixel is a
    // pixel somebody can see; set by `handheldSettings` on a handheld.
    u32 renderResolutionCap = 0;

    // One cascade's tile, in texels. The atlas is always two tiles by two, so
    // this squares: 512 costs 4 MiB, 1024 costs 16, 2048 costs 64. 2048 is the
    // default -- the High preset's, and the size a reference renderer gives its
    // sun -- because at 1024 thin casters in the far cascades lost their
    // shadows entirely (`shadow.h`, `kShadowTileResolution`).
    u32 shadowTileResolution = 2048;

    // How many cascades the sun casts into, 0 through 4. Zero is "no sun
    // shadow", which is a real setting on a weak machine and not a bug: the
    // shadow pass then submits nothing and `shadowRadius` reports zero, so
    // extraction stops keeping off-screen casters as well.
    //
    // Fewer than four does not shrink the atlas -- `shadowTileResolution` is
    // the dial for memory. What it removes is submission: every caster is drawn
    // once per cascade it touches.
    u32 shadowCascades = 4;

    // How far from the camera the sun casts, in metres.
    f32 shadowDistance = 120.0f;

    // How many lights one frame may carry into the cluster assignment. Beyond
    // it, the lights nearest the extraction order's front win -- deterministic,
    // because extraction order is (R10).
    u32 lightBudget = 256;

    bool bloom = true;
    bool ambientOcclusion = true;
    // Screen-space contact shadows for the sun (`contact_shadow.hlsl`): the few
    // centimetres at the base of a caster that the shadow map's biases give
    // away, recovered from the depth buffer.
    bool contactShadows = true;
    // **How many pixels a terrain cell may cover** before its node shows its
    // children (ADR 0140): 4 at low, 3 at medium, 2 at high, 1.5 at ultra, at
    // the viewport's own height. Smaller is finer ground further out, and
    // more of it to build and draw.
    f32 terrainPixelError = 2.0f;
    AntiAliasingMode antiAliasing = AntiAliasingMode::Smaa;
    // How a world drawn smaller than the output is brought up to it -- which
    // only matters when it is: at a render scale of 1 there is nothing to
    // upscale, and FSR 1 does nothing. **FSR 2 at a scale of 1 is the
    // anti-aliasing alone** (ADR 0164), and at any scale it takes
    // `antiAliasing`'s place on the frames it upscales.
    UpscalingMode upscaling = UpscalingMode::None;
    // **How much RCAS sharpens**, 0 to 1 (ADR 0158): after FSR 1's upscale,
    // after temporal anti-aliasing, which softens, and inside FSR 2.
    f32 sharpness = 0.2f;
    // **Whether a frame is made between every two the world is drawn**
    // (ADR 0165): FSR 3's frame generation. The world's main view alone; the
    // interface is drawn on every frame shown, made or drawn. Off in every
    // preset -- it adds a frame's wait to every input.
    bool frameGeneration = false;
    // Whether a world's `DepthOfFieldEffect` is drawn (ADR 0096). **The machine
    // wins over the world**, as it does for bloom: a scene that asks for focus
    // on a machine that cannot afford it draws sharp. `BlurEffect` and
    // `ColorCorrectionEffect` have no switch here, because a game uses them to
    // SAY something -- a pause, a flash of damage -- and turning them off would
    // change what the picture means rather than what it costs.
    bool depthOfField = true;
    // Whether a world's `SunRaysEffect` is drawn, by the same rule.
    bool sunRays = true;

    // False holds the exposure at the calibration key instead of metering the
    // frame. The scene still tonemaps and `ExposureCompensation` still applies;
    // what stops is the three-pass reduction and the frame-to-frame adaptation.
    bool autoExposure = true;

    // **A test instrument, not a setting** (ADR 0091): the name of a surface
    // shader the engine ships, drawn in place of the built-in surface on every
    // static part that has no surface of its own. `--force-surface=pbr` over
    // the screenshot scenes is the proof that the contract can say everything
    // the built-in surface says. Empty for everybody else.
    std::string forcedSurface;

    // **A test instrument** (terrain audit T0): `--debug-view=NAME`. The
    // holes view also turns off what would blend the sky into the ground --
    // anti-aliasing, bloom, occlusion, automatic exposure.
    DebugView debugView = DebugView::None;

    // **A test instrument** (H2): `--no-instancing` draws every object alone,
    // the picture instancing has to match pixel for pixel.
    bool instancing = true;

    // **How many taps the sun's shadow filter takes at a fragment** (ADR
    // 0172): four at Low, eight at Medium, and zero -- the filter's full
    // sixteen -- at High and above. It follows the shadow quality.
    u32 shadowTaps = 0;
    // **A measuring instrument** (ADR 0171): `[debug] shadow_taps`, 1 through
    // 16, said over the level's count to see what the filter costs. Zero is
    // not measuring. Carried across a player's settings changes, as the two
    // instruments above are.
    u32 measuredShadowTaps = 0;
    // **A measuring instrument** (ADR 0171): `[debug] skip`, the terms of a
    // lit surface that are left out, as `MeasureSkip` bits. Zero is none.
    u32 measuredSkip = 0;

    // **The lean ground** (ADR 0175): the terrain's material drawn from one
    // read of each layer's maps at a plane -- no second sample at the far
    // scale, no hexagonal cells, no procedural variation over it. The level's:
    // Low, and a handheld's Medium.
    bool terrainLean = false;
    // What the project said of it (`[graphics] terrain_surface`, or
    // `--terrain-surface=`), over the level: `Level` is nothing said. Carried
    // across a player's settings changes, since no setting of theirs is it.
    enum class TerrainSurface : core::u8
    {
        Level,
        Full,
        Lean,
        // **The ground's compiled variants** (ADR 0179), each a shader of its
        // own with nothing else in it: `Fast`, at most four layers a pixel by
        // their colour alone, lit diffuse; and `Flat`, one colour and no
        // light, which is what drawing the ground costs whatever it does.
        Fast,
        Flat,
    };
    TerrainSurface terrainSurface = TerrainSurface::Level;
    [[nodiscard]] bool leanTerrain() const noexcept
    {
        return terrainSurface == TerrainSurface::Level ? terrainLean : terrainSurface == TerrainSurface::Lean;
    }
    [[nodiscard]] bool compiledTerrain() const noexcept
    {
        return terrainSurface == TerrainSurface::Fast || terrainSurface == TerrainSurface::Flat;
    }

    // **How many levels the bloom's chain has** (ADR 0172), 2 through 5: each
    // is a pass down and a pass up, and each reaches twice as far as the one
    // before. Five on a desk; three on a handheld, where the passes are what
    // bloom costs and the widest two are the least seen.
    u32 bloomLevels = 5;
};

// **The terms `[debug] skip` can leave out of a lit surface** (ADR 0171),
// and the bits the forward shaders read them by (`engine_forward.hlsli` has
// the same list).
namespace MeasureSkip {
inline constexpr u32 Sun = 1u;          // the sun's direct light, its shadow with it
inline constexpr u32 Shadow = 2u;       // the sun's shadow map: every lit face is in the sun
inline constexpr u32 Contact = 4u;      // the contact shadow mask
inline constexpr u32 Lights = 8u;       // every light but the sun
inline constexpr u32 Environment = 16u; // the sky's reflection and its irradiance
inline constexpr u32 Ambient = 32u;
inline constexpr u32 Occlusion = 64u; // the screen-space occlusion's picture
inline constexpr u32 Fog = 128u;
inline constexpr u32 NormalMap = 256u;
inline constexpr u32 MaterialMaps = 512u; // metallic-roughness and emissive
inline constexpr u32 Unlit = 1024u;       // base colour alone
} // namespace MeasureSkip

// The taps a shadow quality filters with: `shadowTaps` for the level's place
// in Low, Medium, High, Ultra.
[[nodiscard]] u32 shadowTapsFor(QualityLevel level) noexcept;

// The named set every preset is. `High` was exactly what the engine shipped
// through M7.5, because every golden in the repository was recorded against
// it; ADR 0158 moved its anti-aliasing from FXAA to SMAA, alone and on purpose,
// and the goldens were recorded again for that one change.
[[nodiscard]] GraphicsSettings settingsFor(QualityLevel quality) noexcept;

// Clamps every field into the range the renderer can honour, so a hand-edited
// project file cannot ask for a 32-texel shadow map or a render scale of zero.
// Returns the corrected copy rather than mutating, because the caller wants to
// report what it changed.
[[nodiscard]] GraphicsSettings clampSettings(GraphicsSettings settings) noexcept;

// **The level a machine starts at when nobody named one**: `High` on a
// desktop, and one step lower on a handheld (ADR 0147 section 2), where the
// same frame is paid for in heat and battery.
[[nodiscard]] QualityLevel defaultQuality(bool handheld) noexcept;

// What a handheld adds to a preset: the cap on the world's resolution, by
// level -- 720, 900 and 1080 pixels on the shorter side for low, medium and
// high, and the display's own for ultra. Applied under the project's file, so
// `[graphics] render_cap` and the per-platform table still decide.
[[nodiscard]] GraphicsSettings handheldSettings(GraphicsSettings settings) noexcept;

// **The fraction of a target the world is rendered at**: `renderScale`, and
// less where that would still be more than `renderResolutionCap` on the
// target's shorter side. One answer for the renderer, which sizes its images
// by it, and for whoever measures the picture in pixels (the terrain's level
// of detail).
[[nodiscard]] f32 effectiveRenderScale(const GraphicsSettings& settings, u32 width, u32 height) noexcept;

[[nodiscard]] std::optional<QualityLevel> parseQuality(std::string_view name) noexcept;
[[nodiscard]] std::optional<DebugView> parseDebugView(std::string_view name) noexcept;
[[nodiscard]] std::string_view qualityName(QualityLevel quality) noexcept;

} // namespace engine::render
