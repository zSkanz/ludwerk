// **The picture drawn smaller as a phone heats** (the mobile lean path): the
// governor's rules, against statuses and times it is handed.

#include <doctest/doctest.h>
#include <ostream>
#include <string_view>

#include "engine/app/thermal_governor.h"
#include "engine/platform/thermal.h"

using engine::app::ThermalGovernor;
using engine::platform::ThermalStatus;

TEST_CASE("a warm phone steps down at once, further while it stays warm, and no further than the last step")
{
    ThermalGovernor governor;
    CHECK_FALSE(governor.sample(ThermalStatus::None, 0.0));
    CHECK(governor.factor() == 1.0f);

    CHECK(governor.sample(ThermalStatus::Moderate, 1.0));
    CHECK(governor.step() == 1);
    // Still moderate, sooner than the wait: held.
    CHECK_FALSE(governor.sample(ThermalStatus::Moderate, 1.0 + ThermalGovernor::ModerateEverySeconds - 1.0));
    CHECK(governor.step() == 1);
    CHECK(governor.sample(ThermalStatus::Moderate, 1.0 + ThermalGovernor::ModerateEverySeconds));
    CHECK(governor.step() == 2);
    // Severe steps on sooner.
    CHECK(governor.sample(ThermalStatus::Severe,
                          1.0 + ThermalGovernor::ModerateEverySeconds + ThermalGovernor::SevereEverySeconds));
    CHECK(governor.step() == 3);
    CHECK(governor.factor() == ThermalGovernor::Steps.back());
    CHECK_FALSE(governor.sample(ThermalStatus::Critical, 1000.0));
    CHECK(governor.step() == 3);
}

TEST_CASE("a phone that cools steps back one at a time, each after a cool spell, and a warm reading restarts it")
{
    ThermalGovernor governor;
    (void)governor.sample(ThermalStatus::Severe, 0.0);
    (void)governor.sample(ThermalStatus::Severe, 10.0);
    REQUIRE(governor.step() == 2);

    CHECK_FALSE(governor.sample(ThermalStatus::Light, 12.0));
    CHECK_FALSE(governor.sample(ThermalStatus::Light, 23.0));
    // Warm again before the spell ended, and sooner than a step further:
    // nothing changes, and the spell starts again.
    CHECK_FALSE(governor.sample(ThermalStatus::Moderate, 24.0));
    CHECK(governor.step() == 2);
    CHECK_FALSE(governor.sample(ThermalStatus::None, 30.0));
    CHECK_FALSE(governor.sample(ThermalStatus::None, 30.0 + ThermalGovernor::CoolSeconds - 1.0));
    CHECK(governor.sample(ThermalStatus::None, 30.0 + ThermalGovernor::CoolSeconds));
    CHECK(governor.step() == 1);
    CHECK(governor.sample(ThermalStatus::None, 30.0 + 2.0 * ThermalGovernor::CoolSeconds));
    CHECK(governor.step() == 0);
    CHECK(governor.factor() == 1.0f);
}

TEST_CASE("a status the system will not give changes nothing")
{
    ThermalGovernor governor;
    (void)governor.sample(ThermalStatus::Moderate, 0.0);
    CHECK_FALSE(governor.sample(ThermalStatus::Unknown, 500.0));
    CHECK(governor.step() == 1);
    CHECK(engine::platform::thermalStatusName(ThermalStatus::Moderate) == "moderate");
    CHECK(engine::platform::thermalStatusName(ThermalStatus::Unknown) == "unknown");
}
