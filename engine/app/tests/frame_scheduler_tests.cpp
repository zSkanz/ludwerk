// The fixed-tick accumulator and the choice of clock it is fed.
#include <doctest/doctest.h>

#include "engine/app/frame_scheduler.h"

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
