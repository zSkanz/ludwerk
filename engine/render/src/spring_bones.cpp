#include "engine/render/spring_bones.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

#include "engine/core/profile.h"
#include "engine/render/animation.h"
#include "engine/render/draw_poses.h"
#include "engine/scene/world.h"

namespace engine::render {

namespace {

using core::CFrameD;
using core::DVec3;
using core::Vec3;

[[nodiscard]] core::u64 keyOf(core::InstanceId id) noexcept
{
    return static_cast<core::u64>(id.index) | (static_cast<core::u64>(id.generation) << 32);
}

// A wind's speed as a push, metres a second squared for each metre a second:
// a cape in a ten metre wind is pushed about as hard as gravity pulls it.
constexpr f32 WindPush = 1.0f;

// Whether `name` is `pattern`, in which a `*` stands for any run of
// characters, none included. Letter for letter otherwise: joints are named
// by a file and matched as the file wrote them.
[[nodiscard]] bool matchesPattern(std::string_view pattern, std::string_view name) noexcept
{
    core::usize p = 0;
    core::usize n = 0;
    core::usize star = std::string_view::npos;
    core::usize resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        }
        else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        }
        else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++resume;
        }
        else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

// The frame a mesh is drawn in, and how many of the world's units one of its
// rig's is. A stretch that is not the same every way has no one answer for a
// chain that swings through all of them; the mean is the least wrong.
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

// A frame of the rig's model space, in the world.
[[nodiscard]] CFrameD inWorld(const Carrier& carrier, const CFrameD& model) noexcept
{
    CFrameD world;
    world.rotation = carrier.frame.rotation * model.rotation;
    world.position =
        carrier.frame.position + core::toDVec3(carrier.frame.rotation * (core::toVec3(model.position) * carrier.scale));
    return world;
}

} // namespace

void SpringBones::update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses,
                         const SpringFrame& frame)
{
    ENG_PROFILE_SCOPE("animation.springs");
    ++frame_;
    chainsStepped_ = 0;
    jointsStepped_ = 0;
    animation.clearPresented();
    if (!(frame.step > 0.0f) || world.springBones().size() == 0) {
        chains_.clear();
        return;
    }

    // What each mesh is presented with: its chains' joints together, since a
    // mesh is presented once.
    std::map<core::u64, std::pair<core::InstanceId, std::vector<AnimationSystem::PresentedJoint>>> presented;
    // The capsules of a mesh, gathered the first time one of its chains asks.
    std::map<core::u64, std::vector<SpringCapsule>> capsulesOf;

    world.springBones().forEach([&](core::InstanceId id, const scene::SpringBoneComponent& spring) {
        if (!spring.enabled || !world.alive(id))
            return;
        const core::InstanceId meshPart = world.parentOf(id);
        if (!meshPart.valid() || world.meshParts().find(meshPart) == nullptr)
            return;
        const core::u32 rigJoints = animation.jointCount(meshPart);
        if (rigJoints == 0 || !animation.seenLately(meshPart))
            return;
        const Carrier carrier = carrierOf(world, poses, meshPart);
        if (core::length(core::toVec3(carrier.frame.position - frame.camera)) > frame.maxDistance)
            return;
        // **The tops of its chains**: the joint it names, and every joint its
        // pattern matches that is not below another it matches -- so a
        // pattern that takes in a whole cape still finds only its columns'
        // tops, and each is a chain of its own.
        roots_.clear();
        if (const core::i32 named = animation.findJoint(meshPart, world.atoms().text(spring.rootJoint)); named >= 0)
            roots_.push_back(static_cast<core::u32>(named));
        if (const std::string_view pattern = world.atoms().text(spring.jointPattern); !pattern.empty()) {
            matched_.assign(rigJoints, 0);
            for (core::u32 joint = 0; joint < rigJoints; ++joint)
                matched_[joint] = matchesPattern(pattern, animation.jointName(meshPart, joint)) ? 1 : 0;
            for (core::u32 joint = 0; joint < rigJoints; ++joint) {
                if (matched_[joint] == 0)
                    continue;
                bool below = false;
                for (core::i32 up = animation.jointParent(meshPart, joint); up >= 0 && !below;
                     up = animation.jointParent(meshPart, static_cast<core::u32>(up)))
                    below = matched_[static_cast<core::usize>(up)] != 0;
                if (!below && std::find(roots_.begin(), roots_.end(), joint) == roots_.end())
                    roots_.push_back(joint);
            }
        }

        const auto stepFrom = [&](core::i32 root) {
            Chain& chain = chains_[keyOf(id) * 1000003ull + static_cast<core::u64>(root)];
            const bool fresh = chain.frame + 1 != frame_;
            if (!(chain.meshPart == meshPart) || chain.rootJoint != static_cast<core::u32>(root) ||
                chain.rigJoints != rigJoints) {
                // The rig's joints from `root` down, in the rig's own order --
                // which has every parent before its children.
                chain = Chain{};
                chain.meshPart = meshPart;
                chain.rootJoint = static_cast<core::u32>(root);
                chain.rigJoints = rigJoints;
                std::vector<core::i32> placeOf(rigJoints, -1);
                placeOf[static_cast<core::usize>(root)] = 0;
                chain.joints.push_back(static_cast<core::u32>(root));
                chain.chain.push_back(SpringJoint{});
                // Passes until one finds nothing: a file may list a child before
                // its parent, and the solver needs each parent placed first.
                for (bool found = true; found;) {
                    found = false;
                    for (core::u32 joint = 0; joint < rigJoints; ++joint) {
                        if (placeOf[joint] >= 0)
                            continue;
                        const core::i32 parent = animation.jointParent(meshPart, joint);
                        if (parent < 0 || placeOf[static_cast<core::usize>(parent)] < 0)
                            continue;
                        placeOf[joint] = static_cast<core::i32>(chain.chain.size());
                        SpringJoint link;
                        link.parent = placeOf[static_cast<core::usize>(parent)];
                        chain.joints.push_back(joint);
                        chain.chain.push_back(link);
                        found = true;
                    }
                }
            }
            else if (fresh) {
                // Back from being off screen or far away: from rest, where it is now.
                chain.state = SpringState{};
            }
            chain.frame = frame_;
            if (chain.chain.size() < 2)
                return;

            // This frame's animation: each joint from its parent, and the first
            // joint's own place -- from ITS parent's, which no chain moves.
            chain.length = 0.0f;
            for (core::usize at = 1; at < chain.chain.size(); ++at) {
                CFrameD local;
                if (!animation.jointLocal(meshPart, chain.joints[at], local))
                    return;
                chain.chain[at].localRotation = local.rotation;
                chain.chain[at].localOffset = core::toVec3(local.position);
                chain.length += core::length(chain.chain[at].localOffset);
            }
            CFrameD rootModel;
            if (!animation.jointLocal(meshPart, static_cast<core::u32>(root), rootModel))
                return;
            if (const core::i32 above = animation.jointParent(meshPart, static_cast<core::u32>(root)); above >= 0) {
                CFrameD parentModel;
                if (animation.jointModel(meshPart, static_cast<core::u32>(above), parentModel))
                    rootModel = parentModel * rootModel;
            }
            const CFrameD rootWorld = inWorld(carrier, rootModel);

            auto [capsules, gathered] = capsulesOf.try_emplace(keyOf(meshPart));
            if (gathered) {
                for (core::InstanceId child = world.firstChild(meshPart); child.valid();
                     child = world.nextSibling(child)) {
                    const scene::SpringColliderComponent* collider = world.springColliders().find(child);
                    if (collider == nullptr)
                        continue;
                    CFrameD model;
                    const core::i32 joint = animation.findJoint(meshPart, world.atoms().text(collider->jointName));
                    if (joint >= 0)
                        (void)animation.jointModel(meshPart, static_cast<core::u32>(joint), model);
                    CFrameD lower;
                    lower.position = core::toDVec3(collider->offset);
                    CFrameD upper;
                    upper.position = core::toDVec3(collider->offset + Vec3{0.0f, collider->length, 0.0f});
                    capsules->second.push_back(SpringCapsule{inWorld(carrier, model * lower).position,
                                                             inWorld(carrier, model * upper).position,
                                                             collider->radius * carrier.scale});
                }
            }

            const Vec3 wind = scene::windAt(frame.wind, core::toVec3(rootWorld.position), frame.time);
            const Vec3 acceleration = frame.gravity * spring.gravityScale + wind * (spring.windInfluence * WindPush);
            const SpringSettings settings{.stiffness = spring.stiffness,
                                          .damping = spring.damping,
                                          .inertia = spring.inertia,
                                          .limitAngle = spring.limitAngle,
                                          .radius = spring.radius};
            // Further in one frame than twice its own length is not a run or a
            // dash: it was put somewhere else.
            const f32 jump = std::max(chain.length * carrier.scale * 2.0f, 1.0f);
            {
                ENG_PROFILE_SCOPE("springs.solve");
                stepSpringChain(chain.chain, chain.state, rootWorld, settings, capsules->second, acceleration,
                                carrier.scale, frame.seconds, jump, frame.step);
            }
            ++chainsStepped_;
            jointsStepped_ += static_cast<core::u32>(chain.chain.size());

            // Back into the rig's own space, which is what a pose is in.
            const core::Mat3 toModel = core::transpose(carrier.frame.rotation);
            const f32 unscale = carrier.scale > 1e-6f ? 1.0f / carrier.scale : 1.0f;
            auto& out = presented[keyOf(meshPart)];
            out.first = meshPart;
            for (core::usize at = 0; at < chain.chain.size(); ++at) {
                CFrameD model;
                model.rotation = toModel * chain.chain[at].rotation;
                model.position = core::toDVec3(
                    (toModel * core::toVec3(chain.chain[at].position - carrier.frame.position)) * unscale);
                out.second.push_back({chain.joints[at], core::toRenderMatrix(model, DVec3{})});
            }
        };
        // A copy: the chains' own gathering uses the scratch lists too.
        const std::vector<core::u32> roots = roots_;
        for (const core::u32 root : roots)
            stepFrom(static_cast<core::i32>(root));
    });

    ENG_PROFILE_SCOPE("springs.present");
    for (auto& [key, mesh] : presented) {
        std::sort(mesh.second.begin(), mesh.second.end(),
                  [](const AnimationSystem::PresentedJoint& a, const AnimationSystem::PresentedJoint& b) {
                      return a.joint < b.joint;
                  });
        // Two chains that name one joint: the first is kept.
        mesh.second.erase(std::unique(mesh.second.begin(), mesh.second.end(),
                                      [](const AnimationSystem::PresentedJoint& a,
                                         const AnimationSystem::PresentedJoint& b) { return a.joint == b.joint; }),
                          mesh.second.end());
        animation.present(mesh.first, mesh.second);
    }

    // What was not reached this frame is forgotten, so it comes back at rest.
    std::erase_if(chains_, [this](const auto& entry) { return entry.second.frame != frame_; });
}

} // namespace engine::render
