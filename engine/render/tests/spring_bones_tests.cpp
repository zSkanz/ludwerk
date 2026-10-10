// The capes of a world (ADR 0194): everything between a `SpringBone` in the
// tree and the chain solver, and the one promise the design rests on -- that
// what a chain moves is the pose a frame is DRAWN with and never the one the
// simulation reads.
//
// **The world here is a world being edited**: a mesh with a rig, nothing
// playing, no script. That is what the editor draws when a cape is parented to
// a hero by hand, and it is the case a run of the example never reaches -- its
// figures are moved by a script, and the editor has no window to open in a
// test. So a still body in the wind is the fixture: if the chain moves here it
// moves in the editor's viewport, by the same call.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <string>

#include "engine/render/animation.h"
#include "engine/render/draw_poses.h"
#include "engine/render/spring_bones.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using core::f32;

namespace {

// A body joint and, under it, columns of four joints hanging a third of a
// unit apart: `Cape_<column><row>`.
render::SkeletonLibrary::Entry capedRig(std::initializer_list<const char*> columns)
{
    render::SkeletonLibrary::Entry entry;
    asset::Joint body;
    body.name = "Body";
    body.parent = asset::Joint::NoParent;
    entry.joints.push_back(body);

    f32 x = 0.0f;
    for (const char* column : columns) {
        for (int row = 0; row < 4; ++row) {
            asset::Joint joint;
            joint.name = std::string("Cape_") + column + std::to_string(row);
            joint.parent = row == 0 ? 0u : static_cast<core::u32>(entry.joints.size() - 1);
            // The top sits on the shoulders; each one below hangs from the last.
            joint.localBind.position =
                row == 0 ? core::DVec3{static_cast<core::f64>(x), 1.6, 0.2} : core::DVec3{0.0, -0.3, 0.0};
            entry.joints.push_back(joint);
        }
        x += 0.25f;
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

    Fixture()
    {
        scene::ClassDescriptor instance;
        instance.name = atoms.intern("Instance");
        instance.defaultName = instance.name;
        instanceClass = classes.registerClass(instance);

        scene::ClassDescriptor meshPart;
        meshPart.name = atoms.intern("MeshPart");
        meshPart.super = instanceClass;
        meshPart.defaultName = meshPart.name;
        meshPart.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.meshParts().add(id, scene::MeshPartComponent{});
        };
        meshPart.detachComponents = [](scene::World& w, core::InstanceId id) { w.meshParts().remove(id); };
        meshPartClass = classes.registerClass(meshPart);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    render::SkeletonLibrary skeletons;
    core::NameAtom content = atoms.intern("asset://models/hero.glb");
    core::InstanceId mesh;

    void rig(render::SkeletonLibrary::Entry entry)
    {
        skeletons.set(content, std::move(entry));
        mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = content;
    }

    // A `SpringBone` under the mesh, as the instance would be.
    core::InstanceId spring(const scene::SpringBoneComponent& says)
    {
        const core::InstanceId id = world.create(instanceClass);
        (void)world.setParent(id, mesh);
        world.springBones().add(id, says);
        return id;
    }

    // A `SpringCollider` under the mesh, as the instance would be.
    core::InstanceId collider(const scene::SpringColliderComponent& says)
    {
        const core::InstanceId id = world.create(instanceClass);
        (void)world.setParent(id, mesh);
        world.springColliders().add(id, says);
        return id;
    }

    // The wind the frames below blow: from the left unless a case says.
    core::Vec3 wind{9.0f, 0.0f, 0.0f};

    // Frames in that wind, a sixtieth of a second each.
    void frames(render::SpringBones& springs, render::AnimationSystem& animation, int count,
                f32 step = render::SpringStep)
    {
        render::DrawPoses poses;
        for (int at = 0; at < count; ++at) {
            poses.begin(world, nullptr, 1.0f);
            render::SpringFrame frame;
            frame.seconds = 1.0f / 60.0f;
            frame.wind.global = wind;
            frame.time = static_cast<f32>(at) / 60.0f;
            frame.step = step;
            springs.update(world, animation, poses, frame);
        }
    }
};

// Where a joint is, in the rig's own space, in the pose a frame is drawn with.
[[nodiscard]] core::Vec3 drawnAt(const render::AnimationSystem& animation, core::InstanceId mesh, core::usize joint)
{
    const render::Pose* pose = animation.drawnPose(mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(joint < pose->model.size());
    return core::transformPoint(pose->model[joint], core::Vec3{});
}

} // namespace

TEST_CASE("a cape on a still body with nothing playing moves in the wind, in the pose that is drawn")
{
    Fixture fixture;
    fixture.rig(capedRig({"M"}));
    scene::SpringBoneComponent says;
    says.rootJoint = fixture.atoms.intern("Cape_M0");
    // A light cloth, so the wind shows: at the default stiffness the tip
    // moves two centimetres, which is right and is not much of a proof.
    says.stiffness = 0.02f;
    (void)fixture.spring(says);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::SpringBones springs;
    fixture.frames(springs, animation, 120);

    CHECK(springs.chainsStepped() == 1);
    CHECK(springs.jointsStepped() == 4);

    // The tip, three joints down, has gone with the wind: at rest it hangs
    // straight under the top, at x = 0.
    const core::Vec3 tip = drawnAt(animation, fixture.mesh, 4);
    CHECK(tip.x > 0.1f);
    // And every joint is still its length from the one above.
    for (core::usize joint = 2; joint <= 4; ++joint) {
        const core::Vec3 link = drawnAt(animation, fixture.mesh, joint) - drawnAt(animation, fixture.mesh, joint - 1);
        CHECK(core::length(link) == doctest::Approx(0.3).epsilon(0.01));
    }
    // The top is where the animation has it: it turns, it does not move.
    const core::Vec3 top = drawnAt(animation, fixture.mesh, 1);
    CHECK(top.x == doctest::Approx(0.0).epsilon(0.001));
    CHECK(top.y == doctest::Approx(1.6).epsilon(0.001));

    // **What the simulation reads is untouched.** Nothing is playing, so the
    // mesh has no pose of its own at all, and a joint asked for is at rest.
    CHECK(animation.pose(fixture.mesh) == nullptr);
    core::CFrameD asked;
    REQUIRE(animation.jointModel(fixture.mesh, 4, asked));
    CHECK(asked.position.x == doctest::Approx(0.0));
    CHECK(asked.position.y == doctest::Approx(0.7).epsilon(0.001));
}

TEST_CASE("a cape that is switched off, or whose joint the rig lacks, is drawn as the animation has it")
{
    Fixture fixture;
    fixture.rig(capedRig({"M"}));
    scene::SpringBoneComponent says;
    says.rootJoint = fixture.atoms.intern("Cape_M0");
    const core::InstanceId id = fixture.spring(says);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::SpringBones springs;
    fixture.frames(springs, animation, 30);
    REQUIRE(springs.chainsStepped() == 1);

    // Off: from the next frame nothing is presented, and the drawn pose is
    // the mesh's own -- none, here.
    fixture.world.springBones().find(id)->enabled = false;
    fixture.frames(springs, animation, 1);
    CHECK(springs.chainsStepped() == 0);
    CHECK(animation.drawnPose(fixture.mesh) == nullptr);

    // A name the rig does not have moves nothing, and is not an error.
    fixture.world.springBones().find(id)->enabled = true;
    fixture.world.springBones().find(id)->rootJoint = fixture.atoms.intern("Tail_0");
    fixture.frames(springs, animation, 5);
    CHECK(springs.chainsStepped() == 0);

    // No time at all in a frame, and no step: the same.
    fixture.world.springBones().find(id)->rootJoint = fixture.atoms.intern("Cape_M0");
    fixture.frames(springs, animation, 5, 0.0f);
    CHECK(springs.chainsStepped() == 0);
    CHECK(animation.drawnPose(fixture.mesh) == nullptr);
}

TEST_CASE("one SpringBone with a pattern is a chain for each column of a cape")
{
    Fixture fixture;
    fixture.rig(capedRig({"L", "M", "R"}));
    render::AnimationSystem animation{fixture.world, fixture.skeletons};

    // The tops by name.
    {
        scene::SpringBoneComponent says;
        says.jointPattern = fixture.atoms.intern("Cape_*0");
        says.stiffness = 0.02f;
        const core::InstanceId id = fixture.spring(says);
        render::SpringBones springs;
        fixture.frames(springs, animation, 60);
        CHECK(springs.chainsStepped() == 3);
        CHECK(springs.jointsStepped() == 12);
        // Every column's tip went with the wind.
        for (const core::usize tip : {core::usize{4}, core::usize{8}, core::usize{12}})
            CHECK(drawnAt(animation, fixture.mesh, tip).x > drawnAt(animation, fixture.mesh, tip - 3).x + 0.05f);
        fixture.world.springBones().remove(id);
    }

    // **Every joint of the cape matched**: a joint below another the pattern
    // matches belongs to that one's chain, so the tops are the same three and
    // no joint is in two chains.
    {
        scene::SpringBoneComponent says;
        says.jointPattern = fixture.atoms.intern("Cape_*");
        (void)fixture.spring(says);
        render::SpringBones springs;
        fixture.frames(springs, animation, 60);
        CHECK(springs.chainsStepped() == 3);
        CHECK(springs.jointsStepped() == 12);
    }
}

TEST_CASE("D607: a cape found by a pattern is kept out of its mesh's colliders, links and all")
{
    // The owner's cape hung inside his character. Part of that was the
    // example -- one thin capsule under a wide block -- and part was the
    // solver, held by its own test. This holds what lies between them: that a
    // `SpringCollider` under the mesh reaches every chain of the mesh, the ones
    // a PATTERN found as much as one a `RootJoint` named; that it is where its
    // joint is; and that what is drawn is clear of it.
    //
    // Three columns a quarter apart hang a fifth behind the body's axis. The
    // capsule stands on the middle column's line, as tall as the cape; a gale
    // blows the cape straight at it.
    Fixture fixture;
    fixture.rig(capedRig({"L", "M", "R"}));
    scene::SpringBoneComponent says;
    says.jointPattern = fixture.atoms.intern("Cape_*0");
    says.stiffness = 0.02f;
    says.radius = 0.05f;
    (void)fixture.spring(says);

    scene::SpringColliderComponent body;
    body.jointName = fixture.atoms.intern("Body");
    body.offset = core::Vec3{0.25f, 0.3f, 0.0f};
    body.length = 1.4f;
    body.radius = 0.15f;
    (void)fixture.collider(body);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::SpringBones springs;
    fixture.wind = core::Vec3{0.0f, 0.0f, -40.0f};

    // How far inside the capsule a point of the rig's space is.
    const auto depth = [&](core::Vec3 point) {
        const core::Vec3 from = body.offset;
        const core::Vec3 axis{0.0f, body.length, 0.0f};
        const core::Vec3 to = point - from;
        const f32 along = std::clamp(core::dot(to, axis) / core::dot(axis, axis), 0.0f, 1.0f);
        return (body.radius + says.radius) - core::length(to - axis * along);
    };

    f32 worst = -1.0e9f;
    for (int round = 0; round < 6; ++round) {
        fixture.frames(springs, animation, 30);
        REQUIRE(springs.chainsStepped() == 3);
        // Every joint of every column, and nine points along the link above it.
        for (core::usize column = 0; column < 3; ++column) {
            for (core::usize row = 1; row < 4; ++row) {
                const core::usize joint = 1 + column * 4 + row;
                const core::Vec3 above = drawnAt(animation, fixture.mesh, joint - 1);
                const core::Vec3 here = drawnAt(animation, fixture.mesh, joint);
                for (int step = 1; step <= 10; ++step)
                    worst = std::max(worst, depth(above + (here - above) * (static_cast<f32>(step) / 10.0f)));
            }
        }
    }
    CHECK(worst < 0.002f);

    // **And it is ON the body**: the middle column, which the capsule stands
    // behind, is held a thickness off it and no further -- pressed there by
    // the gale, not hanging where it was hung and not flung clear.
    const core::Vec3 pressed = drawnAt(animation, fixture.mesh, 1 + 4 + 2);
    CHECK(pressed.z < 0.21f + 0.03f);
    CHECK(pressed.z > 0.15f);
    // The outer columns are a quarter to either side of the capsule's line,
    // further than it and the cape are thick: the gale carries them past.
    CHECK(drawnAt(animation, fixture.mesh, 1 + 0 + 3).z < 0.0f);
    CHECK(drawnAt(animation, fixture.mesh, 1 + 8 + 3).z < 0.0f);
}
