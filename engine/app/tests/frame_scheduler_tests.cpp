// The fixed-tick accumulator and the choice of clock it is fed.
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
