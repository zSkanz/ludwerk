// Which operating system this build is for: `Enum.Platform`'s value, and
// `RunService.Platform` (D456). A fact of the build, known before anything
// runs.
#pragma once

#include "engine/core/types.h"

namespace engine::core {

// `Enum.Platform`, item for item.
enum class HostPlatform : i32
{
    Windows = 0,
    Linux = 1,
    MacOS = 2,
    Android = 3,
    IOS = 4,
};

#if defined(_WIN32)
inline constexpr HostPlatform ThisPlatform = HostPlatform::Windows;
#elif defined(__ANDROID__)
inline constexpr HostPlatform ThisPlatform = HostPlatform::Android;
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
inline constexpr HostPlatform ThisPlatform = HostPlatform::IOS;
#else
inline constexpr HostPlatform ThisPlatform = HostPlatform::MacOS;
#endif
#else
inline constexpr HostPlatform ThisPlatform = HostPlatform::Linux;
#endif

} // namespace engine::core
