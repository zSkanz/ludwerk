#include "engine/app/frame_scheduler.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/log.h"
#include "engine/core/text_key.h"

namespace engine::app {

Frame FrameScheduler::beginFrame(u64 nowNs) noexcept
{
    Frame frame;
    frame.index = totalFrames_;

    if (!started_) {
        // The first frame has no previous one to measure from. Reporting zero
        // beats reporting however long the process took to start, which is what
        // a naive `now - 0` would give and what would make the first frame look
        // like a hitch in every profile.
        started_ = true;
        lastNs_ = nowNs;
        ++totalFrames_;
        return frame;
    }

    // Monotonic clock, so this cannot go backwards -- but a paused debugger can
    // make it enormous, and treating that as real time would burn the catch-up
    // budget on the first frame after every breakpoint.
    //
    // **Two clocks can**, and one did: a headless client that left a
    // match went from the real clock back to the synthetic one, which counts
    // from zero, and `now - last` wrapped to eighteen trillion milliseconds.
    // Time that went backwards is no time at all.
    const u64 elapsedNs = nowNs > lastNs_ ? nowNs - lastNs_ : 0;
    lastNs_ = nowNs;

    frame.renderDt = static_cast<f64>(elapsedNs) / 1'000'000'000.0;
    accumulator_ += frame.renderDt * timeScale_;

    f64 maxAccumulated = timing_.fixedDt * static_cast<f64>(timing_.maxCatchUpTicks);
    // **A stall, caught up once** (`stallCatchUpSeconds`).
    const bool owesMore = accumulator_ > maxAccumulated;
    if (owesMore && timing_.stallCatchUpSeconds > maxAccumulated && !lastHeavy_)
        maxAccumulated = timing_.stallCatchUpSeconds;
    if (accumulator_ > maxAccumulated) {
        frame.clamped = true;
        accumulator_ = maxAccumulated;
    }
    lastHeavy_ = owesMore;

    while (accumulator_ >= timing_.fixedDt) {
        accumulator_ -= timing_.fixedDt;
        ++frame.simTicks;
    }

    // **The count a run of frames has kept is kept across a tick's edge**
    // (`TickSlack`): one fewer than the frames before with nearly a
    // tick owed is lent the rest of it; one more with hardly anything over
    // holds it back. Not a frame that was clamped, and not where the frames
    // have no count of their own.
    const f64 slack = timing_.fixedDt * TickSlack;
    if (!frame.clamped && steadyTicks_ > 0) {
        if (frame.simTicks + 1 == steadyTicks_ && accumulator_ >= timing_.fixedDt - slack) {
            accumulator_ -= timing_.fixedDt;
            ++frame.simTicks;
        }
        else if (frame.simTicks == steadyTicks_ + 1 && accumulator_ < slack) {
            accumulator_ += timing_.fixedDt;
            --frame.simTicks;
        }
    }
    steadyTicks_ = frame.simTicks == lastTicks_ ? frame.simTicks : 0;
    lastTicks_ = frame.simTicks;

    totalTicks_ += frame.simTicks;
    // Between the two ticks it is drawn between, and at an end of them while
    // a tick is lent or held back.
    constexpr f64 JustUnderOne = 0.99999;
    frame.alpha = static_cast<f32>(std::clamp(accumulator_ / timing_.fixedDt, 0.0, JustUnderOne));

    constexpr u64 WarnEveryNs = 5'000'000'000ull;
    if (frame.clamped && !quiet_ && (!warned_ || nowNs - lastWarnNs_ >= WarnEveryNs)) {
        warned_ = true;
        lastWarnNs_ = nowNs;
        const std::array<core::I18nArg, 2> args{core::I18nArg{"ticks", static_cast<core::i64>(frame.simTicks)},
                                                core::I18nArg{"ms", frame.renderDt * 1000.0}};
        core::log(core::LogLevel::Warn, ENG_TR("engine.frame.warn.catch_up_clamped"), args);
    }

    ++totalFrames_;
    return frame;
}

void TimeDilation::configure(bool on, f64 floor, f64 ticksPerSecond) noexcept
{
    on_ = on;
    ticksPerSecond_ = ticksPerSecond > 0.0 ? ticksPerSecond : 60.0;
    floor_ = floor < 0.05 ? 0.05 : (floor > 1.0 ? 1.0 : floor);
    if (!on_)
        rate_ = 1.0;
}

void TimeDilation::note(u64 nowNs, f64 simulatedMs, u32 ticks) noexcept
{
    if (windowStartNs_ == 0 || nowNs < windowStartNs_) {
        windowStartNs_ = nowNs;
        windowSimulatedMs_ = 0.0;
        windowTicks_ = 0;
        return;
    }
    windowSimulatedMs_ += simulatedMs;
    windowTicks_ += ticks;
    const u64 spentNs = nowNs - windowStartNs_;
    if (spentNs < 1'000'000'000ull)
        return;
    busy_ = std::min(1.0, windowSimulatedMs_ / (static_cast<f64>(spentNs) / 1.0e6));
    const f64 tickMs = windowTicks_ > 0 ? windowSimulatedMs_ / static_cast<f64>(windowTicks_) : 0.0;
    windowStartNs_ = nowNs;
    windowSimulatedMs_ = 0.0;
    windowTicks_ = 0;
    if (!on_)
        return;
    calm_ = busy_ < SpeedBelow ? calm_ + 1 : 0;
    if (busy_ > SlowAbove) {
        // The rate whose ticks, at the cost these had, take `FitTo` of a
        // second -- in whole steps, below by one at least.
        const f64 fits = tickMs > 0.0 ? FitTo * 1000.0 / (ticksPerSecond_ * tickMs) : rate_ - Step;
        rate_ = std::max(floor_, std::min(rate_ - Step, std::floor(fits * 20.0) / 20.0));
    }
    else if (calm_ >= CalmSeconds && rate_ < 1.0) {
        rate_ = std::min(1.0, rate_ + Step);
        calm_ = 0;
    }
    // Steps of a twentieth, kept exact so a rate said is the rate run.
    rate_ = std::round(rate_ * 20.0) / 20.0;
}

u64 FrameScheduler::nanosUntilNextTick(u64 nowNs) const noexcept
{
    if (!started_)
        return 0;
    const f64 since = nowNs > lastNs_ ? static_cast<f64>(nowNs - lastNs_) / 1'000'000'000.0 : 0.0;
    // In wall time: the tick owed at this rate is that much further off.
    const f64 owed = (timing_.fixedDt - accumulator_) / timeScale_ - since;
    return owed > 0.0 ? static_cast<u64>(owed * 1'000'000'000.0) : 0;
}

} // namespace engine::app
