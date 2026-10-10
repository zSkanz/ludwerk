// A clip made on one body played on another by the animation system (ADR
// 0199): what reaches the pose.
//
// The roles and the map are `retarget_tests.cpp`'s. Here it is the walk that
// uses them: a clip from a library moves a body whose joints are called
// something else and whose legs are half as long; the body keeps its own
// bone lengths; its hips travel in proportion; and two files that are one
// skeleton are carried as they always were.
#include <cmath>
#include <doctest/doctest.h>
#include <initializer_list>
#include <optional>
#include <string>

#include "engine/asset/animation_graph.h"
#include "engine/render/animation.h"
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

render::SkeletonLibrary::Entry rigOf(std::initializer_list<Bone> bones)
{
    render::SkeletonLibrary::Entry entry;
    for (const Bone& bone : bones) {
        asset::Joint joint;
        joint.name = bone.name;
        joint.parent = bone.parent < 0 ? asset::Joint::NoParent : static_cast<core::u32>(bone.parent);
        joint.localBind.position = bone.offset;
        entry.joints.push_back(joint);
    }
    return entry;
}

// The body the clips were made on: the common free motion library's names,
// arms straight out, legs 0.9 long.
constexpr core::u32 SourceHips = 0;
constexpr core::u32 SourceLeftArm = 5;
constexpr core::u32 SourceLeftForeArm = 6;
render::SkeletonLibrary::Entry tall()
{
    return rigOf({
        {"Hips", -1, {0.0, 1.0, 0.0}},
        {"Spine", 0, {0.0, 0.2, 0.0}},
        {"Spine1", 1, {0.0, 0.2, 0.0}},
        {"Neck", 2, {0.0, 0.2, 0.0}},
        {"Head", 3, {0.0, 0.1, 0.0}},
        {"LeftArm", 2, {0.2, 0.15, 0.0}},
        {"LeftForeArm", 5, {0.3, 0.0, 0.0}},
        {"LeftHand", 6, {0.25, 0.0, 0.0}},
        {"RightArm", 2, {-0.2, 0.15, 0.0}},
        {"RightForeArm", 8, {-0.3, 0.0, 0.0}},
        {"RightHand", 9, {-0.25, 0.0, 0.0}},
        {"LeftUpLeg", 0, {0.1, -0.05, 0.0}},
        {"LeftLeg", 11, {0.0, -0.45, 0.0}},
        {"LeftFoot", 12, {0.0, -0.45, 0.0}},
        {"RightUpLeg", 0, {-0.1, -0.05, 0.0}},
        {"RightLeg", 14, {0.0, -0.45, 0.0}},
        {"RightFoot", 15, {0.0, -0.45, 0.0}},
    });
}

// Another body: a mannequin's names, a longer forearm, legs half as long,
// and its joints in another order.
constexpr core::u32 ShortPelvis = 0;
constexpr core::u32 ShortUpperArm = 9;
constexpr core::u32 ShortLowerArm = 10;
render::SkeletonLibrary::Entry stocky()
{
    return rigOf({
        {"pelvis", -1, {0.0, 0.55, 0.0}},
        {"thigh_l", 0, {0.1, -0.05, 0.0}},
        {"calf_l", 1, {0.0, -0.225, 0.0}},
        {"foot_l", 2, {0.0, -0.225, 0.0}},
        {"thigh_r", 0, {-0.1, -0.05, 0.0}},
        {"calf_r", 4, {0.0, -0.225, 0.0}},
        {"foot_r", 5, {0.0, -0.225, 0.0}},
        {"spine_01", 0, {0.0, 0.25, 0.0}},
        {"spine_02", 7, {0.0, 0.25, 0.0}},
        {"upperarm_l", 8, {0.25, 0.1, 0.0}},
        {"lowerarm_l", 9, {0.5, 0.0, 0.0}},
        {"hand_l", 10, {0.2, 0.0, 0.0}},
        {"upperarm_r", 8, {-0.25, 0.1, 0.0}},
        {"lowerarm_r", 12, {-0.5, 0.0, 0.0}},
        {"hand_r", 13, {-0.2, 0.0, 0.0}},
        {"neck_01", 8, {0.0, 0.2, 0.0}},
        {"head", 15, {0.0, 0.1, 0.0}},
    });
}

// The left arm raised a quarter turn about the model's forward axis -- from
// straight out to straight up -- with the forearm stretched to three times
// its length and the hips 0.4 above where they rest.
asset::AnimationClip raise()
{
    asset::AnimationClip clip;
    clip.name = "Raise";
    clip.duration = 1.0f;

    asset::AnimationChannel turn;
    turn.joint = SourceLeftArm;
    turn.target = asset::AnimationChannel::Target::Rotation;
    turn.stride = 4;
    turn.times = {0.0f, 1.0f};
    const f32 half = std::sqrt(0.5f);
    turn.values = {0.0f, 0.0f, half, half, 0.0f, 0.0f, half, half};
    clip.channels.push_back(turn);

    asset::AnimationChannel stretch;
    stretch.joint = SourceLeftForeArm;
    stretch.target = asset::AnimationChannel::Target::Translation;
    stretch.stride = 3;
    stretch.times = {0.0f, 1.0f};
    stretch.values = {0.9f, 0.0f, 0.0f, 0.9f, 0.0f, 0.0f};
    clip.channels.push_back(stretch);

    asset::AnimationChannel lift;
    lift.joint = SourceHips;
    lift.target = asset::AnimationChannel::Target::Translation;
    lift.stride = 3;
    lift.times = {0.0f, 1.0f};
    lift.values = {0.0f, 1.4f, 0.0f, 0.0f, 1.4f, 0.0f};
    clip.channels.push_back(lift);
    return clip;
}

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId meshPartClass = scene::InvalidClass;
    scene::ClassId playerClass = scene::InvalidClass;
    scene::ClassId bodyClass = scene::InvalidClass;

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

        scene::ClassDescriptor animator;
        animator.name = atoms.intern("AnimationPlayer");
        animator.super = instanceClass;
        animator.defaultName = animator.name;
        animator.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.animationPlayers().add(id, scene::AnimationPlayerComponent{});
        };
        animator.detachComponents = [](scene::World& w, core::InstanceId id) { w.animationPlayers().remove(id); };
        playerClass = classes.registerClass(animator);

        scene::ClassDescriptor character;
        character.name = atoms.intern("CharacterBody");
        character.super = instanceClass;
        character.defaultName = character.name;
        character.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.parts().add(id, scene::PartComponent{});
            w.characterBodies().add(id, scene::CharacterBodyComponent{});
        };
        character.detachComponents = [](scene::World& w, core::InstanceId id) {
            w.characterBodies().remove(id);
            w.parts().remove(id);
        };
        bodyClass = classes.registerClass(character);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    render::SkeletonLibrary skeletons;
    core::NameAtom clips = atoms.intern("asset://clips/library.glb");
    core::NameAtom model = atoms.intern("asset://models/hero.glb");
    core::InstanceId mesh;
    core::InstanceId player;

    void make(render::SkeletonLibrary::Entry library, render::SkeletonLibrary::Entry body)
    {
        library.clips.push_back(raise());
        skeletons.set(clips, std::move(library));
        skeletons.set(model, std::move(body));
        mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = model;
        player = world.create(playerClass);
        (void)world.setParent(player, mesh);
    }
};

[[nodiscard]] core::Vec3 placeOf(const core::Mat4& matrix)
{
    return core::Vec3{matrix.m[3][0], matrix.m[3][1], matrix.m[3][2]};
}

} // namespace

TEST_CASE("retargeting: a clip from a library moves a body of other names and proportions, which keeps its own")
{
    Fixture fixture;
    fixture.make(tall(), stocky());
    // Both are bodies by their names alone.
    const render::SkeletonLibrary::Entry from = tall();
    const render::SkeletonLibrary::Entry onto = stocky();
    const render::retarget::RigRoles fromRoles = render::retarget::assignRoles(from.joints);
    const render::retarget::RigRoles ontoRoles = render::retarget::assignRoles(onto.joints);
    std::string found;
    for (core::usize joint = 0; joint < from.joints.size(); ++joint)
        found +=
            from.joints[joint].name + "=" + std::string(render::retarget::roleName(fromRoles.ofJoint[joint])) + " ";
    for (core::usize joint = 0; joint < onto.joints.size(); ++joint)
        found +=
            onto.joints[joint].name + "=" + std::string(render::retarget::roleName(ontoRoles.ofJoint[joint])) + " ";
    INFO(found);
    REQUIRE(fromRoles.body());
    REQUIRE(ontoRoles.body());
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.player, fixture.clips, "Raise");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    // The arm is raised: its forearm is straight above its upper arm, where
    // the clip's rig has it -- and as far as THIS body's forearm is long,
    // though the clip stretched its own to three times that.
    const core::Vec3 upper = placeOf(pose->model[ShortUpperArm]);
    const core::Vec3 lower = placeOf(pose->model[ShortLowerArm]);
    CHECK(static_cast<double>(lower.x - upper.x) == doctest::Approx(0.0).epsilon(1.0e-4));
    CHECK(static_cast<double>(lower.y - upper.y) == doctest::Approx(0.5).epsilon(1.0e-4));
    CHECK(static_cast<double>(placeOf(pose->local[ShortLowerArm]).x) == doctest::Approx(0.5));

    // **The hips travel in proportion to the legs**: 0.4 on legs of 0.9 is
    // 0.2 on legs of 0.45, from where this body's hips rest.
    CHECK(static_cast<double>(placeOf(pose->local[ShortPelvis]).y) == doctest::Approx(0.55 + 0.2).epsilon(1.0e-4));
    // And the other arm, which the clip does not turn, is where it rests.
    const core::Vec3 right = placeOf(pose->model[13]) - placeOf(pose->model[12]);
    CHECK(static_cast<double>(right.x) == doctest::Approx(-0.5).epsilon(1.0e-4));
    CHECK(static_cast<double>(right.y) == doctest::Approx(0.0).epsilon(1.0e-4));
}

TEST_CASE("retargeting: by name, a body whose joints are called something else is not moved")
{
    Fixture fixture;
    fixture.make(tall(), stocky());
    // `Enum.Retargeting.ByName`.
    fixture.world.animationPlayers().find(fixture.player)->retargeting = 1;
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.player, fixture.clips, "Raise");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    const core::Vec3 arm = placeOf(pose->model[ShortLowerArm]) - placeOf(pose->model[ShortUpperArm]);
    CHECK(static_cast<double>(arm.x) == doctest::Approx(0.5));
    CHECK(static_cast<double>(arm.y) == doctest::Approx(0.0));
    CHECK(static_cast<double>(placeOf(pose->local[ShortPelvis]).y) == doctest::Approx(0.55));
}

TEST_CASE("retargeting: two files that are one skeleton are carried as they always were")
{
    // A body and the clips exported from the same rig as another file: the
    // same joints, named the same, resting the same. Nothing is carried from
    // rest to rest -- the clip's transforms are the rig's own, its stretch
    // and all.
    Fixture fixture;
    fixture.make(tall(), tall());
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.player, fixture.clips, "Raise");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    CHECK(static_cast<double>(placeOf(pose->local[SourceLeftForeArm]).x) == doctest::Approx(0.9));
    CHECK(static_cast<double>(placeOf(pose->local[SourceHips]).y) == doctest::Approx(1.4));
    const core::Vec3 arm = placeOf(pose->model[SourceLeftForeArm]) - placeOf(pose->model[SourceLeftArm]);
    CHECK(static_cast<double>(arm.x) == doctest::Approx(0.0).epsilon(1.0e-4));
    CHECK(static_cast<double>(arm.y) == doctest::Approx(0.9).epsilon(1.0e-4));
}

TEST_CASE("retargeting: a graph reads how fast a body goes in the strides of the body its clips were made on")
{
    // A walk is so many strides a second, and a stride is as long as the legs
    // that take it. A library's walk covers 1.5 metres a second on the body it
    // was made on; carried to a body whose legs are half as long -- the hips'
    // travel scaled with them, ADR 0199 -- the same clip covers 0.75. A graph
    // that places its clips by speed asked that short body's metres a second,
    // found "0.75" halfway to the walk, and played half a walk under a body
    // moving at the whole walk's pace: its feet slid.
    //
    // So a speed a graph reads of a body is the body's, over how much longer
    // or shorter its legs are than the library's: what the library's own body
    // would be doing to keep that step.
    Fixture fixture;
    render::SkeletonLibrary::Entry library = tall();
    for (const char* name : {"Idle", "Walk"}) {
        asset::AnimationClip clip = raise();
        clip.name = name;
        library.clips.push_back(clip);
    }
    fixture.skeletons.set(fixture.clips, std::move(library));
    const core::NameAtom shortModel = fixture.atoms.intern("asset://models/short.glb");
    fixture.skeletons.set(shortModel, stocky());
    const core::NameAtom sameModel = fixture.atoms.intern("asset://models/same.glb");
    fixture.skeletons.set(sameModel, tall());

    render::GraphLibrary graphs;
    const core::NameAtom graphContent = fixture.atoms.intern("asset://anim/hero.animgraph.json");
    asset::GraphReadError error;
    std::optional<asset::AnimationGraph> graph = asset::readAnimationGraph(R"({
      "format": "animgraph", "version": 1,
      "library": "asset://clips/library.glb",
      "parameters": { "Speed": { "number": 0, "from": "CharacterBody.Speed" },
                      "MoveX": { "number": 0, "from": "CharacterBody.MoveX" },
                      "MoveZ": { "number": 0, "from": "CharacterBody.MoveZ" },
                      "Rise": { "number": 0, "from": "CharacterBody.VerticalSpeed" } },
      "layers": [ { "name": "Body", "start": "Move",
        "states": { "Move": { "blend": "Speed",
                              "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.5 } ] } } } ]
    })",
                                                                           &error);
    REQUIRE_MESSAGE(graph.has_value(), error.where << ": " << error.what);
    graphs.set(graphContent, std::move(*graph), fixture.atoms);

    struct Walker
    {
        core::InstanceId body;
        core::InstanceId player;
    };
    const auto walker = [&](core::NameAtom model) {
        Walker made;
        made.body = fixture.world.create(fixture.bodyClass);
        const core::InstanceId mesh = fixture.world.create(fixture.meshPartClass);
        fixture.world.meshParts().find(mesh)->meshContent = model;
        (void)fixture.world.setParent(mesh, made.body);
        made.player = fixture.world.create(fixture.playerClass);
        (void)fixture.world.setParent(made.player, made.body);
        fixture.world.animationPlayers().find(made.player)->graph = graphContent;
        return made;
    };
    const Walker shortLegs = walker(shortModel);
    const Walker sameLegs = walker(sameModel);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&graphs);
    // Both at 0.75 metres a second: forwards and a little to the right, and
    // rising at one metre a second.
    constexpr double Tick = 1.0 / 60.0;
    for (int tick = 0; tick < 40; ++tick) {
        for (const Walker& one : {shortLegs, sameLegs}) {
            core::DVec3& at = fixture.world.parts().find(one.body)->cframe.position;
            at.z -= 0.6 * Tick;
            at.x += 0.45 * Tick;
            at.y += 1.0 * Tick;
        }
        animation.sample(Tick);
    }
    const auto read = [&](const Walker& one, const char* name) {
        return static_cast<double>(animation.graphParameter(one.player, name).value);
    };
    // The library's own body: metres a second, as they are.
    CHECK(read(sameLegs, "Speed") == doctest::Approx(0.75).epsilon(0.01));
    CHECK(read(sameLegs, "MoveZ") == doctest::Approx(0.6).epsilon(0.01));
    // Legs half as long: the whole of the library's walk.
    CHECK(read(shortLegs, "Speed") == doctest::Approx(1.5).epsilon(0.01));
    CHECK(read(shortLegs, "MoveZ") == doctest::Approx(1.2).epsilon(0.01));
    CHECK(read(shortLegs, "MoveX") == doctest::Approx(0.9).epsilon(0.01));
    // How fast it rises is a fall's and a jump's, which no leg takes.
    CHECK(read(shortLegs, "Rise") == doctest::Approx(1.0).epsilon(0.01));
}
