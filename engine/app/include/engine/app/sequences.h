#pragma once

// **Sequences being played** (ADR 0204): every `SequencePlayer` in a world,
// stepped once a tick, and asked once a frame what the picture is.
//
// What a sequence does divides in two, and so does this. **The tick** (`step`)
// advances each player's time and does what moves the world: writes keyed
// properties, begins clips and sets graph parameters, says lines, makes
// sounds, raises events. **The frame** (`camera`, `present`) reads where the
// time is between two ticks and answers with the view to draw through and the
// shape-key weights to draw with -- picture, each machine's own, and nothing a
// replay or a server knows.
//
// What a player is doing -- the clips it began, the sounds it made, the
// characters it is carrying -- is kept here and not in the world, as an
// animation track's state is: the world has the player's time and what was
// asked of it, and those are what is hashed and what travels.

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/sequence.h"
#include "engine/core/id.h"
#include "engine/core/kept_reads.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/scene/animation_host.h"
#include "engine/scene/sequence_files.h"
#include "engine/scene/value.h"

namespace engine::render {
class AnimationSystem;
}

namespace engine::scene {
class World;
class MorphHost;
struct SequencePlayerComponent;
} // namespace engine::scene

namespace engine::app {

class Sequences : public scene::SequenceFiles
{
public:
    // `scene::SequenceFiles`: a file's length, read and measured on the
    // first ask; its markers; and the hold at a moment of it.
    [[nodiscard]] core::f32 sequenceLength(const scene::World& world, core::NameAtom file) override;
    void sequenceMarkers(const scene::World& world, core::NameAtom file,
                         std::vector<scene::SequenceMarker>& out) override;
    [[nodiscard]] std::string sequenceHoldAt(const scene::World& world, core::NameAtom file,
                                             core::f64 seconds) override;
    [[nodiscard]] bool loadSequence(scene::World& world, core::InstanceId player, std::string_view json,
                                    std::string& what) override;
    // What this is given to work with. Set once by the host; any may be left
    // empty, and then that kind of track does nothing.
    struct Sources
    {
        // A content file's text by its URN, or none.
        std::function<std::optional<std::string>(std::string_view)> read;
        // How long a line takes to say: its slot, the longest of its languages.
        std::function<core::f64(std::string_view sound, std::string_view textKey, core::f32 seconds)> lineSeconds;
        // How long the same line is **as this machine hears it**: its
        // recording in the language the voices are in here. What a picture
        // key written `afterLine` is placed by. Empty answers the slot.
        std::function<core::f64(std::string_view sound, std::string_view textKey, core::f32 seconds)> lineHeard{};
        // A stamp's text by its path under `content/`, or none: what a cast
        // track makes its instance of.
        std::function<std::optional<std::string>(std::string_view)> stamp{};
        // Has the rigs and the clips a pose is about to need read now: a tick
        // does it for itself, and a scrub has no tick.
        std::function<void()> prepareRigs{};
    };
    void setSources(Sources sources) { m_sources = std::move(sources); }

    struct Tick
    {
        scene::World* world = nullptr;
        scene::AnimationHost* animation = nullptr;
        scene::MorphHost* morphs = nullptr;
        // Where a line or a sound said from nowhere is put.
        core::InstanceId workspace;
        // Whether this machine's world is the one that decides: a solo game, a
        // server. On a replica only a player this machine made itself is its
        // own to write the world from.
        bool authority = true;
        // The authority's tick now, as far as this machine can tell: its own
        // on an authority, and on a replica the estimate that is counted on
        // between snapshots -- what a replica's sequences run by.
        core::u64 serverTick = 0;
        core::f64 fixedDt = 1.0 / 60.0;
    };
    void step(const Tick& tick);

    // **The view a playing sequence takes this frame**, `ahead` seconds past
    // the last tick, or none: the most recently begun player with
    // `TakesCamera` and a camera track. `placeOf` says where an instance the
    // camera looks at is drawn this frame; empty asks the world.
    struct Camera
    {
        core::CFrameD cframe;
        // Degrees; nought is the game's own camera's.
        core::f32 fieldOfView = 0.0f;
        core::InstanceId player;
        // Where its sequence is, in seconds: what a function bound to the
        // camera is told.
        core::f64 time = 0.0;
    };
    using PlaceOf = std::function<core::DVec3(core::InstanceId)>;
    // Where an instance a camera key is relative to stands and faces this
    // frame; empty asks the world.
    using FrameOf = std::function<core::CFrameD(core::InstanceId)>;
    // **What stands between two places**: how far from `from`, in metres,
    // the first thing that does not move is met on the way to `to`, or none
    // -- `looked` itself, what it is part of and what is part of it, are not
    // in their own way. What a key with `keepInSight` asks, once a frame;
    // empty, the camera is left where its keys put it.
    using Sight = std::function<std::optional<core::f64>(core::DVec3 from, core::DVec3 to, core::InstanceId looked)>;
    [[nodiscard]] std::optional<Camera> camera(const scene::World& world, core::f64 ahead, const PlaceOf& placeOf = {},
                                               const FrameOf& frameOf = {}, const Sight& sight = {}) const;

    // **What a playing sequence puts over the picture this frame**: how much
    // of it a fade's colour covers, and how far in the bars of a wider
    // picture are. Picture, each machine's own, read between ticks as the
    // camera is. All nought with nothing of the kind playing.
    struct Screen
    {
        core::Color3 fadeColor{0.0f, 0.0f, 0.0f};
        core::f32 fade = 0.0f;
        core::f32 bars = 0.0f;
        // The shape of the picture the bars leave: its width over its height.
        core::f32 aspect = 2.39f;
    };
    [[nodiscard]] Screen screen(const scene::World& world, core::f64 ahead) const;

    // **The frame's shape keys**: every morph track of every playing sequence,
    // onto the layer a mouth writes. After the mouths, which clear it.
    void present(const scene::World& world, render::AnimationSystem& animation, core::f64 ahead);

    // Whether a sequence playing here asks for the local players' controls
    // to be held at rest.
    [[nodiscard]] bool suspendsControls(const scene::World& world) const noexcept;

    // --- The editor's scrub -------------------------------------------------
    //
    // Puts the world where `player`'s sequence is at `seconds`, with nothing
    // playing: keyed properties at their values, and `camera` and `present`
    // answer for that moment. **What it writes it remembers**, and
    // `endPreview` puts every one back: a preview never changes the scene.
    //
    // **And every bound character is posed** where a poser was given
    // (`setPoser`): the clip its animation track has it in at that moment, at
    // the frame the moment falls on and as far faded in as it would be; a
    // parameter track at its value, over a graph that is otherwise at rest --
    // nothing steps it, so a transition a parameter would begin does not run.
    // `endPreview` puts every body back as its file rests it.
    void preview(scene::World& world, core::InstanceId player, core::f64 seconds);
    void endPreview(scene::World& world);
    // What poses a scrub: the world's animation. Not owned; null poses nothing.
    void setPoser(scene::AnimationHost* poser) noexcept { m_poser = poser; }
    // False while a clip the scrub wants is still being read: the pose is
    // not there yet, and the same moment asked again will have it.
    [[nodiscard]] bool previewSettled() const noexcept { return !m_preview.unsettled; }
    [[nodiscard]] core::InstanceId previewed() const noexcept { return m_preview.player; }
    [[nodiscard]] core::f64 previewTime() const noexcept { return m_preview.seconds; }
    // Whether the scrubbed sequence's camera is the view (`camera` answers
    // for it) or only its properties and shape keys are shown.
    void setPreviewLooks(bool looks) noexcept { m_previewLooks = looks; }
    [[nodiscard]] bool previewLooks() const noexcept { return m_previewLooks; }

    // **A file as it is being edited**: `sequence` is what `urn` plays as from
    // now, whatever is on disk, until `forget` has it read again. What lets a
    // timeline show a key the moment it is set, before anything is saved.
    void put(const scene::World& world, core::NameAtom urn, asset::Sequence sequence);

    // A player's file as it was read, or null: what a timeline draws.
    [[nodiscard]] const asset::Sequence* sequenceOf(const scene::World& world, core::InstanceId player);
    // Whether the file a player names is there and is not a sequence: what an
    // editor must not open an empty one over.
    [[nodiscard]] bool unreadable(const scene::World& world, core::InstanceId player);
    // **Which reading of its file a player has**: a number that moves each
    // time the file is read again or given by `put`. What tells an editor
    // holding a copy that the file changed under it.
    [[nodiscard]] core::u32 revisionOf(const scene::World& world, core::InstanceId player);
    // How long a file is with its lines' slots.
    [[nodiscard]] core::f32 lengthOf(const scene::World& world, core::InstanceId player);
    // The slot of one line key, in seconds.
    [[nodiscard]] core::f32 lineSlot(const scene::World& world, std::string_view line) const;

    // The instance a track's target names under `player`, or none.
    [[nodiscard]] core::InstanceId resolve(const scene::World& world, core::InstanceId player,
                                           std::string_view target) const;

    // A file changed (hot reload): read again when next asked for.
    // What was read of a forgotten file is kept until a read of the new file
    // succeeds (ADR 0212, section 3): a read in the middle of a save puts it
    // back, quietly, and one no notice follows is said once at a step.
    void forget(std::span<const core::NameAtom> urns);
    void forgetAll();
    void setReloadPatience(double seconds) noexcept { m_kept.setPatience(seconds); }
    // The world is another world: nothing of the last one is kept.
    void clear();

    struct Stats
    {
        core::u32 playing = 0;
        // Tracks looked at by the last tick, and keys it reached.
        core::u32 tracks = 0;
        core::u32 keys = 0;
        // Properties written by the last tick.
        core::u32 written = 0;
        // Property tracks the last tick could write nothing for: what they
        // are bound to has no property of that name, or the key's value is
        // not that property's kind.
        core::u32 refused = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
    struct File
    {
        core::NameAtom urn;
        bool found = false;
        // There was a file, readable or not.
        bool there = false;
        core::u32 revision = 0;
        asset::Sequence sequence;
        // With its lines' slots.
        core::f32 length = 0.0f;
        // Given by a script (`LoadSequence`) and not read from content: kept
        // when files are read again, and never looked for on disk.
        bool made = false;
        // **A picture track as this machine reaches it**: one whose keys are
        // written `afterLine` has them here at the moments this machine's
        // voices put them, in order. A track with none is empty here, and is
        // read as the file has it.
        std::vector<asset::SequenceTrack> heard;
    };
    // Somebody a cast track brought: the instance made of its stamp, and the
    // binding it is known to the other tracks by.
    struct Cast
    {
        core::u32 track = 0;
        core::NameAtom name;
        core::InstanceId root;
        core::InstanceId binding;
        bool shown = false;
    };
    struct Clip
    {
        core::u32 track = 0;
        core::u32 key = 0;
        scene::TrackId id = 0;
        core::InstanceId animator;
        core::f32 fade = 0.2f;
        bool hold = false;
        // Begun and not yet told to go.
        bool on = false;
    };
    struct Carried
    {
        core::InstanceId body;
        core::u32 track = 0;
        bool wasAnchored = false;
    };
    struct State
    {
        core::InstanceId player;
        core::NameAtom file;
        // Between a beginning and an end: its camera is taken and what it
        // began is its to end.
        bool begun = false;
        bool seen = false;
        bool wasPlaying = false;
        // Running, or held by its file: what its clips run by.
        bool wasRunning = false;
        bool wasHeld = false;
        bool warned = false;
        // Said once a player, as the one above is: a track whose property
        // cannot be written.
        bool warnedProperty = false;
        core::f32 speed = 1.0f;
        // Which began last: the camera's, where two play.
        core::u64 order = 0;
        // A track each: the first key not yet reached.
        std::vector<core::u32> cursor;
        std::vector<Clip> clips;
        std::vector<core::InstanceId> sounds;
        std::vector<Carried> carried;
        std::vector<Cast> cast;
        // Said once a player: a cast track whose stamp is not there.
        bool warnedCast = false;
    };
    struct Kept
    {
        core::InstanceId instance;
        core::NameAtom property;
        scene::Value value;
    };
    struct SetParameter
    {
        core::InstanceId animator;
        std::string name;
    };
    struct Preview
    {
        core::InstanceId player;
        core::f64 seconds = 0.0;
        std::vector<Kept> kept;
        // The clips it has posed with and the parameters it set, to let go of.
        std::vector<Clip> clips;
        std::vector<SetParameter> parameters;
        // Who the scrubbed sequence's cast tracks have brought, for as long
        // as the scrub lasts: in the scene and not of it.
        std::vector<Cast> cast;
        bool posed = false;
        bool unsettled = false;
    };
    enum class End : core::u8
    {
        Finished,
        Stopped,
        Skipped,
    };

    [[nodiscard]] File* fileFor(const scene::World& world, core::NameAtom urn);
    void measure(const scene::World& world, File& file) const;
    // A track of `file` as its picture is read: `heard` where it has one.
    [[nodiscard]] static const asset::SequenceTrack& pictured(const File& file, core::usize index) noexcept;
    // Every file measured again where the language the voices are in is not
    // the one they were measured in.
    void hearAgain(const scene::World& world);
    // **A cast track at a moment**: whoever it brings is there, or is not.
    // Made of its stamp the first time it is to be there. `put` is where it
    // is put when it is there; `local` marks what is made as this machine's.
    void bring(scene::World& world, core::InstanceId player, const asset::SequenceTrack& track, core::u32 index,
               std::vector<Cast>& cast, bool show, core::InstanceId put, bool scrub, bool* warned);
    void dismiss(scene::World& world, std::vector<Cast>& cast, bool hiddenOnly);
    [[nodiscard]] const File* fileOf(core::NameAtom urn) const noexcept;
    [[nodiscard]] State& stateFor(core::InstanceId player, bool& fresh);
    [[nodiscard]] const State* stateOf(core::InstanceId player) const noexcept;

    void begin(const Tick& tick, State& state, const File& file);
    void end(const Tick& tick, State& state, const File* file, scene::SequencePlayerComponent& player, bool owner,
             End how);
    // Every track from where its cursor is to `to`; `skipped` marks the events.
    void advance(const Tick& tick, State& state, const File& file, core::f64 to, bool owner, bool skipped);
    void seek(const Tick& tick, State& state, const File& file, core::f64 to, bool owner);
    void writeProperty(scene::World& world, core::InstanceId player, State* state, const asset::SequenceTrack& track,
                       core::u32 index, core::InstanceId target, core::f64 at, bool owner, Preview* preview);
    void startClip(const Tick& tick, State& state, const asset::SequenceTrack& track, core::u32 index, core::u32 key,
                   core::f64 into);
    void sayLine(const Tick& tick, State& state, const asset::SequenceKey& key, core::f64 into);
    void showCaption(const Tick& tick, State& state, const asset::SequenceKey& key);
    void playSound(const Tick& tick, State& state, const asset::SequenceKey& key, core::f64 into);
    void setDown(scene::World& world, const Carried& carried);
    void endSounds(scene::World& world, State& state);
    void anchor(const Tick& tick, scene::SequencePlayerComponent& player) const;

    void posePreview(scene::World& world, const File& file);

    Sources m_sources;
    scene::AnimationHost* m_poser = nullptr;
    std::vector<File> m_files;
    core::KeptReads<core::u32, asset::Sequence> m_kept;
    std::vector<State> m_states;
    Preview m_preview;
    bool m_previewLooks = true;
    core::u32 m_revisions = 0;
    core::u32 m_made = 0;
    core::u64 m_order = 0;
    // The language `heard` was worked out in.
    std::string m_voice;
    // Where what a tick makes is put: the last tick's, which a scrub has not.
    core::InstanceId m_workspace;
    Stats m_stats;
    // The frame's scratch.
    std::vector<std::pair<std::string, core::f32>> m_weights;
};

} // namespace engine::app
