#pragma once

// **The picture drawn smaller before the phone slows itself down** (the mobile
// lean path): from the device's own thermal status, a step down the render
// scale, and back up once it has stayed cool. Off unless the project turns it
// on (`[display] thermal_scaling`); what each step is, and why, is said in the
// log, in the frame report and in F3's Stats.
//
// A pure function of the statuses and the times it is given, so its rules are
// tested without a phone to warm up.

#include <array>

#include "engine/core/text_key.h"
#include "engine/core/types.h"
#include "engine/platform/thermal.h"

namespace engine::app {

class ThermalGovernor
{
public:
    // The render scale each step draws at, of what the settings ask for.
    static constexpr std::array<core::f32, 4> Steps{1.0f, 0.85f, 0.7f, 0.55f};
    // While `Moderate`, one step further at most this often; while `Severe`
    // or hotter, this often. The first step is taken at once.
    static constexpr core::f64 ModerateEverySeconds = 15.0;
    static constexpr core::f64 SevereEverySeconds = 5.0;
    // Cool (`Light` or under) this long, one step back.
    static constexpr core::f64 CoolSeconds = 30.0;

    // One reading at `nowSeconds`; true when the step changed. `Unknown`
    // changes nothing.
    bool sample(platform::ThermalStatus status, core::f64 nowSeconds) noexcept;

    [[nodiscard]] core::u32 step() const noexcept { return m_step; }
    [[nodiscard]] core::f32 factor() const noexcept { return Steps[m_step]; }

private:
    core::u32 m_step = 0;
    core::f64 m_lastStepAt = -1.0e30;
    core::f64 m_coolSince = -1.0;
};

// The status's words for a person, from the catalog: a log line and F3's.
[[nodiscard]] core::TextKey thermalStatusKey(platform::ThermalStatus status) noexcept;

} // namespace engine::app
