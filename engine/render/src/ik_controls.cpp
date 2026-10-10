#include "engine/render/ik_controls.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "engine/core/profile.h"
#include "engine/render/animation.h"
#include "engine/render/draw_poses.h"
#include "engine/render/ik.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::CFrameD;
using core::DVec3;
using core::f32;
using core::Mat3;
using core::Mat4;
using core::Vec3;

constexpr f32 DegreesToRadians = 0.017453292519943295f;

[[nodiscard]] core::u64 keyOf(core::InstanceId id) noexcept
{
    return static_cast<core::u64>(id.index) | (static_cast<core::u64>(id.generation) << 32);
}

// The frame a mesh is drawn in, and how many of the world's units one of its
// rig's is -- as the spring chains take it, and for their reason.
struct Carrier
{
    CFrameD frame{};
    f32 scale = 1.0f;
};

[[nodiscard]] Carrier carrierOf(const scene::World& world, const DrawPoses& poses, core::InstanceId meshPart)
{
    Carrier carrier;
    carrier.frame = poses.part(meshPart);
    const scene::PartComponent* part = world.parts().find(meshPart);
    const scene::MeshPartComponent* mesh = world.meshParts().find(meshPart);
    if (part != nullptr && mesh != nullptr && mesh->meshSize.x > 0.0f && mesh->meshSize.y > 0.0f &&
        mesh->meshSize.z > 0.0f) {
        carrier.scale =
            (part->size.x / mesh->meshSize.x + part->size.y / mesh->meshSize.y + part->size.z / mesh->meshSize.z) /
            3.0f;
    }
    return carrier;
}

// A place in the world, in the rig's model space.
[[nodiscard]] Vec3 toModel(const Carrier& carrier, DVec3 world) noexcept
{
    const f32 unscale = carrier.scale > 1.0e-6f ? 1.0f / carrier.scale : 1.0f;
    return (core::transpose(carrier.frame.rotation) * core::toVec3(world - carrier.frame.position)) * unscale;
}

[[nodiscard]] DVec3 toWorld(const Carrier& carrier, Vec3 model) noexcept
{
    return carrier.frame.position + core::toDVec3(carrier.frame.rotation * (model * carrier.scale));
}

[[nodiscard]] Vec3 placeOf(const Mat4& matrix) noexcept
{
    return Vec3{matrix.m[3][0], matrix.m[3][1], matrix.m[3][2]};
}

// Where a target is drawn this frame: a part, or an attachment on one.
[[nodiscard]] bool drawnPlace(const scene::World& world, const DrawPoses& poses, core::InstanceId id, CFrameD& out)
{
    if (!id.valid() || !world.alive(id) || world.destroyed(id))
        return false;
    if (world.parts().find(id) != nullptr) {
        out = poses.part(id);
        return true;
    }
    if (world.attachments().find(id) != nullptr) {
        out = poses.attachment(id);
        return true;
    }
    return false;
}

// **Which way the body faces, in its own model space**, from the rig itself:
// across the shoulders (or the hips), turned a quarter about up. A file is
// free to face its character along any axis, and a look that took one for
// granted would turn half the world's heads to the wall.
[[nodiscard]] Vec3 facingOf(const AnimationSystem& animation, core::InstanceId mesh)
{
    const retarget::RigRoles* roles = animation.rolesOf(mesh);
    if (roles != nullptr) {
        const std::pair<retarget::Role, retarget::Role> across[] = {
            {retarget::Role::LeftUpperArm, retarget::Role::RightUpperArm},
            {retarget::Role::LeftShoulder, retarget::Role::RightShoulder},
            {retarget::Role::LeftUpperLeg, retarget::Role::RightUpperLeg},
        };
        for (const auto& [left, right] : across) {
            const core::i32 l = roles->joint(left);
            const core::i32 r = roles->joint(right);
            if (l < 0 || r < 0)
                continue;
            const Vec3 span = placeOf(animation.restModel(mesh, static_cast<core::u32>(r))) -
                              placeOf(animation.restModel(mesh, static_cast<core::u32>(l)));
            const Vec3 forward = core::cross(Vec3{0.0f, 1.0f, 0.0f}, span);
            if (core::length(forward) > 1.0e-5f)
                return core::normalize(forward);
        }
    }
    // A rig that is not a body by its names: the way a model faces in the
    // format the engine reads.
    return Vec3{0.0f, 0.0f, 1.0f};
}

// **What rides a joint of a body**: each `Bone` under the mesh that names
// one, and every part welded to such a bone -- a sword to the hand's -- and
// the parts welded to those, a few links deep.
struct Riding
{
    core::InstanceId id;
    core::u32 joint = 0;
};

void gatherRiding(const scene::World& world, core::InstanceId mesh, core::usize jointCount, std::vector<Riding>& out)
{
    out.clear();
    for (core::InstanceId child = world.firstChild(mesh); child.valid(); child = world.nextSibling(child)) {
        const scene::AttachmentComponent* bone = world.attachments().find(child);
        if (bone != nullptr && bone->jointIndex >= 0 && static_cast<core::usize>(bone->jointIndex) < jointCount)
            out.push_back({child, static_cast<core::u32>(bone->jointIndex)});
    }
    if (out.empty())
        return;
    core::usize from = 0;
    for (int depth = 0; depth < 4 && from < out.size(); ++depth) {
        const core::usize until = out.size();
        world.welds().forEach([&](core::InstanceId, const scene::WeldComponent& weld) {
            if (!weld.enabled)
                return;
            for (core::usize at = from; at < until; ++at) {
                const core::InstanceId link = out[at].id;
                const core::InstanceId other = weld.part0 == link   ? weld.part1
                                               : weld.part1 == link ? weld.part0
                                                                    : core::InstanceId{};
                if (!other.valid() || other == mesh || world.parts().find(other) == nullptr)
                    continue;
                const bool known = std::find_if(out.begin(), out.end(),
                                                [other](const Riding& one) { return one.id == other; }) != out.end();
                if (!known)
                    out.push_back({other, out[at].joint});
            }
        });
        from = until;
    }
}

// **How the frame moved a joint**, as a turn and a move about the place the
// body is drawn at. Kept that way and applied there, in single precision
// near nought: as one transform of the world it would lose the millimetres
// far from the origin.
struct Shift
{
    Mat3 turn{};
    DVec3 move{};
    bool any = false;
};

[[nodiscard]] Shift shiftOf(const Carrier& carrier, const Mat4& now, const Mat4& was)
{
    const CFrameD inModel = core::cframeFromMatrix(now) * core::inverse(core::cframeFromMatrix(was));
    const Vec3 moved = core::toVec3(inModel.position);
    Shift shift;
    shift.any =
        !(core::length(moved) < 1.0e-5f && std::fabs(inModel.rotation.m[0][0] - 1.0f) < 1.0e-6f &&
          std::fabs(inModel.rotation.m[1][1] - 1.0f) < 1.0e-6f && std::fabs(inModel.rotation.m[2][2] - 1.0f) < 1.0e-6f);
    shift.turn = carrier.frame.rotation * inModel.rotation * core::transpose(carrier.frame.rotation);
    shift.move = core::toDVec3(carrier.frame.rotation * (moved * carrier.scale));
    return shift;
}

// A drawn place, carried by a joint's shift.
[[nodiscard]] CFrameD carriedBy(const Carrier& carrier, const Shift& shift, const CFrameD& held)
{
    CFrameD carried;
    carried.rotation = shift.turn * held.rotation;
    carried.position = carrier.frame.position +
                       core::toDVec3(shift.turn * core::toVec3(held.position - carrier.frame.position)) + shift.move;
    return carried;
}

// The rotation of a joint's matrix with its scale taken out.
[[nodiscard]] Mat3 turnOf(const Mat4& matrix) noexcept
{
    Mat3 turn;
    for (int column = 0; column < 3; ++column) {
        const Vec3 axis{matrix.m[column][0], matrix.m[column][1], matrix.m[column][2]};
        const f32 length = core::length(axis);
        const Vec3 unit = length > 1.0e-8f
                              ? axis * (1.0f / length)
                              : Vec3{column == 0 ? 1.0f : 0.0f, column == 1 ? 1.0f : 0.0f, column == 2 ? 1.0f : 0.0f};
        turn.m[column][0] = unit.x;
        turn.m[column][1] = unit.y;
        turn.m[column][2] = unit.z;
    }
    return turn;
}

} // namespace

void IkControls::update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses,
                        const IkFrame& frame)
{
    ENG_PROFILE_SCOPE("animation.ik");
    ++frame_;
    controlsSolved_ = 0;
    feetPlaced_ = 0;
    raysCast_ = 0;
    if (world.ikControls().size() == 0 && world.footPlacements().size() == 0) {
        eased_.clear();
        return;
    }

    // What each mesh has, in the order of its instances: a mesh is solved
    // once, all its controls together.
    struct Work
    {
        core::InstanceId mesh;
        std::vector<core::InstanceId> looks;
        std::vector<core::InstanceId> limbs;
        core::InstanceId feet;
    };
    std::map<core::u64, Work> works;
    const auto reached = [&](core::InstanceId mesh) {
        if (!mesh.valid() || world.meshParts().find(mesh) == nullptr || animation.jointCount(mesh) == 0 ||
            !animation.seenLately(mesh))
            return false;
        // **A mesh that wears another's pose has no limbs of its own to
        // solve** (ADR 0201): its joints are its leader's, and a control on
        // the leader is what reaches for both.
        if (animation.leaderOf(mesh).valid())
            return false;
        return core::length(core::toVec3(poses.part(mesh).position - frame.camera)) <= frame.maxDistance;
    };
    world.ikControls().forEach([&](core::InstanceId id, const scene::IKControlComponent& control) {
        if (!world.alive(id) || world.destroyed(id))
            return;
        const core::InstanceId mesh = world.parentOf(id);
        if (!reached(mesh))
            return;
        Work& work = works[keyOf(mesh)];
        work.mesh = mesh;
        (control.type == 1 ? work.looks : work.limbs).push_back(id);
    });
    if (frame.feet && frame.ground) {
        // **A swarm's agents have none of it**: a horde's feet are its
        // clips'. Their bodies are found once a frame, and only when there
        // is a foot to place at all.
        swarmBodies_.clear();
        if (world.footPlacements().size() != 0) {
            world.swarms().forEach([&](core::InstanceId, const scene::SwarmComponent& swarm) {
                for (const scene::SwarmAgent& agent : swarm.agents) {
                    if (agent.alive && agent.body.valid())
                        swarmBodies_.push_back(agent.body);
                }
            });
            std::sort(swarmBodies_.begin(), swarmBodies_.end(), [](core::InstanceId a, core::InstanceId b) {
                return a.index != b.index ? a.index < b.index : a.generation < b.generation;
            });
        }
        world.footPlacements().forEach([&](core::InstanceId id, const scene::FootPlacementComponent& feet) {
            if (!feet.enabled || !(feet.weight > 0.0f) || !world.alive(id) || world.destroyed(id))
                return;
            const core::InstanceId mesh = world.parentOf(id);
            if (!reached(mesh))
                return;
            if (std::binary_search(swarmBodies_.begin(), swarmBodies_.end(), mesh,
                                   [](core::InstanceId a, core::InstanceId b) {
                                       return a.index != b.index ? a.index < b.index : a.generation < b.generation;
                                   }))
                return;
            Work& work = works[keyOf(mesh)];
            work.mesh = mesh;
            // One a mesh: the first, in instance order.
            if (!work.feet.valid() || id.index < work.feet.index)
                work.feet = id;
        });
    }

    // How much of the way a thing eased over `seconds` goes in this frame.
    const auto easeBy = [&](f32 smoothing) {
        return smoothing > 1.0e-4f ? 1.0f - std::exp(-frame.seconds / smoothing) : 1.0f;
    };

    std::vector<AnimationSystem::PresentedJoint> presented;
    // What rides a joint of the body being solved, found when a limb asks.
    std::vector<Riding> riding;
    for (auto& [key, work] : works) {
        const core::InstanceId mesh = work.mesh;
        if (!animation.modelOf(mesh, model_))
            continue;
        const std::span<const asset::Joint> joints = animation.jointsOf(mesh);
        if (joints.size() != model_.size())
            continue;
        const Carrier carrier = carrierOf(world, poses, mesh);
        bool moved = false;
        // The pose as the tick has it, kept beside the one being solved: a
        // limb may reach for a thing this same body holds, and that thing is
        // drawn where the frame has moved the joint it rides.
        if (!work.limbs.empty())
            simulated_ = model_;
        bool gathered = false;

        // A control's target, eased: where it is taken to be this frame and
        // how much of the control is applied. **Eased in the body's own
        // space**: a thing that keeps its place by the body -- a weapon it
        // carries, the rail of a cart it rides -- is not trailed behind by
        // the body's speed.
        const auto ease = [&](core::InstanceId id, const scene::IKControlComponent& control, Vec3& target,
                              f32& weight) {
            Eased& eased = eased_[keyOf(id)];
            const bool fresh = eased.frame + 1 != frame_;
            const f32 wanted = control.enabled ? std::clamp(control.weight, 0.0f, 1.0f) : 0.0f;
            const f32 by = easeBy(control.smoothing);
            if (fresh) {
                // Back in reach, or new: where it is, from nothing applied.
                eased.target = target;
                eased.weight = control.smoothing > 1.0e-4f ? 0.0f : wanted;
            }
            eased.target = eased.target + (target - eased.target) * by;
            eased.weight += (wanted - eased.weight) * by;
            eased.frame = frame_;
            target = eased.target;
            weight = eased.weight;
        };
        // Where a limb's target or pole is drawn: where it is drawn, moved as
        // the frame has moved the joint it rides, when it rides one of this
        // body's.
        const auto reachedFor = [&](core::InstanceId id, CFrameD& place) {
            if (!drawnPlace(world, poses, id, place))
                return false;
            if (!gathered) {
                gatherRiding(world, mesh, model_.size(), riding);
                gathered = true;
            }
            if (riding.empty())
                return true;
            const core::InstanceId holder = world.parts().find(id) != nullptr ? id : world.parentOf(id);
            for (const Riding& one : riding) {
                if (one.id != id && one.id != holder)
                    continue;
                if (const Shift shift = shiftOf(carrier, model_[one.joint], simulated_[one.joint]); shift.any)
                    place = carriedBy(carrier, shift, place);
                break;
            }
            return true;
        };

        // **Looks first**, spine before head as their instances are ordered:
        // a body that turns to look moves everything the limbs then reach
        // from.
        Vec3 facing{0.0f, 0.0f, 1.0f};
        bool faced = false;
        for (const core::InstanceId id : work.looks) {
            const scene::IKControlComponent& control = *world.ikControls().find(id);
            const core::i32 end = animation.findJoint(mesh, world.atoms().text(control.endJoint));
            CFrameD place;
            if (end < 0 || !drawnPlace(world, poses, control.target, place))
                continue;
            Vec3 target = toModel(carrier, (place * control.targetOffset).position);
            f32 weight = 0.0f;
            ease(id, control, target, weight);
            if (!(weight > 1.0e-4f))
                continue;
            if (!faced) {
                facing = facingOf(animation, mesh);
                faced = true;
            }
            // The way the joint faces now: the body's own way, turned as the
            // clip has turned the joint from its rest.
            const Mat3 turned = turnOf(model_[static_cast<core::usize>(end)]) *
                                core::transpose(turnOf(animation.restModel(mesh, static_cast<core::u32>(end))));
            if (ik::solveLook(joints, model_, static_cast<core::u32>(end), static_cast<core::u32>(control.chainLength),
                              target, turned * facing, control.maxAngle * DegreesToRadians, weight)) {
                moved = true;
                ++controlsSolved_;
            }
        }

        // **Then the feet**: the hips, and each leg to its own ground.
        if (work.feet.valid()) {
            const scene::FootPlacementComponent& feet = *world.footPlacements().find(work.feet);
            const core::i32 left = animation.findJoint(mesh, world.atoms().text(feet.leftFoot));
            const core::i32 right = animation.findJoint(mesh, world.atoms().text(feet.rightFoot));
            const core::i32 hips = animation.findJoint(mesh, world.atoms().text(feet.hips));
            if (left >= 0 && right >= 0 && hips >= 0) {
                // What the rays must not find: the character itself -- the
                // mesh, and everything under the thing the mesh belongs to.
                own_.clear();
                core::InstanceId root = mesh;
                for (core::InstanceId above = world.parentOf(mesh); above.valid(); above = world.parentOf(above)) {
                    if (world.workspaces().find(above) != nullptr)
                        break;
                    if (world.parts().find(above) != nullptr || world.models().find(above) != nullptr)
                        root = above;
                }
                own_.push_back(root);
                world.collectDescendants(root, own_);
                std::erase_if(own_, [&](core::InstanceId id) { return world.parts().find(id) == nullptr; });

                // The ground the clips were made on, from the rig at rest:
                // the lower ankle, less the sole.
                const f32 clipGround = std::min(placeOf(animation.restModel(mesh, static_cast<core::u32>(left))).y,
                                                placeOf(animation.restModel(mesh, static_cast<core::u32>(right))).y) -
                                       feet.footHeight;
                const Vec3 down = carrier.frame.rotation * Vec3{0.0f, -1.0f, 0.0f};
                const f32 reach = (2.0f * feet.stepHeight + feet.footHeight) * carrier.scale;
                const Mat3 intoModel = core::transpose(carrier.frame.rotation);
                const auto under = [&](core::i32 joint) {
                    ik::Foot foot;
                    foot.joint = static_cast<core::u32>(joint);
                    // From a step above the clip's ground, straight down past
                    // a step below it.
                    Vec3 from = placeOf(model_[static_cast<core::usize>(joint)]);
                    from.y = clipGround + feet.stepHeight + feet.footHeight;
                    DVec3 point;
                    Vec3 normal{0.0f, 1.0f, 0.0f};
                    ++raysCast_;
                    if (frame.ground(toWorld(carrier, from), down * reach, own_, point, normal)) {
                        foot.hit = true;
                        foot.ground = toModel(carrier, point).y;
                        foot.normal = intoModel * normal;
                    }
                    return foot;
                };
                const ik::Foot leftFoot = under(left);
                const ik::Foot rightFoot = under(right);
                const ik::FeetResult result =
                    ik::solveFeet(joints, model_, static_cast<core::u32>(hips), leftFoot, rightFoot, clipGround,
                                  feet.stepHeight, feet.alignToSlope, std::clamp(feet.weight, 0.0f, 1.0f));
                feetPlaced_ += (result.leftPlanted ? 1u : 0u) + (result.rightPlanted ? 1u : 0u);
                moved = moved || result.leftPlanted || result.rightPlanted || result.hipsOffset != 0.0f;
            }
        }

        // **Then the limbs**, from the body as the looks and the feet left it.
        for (const core::InstanceId id : work.limbs) {
            const scene::IKControlComponent& control = *world.ikControls().find(id);
            const core::i32 end = animation.findJoint(mesh, world.atoms().text(control.endJoint));
            CFrameD place;
            if (end < 0 || !reachedFor(control.target, place))
                continue;
            const CFrameD wanted = place * control.targetOffset;
            Vec3 target = toModel(carrier, wanted.position);
            f32 weight = 0.0f;
            ease(id, control, target, weight);
            if (!(weight > 1.0e-4f))
                continue;
            const Mat3 turn = core::transpose(carrier.frame.rotation) * wanted.rotation;
            Vec3 pole{};
            CFrameD polePlace;
            const bool poled = control.pole.valid() && reachedFor(control.pole, polePlace);
            if (poled)
                pole = toModel(carrier, polePlace.position);
            if (ik::solveTwoBone(joints, model_, static_cast<core::u32>(end), target,
                                 control.alignRotation ? &turn : nullptr, poled ? &pole : nullptr, weight)) {
                moved = true;
                ++controlsSolved_;
            }
        }

        if (!moved)
            continue;
        presented.clear();
        presented.reserve(model_.size());
        for (core::u32 joint = 0; joint < model_.size(); ++joint)
            presented.push_back({joint, model_[joint]});
        animation.present(mesh, presented);
    }

    // What was not reached this frame starts again from nothing applied.
    std::erase_if(eased_, [this](const auto& entry) { return entry.second.frame != frame_; });
}

void IkControls::carryHeld(const scene::World& world, const AnimationSystem& animation, DrawPoses& poses)
{
    const std::span<const core::InstanceId> meshes = animation.presentedMeshes();
    if (meshes.empty())
        return;
    ENG_PROFILE_SCOPE("animation.held");
    std::vector<Riding> riding;
    std::vector<Mat4> simulated;
    for (const core::InstanceId mesh : meshes) {
        const Pose* drawn = animation.drawnPose(mesh);
        if (drawn == nullptr || !animation.modelOf(mesh, simulated) || simulated.size() != drawn->model.size())
            continue;
        gatherRiding(world, mesh, simulated.size(), riding);
        if (riding.empty())
            continue;
        // A follower is drawn in its leader's place: what rides one of its
        // joints is carried from there.
        const Carrier carrier = carrierOf(world, poses, animation.drawnWith(mesh));
        for (const Riding& one : riding) {
            const Shift shift = shiftOf(carrier, drawn->model[one.joint], simulated[one.joint]);
            if (!shift.any)
                continue;
            if (world.attachments().find(one.id) != nullptr) {
                poses.carryAttachment(one.id, carriedBy(carrier, shift, poses.attachment(one.id)));
                continue;
            }
            poses.carryPart(one.id, carriedBy(carrier, shift, poses.part(one.id)));
            // What sits on the part is worked out from it again: a grip, a
            // light on a staff. It may have been asked for already this
            // frame -- a limb reaching for it -- where the part then was.
            for (core::InstanceId child = world.firstChild(one.id); child.valid(); child = world.nextSibling(child))
                poses.forget(child);
        }
    }
}

} // namespace engine::render
