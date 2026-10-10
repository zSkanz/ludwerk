// A mesh that wears another's pose (ADR 0201, `MeshPart.PoseFrom`): a piece of
// armour skinned to a body's skeleton, a glove with a hand's joints and no
// arm's, a bow whose string the body's clip pulls. What is held here is the
// animation system's half -- whose joints a follower's are, on the tick and in
// the frame's picture, and what it costs. Where a follower is drawn and that
// it has no body are the host's (`engine_app_tests`).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <initializer_list>
#include <string>
#include <vector>

#include "engine/render/animation.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using engine::core::f32;

namespace {

struct Joint
{
    const char* name;
    int parent;
    core::DVec3 offset;
};

render::SkeletonLibrary::Entry rigOf(std::initializer_list<Joint> joints)
{
    render::SkeletonLibrary::Entry entry;
    for (const Joint& given : joints) {
        asset::Joint joint;
        joint.name = given.name;
        joint.parent = given.parent < 0 ? asset::Joint::NoParent : static_cast<core::u32>(given.parent);
        joint.localBind.position = given.offset;
        entry.joints.push_back(joint);
    }
    return entry;
}

// A body: hips, a back, an arm of three joints.
constexpr core::u32 Hips = 0;
constexpr core::u32 Arm = 2;
constexpr core::u32 Hand = 4;
render::SkeletonLibrary::Entry body()
{
    return rigOf({
        {"Hips", -1, {0.0, 1.0, 0.0}},
        {"Spine", 0, {0.0, 0.4, 0.0}},
        {"LeftArm", 1, {0.2, 0.1, 0.0}},
        {"LeftForeArm", 2, {0.3, 0.0, 0.0}},
        {"LeftHand", 3, {0.25, 0.0, 0.0}},
    });
}

// The same body with a joint under its hand for what it holds: the rig a
// clip library is made on, where a draw's clip moves a string.
constexpr core::u32 LibraryString = 5;
render::SkeletonLibrary::Entry library()
{
    render::SkeletonLibrary::Entry entry = body();
    asset::Joint string;
    string.name = "String";
    string.parent = Hand;
    string.localBind.position = core::DVec3{0.0, 0.0, 0.1};
    entry.joints.push_back(string);
    return entry;
}

// A breastplate: the body's back and nothing else, in another order.
render::SkeletonLibrary::Entry plate()
{
    return rigOf({
        {"Spine", -1, {0.0, 1.4, 0.0}},
        {"Hips", -1, {0.0, 1.0, 0.0}},
    });
}

// A glove: the hand, and a finger of its own.
constexpr core::u32 GloveHand = 0;
constexpr core::u32 GloveFinger = 1;
render::SkeletonLibrary::Entry glove()
{
    return rigOf({
        {"LeftHand", -1, {0.75, 1.5, 0.0}},
        {"Finger", 0, {0.1, 0.0, 0.0}},
    });
}

// A bow: a grip, and a string no body has.
constexpr core::u32 BowGrip = 0;
constexpr core::u32 BowString = 1;
render::SkeletonLibrary::Entry bow()
{
    return rigOf({
        {"Grip", -1, {0.0, 0.0, 0.0}},
        {"String", 0, {0.0, 0.0, 0.1}},
    });
}

// A clip that raises the arm a quarter turn and pulls the string back.
asset::AnimationClip draw()
{
    asset::AnimationClip clip;
    clip.name = "Draw";
    clip.duration = 1.0f;

    asset::AnimationChannel turn;
    turn.joint = Arm;
    turn.target = asset::AnimationChannel::Target::Rotation;
    turn.stride = 4;
    turn.times = {0.0f, 1.0f};
    const f32 half = std::sqrt(0.5f);
    turn.values = {0.0f, 0.0f, half, half, 0.0f, 0.0f, half, half};
    clip.channels.push_back(turn);

    asset::AnimationChannel pull;
    pull.joint = LibraryString;
    pull.target = asset::AnimationChannel::Target::Translation;
    pull.stride = 3;
    pull.times = {0.0f, 1.0f};
    pull.values = {0.0f, 0.0f, 0.4f, 0.0f, 0.0f, 0.4f};
    clip.channels.push_back(pull);
    return clip;
}

[[nodiscard]] core::Vec3 placeOf(const core::Mat4& matrix)
{
    return core::Vec3{matrix.m[3][0], matrix.m[3][1], matrix.m[3][2]};
}

// Bit for bit: a follower's joint IS its leader's, not near it.
[[nodiscard]] bool same(const core::Mat4& a, const core::Mat4& b)
{
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId meshPartClass = scene::InvalidClass;
    scene::ClassId playerClass = scene::InvalidClass;
    scene::ClassId boneClass = scene::InvalidClass;

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

        scene::ClassDescriptor bone;
        bone.name = atoms.intern("Bone");
        bone.super = instanceClass;
        bone.defaultName = bone.name;
        bone.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.attachments().add(id, scene::AttachmentComponent{});
        };
        bone.detachComponents = [](scene::World& w, core::InstanceId id) { w.attachments().remove(id); };
        boneClass = classes.registerClass(bone);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    render::SkeletonLibrary skeletons;
    core::NameAtom clips = atoms.intern("asset://clips/library.glb");
    core::InstanceId hero;
    core::InstanceId player;

    core::InstanceId mesh(const char* content, render::SkeletonLibrary::Entry rig, core::InstanceId parent = {})
    {
        const core::NameAtom name = atoms.intern(content);
        if (skeletons.find(name) == nullptr)
            skeletons.set(name, std::move(rig));
        const core::InstanceId id = world.create(meshPartClass);
        world.meshParts().find(id)->meshContent = name;
        if (parent.valid())
            REQUIRE_FALSE(world.setParent(id, parent).has_value());
        return id;
    }

    // A body with a player on it, and the library its clips are in.
    void body()
    {
        render::SkeletonLibrary::Entry from = library();
        from.clips.push_back(draw());
        skeletons.set(clips, std::move(from));
        hero = mesh("asset://models/hero.glb", ::body());
        player = world.create(playerClass);
        REQUIRE_FALSE(world.setParent(player, hero).has_value());
        // By equal names: the body and the library are one skeleton, the
        // library with a joint more.
        world.animationPlayers().find(player)->retargeting = 1;
    }

    void wear(core::InstanceId piece, core::InstanceId leader) { world.meshParts().find(piece)->poseFrom = leader; }

    // A `Bone` on a joint of `on`, by name.
    core::InstanceId bone(core::InstanceId on, const char* joint, core::DVec3 offset = {})
    {
        const core::InstanceId id = world.create(boneClass);
        scene::AttachmentComponent* attachment = world.attachments().find(id);
        attachment->jointName = atoms.intern(joint);
        attachment->cframe.position = offset;
        REQUIRE_FALSE(world.setParent(id, on).has_value());
        return id;
    }
};

// The hero drawing its bow, a tick in.
scene::TrackId drawing(render::AnimationSystem& animation, Fixture& fixture)
{
    const scene::TrackId track = animation.createTrack(fixture.player, fixture.clips, "Draw");
    animation.play(track, 0.0f, 1.0f, 1.0f);
    animation.sample(1.0 / 60.0);
    return track;
}

} // namespace

TEST_CASE("ADR 0201: a joint both have is the leader's, exactly, and a piece with part of a skeleton is on the body")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId armour = fixture.mesh("asset://models/plate.glb", plate());
    const core::InstanceId mitt = fixture.mesh("asset://models/glove.glb", glove());
    fixture.wear(armour, fixture.hero);
    fixture.wear(mitt, fixture.hero);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);

    const render::Pose* led = animation.pose(fixture.hero);
    const render::Pose* worn = animation.pose(armour);
    const render::Pose* gloved = animation.pose(mitt);
    REQUIRE(led != nullptr);
    REQUIRE(worn != nullptr);
    REQUIRE(gloved != nullptr);
    CHECK(animation.leaderOf(armour) == fixture.hero);
    CHECK_FALSE(animation.leaderOf(fixture.hero).valid());

    // Matched by name, whatever order each file has them in.
    CHECK(same(worn->model[0], led->model[1]));
    CHECK(same(worn->model[1], led->model[Hips]));

    // **The arm is raised, and the glove is on the hand at the end of it**:
    // a glove has no arm, and posed on its own its hand hung from nothing.
    CHECK(same(gloved->model[GloveHand], led->model[Hand]));
    CHECK(static_cast<double>(placeOf(led->model[Hand]).y) > 1.8);
    // Its own finger rides the hand, a tenth of a metre along it.
    const core::Vec3 finger = placeOf(gloved->model[GloveFinger]) - placeOf(gloved->model[GloveHand]);
    CHECK(static_cast<double>(core::length(finger)) == doctest::Approx(0.1).epsilon(1.0e-4));

    // And nothing was walked for either: every joint they have is the
    // leader's or at rest. One track was walked this tick, for the body.
    CHECK(animation.tracksWalked() == 1);
    CHECK(animation.followersBuilt() >= 2);
}

TEST_CASE("ADR 0201: a joint only the follower has is moved by the leader's track that names it, and rests otherwise")
{
    Fixture fixture;
    fixture.body();
    // In the hand: under a `Bone` of the body, since a bow shares no joint
    // with it and has to be somewhere.
    const core::InstanceId grip = fixture.bone(fixture.hero, "LeftHand", core::DVec3{0.0, 0.05, 0.0});
    const core::InstanceId held = fixture.mesh("asset://models/bow.glb", bow(), grip);
    fixture.wear(held, fixture.hero);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};

    // Nothing playing: the string is where the bow's file rests it.
    animation.sample(1.0 / 60.0);
    const render::Pose* resting = animation.pose(held);
    REQUIRE(resting != nullptr);
    CHECK(static_cast<double>(placeOf(resting->local[BowString]).z) == doctest::Approx(0.1).epsilon(1.0e-5));

    // The body draws: its clip pulls a joint the BODY does not have, and the
    // bow does. One player, one clip, both moved.
    (void)drawing(animation, fixture);
    const render::Pose* led = animation.pose(fixture.hero);
    const render::Pose* drawn = animation.pose(held);
    REQUIRE(led != nullptr);
    REQUIRE(drawn != nullptr);
    CHECK(static_cast<double>(placeOf(drawn->local[BowString]).z) == doctest::Approx(0.4).epsilon(1.0e-5));
    // The body's walk, and the bow's for the one track that names its string.
    CHECK(animation.tracksWalked() == 2);

    // **And the bow is in the hand**: its root on the hand's joint, at the
    // bone's own offset.
    const core::Vec3 hand = placeOf(led->model[Hand]);
    const core::Vec3 root = placeOf(drawn->model[BowGrip]);
    CHECK(static_cast<double>(core::length(root - hand)) == doctest::Approx(0.05).epsilon(1.0e-3));

    // Handed to the hips: parented under another bone, it is there the next
    // tick.
    const core::InstanceId belt = fixture.bone(fixture.hero, "Hips");
    REQUIRE_FALSE(fixture.world.setParent(held, belt).has_value());
    animation.sample(1.0 / 60.0);
    const render::Pose* moved = animation.pose(held);
    REQUIRE(moved != nullptr);
    CHECK(static_cast<double>(core::length(placeOf(moved->model[BowGrip]) - placeOf(led->model[Hips]))) ==
          doctest::Approx(0.0).epsilon(1.0e-3));
}

TEST_CASE("ADR 0201: put on and taken off in play, the next tick each way")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId mitt = fixture.mesh("asset://models/glove.glb", glove());

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);
    // Nobody's: it has no pose from anybody.
    CHECK_FALSE(animation.leaderOf(mitt).valid());

    fixture.wear(mitt, fixture.hero);
    animation.sample(1.0 / 60.0);
    const render::Pose* led = animation.pose(fixture.hero);
    const render::Pose* gloved = animation.pose(mitt);
    REQUIRE(led != nullptr);
    REQUIRE(gloved != nullptr);
    CHECK(same(gloved->model[GloveHand], led->model[Hand]));

    fixture.wear(mitt, core::InstanceId{});
    animation.sample(1.0 / 60.0);
    CHECK_FALSE(animation.leaderOf(mitt).valid());
    // A mesh on its own again: where its own file rests it, or nowhere.
    if (const render::Pose* alone = animation.pose(mitt); alone != nullptr)
        CHECK_FALSE(same(alone->model[GloveHand], animation.pose(fixture.hero)->model[Hand]));
}

TEST_CASE("ADR 0201: a chain of pieces is the first leader's, and a loop follows nobody")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId armour = fixture.mesh("asset://models/plate.glb", plate());
    const core::InstanceId trim = fixture.mesh("asset://models/trim.glb", plate());
    fixture.wear(armour, fixture.hero);
    fixture.wear(trim, armour);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);
    CHECK(animation.leaderOf(trim) == fixture.hero);
    const render::Pose* led = animation.pose(fixture.hero);
    const render::Pose* edged = animation.pose(trim);
    REQUIRE(led != nullptr);
    REQUIRE(edged != nullptr);
    CHECK(same(edged->model[1], led->model[Hips]));

    // Each names the other: neither has a leader, and nothing hangs.
    const core::InstanceId one = fixture.mesh("asset://models/one.glb", plate());
    const core::InstanceId two = fixture.mesh("asset://models/two.glb", plate());
    fixture.wear(one, two);
    fixture.wear(two, one);
    const core::InstanceId self = fixture.mesh("asset://models/self.glb", plate());
    fixture.wear(self, self);
    animation.sample(1.0 / 60.0);
    CHECK_FALSE(animation.leaderOf(one).valid());
    CHECK_FALSE(animation.leaderOf(two).valid());
    CHECK_FALSE(animation.leaderOf(self).valid());
}

TEST_CASE("ADR 0201: what the frame did to the leader's joints is done to the follower's, and to no tick")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId mitt = fixture.mesh("asset://models/glove.glb", glove());
    fixture.wear(mitt, fixture.hero);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);
    const core::Mat4 ticked = animation.pose(mitt)->model[GloveHand];

    // A limb reaching, a foot on a stair, a chain swinging: something put
    // the body's hand somewhere else in this frame's picture.
    core::Mat4 reached = animation.pose(fixture.hero)->model[Hand];
    reached.m[3][0] += 0.3f;
    reached.m[3][1] -= 0.2f;
    const render::AnimationSystem::PresentedJoint moved[] = {{Hand, reached}};
    animation.clearPresented();
    animation.present(fixture.hero, moved, false);
    animation.presentFollowers();

    const render::Pose* shown = animation.drawnPose(mitt);
    REQUIRE(shown != nullptr);
    // **The seam does not open**: the glove is where the hand was put.
    CHECK(same(shown->model[GloveHand], reached));
    // Its own finger went with it.
    const core::Vec3 finger = placeOf(shown->model[GloveFinger]) - placeOf(shown->model[GloveHand]);
    CHECK(static_cast<double>(core::length(finger)) == doctest::Approx(0.1).epsilon(1.0e-4));
    // And the tick's pose is the tick's: picture never reaches it.
    CHECK(same(animation.pose(mitt)->model[GloveHand], ticked));

    // A frame in which nothing corrected the leader corrects no follower.
    animation.clearPresented();
    animation.presentFollowers();
    const render::Pose* plain = animation.drawnPose(mitt);
    REQUIRE(plain != nullptr);
    CHECK(same(plain->model[GloveHand], ticked));
}

TEST_CASE("ADR 0201: a joint a ragdoll put somewhere is where the follower's is")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId mitt = fixture.mesh("asset://models/glove.glb", glove());
    fixture.wear(mitt, fixture.hero);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);

    core::CFrameD fallen;
    fallen.position = core::DVec3{2.0, 0.25, -1.0};
    animation.setJointOverride(fixture.hero, Hand, fallen);
    animation.commitOverrides();

    const render::Pose* led = animation.pose(fixture.hero);
    const render::Pose* gloved = animation.pose(mitt);
    REQUIRE(led != nullptr);
    REQUIRE(gloved != nullptr);
    CHECK(static_cast<double>(placeOf(led->model[Hand]).x) == doctest::Approx(2.0).epsilon(1.0e-4));
    CHECK(same(gloved->model[GloveHand], led->model[Hand]));
}

TEST_CASE("ADR 0201: a follower in view is a reason to pose its leader, though nothing of the leader is seen")
{
    Fixture fixture;
    fixture.body();
    const core::InstanceId armour = fixture.mesh("asset://models/plate.glb", plate());
    fixture.wear(armour, fixture.hero);

    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    (void)drawing(animation, fixture);
    // The renderer says what it drew: the suit, at a good size, and no body
    // under it -- the body of a modular character is often not drawn at all.
    const render::SeenSkin seen[] = {{armour, 0.5f}};
    animation.reportSeen(seen, true);
    const core::u64 before = animation.posesBuilt();
    const core::u64 followed = animation.followersBuilt();
    for (int tick = 0; tick < 8; ++tick) {
        animation.sample(1.0 / 60.0);
        animation.reportSeen(seen, true);
    }
    // Posed every tick, both of them.
    CHECK(animation.posesBuilt() >= before + 8);
    CHECK(animation.followersBuilt() >= followed + 8);
    CHECK(animation.seenLately(fixture.hero));
}

// --- What a follower costs (ADR 0201) ------------------------------------------------

namespace {

// A body of forty joints in a chain, with a clip that turns every one.
render::SkeletonLibrary::Entry longBody(int joints)
{
    render::SkeletonLibrary::Entry entry;
    asset::AnimationClip clip;
    clip.name = "Move";
    clip.duration = 2.0f;
    for (int index = 0; index < joints; ++index) {
        asset::Joint joint;
        joint.name = "J" + std::to_string(index);
        joint.parent = index == 0 ? asset::Joint::NoParent : static_cast<core::u32>(index - 1);
        joint.localBind.position = core::DVec3{0.0, 0.05, 0.0};
        entry.joints.push_back(joint);

        asset::AnimationChannel turn;
        turn.joint = static_cast<core::u32>(index);
        turn.target = asset::AnimationChannel::Target::Rotation;
        turn.stride = 4;
        turn.times = {0.0f, 1.0f, 2.0f};
        const f32 half = std::sqrt(0.5f);
        turn.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, half, half, 0.0f, 0.0f, 0.0f, 1.0f};
        clip.channels.push_back(turn);
    }
    entry.clips.push_back(clip);
    return entry;
}

// A piece: a dozen of the body's joints, by name.
render::SkeletonLibrary::Entry pieceOf(int first, int count)
{
    render::SkeletonLibrary::Entry entry;
    for (int index = 0; index < count; ++index) {
        asset::Joint joint;
        joint.name = "J" + std::to_string(first + index);
        joint.parent = index == 0 ? asset::Joint::NoParent : static_cast<core::u32>(index - 1);
        joint.localBind.position = core::DVec3{0.0, 0.05, 0.0};
        entry.joints.push_back(joint);
    }
    return entry;
}

enum class Dressed
{
    Bare,
    Followers,
    UnderAModel,
};

// What a tick's pose pass costs over three hundred ticks, in microseconds: the
// MEAN, because a body at this size on the screen is posed one tick in three
// and a median would be a tick on which nothing was.
double tickCost(int heroes, Dressed how, bool together)
{
    Fixture fixture;
    const core::NameAtom bodyName = fixture.atoms.intern("asset://models/long.glb");
    fixture.skeletons.set(bodyName, longBody(40));
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    std::vector<render::SeenSkin> seen;
    for (int hero = 0; hero < heroes; ++hero) {
        // Under a model when the pieces are posed today's way: a player on
        // the model drives every skinned mesh under it, each on its own.
        const core::InstanceId model = fixture.world.create(fixture.instanceClass);
        const core::InstanceId body = fixture.world.create(fixture.meshPartClass);
        fixture.world.meshParts().find(body)->meshContent = bodyName;
        REQUIRE_FALSE(fixture.world.setParent(body, model).has_value());
        seen.push_back({body, 0.5f});
        const core::InstanceId player = fixture.world.create(fixture.playerClass);
        REQUIRE_FALSE(fixture.world.setParent(player, how == Dressed::UnderAModel ? model : body).has_value());
        fixture.world.animationPlayers().find(player)->retargeting = 1;
        if (how != Dressed::Bare) {
            for (int piece = 0; piece < 5; ++piece) {
                const std::string content = "asset://models/piece" + std::to_string(piece) + ".glb";
                const core::InstanceId worn = fixture.mesh(content.c_str(), pieceOf(piece * 6, 12), model);
                if (how == Dressed::Followers)
                    fixture.wear(worn, body);
                seen.push_back({worn, 0.5f});
            }
        }
        const scene::TrackId track = animation.createTrack(player, bodyName, "Move");
        animation.play(track, 0.0f, 1.0f, 1.0f);
        // Spread over the clip, as a crowd is -- or all at one moment of it.
        // At a speed that never brings a clip back to a moment it was at: a
        // pose it had before is kept (H10), and what is measured here is a
        // pose being MADE.
        animation.adjustSpeed(track, together ? 1.0137f : 1.0137f + 0.0101f * static_cast<f32>(hero % 37));
    }
    std::vector<double> took;
    for (int tick = 0; tick < 360; ++tick) {
        animation.reportSeen(seen, true);
        const auto from = std::chrono::steady_clock::now();
        animation.sample(1.0 / 60.0);
        const double micros =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count();
        if (tick >= 60)
            took.push_back(micros);
    }
    double sum = 0.0;
    for (const double one : took)
        sum += one;
    MESSAGE("  poses built " << animation.posesBuilt() << ", shared " << animation.posesShared() << ", followers "
                             << animation.followersBuilt() << ", tracks walked " << animation.tracksWalked());
    return sum / static_cast<double>(took.size());
}

} // namespace

TEST_CASE("ADR 0201: a hero's five pieces cost less worn than posed, and a crowd of fifty is measured")
{
    const double bare = tickCost(1, Dressed::Bare, false);
    const double worn = tickCost(1, Dressed::Followers, false);
    const double posed = tickCost(1, Dressed::UnderAModel, false);
    MESSAGE("one hero, microseconds a tick: alone " << bare << ", with five followers " << worn
                                                    << ", with five pieces posed under a model " << posed);
    const double crowdBare = tickCost(50, Dressed::Bare, false);
    const double crowdWorn = tickCost(50, Dressed::Followers, false);
    const double crowdPosed = tickCost(50, Dressed::UnderAModel, false);
    const double crowdTogether = tickCost(50, Dressed::Followers, true);
    MESSAGE("fifty heroes spread over the clip: alone " << crowdBare << ", with five followers each " << crowdWorn
                                                        << ", with five pieces posed each " << crowdPosed
                                                        << "; all at one moment, followers " << crowdTogether);
    // The claim that is the feature's: a piece that is worn is not a pose.
    // Loose, because a machine that is building something else is slow in
    // both -- the numbers above are what is read.
    CHECK(crowdWorn < crowdPosed);
}
