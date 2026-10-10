// Limbs, looks and feet as instances (ADR 0198): what a frame is drawn with,
// and what the tick still reads.
//
// The solvers are held as numbers by `ik_tests.cpp`. Here it is what stands
// round them: a control finds its target where the target is DRAWN, the
// solved pose is the presented one and never the simulated one, a part held
// to a bone is drawn in the hand that was moved, a cape hangs from a shoulder
// a limb moved, and a foot is placed by a ray that does not find the
// character itself.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <initializer_list>
#include <vector>

#include "engine/render/animation.h"
#include "engine/render/draw_poses.h"
#include "engine/render/ik_controls.h"
#include "engine/render/spring_bones.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using engine::core::f32;

namespace {

struct Bone
{
    const char* name;
    int parent;
    core::DVec3 offset;
};

constexpr core::u32 Hips = 0;
constexpr core::u32 Head = 3;
constexpr core::u32 LeftArm = 4;
constexpr core::u32 LeftForeArm = 5;
constexpr core::u32 LeftHand = 6;
constexpr core::u32 RightHand = 9;
constexpr core::u32 LeftFoot = 12;
constexpr core::u32 RightFoot = 15;
constexpr core::u32 Cape = 16;

// A body that stands at the origin of its model with its feet on y = 0: arms
// straight out, the left along +X, so it faces +Z. A cape of two joints hangs
// from its left upper arm.
render::SkeletonLibrary::Entry body()
{
    render::SkeletonLibrary::Entry entry;
    for (const Bone& bone : std::initializer_list<Bone>{
             {"Hips", -1, {0.0, 1.0, 0.0}},
             {"Spine", 0, {0.0, 0.3, 0.0}},
             {"Neck", 1, {0.0, 0.3, 0.0}},
             {"Head", 2, {0.0, 0.1, 0.0}},
             {"LeftArm", 1, {0.2, 0.25, 0.0}},
             {"LeftForeArm", 4, {0.3, 0.0, 0.0}},
             {"LeftHand", 5, {0.3, 0.0, 0.0}},
             {"RightArm", 1, {-0.2, 0.25, 0.0}},
             {"RightForeArm", 7, {-0.3, 0.0, 0.0}},
             {"RightHand", 8, {-0.3, 0.0, 0.0}},
             {"LeftUpLeg", 0, {0.1, 0.0, 0.0}},
             {"LeftLeg", 10, {0.0, -0.45, 0.02}},
             {"LeftFoot", 11, {0.0, -0.45, -0.02}},
             {"RightUpLeg", 0, {-0.1, 0.0, 0.0}},
             {"RightLeg", 13, {0.0, -0.45, 0.02}},
             {"RightFoot", 14, {0.0, -0.45, -0.02}},
             {"Cape1", 4, {0.0, -0.2, -0.1}},
             {"Cape2", 16, {0.0, -0.3, 0.0}},
         }) {
        asset::Joint joint;
        joint.name = bone.name;
        joint.parent = bone.parent < 0 ? asset::Joint::NoParent : static_cast<core::u32>(bone.parent);
        joint.localBind.position = bone.offset;
        entry.joints.push_back(joint);
    }
    return entry;
}

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId meshPartClass = scene::InvalidClass;
    scene::ClassId partClass = scene::InvalidClass;
    scene::ClassId controlClass = scene::InvalidClass;
    scene::ClassId feetClass = scene::InvalidClass;
    scene::ClassId boneClass = scene::InvalidClass;
    scene::ClassId weldClass = scene::InvalidClass;
    scene::ClassId springClass = scene::InvalidClass;
    scene::ClassId swarmClass = scene::InvalidClass;

    template <class Attach, class Detach>
    [[nodiscard]] scene::ClassId make(const char* name, Attach attach, Detach detach)
    {
        scene::ClassDescriptor descriptor;
        descriptor.name = atoms.intern(name);
        descriptor.super = instanceClass;
        descriptor.defaultName = descriptor.name;
        descriptor.attachComponents = attach;
        descriptor.detachComponents = detach;
        return classes.registerClass(descriptor);
    }

    Fixture()
    {
        scene::ClassDescriptor instance;
        instance.name = atoms.intern("Instance");
        instance.defaultName = instance.name;
        instanceClass = classes.registerClass(instance);

        meshPartClass = make(
            "MeshPart",
            [](scene::World& w, core::InstanceId id) {
                w.parts().add(id, scene::PartComponent{});
                w.meshParts().add(id, scene::MeshPartComponent{});
            },
            [](scene::World& w, core::InstanceId id) {
                w.meshParts().remove(id);
                w.parts().remove(id);
            });
        partClass = make(
            "Part", [](scene::World& w, core::InstanceId id) { w.parts().add(id, scene::PartComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.parts().remove(id); });
        controlClass = make(
            "IKControl",
            [](scene::World& w, core::InstanceId id) { w.ikControls().add(id, scene::IKControlComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.ikControls().remove(id); });
        feetClass = make(
            "FootPlacement",
            [](scene::World& w, core::InstanceId id) { w.footPlacements().add(id, scene::FootPlacementComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.footPlacements().remove(id); });
        boneClass = make(
            "Bone", [](scene::World& w, core::InstanceId id) { w.attachments().add(id, scene::AttachmentComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.attachments().remove(id); });
        weldClass = make(
            "Weld", [](scene::World& w, core::InstanceId id) { w.welds().add(id, scene::WeldComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.welds().remove(id); });
        springClass = make(
            "SpringBone",
            [](scene::World& w, core::InstanceId id) { w.springBones().add(id, scene::SpringBoneComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.springBones().remove(id); });
        swarmClass = make(
            "Swarm", [](scene::World& w, core::InstanceId id) { w.swarms().add(id, scene::SwarmComponent{}); },
            [](scene::World& w, core::InstanceId id) { w.swarms().remove(id); });
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    render::SkeletonLibrary skeletons;
    render::AnimationSystem animation{world, skeletons};
    render::DrawPoses poses;
    render::IkControls limbs;
    core::NameAtom content = atoms.intern("asset://models/hero.glb");
    core::InstanceId mesh;

    // The body, standing `at` in the world.
    void stand(core::DVec3 at = {})
    {
        skeletons.set(content, body());
        mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = content;
        world.parts().find(mesh)->cframe.position = at;
        poses.begin(world, nullptr, 1.0f);
    }

    [[nodiscard]] core::InstanceId part(core::DVec3 at)
    {
        const core::InstanceId made = world.create(partClass);
        world.parts().find(made)->cframe.position = at;
        return made;
    }

    [[nodiscard]] scene::IKControlComponent& control(const char* endJoint, core::InstanceId target,
                                                     core::InstanceId* out = nullptr)
    {
        const core::InstanceId made = world.create(controlClass);
        (void)world.setParent(made, mesh);
        scene::IKControlComponent& component = *world.ikControls().find(made);
        component.endJoint = atoms.intern(endJoint);
        component.target = target;
        component.smoothing = 0.0f;
        if (out != nullptr)
            *out = made;
        return component;
    }

    [[nodiscard]] core::Vec3 drawn(core::u32 joint) const
    {
        const render::Pose* pose = animation.drawnPose(mesh);
        REQUIRE(pose != nullptr);
        return core::Vec3{pose->model[joint].m[3][0], pose->model[joint].m[3][1], pose->model[joint].m[3][2]};
    }

    [[nodiscard]] core::Vec3 simulated(core::u32 joint) const
    {
        core::CFrameD model;
        REQUIRE(animation.jointModel(mesh, joint, model));
        return core::toVec3(model.position);
    }

    void frame(const render::IkFrame& given)
    {
        poses.begin(world, nullptr, 1.0f);
        animation.clearPresented();
        limbs.update(world, animation, poses, given);
    }
};

[[nodiscard]] render::IkFrame near()
{
    render::IkFrame frame;
    frame.seconds = 1.0f / 60.0f;
    frame.maxDistance = 60.0f;
    return frame;
}

void closeTo(core::Vec3 got, core::Vec3 want, double within = 1.0e-3)
{
    CHECK(static_cast<double>(got.x) == doctest::Approx(static_cast<double>(want.x)).epsilon(within));
    CHECK(static_cast<double>(got.y) == doctest::Approx(static_cast<double>(want.y)).epsilon(within));
    CHECK(static_cast<double>(got.z) == doctest::Approx(static_cast<double>(want.z)).epsilon(within));
}

} // namespace

TEST_CASE("IK controls: a limb reaches its target in the pose that is drawn, and the tick's pose is as it was")
{
    Fixture fixture;
    fixture.stand(core::DVec3{10.0, 0.0, -4.0});
    // In front of the left shoulder and a little down, within the arm's
    // reach of 0.6: the shoulder is at (0.2, 1.55, 0) in the body's space.
    const core::Vec3 reach{0.4f, 1.4f, 0.35f};
    const core::InstanceId target = fixture.part(core::DVec3{10.0 + 0.4, 1.4, -4.0 + 0.35});
    (void)fixture.control("LeftHand", target);
    const core::Vec3 before = fixture.simulated(LeftHand);

    fixture.frame(near());
    CHECK(fixture.limbs.controlsSolved() == 1);
    closeTo(fixture.drawn(LeftHand), reach);
    // The shoulder is where the clip has it; the elbow is not.
    closeTo(fixture.drawn(LeftArm), fixture.simulated(LeftArm));
    CHECK(core::length(fixture.drawn(LeftForeArm) - fixture.simulated(LeftForeArm)) > 0.05f);
    // **And nothing the tick reads moved.**
    closeTo(fixture.simulated(LeftHand), before);
    CHECK(fixture.animation.pose(fixture.mesh) == nullptr);

    // A body further than the frame reaches is left to its clip.
    render::IkFrame far = near();
    far.maxDistance = 5.0f;
    far.camera = core::DVec3{100.0, 0.0, 0.0};
    fixture.frame(far);
    CHECK(fixture.limbs.controlsSolved() == 0);
    CHECK(fixture.animation.presentedMeshes().empty());
}

TEST_CASE("IK controls: a control's weight and its smoothing ease it in, and off it does nothing")
{
    Fixture fixture;
    fixture.stand();
    const core::InstanceId target = fixture.part(core::DVec3{0.4, 1.4, 0.35});
    scene::IKControlComponent& control = fixture.control("LeftHand", target);
    const core::Vec3 clip = fixture.simulated(LeftHand);
    const core::Vec3 reach{0.4f, 1.4f, 0.35f};

    // Half applied: halfway from where the clip has the hand.
    control.weight = 0.5f;
    fixture.frame(near());
    closeTo(fixture.drawn(LeftHand), clip + (reach - clip) * 0.5f, 5.0e-3);

    // Eased over a quarter of a second: after one frame, a little of the way.
    control.weight = 1.0f;
    control.enabled = false;
    fixture.frame(near());
    control.enabled = true;
    control.smoothing = 0.25f;
    fixture.frame(near());
    fixture.frame(near());
    const f32 early = core::length(fixture.drawn(LeftHand) - clip);
    CHECK(early > 0.0f);
    CHECK(early < core::length(reach - clip) * 0.75f);
    for (int frame = 0; frame < 240; ++frame)
        fixture.frame(near());
    closeTo(fixture.drawn(LeftHand), reach, 5.0e-3);

    // Off: back to the clip, eased the same way, and then nothing presented.
    control.enabled = false;
    for (int frame = 0; frame < 480; ++frame)
        fixture.frame(near());
    CHECK(fixture.animation.presentedMeshes().empty());
}

TEST_CASE("IK controls: a head looks at what it is given, the way the body itself faces")
{
    Fixture fixture;
    fixture.stand();
    // Level with the head (y = 1.7) and to the body's left of straight
    // ahead: ahead is +Z for this rig, by where its shoulders are.
    const core::InstanceId target = fixture.part(core::DVec3{2.0, 1.7, 2.0});
    scene::IKControlComponent& control = fixture.control("Head", target);
    control.type = 1;
    control.chainLength = 0;
    control.maxAngle = 80.0f;

    fixture.frame(near());
    REQUIRE(fixture.limbs.controlsSolved() == 1);
    const render::Pose* pose = fixture.animation.drawnPose(fixture.mesh);
    REQUIRE(pose != nullptr);
    // The head's own +Z -- the way it faced at rest -- now points at the
    // target: forty-five degrees to the left.
    const core::Vec3 facing =
        core::normalize(core::transformDirection(pose->model[Head], core::Vec3{0.0f, 0.0f, 1.0f}));
    closeTo(facing, core::normalize(core::Vec3{2.0f, 0.0f, 2.0f}), 5.0e-3);

    // Past its limit it stops at the limit: a target straight behind turns
    // the head by `MaxAngle` and no further.
    fixture.world.parts().find(target)->cframe.position = core::DVec3{0.5, 1.7, -3.0};
    control.maxAngle = 60.0f;
    fixture.frame(near());
    const render::Pose* limited = fixture.animation.drawnPose(fixture.mesh);
    REQUIRE(limited != nullptr);
    const core::Vec3 turned =
        core::normalize(core::transformDirection(limited->model[Head], core::Vec3{0.0f, 0.0f, 1.0f}));
    CHECK(static_cast<double>(std::acos(std::clamp(turned.z, -1.0f, 1.0f))) ==
          doctest::Approx(60.0 * 3.14159265 / 180.0).epsilon(0.01));
}

TEST_CASE("IK controls: what is welded to a bone is drawn in the hand that was moved, and simulated where it was")
{
    Fixture fixture;
    fixture.stand(core::DVec3{3.0, 0.0, 0.0});
    const core::InstanceId target = fixture.part(core::DVec3{3.0 + 0.4, 1.4, 0.35});
    (void)fixture.control("LeftHand", target);

    // A bone on the hand, as the tick resolved it; a sword welded to the
    // bone, and a charm welded to the sword.
    const core::InstanceId bone = fixture.world.create(fixture.boneClass);
    (void)fixture.world.setParent(bone, fixture.mesh);
    scene::AttachmentComponent& attachment = *fixture.world.attachments().find(bone);
    attachment.jointName = fixture.atoms.intern("LeftHand");
    attachment.jointIndex = static_cast<core::i32>(LeftHand);
    const core::Vec3 hand = fixture.simulated(LeftHand);
    attachment.worldCFrame.position = core::DVec3{3.0, 0.0, 0.0} + core::toDVec3(hand);
    const core::InstanceId sword = fixture.part(attachment.worldCFrame.position + core::DVec3{0.0, 0.0, 0.5});
    const core::InstanceId charm = fixture.part(attachment.worldCFrame.position + core::DVec3{0.0, 0.2, 0.5});
    const auto weld = [&](core::InstanceId a, core::InstanceId b) {
        const core::InstanceId made = fixture.world.create(fixture.weldClass);
        scene::WeldComponent& component = *fixture.world.welds().find(made);
        component.part0 = a;
        component.part1 = b;
    };
    weld(bone, sword);
    weld(sword, charm);
    const core::DVec3 swordWas = fixture.world.parts().find(sword)->cframe.position;

    fixture.frame(near());
    render::IkControls::carryHeld(fixture.world, fixture.animation, fixture.poses);

    // The hand moved by this much, in the world; so did the bone, the sword
    // and the charm -- the turn of the hand about itself aside, which a
    // reach with no `AlignRotation` leaves as the clip had it.
    const core::Vec3 moved = fixture.drawn(LeftHand) - hand;
    REQUIRE(core::length(moved) > 0.05f);
    const core::Vec3 boneMoved =
        core::toVec3(fixture.poses.attachment(bone).position - attachment.worldCFrame.position);
    closeTo(boneMoved, moved, 2.0e-2);
    const core::Vec3 swordMoved = core::toVec3(fixture.poses.part(sword).position - swordWas);
    CHECK(core::length(swordMoved) > 0.05f);
    const core::Vec3 charmMoved =
        core::toVec3(fixture.poses.part(charm).position - fixture.world.parts().find(charm)->cframe.position);
    CHECK(core::length(charmMoved) > 0.05f);
    // **The sword's simulated place did not move**: a blade that hits is the
    // tick's.
    CHECK(fixture.world.parts().find(sword)->cframe.position.x == swordWas.x);
    CHECK(fixture.world.parts().find(sword)->cframe.position.y == swordWas.y);
    CHECK(fixture.world.parts().find(sword)->cframe.position.z == swordWas.z);
    // The body itself is drawn where it is.
    CHECK(fixture.poses.part(fixture.mesh).position.x == 3.0);
}

TEST_CASE("IK controls: a cape hangs from the arm a limb moved, and the limb stays where it was put")
{
    Fixture fixture;
    fixture.stand();
    const core::InstanceId target = fixture.part(core::DVec3{0.25, 2.1, 0.1});
    (void)fixture.control("LeftHand", target);
    const core::InstanceId spring = fixture.world.create(fixture.springClass);
    (void)fixture.world.setParent(spring, fixture.mesh);
    fixture.world.springBones().find(spring)->rootJoint = fixture.atoms.intern("Cape1");

    render::SpringBones chains;
    render::SpringFrame swing;
    swing.seconds = 1.0f / 60.0f;
    swing.afterLimbs = true;
    for (int frame = 0; frame < 3; ++frame) {
        fixture.frame(near());
        chains.update(fixture.world, fixture.animation, fixture.poses, swing);
    }
    CHECK(chains.chainsStepped() == 1);
    // The hand is where the limb put it: the chain's presenting was on top of
    // the limb's, not instead of it.
    closeTo(fixture.drawn(LeftHand), core::Vec3{0.25f, 2.1f, 0.1f}, 5.0e-3);
    // And the cape's top is on the upper arm AS DRAWN -- which the reach
    // turned upward -- not where the clip has that arm.
    const render::Pose* pose = fixture.animation.drawnPose(fixture.mesh);
    REQUIRE(pose != nullptr);
    const core::Vec3 expected = core::transformPoint(pose->model[LeftArm], core::Vec3{0.0f, -0.2f, -0.1f});
    closeTo(fixture.drawn(Cape), expected, 2.0e-2);
    CHECK(core::length(fixture.drawn(Cape) - fixture.simulated(Cape)) > 0.05f);
}

TEST_CASE("IK controls: feet are put on the ground the rays find, the hips come down, and a swarm's body has none")
{
    Fixture fixture;
    fixture.stand();
    const core::InstanceId feet = fixture.world.create(fixture.feetClass);
    (void)fixture.world.setParent(feet, fixture.mesh);
    scene::FootPlacementComponent& placement = *fixture.world.footPlacements().find(feet);
    placement.leftFoot = fixture.atoms.intern("LeftFoot");
    placement.rightFoot = fixture.atoms.intern("RightFoot");
    placement.hips = fixture.atoms.intern("Hips");
    placement.footHeight = 0.1f;
    placement.stepHeight = 0.4f;

    // The rig's ankles rest at y = 0.1, so the clips' ground is y = 0. The
    // ground here is flat at 0 under the left foot and a step DOWN of 0.2
    // under the right.
    std::vector<core::InstanceId> excluded;
    render::IkFrame frame = near();
    frame.ground = [&](core::DVec3 from, core::Vec3 direction, std::span<const core::InstanceId> own,
                       core::DVec3& point, core::Vec3& normal) {
        excluded.assign(own.begin(), own.end());
        const double ground = from.x > 0.0 ? 0.0 : -0.2;
        // The ray starts a step above the clips' ground and looks two down.
        CHECK(from.y == doctest::Approx(0.5).epsilon(1.0e-4));
        CHECK(static_cast<double>(direction.y) < -0.8);
        point = core::DVec3{from.x, ground, from.z};
        normal = core::Vec3{0.0f, 1.0f, 0.0f};
        return true;
    };
    fixture.frame(frame);
    CHECK(fixture.limbs.raysCast() == 2);
    CHECK(fixture.limbs.feetPlaced() >= 1);
    // The character's own parts are what a ray must not find.
    REQUIRE(excluded.size() == 1);
    CHECK(excluded[0] == fixture.mesh);
    // The low foot is on its ground, the hips came down by the step, and the
    // high foot is still on its own -- its knee bent.
    CHECK(static_cast<double>(fixture.drawn(RightFoot).y) == doctest::Approx(-0.2 + 0.1).epsilon(5.0e-3));
    CHECK(static_cast<double>(fixture.drawn(Hips).y) == doctest::Approx(1.0 - 0.2).epsilon(5.0e-3));
    CHECK(static_cast<double>(fixture.drawn(LeftFoot).y) == doctest::Approx(0.1).epsilon(5.0e-3));
    // The tick's feet are where the clip has them.
    CHECK(static_cast<double>(fixture.simulated(RightFoot).y) == doctest::Approx(0.1));

    // No feet where the frame says none -- the lowest quality.
    render::IkFrame none = frame;
    none.feet = false;
    fixture.frame(none);
    CHECK(fixture.limbs.raysCast() == 0);
    CHECK(fixture.animation.presentedMeshes().empty());

    // **And none for a swarm's agent**: a horde's feet are its clips'.
    const core::InstanceId swarm = fixture.world.create(fixture.swarmClass);
    scene::SwarmAgent agent;
    agent.body = fixture.mesh;
    agent.alive = true;
    fixture.world.swarms().find(swarm)->agents.push_back(agent);
    fixture.frame(frame);
    CHECK(fixture.limbs.raysCast() == 0);
}

TEST_CASE("IK controls: what a frame of them costs, for one character and for a hundred")
{
    // Each character has all of it: a head that looks, a hand that reaches
    // and two feet on the ground. The time is printed, not held to a number
    // -- a machine running the rest of the suite beside this is not a
    // measurement -- and the counts are held.
    for (const int characters : {1, 100}) {
        Fixture fixture;
        fixture.skeletons.set(fixture.content, body());
        const core::InstanceId target = fixture.part(core::DVec3{0.5, 1.5, 3.0});
        for (int index = 0; index < characters; ++index) {
            fixture.mesh = fixture.world.create(fixture.meshPartClass);
            fixture.world.meshParts().find(fixture.mesh)->meshContent = fixture.content;
            fixture.world.parts().find(fixture.mesh)->cframe.position =
                core::DVec3{static_cast<double>(index % 10) * 2.0, 0.0, static_cast<double>(index / 10) * 2.0};
            scene::IKControlComponent& look = fixture.control("Head", target);
            look.type = 1;
            (void)fixture.control("LeftHand", target);
            const core::InstanceId feet = fixture.world.create(fixture.feetClass);
            (void)fixture.world.setParent(feet, fixture.mesh);
            scene::FootPlacementComponent& placement = *fixture.world.footPlacements().find(feet);
            placement.leftFoot = fixture.atoms.intern("LeftFoot");
            placement.rightFoot = fixture.atoms.intern("RightFoot");
            placement.hips = fixture.atoms.intern("Hips");
        }
        render::IkFrame frame = near();
        frame.maxDistance = 1000.0f;
        frame.ground = [](core::DVec3 from, core::Vec3, std::span<const core::InstanceId>, core::DVec3& point,
                          core::Vec3& normal) {
            point = core::DVec3{from.x, std::sin(from.x * 3.0) * 0.1, from.z};
            normal = core::Vec3{0.0f, 1.0f, 0.0f};
            return true;
        };
        std::vector<double> micros;
        for (int run = 0; run < 60; ++run) {
            fixture.poses.begin(fixture.world, nullptr, 1.0f);
            fixture.animation.clearPresented();
            const auto begun = std::chrono::steady_clock::now();
            fixture.limbs.update(fixture.world, fixture.animation, fixture.poses, frame);
            render::IkControls::carryHeld(fixture.world, fixture.animation, fixture.poses);
            micros.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begun).count());
        }
        std::sort(micros.begin(), micros.end());
        MESSAGE(characters << " character(s): a frame of looks, limbs and feet, in microseconds: median "
                           << micros[micros.size() / 2] << ", worst " << micros.back());
        CHECK(fixture.limbs.controlsSolved() == static_cast<core::u32>(characters) * 2);
        CHECK(fixture.limbs.raysCast() == static_cast<core::u32>(characters) * 2);
        CHECK(fixture.animation.presentedMeshes().size() == static_cast<core::usize>(characters));
    }
}

TEST_CASE("IK controls: a hand eased onto something the body carries stays on it while the body walks")
{
    // `Smoothing` eased the place a limb reaches for in the WORLD, so a hand
    // on something that moves with the body -- a weapon, a rail of a cart it
    // rides -- trailed behind it by the body's speed times the smoothing:
    // a tenth of a metre at a walk, with the default. It is eased where the
    // body is, and a thing that keeps its place by the body is reached.
    Fixture fixture;
    fixture.stand();
    const core::InstanceId target = fixture.part(core::DVec3{0.4, 1.4, 0.35});
    scene::IKControlComponent& control = fixture.control("LeftHand", target);
    control.smoothing = 0.25f;
    // Both walk on together, 1.2 metres a second.
    for (int frame = 0; frame < 240; ++frame) {
        fixture.world.parts().find(fixture.mesh)->cframe.position.x += 0.02;
        fixture.world.parts().find(target)->cframe.position.x += 0.02;
        fixture.frame(near());
    }
    // In the body's own space the hand is on the target: (0.4, 1.4, 0.35).
    closeTo(fixture.drawn(LeftHand), core::Vec3{0.4f, 1.4f, 0.35f}, 5.0e-3);
}

TEST_CASE("IK controls: a hand reaches a thing the same body holds where the frame draws that thing")
{
    // A staff held in two hands: welded to a bone on the right hand, and the
    // left hand put on a grip further along it. The feet lower the hips for a
    // step down, the right hand goes down with the body, and the staff is
    // DRAWN down with the hand -- but the left hand reached for the grip
    // where the tick had left it, a step's height above.
    Fixture fixture;
    fixture.stand();
    const core::InstanceId feet = fixture.world.create(fixture.feetClass);
    (void)fixture.world.setParent(feet, fixture.mesh);
    scene::FootPlacementComponent& placement = *fixture.world.footPlacements().find(feet);
    placement.leftFoot = fixture.atoms.intern("LeftFoot");
    placement.rightFoot = fixture.atoms.intern("RightFoot");
    placement.hips = fixture.atoms.intern("Hips");

    // The bone on the right hand, as the tick resolved it, and the staff on it.
    const core::InstanceId bone = fixture.world.create(fixture.boneClass);
    (void)fixture.world.setParent(bone, fixture.mesh);
    scene::AttachmentComponent& held = *fixture.world.attachments().find(bone);
    held.jointName = fixture.atoms.intern("RightHand");
    held.jointIndex = static_cast<core::i32>(RightHand);
    const core::Vec3 hand = fixture.simulated(RightHand);
    held.worldCFrame.position = core::toDVec3(hand);
    const core::InstanceId staff = fixture.part(core::toDVec3(hand));
    const core::InstanceId weld = fixture.world.create(fixture.weldClass);
    fixture.world.welds().find(weld)->part0 = bone;
    fixture.world.welds().find(weld)->part1 = staff;
    // The grip: along the staff, within the left arm's reach.
    const core::InstanceId grip = fixture.world.create(fixture.boneClass);
    (void)fixture.world.setParent(grip, staff);
    scene::AttachmentComponent& gripAt = *fixture.world.attachments().find(grip);
    gripAt.cframe.position = core::DVec3{0.7, -0.1, 0.3};
    gripAt.worldCFrame.position = core::toDVec3(hand) + gripAt.cframe.position;
    (void)fixture.control("LeftHand", grip);

    // Both feet find the ground a fifth of a metre down: the whole body
    // comes down by it.
    render::IkFrame frame = near();
    frame.ground = [](core::DVec3 from, core::Vec3, std::span<const core::InstanceId>, core::DVec3& point,
                      core::Vec3& normal) {
        point = core::DVec3{from.x, -0.2, from.z};
        normal = core::Vec3{0.0f, 1.0f, 0.0f};
        return true;
    };
    fixture.frame(frame);
    render::IkControls::carryHeld(fixture.world, fixture.animation, fixture.poses);

    REQUIRE(static_cast<double>(fixture.drawn(Hips).y) == doctest::Approx(0.8).epsilon(5.0e-3));
    // The staff is drawn a fifth of a metre down, with the hand that holds it...
    const core::Vec3 drawnGrip = core::toVec3(fixture.poses.attachment(grip).position);
    CHECK(static_cast<double>(drawnGrip.y) == doctest::Approx(1.55 - 0.1 - 0.2).epsilon(5.0e-3));
    // ...and the left hand is on the grip as it is drawn.
    closeTo(fixture.drawn(LeftHand), drawnGrip, 5.0e-3);
}
