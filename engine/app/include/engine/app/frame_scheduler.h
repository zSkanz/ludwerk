// The frame pipeline's two clocks (architecture.md §3).
//
// SimClock is a fixed tick -- default 1/60, configurable later through
// PhysicsService.FixedTimestep -- and every deterministic thing in the engine
// lives inside one. RenderClock is variable and only rendering sees it, which
// interpolates between the last two sim states by `alpha`.
//
// This is the shape a rollback-capable engine needs and it is deliberately the
// starting point rather than something retrofitted (ADR 0016).
#pragma once

#include "engine/core/types.h"

namespace engine::app {

using core::f32;
using core::f64;
using core::u32;
using core::u64;

struct FrameTiming
{
    f64 fixedDt = 1.0 / 60.0;

    // A frame that owes more than this many ticks stops trying to catch up:
    // the accumulator is clamped and the time is dropped. Without it, a machine
    // that cannot keep up runs more ticks, which makes it slower, which owes it
    // more ticks -- the spiral of death. Dropping simulated time is the lesser
    // evil and the one every fixed-tick engine picks.
    u32 maxCatchUpTicks = 4;

    // **A stall caught up, once** (a predicting client in a match): a frame
    // that owes more than `maxCatchUpTicks` may run up to this many seconds of
    // them, when the frame before it was neither clamped nor such a burst. A
    // client that dropped the rest of a 200 ms stall fell behind its own
    // prediction, and the authority's answers moved its car 1.4 m in a tick.
    // A duration, so a project at another tick rate is covered alike; nought
    // is none. Two heavy frames in a row are a slow machine, not a stall, and
    // the second is clamped as before: no spiral.
    f64 stallCatchUpSeconds = 0.0;
};

struct Frame
{
    u64 index = 0;
    // Seconds since the previous frame, on the wall clock.
    f64 renderDt = 0.0;
    // Fixed steps this frame owes the simulation, already clamped.
    u32 simTicks = 0;
    // Where rendering sits between the last tick and the next, in [0, 1).
    // Passed to render::extract so a 144 Hz display shows smooth motion from a
    // 60 Hz simulation. Held at an end of that range while a tick is lent or
    // owed across its edge (`TickSlack`).
    f32 alpha = 0.0f;
    // True when maxCatchUpTicks clamped this frame -- simulated time was
    // dropped, and anything measuring simulation rate should know.
    bool clamped = false;
};

// **Which clock a frame reads**: the synthetic one -- one tick a
// frame, as fast as the machine goes -- for a headless run that exists to
// produce the same pixels on any runner, and the real one for everything else.
//
// **Once real, real for good.** A headless client that had joined a match and
// left it used to go back to the synthetic clock: its frames ran flat out,
// ticking hundreds of times a second, and the first of them measured a clock
// that restarted from zero. A process that has had real time -- a network
// session, a dev session, a window -- has somebody on the other end of it, and
// a world that suddenly ran at a thousand ticks a second is not what either of
// them saw.
class FrameClock
{
public:
    [[nodiscard]] bool synthetic(bool headless, bool devSession, bool networked) noexcept
    {
        const bool before = m_real;
        m_real = m_real || !headless || devSession || networked;
        m_switched = m_asked && before != m_real;
        m_asked = true;
        return !m_real;
    }

    // Whether the last `synthetic` changed the answer: the frame the scheduler
    // must be told the clock it is fed is another clock (`rebase`).
    [[nodiscard]] bool switched() const noexcept { return m_switched; }

private:
    bool m_real = false;
    bool m_asked = false;
    bool m_switched = false;
};

// **An overloaded world slows its clock instead of stalling** (ADR 0233).
//
// Fed each frame with how much of it the simulation took and in how many
// ticks; once a second at most it moves the rate. **Down at once to what
// fits** while the simulation took more than 90% of the second: the rate at
// which ticks of the cost they had take 70% of a second, a step at least,
// down to the floor. **Up a step of 5%** once three seconds in a row took
// less than half. A world that stepped down a twentieth a second spent ten
// seconds behind its ticks on the way to the rate it could keep -- every
// player's prediction corrected all the while -- where one step is one
// second.
//
// **Changed rarely, because a change costs.** The rate reaches a replica a
// round trip after the authority runs at it, and for that moment the two
// tick at different rates: each of the replica's characters was corrected
// once at every change. A tick's cost varies by a fifth from one second to
// the next, and a band of 70% to 90% around a target of 80% moved the rate
// every few seconds.
class TimeDilation
{
public:
    void configure(bool on, f64 floor, f64 ticksPerSecond = 60.0) noexcept;
    void note(u64 nowNs, f64 simulatedMs, u32 ticks) noexcept;
    [[nodiscard]] bool on() const noexcept { return on_; }
    [[nodiscard]] f64 rate() const noexcept { return rate_; }
    // The share of the last whole second the simulation took, 0 to 1.
    [[nodiscard]] f64 busy() const noexcept { return busy_; }

    static constexpr f64 Step = 0.05;
    static constexpr f64 SlowAbove = 0.9;
    static constexpr f64 SpeedBelow = 0.5;
    static constexpr u32 CalmSeconds = 3;
    // What a rate stepped down to leaves the simulation of each second.
    static constexpr f64 FitTo = 0.7;

private:
    bool on_ = false;
    f64 floor_ = 0.25;
    f64 ticksPerSecond_ = 60.0;
    f64 rate_ = 1.0;
    f64 busy_ = 0.0;
    u64 windowStartNs_ = 0;
    f64 windowSimulatedMs_ = 0.0;
    u64 windowTicks_ = 0;
    u32 calm_ = 0;
};

class FrameScheduler
{
public:
    explicit FrameScheduler(FrameTiming timing = {}) noexcept : timing_(timing) {}

    // Takes the current time rather than reading a clock, so a test can drive
    // the accumulator through exact sequences -- including the catch-up clamp,
    // which is otherwise reachable only by making a machine slow. Nothing about
    // the tick schedule should depend on being able to reproduce a stall.
    [[nodiscard]] Frame beginFrame(u64 nowNs) noexcept;

    // **How far over a tick's edge a frame may be and still be counted as the
    // frames before it were**, as a part of a tick. Frames that keep
    // the simulation's rate -- a display at sixty and ticks at sixty, or
    // thirty and two a frame -- begin a little early or late each time, and
    // where one begins just as a tick comes due, the early frame owed none
    // and the late one two: twice the simulation in one frame of every pair.
    // So the count the last two frames agreed on is kept while the time owed
    // is within this of saying otherwise: a tick lent, or one held back, and
    // made good by the frames after. A rate that is really another -- a
    // display at fifty-nine -- passes the slack and is counted as it is; no
    // time is made or lost. An eighth of a tick is two milliseconds at sixty.
    static constexpr f64 TickSlack = 0.125;

    // Applied at a FrameStart safe point and nowhere else
    // (`PhysicsService.FixedTimestep`, api-design.md §2.1). The accumulator is
    // deliberately NOT rescaled: it holds real time owed to the simulation, and
    // that debt is the same number of seconds whatever the tick becomes.
    void setFixedDt(f64 seconds) noexcept { timing_.fixedDt = seconds; }
    // **How fast the simulation's clock runs against the wall's** (ADR
    // 0233): 1 at full speed, less while an overloaded world is slowed. The
    // tick is the same; fewer of them are owed a second.
    void setTimeScale(f64 scale) noexcept { timeScale_ = scale < 0.01 ? 0.01 : (scale > 1.0 ? 1.0 : scale); }
    [[nodiscard]] f64 timeScale() const noexcept { return timeScale_; }
    // How many ticks a frame may run to catch up; see `catchUpTicksFor`.
    void setMaxCatchUpTicks(u32 ticks) noexcept { timing_.maxCatchUpTicks = ticks > 0 ? ticks : 1; }
    // See `FrameTiming::stallCatchUpSeconds`.
    void setStallCatchUp(f64 seconds) noexcept { timing_.stallCatchUpSeconds = seconds > 0.0 ? seconds : 0.0; }
    // The frame loop says where a long frame's time went itself, so the
    // scheduler's own line, which can only say how long, is not wanted.
    void setQuiet(bool quiet) noexcept { quiet_ = quiet; }

    // **The clock it is fed changed**: the synthetic clock counts
    // from zero and the real one from boot, so the first real frame after a
    // headless run joined a match measured the machine's uptime -- "Frame took
    // 5391574 ms". The next frame measures from `nowNs` instead.
    void rebase(u64 nowNs) noexcept
    {
        if (started_)
            lastNs_ = nowNs;
    }

    // How long until the accumulator owes the next tick, at `nowNs`. Zero when
    // one is already owed.
    [[nodiscard]] u64 nanosUntilNextTick(u64 nowNs) const noexcept;

    [[nodiscard]] u64 totalTicks() const noexcept { return totalTicks_; }
    [[nodiscard]] u64 totalFrames() const noexcept { return totalFrames_; }
    [[nodiscard]] const FrameTiming& timing() const noexcept { return timing_; }

private:
    FrameTiming timing_;
    u64 lastNs_ = 0;
    bool started_ = false;
    f64 accumulator_ = 0.0;
    f64 timeScale_ = 1.0;
    // The ticks of the last frame, and the count it shared with the one
    // before it; none while they differed.
    u32 lastTicks_ = 0;
    // Whether the last frame was clamped or caught a stall up: the next may not.
    bool lastHeavy_ = false;
    u32 steadyTicks_ = 0;
    u64 totalTicks_ = 0;
    u64 totalFrames_ = 0;
    // When the catch-up warning was last written, so a machine that cannot
    // keep up says so every few seconds and not every frame.
    u64 lastWarnNs_ = 0;
    bool warned_ = false;
    bool quiet_ = false;
};

} // namespace engine::app
