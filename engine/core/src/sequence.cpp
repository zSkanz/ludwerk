#include "engine/core/sequence.h"

#include <cmath>

namespace engine::core {
namespace {

template <class Keypoint>
[[nodiscard]] bool validKeypoints(const std::vector<Keypoint>& keypoints) noexcept
{
    if (keypoints.size() < 2 || keypoints.size() > MaxSequenceKeypoints)
        return false;
    if (keypoints.front().time != 0.0f || keypoints.back().time != 1.0f)
        return false;
    for (usize index = 0; index < keypoints.size(); ++index) {
        if (!std::isfinite(keypoints[index].time))
            return false;
        if (index > 0 && keypoints[index].time < keypoints[index - 1].time)
            return false;
    }
    return true;
}

// The stop at or after `time`, found from the end so that of two stops at one
// time the later is the one returned.
template <class Keypoint>
[[nodiscard]] usize upperStop(const std::vector<Keypoint>& keypoints, f32 time) noexcept
{
    for (usize index = 1; index < keypoints.size(); ++index) {
        if (time < keypoints[index].time)
            return index;
    }
    return keypoints.size() - 1;
}

[[nodiscard]] f32 clampedTime(f32 time) noexcept
{
    if (!(time > 0.0f))
        return 0.0f;
    return time < 1.0f ? time : 1.0f;
}

} // namespace

bool validSequence(const std::vector<ColorKeypoint>& keypoints) noexcept
{
    if (!validKeypoints(keypoints))
        return false;
    for (const ColorKeypoint& keypoint : keypoints) {
        if (!std::isfinite(keypoint.value.r) || !std::isfinite(keypoint.value.g) || !std::isfinite(keypoint.value.b))
            return false;
    }
    return true;
}

bool validSequence(const std::vector<NumberKeypoint>& keypoints) noexcept
{
    if (!validKeypoints(keypoints))
        return false;
    for (const NumberKeypoint& keypoint : keypoints) {
        if (!std::isfinite(keypoint.value) || !std::isfinite(keypoint.envelope))
            return false;
    }
    return true;
}

Color3 evaluate(const ColorSequence& sequence, f32 time) noexcept
{
    const std::vector<ColorKeypoint>& points = sequence.keypoints;
    if (points.empty())
        return Color3{1.0f, 1.0f, 1.0f};
    if (points.size() == 1)
        return points.front().value;
    const f32 t = clampedTime(time);
    const usize upper = upperStop(points, t);
    const ColorKeypoint& a = points[upper - 1];
    const ColorKeypoint& b = points[upper];
    const f32 span = b.time - a.time;
    const f32 alpha = span > 0.0f ? (t - a.time) / span : 1.0f;
    return lerp(a.value, b.value, alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha));
}

f32 evaluate(const NumberSequence& sequence, f32 time) noexcept
{
    const std::vector<NumberKeypoint>& points = sequence.keypoints;
    if (points.empty())
        return 0.0f;
    if (points.size() == 1)
        return points.front().value;
    const f32 t = clampedTime(time);
    const usize upper = upperStop(points, t);
    const NumberKeypoint& a = points[upper - 1];
    const NumberKeypoint& b = points[upper];
    const f32 span = b.time - a.time;
    f32 alpha = span > 0.0f ? (t - a.time) / span : 1.0f;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    return a.value + (b.value - a.value) * alpha;
}

} // namespace engine::core
