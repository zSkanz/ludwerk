#include "engine/platform/thermal.h"

#include <array>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace engine::platform {
namespace {

constexpr std::array<std::string_view, 8> Names{"unknown", "none",     "light",     "moderate",
                                                "severe",  "critical", "emergency", "shutdown"};

#if defined(__ANDROID__)
// **Asked of `libandroid` by name**: the thermal manager is Android 11's, and
// a game that names it at link time would not start on the Android 5 it is
// built for. Looked up once; a system without it answers `Unknown`.
struct ThermalApi
{
    using Acquire = void* (*)();
    using Status = int (*)(void*);
    void* manager = nullptr;
    Status status = nullptr;

    ThermalApi()
    {
        void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (android == nullptr)
            return;
        const auto acquire = reinterpret_cast<Acquire>(dlsym(android, "AThermal_acquireManager"));
        status = reinterpret_cast<Status>(dlsym(android, "AThermal_getCurrentThermalStatus"));
        if (acquire != nullptr && status != nullptr)
            manager = acquire();
    }
};
#endif

} // namespace

ThermalStatus thermalStatus() noexcept
{
#if defined(_WIN32)
    char assumed[16];
    const DWORD length = GetEnvironmentVariableA("ENG_ASSUME_THERMAL_STATUS", assumed, sizeof(assumed));
    const std::string_view asked =
        length != 0 && length < sizeof(assumed) ? std::string_view{assumed, length} : std::string_view{};
#else
    const char* assumed = std::getenv("ENG_ASSUME_THERMAL_STATUS");
    const std::string_view asked = assumed != nullptr ? std::string_view{assumed} : std::string_view{};
#endif
    for (std::size_t index = 0; !asked.empty() && index < Names.size(); ++index) {
        if (Names[index] == asked)
            return static_cast<ThermalStatus>(index);
    }
#if defined(__ANDROID__)
    static const ThermalApi api;
    if (api.manager == nullptr)
        return ThermalStatus::Unknown;
    // Android's statuses are nought (none) to six (shutdown); -1 is an error.
    const int status = api.status(api.manager);
    if (status < 0 || status > 6)
        return ThermalStatus::Unknown;
    return static_cast<ThermalStatus>(status + 1);
#else
    return ThermalStatus::Unknown;
#endif
}

std::string_view thermalStatusName(ThermalStatus status) noexcept
{
    const auto index = static_cast<std::size_t>(status);
    return index < Names.size() ? Names[index] : Names[0];
}

} // namespace engine::platform
