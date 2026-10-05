// How fast frames are made (ADR 0147, G0): the cap a frame gets, the wait
// that enforces it, and the watch that says whether the display's sync holds.
#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <vector>

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

TEST_CASE("the rates a display shows evenly are its refresh over one to four, under the game's cap")
{
    using engine::app::evenRatesFor;
    const auto rates = [](float refresh, core::u32 ceiling) {
        const engine::app::EvenRates even = evenRatesFor(refresh, ceiling);
        return std::vector<core::u32>(even.hz.begin(), even.hz.begin() + even.count);
    };
    CHECK(rates(120.0f, 0) == std::vector<core::u32>{120, 60, 40, 30});
    CHECK(rates(120.0f, 60) == std::vector<core::u32>{60, 40, 30});
    // Nothing under twenty-four: sixty over three is a slide show.
    CHECK(rates(60.0f, 0) == std::vector<core::u32>{60, 30});
    CHECK(rates(59.94f, 60) == std::vector<core::u32>{60, 30});
    CHECK(rates(90.0f, 60) == std::vector<core::u32>{45, 30});
    CHECK(rates(144.0f, 0) == std::vector<core::u32>{144, 72, 48, 36});
    // A display that will not say is taken for sixty.
    CHECK(rates(0.0f, 0) == std::vector<core::u32>{60, 30});
    // A cap under every even rate is held as it is.
    CHECK(rates(60.0f, 20) == std::vector<core::u32>{20});
}

namespace {

// A second of frames, each of `workMs`, told to the governor at `fps`: what it
// holds after.
core::u32 runFor(engine::app::RateGovernor& governor, core::u64& now, double seconds, double workMs,
                 float refresh = 120.0f, core::u32 ceiling = 60)
{
    core::u32 rate = governor.rate();
    const core::u64 end = now + static_cast<core::u64>(seconds * 1.0e9);
    while (now < end) {
        // A frame is shown no sooner than its rate allows and no sooner than
        // its work is done.
        const double period = rate != 0 ? 1000.0 / rate : workMs;
        now += static_cast<core::u64>(std::max(period, workMs) * 1.0e6);
        rate = governor.sample(now, static_cast<core::u64>(workMs * 1.0e6), refresh, ceiling);
    }
    return rate;
}

} // namespace

TEST_CASE("the governor holds the highest rate the frames fit, and steps down within seconds when they do not")
{
    engine::app::RateGovernor governor;
    core::u64 now = 1'000'000'000ull;
    // Twelve milliseconds of work fits sixty.
    CHECK(runFor(governor, now, 5.0, 12.0) == 60);
    // Eighteen does not: forty, within two seconds.
    CHECK(runFor(governor, now, 2.5, 18.0) == 40);
    // And stays there: eighteen is not sixty's with room to spare.
    CHECK(runFor(governor, now, 20.0, 18.0) == 40);
    // Twenty-eight does not fit forty either: thirty.
    CHECK(runFor(governor, now, 2.5, 28.0) == 30);
    // The lowest rate is held however slow the frames are.
    CHECK(runFor(governor, now, 5.0, 60.0) == 30);
}

TEST_CASE("the governor steps back up only once the frames have had room for three seconds, and to the rate they fit")
{
    engine::app::RateGovernor governor;
    core::u64 now = 1'000'000'000ull;
    CHECK(runFor(governor, now, 3.0, 12.0) == 60);
    CHECK(runFor(governor, now, 5.0, 28.0) == 30);
    // Twelve milliseconds again: not at once...
    CHECK(runFor(governor, now, 1.5, 12.0) == 30);
    // ...and then straight to sixty, not by way of forty.
    CHECK(runFor(governor, now, 3.0, 12.0) == 60);

    // Fifteen milliseconds fits sixty and has no fifth to spare: from forty it
    // stays at forty rather than try a rate one explosion would lose.
    CHECK(runFor(governor, now, 3.0, 18.0) == 40);
    CHECK(runFor(governor, now, 20.0, 15.0) == 40);
    // Thirteen has the room -- after six seconds this time, not three: the
    // last step up was taken back within ten.
    CHECK(runFor(governor, now, 5.0, 13.0) == 40);
    CHECK(runFor(governor, now, 2.5, 13.0) == 60);
}

TEST_CASE("one slow frame is not a rate lost, and a rate that was a step too far is tried less often")
{
    engine::app::RateGovernor governor;
    core::u64 now = 1'000'000'000ull;
    CHECK(runFor(governor, now, 3.0, 12.0) == 60);
    // A hitch: one frame of eighty milliseconds among sixty.
    now += 80'000'000ull;
    (void)governor.sample(now, 80'000'000ull, 120.0f, 60);
    CHECK(runFor(governor, now, 3.0, 12.0) == 60);

    // At the edge: light enough at forty to be let up, too heavy at sixty to
    // stay. Each time it is taken back, the next try waits twice as long.
    const auto triesIn = [&](double seconds) {
        int ups = 0;
        core::u32 before = governor.rate();
        const core::u64 end = now + static_cast<core::u64>(seconds * 1.0e9);
        while (now < end) {
            // Work that depends on the rate held: thirteen at forty, eighteen
            // at sixty -- a scene whose cost follows how often it is drawn.
            const double work = governor.rate() >= 60 ? 18.0 : 13.0;
            const double period = 1000.0 / governor.rate();
            now += static_cast<core::u64>(std::max(period, work) * 1.0e6);
            const core::u32 rate = governor.sample(now, static_cast<core::u64>(work * 1.0e6), 120.0f, 60);
            if (rate > before)
                ++ups;
            before = rate;
        }
        return ups;
    };
    CHECK(runFor(governor, now, 3.0, 18.0) == 40);
    const int first = triesIn(30.0);
    const int later = triesIn(30.0);
    CHECK(first >= 2);
    CHECK(later < first);
}

TEST_CASE("another display mode starts the governor over, from the highest rate")
{
    engine::app::RateGovernor governor;
    core::u64 now = 1'000'000'000ull;
    CHECK(runFor(governor, now, 4.0, 28.0) == 30);
    // The display went to sixty: the choices are sixty and thirty now.
    CHECK(runFor(governor, now, 0.5, 12.0, 60.0f) == 60);
    governor.reset();
    CHECK(governor.rate() == 0);
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
