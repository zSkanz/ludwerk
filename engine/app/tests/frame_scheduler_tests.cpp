// The fixed-tick accumulator and the choice of clock it is fed.
#include <algorithm>
#include <doctest/doctest.h>

#include "engine/app/frame_scheduler.h"
#include "engine/core/log.h"

using namespace engine;

TEST_CASE("a clock that goes backwards is no time at all, not eighteen trillion milliseconds")
{
    // A headless client that left a match went from the real clock,
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
    // A golden run is synthetic from its first frame to its last. A
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

TEST_CASE("a clock that becomes another clock is measured from the switch, not from boot")
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

TEST_CASE("a machine that cannot keep up says so every few seconds, not every frame")
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

TEST_CASE("frames that keep the ticks' rate run one tick each, however their starts jitter")
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

TEST_CASE("a frame half again as long as the others runs the ticks it owes, and is drawn where its time is")
{
    // Frames of one tick each, and then a hitch of 25.7 ms -- a loaded
    // machine -- that leaves just over two ticks owed. Held back to one, it
    // was drawn at the end of its tick, behind its time, and the frame after
    // drew the car leaping forward to catch up.
    app::FrameScheduler scheduler;
    engine::core::u64 now = 3'000'000'000ull;
    const engine::core::u64 start = now;
    (void)scheduler.beginFrame(now);
    // Nine milliseconds and a quarter into a tick, and frames of one tick
    // each from there: each drawn a little past the middle of its tick.
    now += 9'250'000ull;
    (void)scheduler.beginFrame(now);
    for (int frame = 0; frame < 10; ++frame) {
        now += 16'666'667ull;
        CHECK(scheduler.beginFrame(now).simTicks == 1);
    }
    now += 25'690'000ull;
    const app::Frame hitch = scheduler.beginFrame(now);
    CHECK(hitch.simTicks == 2);
    // Drawn where its time is: the ticks run and the place between them add
    // up to the time since the start, and so in the short frame after.
    const auto drawnAt = [&](const app::Frame& frame) {
        return (static_cast<double>(scheduler.totalTicks()) + static_cast<double>(frame.alpha)) / 60.0;
    };
    const double since = static_cast<double>(now - start) / 1.0e9;
    CHECK(drawnAt(hitch) == doctest::Approx(since).epsilon(0.0005));
    now += 6'010'000ull;
    const app::Frame after = scheduler.beginFrame(now);
    CHECK(drawnAt(after) == doctest::Approx(static_cast<double>(now - start) / 1.0e9).epsilon(0.0005));
}

TEST_CASE("no time is made or lost by it, and frames at another rate are counted as they were")
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

TEST_CASE("ADR 0233: a slowed clock owes fewer ticks a second, and the tick is the same")
{
    app::FrameScheduler scheduler;
    scheduler.setTimeScale(0.5);
    engine::core::u32 ticks = 0;
    // Two seconds of sixty frames a second.
    for (engine::core::u64 frame = 0; frame <= 120; ++frame)
        ticks += scheduler.beginFrame(frame * 16'666'667ull).simTicks;
    CHECK(ticks >= 59);
    CHECK(ticks <= 61);
    CHECK(scheduler.timing().fixedDt == doctest::Approx(1.0 / 60.0));
    // And back at full speed.
    scheduler.setTimeScale(1.0);
    ticks = 0;
    for (engine::core::u64 frame = 121; frame <= 180; ++frame)
        ticks += scheduler.beginFrame(frame * 16'666'667ull).simTicks;
    CHECK(ticks >= 59);
    CHECK(ticks <= 61);
}

TEST_CASE("ADR 0233: a world busy past nine tenths of each second is slowed at once to what fits, down to its "
          "floor, and brought back a step a second under seven")
{
    app::TimeDilation dilation;
    dilation.configure(true, 0.5);
    constexpr engine::core::u64 Second = 1'000'000'000ull;
    engine::core::u64 now = Second;
    // One second with ticks of `tickMs` each, as many as the second holds
    // at the rate, in sixty frames.
    const auto second = [&](double tickMs) {
        const double wanted = 60.0 * dilation.rate();
        const double ran = std::min(wanted, 1000.0 / tickMs);
        for (int frame = 0; frame < 60; ++frame) {
            now += (Second + 59) / 60;
            dilation.note(now, ran * tickMs / 60.0, frame == 0 ? static_cast<engine::core::u32>(ran) : 0u);
        }
    };
    dilation.note(now, 0.0, 0);
    CHECK(dilation.rate() == 1.0);
    // Ticks of 20 ms: a second of them at sixty is 1.2 seconds. What fits
    // seven tenths of a second is 0.58 -- 0.55 in steps -- at once.
    second(20.0);
    CHECK(dilation.rate() == doctest::Approx(0.55));
    // Where it fits, it stays: 0.55 x 60 x 20 ms is 66% of a second.
    for (int at = 0; at < 10; ++at)
        second(20.0);
    CHECK(dilation.rate() == doctest::Approx(0.55));
    // Far over: no lower than the floor.
    second(100.0);
    CHECK(dilation.rate() == doctest::Approx(0.5));
    // Between the two: it stays where it is.
    // Cheap ticks again: back up a twentieth every three calm seconds.
    second(5.0);
    second(5.0);
    CHECK(dilation.rate() == doctest::Approx(0.5));
    second(5.0);
    CHECK(dilation.rate() == doctest::Approx(0.55));
    for (int at = 0; at < 60; ++at)
        second(5.0);
    CHECK(dilation.rate() == 1.0);

    // Off, it never moves.
    app::TimeDilation off;
    off.configure(false, 0.25);
    now = Second;
    off.note(now, 0.0, 0);
    for (int frame = 0; frame < 600; ++frame) {
        now += (Second + 59) / 60;
        off.note(now, 16.0, 1);
    }
    CHECK(off.rate() == 1.0);
}

TEST_CASE("a predicting client catches a stall up once, and a slow machine is clamped as before")
{
    constexpr core::u64 Tick = 16'666'667ull;
    const auto steady = [](app::FrameScheduler& scheduler, core::u64& now, int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            now += Tick;
            (void)scheduler.beginFrame(now);
        }
    };

    // Off, as on a server or alone: a 200 ms frame runs four and drops the rest.
    {
        app::FrameScheduler scheduler;
        core::u64 now = 0;
        (void)scheduler.beginFrame(now);
        steady(scheduler, now, 10);
        now += 200'000'000ull;
        const app::Frame stalled = scheduler.beginFrame(now);
        CHECK(stalled.simTicks == 4);
        CHECK(stalled.clamped);
    }
    // On: the same frame runs the twelve it owes, and drops nothing.
    {
        app::FrameScheduler scheduler;
        scheduler.setStallCatchUp(0.25);
        core::u64 now = 0;
        (void)scheduler.beginFrame(now);
        steady(scheduler, now, 10);
        now += 200'000'000ull;
        const app::Frame stalled = scheduler.beginFrame(now);
        CHECK(stalled.simTicks == 12);
        CHECK_FALSE(stalled.clamped);
        // A longer stall: a quarter of a second of it, and the rest dropped.
        steady(scheduler, now, 10);
        now += 600'000'000ull;
        const app::Frame longer = scheduler.beginFrame(now);
        CHECK(longer.simTicks == 15);
        CHECK(longer.clamped);
        // Two heavy frames in a row are a slow machine: the second is clamped.
        steady(scheduler, now, 10);
        now += 200'000'000ull;
        CHECK(scheduler.beginFrame(now).simTicks == 12);
        now += 200'000'000ull;
        const app::Frame again = scheduler.beginFrame(now);
        CHECK(again.simTicks == 4);
        CHECK(again.clamped);
        // And frames that are all heavy never burst.
        for (int frame = 0; frame < 5; ++frame) {
            now += 200'000'000ull;
            CHECK(scheduler.beginFrame(now).simTicks == 4);
        }
    }
    // A duration, not a count: at thirty ticks a second, a 200 ms stall is six.
    {
        app::FrameScheduler scheduler(app::FrameTiming{.fixedDt = 1.0 / 30.0});
        scheduler.setStallCatchUp(0.25);
        core::u64 now = 0;
        (void)scheduler.beginFrame(now);
        for (int frame = 0; frame < 10; ++frame) {
            now += 2 * Tick;
            (void)scheduler.beginFrame(now);
        }
        now += 200'000'000ull;
        const app::Frame stalled = scheduler.beginFrame(now);
        CHECK(stalled.simTicks == 6);
        CHECK_FALSE(stalled.clamped);
    }
}
