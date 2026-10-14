#include "engine/render/graph_player.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::render {

namespace {

using asset::AnimationGraph;
using asset::GraphClip;
using asset::GraphCondition;
using asset::GraphLayer;
using asset::GraphOp;
using asset::GraphState;
using asset::GraphStateKind;
using asset::GraphTransition;
using core::f32;
using core::f64;
using core::u32;
using core::u64;
using core::usize;

// A fade is over when it is this close to over. Ticks are added up in f64 and
// a fade is written in f32, so twelve sixtieths can fall a hair short of the
// 0.2 a file says -- and a state left at a millionth of its weight for one
// more tick is a track mixed for nothing.
constexpr f64 FadeSlack = 1e-5;

// How far outside a triangle a point may be and still be in it: on an edge,
// the arithmetic puts it a hair to either side.
constexpr f32 InsideSlack = 1e-5f;

[[nodiscard]] u64 bits(f64 value) noexcept
{
    u64 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

[[nodiscard]] u64 bits(f32 value) noexcept
{
    u32 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

struct Point
{
    f32 x = 0.0f;
    f32 y = 0.0f;
};

[[nodiscard]] Point pointOf(const GraphClip& clip) noexcept
{
    return Point{clip.at[0], clip.at[1]};
}

[[nodiscard]] f32 cross(Point a, Point b, Point c) noexcept
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// How far along `a` to `b` the point of it nearest `p` is, 0..1, and how far
// from `p` that point is, squared. Two clips at one point are a segment with
// no length: all of it is `a`.
struct Nearest
{
    f32 along = 0.0f;
    f32 distance2 = 0.0f;
};

[[nodiscard]] Nearest nearestOn(Point a, Point b, Point p) noexcept
{
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    const f32 length2 = dx * dx + dy * dy;
    Nearest out;
    if (length2 > 0.0f)
        out.along = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0f, 1.0f);
    const f32 ox = a.x + dx * out.along - p.x;
    const f32 oy = a.y + dy * out.along - p.y;
    out.distance2 = ox * ox + oy * oy;
    return out;
}

// A time over a length, as whole loops and the part of one. Both come from
// the one division, so whether a loop wrapped and where it now is can never
// disagree -- which is what would fire a loop's events twice.
struct Turns
{
    f64 whole = 0.0;
    f64 part = 0.0;
};

[[nodiscard]] Turns turnsOf(f64 time, f64 length) noexcept
{
    const f64 turns = time / length;
    const f64 whole = std::floor(turns);
    return Turns{whole, turns - whole};
}

} // namespace

void GraphPlayer::reset(const AnimationGraph& graph)
{
    m_parameters.assign(graph.parameters.size(), Parameter{});
    for (usize index = 0; index < graph.parameters.size(); ++index) {
        m_parameters[index].rest = graph.parameters[index].rest;
        m_parameters[index].trigger = graph.parameters[index].kind == asset::GraphParameterKind::Trigger;
    }

    m_layers.assign(graph.layers.size(), Layer{});
    usize most = 0;
    for (usize index = 0; index < graph.layers.size(); ++index) {
        const GraphLayer& layer = graph.layers[index];
        m_layers[index].state = layer.start < layer.states.size() ? layer.start : 0;
        for (const GraphState& each : layer.states)
            most = std::max<usize>(most, each.clipCount);
    }

    m_clips.assign(graph.clips.size(), ClipState{});
    m_tracks.clear();
    m_shares.assign(most, 0.0f);
}

bool GraphPlayer::fits(const AnimationGraph& graph) const
{
    if (m_parameters.size() != graph.parameters.size() || m_layers.size() != graph.layers.size() ||
        m_clips.size() != graph.clips.size())
        return false;
    for (usize index = 0; index < m_layers.size(); ++index) {
        const Layer& layer = m_layers[index];
        const GraphLayer& described = graph.layers[index];
        if (layer.state >= described.states.size())
            return false;
        for (u32 at = 0; at < layer.fadingCount; ++at) {
            if (layer.fading[at].state >= described.states.size())
                return false;
        }
        for (const GraphState& each : described.states) {
            if (each.clipCount > m_shares.size() || each.firstClip + each.clipCount > graph.clips.size())
                return false;
        }
    }
    return true;
}

void GraphPlayer::setOverride(u32 parameter, f32 to)
{
    if (parameter >= m_parameters.size())
        return;
    m_parameters[parameter].overrideValue = std::isfinite(to) ? to : 0.0f;
    m_parameters[parameter].overridden = true;
}

void GraphPlayer::clearOverride(u32 parameter)
{
    if (parameter >= m_parameters.size())
        return;
    // The value goes too, so a player that set and cleared hashes as one that
    // never set.
    m_parameters[parameter].overrideValue = 0.0f;
    m_parameters[parameter].overridden = false;
}

bool GraphPlayer::overridden(u32 parameter) const
{
    return parameter < m_parameters.size() && m_parameters[parameter].overridden;
}

void GraphPlayer::setSource(u32 parameter, f32 to)
{
    if (parameter >= m_parameters.size())
        return;
    m_parameters[parameter].source = std::isfinite(to) ? to : 0.0f;
    m_parameters[parameter].sourced = true;
}

void GraphPlayer::fire(u32 parameter)
{
    if (parameter < m_parameters.size() && m_parameters[parameter].trigger)
        m_parameters[parameter].fired = true;
}

f32 GraphPlayer::value(u32 parameter) const
{
    if (parameter >= m_parameters.size())
        return 0.0f;
    const Parameter& found = m_parameters[parameter];
    if (found.trigger)
        return found.fired ? 1.0f : 0.0f;
    return found.overridden ? found.overrideValue : found.sourced ? found.source : found.rest;
}

f32 GraphPlayer::layerWeight(const AnimationGraph& graph, u32 layer) const
{
    if (layer >= graph.layers.size())
        return 0.0f;
    const GraphLayer& described = graph.layers[layer];
    const f32 weight =
        described.weightParameter >= 0 ? value(static_cast<u32>(described.weightParameter)) : described.weight;
    return std::clamp(weight, 0.0f, 1.0f);
}

u32 GraphPlayer::state(u32 layer) const
{
    return layer < m_layers.size() ? m_layers[layer].state : 0;
}

f32 GraphPlayer::stateProgress(u32 layer) const
{
    return layer < m_layers.size() ? static_cast<f32>(m_layers[layer].progress) : 0.0f;
}

f32 GraphPlayer::stateWeight(u32 layer) const
{
    return layer < m_layers.size() ? m_layers[layer].weight : 0.0f;
}

std::span<const GraphPlayer::Fading> GraphPlayer::fading(u32 layer) const
{
    if (layer >= m_layers.size())
        return {};
    return std::span<const Fading>(m_layers[layer].fading.data(), m_layers[layer].fadingCount);
}

bool GraphPlayer::holds(const GraphCondition& condition) const
{
    const f32 have = value(condition.parameter);
    switch (condition.op) {
    case GraphOp::IsSet:
        return have != 0.0f;
    case GraphOp::Equal:
        return have == condition.value;
    case GraphOp::NotEqual:
        return have != condition.value;
    case GraphOp::Less:
        return have < condition.value;
    case GraphOp::LessEqual:
        return have <= condition.value;
    case GraphOp::Greater:
        return have > condition.value;
    case GraphOp::GreaterEqual:
        return have >= condition.value;
    }
    return false;
}

void GraphPlayer::enter(Layer& layer, u32 to, f32 fade)
{
    if (fade > 0.0f) {
        // What the layer was in joins what it is fading from, with the weight
        // it had reached -- so a fade left half way goes on fading out beside
        // the rest instead of vanishing.
        if (layer.weight > 0.0f) {
            if (layer.fadingCount == MaxFading) {
                u32 lightest = 0;
                for (u32 at = 1; at < layer.fadingCount; ++at) {
                    if (layer.fading[at].weight < layer.fading[lightest].weight)
                        lightest = at;
                }
                // The one leaving is the lightest only if nothing fading is
                // lighter still: then it is the one that goes.
                if (layer.fading[lightest].weight <= layer.weight) {
                    for (u32 at = lightest; at + 1 < layer.fadingCount; ++at) {
                        layer.fading[at] = layer.fading[at + 1];
                        layer.fadingTime[at] = layer.fadingTime[at + 1];
                    }
                    --layer.fadingCount;
                }
            }
            if (layer.fadingCount < MaxFading) {
                layer.fading[layer.fadingCount] = Fading{layer.state, layer.weight};
                layer.fadingTime[layer.fadingCount] = layer.time;
                ++layer.fadingCount;
            }
        }
        f32 total = 0.0f;
        for (u32 at = 0; at < layer.fadingCount; ++at)
            total += layer.fading[at].weight;
        if (total > 0.0f) {
            // The newcomer starts at nothing, so what is fading holds the
            // whole layer between it, each in the proportion it had.
            for (u32 at = 0; at < layer.fadingCount; ++at) {
                layer.fadingShare[at] = layer.fading[at].weight / total;
                layer.fading[at].weight = layer.fadingShare[at];
            }
            layer.weight = 0.0f;
            layer.fadeLength = fade;
        }
        else {
            layer.fadingCount = 0;
        }
    }
    else {
        layer.fadingCount = 0;
    }
    if (layer.fadingCount == 0) {
        layer.weight = 1.0f;
        layer.fadeLength = 0.0f;
    }
    layer.state = to;
    layer.time = 0.0;
    layer.fadeElapsed = 0.0;
    layer.progress = 0.0;
    layer.progressKnown = false;
    layer.entered = true;
}

void GraphPlayer::tryTransitions(const AnimationGraph& graph, u32 index, std::vector<Signal>& signals)
{
    Layer& layer = m_layers[index];
    for (const GraphTransition& transition : graph.layers[index].transitions) {
        // `*` is any state but the one it leads to: a layer already there
        // does not start it again every tick the condition holds.
        if (transition.from >= 0 ? static_cast<u32>(transition.from) != layer.state : transition.to == layer.state)
            continue;
        if (transition.after >= 0.0f && !(layer.progressKnown && static_cast<f64>(transition.after) <= layer.progress))
            continue;
        if (!std::all_of(transition.when.begin(), transition.when.end(),
                         [this](const GraphCondition& condition) { return holds(condition); }))
            continue;

        // One firing drives one transition: a layer above that waits on the
        // same trigger does not see it.
        for (const GraphCondition& condition : transition.when) {
            if (condition.parameter < m_parameters.size())
                m_parameters[condition.parameter].fired = false;
        }
        signals.push_back(Signal{Signal::Kind::StateChanged, index, layer.state, transition.to, 0, 0});
        enter(layer, transition.to, transition.fade);
        return;
    }
}

void GraphPlayer::weigh(const AnimationGraph& graph, const GraphState& blend, std::span<f32> shares) const
{
    std::fill(shares.begin(), shares.end(), 0.0f);
    const std::span<const GraphClip> uses(graph.clips.data() + blend.firstClip, blend.clipCount);
    const usize count = uses.size();
    if (count == 1 || (blend.kind != GraphStateKind::Blend1 && blend.kind != GraphStateKind::Blend2)) {
        shares[0] = 1.0f;
        return;
    }

    if (blend.kind == GraphStateKind::Blend1) {
        const f32 at = value(static_cast<u32>(std::max(blend.blendX, 0)));
        if (at <= uses.front().at[0]) {
            shares[0] = 1.0f;
            return;
        }
        if (at >= uses.back().at[0]) {
            shares[count - 1] = 1.0f;
            return;
        }
        // The first clip past the parameter, and the one before it. The one
        // past is strictly past, so two clips written at one value are never
        // divided by the nothing between them.
        for (usize index = 0; index + 1 < count; ++index) {
            const f32 low = uses[index].at[0];
            const f32 high = uses[index + 1].at[0];
            if (high > at) {
                const f32 along = (at - low) / (high - low);
                shares[index] = 1.0f - along;
                shares[index + 1] = along;
                return;
            }
        }
        shares[count - 1] = 1.0f;
        return;
    }

    const Point at{value(static_cast<u32>(std::max(blend.blendX, 0))),
                   value(static_cast<u32>(std::max(blend.blendY, 0)))};
    for (const std::array<u32, 3>& triangle : blend.triangles) {
        if (triangle[0] >= count || triangle[1] >= count || triangle[2] >= count)
            continue;
        const Point a = pointOf(uses[triangle[0]]);
        const Point b = pointOf(uses[triangle[1]]);
        const Point c = pointOf(uses[triangle[2]]);
        const f32 area = cross(a, b, c);
        if (area == 0.0f)
            continue;
        f32 second = cross(a, at, c) / area;
        f32 third = cross(a, b, at) / area;
        f32 first = 1.0f - second - third;
        if (first < -InsideSlack || second < -InsideSlack || third < -InsideSlack)
            continue;
        first = std::max(first, 0.0f);
        second = std::max(second, 0.0f);
        third = std::max(third, 0.0f);
        const f32 total = first + second + third;
        shares[triangle[0]] = first / total;
        shares[triangle[1]] = second / total;
        shares[triangle[2]] = third / total;
        return;
    }

    // Outside every triangle: the nearest point of any edge, which is two
    // clips. With no triangle at all -- two clips, or all on one line -- the
    // clips in the file's order are the line to be near.
    usize from = 0;
    usize to = 0;
    Nearest best;
    best.distance2 = std::numeric_limits<f32>::max();
    const auto consider = [&](usize a, usize b) {
        const Nearest found = nearestOn(pointOf(uses[a]), pointOf(uses[b]), at);
        if (found.distance2 < best.distance2) {
            best = found;
            from = a;
            to = b;
        }
    };
    if (blend.triangles.empty()) {
        for (usize index = 0; index + 1 < count; ++index)
            consider(index, index + 1);
    }
    else {
        for (const std::array<u32, 3>& triangle : blend.triangles) {
            if (triangle[0] >= count || triangle[1] >= count || triangle[2] >= count)
                continue;
            consider(triangle[0], triangle[1]);
            consider(triangle[1], triangle[2]);
            consider(triangle[2], triangle[0]);
        }
    }
    shares[from] = 1.0f - best.along;
    shares[to] += best.along;
}

GraphPlayer::Played GraphPlayer::play(const AnimationGraph& graph, const GraphState& playing, f64& time, f32 weight,
                                      f64 dt, std::span<const f32> clipLengths)
{
    Played out;
    const u32 count = playing.clipCount;
    if (playing.kind == GraphStateKind::None || count == 0)
        return out;

    const std::span<f32> shares(m_shares.data(), count);
    weigh(graph, playing, shares);
    u32 heaviest = 0;
    for (u32 index = 1; index < count; ++index) {
        if (shares[index] > shares[heaviest])
            heaviest = index;
    }
    out.clip = playing.firstClip + heaviest;

    // A clip whose file has not loaded has no length yet, and one that came
    // out of a bad file may have any: neither is played.
    const auto lengthOf = [&](u32 index) -> f64 {
        const usize clip = playing.firstClip + index;
        if (clip >= clipLengths.size() || !(clipLengths[clip] > 0.0f) || !std::isfinite(clipLengths[clip]))
            return 0.0;
        return static_cast<f64>(clipLengths[clip]);
    };
    const auto emit = [&](u32 index, f64 at) {
        const u32 clip = playing.firstClip + index;
        const f32 share = weight * shares[index];
        // Fading states are played before the one the layer is in, so where
        // one state is alive twice the newer time is the one left here.
        ClipState& answer = m_clips[clip];
        answer.active = true;
        answer.time = at;
        answer.weight += share;
        if (share > 0.0f)
            m_tracks.push_back(Track{clip, at, share});
    };
    const f64 advance = dt * static_cast<f64>(playing.speed);
    const f64 before = time;

    if (playing.sync && count > 1) {
        // One phase for all of them, moving at the rate of a clip as long as
        // their mix: half a walk and half a run step at the pace between.
        f64 mixed = 0.0;
        f64 mixedWeight = 0.0;
        for (u32 index = 0; index < count; ++index) {
            const f64 length = lengthOf(index);
            if (length > 0.0) {
                mixed += static_cast<f64>(shares[index]) * length;
                mixedWeight += static_cast<f64>(shares[index]);
            }
        }
        const f64 length = mixedWeight > 0.0 ? mixed / mixedWeight : 0.0;
        if (length > 0.0) {
            const f64 reached = before + advance / length;
            if (playing.loop) {
                const f64 whole = std::floor(reached);
                time = reached - whole;
                out.wrapped = whole >= 1.0;
            }
            else {
                time = std::min(reached, 1.0);
            }
            out.known = true;
        }
        for (u32 index = 0; index < count; ++index)
            emit(index, time * lengthOf(index));
        out.previous = before;
        out.now = time;
        return out;
    }

    // Each clip on its own length, all from the one count of seconds since
    // the state was entered. Nothing is counted while no clip has a length:
    // a clip that loads late starts at its beginning, not where it would
    // have got to.
    f64 longest = 0.0;
    for (u32 index = 0; index < count; ++index)
        longest = std::max(longest, lengthOf(index));
    if (longest > 0.0) {
        time = before + advance;
        if (!playing.loop) {
            // Held at the end, and the count with it.
            time = std::min(time, longest);
        }
        else if (count == 1) {
            // One clip has one length to wrap on; several have no common one,
            // and go on counting.
            const Turns turns = turnsOf(time, longest);
            if (turns.whole >= 1.0)
                time = turns.part * longest;
        }
    }
    for (u32 index = 0; index < count; ++index) {
        const f64 length = lengthOf(index);
        f64 at = 0.0;
        if (length > 0.0) {
            if (!playing.loop)
                at = std::min(time, length);
            else
                at = time < length ? time : turnsOf(time, length).part * length;
        }
        emit(index, at);
    }

    const f64 length = lengthOf(heaviest);
    if (length > 0.0) {
        out.known = true;
        if (playing.loop) {
            const Turns was = turnsOf(before, length);
            const Turns reached = turnsOf(before + advance, length);
            // Where it is comes from the time as it was kept, so the next
            // step's "before" is this step's "now" to the bit.
            out.previous = was.part;
            out.now = turnsOf(time, length).part;
            out.wrapped = reached.whole > was.whole;
        }
        else {
            out.previous = std::min(before / length, 1.0);
            out.now = std::min(time / length, 1.0);
        }
    }
    return out;
}

void GraphPlayer::step(const AnimationGraph& graph, f64 dt, std::span<const f32> clipLengths,
                       std::vector<Signal>& signals)
{
    if (!fits(graph))
        reset(graph);
    // Also what a NaN is taken as.
    if (!(dt > 0.0))
        dt = 0.0;

    for (u32 index = 0; index < m_layers.size(); ++index)
        tryTransitions(graph, index, signals);

    m_tracks.clear();
    std::fill(m_clips.begin(), m_clips.end(), ClipState{});

    for (u32 index = 0; index < m_layers.size(); ++index) {
        Layer& layer = m_layers[index];
        const GraphLayer& described = graph.layers[index];

        if (layer.fadingCount > 0) {
            layer.fadeElapsed += dt;
            const f64 length = static_cast<f64>(layer.fadeLength);
            if (layer.fadeElapsed >= length * (1.0 - FadeSlack)) {
                layer.weight = 1.0f;
                layer.fadeLength = 0.0f;
                layer.fadeElapsed = 0.0;
                layer.fadingCount = 0;
            }
            else {
                layer.weight = static_cast<f32>(layer.fadeElapsed / length);
                for (u32 at = 0; at < layer.fadingCount; ++at)
                    layer.fading[at].weight = layer.fadingShare[at] * (1.0f - layer.weight);
            }
        }

        for (u32 at = 0; at < layer.fadingCount; ++at) {
            (void)play(graph, described.states[layer.fading[at].state], layer.fadingTime[at], layer.fading[at].weight,
                       dt, clipLengths);
        }
        const Played played = play(graph, described.states[layer.state], layer.time, layer.weight, dt, clipLengths);
        layer.progress = played.known ? played.now : 0.0;
        layer.progressKnown = played.known;
        if (!played.known)
            continue;

        // Only the state the layer is in, and of a blend only the clip that
        // weighs most: a walk and a run fading into each other step once.
        const std::vector<asset::GraphEvent>& events = graph.clips[played.clip].events;
        const auto crossed = [&](f64 after, f64 upTo) {
            for (u32 event = 0; event < events.size(); ++event) {
                const f64 at = static_cast<f64>(events[event].at);
                if (at > after && at <= upTo)
                    signals.push_back(Signal{Signal::Kind::Event, index, layer.state, layer.state, played.clip, event});
            }
        };
        // A state just entered has not passed its start yet, so an event at
        // nought is ahead of it; after that, nought is passed at each wrap.
        const f64 previous = layer.entered ? -1.0 : played.previous;
        if (played.wrapped) {
            crossed(previous, 1.0);
            crossed(-1.0, played.now);
        }
        else {
            crossed(previous, played.now);
        }
        layer.entered = false;
    }

    for (Parameter& parameter : m_parameters)
        parameter.fired = false;
}

void GraphPlayer::hashInto(std::vector<u64>& words) const
{
    words.push_back(static_cast<u64>(m_layers.size()));
    for (const Layer& layer : m_layers) {
        words.push_back(static_cast<u64>(layer.state) | (static_cast<u64>(layer.fadingCount) << 32));
        words.push_back(bits(layer.time));
        words.push_back(bits(layer.weight) | (bits(layer.fadeLength) << 32));
        words.push_back(bits(layer.fadeElapsed));
        words.push_back(bits(layer.progress));
        words.push_back((layer.progressKnown ? 1u : 0u) | (layer.entered ? 2u : 0u));
        for (u32 at = 0; at < layer.fadingCount; ++at) {
            words.push_back(layer.fading[at].state);
            words.push_back(bits(layer.fadingTime[at]));
            words.push_back(bits(layer.fading[at].weight) | (bits(layer.fadingShare[at]) << 32));
        }
    }
    words.push_back(static_cast<u64>(m_parameters.size()));
    for (const Parameter& parameter : m_parameters) {
        words.push_back((parameter.overridden ? 1u : 0u) | (parameter.sourced ? 2u : 0u) | (parameter.fired ? 4u : 0u));
        words.push_back(bits(parameter.overrideValue) | (bits(parameter.source) << 32));
    }
}

} // namespace engine::render
