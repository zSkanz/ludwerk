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
    // 60 Hz simulation.
    f32 alpha = 0.0f;
    // True when maxCatchUpTicks clamped this frame -- simulated time was
    // dropped, and anything measuring simulation rate should know.
    bool clamped = false;
};

// **Which clock a frame reads** (D209, D213): the synthetic one -- one tick a
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

class FrameScheduler
{
public:
    explicit FrameScheduler(FrameTiming timing = {}) noexcept : timing_(timing) {}

    // Takes the current time rather than reading a clock, so a test can drive
    // the accumulator through exact sequences -- including the catch-up clamp,
    // which is otherwise reachable only by making a machine slow. Nothing about
    // the tick schedule should depend on being able to reproduce a stall.
    [[nodiscard]] Frame beginFrame(u64 nowNs) noexcept;

    // Applied at a FrameStart safe point and nowhere else
    // (`PhysicsService.FixedTimestep`, api-design.md §2.1). The accumulator is
    // deliberately NOT rescaled: it holds real time owed to the simulation, and
    // that debt is the same number of seconds whatever the tick becomes.
    void setFixedDt(f64 seconds) noexcept { timing_.fixedDt = seconds; }
    // How many ticks a frame may run to catch up; see `catchUpTicksFor`.
    void setMaxCatchUpTicks(u32 ticks) noexcept { timing_.maxCatchUpTicks = ticks > 0 ? ticks : 1; }
    // The frame loop says where a long frame's time went itself (N11), so the
    // scheduler's own line, which can only say how long, is not wanted.
    void setQuiet(bool quiet) noexcept { quiet_ = quiet; }

    // **The clock it is fed changed** (audit A9): the synthetic clock counts
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
    u64 totalTicks_ = 0;
    u64 totalFrames_ = 0;
    // When the catch-up warning was last written, so a machine that cannot
    // keep up says so every few seconds and not every frame (audit A14).
    u64 lastWarnNs_ = 0;
    bool warned_ = false;
    bool quiet_ = false;
};

} // namespace engine::app
