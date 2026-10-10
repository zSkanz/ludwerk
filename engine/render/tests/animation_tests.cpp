// Skeletal animation: sampling, blending and the pose walk (M6).
//
// Every case here is a fact about what a clip is supposed to do, written before
// the number was read off a run. The two that matter most are the ones about
// what animation must NOT do: a joint no channel drives keeps its rest pose, and
// two tracks blend in load order rather than in whatever order a container hands
// them over (R10).
#include <array>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>

#include "engine/render/animation.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using engine::core::f32;
using engine::core::Mat4;

namespace {

constexpr f32 kEpsilon = 1e-4f;

bool close(f32 a, f32 b) noexcept
{
    return std::fabs(a - b) <= kEpsilon;
}

// A two-joint skeleton: a root at the origin and a child one unit up. Small
// enough to check every number in the palette by hand, which is the point.
render::SkeletonLibrary::Entry twoJointSkeleton()
{
    render::SkeletonLibrary::Entry entry;

    asset::Joint root;
    root.name = "root";
    root.parent = asset::Joint::NoParent;
    entry.joints.push_back(root);

    asset::Joint child;
    child.name = "child";
    child.parent = 0;
    child.localBind.position = core::DVec3{0.0, 1.0, 0.0};
    // Model space to joint space at bind: the child sits one unit up, so its
    // inverse bind takes a model-space point one unit DOWN.
    child.inverseBind.m[3][1] = -1.0f;
    entry.joints.push_back(child);

    return entry;
}

// One second, one channel: the child slides from y = 1 to y = 3.
asset::AnimationClip slideClip(const char* name)
{
    asset::AnimationChannel channel;
    channel.joint = 1;
    channel.target = asset::AnimationChannel::Target::Translation;
    channel.stride = 3;
    channel.times = {0.0f, 1.0f};
    channel.values = {0.0f, 1.0f, 0.0f, 0.0f, 3.0f, 0.0f};

    asset::AnimationClip clip;
    clip.name = name;
    clip.duration = 1.0f;
    clip.channels.push_back(channel);
    return clip;
}

// A world with a `MeshPart` naming one content URN and an `AnimationPlayer`
// under it. Hand-built for the same reason `render_world_tests.cpp` hand-builds
// its own: an animation test must not fail because an API definition moved.
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

    core::NameAtom content = atoms.intern("asset://models/rig.glb");

    // The mesh is the player's parent, which is where `createTrack` looks for
    // the skeleton and where the pose is keyed.
    core::InstanceId mesh;

    [[nodiscard]] core::InstanceId rig(render::SkeletonLibrary::Entry entry)
    {
        skeletons.set(content, std::move(entry));
        mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = content;
        const core::InstanceId player = world.create(instanceClass);
        (void)world.setParent(player, mesh);
        return player;
    }
};

} // namespace

TEST_CASE("D611: a rig read again is found again by name, and what was playing goes on from where it was")
{
    // A model exported again while the game runs: its clips may be in
    // another order and another length. A track holds its clip by INDEX, and
    // held it for ever -- so after the reload a walk played whatever clip
    // had taken the walk's place in the file.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.setLooped(track, true);
    animation.play(track, 0.0f, 1.0f, 1.0f);
    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);
    REQUIRE(animation.pose(fixture.mesh) != nullptr);
    // Half a second in: halfway from one to three.
    CHECK(close(animation.pose(fixture.mesh)->local[1].m[3][1], 2.0f));

    // The same rig again, with another clip in front of the slide and the
    // slide twice as long and reaching five.
    render::SkeletonLibrary::Entry again = twoJointSkeleton();
    asset::AnimationClip other = slideClip("Other");
    other.channels[0].values = {0.0f, 40.0f, 0.0f, 0.0f, 40.0f, 0.0f};
    again.clips.push_back(other);
    asset::AnimationClip longer = slideClip("Slide");
    longer.duration = 2.0f;
    longer.channels[0].times = {0.0f, 2.0f};
    longer.channels[0].values = {0.0f, 1.0f, 0.0f, 0.0f, 5.0f, 0.0f};
    again.clips.push_back(longer);
    fixture.skeletons.set(fixture.content, std::move(again));

    animation.sample(1.0 / 60.0);
    REQUIRE(animation.pose(fixture.mesh) != nullptr);
    // Still the slide, by its name -- not the clip that took its place --
    // from the time it was at, in the clip as it is now.
    CHECK(close(animation.state(track).length, 2.0f));
    const f32 at = animation.pose(fixture.mesh)->local[1].m[3][1];
    CHECK(at > 1.9f);
    CHECK(at < 2.2f);

    // A rig that comes back without the clip: the track plays nothing, and
    // still answers.
    fixture.skeletons.set(fixture.content, twoJointSkeleton());
    animation.sample(1.0 / 60.0);
    CHECK(animation.pose(fixture.mesh) == nullptr);
    CHECK(animation.state(track).playing);
}

TEST_CASE("a track that names a clip the file does not have still answers reads")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Bend"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId missing = animation.createTrack(player, {}, "NoSuchClip");

    // Not zero and not a crash: a mesh that has not finished loading would
    // otherwise make a perfectly ordinary frame a nil index.
    REQUIRE(missing != 0);
    CHECK(close(animation.state(missing).length, 0.0f));
    animation.play(missing, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);
    // No clip means no pose at all, rather than a bind-pose palette built every
    // tick for a track that drives nothing.
    CHECK(animation.pose(fixture.mesh) == nullptr);
}

TEST_CASE("D509: a track loaded before its mesh's file arrived binds when it does, and plays as it was told")
{
    // A figure made the moment a body appears asked for its run before the
    // mesh had loaded: the track found no clip, kept a length of zero, and
    // never played -- the others were seen sliding about.
    Fixture fixture;
    const core::InstanceId mesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(mesh)->meshContent = fixture.content;
    const core::InstanceId player = fixture.world.create(fixture.instanceClass);
    (void)fixture.world.setParent(player, mesh);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId run = animation.createTrack(player, {}, "Run");
    REQUIRE(run != 0);
    CHECK(close(animation.state(run).length, 0.0f));
    // Told to play, at half speed, before there is anything to play.
    animation.play(run, 0.0f, 1.0f, 0.5f);
    animation.sample(1.0 / 60.0);
    CHECK(animation.pose(mesh) == nullptr);

    // The file arrives.
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Walk"));
    entry.clips.push_back(slideClip("Run"));
    fixture.skeletons.set(fixture.content, std::move(entry));

    animation.sample(1.0 / 60.0);
    CHECK(close(animation.state(run).length, 1.0f));
    CHECK(animation.state(run).playing);
    REQUIRE(animation.pose(mesh) != nullptr);
    // From its beginning, at the speed it was given: half a tick's worth in.
    CHECK(animation.state(run).timePosition == doctest::Approx(0.5 / 60.0));
}

TEST_CASE("an empty clip name takes the file's first clip")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Walk"));
    entry.clips.push_back(slideClip("Run"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    CHECK(close(animation.state(animation.createTrack(player, {}, "")).length, 1.0f));
}

TEST_CASE("sampling walks the clip and the palette is joint times inverse bind")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);

    // Half a second in, halfway between the two keys: y = 2.
    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 0.5f));

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->palette.size() == 2);
    // The child is at y = 2 and its inverse bind subtracts the 1 it was bound
    // at, so a vertex skinned to it moves up by exactly one unit.
    CHECK(close(pose->palette[1].m[3][1], 1.0f));
    // The root is untouched by the clip, and its palette entry is the identity.
    CHECK(close(pose->palette[0].m[3][1], 0.0f));
    CHECK(close(pose->palette[0].m[0][0], 1.0f));
}

TEST_CASE("a joint no channel drives keeps its rest transform")
{
    // The failure this guards against does not look like a bug in the joint that
    // is animated: it looks like every OTHER joint collapsing to the origin,
    // which is what a single weighted average over all joints would do.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.joints[0].localBind.position = core::DVec3{5.0, 0.0, 0.0};
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.play(animation.createTrack(player, {}, "Slide"), 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    CHECK(close(pose->palette[0].m[3][0], 5.0f));
}

TEST_CASE("a non-looping clip stops at its end and reports it once")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);

    for (int tick = 0; tick < 59; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(animation.drainEnded().empty());

    for (int tick = 0; tick < 5; ++tick)
        animation.sample(1.0 / 60.0);
    const std::span<const scene::TrackId> ended = animation.drainEnded();
    REQUIRE(ended.size() == 1);
    CHECK(ended[0] == track);
    CHECK_FALSE(animation.state(track).playing);
    // Clamped at the end rather than run past it, so a `TimePosition` read after
    // the fact is the clip's length and not whatever the tick overshot to.
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 1.0f));
    // Drained: the second frame does not fire `Ended` again.
    CHECK(animation.drainEnded().empty());

    // And the pose HOLDS the last frame rather than snapping to bind. `Ended` is
    // a deferred signal, so a handler that starts the next animation runs a tick
    // later -- and a character that returns to its rest pose for that one tick
    // is a visible pop.
    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    CHECK(close(pose->palette[1].m[3][1], 2.0f));
    animation.stop(track, 0.0f);
    animation.sample(1.0 / 60.0);
    // Stopped, so the pose goes away rather than freezing the character
    // mid-stride: null means bind pose, which is what an unanimated skinned mesh
    // should look like.
    CHECK(animation.pose(fixture.mesh) == nullptr);
}

TEST_CASE("D516: a mesh whose tracks are stopped is posed once, not every tick -- and moves again when one plays")
{
    // Three hundred bodies parked in a pool, tracks loaded and not playing,
    // were three hundred poses rebuilt to the same answer every tick.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.sample(1.0 / 60.0);
    const core::u64 settled = animation.posesBuilt();
    for (int tick = 0; tick < 10; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(animation.posesBuilt() == settled);

    // Played, it is posed every tick it moves.
    animation.play(track, 0.0f, 1.0f, 1.0f);
    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(animation.posesBuilt() == settled + 30);
    const render::Pose* moving = animation.pose(fixture.mesh);
    REQUIRE(moving != nullptr);
    CHECK(close(moving->palette[1].m[3][1], 1.0f));

    // Stopped, it goes back to rest on the next tick -- and then is left alone.
    animation.stop(track, 0.0f);
    animation.sample(1.0 / 60.0);
    CHECK(animation.pose(fixture.mesh) == nullptr);
    const core::u64 rested = animation.posesBuilt();
    for (int tick = 0; tick < 10; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(animation.posesBuilt() == rested);
}

TEST_CASE("H3: a rig nobody sees is not posed, a small one is posed every few ticks, and its clock never stops")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);

    SUBCASE("with nobody reporting -- a server, a replay -- every tick")
    {
        const core::u64 before = animation.posesBuilt();
        for (int tick = 0; tick < 8; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(animation.posesBuilt() == before + 8);
    }
    SUBCASE("reported, but not reached: not posed, and caught up when a joint is asked for")
    {
        animation.reportSeen({}, true);
        const core::u64 before = animation.posesBuilt();
        for (int tick = 0; tick < 29; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(animation.posesBuilt() == before);
        // Half way, as an every-tick pose would say -- built now.
        core::CFrameD child;
        REQUIRE(animation.jointModel(fixture.mesh, 1, child));
        CHECK(child.position.y == doctest::Approx(2.0).epsilon(0.02));
        CHECK(animation.posesBuilt() == before + 1);
    }
    SUBCASE("seen small: every eighth tick; seen large: every tick")
    {
        const std::array<render::SeenSkin, 1> small{render::SeenSkin{fixture.mesh, 0.01f}};
        animation.reportSeen(small, true);
        // Posed -- built, or copied from a pose with the same inputs (H10).
        const auto posed = [&animation] { return animation.posesBuilt() + animation.posesShared(); };
        const core::u64 before = posed();
        for (int tick = 0; tick < 32; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(posed() == before + 4);

        const std::array<render::SeenSkin, 1> large{render::SeenSkin{fixture.mesh, 0.5f}};
        animation.reportSeen(large, true);
        const core::u64 after = animation.posesBuilt();
        for (int tick = 0; tick < 8; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(animation.posesBuilt() == after + 8);
    }
    SUBCASE("AnimationDetail scales the size a rig is taken to be: a far camera's crowd, posed every tick")
    {
        // Four per cent of the picture's height: every fourth tick.
        const std::array<render::SeenSkin, 1> far{render::SeenSkin{fixture.mesh, 0.04f}};
        animation.reportSeen(far, true);
        const auto posed = [&animation] { return animation.posesBuilt() + animation.posesShared(); };
        core::u64 before = posed();
        for (int tick = 0; tick < 32; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(posed() == before + 8);

        // Taken for four times that, sixteen per cent: every tick.
        animation.setDetail(4.0f);
        before = posed();
        for (int tick = 0; tick < 8; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(posed() == before + 8);

        // And for half of it, two per cent: every eighth at most -- a pose at
        // a moment of the clip it already has is not made again.
        animation.setDetail(0.5f);
        before = posed();
        for (int tick = 0; tick < 32; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(posed() > before);
        CHECK(posed() <= before + 4);
    }
    SUBCASE("AlwaysAnimate: every tick, seen or not")
    {
        scene::AnimationPlayerComponent always;
        always.cullingMode = 1;
        fixture.world.animationPlayers().add(player, always);
        animation.reportSeen({}, true);
        const core::u64 before = animation.posesBuilt();
        for (int tick = 0; tick < 8; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(animation.posesBuilt() == before + 8);
    }
}

TEST_CASE("H10: two meshes of one rig at one moment of one clip share a pose, bit for bit; another moment does not")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId first = fixture.rig(entry);
    const core::InstanceId firstMesh = fixture.mesh;
    const core::InstanceId second = fixture.rig(entry);
    const core::InstanceId secondMesh = fixture.mesh;
    const core::InstanceId third = fixture.rig(std::move(entry));
    const core::InstanceId thirdMesh = fixture.mesh;

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId a = animation.createTrack(first, {}, "Slide");
    const scene::TrackId b = animation.createTrack(second, {}, "Slide");
    const scene::TrackId c = animation.createTrack(third, {}, "Slide");
    animation.play(a, 0.0f, 1.0f, 1.0f);
    animation.play(b, 0.0f, 1.0f, 1.0f);
    // The third a little faster: another moment of the clip.
    animation.play(c, 0.0f, 1.0f, 1.5f);
    for (int tick = 0; tick < 12; ++tick)
        animation.sample(1.0 / 60.0);

    // Three posed a tick: the second always a copy of the first, and the
    // third a copy too whenever it reaches a moment of the clip the others
    // have already been at -- the cache keeps poses across ticks.
    CHECK(animation.posesBuilt() + animation.posesShared() == 36);
    CHECK(animation.posesShared() >= 12);
    const render::Pose* one = animation.pose(firstMesh);
    const render::Pose* two = animation.pose(secondMesh);
    const render::Pose* three = animation.pose(thirdMesh);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    REQUIRE(three != nullptr);
    CHECK(std::memcmp(one->palette.data(), two->palette.data(), one->palette.size() * sizeof(core::Mat4)) == 0);
    CHECK_FALSE(close(one->palette[1].m[3][1], three->palette[1].m[3][1]));
    // **And it is the one pose, held by both**: each had a copy of it, three
    // arrays of matrices a body a tick, which was most of what a pose cost a
    // crowd.
    CHECK(one == two);
    CHECK(one != three);

    SUBCASE("posed at a reduced rate, the third samples its clip's keys and shares with whoever is at the same key")
    {
        // Small on the picture: every eighth tick, and at the clip's own keys
        // (one second apart in this clip), so all three sit on key zero.
        const std::array<render::SeenSkin, 3> small{render::SeenSkin{firstMesh, 0.01f},
                                                    render::SeenSkin{secondMesh, 0.01f},
                                                    render::SeenSkin{thirdMesh, 0.01f}};
        animation.reportSeen(small, true);
        const core::u64 shared = animation.posesShared();
        for (int tick = 0; tick < 64; ++tick)
            animation.sample(1.0 / 60.0);
        CHECK(animation.posesShared() > shared);
    }
}

TEST_CASE("a body that comes to a moment of a clip another was at seconds ago takes the pose that one built")
{
    // A horde is not in step: its bodies began their clips at different
    // ticks, and each is where another was a while ago. A pose was kept for
    // thirty-two ticks -- so of five hundred bodies on a handful of clips,
    // two in three were built from their keys every tick, each a pose some
    // other body had built within the last two seconds.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId first = fixture.rig(entry);
    const core::InstanceId second = fixture.rig(std::move(entry));
    const core::InstanceId secondMesh = fixture.mesh;

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId a = animation.createTrack(first, {}, "Slide");
    const scene::TrackId b = animation.createTrack(second, {}, "Slide");
    animation.setLooped(a, true);
    animation.setLooped(b, true);
    animation.play(a, 0.0f, 1.0f, 1.0f);
    // A hundred ticks on its own, through a loop and most of another.
    for (int tick = 0; tick < 100; ++tick)
        animation.sample(1.0 / 60.0);
    animation.play(b, 0.0f, 1.0f, 1.0f);
    const core::u64 built = animation.posesBuilt();
    const core::u64 shared = animation.posesShared();
    for (int tick = 0; tick < 50; ++tick)
        animation.sample(1.0 / 60.0);
    // The second is fifty ticks into its clip, where the first was a hundred
    // ticks ago: every one of its poses was the first's, kept. What was built
    // in those fifty ticks is the first's own at most.
    CHECK(animation.posesBuilt() - built <= 50);
    CHECK(animation.posesShared() - shared >= 50);
    REQUIRE(animation.pose(secondMesh) != nullptr);
}

TEST_CASE("a pose a crowd shares is written to by none of them: a ragdoll and a bone are one body's own")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId first = fixture.rig(entry);
    const core::InstanceId firstMesh = fixture.mesh;
    const core::InstanceId second = fixture.rig(entry);
    const core::InstanceId secondMesh = fixture.mesh;
    const core::InstanceId third = fixture.rig(std::move(entry));
    const core::InstanceId thirdMesh = fixture.mesh;

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    for (const core::InstanceId player : {first, second, third})
        animation.play(animation.createTrack(player, {}, "Slide"), 0.0f, 1.0f, 1.0f);
    for (int tick = 0; tick < 6; ++tick)
        animation.sample(1.0 / 60.0);
    REQUIRE(animation.pose(firstMesh) != nullptr);
    REQUIRE(animation.pose(firstMesh) == animation.pose(secondMesh));
    REQUIRE(animation.pose(firstMesh) == animation.pose(thirdMesh));
    const std::vector<core::Mat4> shared = animation.pose(secondMesh)->palette;

    // The first goes limp at a joint: its pose is its own from here, and the
    // other two hold what they held, to the bit.
    scene::SkeletonHost& host = animation;
    core::CFrameD moved;
    moved.position = core::DVec3{5.0, 1.0, 0.0};
    host.setJointOverride(firstMesh, 1, moved);
    host.commitOverrides();
    CHECK(animation.pose(firstMesh) != animation.pose(secondMesh));
    CHECK(animation.pose(secondMesh) == animation.pose(thirdMesh));
    CHECK(std::memcmp(animation.pose(secondMesh)->palette.data(), shared.data(), shared.size() * sizeof(core::Mat4)) ==
          0);
    core::CFrameD at;
    REQUIRE(host.jointModel(firstMesh, 1, at));
    CHECK(at.position.x == doctest::Approx(5.0));
    REQUIRE(host.jointModel(secondMesh, 1, at));
    CHECK_FALSE(at.position.x == doctest::Approx(5.0));

    // And a `Bone` on the third turns the third alone: a rig with a bone is
    // posed for itself, and one without is not asked whether it has any.
    const core::InstanceId bone = fixture.world.create(fixture.instanceClass);
    scene::AttachmentComponent attachment;
    attachment.jointIndex = 1;
    attachment.transform.position = core::DVec3{0.0, 3.0, 0.0};
    fixture.world.attachments().add(bone, attachment);
    (void)fixture.world.setParent(bone, thirdMesh);
    animation.sample(1.0 / 60.0);
    animation.sample(1.0 / 60.0);
    CHECK(animation.pose(thirdMesh) != animation.pose(secondMesh));
    core::CFrameD turned;
    core::CFrameD plain;
    REQUIRE(host.jointModel(thirdMesh, 1, turned));
    REQUIRE(host.jointModel(secondMesh, 1, plain));
    CHECK(turned.position.y == doctest::Approx(plain.position.y + 3.0).epsilon(0.01));
}

TEST_CASE("H3: the intervals shrink with the rig on the picture")
{
    CHECK(render::AnimationSystem::updateInterval(0.5f) == 1);
    CHECK(render::AnimationSystem::updateInterval(0.08f) == 2);
    CHECK(render::AnimationSystem::updateInterval(0.04f) == 4);
    CHECK(render::AnimationSystem::updateInterval(0.01f) == 8);
}

TEST_CASE("a looping clip wraps rather than resetting, and never ends")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.setLooped(track, true);
    animation.play(track, 0.0f, 1.0f, 1.0f);

    // 1.05 seconds at 1/60 does not land on the loop point, which is the whole
    // point: a reset to zero would lose the 0.05 and the loop would drift
    // against everything else in the scene.
    for (int tick = 0; tick < 63; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 0.05f));
    CHECK(animation.state(track).playing);
    CHECK(animation.drainEnded().empty());
}

TEST_CASE("Play restarts from the beginning, unlike a tween")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);

    animation.play(track, 0.0f, 1.0f, 1.0f);
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 0.0f));
}

TEST_CASE("two tracks at half weight land halfway between their clips")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Up"));

    // A second clip that drives the same joint the other way.
    asset::AnimationClip down = slideClip("Down");
    down.channels[0].values = {0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f};
    entry.clips.push_back(down);
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId up = animation.createTrack(player, {}, "Up");
    const scene::TrackId downTrack = animation.createTrack(player, {}, "Down");
    // Three to one rather than one to one, so the answer is a number the bind
    // pose could not also produce -- an even blend of +3 and -1 IS the bind
    // pose, and a case that cannot tell blending from doing nothing is not a
    // case.
    animation.play(up, 0.0f, 0.75f, 1.0f);
    animation.play(downTrack, 0.0f, 0.25f, 1.0f);

    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);

    // Halfway: up is at 2, down is at 0, so the joint sits at 1.5 and the
    // inverse bind takes off the 1 it was bound at.
    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    CHECK(close(pose->palette[1].m[3][1], 0.5f));
}

TEST_CASE("a weight of zero contributes nothing rather than dragging a joint to the origin")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Up"));
    entry.clips.push_back(slideClip("Also"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId loud = animation.createTrack(player, {}, "Up");
    const scene::TrackId silent = animation.createTrack(player, {}, "Also");
    animation.play(loud, 0.0f, 1.0f, 1.0f);
    animation.play(silent, 0.0f, 0.0f, 1.0f);

    for (int tick = 0; tick < 60; ++tick)
        animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    CHECK(close(pose->palette[1].m[3][1], 2.0f));
}

TEST_CASE("D438: a track played, stopped and played again plays -- at the weight it had")
{
    // "The idle only runs once, the walk only runs once." `Stop` left the
    // track's own weight at zero and `Play` faded in to it: the second time a
    // clip was played nothing showed, and with a fade it stopped itself again.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.setLooped(track, true);
    const auto ticks = [&](int count) {
        for (int tick = 0; tick < count; ++tick)
            animation.sample(1.0 / 60.0);
    };
    // As the script's `Play` does it: to the track's own weight.
    const auto play = [&](core::f32 fade) {
        const scene::TrackState state = animation.state(track);
        animation.play(track, fade, state.weight, state.speed);
    };

    play(0.15f);
    ticks(30);
    REQUIRE(animation.state(track).playing);
    REQUIRE(close(animation.state(track).blend, 1.0f));

    animation.stop(track, 0.15f);
    ticks(30);
    REQUIRE_FALSE(animation.state(track).playing);
    // Stopped, and still the weight it was given.
    CHECK(close(animation.state(track).weight, 1.0f));

    // **Again.** It fades in, and is still playing three loops later.
    play(0.15f);
    ticks(6);
    CHECK(animation.state(track).playing);
    CHECK(animation.state(track).blend > 0.0f);
    const int loop = static_cast<int>(animation.state(track).length * 60.0f) + 1;
    ticks(loop * 3);
    CHECK(animation.state(track).playing);
    CHECK(close(animation.state(track).blend, 1.0f));
    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);

    // With no fade either way.
    animation.stop(track, 0.0f);
    play(0.0f);
    CHECK(animation.state(track).playing);
    CHECK(close(animation.state(track).blend, 1.0f));

    // **A `Play` that lands while the stop is still fading out**: the
    // fade-out is over, and the track carries on from where the fade was.
    animation.stop(track, 0.5f);
    ticks(15);
    const core::f32 during = animation.state(track).blend;
    CHECK(during > 0.2f);
    CHECK(during < 0.8f);
    play(0.5f);
    CHECK(close(animation.state(track).blend, during));
    ticks(60);
    CHECK(animation.state(track).playing);
    CHECK(close(animation.state(track).blend, 1.0f));

    // A weight written while it is stopped is the weight it plays at next,
    // and does not start it.
    animation.stop(track, 0.0f);
    animation.adjustWeight(track, 0.5f, 0.0f);
    CHECK_FALSE(animation.state(track).playing);
    CHECK(close(animation.state(track).blend, 0.0f));
    play(0.0f);
    CHECK(close(animation.state(track).blend, 0.5f));
    CHECK(close(animation.state(track).weight, 0.5f));
}

TEST_CASE("a fade reaches its target and a fade to zero is a stop")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.5f, 1.0f, 1.0f);
    CHECK(close(animation.state(track).blend, 0.0f));
    // What the script set is not the fade's to change.
    CHECK(close(animation.state(track).weight, 1.0f));

    for (int tick = 0; tick < 15; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(animation.state(track).blend, 0.5f));

    for (int tick = 0; tick < 15; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(animation.state(track).blend, 1.0f));

    animation.stop(track, 0.25f);
    CHECK(animation.state(track).playing);
    for (int tick = 0; tick < 15; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(animation.state(track).blend, 0.0f));
    CHECK(close(animation.state(track).weight, 1.0f));
    CHECK_FALSE(animation.state(track).playing);
    // A stop is not an end: `Ended` fires for a clip that finished, and this one
    // was cut short.
    CHECK(animation.drainEnded().empty());
}

TEST_CASE("Speed scales the clock and a speed of zero holds the pose")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 2.0f);
    for (int tick = 0; tick < 15; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 0.5f));

    animation.adjustSpeed(track, 0.0f);
    for (int tick = 0; tick < 60; ++tick)
        animation.sample(1.0 / 60.0);
    CHECK(close(static_cast<f32>(animation.state(track).timePosition), 0.5f));
}

TEST_CASE("retire forgets the tracks of an instance that is gone, and keeps answering reads")
{
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Slide"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);
    REQUIRE(animation.pose(fixture.mesh) != nullptr);

    REQUIRE(fixture.world.destroy(player));
    // `destroy` is synchronous but the generation bump is not: `alive` stays
    // true until the drain retires it, which is what gives a `Destroying`
    // handler a handle to work with.
    fixture.world.retireDestroyed();
    animation.retire(fixture.world);

    CHECK_FALSE(animation.state(track).playing);
    CHECK(animation.pose(fixture.mesh) == nullptr);
    // The handle a script still holds resolves to a stopped track rather than to
    // whatever took the slot.
    CHECK(close(animation.state(track).length, 1.0f));
    animation.sample(1.0 / 60.0);
}

TEST_CASE("D515: a step clip holds each key, and a cubic spline follows its tangents")
{
    // The slide's child, y from 1 to 3 over a second, read at a quarter of
    // the way: a line is at 1.5, a step still at 1, and a spline that leaves
    // its first key at four units a second is at 1.875.
    const auto childYAt = [](asset::AnimationChannel channel) {
        Fixture fixture;
        render::SkeletonLibrary::Entry entry = twoJointSkeleton();
        asset::AnimationClip clip;
        clip.name = "Slide";
        clip.duration = 1.0f;
        clip.channels.push_back(std::move(channel));
        entry.clips.push_back(clip);
        const core::InstanceId player = fixture.rig(std::move(entry));
        render::AnimationSystem animation{fixture.world, fixture.skeletons};
        animation.play(animation.createTrack(player, {}, "Slide"), 0.0f, 1.0f, 1.0f);
        for (int tick = 0; tick < 15; ++tick)
            animation.sample(1.0 / 60.0);
        const render::Pose* pose = animation.pose(fixture.mesh);
        REQUIRE(pose != nullptr);
        // The palette undoes the bind's one unit up.
        return pose->palette[1].m[3][1] + 1.0f;
    };

    asset::AnimationChannel line = slideClip("Slide").channels.front();
    CHECK(close(childYAt(line), 1.5f));

    asset::AnimationChannel step = line;
    step.interpolation = asset::AnimationChannel::Interpolation::Step;
    CHECK(close(childYAt(step), 1.0f));

    asset::AnimationChannel spline = line;
    spline.interpolation = asset::AnimationChannel::Interpolation::CubicSpline;
    // In-tangent, value, out-tangent, a key at a time.
    spline.values = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 4.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(close(childYAt(spline), 1.875f));
}

TEST_CASE("rotation is interpolated the short way round")
{
    // The failure is one key long and looks like a joint spinning through 350
    // degrees instead of 10, which is why it is checked rather than eyeballed.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();

    asset::AnimationChannel channel;
    channel.joint = 1;
    channel.target = asset::AnimationChannel::Target::Rotation;
    channel.stride = 4;
    channel.times = {0.0f, 1.0f};
    // Identity, then the SAME rotation written with every sign flipped -- which
    // is the same orientation and must therefore not move at all.
    channel.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f};

    asset::AnimationClip clip;
    clip.name = "Flip";
    clip.duration = 1.0f;
    clip.channels.push_back(channel);
    entry.clips.push_back(clip);
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    animation.play(animation.createTrack(player, {}, "Flip"), 0.0f, 1.0f, 1.0f);
    for (int tick = 0; tick < 30; ++tick)
        animation.sample(1.0 / 60.0);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    // Halfway between q and -q is the identity if the sign is aligned, and a
    // zero quaternion if it is not.
    CHECK(close(pose->palette[1].m[0][0], 1.0f));
    CHECK(close(pose->palette[1].m[1][1], 1.0f));
    CHECK(close(pose->palette[1].m[2][2], 1.0f));
}

TEST_CASE("the skeleton library answers by content and is sorted, not hashed")
{
    core::AtomTable atoms;
    render::SkeletonLibrary library;

    // Interned out of alphabetical order on purpose: `find` must not depend on
    // the order they arrived in.
    const core::NameAtom second = atoms.intern("asset://models/z.glb");
    const core::NameAtom first = atoms.intern("asset://models/a.glb");

    render::SkeletonLibrary::Entry entry;
    entry.joints.emplace_back();
    library.set(second, entry);
    entry.joints.emplace_back();
    library.set(first, entry);

    REQUIRE(library.find(second) != nullptr);
    REQUIRE(library.find(first) != nullptr);
    CHECK(library.find(second)->joints.size() == 1);
    CHECK(library.find(first)->joints.size() == 2);
    CHECK(library.find(atoms.intern("asset://models/absent.glb")) == nullptr);

    // A second `set` on the same content replaces rather than appending.
    library.set(second, render::SkeletonLibrary::Entry{});
    CHECK(library.find(second)->joints.empty());

    library.clear();
    CHECK(library.find(first) == nullptr);
}

// --- scene::SkeletonHost -----------------------------------------------------

// A three-joint chain -- root, arm one unit up, hand one unit above that -- so
// that a joint can be overridden with a joint BELOW it that is not, which is the
// case the forward pass exists for.
render::SkeletonLibrary::Entry threeJointChain()
{
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    asset::Joint hand;
    hand.name = "hand";
    hand.parent = 1;
    hand.localBind.position = core::DVec3{0.0, 1.0, 0.0};
    hand.inverseBind.m[3][1] = -2.0f;
    entry.joints.push_back(hand);
    return entry;
}

TEST_CASE("a skeleton answers what it is made of")
{
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    CHECK(host.jointCount(fixture.mesh) == 3);
    CHECK(host.findJoint(fixture.mesh, "hand") == 2);
    CHECK(host.findJoint(fixture.mesh, "root") == 0);
    // A name nothing carries is -1 rather than 0, because 0 is a real joint --
    // and a `Bone` that silently resolved to the root would put a sword at a
    // character's feet.
    CHECK(host.findJoint(fixture.mesh, "tail") == -1);
    CHECK(host.jointName(fixture.mesh, 1) == "child");
    CHECK(host.jointParent(fixture.mesh, 2) == 1);
    CHECK(host.jointParent(fixture.mesh, 0) == -1);

    // A mesh with no rig at all, which is most of them.
    const core::InstanceId plain = fixture.world.create(fixture.meshPartClass);
    CHECK(host.jointCount(plain) == 0);
    CHECK(host.findJoint(plain, "root") == -1);
}

TEST_CASE("a joint answers where it is even with nothing playing")
{
    // **The whole reason `jointModel` is a call and not a peek at the palette.**
    // A character standing still has no pose at all, and a socket welded to its
    // hand still has to be somewhere -- at the hand, in the rest chain.
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    core::CFrameD at;
    REQUIRE(host.jointModel(fixture.mesh, 2, at));
    // Two units up: one for the arm and one for the hand above it.
    CHECK(at.position.y == doctest::Approx(2.0));
    CHECK(at.position.x == doctest::Approx(0.0));

    REQUIRE(host.jointModel(fixture.mesh, 0, at));
    CHECK(at.position.y == doctest::Approx(0.0));

    CHECK_FALSE(host.jointModel(fixture.mesh, 7, at));
}

TEST_CASE("a joint answers where the clip put it once one is playing")
{
    Fixture fixture;
    const core::InstanceId player = fixture.rig(threeJointChain());
    render::SkeletonLibrary::Entry posed = threeJointChain();
    posed.clips.push_back(slideClip("slide"));
    fixture.skeletons.set(fixture.content, posed);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    // Half a second into a clip that slides the child from y = 1 to y = 3.
    animation.sample(0.5);

    core::CFrameD at;
    REQUIRE(animation.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.y == doctest::Approx(2.0).epsilon(0.01));
    // And the hand rode along, one unit above wherever the arm ended up.
    REQUIRE(animation.jointModel(fixture.mesh, 2, at));
    CHECK(at.position.y == doctest::Approx(3.0).epsilon(0.01));
}

TEST_CASE("G9: a bone's Transform turns its joint, and the joints below it follow")
{
    // `Bone.Transform` was documented as the writable half of procedural
    // animation and moved nothing: a chest turned eighty degrees left the
    // figure, and the hand bone, as they were -- with a clip and without.
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);

    // A `Bone` on the middle joint, a quarter turn about Z in its own space.
    const core::InstanceId bone = fixture.world.create(fixture.instanceClass);
    scene::AttachmentComponent attachment;
    attachment.jointIndex = 1;
    // Columns are the axes: right becomes up, up becomes left.
    attachment.transform.rotation.m[0][0] = 0.0f;
    attachment.transform.rotation.m[0][1] = 1.0f;
    attachment.transform.rotation.m[1][0] = -1.0f;
    attachment.transform.rotation.m[1][1] = 0.0f;
    fixture.world.attachments().add(bone, attachment);
    (void)fixture.world.setParent(bone, fixture.mesh);

    animation.sample(1.0 / 60.0);
    core::CFrameD at;
    // The middle joint stays where it was, turned.
    REQUIRE(animation.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.y == doctest::Approx(1.0).epsilon(0.01));
    // The hand, a unit along the turned joint's up: to its side now, not above.
    REQUIRE(animation.jointModel(fixture.mesh, 2, at));
    CHECK(at.position.x == doctest::Approx(-1.0).epsilon(0.01));
    CHECK(at.position.y == doctest::Approx(1.0).epsilon(0.01));

    // The identity again is a bone nobody drives.
    fixture.world.attachments().find(bone)->transform = core::CFrameD{};
    animation.sample(1.0 / 60.0);
    REQUIRE(animation.jointModel(fixture.mesh, 2, at));
    CHECK(at.position.x == doctest::Approx(0.0).epsilon(0.01));
    CHECK(at.position.y == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE("an override moves the joints below it")
{
    // A ragdoll simulates a dozen bones and a hand has twenty. The ones it does
    // NOT simulate take their parent's new transform and their own unchanged
    // local -- which is what makes them ride along rather than stay where the
    // clip left them.
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    core::CFrameD moved;
    moved.position = core::DVec3{5.0, 1.0, 0.0};
    host.setJointOverride(fixture.mesh, 1, moved);
    host.commitOverrides();

    core::CFrameD at;
    REQUIRE(host.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.x == doctest::Approx(5.0));
    // The hand was never overridden and followed anyway.
    REQUIRE(host.jointModel(fixture.mesh, 2, at));
    CHECK(at.position.x == doctest::Approx(5.0));
    CHECK(at.position.y == doctest::Approx(2.0));
    // And the root above it did not move, because an override reaches down and
    // not up.
    REQUIRE(host.jointModel(fixture.mesh, 0, at));
    CHECK(at.position.x == doctest::Approx(0.0));
}

TEST_CASE("going limp keeps the pose the animation left, joint by joint")
{
    // **The repair that is invisible until a ragdoll takes over a character
    // that was moving.** A mesh whose tracks stop contributing used to have its
    // pose ERASED -- and `commitOverrides` then rebuilds from the REST chain,
    // so every joint the ragdoll does not simulate snaps out of the animation
    // it was in and into bind pose, on the exact frame the character goes limp.
    //
    // Here the clip has slid the child to y = 3 and the hand rides one above it.
    // The ragdoll then drives the CHILD and leaves the hand alone: the hand must
    // keep the local the clip gave it, not the rest one.
    Fixture fixture;
    const core::InstanceId player = fixture.rig(threeJointChain());
    render::SkeletonLibrary::Entry posed = threeJointChain();
    // The hand is bent out along X by the clip, which the rest chain does not do.
    asset::AnimationChannel bend;
    bend.joint = 2;
    bend.target = asset::AnimationChannel::Target::Translation;
    bend.stride = 3;
    bend.times = {0.0f, 1.0f};
    bend.values = {3.0f, 1.0f, 0.0f, 3.0f, 1.0f, 0.0f};
    asset::AnimationClip clip = slideClip("slide");
    clip.channels.push_back(bend);
    posed.clips.push_back(clip);
    fixture.skeletons.set(fixture.content, posed);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0);

    core::CFrameD handWhilePlaying;
    REQUIRE(host.jointModel(fixture.mesh, 2, handWhilePlaying));
    // Bent three units out along X by the clip.
    CHECK(handWhilePlaying.position.x == doctest::Approx(3.0).epsilon(0.01));

    // The character goes limp: the clip stops contributing, and the ragdoll
    // starts driving the joint above the hand in the same tick.
    animation.stop(track, 0.0f);
    core::CFrameD driven;
    driven.position = core::DVec3{0.0, 6.0, 0.0};
    host.setJointOverride(fixture.mesh, 1, driven);
    animation.sample(1.0 / 60.0);
    host.commitOverrides();

    core::CFrameD hand;
    REQUIRE(host.jointModel(fixture.mesh, 2, hand));
    // It followed the driven joint up...
    CHECK(hand.position.y == doctest::Approx(7.0).epsilon(0.01));
    // ...and it is STILL BENT, which is the whole point. Rebuilt from rest it
    // would be at x = 0, straight, and the character's hand would snap open.
    CHECK(hand.position.x == doctest::Approx(3.0).epsilon(0.01));
}

TEST_CASE("an override survives a tick in which no clip contributes")
{
    // **The repair that is invisible until a ragdoll goes limp.** A mesh with
    // nothing playing used to have its pose ERASED -- and a mesh with no track
    // was never even visited -- so a ragdoll driving a character with no
    // animation had nowhere to write and the character snapped to bind pose on
    // exactly the frame it must not.
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    core::CFrameD moved;
    moved.position = core::DVec3{0.0, 9.0, 0.0};
    host.setJointOverride(fixture.mesh, 1, moved);
    host.commitOverrides();

    // A tick with no track at all. The pose must still be there afterwards.
    animation.sample(1.0 / 60.0);

    REQUIRE(animation.pose(fixture.mesh) != nullptr);
    core::CFrameD at;
    REQUIRE(host.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.y == doctest::Approx(9.0));
}

TEST_CASE("overrides last one tick and no longer")
{
    // An override that outlived the tick that set it is a ragdoll still driving
    // a character nobody is simulating.
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    core::CFrameD moved;
    moved.position = core::DVec3{4.0, 1.0, 0.0};
    host.setJointOverride(fixture.mesh, 1, moved);
    host.commitOverrides();

    core::CFrameD at;
    REQUIRE(host.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.x == doctest::Approx(4.0));

    // A second commit with nothing set changes nothing: the palette holds the
    // last committed pose rather than being rebuilt from an override that is
    // gone.
    host.commitOverrides();
    REQUIRE(host.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.x == doctest::Approx(4.0));
}

TEST_CASE("clearing an override takes the mesh out of the drive entirely")
{
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    core::CFrameD moved;
    moved.position = core::DVec3{4.0, 1.0, 0.0};
    host.setJointOverride(fixture.mesh, 1, moved);
    host.clearJointOverrides(fixture.mesh);
    host.commitOverrides();

    // Never committed, so there is no pose -- and a null pose means bind pose,
    // which is the honest answer for a mesh nothing is driving.
    CHECK(animation.pose(fixture.mesh) == nullptr);
    core::CFrameD at;
    REQUIRE(host.jointModel(fixture.mesh, 1, at));
    CHECK(at.position.x == doctest::Approx(0.0));
}

TEST_CASE("an override on a joint the rig does not have is refused")
{
    Fixture fixture;
    (void)fixture.rig(threeJointChain());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    scene::SkeletonHost& host = animation;

    host.setJointOverride(fixture.mesh, 9, core::CFrameD{});
    host.commitOverrides();
    // Nothing was driven, so nothing was built.
    CHECK(animation.pose(fixture.mesh) == nullptr);
}

TEST_CASE("the pose keeps the model transforms it used to throw away")
{
    Fixture fixture;
    const core::InstanceId player = fixture.rig(threeJointChain());
    render::SkeletonLibrary::Entry posed = threeJointChain();
    posed.clips.push_back(slideClip("slide"));
    fixture.skeletons.set(fixture.content, posed);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->model.size() == 3);
    REQUIRE(pose->local.size() == 3);
    REQUIRE(pose->palette.size() == 3);

    // `model` is joint space to MODEL space, and `palette` is that with
    // `inverseBind` folded in -- two different matrices, and confusing them is
    // what puts a socket at the origin.
    // Compared at f32. `doctest::Approx` takes a double, and letting a matrix
    // entry promote into it is a `-Wdouble-promotion` error under Clang.
    const auto nearF = [](f32 value, f32 expected) { return std::fabs(value - expected) < 0.01f; };
    CHECK(nearF(pose->model[1].m[3][1], 2.0f));
    CHECK(nearF(pose->palette[1].m[3][1], 1.0f));
    // `local` is relative to the parent: the child slid to two units above a
    // root that did not move.
    CHECK(nearF(pose->local[1].m[3][1], 2.0f));
    CHECK(nearF(pose->local[2].m[3][1], 1.0f));
}

// --- One player over several meshes -------------------------------------------

namespace {

// The same two-joint skeleton, with the joints in the OTHER order -- which is
// what two files exported separately look like. Matching by index would drive
// the child with the root's channel and twist the sleeve.
render::SkeletonLibrary::Entry shirtSkeleton()
{
    render::SkeletonLibrary::Entry entry;

    asset::Joint child;
    child.name = "child";
    child.parent = asset::Joint::NoParent;
    child.localBind.position = core::DVec3{0.0, 1.0, 0.0};
    child.inverseBind.m[3][1] = -1.0f;
    entry.joints.push_back(child);

    asset::Joint root;
    root.name = "root";
    root.parent = asset::Joint::NoParent;
    entry.joints.push_back(root);

    return entry;
}

} // namespace

TEST_CASE("one player over a Model drives every skinned mesh under it")
{
    // **A character is a body, a shirt and a pair of trousers.** Several skinned
    // meshes wearing the same skeleton, and one clip has to move all of them --
    // parented to the `Model` rather than to any one piece.
    Fixture fixture;

    render::SkeletonLibrary::Entry body = twoJointSkeleton();
    body.clips.push_back(slideClip("slide"));
    const core::NameAtom bodyContent = fixture.atoms.intern("asset://models/body.glb");
    const core::NameAtom shirtContent = fixture.atoms.intern("asset://models/shirt.glb");
    fixture.skeletons.set(bodyContent, body);
    // The shirt has NO clips of its own, which is the point: only one piece
    // needs to carry the animation.
    fixture.skeletons.set(shirtContent, shirtSkeleton());

    const core::InstanceId model = fixture.world.create(fixture.instanceClass);
    const core::InstanceId bodyMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(bodyMesh)->meshContent = bodyContent;
    REQUIRE(fixture.world.setParent(bodyMesh, model) == std::nullopt);
    const core::InstanceId shirtMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(shirtMesh)->meshContent = shirtContent;
    REQUIRE(fixture.world.setParent(shirtMesh, model) == std::nullopt);

    // Parented to the MODEL, not to a mesh.
    const core::InstanceId player = fixture.world.create(fixture.instanceClass);
    REQUIRE(fixture.world.setParent(player, model) == std::nullopt);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    REQUIRE(track != 0);
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    // The body moved: the clip slides its `child` from y = 1 to y = 3, so half a
    // second in it is at 2.
    core::CFrameD at;
    REQUIRE(animation.jointModel(bodyMesh, 1, at));
    CHECK(at.position.y == doctest::Approx(2.0).epsilon(0.01));

    // **And the shirt moved with it**, through the joint called `child` -- which
    // is index 0 in its file rather than index 1. Matched by index it would have
    // been the root that moved, and the sleeve would twist.
    REQUIRE(animation.jointModel(shirtMesh, 0, at));
    CHECK(at.position.y == doctest::Approx(2.0).epsilon(0.01));
    // The shirt's own root did not move, which is what says the remap landed on
    // the right joint rather than on all of them.
    REQUIRE(animation.jointModel(shirtMesh, 1, at));
    CHECK(at.position.y == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("tracks sharing a drive root observe reparenting and skeleton arrival each sample")
{
    Fixture fixture;
    auto body = twoJointSkeleton();
    body.clips.push_back(slideClip("slide"));
    const auto content = fixture.atoms.intern("asset://body");
    const auto clothes = fixture.atoms.intern("asset://clothes");
    fixture.skeletons.set(content, std::move(body));
    const auto root = fixture.world.create(fixture.instanceClass);
    const auto other = fixture.world.create(fixture.instanceClass);
    const auto mesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(mesh)->meshContent = content;
    REQUIRE_FALSE(fixture.world.setParent(mesh, root).has_value());
    const auto player = fixture.world.create(fixture.instanceClass);
    REQUIRE_FALSE(fixture.world.setParent(player, root).has_value());
    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const auto walk = animation.createTrack(player, {}, "slide");
    const auto attack = animation.createTrack(player, {}, "slide");
    animation.play(walk, 0.0f, 1.0f, 1.0f);
    animation.play(attack, 0.0f, 1.0f, 0.5f);
    animation.sample(0.2);
    core::CFrameD at;
    REQUIRE(animation.jointModel(mesh, 1, at));
    CHECK(at.position.y == doctest::Approx(1.3));

    const auto shirt = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(shirt)->meshContent = clothes;
    REQUIRE_FALSE(fixture.world.setParent(shirt, root).has_value());
    animation.sample(0.2);
    CHECK(animation.pose(shirt) == nullptr);
    fixture.skeletons.set(clothes, shirtSkeleton());
    animation.sample(0.2);
    REQUIRE(animation.jointModel(shirt, 0, at));
    CHECK(at.position.y == doctest::Approx(1.9));

    REQUIRE_FALSE(fixture.world.setParent(shirt, other).has_value());
    animation.sample(0.2);
    // Detached meshes keep their last pose, but no longer follow the tracks.
    REQUIRE(animation.jointModel(shirt, 0, at));
    CHECK(at.position.y == doctest::Approx(1.9));
    REQUIRE_FALSE(fixture.world.setParent(shirt, root).has_value());
    animation.stop(attack, 0.0f);
    animation.sample(0.1);
    REQUIRE(animation.jointModel(shirt, 0, at));
    CHECK(at.position.y == doctest::Approx(2.8));
}

TEST_CASE("a joint the other rig does not have is skipped rather than guessed")
{
    // A shirt with no fingers keeps its own sleeve rather than inheriting a
    // finger's rotation.
    Fixture fixture;
    render::SkeletonLibrary::Entry body = twoJointSkeleton();
    body.clips.push_back(slideClip("slide"));
    const core::NameAtom bodyContent = fixture.atoms.intern("asset://models/body.glb");
    const core::NameAtom hatContent = fixture.atoms.intern("asset://models/hat.glb");
    fixture.skeletons.set(bodyContent, body);

    // One joint, and not the one the clip drives.
    render::SkeletonLibrary::Entry hat;
    asset::Joint only;
    only.name = "root";
    only.parent = asset::Joint::NoParent;
    hat.joints.push_back(only);
    fixture.skeletons.set(hatContent, hat);

    const core::InstanceId model = fixture.world.create(fixture.instanceClass);
    const core::InstanceId bodyMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(bodyMesh)->meshContent = bodyContent;
    REQUIRE(fixture.world.setParent(bodyMesh, model) == std::nullopt);
    const core::InstanceId hatMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(hatMesh)->meshContent = hatContent;
    REQUIRE(fixture.world.setParent(hatMesh, model) == std::nullopt);

    const core::InstanceId player = fixture.world.create(fixture.instanceClass);
    REQUIRE(fixture.world.setParent(player, model) == std::nullopt);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    core::CFrameD at;
    REQUIRE(animation.jointModel(hatMesh, 0, at));
    CHECK(at.position.y == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("a player parented straight to a mesh still drives only that mesh")
{
    // The older shape, and the one a character made of one mesh uses. Widening
    // the rule must not widen it past what somebody asked for.
    Fixture fixture;
    render::SkeletonLibrary::Entry body = twoJointSkeleton();
    body.clips.push_back(slideClip("slide"));
    const core::NameAtom bodyContent = fixture.atoms.intern("asset://models/body.glb");
    const core::NameAtom shirtContent = fixture.atoms.intern("asset://models/shirt.glb");
    fixture.skeletons.set(bodyContent, body);
    fixture.skeletons.set(shirtContent, shirtSkeleton());

    const core::InstanceId model = fixture.world.create(fixture.instanceClass);
    const core::InstanceId bodyMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(bodyMesh)->meshContent = bodyContent;
    REQUIRE(fixture.world.setParent(bodyMesh, model) == std::nullopt);
    const core::InstanceId shirtMesh = fixture.world.create(fixture.meshPartClass);
    fixture.world.meshParts().find(shirtMesh)->meshContent = shirtContent;
    REQUIRE(fixture.world.setParent(shirtMesh, model) == std::nullopt);

    // Parented to the BODY.
    const core::InstanceId player = fixture.world.create(fixture.instanceClass);
    REQUIRE(fixture.world.setParent(player, bodyMesh) == std::nullopt);

    render::AnimationSystem animation(fixture.world, fixture.skeletons);
    const scene::TrackId track = animation.createTrack(player, {}, "slide");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    core::CFrameD at;
    REQUIRE(animation.jointModel(bodyMesh, 1, at));
    CHECK(at.position.y == doctest::Approx(2.0).epsilon(0.01));
    // The shirt is untouched: it has no pose at all.
    CHECK(animation.pose(shirtMesh) == nullptr);
}

// --- A clip from another file (S6.8) -----------------------------------------
//
// **One walk cycle authored once and played by every character in a game** is
// the reason a clip is addressable at all. Until now `LoadAnimation` refused a
// path that was not the player's own mesh, so a clip existed only inside the
// glTF its skeleton came from -- which means a project with twelve characters
// has twelve copies of every animation.
//
// The retargeting is not new work: `jointMapFor` has mapped joints by NAME since
// one player had to drive a body and a shirt with different rigs. What was
// missing was any way to say which file the clip was in.

TEST_CASE("a clip from another file drives this rig, matched by joint name")
{
    Fixture fixture;
    const core::InstanceId player = fixture.rig(twoJointSkeleton());

    // A second file with the same joint NAMES and the clip in it. Nothing in
    // this rig has a clip of its own, which is the case a shared library is.
    const core::NameAtom library = fixture.atoms.intern("asset://anim/locomotion.glb");
    render::SkeletonLibrary::Entry clips = twoJointSkeleton();
    clips.clips.push_back(slideClip("Walk"));
    fixture.skeletons.set(library, std::move(clips));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, library, "Walk");
    REQUIRE(track != 0);
    CHECK(close(animation.state(track).length, 1.0f));

    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    // Halfway through a slide from 1 to 3 on Y: the child joint is at 2.
    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->model.size() == 2);
    CHECK(close(pose->model[1].m[3][1], 2.0f));
}

TEST_CASE("a joint the target rig does not have is skipped rather than guessed")
{
    // A clip for a horse played on a person moves the joints they have in common
    // and no others. Guessing an index instead is a limb folded backwards.
    Fixture fixture;
    const core::InstanceId player = fixture.rig(twoJointSkeleton());

    const core::NameAtom library = fixture.atoms.intern("asset://anim/tail.glb");
    render::SkeletonLibrary::Entry clips;
    asset::Joint root;
    root.name = "root";
    root.parent = asset::Joint::NoParent;
    clips.joints.push_back(root);
    asset::Joint tail;
    tail.name = "tail";
    tail.parent = 0;
    clips.joints.push_back(tail);
    // The clip moves joint 1, which is "tail" -- a name this rig's joint 1
    // ("child") does not share.
    clips.clips.push_back(slideClip("Swish"));
    fixture.skeletons.set(library, std::move(clips));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, library, "Swish");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(0.5);

    // The child stays at its bind position, one unit up, rather than sliding to
    // two: nothing addressed it.
    const render::Pose* pose = animation.pose(fixture.mesh);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->model.size() == 2);
    CHECK(close(pose->model[1].m[3][1], 1.0f));
}

TEST_CASE("naming a file nothing has loaded is a track that plays nothing")
{
    // The same answer a name the file does not have gives, and the state a
    // `MeshPart` is in for the frames before its own file arrives. Not an error:
    // an ordinary frame must not be a nil index.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Walk"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, fixture.atoms.intern("asset://anim/absent.glb"), "Walk");
    CHECK(close(animation.state(track).length, 0.0f));
}

TEST_CASE("no file named still means the player's own mesh")
{
    // Every call that existed before this feature, and the common case after it.
    Fixture fixture;
    render::SkeletonLibrary::Entry entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Walk"));
    const core::InstanceId player = fixture.rig(std::move(entry));

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const scene::TrackId track = animation.createTrack(player, {}, "Walk");
    CHECK(close(animation.state(track).length, 1.0f));
}

TEST_CASE("animation drivers match instance order after lower slots are recycled")
{
    Fixture fixture;
    const auto recycled = fixture.world.create(fixture.meshPartClass);
    auto entry = twoJointSkeleton();
    entry.clips.push_back(slideClip("Walk"));
    const auto olderPlayer = fixture.rig(entry);
    const auto olderMesh = fixture.mesh;
    REQUIRE(fixture.world.destroy(recycled));
    fixture.world.retireDestroyed();
    const auto newerPlayer = fixture.rig(std::move(entry));
    const auto newerMesh = fixture.mesh;
    REQUIRE(newerMesh.index < olderMesh.index);
    REQUIRE(newerMesh.generation > olderMesh.generation);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    const auto olderTrack = animation.createTrack(olderPlayer, {}, "Walk");
    const auto newerTrack = animation.createTrack(newerPlayer, {}, "Walk");
    animation.play(olderTrack, 0.0f, 1.0f, 1.0f);
    animation.play(newerTrack, 0.0f, 1.0f, 0.5f);
    animation.sample(0.5);
    const auto* olderPose = animation.pose(olderMesh);
    const auto* newerPose = animation.pose(newerMesh);
    REQUIRE(olderPose != nullptr);
    REQUIRE(newerPose != nullptr);
    CHECK(close(olderPose->model[1].m[3][1], 2.0f));
    CHECK(close(newerPose->model[1].m[3][1], 1.5f));
}
