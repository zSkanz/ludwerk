// A value that changes along a line from 0 to 1 (ADR 0110): a colour sequence
// and a number sequence, what a `UIGradient` colours and fades its parent with.
//
// **Keypoints, not a function.** A sequence is up to `MaxSequenceKeypoints`
// stops at rising times, the first at exactly 0 and the last at exactly 1, and
// between two stops the value is their straight-line mix. That is the whole
// model, and it is the one a person drags stops along a bar to edit.
//
// Here rather than in `scene` because it is arithmetic on a small value, the
// same kind of thing `Color3` and `UDim2` beside it are: `scene` stores one,
// `script` builds one and `ui` evaluates one, and all three reach `core`.
#pragma once

#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::core {

// Twenty, which is what a person can place along a bar and still tell apart --
// and what keeps a sequence a small value on the wire and in a scene file.
inline constexpr usize MaxSequenceKeypoints = 20;

struct ColorKeypoint
{
    f32 time = 0.0f;
    Color3 value{1.0f, 1.0f, 1.0f};

    [[nodiscard]] bool operator==(const ColorKeypoint&) const noexcept = default;
};

struct NumberKeypoint
{
    f32 time = 0.0f;
    f32 value = 0.0f;
    // How far a particle may stray from `value` at this time. Carried so a
    // sequence says the same thing wherever it goes; the UI ignores it.
    f32 envelope = 0.0f;

    [[nodiscard]] bool operator==(const NumberKeypoint&) const noexcept = default;
};

struct ColorSequence
{
    // White from 0 to 1: a gradient that changes nothing.
    std::vector<ColorKeypoint> keypoints{ColorKeypoint{0.0f, Color3{1.0f, 1.0f, 1.0f}},
                                         ColorKeypoint{1.0f, Color3{1.0f, 1.0f, 1.0f}}};

    [[nodiscard]] bool operator==(const ColorSequence&) const = default;
};

struct NumberSequence
{
    // Zero from 0 to 1: a transparency that hides nothing.
    std::vector<NumberKeypoint> keypoints{NumberKeypoint{0.0f, 0.0f, 0.0f}, NumberKeypoint{1.0f, 0.0f, 0.0f}};

    [[nodiscard]] bool operator==(const NumberSequence&) const = default;
};

// Whether a list of keypoints is a sequence: two to twenty, the first at 0 and
// the last at 1, times never falling, and every number finite. Equal times are
// legal -- two stops at one time are a hard edge.
[[nodiscard]] bool validSequence(const std::vector<ColorKeypoint>& keypoints) noexcept;
[[nodiscard]] bool validSequence(const std::vector<NumberKeypoint>& keypoints) noexcept;

// The value at `time`, clamped into [0, 1]. At a time two stops share, the
// later one wins, so a hard edge is on the side it was placed.
[[nodiscard]] Color3 evaluate(const ColorSequence& sequence, f32 time) noexcept;
[[nodiscard]] f32 evaluate(const NumberSequence& sequence, f32 time) noexcept;

} // namespace engine::core
