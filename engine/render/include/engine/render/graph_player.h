// **What steps an animation graph** (ADR 0197): one of these a character, moved
// once a simulation tick. It makes no pose and knows no skeleton. What it
// answers is, for every use of a clip in the graph, whether that clip is
// playing, at what time and with what weight -- and the pose walk that already
// mixes tracks mixes those.
//
// **Simulation, not picture** (R10). It reads no clock but the `dt` it is
// handed, keeps nothing in a container whose order is not defined, tries a
// layer's transitions in the file's order and reports events in layer order;
// two players given the same graph and the same calls answer the same, and
// `hashInto` is how a run that diverged is caught at the tick it diverged.
//
// **The graph is not kept.** Every call that needs it is handed it, so a
// player is plain state that can be copied, and a graph reloaded from its file
// is a `reset` away. A `step` with a graph the state does not fit resets
// first rather than read past an end.
//
// A weight here is a state's weight in its own layer's cross-fade times a
// clip's share of its state's blend. The layer's own weight and mask are not
// in it: the caller applies those (`layerWeight`).
#pragma once

#include <array>
#include <span>
#include <vector>

#include "engine/asset/animation_graph.h"
#include "engine/core/types.h"

namespace engine::render {

class GraphPlayer
{
public:
    // How many states a layer may be fading out of at once. A fifth pushes out
    // whichever weighs least.
    static constexpr core::usize MaxFading = 4;

    // One use of a clip, parallel to `AnimationGraph::clips`.
    struct ClipState
    {
        // Seconds into the clip. Nought while its length is not known.
        core::f64 time = 0.0;
        core::f32 weight = 0.0f;
        // Its state is the one its layer is in, or one still fading out. A
        // clip of a blend the parameters leave out is active at weight nought.
        bool active = false;
    };

    // One clip of one state the layer is in or fading from, this step. Where
    // `ClipState` has one entry a use, this has one for each time the use is
    // playing: a state entered again while it is still fading out is two.
    struct Track
    {
        // Into `AnimationGraph::clips`.
        core::u32 clip = 0;
        core::f64 time = 0.0;
        core::f32 weight = 0.0f;
    };

    struct Signal
    {
        enum class Kind : core::u8
        {
            // A transition was taken: `from` and `to` are states of `layer`.
            StateChanged,
            // A clip passed one of its events: `clip` is into
            // `AnimationGraph::clips`, `event` into that clip's `events`, and
            // `from` and `to` are both the state the layer is in.
            Event,
        };
        Kind kind = Kind::StateChanged;
        core::u32 layer = 0;
        core::u32 from = 0;
        core::u32 to = 0;
        core::u32 clip = 0;
        core::u32 event = 0;
    };

    // A state a layer is still fading out of.
    struct Fading
    {
        core::u32 state = 0;
        core::f32 weight = 0.0f;
    };

    // Every layer in its start state at time nought, every parameter at rest
    // with nothing set and nothing fired. No clip is active until a `step`.
    void reset(const asset::AnimationGraph& graph);

    // A script's value for a parameter: it wins over the source until cleared.
    // A value that is not finite is taken as nought.
    void setOverride(core::u32 parameter, core::f32 to);
    void clearOverride(core::u32 parameter);
    [[nodiscard]] bool overridden(core::u32 parameter) const;
    // What the world says the parameter is; kept until it is set again.
    void setSource(core::u32 parameter, core::f32 to);
    // A trigger: true until a transition takes it or the next `step` ends.
    // Nothing for a parameter that is not a trigger.
    void fire(core::u32 parameter);
    // The override, else the source, else the value at rest. A trigger's is 1
    // while it is fired and 0 otherwise -- it is fired, never set.
    [[nodiscard]] core::f32 value(core::u32 parameter) const;

    // One tick. `clipLengths[i]` is how long `graph.clips[i]` is, in seconds,
    // or nought while its file has not loaded: such a clip stays at time
    // nought, weighs what it would, and never satisfies an `after`.
    //
    // In order: each layer's transitions are tried, in layer order, against
    // the state as the last step left it; fades advance; time advances; blends
    // are weighed by the parameters as they are now; the events the step
    // crossed are found; every trigger is cleared.
    //
    // `signals` is appended to, never cleared: every `StateChanged` of the
    // step in layer order, then every `Event` in layer order and, within a
    // layer, in the order the clip crossed them.
    void step(const asset::AnimationGraph& graph, core::f64 dt, std::span<const core::f32> clipLengths,
              std::vector<Signal>& signals);

    // Parallel to `graph.clips`. Where two instances of one state are alive
    // at once, an entry carries both weights and the newer one's time;
    // `tracks` keeps them apart.
    [[nodiscard]] std::span<const ClipState> clips() const { return m_clips; }
    // What the last step played, with no weight of nought: layer by layer, a
    // layer's fading states oldest first and then the one it is in, a state's
    // clips in the graph's order.
    [[nodiscard]] std::span<const Track> tracks() const { return m_tracks; }

    // The layer's number, or its parameter's value, held to 0..1.
    [[nodiscard]] core::f32 layerWeight(const asset::AnimationGraph& graph, core::u32 layer) const;
    // The state the layer is in: the newest, whatever it is still fading from.
    [[nodiscard]] core::u32 state(core::u32 layer) const;
    // 0..1 through that state, as the last step left it: a clip's time over
    // its length, a synced blend's phase, an unsynced blend's heaviest clip.
    // Nought for a state with no clip and while no length is known.
    [[nodiscard]] core::f32 stateProgress(core::u32 layer) const;
    // How much of the layer the state it is in holds: 1 unless it is fading in.
    [[nodiscard]] core::f32 stateWeight(core::u32 layer) const;
    // What the layer is still fading out of, oldest first.
    [[nodiscard]] std::span<const Fading> fading(core::u32 layer) const;

    // Everything that decides what a later step answers, in a fixed order:
    // each layer's state, time, weight, fade and what it is fading from, and
    // each parameter's override, source and whether it is fired.
    void hashInto(std::vector<core::u64>& words) const;

private:
    struct Parameter
    {
        core::f32 rest = 0.0f;
        core::f32 overrideValue = 0.0f;
        core::f32 source = 0.0f;
        bool trigger = false;
        bool overridden = false;
        bool sourced = false;
        bool fired = false;
    };

    struct Layer
    {
        core::u32 state = 0;
        // Seconds played, or the phase of a synced blend.
        core::f64 time = 0.0;
        core::f32 weight = 1.0f;
        // The fade in under way: how long it is and how far along. A length of
        // nought is no fade.
        core::f32 fadeLength = 0.0f;
        core::f64 fadeElapsed = 0.0;
        // As the last step left it, for the transitions the next one tries.
        core::f64 progress = 0.0;
        bool progressKnown = false;
        // No step has yet found this state's events: the next one that can
        // counts an event at nought as crossed.
        bool entered = true;
        core::u32 fadingCount = 0;
        std::array<Fading, MaxFading> fading{};
        std::array<core::f64, MaxFading> fadingTime{};
        // Each one's part of whatever the state fading in has not yet taken,
        // as it was when the transition was taken. They add to one.
        std::array<core::f32, MaxFading> fadingShare{};
    };

    // What playing a state for a step came to, for its events and progress.
    struct Played
    {
        // The clip that weighs most, into `AnimationGraph::clips`.
        core::u32 clip = 0;
        core::f64 previous = 0.0;
        core::f64 now = 0.0;
        bool wrapped = false;
        // Whether there is a length to be any way through.
        bool known = false;
    };

    [[nodiscard]] bool fits(const asset::AnimationGraph& graph) const;
    [[nodiscard]] bool holds(const asset::GraphCondition& condition) const;
    void tryTransitions(const asset::AnimationGraph& graph, core::u32 index, std::vector<Signal>& signals);
    void enter(Layer& layer, core::u32 to, core::f32 fade);
    void weigh(const asset::AnimationGraph& graph, const asset::GraphState& blend, std::span<core::f32> shares) const;
    Played play(const asset::AnimationGraph& graph, const asset::GraphState& playing, core::f64& time, core::f32 weight,
                core::f64 dt, std::span<const core::f32> clipLengths);

    std::vector<Parameter> m_parameters;
    std::vector<Layer> m_layers;
    std::vector<ClipState> m_clips;
    std::vector<Track> m_tracks;
    // A state's clips' shares of its blend, while it is being played.
    std::vector<core::f32> m_shares;
};

} // namespace engine::render
