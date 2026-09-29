#include "engine/app/frame_scheduler.h"

#include <array>

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
    // **Two clocks can**, and one did (D209): a headless client that left a
    // match went from the real clock back to the synthetic one, which counts
    // from zero, and `now - last` wrapped to eighteen trillion milliseconds.
    // Time that went backwards is no time at all.
    const u64 elapsedNs = nowNs > lastNs_ ? nowNs - lastNs_ : 0;
    lastNs_ = nowNs;

    frame.renderDt = static_cast<f64>(elapsedNs) / 1'000'000'000.0;
    accumulator_ += frame.renderDt;

    const f64 maxAccumulated = timing_.fixedDt * static_cast<f64>(timing_.maxCatchUpTicks);
    if (accumulator_ > maxAccumulated) {
        frame.clamped = true;
        accumulator_ = maxAccumulated;
    }

    while (accumulator_ >= timing_.fixedDt) {
        accumulator_ -= timing_.fixedDt;
        ++frame.simTicks;
    }

    totalTicks_ += frame.simTicks;
    frame.alpha = static_cast<f32>(accumulator_ / timing_.fixedDt);

    constexpr u64 WarnEveryNs = 5'000'000'000ull;
    if (frame.clamped && (!warned_ || nowNs - lastWarnNs_ >= WarnEveryNs)) {
        warned_ = true;
        lastWarnNs_ = nowNs;
        const std::array<core::I18nArg, 2> args{core::I18nArg{"ticks", static_cast<core::i64>(frame.simTicks)},
                                                core::I18nArg{"ms", frame.renderDt * 1000.0}};
        core::log(core::LogLevel::Warn, ENG_TR("engine.frame.warn.catch_up_clamped"), args);
    }

    ++totalFrames_;
    return frame;
}

u64 FrameScheduler::nanosUntilNextTick(u64 nowNs) const noexcept
{
    if (!started_)
        return 0;
    const f64 since = nowNs > lastNs_ ? static_cast<f64>(nowNs - lastNs_) / 1'000'000'000.0 : 0.0;
    const f64 owed = timing_.fixedDt - accumulator_ - since;
    return owed > 0.0 ? static_cast<u64>(owed * 1'000'000'000.0) : 0;
}

} // namespace engine::app
