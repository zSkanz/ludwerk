// How fast frames are made (ADR 0147, G0): the cap a frame gets, the wait
// that enforces it, and the watch that says whether the display's sync holds.
#include <algorithm>
#include <array>
#include <cmath>
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

// **A device, as the governor sees one**: a display that shows a frame at its
// first refresh once the frame's work is done, and no sooner than the rate
// held allows.
struct Device
{
    float refresh = 120.0f;
    core::u32 ceiling = 60;
    engine::app::RateGovernor governor{};
    core::u64 now = 1'000'000'000ull;

    core::u32 frame(double workMs, double cpuMs = 6.0, double simMs = 0.0)
    {
        const core::u32 held = governor.rate();
        const double refreshMs = 1000.0 / static_cast<double>(refresh);
        const double placeMs = held != 0 ? 1000.0 / held : refreshMs;
        const double intervalMs = std::ceil(std::max(placeMs, workMs) / refreshMs - 1.0e-6) * refreshMs;
        now += static_cast<core::u64>(intervalMs * 1.0e6);
        const engine::app::FrameCost cost{.intervalNs = static_cast<core::u64>(intervalMs * 1.0e6),
                                          .workNs = static_cast<core::u64>(workMs * 1.0e6),
                                          .cpuNs = static_cast<core::u64>(cpuMs * 1.0e6),
                                          .simNs = static_cast<core::u64>(simMs * 1.0e6)};
        return governor.sample(now, cost, refresh, ceiling);
    }

    // `seconds` of frames of one cost: the rate held after them.
    core::u32 run(double seconds, double workMs, double cpuMs = 6.0, double simMs = 0.0)
    {
        core::u32 rate = governor.rate();
        const core::u64 end = now + static_cast<core::u64>(seconds * 1.0e9);
        while (now < end)
            rate = frame(workMs, cpuMs, simMs);
        return rate;
    }

    // `seconds` at a display's own rate with one frame in `every` shown for
    // two refreshes: the lowest rate held in them.
    core::u32 runDoubling(double seconds, int every)
    {
        core::u32 lowest = governor.rate();
        const core::u64 end = now + static_cast<core::u64>(seconds * 1.0e9);
        for (int index = 0; now < end; ++index)
            lowest = std::min(lowest, frame(index % every == 0 ? 20.0 : 10.0));
        return lowest;
    }
};

} // namespace

TEST_CASE("D558: a launch's long frames and a game that holds its rate do not put a phone at thirty for good")
{
    // The owner's phone, as its log has it: a display at sixty, a cap of
    // sixty; two frames of a scene's load, 662 and 1595 ms; then a game that
    // holds fifty-nine, one frame in twenty shown for two refreshes. The
    // governor went to thirty three seconds in and never came back.
    engine::app::RateGovernor governor;
    core::u64 now = 1'000'000'000ull;
    const auto frame = [&](double intervalMs) {
        const auto ns = static_cast<core::u64>(intervalMs * 1.0e6);
        now += ns;
        return governor.sample(now, engine::app::FrameCost{.intervalNs = ns, .workNs = ns, .cpuNs = 10'000'000ull},
                               60.0f, 60);
    };
    (void)frame(16.6);
    (void)frame(662.0);
    for (int index = 0; index < 20; ++index)
        (void)frame(index % 3 == 0 ? 33.3 : 16.6);
    (void)frame(1595.0);
    core::u32 lowest = 60;
    for (int index = 0; index < 60 * 60; ++index) {
        const core::u32 rate = frame(index % 20 == 0 ? 33.3 : 16.6);
        // Judged only once the launch is behind it.
        if (index > 5 * 60)
            lowest = std::min(lowest, rate);
    }
    CHECK(governor.rate() == 60);
    CHECK(lowest == 60);
}

TEST_CASE("D558: nothing is judged in a start's first seconds, and a hitch is left out with the second it fell in")
{
    // Forty milliseconds a frame from the first one: for the four seconds a
    // start is given, that is a start.
    Device slow;
    CHECK(slow.run(3.5, 40.0) == 60);
    // And after them it is the game.
    CHECK(slow.run(4.0, 40.0) < 60);

    Device device;
    CHECK(device.run(6.0, 12.0) == 60);
    // A scene arriving: a third of a second in one frame, and the frames
    // after it slow while what it brought is made.
    (void)device.frame(300.0);
    CHECK(device.run(0.9, 30.0) == 60);
    CHECK(device.run(3.0, 12.0) == 60);
    // Half a second of slow frames with no hitch before them is not a rate
    // lost either: no second of it was late twice running.
    CHECK(device.run(0.5, 30.0) == 60);
    CHECK(device.run(3.0, 12.0) == 60);

    // A curtain lifting is a start again.
    CHECK(device.run(3.0, 18.0) == 40);
    device.governor.reset();
    CHECK(device.governor.rate() == 0);
    CHECK(device.run(3.5, 40.0) == 60);
}

TEST_CASE("the governor gives a rate up on a sustained miss only, a step at a time, and says why in numbers")
{
    Device device;
    // Twelve milliseconds of work fits sixty.
    CHECK(device.run(6.0, 12.0) == 60);
    CHECK(device.governor.lastStep().from == 0);
    // Eighteen does not: forty, once two seconds running have said so.
    CHECK(device.run(1.5, 18.0, 16.0) == 60);
    CHECK(device.run(1.5, 18.0, 16.0) == 40);
    const engine::app::RateGovernor::Step down = device.governor.lastStep();
    CHECK(down.from == 60);
    CHECK(down.to == 40);
    CHECK(down.frames >= 40);
    CHECK(down.lateFrames == down.frames);
    CHECK(down.meanCpuNs == 16'000'000ull);
    // And stays there: sixteen milliseconds on the CPU is not sixty's.
    CHECK(device.run(30.0, 18.0, 16.0) == 40);
    // Twenty-eight does not fit forty either: thirty.
    CHECK(device.run(3.0, 28.0, 16.0) == 30);
    // The lowest rate is held however slow the frames are.
    CHECK(device.run(5.0, 60.0, 16.0) == 30);
}

TEST_CASE("D558: a phone that holds fifty-nine, and dips to fifty while its GPU's clock is down, stays at sixty")
{
    Device phone;
    phone.refresh = 60.0f;
    CHECK(phone.run(5.0, 10.0) == 60);
    // One frame in twenty shown twice: fifty-seven.
    CHECK(phone.runDoubling(10.0, 20) == 60);
    // One in five, for ten seconds: fifty. A warm phone does this.
    CHECK(phone.runDoubling(10.0, 5) == 60);
    CHECK(phone.runDoubling(10.0, 20) == 60);
    // One in two is forty: that is not sixty, and thirty shown evenly is the
    // better game.
    (void)phone.runDoubling(3.0, 2);
    CHECK(phone.governor.rate() == 30);
    CHECK(phone.governor.lastStep().from == 60);
    CHECK(phone.governor.lastStep().lateFrames * 10 > phone.governor.lastStep().frames * 4);
}

TEST_CASE("D558: the rate above is tried once the CPU's part fits it, and one taken back is tried less often")
{
    // A scene the GPU cannot draw sixty times a second, on a CPU that could:
    // eighteen milliseconds a frame, eight of them the CPU's. What a frame
    // held at forty takes says nothing of whether sixty would fit, so the
    // governor tries, and finds out.
    Device device;
    CHECK(device.run(7.0, 18.0, 8.0) == 40);

    std::vector<double> tries;
    core::u32 before = device.governor.rate();
    const core::u64 start = device.now;
    double longestAtSixty = 0.0;
    core::u64 upAt = 0;
    while (device.now < start + 150'000'000'000ull) {
        const core::u32 rate = device.frame(18.0, 8.0);
        if (rate > before) {
            tries.push_back(static_cast<double>(device.now - start) / 1.0e9);
            upAt = device.now;
            CHECK(device.governor.lastStep().from == 40);
            CHECK(device.governor.lastStep().to == 60);
            CHECK(device.governor.lastStep().meanCpuNs == 8'000'000ull);
        }
        else if (rate < before) {
            longestAtSixty = std::max(longestAtSixty, static_cast<double>(device.now - upAt) / 1.0e9);
        }
        before = rate;
    }
    // Five seconds, then ten after it was taken back, then twenty, forty, and
    // a minute from there: five tries in two and a half minutes, each given up
    // in a little over two seconds.
    REQUIRE(tries.size() == 5);
    CHECK(tries[0] < 6.0);
    for (std::size_t index = 2; index < tries.size(); ++index)
        CHECK(tries[index] - tries[index - 1] > (tries[index - 1] - tries[index - 2]) * 1.2);
    CHECK(longestAtSixty < 3.0);

    // The scene lightens: the next try stands...
    CHECK(device.run(100.0, 12.0, 8.0) == 60);
    // ...and having stood half a minute, it has earned the first wait back:
    // given up again, sixty is tried five seconds on, not a minute.
    CHECK(device.run(3.0, 18.0, 8.0) == 40);
    const core::u64 givenUp = device.now;
    while (device.governor.rate() == 40 && device.now < givenUp + 90'000'000'000ull)
        (void)device.frame(18.0, 8.0);
    CHECK(device.governor.rate() == 60);
    CHECK(device.now - givenUp < 6'000'000'000ull);

    // A CPU that does not fit the rate above is not sent to try it.
    Device bound;
    CHECK(bound.run(7.0, 18.0, 16.0) == 40);
    CHECK(bound.run(60.0, 18.0, 16.0) == 40);
}

TEST_CASE("D559: a frame's simulation is weighed at the rate above, where the frame runs half the ticks")
{
    // A display at sixty: the rates are sixty and thirty. A stretch too heavy
    // for sixty puts the phone at thirty, where every frame runs two of the
    // simulation's ticks: sixteen milliseconds on the CPU, ten of them the
    // ticks'. At sixty a frame runs one -- six and five, eleven -- so sixty
    // is to be tried. It was not: sixteen does not fit a sixtieth of a second
    // with a tenth to spare, and the phone drew thirty for the rest of the run.
    Device device;
    device.refresh = 60.0f;
    CHECK(device.run(7.0, 24.0, 16.0, 10.0) == 30);
    const core::u64 start = device.now;
    while (device.governor.rate() == 30 && device.now < start + 20'000'000'000ull)
        (void)device.frame(24.0, 16.0, 10.0);
    CHECK(device.governor.rate() == 60);
    CHECK(device.now - start < 6'000'000'000ull);
    CHECK(device.governor.lastStep().from == 30);
    CHECK(device.governor.lastStep().meanCpuNs == 16'000'000ull);
    CHECK(device.governor.lastStep().estimateNs == 11'000'000ull);

    // What is not the simulation's is the frame's at any rate: sixteen
    // milliseconds of which one is the ticks' is fifteen and a half at sixty.
    Device bound;
    bound.refresh = 60.0f;
    CHECK(bound.run(7.0, 24.0, 16.0, 1.0) == 30);
    CHECK(bound.run(60.0, 24.0, 16.0, 1.0) == 30);
}

TEST_CASE("D560: at the display's own rate a governed frame is paced as one nobody governs")
{
    using engine::app::governedPaceFor;
    // A phone's display at sixty and its game capped at sixty: the cap is
    // waited out at the frame's end, as with the governor off, and nothing
    // is held at the present.
    CHECK(governedPaceFor(60, 60.0f, 60).holdHz == 0);
    CHECK(governedPaceFor(60, 60.0f, 60).endHz == 60);
    // With no cap there is the display alone.
    CHECK(governedPaceFor(120, 120.0f, 0).holdHz == 0);
    CHECK(governedPaceFor(120, 120.0f, 0).endHz == 0);
    // Under the display's rate the frame is held at its present, and its end
    // waits for nothing: two waits would be two grids.
    CHECK(governedPaceFor(30, 60.0f, 60).holdHz == 30);
    CHECK(governedPaceFor(30, 60.0f, 60).endHz == 0);
    CHECK(governedPaceFor(60, 120.0f, 60).holdHz == 60);
    CHECK(governedPaceFor(60, 120.0f, 60).endHz == 0);
    // A display that does not say its rate is not one to leave the pacing to.
    CHECK(governedPaceFor(60, 0.0f, 60).holdHz == 60);
    CHECK(governedPaceFor(60, 0.0f, 60).endHz == 0);
}

TEST_CASE("another display mode starts the governor over, from the highest rate")
{
    Device device;
    CHECK(device.run(10.0, 28.0) == 30);
    // The display went to sixty: the choices are sixty and thirty now.
    device.refresh = 60.0f;
    CHECK(device.run(0.5, 12.0) == 60);
    device.governor.reset();
    CHECK(device.governor.rate() == 0);
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

TEST_CASE("the curtain is settling from the first frame with nothing to wait for until it lifts (ADR 0176)")
{
    LoadingCurtain curtain;
    CHECK_FALSE(curtain.settling());
    curtain.raise();
    CHECK_FALSE(curtain.settling());

    core::u64 now = Millisecond;
    const auto frame = [&](bool idle) {
        now += 16 * Millisecond;
        return curtain.update({.nowNs = now, .loadersIdle = idle, .groundMeshed = true, .holds = 0});
    };
    // Still loading: up, and not settling.
    CHECK(frame(false) == LoadingCurtain::Lift::Kept);
    CHECK_FALSE(curtain.settling());
    // Idle: the world is drawn behind it from here.
    CHECK(frame(true) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.settling());
    // Something arrives again: back to waiting.
    CHECK(frame(false) == LoadingCurtain::Lift::Kept);
    CHECK_FALSE(curtain.settling());
    // Three settled frames and it lifts; the two before the lift are the ones
    // the world is drawn in.
    CHECK(frame(true) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.settling());
    CHECK(frame(true) == LoadingCurtain::Lift::Kept);
    CHECK(curtain.settling());
    CHECK(frame(true) == LoadingCurtain::Lift::Ready);
    CHECK_FALSE(curtain.up());
    CHECK_FALSE(curtain.settling());
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
