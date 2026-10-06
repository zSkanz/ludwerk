#pragma once

// **The measuring keys** (ADR 0171): `[debug]` in the project's file, and the
// flags of the same names. A phone has no profiler attached and takes no
// command line, so what a frame's parts cost is found by taking them out one
// at a time -- and by the GPU's time per pass, which the frame report prints.
// None of this is a setting of the game: every key is off unless asked for,
// and the report says which are in force so a measurement is never read
// without knowing what it was a measurement of.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"
#include "engine/render/render_world.h"
#include "engine/render/settings.h"
#include "engine/rhi/device.h"
#include "engine/scene/world.h"

namespace engine::app {

// `[debug] hide = "foliage,terrain"`: what is taken out of every frame drawn,
// after the world is extracted and before the renderer is handed it. The
// simulation goes on as it was -- a hidden enemy still walks -- so the
// difference in a frame's time is what drawing the thing cost.
struct DebugHide
{
    // Meshes that are none of the kinds below: parts and static models.
    bool parts = false;
    // Meshes drawn with a skeleton.
    bool skinned = false;
    bool terrain = false;
    // The block world.
    bool voxels = false;
    bool foliage = false;
    // Whatever is drawn blended, of the meshes.
    bool transparent = false;
    bool particles = false;
    // Beams and trails.
    bool ribbons = false;
    bool decals = false;
    bool sprites = false;
    // The interface drawn in the world: billboards and surfaces.
    bool worldUi = false;
    // The interface drawn on the screen.
    bool ui = false;
    // Every light but the sun.
    bool lights = false;
    bool highlights = false;

    [[nodiscard]] bool any() const noexcept
    {
        return parts || skinned || terrain || voxels || foliage || transparent || particles || ribbons || decals ||
               sprites || worldUi || ui || lights || highlights;
    }
};

// The names `hide` takes, in the order they are said back.
[[nodiscard]] std::span<const std::string_view> debugHideNames() noexcept;

// Reads a list separated by commas (spaces around a name are nothing). A name
// that is none of `debugHideNames` is put in `unknown` and hides nothing.
[[nodiscard]] DebugHide parseDebugHide(std::string_view list, std::vector<std::string>* unknown = nullptr);

// The same list back, of what is hidden: "foliage,terrain". Empty for none.
[[nodiscard]] std::string debugHideText(const DebugHide& hide);

// Takes what is hidden out of a frame's world.
void applyDebugHide(render::RenderWorld& world, const DebugHide& hide);

// `[debug] skip = "shadow,environment"`: the terms of a lit surface that are
// left out, as `render::MeasureSkip` bits -- `sun`, `shadow`, `contact`,
// `lights`, `environment`, `ambient`, `occlusion`, `fog`, `normal_map`,
// `material_maps`, and `unlit` for the base colour alone. Where `hide` says
// which objects cost a frame its time, this says which part of lighting them.
[[nodiscard]] std::span<const std::string_view> debugSkipNames() noexcept;
[[nodiscard]] core::u32 parseDebugSkip(std::string_view list, std::vector<std::string>* unknown = nullptr);
[[nodiscard]] std::string debugSkipText(core::u32 skip);

// **What the report says is in force**: "gpu_pass_times, hide=foliage,terrain,
// skip=shadow, shadow_taps=4". Empty when nothing is.
[[nodiscard]] std::string debugKeysInForce(bool gpuPassTimes, const DebugHide& hide, core::u32 shadowTaps,
                                           bool logUiTouches, core::u32 skip = 0);

// `[debug] log_ui_touches`: a line for a finger that came down, saying which
// element of the interface took it -- by its whole name and its rectangle --
// or that none did and it is the game's. `under` is what the hit test
// answered.
void logUiTouch(const scene::World& world, core::InstanceId under, core::i64 finger, core::Vec2 at);
// An instance's whole name, from the root down: "Game.PlayerGui.Hud.Stick".
[[nodiscard]] std::string wholeName(const scene::World& world, core::InstanceId id);

// **The GPU's time by pass, over the frames since it was last said**: summed
// here frame by frame, and said as a mean.
class PassTimeLedger
{
public:
    void add(std::span<const rhi::PassTime> frame);
    [[nodiscard]] core::u64 frames() const noexcept { return frames_; }
    void clear() noexcept;

    struct Line
    {
        // "forward 6.21, shadow 3.02, ui 0.40": a frame's mean in
        // milliseconds, the costliest first.
        std::string passes;
        // Their sum.
        core::f64 total = 0.0;
        // How many times a frame was stopped to be timed.
        core::f64 submits = 0.0;
        // What one of those stops costs when its pass clears one pixel.
        core::f64 floor = 0.0;
        // What the same fixed work took, a frame's mean: how fast the GPU
        // was running. Nought where the device times none.
        core::f64 clock = 0.0;
    };
    [[nodiscard]] Line line() const;

private:
    struct Total
    {
        std::string name;
        core::f64 milliseconds = 0.0;
        core::u64 submits = 0;
    };
    std::vector<Total> totals_;
    core::u64 frames_ = 0;
};

} // namespace engine::app
