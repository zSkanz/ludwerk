// The wind (ADR 0115): a pure function of the settings, the place and the
// simulation's clock.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>

#include "engine/scene/wind.h"

using namespace engine;

namespace {

[[nodiscard]] double lengthOf(core::Vec3 v)
{
    return std::sqrt(static_cast<double>(v.x * v.x + v.y * v.y + v.z * v.z));
}

} // namespace

TEST_CASE("a still world has no wind anywhere, ever")
{
    const scene::WindSettings still{};
    const core::Vec3 wind = scene::windAt(still, core::Vec3{12.0f, 3.0f, -40.0f}, 99.5f);
    CHECK(wind.x == 0.0f);
    CHECK(wind.y == 0.0f);
    CHECK(wind.z == 0.0f);
}

TEST_CASE("a steady wind is the global wind, everywhere")
{
    const scene::WindSettings steady{core::Vec3{4.0f, 0.0f, 3.0f}, 0.0f, 0.0f};
    for (float x = -100.0f; x < 100.0f; x += 17.0f) {
        const core::Vec3 wind = scene::windAt(steady, core::Vec3{x, 0.0f, x * 0.5f}, x * 0.1f);
        CHECK(static_cast<double>(wind.x) == doctest::Approx(4.0).epsilon(1e-4));
        CHECK(static_cast<double>(wind.z) == doctest::Approx(3.0).epsilon(1e-4));
    }
}

TEST_CASE("gusts rise and fall within their share of the speed, and travel downwind")
{
    const scene::WindSettings gusty{core::Vec3{10.0f, 0.0f, 0.0f}, 0.5f, 0.0f};
    double lowest = 1.0e9;
    double highest = 0.0;
    for (float x = 0.0f; x < 400.0f; x += 1.3f) {
        const double speed = lengthOf(scene::windAt(gusty, core::Vec3{x, 0.0f, 7.0f}, 0.0f));
        lowest = std::min(lowest, speed);
        highest = std::max(highest, speed);
    }
    // Half again at most, half less at least, and it does vary.
    CHECK(lowest >= 5.0 - 1e-3);
    CHECK(highest <= 15.0 + 1e-3);
    CHECK(highest - lowest > 2.0);

    // **A gust is seen crossing a field**: what blows at x at one time blows
    // ten metres downwind a second later, at ten metres a second.
    for (float x = 0.0f; x < 100.0f; x += 9.1f) {
        const core::Vec3 here = scene::windAt(gusty, core::Vec3{x, 0.0f, 3.0f}, 2.0f);
        const core::Vec3 later = scene::windAt(gusty, core::Vec3{x + 10.0f, 0.0f, 3.0f}, 3.0f);
        CHECK(static_cast<double>(later.x) == doctest::Approx(static_cast<double>(here.x)).epsilon(1e-3));
    }
}

TEST_CASE("turbulence turns the direction and keeps the speed")
{
    const scene::WindSettings turbulent{core::Vec3{0.0f, 0.0f, 8.0f}, 0.0f, 1.0f};
    bool turned = false;
    for (float x = 0.0f; x < 60.0f; x += 3.3f) {
        const core::Vec3 wind = scene::windAt(turbulent, core::Vec3{x, 0.0f, x * 0.7f}, 1.5f);
        CHECK(lengthOf(wind) == doctest::Approx(8.0).epsilon(1e-4));
        turned = turned || std::abs(static_cast<double>(wind.x)) > 0.5;
    }
    CHECK(turned);
}
