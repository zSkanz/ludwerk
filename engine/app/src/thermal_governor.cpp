#include "engine/app/thermal_governor.h"

namespace engine::app {

bool ThermalGovernor::sample(platform::ThermalStatus status, core::f64 nowSeconds) noexcept
{
    using platform::ThermalStatus;
    if (status == ThermalStatus::Unknown)
        return false;
    const core::u32 last = static_cast<core::u32>(Steps.size()) - 1;
    if (status >= ThermalStatus::Moderate) {
        m_coolSince = -1.0;
        const core::f64 every = status >= ThermalStatus::Severe ? SevereEverySeconds : ModerateEverySeconds;
        if (m_step < last && (m_step == 0 || nowSeconds - m_lastStepAt >= every)) {
            ++m_step;
            m_lastStepAt = nowSeconds;
            return true;
        }
        return false;
    }
    // `Light` and `None`: cool. Each step back waits a cool spell of its own.
    if (m_step == 0) {
        m_coolSince = -1.0;
        return false;
    }
    if (m_coolSince < 0.0)
        m_coolSince = nowSeconds;
    if (nowSeconds - m_coolSince >= CoolSeconds) {
        --m_step;
        m_lastStepAt = nowSeconds;
        m_coolSince = nowSeconds;
        return true;
    }
    return false;
}

core::TextKey thermalStatusKey(platform::ThermalStatus status) noexcept
{
    using platform::ThermalStatus;
    switch (status) {
    case ThermalStatus::None:
        return ENG_TR("engine.thermal.none");
    case ThermalStatus::Light:
        return ENG_TR("engine.thermal.light");
    case ThermalStatus::Moderate:
        return ENG_TR("engine.thermal.moderate");
    case ThermalStatus::Severe:
        return ENG_TR("engine.thermal.severe");
    case ThermalStatus::Critical:
        return ENG_TR("engine.thermal.critical");
    case ThermalStatus::Emergency:
        return ENG_TR("engine.thermal.emergency");
    case ThermalStatus::Shutdown:
        return ENG_TR("engine.thermal.shutdown");
    case ThermalStatus::Unknown:
        break;
    }
    return ENG_TR("engine.thermal.unknown");
}

} // namespace engine::app
