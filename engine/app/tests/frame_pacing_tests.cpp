// How fast frames are made (ADR 0147, G0): the cap a frame gets, the wait
// that enforces it, and the watch that says whether the display's sync holds.
#include <array>
#include <doctest/doctest.h>

#include "engine/app/frame_pacing.h"

using namespace engine;
using engine::app::FrameLimiter;
using engine::app::FramePacing;
using engine::app::FrameWindowState;
using engine::app::LoadingCurtain;
using engine::app::SyncWatch;

namespace {

constexpr core::u64 Millisecond = 1'000'000ull;

} // namespace

TEST_CASE("in front, with the display's sync holding, a frame has no cap but the game's own")
{
    FramePacing pacing;
    FrameWindowState window;
    window.refreshRate = 144.0f;
    // The defaults: sync on, no cap -- the present waits for the display.
    CHECK(app::frameCapFor(pacing, window) == 0u);
    // A game that asks for thirty gets thirty, under the sync.
    pacing.maxFrameRate = 30;
    CHECK(app::frameCapFor(pacing, window) == 30u);
    // Sync off: the cap alone, and none is none.
    pacing.vsync = false;
    CHECK(app::frameCapFor(pacing, window) == 30u);
    pacing.maxFrameRate = 0;
    CHECK(app::frameCapFor(pacing, window) == 0u);
}

TEST_CASE("a window nobody is looking at is drawn at the background rate")
{
    // The owner's editor at two thousand frames a second: unfocused or
    // minimised, it had nothing to wait on.
    FramePacing pacing;
    FrameWindowState window;
    window.refreshRate = 60.0f;
    window.focused = false;
    CHECK(app::frameCapFor(pacing, window) == 10u);
    window.focused = true;
    window.minimized = true;
    window.presented = false;
    CHECK(app::frameCapFor(pacing, window) == 10u);
    // A game capped lower than the background rate keeps its own cap.
    pacing.maxFrameRate = 5;
    CHECK(app::frameCapFor(pacing, window) == 5u);
    // And one that turns the throttle off is paced as in front -- by the
    // refresh, for a window with no backbuffer the sync cannot hold.
    pacing.maxFrameRate = 0;
    pacing.backgroundFrameRate = 0;
    CHECK(app::frameCapFor(pacing, window) == 60u);
}

TEST_CASE("where the sync is asked for and does not hold, the refresh is the cap")
{
    FramePacing pacing;
    FrameWindowState window;
    window.refreshRate = 59.94f;
    // A frame with no backbuffer: covered, or between two swapchains.
    window.presented = false;
    CHECK(app::frameCapFor(pacing, window) == 60u);
    // A driver told to ignore the sync.
    window.presented = true;
    window.syncHeld = false;
    CHECK(app::frameCapFor(pacing, window) == 60u);
    // A display that will not say its rate is taken for sixty.
    window.refreshRate = 0.0f;
    CHECK(app::frameCapFor(pacing, window) == 60u);
    // With sync off nobody asked for the refresh.
    pacing.vsync = false;
    CHECK(app::frameCapFor(pacing, window) == 0u);
}

TEST_CASE("the limiter holds frames to a grid, whatever each one cost")
{
    FrameLimiter limiter;
    const core::u64 period = 1'000'000'000ull / 60;
    core::u64 now = 1000 * Millisecond;
    // The first frame starts the grid and waits for nothing.
    CHECK(limiter.waitNs(now, 60) == 0u);
    core::u64 deadline = now + period;
    // Frames of 5, 9 and 2 ms each end on the next line of the grid.
    for (const core::u64 cost : std::array<core::u64, 3>{5 * Millisecond, 9 * Millisecond, 2 * Millisecond}) {
        now += cost;
        const core::u64 wait = limiter.waitNs(now, 60);
        CHECK(now + wait == deadline);
        now += wait;
        deadline += period;
    }
    // A frame a little late waits for nothing and keeps the grid.
    now += period + 3 * Millisecond;
    CHECK(limiter.waitNs(now, 60) == 0u);
    deadline += period;
    now += 4 * Millisecond;
    CHECK(now + limiter.waitNs(now, 60) == deadline);
}

TEST_CASE("a hitch is not owed back, and no cap is no wait")
{
    FrameLimiter limiter;
    const core::u64 period = 1'000'000'000ull / 60;
    core::u64 now = 1000 * Millisecond;
    (void)limiter.waitNs(now, 60);
    // Half a second lost: the next frame waits a period from here, not none
    // for the thirty it missed.
    now += 500 * Millisecond;
    CHECK(limiter.waitNs(now, 60) == 0u);
    now += 1 * Millisecond;
    CHECK(limiter.waitNs(now, 60) == period - 1 * Millisecond);

    CHECK(limiter.waitNs(now, 0) == 0u);
    // A cap that changes starts its own grid.
    now += 2 * Millisecond;
    CHECK(limiter.waitNs(now, 30) == 0u);
    now += 2 * Millisecond;
    CHECK(limiter.waitNs(now, 30) == 1'000'000'000ull / 30 - 2 * Millisecond);
}

TEST_CASE("a throttled frame may run the ticks it owes")
{
    // Ten frames a second at sixty ticks: six a frame, and one over.
    CHECK(app::catchUpTicksFor(4, 1.0 / 60.0, 10) == 7u);
    // In front at sixty or a hundred and forty-four, the usual four.
    CHECK(app::catchUpTicksFor(4, 1.0 / 60.0, 60) == 4u);
    CHECK(app::catchUpTicksFor(4, 1.0 / 60.0, 144) == 4u);
    CHECK(app::catchUpTicksFor(4, 1.0 / 60.0, 0) == 4u);
    // Never more than a second's worth and one.
    CHECK(app::catchUpTicksFor(4, 1.0 / 240.0, 1) == 65u);
}

TEST_CASE("the watch says the sync is not holding only after frames it could not have let through")
{
    SyncWatch watch;
    CHECK(watch.held());
    const core::u64 period = 1'000'000'000ull / 60;
    // A second of frames at the refresh: held.
    for (int frame = 0; frame < 60; ++frame)
        watch.sample(period, 60.0f);
    CHECK(watch.held());
    // Twenty-nine frames of half a millisecond are a burst after a hitch.
    for (int frame = 0; frame < 29; ++frame)
        watch.sample(Millisecond / 2, 60.0f);
    CHECK(watch.held());
    // Thirty are a driver that does not wait.
    watch.sample(Millisecond / 2, 60.0f);
    CHECK_FALSE(watch.held());
    // And one frame the display held says it is back.
    watch.sample(period, 60.0f);
    CHECK(watch.held());
    // A display with no rate to hold to is never said to have failed.
    for (int frame = 0; frame < 100; ++frame)
        watch.sample(Millisecond / 2, 0.0f);
    CHECK(watch.held());
}

TEST_CASE("NA4: a window in the background in a networked session runs at the simulation's rate")
{
    const app::FramePacing pacing{.vsync = true, .maxFrameRate = 0, .backgroundFrameRate = 10};
    app::FrameWindowState window;
    window.focused = false;
    CHECK(app::frameCapFor(pacing, window) == 10u);
    window.networked = true;
    CHECK(app::frameCapFor(pacing, window) == 60u);
    window.minimized = true;
    CHECK(app::frameCapFor(pacing, window) == 60u);
    // A cap the game asked for still stands, and a higher background rate is
    // the game's to keep.
    const app::FramePacing capped{.vsync = true, .maxFrameRate = 30, .backgroundFrameRate = 10};
    CHECK(app::frameCapFor(capped, window) == 30u);
    const app::FramePacing generous{.vsync = true, .maxFrameRate = 0, .backgroundFrameRate = 120};
    CHECK(app::frameCapFor(generous, window) == 120u);
}

TEST_CASE("the loading curtain lifts once the loaders are idle, the ground is meshed and no script holds it")
{
    // ADR 0159: a scene whose scripts build its terrain is not shown until
    // the ground round the camera is drawn as it will be.
    LoadingCurtain curtain;
    CHECK_FALSE(curtain.up());
    curtain.raise();
    REQUIRE(curtain.up());
    core::u64 now = 1;
    const auto frame = [&](bool idle, bool meshed, core::u32 holds) {
        now += 16 * Millisecond;
        return curtain.update({.nowNs = now, .loadersIdle = idle, .groundMeshed = meshed, .holds = holds});
    };
    // The meshes are in; the ground is not.
    for (int at = 0; at < 30; ++at)
        CHECK(frame(true, false, 0) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.up());
    // A script holds it, with everything in.
    for (int at = 0; at < 30; ++at)
        CHECK(frame(true, true, 1) == LoadingCurtain::Lift::Kept);
    // Released: three frames running, then up.
    CHECK(frame(true, true, 0) == LoadingCurtain::Lift::Kept);
    CHECK(frame(true, true, 0) == LoadingCurtain::Lift::Kept);
    CHECK(frame(true, true, 0) == LoadingCurtain::Lift::Ready);
    CHECK_FALSE(curtain.up());
    // Down, it stays down.
    CHECK(frame(false, false, 2) == LoadingCurtain::Lift::Kept);
    CHECK_FALSE(curtain.up());
}

TEST_CASE("the loading curtain gives up after ten seconds, or a minute while a script holds it")
{
    LoadingCurtain curtain;
    curtain.raise();
    // Never meshed, nothing held: ten seconds.
    CHECK(curtain.update({.nowNs = 1, .groundMeshed = false}) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.update({.nowNs = 1 + 9'000 * Millisecond, .groundMeshed = false}) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.update({.nowNs = 1 + 10'001 * Millisecond, .groundMeshed = false}) == LoadingCurtain::Lift::TimedOut);
    CHECK_FALSE(curtain.up());
    // Held: a minute.
    curtain.raise();
    CHECK(curtain.update({.nowNs = 100, .holds = 1}) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.update({.nowNs = 100 + 30'000 * Millisecond, .holds = 1}) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.update({.nowNs = 100 + 60'001 * Millisecond, .holds = 1}) == LoadingCurtain::Lift::TimedOut);
}
