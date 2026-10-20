// **A tween's clock, one tick at a time** (api-design.md §2.3, ADR 0254).
//
// What `TweenService` advances every tick, kept apart from the script runtime
// that owns a `Tween` so that a replica can advance the same clock for a part
// whose tween its authority runs: given the clock as the authority had it at
// one tick, the same steps reach the same value at any later tick, bit for
// bit (ADR 0254 §2.1). Nothing here writes a property; the caller does, with
// the eased fraction this answers.
#pragma once

#include "engine/core/easing.h"
#include "engine/core/types.h"

namespace engine::scene {

// `TweenInfo`: how long, how it eases, how often, and whether it comes back.
struct TweenTiming
{
    core::f64 time = 1.0;
    core::f64 delayTime = 0.0;
    core::i32 repeatCount = 0;
    core::EasingStyle style = core::EasingStyle::Quad;
    core::EasingDirection direction = core::EasingDirection::Out;
    bool reverses = false;
    [[nodiscard]] constexpr bool operator==(const TweenTiming&) const noexcept = default;
};

// `Enum.PlaybackState`'s values.
enum class TweenPlayback : core::u8
{
    Begin = 0,
    Delayed = 1,
    Playing = 2,
    Paused = 3,
    Completed = 4,
    Cancelled = 5,
};

// Where a tween is: the phase it is in and how far into it.
struct TweenClock
{
    // Seconds inside the current phase -- the delay, or the traversal.
    core::f64 elapsed = 0.0;
    // Traversals finished; a reversing tween counts a there-and-back as one.
    core::i32 repeatsDone = 0;
    TweenPlayback state = TweenPlayback::Begin;
    // Playing the return half of a reversing traversal.
    bool returning = false;
    // Whether the start of this traversal has been taken.
    bool captured = false;
};

// What one tick of the clock asks of its caller.
struct TweenTick
{
    // Write `start` blended towards the goal by `eased`.
    bool writes = false;
    core::f32 eased = 0.0f;
    // Take the property's value as this traversal's start, before writing.
    bool captureStart = false;
    // The last traversal is done: the tween has completed.
    bool completed = false;
};

// One tick of `dt` seconds, for a clock that is playing or delayed; a clock in
// any other state is not advanced and asks nothing.
[[nodiscard]] TweenTick advanceTweenClock(const TweenTiming& timing, TweenClock& clock, core::f64 dt) noexcept;

} // namespace engine::scene
