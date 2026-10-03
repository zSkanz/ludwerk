// The frame's scoped timers (H0): a tree that adds up, on a clock the test
// moves by hand.
#include <doctest/doctest.h>
#include <thread>

#include "engine/core/profile.h"

namespace {

namespace profile = engine::core::profile;
using engine::core::u64;

u64 g_now = 0;

u64 fakeNow()
{
    return g_now;
}

// Spends `ms` of the fake clock.
void spend(double ms)
{
    g_now += static_cast<u64>(ms * 1.0e6);
}

struct Rig
{
    Rig()
    {
        g_now = 0;
        profile::setClockForTests(fakeNow);
        profile::setEnabled(true);
    }
    ~Rig()
    {
        profile::setEnabled(false);
        profile::setClockForTests(nullptr);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
};

// One frame: a tick of 6 ms holding physics (3 ms) and two script calls of
// 1 ms; 2 ms of the tick is in neither.
void frame(bool withScripts)
{
    ENG_PROFILE_SCOPE("test.frame");
    {
        ENG_PROFILE_SCOPE("test.tick");
        {
            ENG_PROFILE_SCOPE("test.physics");
            spend(3.0);
        }
        if (withScripts) {
            for (int call = 0; call < 2; ++call) {
                ENG_PROFILE_SCOPE("test.scripts");
                spend(1.0);
            }
        }
        spend(withScripts ? 0.0 : 2.0);
    }
    spend(1.0);
}

} // namespace

TEST_CASE("H0: the scopes of a frame form a tree whose parts add up to the whole")
{
    Rig rig;
    for (int index = 0; index < 10; ++index) {
        frame(true);
        profile::endFrame();
    }
    const std::vector<profile::ScopeReport> rows = profile::report(0);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].name == "test.frame");
    CHECK(rows[0].depth == 0);
    CHECK(rows[0].medianMs == doctest::Approx(6.0));
    CHECK(rows[1].name == "test.tick");
    CHECK(rows[1].depth == 1);
    CHECK(rows[1].medianMs == doctest::Approx(5.0));
    CHECK(rows[2].name == "test.physics");
    CHECK(rows[2].depth == 2);
    CHECK(rows[3].name == "test.scripts");
    CHECK(rows[3].calls == doctest::Approx(2.0));
    CHECK(rows[3].medianMs == doctest::Approx(2.0));

    // What no scope under a scope accounts for is its own: the frame's 1 ms
    // outside the tick, and nothing in a tick its children fill.
    CHECK(rows[0].selfMedianMs == doctest::Approx(1.0));
    CHECK(rows[1].selfMedianMs == doctest::Approx(0.0));
    // And the parts add up to the whole, within a rounding.
    double children = 0.0;
    for (const profile::ScopeReport& row : rows) {
        if (row.depth == 1)
            children += row.medianMs;
    }
    CHECK(children + rows[0].selfMedianMs == doctest::Approx(rows[0].medianMs).epsilon(0.01));
}

TEST_CASE("H0: a scope that did not run in a frame counts it as zero, and warm-up frames can be left out")
{
    Rig rig;
    for (int index = 0; index < 4; ++index) {
        frame(index >= 2);
        profile::endFrame();
    }
    // Two frames without scripts, two with: the median of the four is the
    // middle pair's, one zero and one 2 ms.
    std::vector<profile::ScopeReport> rows = profile::report(0);
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].name == "test.scripts");
    CHECK(rows[3].p95Ms == doctest::Approx(2.0));
    CHECK(rows[3].worstMs == doctest::Approx(2.0));
    // Leaving out the first two, scripts ran in every frame left.
    rows = profile::report(2);
    CHECK(rows[3].medianMs == doctest::Approx(2.0));
    CHECK(rows[1].selfMedianMs == doctest::Approx(0.0));
}

TEST_CASE("H0: a scope that ran only in frames left out is not reported")
{
    Rig rig;
    {
        ENG_PROFILE_SCOPE("test.loading");
        spend(50.0);
    }
    profile::endFrame();
    for (int index = 0; index < 3; ++index) {
        frame(false);
        profile::endFrame();
    }
    const std::vector<profile::ScopeReport> rows = profile::report(1);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "test.frame");
}

TEST_CASE("H0: off, or on another thread, a scope records nothing")
{
    {
        Rig rig;
        std::thread worker([] {
            ENG_PROFILE_SCOPE("test.worker");
            spend(5.0);
        });
        worker.join();
        profile::endFrame();
        CHECK(profile::report(0).empty());
    }
    profile::setEnabled(false);
    CHECK_FALSE(profile::enabled());
    {
        ENG_PROFILE_SCOPE("test.off");
    }
    profile::endFrame();
    CHECK(profile::report(0).empty());
}
