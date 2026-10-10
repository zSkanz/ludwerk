// An animation graph played by the animation system (ADR 0197): what the
// graph's states and layers make of a pose, joint by joint.
//
// The graph alone -- states, conditions, fades, events -- is held by
// `graph_player_tests.cpp`. Here it is the part the pose walk gained: a track
// that weighs a joint by a mask, a layer that replaces what is under it, a
// clip that adds, and the things round them a game relies on (a crowd still
// shares its poses; the world's hash moves with a graph's state; a trigger
// fires on a change and not on a first sight).
#include <cmath>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/asset/animation_graph.h"
#include "engine/render/animation.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using engine::core::f32;
using engine::core::f64;

namespace {

constexpr f64 Tick = 1.0 / 60.0;

// A clip that holds one joint at `x` along X for a second, and a second
// joint with it when asked: numbers a pose can be read back as.
asset::AnimationChannel held(core::u32 joint, f32 x, f32 y)
{
    asset::AnimationChannel channel;
    channel.joint = joint;
    channel.target = asset::AnimationChannel::Target::Translation;
    channel.stride = 3;
    channel.times = {0.0f, 1.0f};
    channel.values = {x, y, 0.0f, x, y, 0.0f};
    return channel;
}

constexpr core::u32 Legs = 1;
constexpr core::u32 Spine = 2;
constexpr core::u32 Arm = 3;

// A root, legs under it, a spine under it and an arm under the spine. Each
// rests one up from its parent, so a clip's X is all that moves.
render::SkeletonLibrary::Entry body()
{
    render::SkeletonLibrary::Entry entry;
    const auto joint = [&](const char* name, core::u32 parent) {
        asset::Joint made;
        made.name = name;
        made.parent = parent;
        if (parent != asset::Joint::NoParent)
            made.localBind.position = core::DVec3{0.0, 1.0, 0.0};
        entry.joints.push_back(made);
    };
    joint("Root", asset::Joint::NoParent);
    joint("Legs", 0);
    joint("Spine", 0);
    joint("Arm", 2);

    const auto clip = [&](const char* name, f32 x) {
        asset::AnimationClip made;
        made.name = name;
        made.duration = 1.0f;
        made.channels.push_back(held(Legs, x, 1.0f));
        made.channels.push_back(held(Arm, x, 1.0f));
        entry.clips.push_back(made);
    };
    clip("Idle", 0.0f);
    clip("Walk", 1.0f);
    clip("Run", 3.0f);
    clip("Slash", 10.0f);

    // What a lean adds: the spine goes from where it rests to five along X
    // over its second.
    asset::AnimationClip lean;
    lean.name = "Lean";
    lean.duration = 1.0f;
    asset::AnimationChannel leaning;
    leaning.joint = Spine;
    leaning.target = asset::AnimationChannel::Target::Translation;
    leaning.stride = 3;
    leaning.times = {0.0f, 1.0f};
    leaning.values = {0.0f, 1.0f, 0.0f, 5.0f, 1.0f, 0.0f};
    lean.channels.push_back(leaning);
    entry.clips.push_back(lean);
    return entry;
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

        scene::ClassDescriptor player;
        player.name = atoms.intern("AnimationPlayer");
        player.super = instanceClass;
        player.defaultName = player.name;
        player.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.animationPlayers().add(id, scene::AnimationPlayerComponent{});
        };
        player.detachComponents = [](scene::World& w, core::InstanceId id) { w.animationPlayers().remove(id); };
        playerClass = classes.registerClass(player);

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
    render::GraphLibrary graphs;
    core::NameAtom rigContent = atoms.intern("asset://models/hero.glb");
    core::NameAtom graphContent = atoms.intern("asset://anim/hero.animgraph.json");

    void load(std::string_view json)
    {
        asset::GraphReadError error;
        std::optional<asset::AnimationGraph> graph = asset::readAnimationGraph(json, &error);
        REQUIRE_MESSAGE(graph.has_value(), error.where << ": " << error.what);
        graphs.set(graphContent, std::move(*graph), atoms);
    }

    struct Hero
    {
        core::InstanceId mesh;
        core::InstanceId player;
    };
    // A mesh on the rig with a player under it that names the graph.
    [[nodiscard]] Hero hero(core::InstanceId under = {})
    {
        if (skeletons.find(rigContent) == nullptr)
            skeletons.set(rigContent, body());
        Hero made;
        made.mesh = world.create(meshPartClass);
        world.meshParts().find(made.mesh)->meshContent = rigContent;
        if (under.valid())
            (void)world.setParent(made.mesh, under);
        made.player = world.create(playerClass);
        (void)world.setParent(made.player, made.mesh);
        world.animationPlayers().find(made.player)->graph = graphContent;
        return made;
    }
};

// A joint's place from its parent, as the pose has it.
[[nodiscard]] core::Vec3 placeOf(const render::AnimationSystem& animation, core::InstanceId mesh, core::u32 joint)
{
    const render::Pose* pose = animation.pose(mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(joint < pose->local.size());
    return core::Vec3{pose->local[joint].m[3][0], pose->local[joint].m[3][1], pose->local[joint].m[3][2]};
}

void run(render::AnimationSystem& animation, int ticks)
{
    for (int tick = 0; tick < ticks; ++tick)
        animation.sample(Tick);
}

const char* const Moving = R"({
  "format": "animgraph", "version": 1,
  "parameters": { "Speed": { "number": 0 }, "Attack": { "trigger": true }, "Aim": { "number": 0 },
                  "Upper": { "number": 1 } },
  "layers": [
    { "name": "Body", "start": "Move",
      "states": { "Move": { "blend": "Speed", "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 2 },
                                                         { "clip": "Run", "at": 6 } ] } } },
    { "name": "Arms", "mask": [ "Spine" ], "weight": "Upper", "start": "None",
      "states": { "None": {}, "Slash": { "clip": "Slash" } },
      "transitions": [ { "from": "None", "to": "Slash", "when": [ ["Attack"] ], "fade": 0 },
                       { "from": "Slash", "to": "None", "when": [ ["Speed", ">", 100] ], "fade": 0 } ] },
    { "name": "Lean", "additive": true, "weight": "Aim", "start": "Lean",
      "states": { "Lean": { "clip": "Lean", "loop": false } } }
  ]
})";

} // namespace

TEST_CASE("graph: a blend along a parameter poses the joints between its two clips, in proportion")
{
    Fixture fixture;
    fixture.load(Moving);
    const Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);

    // Standing still: the idle alone.
    run(animation, 2);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(0.0));

    // Halfway from a walk (2) to a run (6): halfway from 1 to 3.
    CHECK(animation.setGraphParameter(hero.player, "Speed", 4.0f) == scene::GraphWrite::Done);
    run(animation, 1);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(2.0));
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Arm).x) == doctest::Approx(2.0));
    // What no clip of it moves rests.
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Spine).y) == doctest::Approx(1.0));

    // A name the graph does not declare is refused, once it has loaded.
    CHECK(animation.setGraphParameter(hero.player, "Sped", 1.0f) == scene::GraphWrite::Unknown);
    const scene::GraphParameterValue speed = animation.graphParameter(hero.player, "Speed");
    CHECK(speed.kind == scene::GraphParameterValue::Kind::Number);
    CHECK(static_cast<double>(speed.value) == doctest::Approx(4.0));
    CHECK(animation.graphState(hero.player, "") == "Move");
    CHECK(animation.graphState(hero.player, "Arms") == "None");
}

TEST_CASE("graph: a masked layer takes the joints it names and leaves the legs to the layer under it")
{
    Fixture fixture;
    fixture.load(Moving);
    const Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    (void)animation.setGraphParameter(hero.player, "Speed", 2.0f);
    run(animation, 2);
    REQUIRE(static_cast<double>(placeOf(animation, hero.mesh, Arm).x) == doctest::Approx(1.0));

    // The attack: its clip moves the legs and the arm to ten, and its layer
    // is masked from the spine down the arm.
    (void)animation.setGraphParameter(hero.player, "Attack", 1.0f);
    run(animation, 2);
    CHECK(animation.graphState(hero.player, "Arms") == "Slash");
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Arm).x) == doctest::Approx(10.0));
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(1.0));

    // **A layer's weight is a cross-fade with what is under it**: at a half,
    // the arm is halfway from the walk's one to the slash's ten.
    (void)animation.setGraphParameter(hero.player, "Upper", 0.5f);
    run(animation, 1);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Arm).x) == doctest::Approx(5.5));
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(1.0));
    // And at nought the layer is not there.
    (void)animation.setGraphParameter(hero.player, "Upper", 0.0f);
    run(animation, 1);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Arm).x) == doctest::Approx(1.0));
}

TEST_CASE("graph: an additive clip adds how far it is from its first frame, and nothing at its start")
{
    Fixture fixture;
    fixture.load(Moving);
    const Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);

    // At weight nought it is not there, however far its clip has played.
    run(animation, 31);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Spine).x) == doctest::Approx(0.0));

    // At weight one: half a second in, half of its five, on top of the rest
    // place -- and the joints it has no channel for are as they were.
    (void)animation.setGraphParameter(hero.player, "Aim", 1.0f);
    run(animation, 1);
    const core::Vec3 spine = placeOf(animation, hero.mesh, Spine);
    CHECK(static_cast<double>(spine.x) == doctest::Approx(5.0 * 32.0 / 60.0).epsilon(0.02));
    CHECK(static_cast<double>(spine.y) == doctest::Approx(1.0));
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(0.0));

    // At a half, half of that.
    (void)animation.setGraphParameter(hero.player, "Aim", 0.5f);
    run(animation, 1);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Spine).x) ==
          doctest::Approx(0.5 * 5.0 * 33.0 / 60.0).epsilon(0.02));
}

TEST_CASE("graph: a state with no clip in the first layer weighs the rest pose while another fades in")
{
    Fixture fixture;
    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "parameters": { "Go": { "trigger": true } },
      "layers": [ { "name": "Body", "start": "None",
        "states": { "None": {}, "Run": { "clip": "Run" } },
        "transitions": [ { "from": "None", "to": "Run", "when": [ ["Go"] ], "fade": 1.0 } ] } ]
    })");
    const Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    run(animation, 2);
    // Nothing plays: no pose at all, which is the rest pose.
    CHECK(animation.pose(hero.mesh) == nullptr);

    (void)animation.setGraphParameter(hero.player, "Go", 1.0f);
    run(animation, 31);
    // Half a second into a fade of one: half of the run's three -- not the
    // whole of it from the first tick, which is what one track at any weight
    // alone would be.
    const double x = static_cast<double>(placeOf(animation, hero.mesh, Legs).x);
    CHECK(x > 1.2);
    CHECK(x < 1.8);
    run(animation, 40);
    CHECK(static_cast<double>(placeOf(animation, hero.mesh, Legs).x) == doctest::Approx(3.0));
}

TEST_CASE("graph: a crowd on one rig in one state at one moment still shares one pose")
{
    Fixture fixture;
    fixture.load(Moving);
    std::vector<Fixture::Hero> crowd;
    for (int index = 0; index < 8; ++index)
        crowd.push_back(fixture.hero());
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    for (const Fixture::Hero& hero : crowd) {
        (void)animation.setGraphParameter(hero.player, "Speed", 2.0f);
        (void)animation.setGraphParameter(hero.player, "Attack", 1.0f);
    }
    run(animation, 3);
    const core::u64 built = animation.posesBuilt();
    const core::u64 shared = animation.posesShared();
    run(animation, 1);
    // One built, seven taken from it: the mask and the layer are in what a
    // pose is told apart by, and are alike for all eight.
    CHECK(animation.posesBuilt() - built == 1);
    CHECK(animation.posesShared() - shared == 7);
    CHECK(animation.pose(crowd.front().mesh) == animation.pose(crowd.back().mesh));

    // One of them in another state is its own.
    (void)animation.setGraphParameter(crowd.back().player, "Upper", 0.25f);
    run(animation, 1);
    CHECK(animation.pose(crowd.front().mesh) != animation.pose(crowd.back().mesh));
    CHECK(static_cast<double>(placeOf(animation, crowd.back().mesh, Arm).x) == doctest::Approx(1.0 + 9.0 * 0.25));
}

TEST_CASE("graph: its state is a digest the world's hash can read, and it moves when the state does")
{
    Fixture fixture;
    fixture.load(Moving);
    const Fixture::Hero one = fixture.hero();
    const Fixture::Hero two = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    run(animation, 3);

    std::vector<std::pair<core::InstanceId, core::u64>> digests;
    animation.graphDigests(digests);
    REQUIRE(digests.size() == 2);
    CHECK(digests[0].first == one.player);
    CHECK(digests[1].first == two.player);
    // Two graphs with one history: one digest.
    CHECK(digests[0].second == digests[1].second);
    CHECK(digests[0].second != 0);

    (void)animation.setGraphParameter(two.player, "Attack", 1.0f);
    run(animation, 1);
    digests.clear();
    animation.graphDigests(digests);
    REQUIRE(digests.size() == 2);
    CHECK(digests[0].second != digests[1].second);
}

TEST_CASE("graph: what it did is said once, by name, and a parameter set before its file arrived is kept")
{
    Fixture fixture;
    const Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);

    // The file is not there yet: the write is kept, a read has nothing.
    CHECK(animation.setGraphParameter(hero.player, "Go", 1.0f) == scene::GraphWrite::Done);
    CHECK(animation.graphParameter(hero.player, "Go").kind == scene::GraphParameterValue::Kind::None);
    CHECK(animation.graphState(hero.player, "").empty());
    run(animation, 2);
    CHECK(animation.drainGraphSignals().empty());

    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "parameters": { "Go": { "trigger": true } },
      "events": { "Run": [ { "at": 0.5, "name": "Step" } ] },
      "layers": [ { "name": "Body", "start": "Idle",
        "states": { "Idle": { "clip": "Idle" }, "Run": { "clip": "Run" } },
        "transitions": [ { "from": "Idle", "to": "Run", "when": [ ["Go"] ], "fade": 0 } ] } ]
    })");
    // The trigger kept from before fires now.
    run(animation, 1);
    std::vector<std::string> said;
    const auto hear = [&] {
        for (const scene::GraphSignal& signal : animation.drainGraphSignals()) {
            CHECK(signal.player == hero.player);
            said.push_back(signal.event ? std::string("event ") + std::string(signal.to)
                                        : std::string(signal.layer) + " " + std::string(signal.from) + ">" +
                                              std::string(signal.to));
        }
    };
    hear();
    run(animation, 1);
    hear();
    REQUIRE(said.size() == 1);
    CHECK(said[0] == "Body Idle>Run");
    // Drained once.
    CHECK(animation.drainGraphSignals().empty());

    // And the clip's event, when the clip passes it: once a loop.
    said.clear();
    for (int tick = 0; tick < 60; ++tick) {
        run(animation, 1);
        hear();
    }
    REQUIRE(said.size() == 1);
    CHECK(said[0] == "event Step");
}

TEST_CASE("graph: a trigger read from an attribute fires when the attribute changes, not when it is first seen")
{
    Fixture fixture;
    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "parameters": { "Attack": { "trigger": true, "from": "Attribute.Attack" } },
      "layers": [ { "name": "Arms", "start": "None",
        "states": { "None": {}, "Slash": { "clip": "Slash", "loop": false } },
        "transitions": [ { "from": "None", "to": "Slash", "when": [ ["Attack"] ], "fade": 0 },
                         { "from": "Slash", "to": "None", "after": 1.0, "fade": 0 } ] } ]
    })");
    // The attribute is on what the player is parented to, and already set:
    // an attack that happened before this machine saw the character.
    const Fixture::Hero hero = fixture.hero();
    const core::NameAtom attack = fixture.atoms.intern("Attack");
    (void)fixture.world.setAttribute(hero.mesh, attack, scene::Value{7.0});
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    run(animation, 5);
    CHECK(animation.graphState(hero.player, "") == "None");

    // The authority sets it to something it was not: every machine fires.
    (void)fixture.world.setAttribute(hero.mesh, attack, scene::Value{8.0});
    run(animation, 1);
    CHECK(animation.graphState(hero.player, "") == "Slash");
    // Unchanged, it does not fire again when the slash is over.
    run(animation, 90);
    CHECK(animation.graphState(hero.player, "") == "None");
    run(animation, 30);
    CHECK(animation.graphState(hero.player, "") == "None");
}

TEST_CASE("graph: how fast its body goes is read from where the body was, on any machine")
{
    Fixture fixture;
    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "parameters": { "Speed": { "number": 0, "from": "CharacterBody.Speed" },
                      "MoveZ": { "number": 0, "from": "CharacterBody.MoveZ" },
                      "Grounded": { "boolean": true, "from": "CharacterBody.Grounded" } },
      "layers": [ { "name": "Body", "start": "Move",
        "states": { "Move": { "blend": "Speed", "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Run", "at": 6 } ] } } } ]
    })");
    const core::InstanceId character = fixture.world.create(fixture.bodyClass);
    const Fixture::Hero hero = fixture.hero(character);
    // The player on the body itself, as a character is made.
    (void)fixture.world.setParent(hero.player, character);
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);

    // Six metres a second, forward: a tenth of a metre a tick along -Z.
    for (int tick = 0; tick < 30; ++tick) {
        fixture.world.parts().find(character)->cframe.position.z -= 0.1;
        animation.sample(Tick);
    }
    CHECK(static_cast<double>(animation.graphParameter(hero.player, "Speed").value) ==
          doctest::Approx(6.0).epsilon(0.01));
    CHECK(static_cast<double>(animation.graphParameter(hero.player, "MoveZ").value) ==
          doctest::Approx(6.0).epsilon(0.01));
    CHECK(animation.graphParameter(hero.player, "Grounded").kind == scene::GraphParameterValue::Kind::Boolean);
    CHECK(static_cast<double>(animation.graphParameter(hero.player, "Grounded").value) == doctest::Approx(0.0));

    // **A script's value wins until it is handed back.**
    (void)animation.setGraphParameter(hero.player, "Speed", 1.0f);
    fixture.world.parts().find(character)->cframe.position.z -= 0.1;
    animation.sample(Tick);
    CHECK(static_cast<double>(animation.graphParameter(hero.player, "Speed").value) == doctest::Approx(1.0));
    (void)animation.clearGraphParameter(hero.player, "Speed");
    fixture.world.parts().find(character)->cframe.position.z -= 0.1;
    animation.sample(Tick);
    CHECK(static_cast<double>(animation.graphParameter(hero.player, "Speed").value) ==
          doctest::Approx(6.0).epsilon(0.01));
}

TEST_CASE("graph: a clip in a file no mesh wears is asked for, and a player that goes gives its tracks back")
{
    Fixture fixture;
    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "library": "asset://clips/humanoid.glb",
      "layers": [ { "name": "Body", "start": "Idle",
        "states": { "Idle": { "clip": "Idle" }, "Wave": { "clip": "asset://clips/extra.glb#Wave" } } } ]
    })");
    Fixture::Hero hero = fixture.hero();
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);
    run(animation, 1);
    // Both files, each once, in atom order.
    const std::span<const core::NameAtom> wanted = animation.wantedClipFiles();
    REQUIRE(wanted.size() == 2);
    CHECK(fixture.atoms.text(wanted[0]) == "asset://clips/humanoid.glb");
    CHECK(fixture.atoms.text(wanted[1]) == "asset://clips/extra.glb");

    // The library arrives: its clip plays on the hero's rig, by joint name.
    render::SkeletonLibrary::Entry library = body();
    fixture.skeletons.set(fixture.atoms.intern("asset://clips/humanoid.glb"), std::move(library));
    run(animation, 2);
    CHECK(animation.wantedClipFiles().size() == 1);
    CHECK(animation.pose(hero.mesh) != nullptr);

    // Heroes come and go; the tracks their graphs made are used again.
    const core::usize slots = animation.trackSlots();
    for (int round = 0; round < 20; ++round) {
        REQUIRE(fixture.world.destroy(hero.mesh));
        fixture.world.retireDestroyed();
        animation.retire(fixture.world);
        hero = fixture.hero();
        run(animation, 2);
    }
    CHECK(animation.trackSlots() == slots);
}

TEST_CASE("graph: a character that was just made does not start with a jump")
{
    // A body says whether it is on the ground after the physics has stepped
    // it, and the graph is stepped before the physics in a tick: on the tick
    // a character is made, "not on the ground" is what a body that has never
    // been asked says. A graph with the plainest rule there is -- in the air
    // is a jump -- played one tick of the jump for every character that
    // appeared, standing on the floor.
    Fixture fixture;
    fixture.load(R"({
      "format": "animgraph", "version": 1,
      "parameters": { "Grounded": { "boolean": true, "from": "CharacterBody.Grounded" } },
      "layers": [ { "name": "Body", "start": "Move",
        "states": { "Move": { "clip": "Idle" }, "Jump": { "clip": "Run" } },
        "transitions": [ { "from": "Move", "to": "Jump", "when": [ ["Grounded", "==", false] ], "fade": 0 },
                         { "from": "Jump", "to": "Move", "when": [ ["Grounded", "==", true] ], "fade": 0 } ] } ]
    })");
    const core::InstanceId character = fixture.world.create(fixture.bodyClass);
    const Fixture::Hero hero = fixture.hero(character);
    (void)fixture.world.setParent(hero.player, character);
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.setGraphs(&fixture.graphs);

    // The tick it is made in: nothing has stepped it, and it says "false".
    REQUIRE_FALSE(fixture.world.characterBodies().find(character)->grounded);
    animation.sample(Tick);
    CHECK(animation.graphState(hero.player, "") == "Move");
    CHECK(animation.drainGraphSignals().empty());
    // The physics then finds the floor under it.
    fixture.world.characterBodies().find(character)->grounded = true;
    run(animation, 3);
    CHECK(animation.graphState(hero.player, "") == "Move");
    CHECK(animation.drainGraphSignals().empty());

    // And a body that IS in the air, a tick later, is.
    fixture.world.characterBodies().find(character)->grounded = false;
    run(animation, 1);
    CHECK(animation.graphState(hero.player, "") == "Jump");
}
