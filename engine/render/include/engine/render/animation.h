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

#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/asset/animation_graph.h"
#include "engine/asset/model.h"
#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/render/graph_player.h"
#include "engine/render/retarget.h"
#include "engine/scene/animation_host.h"
#include "engine/scene/morph_host.h"
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
        // The mesh's morph targets (ADR 0196), in its file's order: what each
        // is called, and the weight the file gives it at rest. A mesh with
        // targets and no skeleton has an entry for these and its clips alone
        // -- so `joints` may be empty, and whoever wants a rig asks that.
        std::vector<std::string> morphNames;
        std::vector<f32> morphDefaults;
        // What a `.rig.json` beside the model says of its joints' roles (ADR
        // 0199): none for nearly every rig, whose names say it.
        std::vector<retarget::RoleOverride> roles;
    };

    void set(core::NameAtom content, Entry entry);
    // The rig is gone, to be read again: a model exported anew (D611).
    void forget(core::NameAtom content);
    void clear() noexcept;
    // Counted up on every change: what a pose remembered from an earlier
    // skeleton or clip is checked against (H10).
    [[nodiscard]] core::u64 revision() const noexcept { return revision_; }
    // Counted up when a rig that was there is replaced or taken away -- not
    // when a new one arrives: what a track found in a rig, by index, is good
    // until this moves.
    [[nodiscard]] core::u64 replaced() const noexcept { return replaced_; }

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
    core::u64 replaced_ = 0;
};

// The animation graphs that have loaded (ADR 0197), keyed by the content URN
// an `AnimationPlayer.Graph` names. Filled by whoever reads content -- the
// host, on the simulation's side, since a graph decides a pose the tick reads.
class GraphLibrary
{
public:
    // Where a parameter's value comes from when no script has set it.
    enum class Source : core::u8
    {
        None,
        Speed,
        VerticalSpeed,
        MoveX,
        MoveZ,
        Grounded,
        State,
        Attribute,
    };
    // A graph as it is played: the file, and its names resolved once -- which
    // file and which clip each use of a clip means, and what each parameter
    // reads.
    struct Entry
    {
        asset::AnimationGraph graph;
        // Parallel to `graph.clips`: the file the clip is in (none for the
        // mesh's own) and its name there.
        std::vector<core::NameAtom> clipFiles;
        std::vector<std::string> clipNames;
        // Parallel to `graph.parameters`.
        std::vector<Source> sources;
        std::vector<core::NameAtom> attributes;
    };
    // `atoms` interns the files and attributes the graph names.
    void set(core::NameAtom content, asset::AnimationGraph graph, core::AtomTable& atoms);
    // The graph is gone: a file that was deleted, or is about to be read again.
    void forget(core::NameAtom content);
    void clear() noexcept;
    // Counted up on every change: what was built from a graph is checked
    // against it.
    [[nodiscard]] core::u64 revision() const noexcept { return revision_; }
    [[nodiscard]] const Entry* find(core::NameAtom content) const noexcept;

private:
    // Sorted by atom, as `SkeletonLibrary` is and for its reason. Held by
    // pointer so a graph somebody is reading does not move when another
    // arrives.
    struct Slot
    {
        core::NameAtom content;
        std::unique_ptr<Entry> entry;
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
class AnimationSystem final : public scene::AnimationHost, public scene::SkeletonHost, public scene::MorphHost
{
public:
    // The world and the library are references the system keeps: it is created
    // by `app` after both exist and destroyed before either does, which is the
    // same lifetime `PhysicsSync` has.
    AnimationSystem(const scene::World& world, const SkeletonLibrary& skeletons);

    // Where graphs are found (ADR 0197). Without one, a player's `Graph` is
    // a name nothing answers to and it plays its tracks alone.
    void setGraphs(const GraphLibrary* graphs) noexcept { graphs_ = graphs; }

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

    // --- Animation graphs (ADR 0197) ----------------------------------------

    scene::GraphWrite setGraphParameter(core::InstanceId player, std::string_view name, f32 value) override;
    scene::GraphWrite clearGraphParameter(core::InstanceId player, std::string_view name) override;
    [[nodiscard]] scene::GraphParameterValue graphParameter(core::InstanceId player,
                                                            std::string_view name) const override;
    [[nodiscard]] std::string_view graphState(core::InstanceId player, std::string_view layer) const override;
    [[nodiscard]] std::span<const scene::GraphSignal> drainGraphSignals() override;
    void graphDigests(std::vector<std::pair<core::InstanceId, core::u64>>& into) const override;

    // **The files tracks are waiting for**: a clip named from a file no mesh
    // in the world wears -- a library of clips -- is loaded by nobody unless
    // somebody asks. In atom order, each once; whoever reads content loads
    // them.
    [[nodiscard]] std::span<const core::NameAtom> wantedClipFiles() const noexcept { return wantedFiles_; }

    void describeGraph(core::InstanceId player, std::vector<scene::GraphLayerView>& layers,
                       std::vector<scene::GraphParameterView>& parameters) const override;

    // --- scene::SkeletonHost ------------------------------------------------

    [[nodiscard]] core::u32 jointCount(core::InstanceId meshPart) const override;
    [[nodiscard]] core::i32 findJoint(core::InstanceId meshPart, std::string_view name) const override;
    [[nodiscard]] core::i32 jointParent(core::InstanceId meshPart, core::u32 joint) const override;
    [[nodiscard]] std::string_view jointName(core::InstanceId meshPart, core::u32 joint) const override;
    [[nodiscard]] std::string_view jointRole(core::InstanceId meshPart, core::u32 joint) const override;
    [[nodiscard]] bool jointModel(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const override;
    void setJointOverride(core::InstanceId meshPart, core::u32 joint, const core::CFrameD& model) override;
    void clearJointOverrides(core::InstanceId meshPart) override;
    void commitOverrides() override;

    // --- scene::MorphHost (ADR 0196) ----------------------------------------

    [[nodiscard]] core::u32 morphTargetCount(core::InstanceId meshPart) const override;
    [[nodiscard]] std::string_view morphTargetName(core::InstanceId meshPart, core::u32 target) const override;
    [[nodiscard]] f32 morphWeight(core::InstanceId meshPart, std::string_view name) const override;
    void setMorphWeight(core::InstanceId meshPart, std::string_view name, f32 weight) override;
    void clearMorphWeight(core::InstanceId meshPart, std::string_view name) override;

    // **The weights `meshPart` is drawn with now**, a target each in its
    // file's order: the file's own, moved by the clips playing on it, and a
    // script's over those. Empty when they are the file's own untouched --
    // nothing plays a weight on this mesh and no script set one -- which is
    // every body that is not a face in use, and costs it a lookup.
    //
    // Worked out when it is asked, from where the tracks are, and not kept in
    // the pose: a pose is built at the rate its mesh is seen, shared between
    // the bodies of a crowd and skipped for one nobody looks at, and a face's
    // weights are none of those things. The span is good until the next call.
    [[nodiscard]] std::span<const f32> drawnMorphWeights(core::InstanceId meshPart) const;

    // The pose of one `MeshPart`, or null for a mesh with no skeleton or nothing
    // driving it. Read by the renderer; null means "draw it in bind pose", which
    // is what an unanimated skinned mesh should look like.
    [[nodiscard]] const Pose* pose(core::InstanceId meshPart) const noexcept;

    // --- The pose a frame is DRAWN with (ADR 0194) --------------------------
    //
    // **A copy, and the only thing a cape may write.** Secondary motion is
    // stepped at the rate frames are drawn, differently on every machine and
    // not at all on a server. Written into the pose above it would be read
    // back by the simulation -- a `Bone` on a cape's joint is an attachment
    // the physics tick places -- and the world's state would then depend on
    // one machine's frame rate. So what a chain moves is a copy of the pose,
    // made here and kept until the next frame clears it, and only `extract`
    // asks for it. `jointModel`, the sockets and the ragdoll never see it.

    // One joint's place in a presented pose: joint space to MODEL space.
    struct PresentedJoint
    {
        core::u32 joint = 0;
        core::Mat4 model{};
    };
    // The pose `meshPart` is drawn with this frame: its own with `joints`
    // substituted and every joint below them carried along. `joints` in
    // ascending order of joint. One call a mesh a frame; a second replaces
    // the first.
    //
    // `over`: on top of what this frame already presented for the mesh, where
    // it did -- the spring chains after the limbs (ADR 0198), each moving its
    // own joints of one picture.
    void present(core::InstanceId meshPart, std::span<const PresentedJoint> joints, bool over = false);
    // Nothing is presented any more: every mesh is drawn with its own pose.
    void clearPresented() noexcept
    {
        presented_.clear();
        presentedMeshes_.clear();
    }
    // The meshes presented this frame, in the order they first were.
    [[nodiscard]] std::span<const core::InstanceId> presentedMeshes() const noexcept { return presentedMeshes_; }
    // A joint's place in the pose `meshPart` is DRAWN with: the presented one
    // where the frame has one, the simulated one otherwise. What hangs from a
    // joint in the picture -- a cape from a shoulder a limb's reach moved --
    // asks this.
    [[nodiscard]] bool drawnJointModel(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const;

    // --- For what solves a pose after the clips (ADR 0198) ------------------

    // The rig's joints, or none for a mesh with no skeleton.
    [[nodiscard]] std::span<const asset::Joint> jointsOf(core::InstanceId meshPart) const;
    // Every joint's place in model space as the simulation has it now -- the
    // pose, or the rest chain for a mesh nothing drives -- into `out`. False
    // for a mesh with no skeleton.
    [[nodiscard]] bool modelOf(core::InstanceId meshPart, std::vector<core::Mat4>& out) const;
    // A joint's place in the rest pose.
    [[nodiscard]] core::Mat4 restModel(core::InstanceId meshPart, core::u32 joint) const;
    // The roles of the rig's joints (ADR 0199), or null for no skeleton.
    [[nodiscard]] const retarget::RigRoles* rolesOf(core::InstanceId meshPart) const;
    // What `extract` draws: the presented pose where there is one, else
    // `pose`.
    [[nodiscard]] const Pose* drawnPose(core::InstanceId meshPart) const noexcept;
    // A joint's animated transform from its parent this tick, or its rest one
    // for a mesh nothing is driving. False for a joint the rig does not have.
    [[nodiscard]] bool jointLocal(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const;
    // Whether the renderer reached this mesh in the frame before (`reportSeen`),
    // or nobody is reporting at all -- a test, a tool -- and everything counts.
    [[nodiscard]] bool seenLately(core::InstanceId meshPart) const noexcept;

    // How many poses have been built since this system was made: what a test
    // counts to know that a pose nobody changed was not built again.
    [[nodiscard]] core::u64 posesBuilt() const noexcept { return posesBuilt_; }
    // How many tracks there are places for, living or not: what a test holds
    // to know that characters coming and going do not grow it for ever.
    [[nodiscard]] usize trackSlots() const noexcept { return tracks_.size(); }

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

        // --- A graph's track (ADR 0197) ----------------------------------
        //
        // A track a graph made is stepped by the graph, not by its own clock
        // and fade: `time`, `weight` and `playing` are written each tick.
        // `graph` is the instance that owns it, or `NoGraph` for a track a
        // script loaded.
        u32 graph = NoGraph;
        // **The layer it is mixed in.** 0 is where every track a script
        // plays is, and a graph's first layer: one weighted average, as it
        // always was. A layer above replaces what is under it on the joints
        // it has, by its weight; an additive one is applied after them all.
        core::u16 layer = 0;
        bool additive = false;
        // The layer's own weight this tick, and the graph its mask is read
        // from (by `layer`).
        f32 layerWeight = 1.0f;
        core::NameAtom graphContent;
        // `AnimationPlayer.Retargeting` (ADR 0199), as its player has it this
        // tick: 0 Automatic, 1 ByName.
        core::u8 retargeting = 0;
        f32 posedLayerWeight = 1.0f;
    };

    // Whether a track has nothing to change in a pose: stopped or holding,
    // and as it was when it was last posed.
    [[nodiscard]] static bool quiet(const Track& track) noexcept
    {
        return !track.playing && track.posed && !track.posedPlaying && track.posedHolding == track.holding &&
               track.posedWeight == track.weight && track.posedTime == track.time &&
               track.posedLayerWeight == track.layerWeight;
    }

    static constexpr u32 NoClip = 0xFFFFFFFFu;
    static constexpr u32 NoGraph = 0xFFFFFFFFu;

    // --- The walk's lanes (ADR 0197) ------------------------------------
    //
    // What one layer's tracks add up to, a joint a component: the weighted
    // sums, the weights, and how much of the joint the layer covers (its
    // tracks' weights times the layer's own).
    struct Lanes
    {
        std::vector<core::DVec3> translation;
        std::vector<f32> rotation;
        std::vector<core::Vec3> scale;
        std::vector<f32> weightT;
        std::vector<f32> weightR;
        std::vector<f32> weightS;
        std::vector<f32> coverT;
        std::vector<f32> coverR;
        std::vector<f32> coverS;
        void clear(usize joints, bool cover);
    };
    // One track's clip at its time into `lanes`, joint by joint through the
    // track's mask and the map onto this rig. False when it had nothing.
    bool accumulate(const Track& track, core::NameAtom rig, usize jointCount, bool quantise, Lanes& lanes);
    // An additive track: how far its clip is from its own first frame, onto
    // `addT_`, `addR_` and `addS_`.
    bool accumulateAdditive(const Track& track, core::NameAtom rig, usize jointCount, bool quantise);
    // `upper` over `base`: where the layer has a joint it takes its cover of
    // it, and what is under keeps the rest.
    void mergeLayer(const SkeletonLibrary::Entry& skeleton, Lanes& base, const Lanes& upper);
    // A track's mask on `rig`: a weight a joint, or null for the whole rig.
    [[nodiscard]] const std::vector<f32>* maskFor(const Track& track, core::NameAtom rig);
    // What of a track besides its clip, time and weight decides a pose: its
    // layer, whether it adds, its mask and the layer's weight.
    [[nodiscard]] core::u64 layerWordOf(const Track& track, core::NameAtom rig);
    struct Mask
    {
        core::NameAtom graph;
        core::NameAtom rig;
        core::u16 layer = 0;
        bool whole = true;
        std::vector<f32> weights;
    };
    std::vector<Mask> masks_;
    core::u64 masksRevision_ = ~core::u64{0};
    Lanes base_;
    Lanes upper_;
    std::vector<core::DVec3> addT_;
    std::vector<f32> addR_;
    std::vector<core::Vec3> addS_;

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
        // Whether roles were allowed (`Retargeting` Automatic): two maps a
        // pair of rigs, at the most.
        bool automatic = false;
        // `slots[i]` is the joint in `to` that joint `i` of `from` is, or -1.
        std::vector<core::i32> slots;
        // **By what each joint is** (ADR 0199), where both rigs are bodies
        // and are not one skeleton under two names: turns carried from rest
        // to rest, the hips' travel scaled, every other length the target's.
        // `carried.roles` false is by equal names, as it always was.
        retarget::Map carried;
        // Said once a pair: the roles a clip moves that the target lacks.
        mutable bool warned = false;
    };

    // The identity is returned as null: a clip applied to its own rig needs no
    // map, which is every character that is one mesh. `mode` is the track's
    // `Retargeting`.
    [[nodiscard]] const JointMap* jointMapFor(core::NameAtom from, core::NameAtom to, core::u8 mode) const;
    // A rig's roles, found once a rig.
    struct RolesOf
    {
        core::NameAtom content;
        retarget::RigRoles roles;
    };
    [[nodiscard]] const retarget::RigRoles* rolesFor(core::NameAtom content) const;
    mutable std::vector<RolesOf> rigRoles_;
    mutable core::u64 mapsRevision_ = ~core::u64{0};
    // Says which roles `clip` moves that `map`'s target has no joint for.
    void warnUnmapped(const JointMap& map, const asset::AnimationClip& clip) const;
    std::vector<core::u8> keyed_;

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
    const GraphLibrary* graphs_ = nullptr;

    // **One player's graph** (ADR 0197): the evaluator, the tracks it writes
    // each tick -- one a use of a clip -- and what it last read of the world.
    struct GraphInstance
    {
        core::InstanceId player;
        core::NameAtom content;
        // The library's revision it was bound at, and whether it is.
        core::u64 revision = 0;
        bool bound = false;
        bool alive = true;
        GraphPlayer evaluator;
        // Parallel to the graph's clips: indices into `tracks_`.
        std::vector<u32> tracks;
        std::vector<f32> lengths;
        // What a script set before the graph's file arrived, in order.
        struct Pending
        {
            std::string name;
            f32 value = 0.0f;
            bool clear = false;
        };
        std::vector<Pending> pending;
        // The `CharacterBody` its sources read, where it last was and how
        // fast it is taken to be going.
        core::InstanceId body;
        bool placed = false;
        core::DVec3 lastPosition{};
        core::Vec3 velocity{0.0f, 0.0f, 0.0f};
        // For a trigger with a source: what the source was when last looked
        // at, so a change can be told from a first sight.
        std::vector<core::u64> seen;
        std::vector<core::u8> sighted;
        // The stamp of the last tick that found its player with this graph.
        core::u64 stamp = 0;
    };
    std::vector<GraphInstance> graphInstances_;
    // Player to instance. Looked up, never walked (R10).
    std::unordered_map<core::u64, u32> graphIndex_;
    // Tracks of graphs that are gone, to be used again: no script holds one.
    std::vector<u32> freeTracks_;
    std::vector<GraphPlayer::Signal> graphScratch_;
    std::vector<scene::GraphSignal> graphSignals_;
    std::vector<scene::GraphSignal> graphSignalsDrained_;
    std::vector<core::NameAtom> wantedFiles_;
    std::vector<core::InstanceId> graphPlayers_;
    // `SkeletonLibrary::replaced` as the tracks last saw it (D611).
    core::u64 replacedSeen_ = 0;

    void stepGraphs(f64 fixedDt);
    [[nodiscard]] GraphInstance* graphOf(core::InstanceId player) noexcept;
    [[nodiscard]] const GraphInstance* graphOf(core::InstanceId player) const noexcept;
    GraphInstance& graphFor(core::InstanceId player);
    void bindGraph(GraphInstance& instance, const GraphLibrary::Entry& entry);
    void unbindGraph(GraphInstance& instance);
    void readSources(GraphInstance& instance, const GraphLibrary::Entry& entry, f64 fixedDt);
    [[nodiscard]] core::InstanceId bodyOf(core::InstanceId player) const;

    // **What scripts set, by name** (ADR 0196): a mesh's few overrides, in the
    // order they were first set. By name and not by index because a script
    // sets a weight on a mesh whose file may not have arrived, and keeps
    // whatever it set across a reload of that file.
    struct MorphOverride
    {
        std::string name;
        f32 weight = 0.0f;
    };
    // Ordered, though nothing walks it for output: `retire` sweeps it.
    std::map<core::u64, std::vector<MorphOverride>> morphOverrides_;
    // The entry whose targets `meshPart` has, or null.
    [[nodiscard]] const SkeletonLibrary::Entry* morphsOf(core::InstanceId meshPart) const;
    // What the clips make of each target of `meshPart` into `morphScratch_`;
    // false when no playing track has a weight channel for it.
    bool clipMorphWeights(core::InstanceId meshPart, const SkeletonLibrary::Entry& entry) const;
    mutable std::vector<f32> morphScratch_;
    mutable std::vector<f32> morphSum_;
    mutable std::vector<f32> morphTotal_;
    // **The tracks whose clip has a weight channel at all**, found once a
    // tick: nearly no clip has one, and a horde of bodies with targets and
    // tracks without would otherwise ask every track about every body.
    [[nodiscard]] std::span<const u32> weightTracks() const;
    mutable std::vector<u32> weightTracks_;
    mutable core::u64 weightTracksAt_ = ~core::u64{0};
    mutable usize weightTracksOf_ = 0;

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
    // The poses drawn in place of those, this frame (`present`).
    std::unordered_map<core::u64, Pose> presented_;
    std::vector<core::InstanceId> presentedMeshes_;
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
    // Deduplicate while collecting, before sorting in instance order. A mesh
    // can be named by a skipped pose, both tracks and their common drive root.
    struct MeshMark
    {
        core::u64 stamp = 0;
        core::u32 generation = 0;
    };
    std::vector<MeshMark> meshMarks_;
    core::u64 posesBuilt_ = 0;
    // **Which tracks drive which mesh this tick** (H10), as (mesh key, track)
    // sorted by instance index, generation and track: a pose asked every track in the world whether it drove its mesh
    // -- a thousand tracks for each of hundreds of meshes, each walking the
    // mesh's ancestors.
    std::vector<std::pair<core::u64, u32>> drivers_;
    // Group tracks by drive root within one sample only: hierarchy and loaded
    // skeleton changes are observed again on the next sample.
    std::vector<std::pair<core::u64, u32>> rootDrivers_;
    std::vector<core::InstanceId> descendants_;
    std::vector<core::InstanceId> driveMeshes_;
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
