// The seam between the Instance tree and skeletal animation.
//
// Exactly the shape `PhysicsSync` uses for `IPhysics3D*`, and for the same
// reason: the thing that can answer these questions is `render` (it owns the
// loaded clips and the skeleton), the thing that asks them is `script` (it owns
// the `AnimationTrack` userdata and its signal), and neither may include the
// other -- `script` is L5 and `render` is L4, but architecture.md §2 does not
// give `script` a `render` dependency and this is not the milestone to add one.
//
// So the interface lives here at L3, `render` implements it, `app` injects it,
// and `script` holds a pointer. Nothing in this header names a clip, a joint or
// a matrix: what crosses is a track id and six numbers.
//
// **Null is a real state and not an error.** A build with no render module has
// no animation host, and every call site checks -- exactly as it does for the
// physics mirror, which answers `nil` to a raycast in a world with no physics.
#pragma once

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/scene/types.h"

namespace engine::scene {

// A handle into the host's own table. 0 is "no track", which is what
// `LoadAnimation` returns for a player whose mesh has no such clip -- and the
// reason it returns a track at all rather than nil is that a mesh which has not
// finished loading would otherwise make a perfectly ordinary frame a crash.
using TrackId = u32;

// Everything a script can read off a track, in one read.
//
// One struct rather than six virtual getters: a property read crosses this seam
// and a virtual call per property would be six for what a HUD does every frame.
struct TrackState
{
    f64 timePosition = 0.0;
    f32 length = 0.0f;
    f32 speed = 1.0f;
    // **What the script set** (`AnimationTrack.Weight`), which a fade does not
    // touch: `Play` fades in TO it and `Stop` fades out FROM it, and after a
    // stop it is what it was -- so the track plays again at the weight it had,
    // where it came back at nothing (D438).
    f32 weight = 1.0f;
    // What the track is blended with this tick: `weight`, times where the
    // fade is. Zero for a track that is not playing.
    f32 blend = 0.0f;
    bool looped = false;
    bool playing = false;
};

// --- Animation graphs (ADR 0197) --------------------------------------------
//
// A player with a `Graph` mixes its clips itself; what a script does is set
// the graph's parameters and hear what it did. Names cross the seam, never an
// index: a script may set a parameter before the graph's file has arrived.

// What a parameter holds, for a script's read. `None` for no graph loaded or
// a name it does not declare.
struct GraphParameterValue
{
    enum class Kind : core::u8
    {
        None,
        Number,
        Boolean,
        Trigger,
    };
    Kind kind = Kind::None;
    f32 value = 0.0f;
};

// How a write to a parameter went.
enum class GraphWrite : core::u8
{
    // Set, or kept for a graph that has not loaded yet.
    Done,
    // The graph has loaded and declares no such parameter.
    Unknown,
    // No animation at all behind this player.
    Nothing,
};

// One thing a graph did in the tick just sampled. The views are the graph's
// own names and are good until the next `sample`.
struct GraphSignal
{
    core::InstanceId player;
    // An event a clip reached (`to` is its name, `from` is empty), or a
    // layer leaving one state for another.
    bool event = false;
    std::string_view layer;
    std::string_view from;
    std::string_view to;
};

// What a graph is doing, for a readout (an editor's panel): a layer's state
// and what it is fading out of, and a parameter's value and where it comes
// from. The views are the graph's own names, good until the next `sample`.
struct GraphLayerView
{
    std::string_view name;
    std::string_view state;
    f32 progress = 0.0f;
    f32 weight = 1.0f;
    // What it is still fading out of, oldest first, each with its weight.
    std::vector<std::pair<std::string_view, f32>> fading;
};

struct GraphParameterView
{
    std::string_view name;
    std::string_view from;
    GraphParameterValue value;
    bool overridden = false;
};

class AnimationHost
{
public:
    virtual ~AnimationHost() = default;

    // Empty lists for a player with no graph bound.
    virtual void describeGraph(core::InstanceId /*player*/, std::vector<GraphLayerView>& layers,
                               std::vector<GraphParameterView>& parameters) const
    {
        layers.clear();
        parameters.clear();
    }

    // Sets a parameter of `player`'s graph on this machine, over whatever the
    // graph reads from the world; a trigger is fired. `clearGraphParameter`
    // hands it back.
    virtual GraphWrite setGraphParameter(core::InstanceId /*player*/, std::string_view /*name*/, f32 /*value*/)
    {
        return GraphWrite::Nothing;
    }
    virtual GraphWrite clearGraphParameter(core::InstanceId /*player*/, std::string_view /*name*/)
    {
        return GraphWrite::Nothing;
    }
    [[nodiscard]] virtual GraphParameterValue graphParameter(core::InstanceId /*player*/,
                                                             std::string_view /*name*/) const
    {
        return {};
    }
    // The state a layer is in, by name; the first layer for an empty name.
    [[nodiscard]] virtual std::string_view graphState(core::InstanceId /*player*/, std::string_view /*layer*/) const
    {
        return {};
    }
    // What the graphs did in the last `sample`, in the order they did it;
    // handed over once.
    [[nodiscard]] virtual std::span<const GraphSignal> drainGraphSignals() { return {}; }
    // **A digest of each graph's state**, a player each, in the order the
    // graphs are stepped: what the host writes where the world's hash reads
    // it (R10). A graph that diverges between two runs shows at the tick it
    // diverges.
    virtual void graphDigests(std::vector<std::pair<core::InstanceId, core::u64>>& /*into*/) const {}

    // A track for one clip, by name. An empty name means the file's first clip.
    // Returns 0 when the player has no skeleton or the clip is not there; the
    // track still exists and still answers reads, with a length of zero.
    //
    // **`content` is the file the CLIP lives in, which need not be the file the
    // skeleton came from** (S6.8). Empty means the player's own mesh, which is
    // the common case and the only one that existed before. A different URN is a
    // clip authored somewhere else -- one walk cycle shared by every character
    // in a game -- and it is retargeted onto this rig by joint NAME.
    //
    // That retargeting is not new work: `AnimationSystem` has mapped joints by
    // name since one player had to drive a body and a shirt with different rigs.
    // What was missing was any way to say which file the clip was in.
    [[nodiscard]] virtual TrackId createTrack(core::InstanceId player, core::NameAtom content,
                                              std::string_view clip) = 0;

    virtual void play(TrackId track, f32 fadeTime, f32 weight, f32 speed) = 0;
    virtual void stop(TrackId track, f32 fadeTime) = 0;
    virtual void adjustWeight(TrackId track, f32 weight, f32 fadeTime) = 0;
    virtual void adjustSpeed(TrackId track, f32 speed) = 0;
    virtual void setLooped(TrackId track, bool looped) = 0;

    [[nodiscard]] virtual TrackState state(TrackId track) const = 0;

    // Advances every playing track by one tick and rebuilds each player's pose.
    // Called at `PreAnimation` (architecture.md §3, step 5b).
    virtual void sample(f64 fixedDt) = 0;

    // The tracks that reached the end of a non-looping clip since the last call.
    // Drained rather than pushed as a `Change`, because a `scene::Change` names
    // an Instance and a track is not one -- and inventing a change kind that
    // carried a track id would put animation's vocabulary in scene's queue.
    [[nodiscard]] virtual std::span<const TrackId> drainEnded() = 0;

    // Forgets every track belonging to instances that no longer exist. Called
    // once per frame: a track is a reference to a player and never a reason to
    // keep one alive.
    virtual void retire(const class World& world) = 0;
};

} // namespace engine::scene
