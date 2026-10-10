// **The capes, tails and hair of a world, a frame at a time** (ADR 0194):
// every `SpringBone` gathered into a chain of its rig's joints, stepped
// (`spring_chain.h`) and handed to the animation as the pose its mesh is
// DRAWN with.
//
// Picture only. It reads the pose and the drawn place of each mesh, and writes
// nothing the simulation reads: not a property, not the pose the sockets and
// the ragdoll use. A world with no window never makes one of these.
#pragma once

#include <unordered_map>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/render/spring_chain.h"
#include "engine/scene/wind.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class AnimationSystem;
class DrawPoses;

// What one frame tells the chains.
struct SpringFrame
{
    // Seconds since the frame before.
    f32 seconds = 0.0f;
    // Where the picture is taken from, and how far from it a chain is still
    // stepped. Past it a body's chain is the animation's, rigid.
    core::DVec3 camera{};
    f32 maxDistance = 60.0f;
    // The solver's fixed step, seconds: `SpringStep`, or `SpringCoarseStep`
    // for half the work. Nought steps nothing -- every chain is the
    // animation's.
    f32 step = SpringStep;
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    scene::WindSettings wind{};
    // Seconds, for the wind's gusts.
    f32 time = 0.0f;
};

class SpringBones
{
public:
    // A frame: every chain that is on, seen and near is stepped and presented;
    // every other one is forgotten, so it starts from rest when it comes back.
    void update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses,
                const SpringFrame& frame);

    // Every chain forgotten: a scene changed, or the feature was turned off.
    void clear() noexcept { chains_.clear(); }

    // How many chains the last `update` stepped, and the joints in them.
    [[nodiscard]] core::u32 chainsStepped() const noexcept { return chainsStepped_; }
    [[nodiscard]] core::u32 jointsStepped() const noexcept { return jointsStepped_; }

private:
    struct Chain
    {
        core::InstanceId meshPart{};
        // The rig's joint the chain hangs from.
        core::u32 rootJoint = 0;
        core::u32 rigJoints = 0;
        // The rig's joint for each joint of the chain, ascending.
        std::vector<core::u32> joints;
        std::vector<SpringJoint> chain;
        SpringState state;
        // Total length in the rig's own units, for what counts as a jump.
        f32 length = 0.0f;
        // The frame it was last stepped in: one that was not is forgotten.
        core::u64 frame = 0;
    };

    std::unordered_map<core::u64, Chain> chains_;
    // Scratch for one instance's chain tops and its pattern's matches.
    std::vector<core::u32> roots_;
    std::vector<core::u8> matched_;
    core::u64 frame_ = 0;
    core::u32 chainsStepped_ = 0;
    core::u32 jointsStepped_ = 0;
};

} // namespace engine::render
