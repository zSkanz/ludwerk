#include "engine/render/transform_history.h"

#include <cmath>

#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render {

void TransformHistory::capture(const scene::World& world)
{
    ++stamp_;

    const auto record = [&](core::InstanceId id, const core::CFrameD& cframe) {
        if (entries_.size() <= id.index)
            entries_.resize(static_cast<std::size_t>(id.index) + 1);
        entries_[id.index] = Entry{.generation = id.generation, .stamp = stamp_, .cframe = cframe};
    };

    world.parts().forEach([&](core::InstanceId id, const scene::PartComponent& part) { record(id, part.cframe); });
    world.cameras().forEach(
        [&](core::InstanceId id, const scene::CameraComponent& camera) { record(id, camera.cframe); });
}

const core::CFrameD* TransformHistory::previous(core::InstanceId id) const noexcept
{
    if (!id.valid() || id.index >= entries_.size())
        return nullptr;
    const Entry& entry = entries_[id.index];
    if (entry.generation != id.generation || entry.stamp != stamp_)
        return nullptr;
    return &entry.cframe;
}

void TransformHistory::clear() noexcept
{
    entries_.clear();
    stamp_ = 0;
}

core::f64 teleportReach(core::Vec3 size) noexcept
{
    const core::f32 largest = std::fmax(size.x, std::fmax(size.y, size.z));
    return static_cast<core::f64>(std::fmax(largest, 0.5f));
}

namespace {

core::CFrameD betweenTicks(const TransformHistory* history, core::InstanceId id, const core::CFrameD& current,
                           core::f32 alpha, core::f64 teleport)
{
    using core::CFrameD;
    using core::DVec3;
    if (history == nullptr || alpha <= 0.0f)
        return current;
    const CFrameD* earlier = history->previous(id);
    if (earlier == nullptr)
        return current;
    {
        const DVec3 step = current.position - earlier->position;
        if (step.x * step.x + step.y * step.y + step.z * step.z > teleport * teleport)
            return current;
    }

    // **Two early outs, and they are the difference between this costing
    // nothing and costing two and a half milliseconds.** `core::lerp` on a
    // CFrame slerps the rotation, which is a quaternion round trip with a
    // trig pair in it -- and an open world is thousands of parts that did
    // not move at all. Measured on `examples/10-open-world`: the median
    // frame at 1080p went from 3.5 ms to 6.1 ms with the slerp on every
    // part, and back with these two comparisons in front of it.
    if (earlier->rotation == current.rotation) {
        if (earlier->position == current.position)
            return current;
        // Moved without turning, which is most of what moves: a lift, a
        // sliding platform, anything driven along a path.
        const core::f64 t = static_cast<core::f64>(alpha);
        const DVec3 delta = current.position - earlier->position;
        CFrameD moved = current;
        moved.position = DVec3{earlier->position.x + delta.x * t, earlier->position.y + delta.y * t,
                               earlier->position.z + delta.z * t};
        return moved;
    }

    return core::lerp(*earlier, current, static_cast<core::f64>(alpha));
}

} // namespace

core::CFrameD interpolatedCFrame(const TransformHistory* history, core::InstanceId id, const core::CFrameD& current,
                                 core::f32 alpha, core::f64 teleport)
{
    core::CFrameD drawn = betweenTicks(history, id, current, alpha, teleport);
    if (history != nullptr && id.valid() && id == history->visualOffsetId())
        drawn.position = drawn.position + history->visualOffset();
    return drawn;
}

} // namespace engine::render
