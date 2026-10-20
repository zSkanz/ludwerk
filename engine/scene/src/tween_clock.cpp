#include "engine/scene/tween_clock.h"

namespace engine::scene {

TweenTick advanceTweenClock(const TweenTiming& timing, TweenClock& clock, core::f64 dt) noexcept
{
    TweenTick out;
    if (clock.state != TweenPlayback::Playing && clock.state != TweenPlayback::Delayed)
        return out;

    clock.elapsed += dt;
    if (clock.state == TweenPlayback::Delayed) {
        if (clock.elapsed < timing.delayTime)
            return out;
        clock.elapsed -= timing.delayTime;
        clock.state = TweenPlayback::Playing;
    }

    // Taken at the first tick that writes, not at `Create`: a tween created
    // now and played in three seconds moves from where the property IS then.
    if (!clock.captured) {
        out.captureStart = true;
        clock.captured = true;
    }

    const core::f64 duration = timing.time;
    bool traversalDone = false;
    core::f64 raw = 1.0;
    if (duration <= 0.0) {
        // A zero-length tween lands on its goal in one tick rather than
        // dividing by zero.
        traversalDone = true;
    }
    else if (clock.elapsed >= duration) {
        traversalDone = true;
    }
    else {
        raw = clock.elapsed / duration;
    }

    const core::f64 progress = clock.returning ? 1.0 - raw : raw;
    out.writes = true;
    out.eased = core::ease(static_cast<core::f32>(progress), timing.style, timing.direction);

    if (!traversalDone)
        return out;

    if (timing.reverses && !clock.returning) {
        clock.returning = true;
        clock.elapsed = 0.0;
        return out;
    }

    clock.returning = false;
    clock.elapsed = 0.0;
    clock.captured = false;
    clock.repeatsDone += 1;

    // A negative `RepeatCount` repeats forever, the one negative number in
    // `TweenInfo` that means something.
    const bool forever = timing.repeatCount < 0;
    if (forever || clock.repeatsDone <= timing.repeatCount) {
        clock.state = timing.delayTime > 0.0 ? TweenPlayback::Delayed : TweenPlayback::Playing;
        return out;
    }
    out.completed = true;
    return out;
}

} // namespace engine::scene
