// A mesh's morph weights as the animation system keeps them (ADR 0196): the
// file's own, moved by the clips playing on the mesh, with a script's value
// over both -- and what a body that is not using any of it costs.

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <span>
#include <string>

#include "engine/render/animation.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using core::f32;

namespace {

[[nodiscard]] bool close(f32 a, f32 b) noexcept
{
    return std::fabs(a - b) <= 1e-4f;
}

// A clip of one second that takes target `target` from `from` to `to`.
[[nodiscard]] asset::AnimationClip weightClip(const char* name, core::u32 target, f32 from, f32 to)
{
    asset::AnimationChannel channel;
    channel.joint = target;
    channel.target = asset::AnimationChannel::Target::Weight;
    channel.stride = 1;
    channel.times = {0.0f, 1.0f};
    channel.values = {from, to};

    asset::AnimationClip clip;
    clip.name = name;
    clip.duration = 1.0f;
    clip.weights.push_back(channel);
    return clip;
}

// A face: three targets, the third at a quarter in its file, and no skeleton.
[[nodiscard]] render::SkeletonLibrary::Entry face()
{
    render::SkeletonLibrary::Entry entry;
    entry.morphNames = {"Smile", "Blink", "Jaw"};
    entry.morphDefaults = {0.0f, 0.0f, 0.25f};
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

    // A mesh of `content` under `parent`, or under nothing.
    [[nodiscard]] core::InstanceId meshOf(core::NameAtom content, core::InstanceId parent = {})
    {
        const core::InstanceId mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = content;
        if (parent.valid())
            (void)world.setParent(mesh, parent);
        return mesh;
    }

    [[nodiscard]] core::InstanceId playerUnder(core::InstanceId parent)
    {
        const core::InstanceId player = world.create(instanceClass);
        (void)world.setParent(player, parent);
        return player;
    }
};

// Sixty ticks a second, as the simulation has them.
void advance(render::AnimationSystem& animation, int ticks)
{
    for (int tick = 0; tick < ticks; ++tick)
        animation.sample(1.0 / 60.0);
}

} // namespace

TEST_CASE("a mesh nothing plays a weight on, and no script set one on, is drawn by its file alone")
{
    // **What a horde pays**: an empty answer, before anything is looked up.
    // The renderer takes the file's own weights for it -- which for nearly
    // every target of every mesh are nought, and the body stays in its run.
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/face.glb");
    render::SkeletonLibrary::Entry entry = face();
    // A clip that moves no weight: a body's walk.
    asset::AnimationClip walk;
    walk.name = "Walk";
    walk.duration = 1.0f;
    entry.clips.push_back(walk);
    fixture.skeletons.set(content, std::move(entry));
    const core::InstanceId mesh = fixture.meshOf(content);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    CHECK(animation.drawnMorphWeights(mesh).empty());

    const scene::TrackId track = animation.createTrack(fixture.playerUnder(mesh), {}, "Walk");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    advance(animation, 10);
    CHECK(animation.drawnMorphWeights(mesh).empty());

    // And what a script reads of it is the file's.
    CHECK(animation.morphTargetCount(mesh) == 3);
    CHECK(animation.morphTargetName(mesh, 1) == "Blink");
    CHECK(animation.morphTargetName(mesh, 3).empty());
    CHECK(close(animation.morphWeight(mesh, "Jaw"), 0.25f));
    CHECK(close(animation.morphWeight(mesh, "Smile"), 0.0f));
    CHECK(close(animation.morphWeight(mesh, "NoSuchTarget"), 0.0f));
}

TEST_CASE("a clip's weight channel moves its target, on a mesh with no skeleton, and leaves the others to the file")
{
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/face.glb");
    render::SkeletonLibrary::Entry entry = face();
    entry.clips.push_back(weightClip("Smiling", 0, 0.0f, 1.0f));
    fixture.skeletons.set(content, std::move(entry));
    const core::InstanceId mesh = fixture.meshOf(content);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.playerUnder(mesh), {}, "Smiling");
    REQUIRE(close(animation.state(track).length, 1.0f));
    animation.play(track, 0.0f, 1.0f, 1.0f);

    advance(animation, 30);
    const std::span<const f32> half = animation.drawnMorphWeights(mesh);
    REQUIRE(half.size() == 3);
    CHECK(close(half[0], 0.5f));
    CHECK(close(half[1], 0.0f));
    // The clip says nothing of the jaw: the file's quarter stands.
    CHECK(close(half[2], 0.25f));
    CHECK(close(animation.morphWeight(mesh, "Smile"), 0.5f));

    // At its end the clip holds its last key, as a joint's does.
    advance(animation, 60);
    CHECK(close(animation.drawnMorphWeights(mesh)[0], 1.0f));

    // Stopped, the mesh is the file's again -- and says so by saying nothing.
    animation.stop(track, 0.0f);
    advance(animation, 1);
    CHECK(animation.drawnMorphWeights(mesh).empty());
}

TEST_CASE("a clip fading in eases its weights in from the file's, and two clips are averaged")
{
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/face.glb");
    render::SkeletonLibrary::Entry entry = face();
    // Both hold the jaw: one wide open, one shut.
    entry.clips.push_back(weightClip("Open", 2, 1.0f, 1.0f));
    entry.clips.push_back(weightClip("Shut", 2, 0.0f, 0.0f));
    fixture.skeletons.set(content, std::move(entry));
    const core::InstanceId mesh = fixture.meshOf(content);
    const core::InstanceId player = fixture.playerUnder(mesh);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId open = animation.createTrack(player, {}, "Open");
    animation.setLooped(open, true);
    // Half a track is half the way from the file's quarter to one.
    animation.play(open, 0.0f, 0.5f, 1.0f);
    advance(animation, 1);
    CHECK(close(animation.drawnMorphWeights(mesh)[2], 0.5f * 1.0f + 0.5f * 0.25f));

    // A whole one is the clip's value.
    animation.adjustWeight(open, 1.0f, 0.0f);
    advance(animation, 1);
    CHECK(close(animation.drawnMorphWeights(mesh)[2], 1.0f));

    // Two whole tracks on one target are averaged by their weights: open
    // and shut is half open.
    const scene::TrackId shut = animation.createTrack(player, {}, "Shut");
    animation.setLooped(shut, true);
    animation.play(shut, 0.0f, 1.0f, 1.0f);
    advance(animation, 1);
    CHECK(close(animation.drawnMorphWeights(mesh)[2], 0.5f));
}

TEST_CASE("a script's weight wins over the clip while it is set, and clearing it gives the target back")
{
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/face.glb");
    render::SkeletonLibrary::Entry entry = face();
    entry.clips.push_back(weightClip("Smiling", 0, 0.4f, 0.4f));
    fixture.skeletons.set(content, std::move(entry));
    const core::InstanceId mesh = fixture.meshOf(content);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.playerUnder(mesh), {}, "Smiling");
    animation.setLooped(track, true);
    animation.play(track, 0.0f, 1.0f, 1.0f);
    advance(animation, 5);
    REQUIRE(close(animation.drawnMorphWeights(mesh)[0], 0.4f));

    // Past one and below nought are weights like any other.
    animation.setMorphWeight(mesh, "Smile", 1.5f);
    animation.setMorphWeight(mesh, "Blink", -0.5f);
    advance(animation, 5);
    std::span<const f32> drawn = animation.drawnMorphWeights(mesh);
    REQUIRE(drawn.size() == 3);
    CHECK(close(drawn[0], 1.5f));
    CHECK(close(drawn[1], -0.5f));
    CHECK(close(drawn[2], 0.25f));
    CHECK(close(animation.morphWeight(mesh, "Smile"), 1.5f));

    // Set again, it is replaced and not added.
    animation.setMorphWeight(mesh, "Smile", 0.75f);
    CHECK(close(animation.drawnMorphWeights(mesh)[0], 0.75f));

    // Cleared, the clip has it back the same frame; the other stays set.
    animation.clearMorphWeight(mesh, "Smile");
    drawn = animation.drawnMorphWeights(mesh);
    CHECK(close(drawn[0], 0.4f));
    CHECK(close(drawn[1], -0.5f));

    // With nothing playing, a script's weight alone is an answer; a weight of
    // nought set by a script is still the script's, over the file's quarter.
    animation.stop(track, 0.0f);
    advance(animation, 1);
    animation.setMorphWeight(mesh, "Jaw", 0.0f);
    drawn = animation.drawnMorphWeights(mesh);
    REQUIRE(drawn.size() == 3);
    CHECK(close(drawn[2], 0.0f));
    animation.clearMorphWeight(mesh, "Jaw");
    animation.clearMorphWeight(mesh, "Blink");
    CHECK(animation.drawnMorphWeights(mesh).empty());
}

TEST_CASE("a weight set before its mesh has loaded is kept, and one set on a mesh that is gone is not")
{
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/late.glb");
    const core::InstanceId mesh = fixture.meshOf(content);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    // The file is not here: no targets to name, and the weight is kept.
    CHECK(animation.morphTargetCount(mesh) == 0);
    animation.setMorphWeight(mesh, "Blink", 1.0f);
    CHECK(close(animation.morphWeight(mesh, "Blink"), 1.0f));
    CHECK(animation.drawnMorphWeights(mesh).empty());

    // It arrives, and the weight is drawn; a name it does not have is not.
    animation.setMorphWeight(mesh, "Eyebrow", 1.0f);
    fixture.skeletons.set(content, face());
    const std::span<const f32> drawn = animation.drawnMorphWeights(mesh);
    REQUIRE(drawn.size() == 3);
    CHECK(close(drawn[1], 1.0f));
    CHECK(close(drawn[0], 0.0f));

    // Destroyed, what was set for it goes with the next sweep: a new instance
    // in its slot does not inherit a blink.
    (void)fixture.world.destroy(mesh);
    animation.retire(fixture.world);
    const core::InstanceId next = fixture.meshOf(content);
    CHECK(animation.drawnMorphWeights(next).empty());
    CHECK(close(animation.morphWeight(next, "Blink"), 0.0f));
}

TEST_CASE("a clip from another file moves a mesh's targets by their names")
{
    // A head and a clip library exported apart: the library numbers `Jaw`
    // first where the head numbers it third. By name, as a joint is.
    Fixture fixture;
    const core::NameAtom head = fixture.atoms.intern("asset://models/head.glb");
    const core::NameAtom library = fixture.atoms.intern("asset://models/talk.glb");
    fixture.skeletons.set(head, face());
    render::SkeletonLibrary::Entry clips;
    clips.morphNames = {"Jaw", "Tongue"};
    clips.morphDefaults = {0.0f, 0.0f};
    asset::AnimationClip talk = weightClip("Talk", 0, 0.8f, 0.8f);
    // And a target the head lacks, which moves nothing.
    talk.weights.push_back(weightClip("", 1, 1.0f, 1.0f).weights[0]);
    clips.clips.push_back(talk);
    fixture.skeletons.set(library, std::move(clips));

    const core::InstanceId mesh = fixture.meshOf(head);
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(fixture.playerUnder(mesh), library, "Talk");
    animation.setLooped(track, true);
    animation.play(track, 0.0f, 1.0f, 1.0f);
    advance(animation, 3);

    const std::span<const f32> drawn = animation.drawnMorphWeights(mesh);
    REQUIRE(drawn.size() == 3);
    CHECK(close(drawn[2], 0.8f));
    CHECK(close(drawn[0], 0.0f));
    CHECK(close(drawn[1], 0.0f));
}

TEST_CASE("one player over a model plays a face's weights with the body's clip")
{
    // The character is a `Model`: a body with a rig and the clip, and a face
    // of the same file's targets under it. The player is the model's, and the
    // clip's weight channels reach the face as its joints reach the shirt.
    Fixture fixture;
    const core::NameAtom content = fixture.atoms.intern("asset://models/hero.glb");
    render::SkeletonLibrary::Entry entry = face();
    asset::Joint root;
    root.name = "root";
    root.parent = asset::Joint::NoParent;
    entry.joints.push_back(root);
    entry.clips.push_back(weightClip("Blinking", 1, 1.0f, 1.0f));
    fixture.skeletons.set(content, std::move(entry));

    const core::InstanceId model = fixture.world.create(fixture.instanceClass);
    const core::InstanceId body = fixture.meshOf(content, model);
    const core::InstanceId other = fixture.meshOf(content);
    const core::InstanceId player = fixture.playerUnder(model);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Blinking");
    animation.setLooped(track, true);
    animation.play(track, 0.0f, 1.0f, 1.0f);
    advance(animation, 3);

    REQUIRE(animation.drawnMorphWeights(body).size() == 3);
    CHECK(close(animation.drawnMorphWeights(body)[1], 1.0f));
    // A mesh of the same file that the player is not over is not blinking.
    CHECK(animation.drawnMorphWeights(other).empty());
}
