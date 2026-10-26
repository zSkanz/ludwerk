#pragma once

// **How hot the device is**, as its own system rates it (the mobile lean
// path): what a phone that is about to slow itself down says before it does,
// so a game can draw less instead of dropping frames.

#include <string_view>

#include "engine/core/types.h"

namespace engine::platform {

// Android's thermal status, in its order (`AThermalStatus`). `Unknown` where
// the system will not say -- any platform but Android, and an Android before
// 11.
enum class ThermalStatus : core::u8
{
    Unknown,
    None,
    Light,
    Moderate,
    Severe,
    Critical,
    Emergency,
    Shutdown,
};

// The status now. `ENG_ASSUME_THERMAL_STATUS` (one of the names below) answers
// in its place on any platform: what a test and a computer try it with.
[[nodiscard]] ThermalStatus thermalStatus() noexcept;

// `unknown`, `none`, `light`, `moderate`, `severe`, `critical`, `emergency`,
// `shutdown`: the status's name, and what a log and a catalog key end in.
[[nodiscard]] std::string_view thermalStatusName(ThermalStatus status) noexcept;

} // namespace engine::platform
