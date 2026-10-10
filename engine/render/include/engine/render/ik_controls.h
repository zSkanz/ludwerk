// Limbs that reach, heads that look and feet on the ground (ADR 0198), each
// frame, in the pose the frame is drawn with.
//
// **Picture, not simulation.** Everything here runs at the rate frames are
// drawn, from where each body is DRAWN this frame, and writes only the
// presented pose of ADR 0194 -- the copy of a pose the renderer alone reads.
// The pose the tick reads is untouched: a `Bone` on a hand a control moved
// still says where the clip has the hand.
//
// The solvers are `ik.h`'s and know no instance; this is what reads the
// instances, finds each target where it is drawn, casts the rays for the
// feet and hands the solved pose over. It runs BEFORE the spring chains, so
// a cape hangs from shoulders an arm's reach moved; and `carryHeld`, after
// both, draws what is held to a bone where the frame has the bone.
#pragma once

#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class AnimationSystem;
class DrawPoses;

// A frame's worth of what the controls need from outside.
struct IkFrame
{
    // The frame's length, in seconds: what `Smoothing` eases by.
    core::f32 seconds = 0.0f;
    // Bodies further from here than `maxDistance` are left to their clips.
    core::DVec3 camera{};
    core::f32 maxDistance = 60.0f;
    // Whether feet are placed at all: not at the lowest quality.
    bool feet = true;
    // **The ground under a foot**: the nearest thing from `from` along
    // `direction` (whose length is how far to look) that is not one of
    // `own` -- the character's own parts. False for nothing. Absent, no foot
    // is placed: a host with no physics has no ground to find.
    std::function<bool(core::DVec3 from, core::Vec3 direction, std::span<const core::InstanceId> own,
                       core::DVec3& point, core::Vec3& normal)>
        ground;
};

class IkControls
{
public:
    // Solves every control and foot placement the frame reaches and presents
    // each body's pose. Call after `AnimationSystem::clearPresented` and
    // before the spring chains.
    void update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses, const IkFrame& frame);

    // **What is held to a bone is drawn where the frame has the bone**: for
    // every body with a presented pose, each `Bone` under it whose joint the
    // frame moved carries its own drawn place, and with it every part welded
    // to it and the parts welded to those. Call after everything that
    // presents a pose, before the frame is extracted.
    static void carryHeld(const scene::World& world, const AnimationSystem& animation, DrawPoses& poses);

    void clear() noexcept { eased_.clear(); }
    // What the last frame did, for the frame report and for a test.
    [[nodiscard]] core::u32 controlsSolved() const noexcept { return controlsSolved_; }
    [[nodiscard]] core::u32 feetPlaced() const noexcept { return feetPlaced_; }
    [[nodiscard]] core::u32 raysCast() const noexcept { return raysCast_; }

private:
    // What `Smoothing` remembers of a control between frames: where its
    // target was taken to be, and how much of it was applied.
    struct Eased
    {
        // In the body's own space: what keeps its place by the body is not
        // trailed behind by the body's speed.
        core::Vec3 target{};
        core::f32 weight = 0.0f;
        core::u64 frame = 0;
    };
    // Control to its eased state. Looked up, never walked for output (R10 has
    // nothing to say of a picture, and this is still not walked).
    std::unordered_map<core::u64, Eased> eased_;
    std::vector<core::Mat4> model_;
    // The pose as the tick has it, beside the one being solved.
    std::vector<core::Mat4> simulated_;
    std::vector<core::InstanceId> own_;
    std::vector<core::InstanceId> swarmBodies_;
    core::u64 frame_ = 0;
    core::u32 controlsSolved_ = 0;
    core::u32 feetPlaced_ = 0;
    core::u32 raysCast_ = 0;
};

} // namespace engine::render
