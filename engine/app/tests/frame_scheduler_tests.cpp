// The fixed-tick accumulator and the choice of clock it is fed.
#include <algorithm>
#include <doctest/doctest.h>

#include "engine/app/frame_scheduler.h"
#include "engine/core/log.h"

using namespace engine;

TEST_CASE("a clock that goes backwards is no time at all, not eighteen trillion milliseconds")
{
    // **D209.** A headless client that left a match went from the real clock,
    // days into the machine's uptime, back to the synthetic one, which counts
    // from zero: `now - last` wrapped, and the frame reported "took
    // 18446440048196 ms" and clamped.
    app::FrameScheduler scheduler;
    (void)scheduler.beginFrame(300'000'000'000'000ull);
    const app::Frame back = scheduler.beginFrame(16'666'667ull);
    CHECK(back.renderDt == 0.0);
    CHECK_FALSE(back.clamped);
    CHECK(back.simTicks == 0);

    // And the clock goes on from there.
    const app::Frame next = scheduler.beginFrame(16'666'667ull + 16'666'667ull);
    CHECK(next.simTicks == 1);
}

TEST_CASE("a headless process that has had real time keeps it")
{
    // **D213.** A golden run is synthetic from its first frame to its last. A
    // headless client back in solo after a match used to go synthetic too, and
    // ran hundreds of ticks a second with nobody asking for them.
    app::FrameClock golden;
    for (int frame = 0; frame < 3; ++frame)
        CHECK(golden.synthetic(/*headless=*/true, /*devSession=*/false, /*networked=*/false));

    app::FrameClock client;
    CHECK(client.synthetic(true, false, false));
    CHECK_FALSE(client.synthetic(true, false, /*networked=*/true));
    CHECK_FALSE(client.synthetic(true, false, /*networked=*/false));

    app::FrameClock windowed;
    CHECK_FALSE(windowed.synthetic(/*headless=*/false, false, false));
}

TEST_CASE("a clock that becomes another clock is measured from the switch, not from boot (audit A9)")
{
    // A headless run on the synthetic clock, then a `Join`: the real clock is
    // the machine's uptime, and the first real frame took all of it -- "Frame
    // took 5391574 ms", clamped, and the catch-up ticks run for nothing.
    app::FrameClock clock;
    app::FrameScheduler scheduler;
    REQUIRE(clock.synthetic(true, false, false));
    CHECK_FALSE(clock.switched());
    (void)scheduler.beginFrame(0);
    (void)scheduler.beginFrame(16'666'667ull);

    const engine::core::u64 uptime = 5'391'574'000'000ull;
    REQUIRE_FALSE(clock.synthetic(true, false, /*networked=*/true));
    REQUIRE(clock.switched());
    scheduler.rebase(uptime);
    const app::Frame joined = scheduler.beginFrame(uptime);
    CHECK_FALSE(joined.clamped);
    CHECK(joined.renderDt == 0.0);

    const app::Frame next = scheduler.beginFrame(uptime + 16'666'667ull);
    CHECK_FALSE(next.clamped);
    CHECK(next.simTicks == 1);
    // And only on the frame it changed.
    (void)clock.synthetic(true, false, true);
    CHECK_FALSE(clock.switched());
}

TEST_CASE("a machine that cannot keep up says so every few seconds, not every frame (audit A14)")
{
    int warnings = 0;
    const engine::core::LogSink previous =
        engine::core::setLogSink([&warnings](engine::core::LogLevel level, std::string_view) {
            if (level == engine::core::LogLevel::Warn)
                ++warnings;
        });
    app::FrameScheduler scheduler;
    engine::core::u64 now = 0;
    (void)scheduler.beginFrame(now);
    // Two hundred frames of 100 ms each: every one clamped, twenty seconds.
    for (int frame = 0; frame < 200; ++frame) {
        now += 100'000'000ull;
        CHECK(scheduler.beginFrame(now).clamped);
    }
    engine::core::setLogSink(previous);
    CHECK(warnings >= 4);
    CHECK(warnings <= 5);
}

TEST_CASE("D565: frames that keep the ticks' rate run one tick each, however their starts jitter")
{
    // A display at sixty and a simulation at sixty, and frames that begin
    // half a millisecond early or late -- a sleep's precision on a phone.
    // Where a frame begins just as a tick comes due, the early frame owed
    // none and the late one after it two: twice the simulation in one frame
    // of every pair, for as long as the two clocks stood that way.
    app::FrameScheduler scheduler;
    engine::core::u64 now = 1'000'000'000ull;
    (void)scheduler.beginFrame(now);
    for (int frame = 0; frame < 4; ++frame) {
        now += 16'666'667ull;
        CHECK(scheduler.beginFrame(now).simTicks == 1);
    }
    int notOne = 0;
    for (int frame = 0; frame < 600; ++frame) {
        now += frame % 2 == 0 ? 16'166'667ull : 17'166'667ull;
        const app::Frame begun = scheduler.beginFrame(now);
        notOne += begun.simTicks == 1 ? 0 : 1;
        CHECK(begun.alpha >= 0.0f);
        CHECK(begun.alpha < 1.0f);
    }
    CHECK(notOne == 0);
    CHECK(scheduler.totalTicks() == 604);

    // The same at thirty frames a second, two ticks a frame.
    app::FrameScheduler half;
    now = 1'000'000'000ull;
    (void)half.beginFrame(now);
    for (int frame = 0; frame < 4; ++frame) {
        now += 33'333'334ull;
        CHECK(half.beginFrame(now).simTicks == 2);
    }
    int notTwo = 0;
    for (int frame = 0; frame < 300; ++frame) {
        now += frame % 2 == 0 ? 32'533'334ull : 34'133'334ull;
        notTwo += half.beginFrame(now).simTicks == 2 ? 0 : 1;
    }
    CHECK(notTwo == 0);
}

TEST_CASE("D565: no time is made or lost by it, and frames at another rate are counted as they were")
{
    // A display a hair slower than the simulation: the ticks it owes are
    // owed, a frame of two every so often and never a second of drift.
    app::FrameScheduler slower;
    engine::core::u64 now = 5'000'000'000ull;
    const engine::core::u64 start = now;
    (void)slower.beginFrame(now);
    engine::core::u32 most = 0;
    for (int frame = 0; frame < 1200; ++frame) {
        now += 16'949'153ull;
        most = std::max(most, slower.beginFrame(now).simTicks);
    }
    const double owed = static_cast<double>(now - start) / 1.0e9 * 60.0;
    CHECK(static_cast<double>(slower.totalTicks()) > owed - 1.5);
    CHECK(static_cast<double>(slower.totalTicks()) < owed + 1.5);
    CHECK(most == 2);

    // A display at twice the simulation's rate has no count that is the
    // frames' own -- one, none, one, none -- and nothing is held to one: every
    // frame's place between two ticks is exactly where its time puts it.
    app::FrameScheduler faster;
    now = 9'000'000'000ull;
    (void)faster.beginFrame(now);
    double place = 0.0;
    for (int frame = 0; frame < 600; ++frame) {
        const engine::core::u64 step = frame % 2 == 0 ? 8'033'333ull : 8'633'334ull;
        now += step;
        const app::Frame begun = faster.beginFrame(now);
        place += static_cast<double>(step) / 1.0e9 * 60.0;
        place -= static_cast<double>(begun.simTicks);
        CHECK(begun.simTicks <= 1);
        CHECK(static_cast<double>(begun.alpha) == doctest::Approx(place).epsilon(0.001));
    }

    // And a hitch is caught up as before, then one a frame again.
    app::FrameScheduler hitched;
    now = 2'000'000'000ull;
    (void)hitched.beginFrame(now);
    for (int frame = 0; frame < 4; ++frame) {
        now += 16'666'667ull;
        (void)hitched.beginFrame(now);
    }
    now += 50'000'001ull;
    CHECK(hitched.beginFrame(now).simTicks == 3);
    for (int frame = 0; frame < 10; ++frame) {
        now += 16'666'667ull;
        CHECK(hitched.beginFrame(now).simTicks == 1);
    }
}
