// Skeletal animation: clip playback and linear blending (roadmap M6,
// api-design.md §2.2's `AnimationPlayer` and `AnimationTrack`).
//
// It lives in `render` because `render` is the module that already has both
// halves: `asset`, which owns the clips a glTF file carries, and `scene`, which
// owns the tree the players sit in. `script` reaches it through
// `scene::AnimationHost`, which is the `IPhysics3D*` arrangement one layer up.
//
// **Sampling is deterministic and runs at `PreAnimation`.** Track time, `Speed`
// and `Weight` all advance on the SimClock; nothing here reads a wall clock, and
// blending walks tracks in **load order** rather than in whatever order a
// container hands them over -- R10 forbids the second, and two tracks at weight
// 0.5 have to blend the same way on every run.
//
// v1 is clip playback and linear blending. No state machines, no IK, no root
// motion and no additive or masked blending: the roadmap's words are "enough for
// idle/walk/jump".
#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/asset/model.h"
#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/scene/animation_host.h"
#include "engine/scene/skeleton_host.h"

namespace engine::scene {
class World;
}

namespace engine::render {

using core::f32;
using core::f64;
using core::u32;
using core::usize;

// One loaded skeleton and its clips, keyed by the content URN the `MeshPart`
// names. Populated by `MeshLoader` beside the GPU geometry, because the file is
// read once and both halves come out of it.
class SkeletonLibrary
{
public:
    struct Entry
    {
        std::vector<asset::Joint> joints;
        std::vector<asset::AnimationClip> clips;
    };

    void set(core::NameAtom content, Entry entry);
    void clear() noexcept;
    // Counted up on every change: what a pose remembered from an earlier
    // skeleton or clip is checked against (H10).
    [[nodiscard]] core::u64 revision() const noexcept { return revision_; }

    // Null for a URN nothing has loaded, or one whose file had no skeleton --
    // which is most of them.
    [[nodiscard]] const Entry* find(core::NameAtom content) const noexcept;

private:
    // Sorted by atom, like `MeshLibrary`, and for the same reason: R10 forbids
    // an unordered container's iteration reaching observable output.
    struct Slot
    {
        core::NameAtom content;
        Entry entry;
    };
    std::vector<Slot> entries_;
    core::u64 revision_ = 0;
};

// The pose one skinned mesh is in, ready for a draw. Model space, one matrix per
// joint, `joint * inverseBind` already combined -- which is what a vertex shader
// multiplies by and is therefore the only form worth storing.
//
// **Keyed by the `MeshPart`, not by the `AnimationPlayer`.** A pose is a
// property of a skeleton, and the skeleton belongs to the mesh; two players
// parented to one mesh are two sources blending into one pose, which is what
// they look like on screen. It is also what lets `extract` ask for a pose with
// the id it already has.
struct Pose
{
    std::vector<core::Mat4> palette;

    // The same joints before `inverseBind` was folded in: joint space to MODEL
    // space, which is what anything other than a vertex shader wants. A socket
    // on a hand needs where the hand IS, and the palette says where it moved
    // FROM its bind pose -- two different matrices, and the second is useless
    // for the question.
    //
    // Free to keep: `rebuildPose` computes it into a scratch and threw it away.
    std::vector<core::Mat4> model;

    // Each joint relative to its parent, this tick.
    //
    // Kept so that an OVERRIDE is cheap: replacing one joint's model transform
    // and re-running the forward pass needs every other joint's local, and
    // recovering it from `model` would be an inverse per joint per tick.
    std::vector<core::Mat4> local;
};

// **A skinned mesh the renderer drew a frame of** (H3): the camera or a shadow
// reached it, and its bounding sphere's diameter covered `screenHeight` of the
// picture's height. What decides how often a mesh nobody needs every tick of
// is posed.
struct SeenSkin
{
    core::InstanceId meshPart;
    f32 screenHeight = 0.0f;
};

// **Two hosts, one implementation**, and they are separate interfaces on
// purpose: `AnimationHost` is about tracks and weights and names no joint,
// while `SkeletonHost` is about joints and names no track. A caller that wants
// a socket on a hand should not have to link the thing that plays clips.
class AnimationSystem final : public scene::AnimationHost, public scene::SkeletonHost
{
public:
    // The world and the library are references the system keeps: it is created
    // by `app` after both exist and destroyed before either does, which is the
    // same lifetime `PhysicsSync` has.
    AnimationSystem(const scene::World& world, const SkeletonLibrary& skeletons);

    [[nodiscard]] scene::TrackId createTrack(core::InstanceId player, core::NameAtom content,
                                             std::string_view clip) override;
    void play(scene::TrackId track, f32 fadeTime, f32 weight, f32 speed) override;
    void stop(scene::TrackId track, f32 fadeTime) override;
    void adjustWeight(scene::TrackId track, f32 weight, f32 fadeTime) override;
    void adjustSpeed(scene::TrackId track, f32 speed) override;
    void setLooped(scene::TrackId track, bool looped) override;
    [[nodiscard]] scene::TrackState state(scene::TrackId track) const override;
    void sample(f64 fixedDt) override;
    [[nodiscard]] std::span<const scene::TrackId> drainEnded() override;
    void retire(const scene::World& world) override;

    // --- scene::SkeletonHost ------------------------------------------------

    [[nodiscard]] core::u32 jointCount(core::InstanceId meshPart) const override;
    [[nodiscard]] core::i32 findJoint(core::InstanceId meshPart, std::string_view name) const override;
    [[nodiscard]] core::i32 jointParent(core::InstanceId meshPart, core::u32 joint) const override;
    [[nodiscard]] std::string_view jointName(core::InstanceId meshPart, core::u32 joint) const override;
    [[nodiscard]] bool jointModel(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const override;
    void setJointOverride(core::InstanceId meshPart, core::u32 joint, const core::CFrameD& model) override;
    void clearJointOverrides(core::InstanceId meshPart) override;
    void commitOverrides() override;

    // The pose of one `MeshPart`, or null for a mesh with no skeleton or nothing
    // driving it. Read by the renderer; null means "draw it in bind pose", which
    // is what an unanimated skinned mesh should look like.
    [[nodiscard]] const Pose* pose(core::InstanceId meshPart) const noexcept;

    // How many poses have been built since this system was made: what a test
    // counts to know that a pose nobody changed was not built again.
    [[nodiscard]] core::u64 posesBuilt() const noexcept { return posesBuilt_; }

    // **What the renderer saw** (H3), once a frame: every skinned mesh the
    // camera or a shadow reached, and how big. `fresh` starts the frame's list
    // -- the main view's -- and the views drawn after it add to it. Until the
    // first report every mesh is posed every tick, which is what a server and
    // a replay, with nobody looking, keep doing.
    void reportSeen(std::span<const SeenSkin> seen, bool fresh);

    // Whether `meshPart` wears a skeleton this system poses: what the renderer
    // reports on.
    [[nodiscard]] bool animates(core::InstanceId meshPart) const;

    // **`GraphicsService.AnimationDetail`**: a scale on the size a mesh is
    // taken to be when its update interval is chosen, 1 by default. A camera
    // that stands far from a crowd sees every rig small, and at 1 poses nearly
    // all of them at a half or a quarter of the ticks.
    void setDetail(f32 detail) noexcept { detail_ = detail > 0.0f ? detail : 1.0f; }
    [[nodiscard]] f32 detail() const noexcept { return detail_; }

    // How often a mesh seen this big is posed: every tick, or every second,
    // fourth or eighth. Public so the thresholds are a thing a test holds.
    [[nodiscard]] static core::u32 updateInterval(f32 screenHeight) noexcept
    {
        return screenHeight >= 0.12f ? 1u : screenHeight >= 0.06f ? 2u : screenHeight >= 0.03f ? 4u : 8u;
    }

private:
    struct Track
    {
        core::InstanceId player;
        // The `MeshPart` the player was parented to, and the content URN of its
        // skeleton -- both resolved once at creation. A player that is reparented
        // or whose mesh changes content keeps playing against the rig the track
        // was made for, which is honest: a different skeleton is a different rig,
        // and a joint index means nothing across the two.
        //
        // It is also what lets `retire` find the pose of a player that has
        // already been destroyed, when asking the tree is no longer possible.
        core::InstanceId meshPart;
        core::NameAtom content;

        // **The instance the player is parented to, when that is not a
        // `MeshPart`.** A character is a body, a shirt and a pair of trousers --
        // several skinned meshes wearing the same skeleton -- and one clip has
        // to drive all of them. Parent the player to the `Model` and this is the
        // model; every skinned mesh under it is driven by this track.
        //
        // Invalid for a player parented straight to a mesh, which is the older
        // and still-supported shape: that track drives that mesh and no other.
        core::InstanceId driveRoot;
        // What was asked for: the file the clip is in (none for the mesh's
        // own) and its name (empty for the file's first). Kept so a track made
        // before its file arrived binds when it does (D509).
        core::NameAtom clipFrom;
        std::string clipName;
        // Index into the entry's `clips`, or `NoClip`.
        u32 clip = NoClip;
        f64 time = 0.0;
        f32 length = 0.0f;
        f32 speed = 1.0f;
        // What the track is blended with now -- the fade's own number.
        f32 weight = 1.0f;
        // **What the script set**, which a fade never writes (D438): `Stop`
        // faded `weight` to zero and `Play` then faded in "to the track's own
        // weight" -- zero -- so an idle played, stopped and played again showed
        // nothing the second time.
        f32 ownWeight = 1.0f;
        // A `Stop` fading out. `Play` ends it; a `Weight` written meanwhile
        // is kept for the next `Play` and does not bring the track back.
        bool stopping = false;
        // Where the weight is going and how long is left to get there. A target
        // and a REMAINING time rather than a start/end pair, because
        // `AdjustWeight` may retarget one mid-fade and a pair would then have to
        // invent a new start -- and rather than a rate, because a rate has to be
        // compared against the target to know when it has arrived and floating
        // point makes that comparison a coin toss.
        f32 targetWeight = 1.0f;
        f64 fadeRemaining = 0.0;
        bool looped = false;
        bool playing = false;
        // A non-looping clip that reached its end HOLDS its last frame until it
        // is stopped or replayed. `playing` is already false by then -- the two
        // are different questions, and answering the second with the first would
        // snap a character back to bind pose for the one tick between `Ended`
        // and the handler that reacts to it.
        bool holding = false;
        bool alive = true;
        // **What the track was when a pose last took it in** (D516): a track
        // not playing, at the time, weight and hold it was posed at, has
        // nothing new to say -- and three hundred idle bodies in a pool were
        // three hundred poses rebuilt to the same answer every tick.
        bool posed = false;
        bool posedPlaying = false;
        bool posedHolding = false;
        f32 posedWeight = 0.0f;
        f64 posedTime = 0.0;
    };

    // Whether a track has nothing to change in a pose: stopped or holding,
    // and as it was when it was last posed.
    [[nodiscard]] static bool quiet(const Track& track) noexcept
    {
        return !track.playing && track.posed && !track.posedPlaying && track.posedHolding == track.holding &&
               track.posedWeight == track.weight && track.posedTime == track.time;
    }

    static constexpr u32 NoClip = 0xFFFFFFFFu;

    // Finds the track's mesh and clip, if they are there yet. True once bound.
    bool bindTrack(Track& track) const;

    // `drivers`: the tracks that drive this mesh, in track order, when the
    // tick has them indexed (H10); null asks every track.
    // `quantise`: a mesh posed at a reduced rate samples its clips at their
    // own key times, so the crowd at that rate shares poses (H10).
    void rebuildPose(core::InstanceId meshPart, const SkeletonLibrary::Entry& skeleton,
                     std::span<const u32> drivers = {}, bool indexed = false, bool quantise = false);
    // The time a track's clip is sampled at: its own, or its key's.
    [[nodiscard]] f32 sampleTime(const Track& track, bool quantise);
    // `Bone.Transform` (G9): the rig a bone is on, and every turn its bones ask
    // of it.
    [[nodiscard]] core::InstanceId rigOf(core::InstanceId bone) const;
    [[nodiscard]] std::vector<std::pair<core::u32, core::Mat4>> boneOffsets(core::InstanceId meshPart,
                                                                            core::usize jointCount) const;

    // The skeleton a `MeshPart` renders, or null. One lookup, written once.
    [[nodiscard]] const SkeletonLibrary::Entry* skeletonOf(core::InstanceId meshPart) const;

    // Whether this track drives this mesh: it is the track's own mesh, or the
    // mesh is a skinned descendant of the track's drive root.
    // The first skinned mesh under `root` that carries clips, in tree order.
    // Where a player parented to a `Model` takes its animation data from.
    [[nodiscard]] core::InstanceId clipSourceUnder(core::InstanceId root) const;

    [[nodiscard]] bool drives(const Track& track, core::InstanceId meshPart) const;

    // How the CLIP's joint indices map onto another rig's, by name.
    //
    // **By name and never by index.** A body and a shirt exported as two files
    // wear the same skeleton in the sense that matters -- the same joints, named
    // the same -- and in no other: an exporter is free to order them differently,
    // and applying a clip through the wrong index twists a sleeve in a way that
    // looks like a broken animation rather than like a mismatched rig.
    //
    // Cached per (from, to) content pair, because a crowd of a hundred
    // characters is two rigs and one map -- and computed once, since a rig does
    // not change under its own URN.
    struct JointMap
    {
        core::NameAtom from;
        core::NameAtom to;
        // `slots[i]` is the joint in `to` that joint `i` of `from` is, or -1.
        std::vector<core::i32> slots;
    };

    // The identity is returned as null: a clip applied to its own rig needs no
    // map, which is every character that is one mesh.
    [[nodiscard]] const JointMap* jointMapFor(core::NameAtom from, core::NameAtom to) const;

    // A joint's model transform with no pose at all: the rest chain, walked
    // parents-first. What a character standing in bind pose answers.
    [[nodiscard]] static core::Mat4 restModelOf(const SkeletonLibrary::Entry& skeleton, core::u32 joint);

    // One mesh's overrides for this tick. Sorted by joint, so applying them is a
    // merge against the forward pass rather than a lookup per joint -- and so
    // that two overrides of one joint resolve the same way every time (R10).
    struct Override
    {
        core::u32 joint = 0;
        core::Mat4 model;
    };

    struct OverrideSet
    {
        core::InstanceId meshPart;
        std::vector<Override> joints;
    };

    [[nodiscard]] OverrideSet* overridesFor(core::InstanceId meshPart) noexcept;
    [[nodiscard]] const OverrideSet* overridesFor(core::InstanceId meshPart) const noexcept;

    const scene::World* world_ = nullptr;
    const SkeletonLibrary* skeletons_ = nullptr;

    // Index 0 is never handed out, so a `TrackId` of 0 can mean "none" the way
    // an invalid `InstanceId` does.
    std::vector<Track> tracks_{Track{}};
    std::vector<scene::TrackId> ended_;
    std::vector<scene::TrackId> endedDrained_;

    // One pose per skinned mesh that has tracks. Keyed rather than pooled because
    // a `MeshPart` is an Instance and this is not scene's storage -- and because
    // the count is a handful even in a crowd.
    //
    // **Held, not owned**: the meshes of a crowd on one rig, one clip and one
    // moment hold the one pose that was built (H10). Each had a copy of it --
    // three arrays of matrices a body a tick, which was most of what a pose
    // cost a crowd. Nothing writes to a pose another may hold: what builds or
    // overrides one takes it alone first (`ownPose`).
    std::unordered_map<core::u64, std::shared_ptr<const Pose>> poses_;
    // This mesh's pose to write to: the one it holds, when nothing else does;
    // a copy of it otherwise, or a new one.
    [[nodiscard]] Pose& ownPose(core::InstanceId meshPart, bool keep);
    // The rigs a `Bone` that names a joint hangs from, this tick, in id order:
    // the only ones whose children are searched for bones to turn a pose by.
    std::vector<core::InstanceId> boned_;

    // Scratch, reused so a steady-state tick allocates nothing.
    //
    // The pose accumulates per COMPONENT rather than per joint transform,
    // because a joint no channel drives has to keep its rest transform: one
    // weighted average over all three would collapse an undriven joint to the
    // origin, which is what makes a clip that animates one arm eat the other.
    // The meshes whose pose this tick has to rebuild, collected before the walk
    // so it is one pass per mesh rather than one per track.
    std::vector<core::InstanceId> meshes_;
    core::u64 posesBuilt_ = 0;
    // **Which tracks drive which mesh this tick** (H10), as (mesh key, track)
    // sorted: a pose asked every track in the world whether it drove its mesh
    // -- a thousand tracks for each of hundreds of meshes, each walking the
    // mesh's ancestors.
    std::vector<std::pair<core::u64, u32>> drivers_;
    std::vector<u32> driving_;
    // **Poses shared** (H10): what a pose was built from -- the rig, and each
    // track's clip, time and weight -- to the pose. A mesh with the same
    // inputs copies the answer: bit for bit what it would have computed, on
    // whatever tick it is asked, for as long as the rigs and clips are the
    // ones it was built from. Kept while used; a crowd at a reduced rate,
    // posed on staggered ticks, shares across them.
    struct SignatureHash
    {
        [[nodiscard]] core::usize operator()(const std::vector<core::u64>& signature) const noexcept
        {
            // **Every bit of a word reaches every bit of the hash.** It was
            // FNV's step taken a 64-bit word at a time, and a multiplication
            // carries upwards only: the clip's time is the HIGH half of its
            // word, so every moment of one clip had the same low bits -- the
            // bits a table takes its bucket from -- and the index was one
            // chain a clip, walked end to end for every body of a crowd.
            core::u64 hash = 0x9E3779B97F4A7C15ull;
            for (const core::u64 word : signature) {
                hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
                hash ^= hash >> 33;
            }
            hash *= 0xC4CEB9FE1A85EC53ull;
            hash ^= hash >> 33;
            return static_cast<core::usize>(hash);
        }
    };
    struct SharedPose
    {
        std::shared_ptr<const Pose> pose;
        core::u64 used = 0;
    };
    std::unordered_map<std::vector<core::u64>, SharedPose, SignatureHash> shared_;
    // **How long a pose nobody took is kept, and how many are.** A body comes
    // back to a moment of its clip once a loop, and a crowd on one clip is
    // spread over every moment of it -- so a pose is worth keeping for a
    // loop and more, not the half second it was: at thirty-two ticks and a
    // thousand entries a horde of five hundred built two poses in three, and
    // at these builds one in thirty. Four seconds of ticks; and eight
    // thousand poses of eight joints are twelve megabytes.
    //
    // **And by what they hold, not only by how many** (`MostSharedJoints`): a
    // pose is three matrices a joint, and eight thousand poses of a rig of
    // sixty joints would be ninety megabytes. Sixteen megabytes of joints at
    // the most, whatever the rigs are.
    static constexpr core::u64 SharedPoseTicks = 256;
    static constexpr core::usize MostSharedPoses = 8192;
    static constexpr core::usize MostSharedJoints = (16u << 20) / (3u * sizeof(core::Mat4));
    // The joints of every pose the index holds.
    core::usize sharedJoints_ = 0;
    // Where the tick's sweep of the index is, and what it found to let go.
    core::usize sharedSweep_ = 0;
    std::vector<const std::vector<core::u64>*> expired_;
    core::u64 sharedRevision_ = 0;
    std::vector<core::u64> signature_;
    // A clip's key period, by (content, clip): what a quantised time rounds to.
    std::unordered_map<core::u64, f32> keyPeriods_;
    core::u64 posesShared_ = 0;

public:
    // How many poses were copied from another mesh's this run (H10).
    [[nodiscard]] core::u64 posesShared() const noexcept { return posesShared_; }

private:
    // **The update rate** (H3). What the renderer last reported, by mesh: how
    // much of the picture's height it covered. Looked up, never iterated (R10).
    std::unordered_map<core::u64, f32> seen_;
    bool seeing_ = false;
    f32 detail_ = 1.0f;
    // Ticks sampled, which staggers the meshes that skip: a crowd's posing is
    // spread across the ticks rather than all on one of every eight.
    core::u64 sampled_ = 0;
    // The meshes posed every tick whatever is seen: an `AlwaysAnimate` player's,
    // a rig a `Bone` hangs from, one a ragdoll drove last tick.
    std::vector<core::InstanceId> always_;
    std::vector<core::InstanceId> overridden_;
    // The meshes that skipped a pose they were due, carried to the next tick so
    // the pose catches up whatever their tracks do meanwhile; and, by key,
    // those whose pose is behind -- built when a joint of one is asked for.
    std::vector<core::InstanceId> skipped_;
    std::unordered_map<core::u64, bool> stale_;

    // Builds `meshPart`'s pose now, if it is behind (H3).
    void catchUp(core::InstanceId meshPart);
    // The meshes a `Bone` turned this tick (G9).
    std::vector<core::InstanceId> turned_;
    std::vector<core::DVec3> translation_;
    std::vector<f32> rotation_;
    std::vector<core::Vec3> scale_;
    std::vector<f32> weightT_;
    std::vector<f32> weightR_;
    std::vector<f32> weightS_;
    std::vector<core::Mat4> model_;

    // A vector rather than a map, and sorted by instance: a scene has a handful
    // of ragdolls, and R10 forbids an unordered container's iteration order
    // reaching observable output -- which a pose most certainly is.
    std::vector<OverrideSet> overrides_;

    // Mutable because it is a memo: asking for a map is a read, and building one
    // the first time is what makes the second read free.
    mutable std::vector<JointMap> jointMaps_;
};

} // namespace engine::render
