#include "engine/render/draw_poses.h"

#include <algorithm>
#include <cmath>

#include "engine/render/transform_history.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render {

void DrawPoses::begin(const scene::World& world, const TransformHistory* history, core::f32 alpha) noexcept
{
    world_ = &world;
    history_ = history;
    alpha_ = alpha;
    ++stamp_;
}

const core::CFrameD* DrawPoses::remembered(core::InstanceId id, Kind kind) const noexcept
{
    if (!id.valid() || id.index >= entries_.size())
        return nullptr;
    const Entry& entry = entries_[id.index];
    return entry.stamp == stamp_ && entry.generation == id.generation && entry.kind == kind ? &entry.cframe : nullptr;
}

void DrawPoses::keep(core::InstanceId id, Kind kind, const core::CFrameD& cframe) const
{
    // With no history every answer is the simulated place, cheaper to read
    // again than to keep: a pose resolved for a world drawn at its tick --
    // the fallback a caller with no frame makes -- allocates nothing.
    if (history_ == nullptr)
        return;
    if (entries_.size() <= id.index)
        entries_.resize(static_cast<std::size_t>(id.index) + 1);
    entries_[id.index] = Entry{.generation = id.generation, .stamp = stamp_, .kind = kind, .cframe = cframe};
}

void DrawPoses::carryPart(core::InstanceId id, const core::CFrameD& cframe)
{
    if (!id.valid())
        return;
    // Kept whatever `keep` would do: this is not a memo of an answer that
    // could be worked out again, it IS the answer.
    if (entries_.size() <= id.index)
        entries_.resize(static_cast<std::size_t>(id.index) + 1);
    entries_[id.index] = Entry{.generation = id.generation, .stamp = stamp_, .kind = Kind::Part, .cframe = cframe};
}

void DrawPoses::carryAttachment(core::InstanceId id, const core::CFrameD& cframe)
{
    if (!id.valid())
        return;
    if (entries_.size() <= id.index)
        entries_.resize(static_cast<std::size_t>(id.index) + 1);
    entries_[id.index] =
        Entry{.generation = id.generation, .stamp = stamp_, .kind = Kind::Attachment, .cframe = cframe};
}

void DrawPoses::forget(core::InstanceId id) noexcept
{
    if (id.valid() && id.index < entries_.size() && entries_[id.index].generation == id.generation)
        entries_[id.index].stamp = 0;
}

core::DVec3 DrawPoses::slideOf(core::InstanceId id) const noexcept
{
    // **The corrected part and everything under it**: a character slid back
    // after a correction carries its hat, its limbs and the name over it
    // with it, or the name is left where the correction put the body.
    if (history_ == nullptr || world_ == nullptr)
        return core::DVec3{};
    const core::InstanceId slid = history_->visualOffsetId();
    const core::DVec3 offset = history_->visualOffset();
    if (!slid.valid() || (offset.x == 0.0 && offset.y == 0.0 && offset.z == 0.0))
        return core::DVec3{};
    for (core::InstanceId cursor = id; cursor.valid(); cursor = world_->parentOf(cursor)) {
        if (cursor == slid)
            return history_->visualOffset();
    }
    return core::DVec3{};
}

core::CFrameD DrawPoses::part(core::InstanceId id) const
{
    if (world_ == nullptr || !id.valid())
        return core::CFrameD{};
    if (const core::CFrameD* known = remembered(id, Kind::Part); known != nullptr)
        return *known;
    const scene::PartComponent* component = world_->parts().find(id);
    if (component == nullptr) {
        // Not a part: the part it hangs from.
        for (core::InstanceId cursor = world_->parentOf(id); cursor.valid(); cursor = world_->parentOf(cursor)) {
            if (world_->parts().find(cursor) != nullptr)
                return part(cursor);
        }
        return core::CFrameD{};
    }
    core::CFrameD drawn = interpolatedCFrame(history_, id, component->cframe, alpha_, teleportReach(component->size));
    const core::DVec3 slide = slideOf(id);
    drawn.position = drawn.position + slide;
    keep(id, Kind::Part, drawn);
    return drawn;
}

core::CFrameD DrawPoses::camera(core::InstanceId id) const
{
    if (world_ == nullptr || !id.valid())
        return core::CFrameD{};
    if (const core::CFrameD* known = remembered(id, Kind::Camera); known != nullptr)
        return *known;
    const scene::CameraComponent* component = world_->cameras().find(id);
    if (component == nullptr)
        return core::CFrameD{};
    // **A presented camera is drawn as it was written** (ADR 0136): a render
    // step already placed it for this frame, and interpolating it again would
    // put it a frame behind what it follows.
    if (component->presenting) {
        keep(id, Kind::Camera, component->presented);
        return component->presented;
    }
    core::CFrameD drawn = interpolatedCFrame(history_, id, component->cframe, alpha_);
    // A camera hung on a corrected character slides with it.
    drawn.position = drawn.position + slideOf(id);
    keep(id, Kind::Camera, drawn);
    return drawn;
}

core::CFrameD DrawPoses::attachment(core::InstanceId id) const
{
    if (world_ == nullptr || !id.valid())
        return core::CFrameD{};
    if (const core::CFrameD* known = remembered(id, Kind::Attachment); known != nullptr)
        return *known;
    const scene::AttachmentComponent* component = world_->attachments().find(id);
    if (component == nullptr)
        return core::CFrameD{};
    // Where it sits on its part, carried by that part as drawn. Its own
    // `CFrame` is that place for an attachment that is not a bone -- exactly,
    // and whatever moved the part after the physics resolved `WorldCFrame`
    // (a script's `Heartbeat`), which left the resolved one a move behind. A
    // bone's place is the rig's pose too, as the physics last resolved it.
    // An attachment on no part is where it is.
    core::CFrameD drawn = component->worldCFrame;
    const core::InstanceId owner = world_->parentOf(id);
    if (const scene::PartComponent* on = owner.valid() ? world_->parts().find(owner) : nullptr; on != nullptr) {
        drawn = component->jointName.id == 0 ? part(owner) * component->cframe
                                             : part(owner) * (core::inverse(on->cframe) * component->worldCFrame);
    }
    keep(id, Kind::Attachment, drawn);
    return drawn;
}

Pose2D DrawPoses::part2d(core::InstanceId id) const
{
    // A 2D pose is kept in the 3D slot as the history keeps it: x, y, and the
    // turn in degrees where z would be.
    Pose2D pose;
    if (world_ == nullptr || !id.valid())
        return pose;
    if (const core::CFrameD* known = remembered(id, Kind::Part2D); known != nullptr)
        return Pose2D{core::DVec3{known->position.x, known->position.y, 0.0},
                      static_cast<core::f32>(known->position.z)};
    const scene::Part2DComponent* component = world_->parts2d().find(id);
    if (component == nullptr)
        return pose;
    pose.position =
        core::DVec3{static_cast<core::f64>(component->position.x), static_cast<core::f64>(component->position.y), 0.0};
    pose.rotation = component->rotation;
    const core::CFrameD* earlier = history_ != nullptr ? history_->previous(id) : nullptr;
    if (earlier != nullptr) {
        const core::f64 dx = pose.position.x - earlier->position.x;
        const core::f64 dy = pose.position.y - earlier->position.y;
        const core::f64 reach = teleportReach(core::Vec3{component->size.x, component->size.y, 0.0f});
        if (dx * dx + dy * dy <= reach * reach) {
            const core::f64 t = static_cast<core::f64>(alpha_);
            pose.position = core::DVec3{earlier->position.x + dx * t, earlier->position.y + dy * t, 0.0};
            // The shorter way round, so a turn past 180 does not spin back.
            const core::f64 before = earlier->position.z;
            const core::f64 turn = std::remainder(static_cast<core::f64>(component->rotation) - before, 360.0);
            pose.rotation = static_cast<core::f32>(before + turn * t);
        }
    }
    const core::DVec3 slide = slideOf(id);
    pose.position = core::DVec3{pose.position.x + slide.x, pose.position.y + slide.y, 0.0};
    core::CFrameD kept;
    kept.position = core::DVec3{pose.position.x, pose.position.y, static_cast<core::f64>(pose.rotation)};
    keep(id, Kind::Part2D, kept);
    return pose;
}

} // namespace engine::render
