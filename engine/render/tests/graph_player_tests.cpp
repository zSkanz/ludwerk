// The animation graph's evaluator (ADR 0197), on numbers: states, transitions,
// fades, blends, one phase for a synced blend, events once and in order.

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
#include <optional>
// doctest prints a string it compared through `operator<<`.
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/animation_graph.h"
#include "engine/render/graph_player.h"

using namespace engine;
using render::GraphPlayer;
using Signal = render::GraphPlayer::Signal;

namespace {

// The parameters every graph here has, by index.
constexpr core::u32 Speed = 0;
constexpr core::u32 X = 1;
constexpr core::u32 Y = 2;
constexpr core::u32 Go = 3;
constexpr core::u32 Attack = 4;

// A graph, a player on it, every clip a second long until a test says otherwise.
struct Rig
{
    asset::AnimationGraph graph;
    GraphPlayer player;
    std::vector<core::f32> lengths;
    // What the last step signalled, and nothing older.
    std::vector<Signal> signals;

    void step(core::f64 dt)
    {
        signals.clear();
        player.step(graph, dt, lengths, signals);
    }

    [[nodiscard]] const GraphPlayer::ClipState& clip(core::usize index) const { return player.clips()[index]; }

    // Everything a layer's states weigh between them: always one.
    [[nodiscard]] double layerTotal(core::u32 layer) const
    {
        double total = static_cast<double>(player.stateWeight(layer));
        for (const GraphPlayer::Fading& fading : player.fading(layer))
            total += static_cast<double>(fading.weight);
        return total;
    }

    [[nodiscard]] core::usize count(Signal::Kind kind) const
    {
        return static_cast<core::usize>(
            std::count_if(signals.begin(), signals.end(), [&](const Signal& signal) { return signal.kind == kind; }));
    }

    // The names of the events the last step fired, in order, joined by spaces.
    [[nodiscard]] std::string events() const
    {
        std::string out;
        for (const Signal& signal : signals) {
            if (signal.kind != Signal::Kind::Event)
                continue;
            if (!out.empty())
                out.push_back(' ');
            out.append(graph.clips[signal.clip].events[signal.event].name);
        }
        return out;
    }
};

// `layers` is the inside of the file's `layers` array; `more` goes among its
// top-level keys.
[[nodiscard]] Rig rigOf(std::string_view layers, std::string_view more = {})
{
    std::string json = R"({"format":"animgraph","version":1,"parameters":{"Speed":{"number":0},"X":{"number":0},)"
                       R"("Y":{"number":0},"Go":{"boolean":false},"Attack":{"trigger":true}},)";
    json.append(more);
    if (!more.empty())
        json.push_back(',');
    json.append(R"("layers":[)");
    json.append(layers);
    json.append("]}");

    asset::GraphReadError error;
    std::optional<asset::AnimationGraph> read = asset::readAnimationGraph(json, &error);
    CAPTURE(error.where);
    CAPTURE(error.what);
    REQUIRE(read.has_value());
    Rig rig;
    rig.graph = std::move(*read);
    rig.lengths.assign(rig.graph.clips.size(), 1.0f);
    rig.player.reset(rig.graph);
    return rig;
}

// One layer of two one-clip states, `A` playing `Idle` and `B` playing `Run`.
[[nodiscard]] std::string twoStates(std::string_view transitions, std::string_view name = "Body")
{
    std::string out = R"({"name":")";
    out.append(name);
    out.append(R"(","start":"A","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"}},"transitions":[)");
    out.append(transitions);
    out.append("]}");
    return out;
}

// One layer whose one state is a blend along `Speed`, written as given.
[[nodiscard]] std::string blendLayer(std::string_view state)
{
    std::string out = R"({"name":"Body","start":"Move","states":{"Move":)";
    out.append(state);
    out.append("}}");
    return out;
}

[[nodiscard]] double approx(core::f32 value)
{
    return static_cast<double>(value);
}

[[nodiscard]] std::vector<core::u64> hashOf(const GraphPlayer& player)
{
    std::vector<core::u64> words;
    player.hashInto(words);
    return words;
}

} // namespace

TEST_CASE("a graph player starts every layer in its start state with parameters at rest")
{
    Rig rig = rigOf(R"({"name":"Body","start":"B","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"}}},)"
                    R"({"name":"Arms","start":"None","states":{"None":{},"Slash":{"clip":"Slash"}}})",
                    R"("events":{"Run":[{"at":0,"name":"Start"}]})");
    CHECK(rig.player.state(0) == 1);
    CHECK(rig.player.state(1) == 0);
    CHECK(rig.player.stateWeight(0) == 1.0f);
    CHECK(rig.player.fading(0).empty());
    REQUIRE(rig.player.clips().size() == 3);
    CHECK_FALSE(rig.clip(1).active);
    CHECK(rig.player.tracks().empty());

    // The start state is at nought before the first step moves it: an event
    // at nought is ahead of it, not behind.
    rig.step(0.25);
    CHECK(rig.events() == "Start");
    CHECK(rig.clip(1).active);
    CHECK(rig.clip(1).time == doctest::Approx(0.25));
    CHECK(rig.clip(1).weight == 1.0f);
    CHECK_FALSE(rig.clip(0).active);
    CHECK_FALSE(rig.clip(2).active);
    REQUIRE(rig.player.tracks().size() == 1);
    CHECK(rig.player.tracks()[0].clip == 1);
    CHECK(approx(rig.player.stateProgress(0)) == doctest::Approx(0.25));

    // Out of range is an answer, not a crash.
    CHECK(rig.player.state(9) == 0);
    CHECK(rig.player.stateProgress(9) == 0.0f);
    CHECK(rig.player.fading(9).empty());
    CHECK(rig.player.value(99) == 0.0f);
    CHECK(rig.player.layerWeight(rig.graph, 9) == 0.0f);
}

TEST_CASE("a graph player that was never reset resets itself on its first step")
{
    Rig rig = rigOf(twoStates(""));
    GraphPlayer fresh;
    std::vector<Signal> signals;
    fresh.step(rig.graph, 0.25, rig.lengths, signals);
    REQUIRE(fresh.clips().size() == 2);
    CHECK(fresh.clips()[0].active);
    CHECK(fresh.clips()[0].time == doctest::Approx(0.25));
}

TEST_CASE("a graph parameter is its override, else its source, else its rest")
{
    Rig rig = rigOf(twoStates(""));
    CHECK(rig.player.value(Speed) == 0.0f);
    CHECK_FALSE(rig.player.overridden(Speed));
    rig.player.setSource(Speed, 3.0f);
    CHECK(rig.player.value(Speed) == 3.0f);
    rig.player.setOverride(Speed, 5.0f);
    CHECK(rig.player.overridden(Speed));
    CHECK(rig.player.value(Speed) == 5.0f);
    // The world goes on saying what it says under a script's value.
    rig.player.setSource(Speed, 4.0f);
    CHECK(rig.player.value(Speed) == 5.0f);
    rig.player.clearOverride(Speed);
    CHECK_FALSE(rig.player.overridden(Speed));
    CHECK(rig.player.value(Speed) == 4.0f);

    rig.player.setOverride(Speed, std::numeric_limits<core::f32>::quiet_NaN());
    CHECK(rig.player.value(Speed) == 0.0f);
    rig.player.setSource(X, std::numeric_limits<core::f32>::infinity());
    CHECK(rig.player.value(X) == 0.0f);

    // A trigger is fired, never set.
    rig.player.setOverride(Attack, 1.0f);
    CHECK(rig.player.value(Attack) == 0.0f);
    rig.player.fire(Attack);
    CHECK(rig.player.value(Attack) == 1.0f);
    // And only a trigger is fired.
    rig.player.fire(Speed);
    rig.player.clearOverride(Speed);
    CHECK(rig.player.value(Speed) == 4.0f);
}

TEST_CASE("a blend along one parameter mixes the two clips either side, in proportion")
{
    Rig rig = rigOf(blendLayer(R"({"blend":"Speed","clips":[{"clip":"Idle","at":0},{"clip":"Walk","at":2},)"
                               R"({"clip":"Run","at":6}]})"));
    rig.player.setOverride(Speed, 3.0f);
    rig.step(0.25);
    CHECK(rig.clip(0).active);
    CHECK(rig.clip(0).weight == 0.0f);
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.75));
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.25));
    // Nothing else is played: a clip the parameter leaves out is no track.
    REQUIRE(rig.player.tracks().size() == 2);
    CHECK(rig.player.tracks()[0].clip == 1);
    CHECK(rig.player.tracks()[1].clip == 2);
    CHECK(approx(rig.player.tracks()[0].weight) == doctest::Approx(0.75));

    rig.player.setOverride(Speed, 1.0f);
    rig.step(0.25);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.5));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.5));
    CHECK(rig.clip(2).weight == 0.0f);

    // On a clip, that clip alone; past either end, the end.
    struct End
    {
        core::f32 speed;
        core::u32 alone;
    };
    for (const End end : {End{2.0f, 1}, End{-4.0f, 0}, End{0.0f, 0}, End{6.0f, 2}, End{60.0f, 2}}) {
        CAPTURE(end.speed);
        rig.player.setOverride(Speed, end.speed);
        rig.step(0.25);
        for (core::u32 index = 0; index < 3; ++index)
            CHECK(rig.clip(index).weight == (index == end.alone ? 1.0f : 0.0f));
    }
}

TEST_CASE("a blend with two clips at one value divides by nothing")
{
    Rig rig = rigOf(blendLayer(R"({"blend":"Speed","clips":[{"clip":"Idle","at":0},{"clip":"Walk","at":2},)"
                               R"({"clip":"Jog","at":2},{"clip":"Run","at":6}]})"));
    for (const core::f32 speed : {0.0f, 1.0f, 2.0f, 3.0f, 6.0f}) {
        CAPTURE(speed);
        rig.player.setOverride(Speed, speed);
        rig.step(0.1);
        double total = 0.0;
        for (core::usize index = 0; index < 4; ++index) {
            CHECK(std::isfinite(rig.clip(index).weight));
            CHECK(rig.clip(index).weight >= 0.0f);
            total += static_cast<double>(rig.clip(index).weight);
        }
        CHECK(total == doctest::Approx(1.0));
    }
    // At the doubled value, below it is the first of the two and above it the
    // second: the blend is continuous from either side.
    rig.player.setOverride(Speed, 1.0f);
    rig.step(0.1);
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.5));
    CHECK(rig.clip(2).weight == 0.0f);
    rig.player.setOverride(Speed, 4.0f);
    rig.step(0.1);
    CHECK(rig.clip(1).weight == 0.0f);
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.5));
}

TEST_CASE("a synced blend keeps one phase, at the rate of its clips' mix")
{
    Rig rig =
        rigOf(blendLayer(R"({"blend":"Speed","sync":true,"clips":[{"clip":"Walk","at":0},{"clip":"Run","at":1}]})"));
    rig.lengths = {1.0f, 2.0f};
    rig.player.setOverride(Speed, 0.5f);

    // Half of a second-long clip and half of a two-second one is a clip of a
    // second and a half: 0.15 s is a tenth of it.
    rig.step(0.15);
    CHECK(approx(rig.player.stateProgress(0)) == doctest::Approx(0.1));
    CHECK(rig.clip(0).time == doctest::Approx(0.1));
    CHECK(rig.clip(1).time == doctest::Approx(0.2));

    for (int tick = 0; tick < 40; ++tick) {
        rig.step(0.15);
        const core::f32 phase = rig.player.stateProgress(0);
        CHECK(phase >= 0.0f);
        CHECK(phase <= 1.0f);
        // Their feet stay together: each is as far through itself as the other.
        CHECK(rig.clip(1).time == doctest::Approx(2.0 * rig.clip(0).time));
        CHECK(rig.clip(0).time == doctest::Approx(static_cast<double>(phase)).epsilon(1e-5));
    }
    // Forty-one tenths of a loop: a tenth into the fifth.
    CHECK(approx(rig.player.stateProgress(0)) == doctest::Approx(0.1).epsilon(1e-4));

    // All of the longer clip: the phase moves at the longer clip's rate.
    rig.player.setOverride(Speed, 1.0f);
    const double before = approx(rig.player.stateProgress(0));
    rig.step(0.2);
    CHECK(approx(rig.player.stateProgress(0)) - before == doctest::Approx(0.1).epsilon(1e-4));
}

TEST_CASE("an unsynced blend plays each clip on its own length")
{
    Rig rig = rigOf(blendLayer(R"({"blend":"Speed","clips":[{"clip":"Walk","at":0},{"clip":"Run","at":1}]})"));
    rig.lengths = {1.0f, 2.0f};
    rig.player.setOverride(Speed, 0.75f);
    for (int tick = 0; tick < 5; ++tick)
        rig.step(0.25);
    CHECK(rig.clip(0).time == doctest::Approx(0.25));
    CHECK(rig.clip(1).time == doctest::Approx(1.25));
    // How far through it is, is how far through its heaviest clip is.
    CHECK(approx(rig.player.stateProgress(0)) == doctest::Approx(0.625));
    rig.player.setOverride(Speed, 0.25f);
    rig.step(0.25);
    CHECK(approx(rig.player.stateProgress(0)) == doctest::Approx(0.5));
}

TEST_CASE("a clip state loops or holds, at the speed its state says")
{
    Rig rig = rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Idle","speed":2}}},)"
                    R"({"name":"Arms","start":"A","states":{"A":{"clip":"Slash","loop":false}}})");
    rig.lengths = {1.0f, 0.5f};
    rig.step(0.2);
    CHECK(rig.clip(0).time == doctest::Approx(0.4));
    CHECK(rig.clip(1).time == doctest::Approx(0.2));
    rig.step(0.2);
    rig.step(0.2);
    // The loop wrapped; the one-shot reached its end and stays there.
    CHECK(rig.clip(0).time == doctest::Approx(0.2));
    CHECK(rig.clip(1).time == doctest::Approx(0.5));
    CHECK(rig.player.stateProgress(1) == 1.0f);
    for (int tick = 0; tick < 5; ++tick)
        rig.step(0.2);
    CHECK(rig.clip(1).time == doctest::Approx(0.5));
    CHECK(rig.clip(1).weight == 1.0f);
    CHECK(rig.player.stateProgress(1) == 1.0f);
}

TEST_CASE("a state held at its end is the same state every step after")
{
    Rig rig = rigOf(R"({"name":"Arms","start":"A","states":{"A":{"clip":"Slash","loop":false}}})");
    for (int tick = 0; tick < 6; ++tick)
        rig.step(0.25);
    const std::vector<core::u64> held = hashOf(rig.player);
    for (int tick = 0; tick < 6; ++tick)
        rig.step(0.25);
    CHECK(held == hashOf(rig.player));
}

TEST_CASE("a transition's conditions: every operator")
{
    const auto taken = [](std::string_view condition, core::f32 speed, bool go = false) {
        std::string transition = R"({"from":"A","to":"B","when":[)";
        transition.append(condition);
        transition.append("]}");
        Rig rig = rigOf(twoStates(transition));
        rig.player.setOverride(Speed, speed);
        rig.player.setOverride(Go, go ? 1.0f : 0.0f);
        rig.step(0.1);
        return rig.player.state(0) == 1;
    };
    CHECK(taken(R"(["Speed","==",2])", 2.0f));
    CHECK_FALSE(taken(R"(["Speed","==",2])", 2.5f));
    CHECK(taken(R"(["Speed","~=",2])", 2.5f));
    CHECK_FALSE(taken(R"(["Speed","~=",2])", 2.0f));
    CHECK(taken(R"(["Speed","!=",2])", 1.0f));
    CHECK_FALSE(taken(R"(["Speed","!=",2])", 2.0f));
    CHECK(taken(R"(["Speed","<",2])", 1.5f));
    CHECK_FALSE(taken(R"(["Speed","<",2])", 2.0f));
    CHECK(taken(R"(["Speed","<=",2])", 2.0f));
    CHECK_FALSE(taken(R"(["Speed","<=",2])", 2.5f));
    CHECK(taken(R"(["Speed",">",2])", 2.5f));
    CHECK_FALSE(taken(R"(["Speed",">",2])", 2.0f));
    CHECK(taken(R"(["Speed",">=",2])", 2.0f));
    CHECK_FALSE(taken(R"(["Speed",">=",2])", 1.5f));

    // A boolean alone, or against a boolean.
    CHECK(taken(R"(["Go"])", 0.0f, true));
    CHECK_FALSE(taken(R"(["Go"])", 0.0f, false));
    CHECK(taken(R"(["Go","==",false])", 0.0f, false));
    CHECK_FALSE(taken(R"(["Go","==",false])", 0.0f, true));
    CHECK(taken(R"(["Go","==",true])", 0.0f, true));

    // Every condition must hold, and none is "always".
    CHECK(taken(R"(["Speed",">",1],["Go"])", 2.0f, true));
    CHECK_FALSE(taken(R"(["Speed",">",1],["Go"])", 2.0f, false));
    CHECK_FALSE(taken(R"(["Speed",">",1],["Go"])", 0.5f, true));
    CHECK(taken("", 0.0f));
}

TEST_CASE("a layer's transitions are tried in the file's order, one a step")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"},"C":{"clip":"Jump"}},)"
              R"("transitions":[{"from":"A","to":"C","when":[["Speed",">",5]]},)"
              R"({"from":"A","to":"B","when":[["Speed",">",1]]},{"from":"B","to":"C","when":[["Speed",">",1]]}]})");
    rig.player.setOverride(Speed, 9.0f);
    rig.step(0.1);
    // Both of A's hold; the first written wins.
    CHECK(rig.player.state(0) == 2);
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].kind == Signal::Kind::StateChanged);
    CHECK(rig.signals[0].layer == 0);
    CHECK(rig.signals[0].from == 0);
    CHECK(rig.signals[0].to == 2);

    rig.player.reset(rig.graph);
    rig.player.setOverride(Speed, 3.0f);
    rig.step(0.1);
    // B's own transition holds too, and waits for the next step.
    CHECK(rig.player.state(0) == 1);
    CHECK(rig.count(Signal::Kind::StateChanged) == 1);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 2);
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].from == 1);
    CHECK(rig.signals[0].to == 2);
}

TEST_CASE("a trigger fires one transition and is gone")
{
    SUBCASE("one firing does not drive the way there and the way back")
    {
        Rig rig =
            rigOf(twoStates(R"({"from":"A","to":"B","when":[["Attack"]]},{"from":"B","to":"A","when":[["Attack"]]})"));
        rig.step(0.1);
        CHECK(rig.player.state(0) == 0);
        rig.player.fire(Attack);
        rig.step(0.1);
        CHECK(rig.player.state(0) == 1);
        CHECK(rig.player.value(Attack) == 0.0f);
        for (int tick = 0; tick < 3; ++tick) {
            rig.step(0.1);
            CHECK(rig.player.state(0) == 1);
            CHECK(rig.signals.empty());
        }
    }
    SUBCASE("a layer that takes it leaves nothing for the layer above")
    {
        const std::string take = R"({"from":"A","to":"B","when":[["Attack"]]})";
        Rig rig = rigOf(twoStates(take, "Body") + "," + twoStates(take, "Arms"));
        rig.player.fire(Attack);
        rig.step(0.1);
        CHECK(rig.player.state(0) == 1);
        CHECK(rig.player.state(1) == 0);
        rig.step(0.1);
        CHECK(rig.player.state(1) == 0);
    }
    SUBCASE("one nobody takes is gone when the step ends")
    {
        Rig rig = rigOf(twoStates(R"({"from":"A","to":"B","when":[["Attack"],["Go"]]})"));
        rig.player.fire(Attack);
        CHECK(rig.player.value(Attack) == 1.0f);
        rig.step(0.1);
        CHECK(rig.player.state(0) == 0);
        CHECK(rig.player.value(Attack) == 0.0f);
        // The other condition coming true later finds no trigger waiting.
        rig.player.setOverride(Go, 1.0f);
        rig.step(0.1);
        CHECK(rig.player.state(0) == 0);
        rig.player.fire(Attack);
        rig.step(0.1);
        CHECK(rig.player.state(0) == 1);
    }
}

TEST_CASE("`after` holds a transition until that much of the state has played")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"Jump","states":{"Jump":{"clip":"Jump","loop":false},"Fall":{"clip":"Fall"}},)"
              R"("transitions":[{"from":"Jump","to":"Fall","after":0.5}]})");
    rig.step(0.25);
    CHECK(rig.player.state(0) == 0);
    // Half way, as this step leaves it: the next one is the first to see it.
    rig.step(0.25);
    CHECK(rig.player.state(0) == 0);
    CHECK(rig.player.stateProgress(0) == 0.5f);
    rig.step(0.25);
    CHECK(rig.player.state(0) == 1);

    SUBCASE("the end of a clip that does not loop is reached and held")
    {
        Rig whole = rigOf(R"({"name":"Body","start":"Jump","states":{"Jump":{"clip":"Jump","loop":false},)"
                          R"("Fall":{"clip":"Fall"}},"transitions":[{"from":"Jump","to":"Fall","after":1.0}]})");
        whole.lengths = {0.7f, 1.0f};
        int ticks = 0;
        while (whole.player.state(0) == 0 && ticks < 100) {
            whole.step(1.0 / 60.0);
            ++ticks;
        }
        // 0.7 s is forty-two sixtieths; the step after the one that gets
        // there is the one that leaves.
        CHECK(ticks >= 43);
        CHECK(ticks <= 44);
    }
    SUBCASE("a condition and an `after` are both needed")
    {
        Rig both = rigOf(twoStates(R"({"from":"A","to":"B","when":[["Go"]],"after":0.5})"));
        both.player.setOverride(Go, 1.0f);
        both.step(0.25);
        both.step(0.25);
        CHECK(both.player.state(0) == 0);
        both.player.setOverride(Go, 0.0f);
        both.step(0.25);
        CHECK(both.player.state(0) == 0);
        both.player.setOverride(Go, 1.0f);
        both.step(0.1);
        CHECK(both.player.state(0) == 1);
    }
}

TEST_CASE("a clip whose length is not known stays at nought and satisfies no `after`")
{
    Rig rig = rigOf(twoStates(R"({"from":"A","to":"B","after":0})"));
    rig.lengths = {0.0f, 1.0f};
    for (int tick = 0; tick < 20; ++tick) {
        rig.step(0.25);
        CHECK(rig.player.state(0) == 0);
        CHECK(rig.clip(0).active);
        CHECK(rig.clip(0).time == 0.0);
        // It weighs what it would: the pose waits for the clip, the graph does not.
        CHECK(rig.clip(0).weight == 1.0f);
        CHECK(rig.player.stateProgress(0) == 0.0f);
    }
    // A span too short to hold the clip is the same as nought.
    rig.lengths.clear();
    rig.step(0.25);
    CHECK(rig.player.state(0) == 0);

    // Its file arrives: it plays from its beginning, and `after` can hold.
    rig.lengths = {1.0f, 1.0f};
    rig.step(0.25);
    CHECK(rig.player.state(0) == 0);
    CHECK(rig.clip(0).time == doctest::Approx(0.25));
    rig.step(0.25);
    CHECK(rig.player.state(0) == 1);
}

TEST_CASE("a fade's weights add to one at every step and end at one and nothing")
{
    Rig rig = rigOf(twoStates(R"({"from":"A","to":"B","when":[["Go"]],"fade":0.2})"));
    rig.step(1.0 / 60.0);
    CHECK(rig.clip(0).weight == 1.0f);
    rig.player.setOverride(Go, 1.0f);
    for (int tick = 1; tick <= 11; ++tick) {
        CAPTURE(tick);
        rig.step(1.0 / 60.0);
        CHECK(rig.player.state(0) == 1);
        CHECK(approx(rig.player.stateWeight(0)) == doctest::Approx(tick / 12.0).epsilon(1e-4));
        REQUIRE(rig.player.fading(0).size() == 1);
        CHECK(rig.player.fading(0)[0].state == 0);
        CHECK(rig.layerTotal(0) == doctest::Approx(1.0));
        CHECK(approx(rig.clip(0).weight) + approx(rig.clip(1).weight) == doctest::Approx(1.0));
        CHECK(approx(rig.clip(1).weight) == doctest::Approx(tick / 12.0).epsilon(1e-4));
        // What fades out goes on playing; what fades in started at nought.
        CHECK(rig.clip(0).time == doctest::Approx((tick + 1) / 60.0));
        CHECK(rig.clip(1).time == doctest::Approx(tick / 60.0));
    }
    rig.step(1.0 / 60.0);
    CHECK(rig.player.stateWeight(0) == 1.0f);
    CHECK(rig.player.fading(0).empty());
    CHECK(rig.clip(1).weight == 1.0f);
    CHECK_FALSE(rig.clip(0).active);
    CHECK(rig.clip(0).weight == 0.0f);
    REQUIRE(rig.player.tracks().size() == 1);
    CHECK(rig.player.tracks()[0].clip == 1);
}

TEST_CASE("a transition with no fade is taken at once, and drops whatever was fading")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"},"C":{"clip":"Jump"}},)"
              R"("transitions":[{"from":"A","to":"B","when":[["Speed","==",1]],"fade":5},)"
              R"({"from":"B","to":"C","when":[["Speed","==",2]]}]})");
    rig.player.setOverride(Speed, 1.0f);
    rig.step(0.5);
    CHECK(rig.player.fading(0).size() == 1);
    rig.player.setOverride(Speed, 2.0f);
    rig.step(0.5);
    CHECK(rig.player.state(0) == 2);
    CHECK(rig.player.stateWeight(0) == 1.0f);
    CHECK(rig.player.fading(0).empty());
    CHECK(rig.clip(2).weight == 1.0f);
    CHECK_FALSE(rig.clip(0).active);
    CHECK_FALSE(rig.clip(1).active);
}

TEST_CASE("a fade interrupted keeps what was fading, in proportion, adding to one")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"},"C":{"clip":"Jump"}},)"
              R"("transitions":[{"from":"A","to":"B","when":[["Speed","==",1]],"fade":0.4},)"
              R"({"from":"B","to":"C","when":[["Speed","==",2]],"fade":0.4}]})");
    rig.player.setOverride(Speed, 1.0f);
    rig.step(0.1);
    rig.step(0.1);
    rig.step(0.1);
    // Three quarters of the way to B.
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.25).epsilon(1e-4));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.75).epsilon(1e-4));

    rig.player.setOverride(Speed, 2.0f);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 2);
    REQUIRE(rig.player.fading(0).size() == 2);
    // Oldest first.
    CHECK(rig.player.fading(0)[0].state == 0);
    CHECK(rig.player.fading(0)[1].state == 1);
    // C has a quarter; A and B share the rest one to three, as they stood.
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.25).epsilon(1e-4));
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.25 * 0.75).epsilon(1e-4));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.75 * 0.75).epsilon(1e-4));
    CHECK(rig.layerTotal(0) == doctest::Approx(1.0));
    // The half-faded B went on from where it was; C started.
    CHECK(rig.clip(1).time == doctest::Approx(0.4));
    CHECK(rig.clip(2).time == doctest::Approx(0.1));
    CHECK(rig.player.tracks().size() == 3);

    rig.step(0.1);
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.25 * 0.5).epsilon(1e-4));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.75 * 0.5).epsilon(1e-4));
    CHECK(rig.layerTotal(0) == doctest::Approx(1.0));
    rig.step(0.1);
    rig.step(0.1);
    CHECK(rig.clip(2).weight == 1.0f);
    CHECK(rig.player.fading(0).empty());
}

TEST_CASE("a layer fades out of four states at most, and the lightest makes room")
{
    std::string states;
    std::string transitions;
    for (int index = 0; index < 6; ++index) {
        const std::string name = "S" + std::to_string(index);
        states += (index == 0 ? "\"" : ",\"") + name + R"(":{"clip":"C)" + std::to_string(index) + "\"}";
        if (index == 0)
            continue;
        transitions += std::string(index == 1 ? "" : ",") + R"({"from":"S)" + std::to_string(index - 1) +
                       R"(","to":")" + name + R"(","when":[["Speed","==",)" + std::to_string(index) +
                       R"(]],"fade":10})";
    }
    Rig rig = rigOf(R"({"name":"Body","start":"S0","states":{)" + states + R"(},"transitions":[)" + transitions + "]}");
    for (int index = 1; index <= 5; ++index) {
        rig.player.setOverride(Speed, static_cast<core::f32>(index));
        rig.step(1.0);
        CHECK(rig.player.state(0) == static_cast<core::u32>(index));
        CHECK(rig.player.fading(0).size() == static_cast<core::usize>(std::min(index, 4)));
        CHECK(rig.layerTotal(0) == doctest::Approx(1.0));
        double clips = 0.0;
        for (const GraphPlayer::ClipState& clip : rig.player.clips())
            clips += static_cast<double>(clip.weight);
        CHECK(clips == doctest::Approx(1.0));
    }
    // S1 had faded furthest (it was a tenth of the layer, three fades ago),
    // and is the one that went.
    const std::span<const GraphPlayer::Fading> fading = rig.player.fading(0);
    REQUIRE(fading.size() == 4);
    CHECK(fading[0].state == 0);
    CHECK(fading[1].state == 2);
    CHECK(fading[2].state == 3);
    CHECK(fading[3].state == 4);
    CHECK_FALSE(rig.clip(1).active);
}

TEST_CASE("a `*` transition leaves any state but the one it leads to")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"Move","states":{"Move":{"clip":"Idle"},"Jump":{"clip":"Jump"},)"
              R"("Fall":{"clip":"Fall"}},"transitions":[{"from":"Move","to":"Jump","when":[["Speed","==",1]]},)"
              R"({"from":"Jump","to":"Fall","when":[["Speed","==",2]]},{"from":"*","to":"Move","when":[["Go"]]}]})");
    rig.player.setOverride(Go, 1.0f);
    // Already there: the condition holding is not a reason to start again.
    for (int tick = 0; tick < 4; ++tick) {
        rig.step(0.1);
        CHECK(rig.signals.empty());
        CHECK(rig.player.state(0) == 0);
    }
    CHECK(rig.clip(0).time == doctest::Approx(0.4));

    rig.player.setOverride(Go, 0.0f);
    rig.player.setOverride(Speed, 1.0f);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 1);
    rig.player.setOverride(Go, 1.0f);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 0);

    rig.player.setOverride(Go, 0.0f);
    rig.step(0.1);
    rig.player.setOverride(Speed, 2.0f);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 2);
    rig.player.setOverride(Go, 1.0f);
    rig.step(0.1);
    CHECK(rig.player.state(0) == 0);
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].from == 2);
    CHECK(rig.signals[0].to == 0);
}

TEST_CASE("a state with no clip holds its weight and plays nothing")
{
    Rig rig = rigOf(R"({"name":"Arms","start":"None","states":{"None":{},"Slash":{"clip":"Slash","loop":false}},)"
                    R"("transitions":[{"from":"*","to":"Slash","when":[["Attack"]],"fade":0.1},)"
                    R"({"from":"Slash","to":"None","after":1.0,"fade":0.2},{"from":"None","to":"Slash","after":0}]})");
    for (int tick = 0; tick < 3; ++tick) {
        rig.step(0.1);
        CHECK(rig.player.state(0) == 0);
        CHECK(rig.player.stateWeight(0) == 1.0f);
        CHECK(rig.player.stateProgress(0) == 0.0f);
        CHECK_FALSE(rig.clip(0).active);
        CHECK(rig.player.tracks().empty());
        // Nothing to be any way through: its `after` never comes.
        CHECK(rig.signals.empty());
    }

    rig.player.fire(Attack);
    rig.step(0.05);
    CHECK(rig.player.state(0) == 1);
    // Half in: the nothing it came from holds the other half, as nothing.
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.5).epsilon(1e-4));
    REQUIRE(rig.player.fading(0).size() == 1);
    CHECK(rig.player.fading(0)[0].state == 0);
    CHECK(approx(rig.player.fading(0)[0].weight) == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(rig.layerTotal(0) == doctest::Approx(1.0));

    // Through the slash and back out to nothing.
    int ticks = 0;
    while (rig.player.state(0) == 1 && ticks < 100) {
        rig.step(0.05);
        ++ticks;
    }
    CHECK(rig.player.state(0) == 0);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.75).epsilon(1e-4));
    for (int tick = 0; tick < 3; ++tick)
        rig.step(0.05);
    CHECK_FALSE(rig.clip(0).active);
    CHECK(rig.player.stateWeight(0) == 1.0f);
}

TEST_CASE("a layer's weight is its number or its parameter, held to 0..1")
{
    Rig rig = rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Idle"}}},)"
                    R"({"name":"Lean","additive":true,"weight":"Speed","start":"A","states":{"A":{"clip":"Aim"}}},)"
                    R"({"name":"Arms","weight":0.25,"start":"A","states":{"A":{"clip":"Wave"}}},)"
                    R"({"name":"Face","weight":"Go","start":"A","states":{"A":{"clip":"Blink"}}})");
    CHECK(rig.player.layerWeight(rig.graph, 0) == 1.0f);
    CHECK(rig.player.layerWeight(rig.graph, 1) == 0.0f);
    CHECK(rig.player.layerWeight(rig.graph, 2) == 0.25f);
    CHECK(rig.player.layerWeight(rig.graph, 3) == 0.0f);
    rig.player.setSource(Speed, 0.3f);
    CHECK(rig.player.layerWeight(rig.graph, 1) == 0.3f);
    rig.player.setOverride(Speed, 2.0f);
    CHECK(rig.player.layerWeight(rig.graph, 1) == 1.0f);
    rig.player.setOverride(Speed, -1.0f);
    CHECK(rig.player.layerWeight(rig.graph, 1) == 0.0f);
    rig.player.setOverride(Go, 1.0f);
    CHECK(rig.player.layerWeight(rig.graph, 3) == 1.0f);

    // A clip's weight says nothing of its layer's: the caller applies that.
    rig.player.setOverride(Speed, 0.5f);
    rig.step(0.1);
    CHECK(rig.clip(1).weight == 1.0f);
    CHECK(rig.clip(2).weight == 1.0f);
}

TEST_CASE("an event fires once each time its clip passes it, and on entry at nought")
{
    Rig rig = rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Run"}}})",
                    R"("events":{"Run":[{"at":0.5,"name":"Half"},{"at":0,"name":"Start"}]})");
    std::vector<std::string> fired;
    for (int tick = 0; tick < 9; ++tick) {
        rig.step(0.25);
        fired.push_back(rig.events());
        for (const Signal& signal : rig.signals) {
            CHECK(signal.kind == Signal::Kind::Event);
            CHECK(signal.layer == 0);
            CHECK(signal.clip == 0);
            CHECK(signal.from == 0);
            CHECK(signal.to == 0);
        }
    }
    // Entered at nought; half way on the second quarter; nought again each
    // time the loop comes round, and never on the step after.
    CHECK(fired == std::vector<std::string>{"Start", "Half", "", "Start", "", "Half", "", "Start", ""});
    // The index is into the clip's events as the graph sorted them.
    rig.step(0.25);
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].event == 1);
}

TEST_CASE("the events of one step come in the order the clip passed them")
{
    const std::string_view events = R"("events":{"Run":[{"at":0.9,"name":"Late"},{"at":0.05,"name":"Early"},)"
                                    R"({"at":0.3,"name":"Mid"},{"at":0.2,"name":"Low"},{"at":1,"name":"End"}]})";
    Rig rig = rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Run"}}},)"
                    R"({"name":"Arms","start":"A","states":{"A":{"clip":"Run"}}})",
                    events);
    rig.step(0.1);
    // Both layers, the lower first.
    CHECK(rig.events() == "Early Early");
    REQUIRE(rig.signals.size() == 2);
    CHECK(rig.signals[0].layer == 0);
    CHECK(rig.signals[0].clip == 0);
    CHECK(rig.signals[1].layer == 1);
    CHECK(rig.signals[1].clip == 1);

    rig.step(0.3);
    CHECK(rig.events() == "Low Mid Low Mid");
    rig.step(0.4);
    CHECK(rig.events().empty());
    // Across the wrap: what was left of the loop, then what the new one has
    // already passed.
    rig.step(0.3);
    CHECK(rig.events() == "Late End Early Late End Early");
    rig.step(0.05);
    CHECK(rig.events().empty());

    SUBCASE("a clip that does not loop fires its last event once and never again")
    {
        Rig once = rigOf(R"({"name":"Body","start":"A","states":{"A":{"clip":"Run","loop":false}}})", events);
        std::string all;
        for (int tick = 0; tick < 12; ++tick) {
            once.step(0.25);
            if (!once.events().empty())
                all += once.events() + "|";
        }
        CHECK(all == "Early Low|Mid|Late End|");
    }
}

TEST_CASE("an event does not fire from a state that is fading out, and fires on the way in")
{
    Rig rig =
        rigOf(twoStates(R"({"from":"A","to":"B","when":[["Go"]],"fade":10})"),
              R"("events":{"Idle":[{"at":0.5,"name":"Out"}],"Run":[{"at":0,"name":"In"},{"at":0.5,"name":"On"}]})");
    rig.step(0.25);
    CHECK(rig.events().empty());
    rig.player.setOverride(Go, 1.0f);
    rig.step(0.25);
    // A reached its half way this step -- as a state being left.
    CHECK(rig.clip(0).time == doctest::Approx(0.5));
    REQUIRE(rig.signals.size() == 2);
    CHECK(rig.signals[0].kind == Signal::Kind::StateChanged);
    CHECK(rig.signals[1].kind == Signal::Kind::Event);
    CHECK(rig.signals[1].clip == 1);
    CHECK(rig.signals[1].from == 1);
    CHECK(rig.events() == "In");
    rig.step(0.25);
    CHECK(rig.events() == "On");
    // A goes round again under the fade and still says nothing.
    for (int tick = 0; tick < 4; ++tick) {
        rig.step(0.25);
        CHECK(rig.events().find("Out") == std::string::npos);
    }
}

TEST_CASE("in a blend only the heaviest clip's events fire")
{
    Rig rig =
        rigOf(blendLayer(R"({"blend":"Speed","sync":true,"clips":[{"clip":"Walk","at":0},{"clip":"Run","at":1}]})"),
              R"("events":{"Walk":[{"at":0.5,"name":"WalkStep"}],"Run":[{"at":0.5,"name":"RunStep"}]})");
    rig.player.setOverride(Speed, 0.75f);
    rig.step(0.25);
    CHECK(rig.events().empty());
    rig.step(0.25);
    CHECK(rig.events() == "RunStep");
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].clip == 1);
    CHECK(rig.signals[0].event == 0);

    rig.player.setOverride(Speed, 0.25f);
    rig.step(0.5);
    rig.step(0.5);
    CHECK(rig.events() == "WalkStep");
    // Level: the first written.
    rig.player.setOverride(Speed, 0.5f);
    rig.step(0.5);
    rig.step(0.5);
    CHECK(rig.events() == "WalkStep");
}

TEST_CASE("a blend across two parameters: in a triangle, on a corner, outside")
{
    Rig rig = rigOf(R"({"name":"Body","start":"S","states":{"S":{"blend":["X","Y"],"clips":[{"clip":"A","at":[0,0]},)"
                    R"({"clip":"B","at":[1,0]},{"clip":"C","at":[0,1]}]}}})");
    const auto at = [&](core::f32 x, core::f32 y) {
        rig.player.setOverride(X, x);
        rig.player.setOverride(Y, y);
        rig.step(0.1);
    };
    // On a corner, that clip alone.
    at(1.0f, 0.0f);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.0));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(1.0));
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.0));
    at(0.0f, 1.0f);
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(1.0));
    at(0.0f, 0.0f);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(1.0));

    // At the middle, a third each.
    at(1.0f / 3.0f, 1.0f / 3.0f);
    for (core::usize index = 0; index < 3; ++index)
        CHECK(approx(rig.clip(index).weight) == doctest::Approx(1.0 / 3.0).epsilon(1e-4));

    // Inside, by where it lies.
    at(0.5f, 0.25f);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.25));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.5));
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.25));

    // Outside, the nearest point of the nearest edge: two clips and no third.
    at(0.25f, -1.0f);
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(0.75));
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.25));
    CHECK(rig.clip(2).weight == 0.0f);
    at(1.0f, 1.0f);
    CHECK(rig.clip(0).weight == 0.0f);
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.5));
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.5));
    // Past a corner, the corner.
    at(5.0f, -5.0f);
    CHECK(rig.clip(1).weight == 1.0f);
}

TEST_CASE("a blend across two parameters with no triangle follows its clips as a line")
{
    Rig rig =
        rigOf(R"({"name":"Body","start":"S","states":{"S":{"blend":["X","Y"],"clips":[{"clip":"A","at":[0,0]},)"
              R"({"clip":"B","at":[1,0]},{"clip":"C","at":[2,0]}]},"One":{"blend":["X","Y"],)"
              R"("clips":[{"clip":"Only","at":[3,3]}]}},"transitions":[{"from":"S","to":"One","when":[["Go"]]}]})");
    REQUIRE(rig.graph.layers[0].states[0].triangles.empty());
    rig.player.setOverride(X, 1.5f);
    rig.player.setOverride(Y, 5.0f);
    rig.step(0.1);
    CHECK(rig.clip(0).weight == 0.0f);
    CHECK(approx(rig.clip(1).weight) == doctest::Approx(0.5));
    CHECK(approx(rig.clip(2).weight) == doctest::Approx(0.5));
    rig.player.setOverride(X, -3.0f);
    rig.step(0.1);
    CHECK(rig.clip(0).weight == 1.0f);

    rig.player.setOverride(Go, 1.0f);
    rig.step(0.1);
    CHECK(rig.clip(3).weight == 1.0f);
}

TEST_CASE("a state entered again while it fades out is two tracks and one clip entry")
{
    Rig rig = rigOf(R"({"name":"Arms","start":"Slash","states":{"Slash":{"clip":"Slash","loop":false}},)"
                    R"("transitions":[{"from":"Slash","to":"Slash","when":[["Attack"]],"fade":0.4}]})");
    rig.step(0.25);
    rig.step(0.25);
    rig.player.fire(Attack);
    rig.step(0.1);
    REQUIRE(rig.signals.size() == 1);
    CHECK(rig.signals[0].from == 0);
    CHECK(rig.signals[0].to == 0);
    REQUIRE(rig.player.tracks().size() == 2);
    // The old swing, still going out; the new one, coming in from its start.
    CHECK(rig.player.tracks()[0].clip == 0);
    CHECK(rig.player.tracks()[0].time == doctest::Approx(0.6));
    CHECK(approx(rig.player.tracks()[0].weight) == doctest::Approx(0.75).epsilon(1e-4));
    CHECK(rig.player.tracks()[1].clip == 0);
    CHECK(rig.player.tracks()[1].time == doctest::Approx(0.1));
    CHECK(approx(rig.player.tracks()[1].weight) == doctest::Approx(0.25).epsilon(1e-4));
    // The one entry has both weights and the newer time.
    CHECK(approx(rig.clip(0).weight) == doctest::Approx(1.0));
    CHECK(rig.clip(0).time == doctest::Approx(0.1));
}

TEST_CASE("a graph player's hash is equal for equal histories and moves with any difference")
{
    const std::string layers =
        R"({"name":"Body","start":"Move","states":{"Move":{"blend":"Speed","sync":true,"clips":[{"clip":"Idle","at":0},)"
        R"({"clip":"Run","at":5}]},"Jump":{"clip":"Jump","loop":false}},"transitions":[)"
        R"({"from":"Move","to":"Jump","when":[["Go"]],"fade":0.3},{"from":"*","to":"Move","when":[["Go","==",false]],"fade":0.3}]},)"
        R"({"name":"Arms","start":"None","states":{"None":{},"Slash":{"clip":"Slash","loop":false}},)"
        R"("transitions":[{"from":"*","to":"Slash","when":[["Attack"]],"fade":0.1},{"from":"Slash","to":"None","after":1,"fade":0.2}]})";
    Rig a = rigOf(layers);
    Rig b = rigOf(layers);
    a.lengths = {1.0f, 0.7f, 0.5f, 0.4f};
    b.lengths = a.lengths;
    CHECK(hashOf(a.player) == hashOf(b.player));
    CHECK_FALSE(hashOf(a.player).empty());

    const auto drive = [](Rig& rig, int tick) {
        rig.player.setSource(Speed, static_cast<core::f32>(tick % 7));
        if (tick == 5)
            rig.player.setOverride(Go, 1.0f);
        if (tick == 9)
            rig.player.clearOverride(Go);
        if (tick % 6 == 2)
            rig.player.fire(Attack);
        rig.step(1.0 / 60.0);
    };
    std::vector<Signal> signalsA;
    std::vector<Signal> signalsB;
    for (int tick = 0; tick < 60; ++tick) {
        drive(a, tick);
        drive(b, tick);
        signalsA.insert(signalsA.end(), a.signals.begin(), a.signals.end());
        signalsB.insert(signalsB.end(), b.signals.begin(), b.signals.end());
        REQUIRE(hashOf(a.player) == hashOf(b.player));
    }
    // The history did something: states changed, on both layers.
    CHECK(signalsA.size() >= 4);
    REQUIRE(signalsA.size() == signalsB.size());
    for (core::usize index = 0; index < signalsA.size(); ++index) {
        CHECK(signalsA[index].kind == signalsB[index].kind);
        CHECK(signalsA[index].layer == signalsB[index].layer);
        CHECK(signalsA[index].to == signalsB[index].to);
    }
    for (core::usize index = 0; index < a.player.clips().size(); ++index) {
        CHECK(a.clip(index).time == b.clip(index).time);
        CHECK(a.clip(index).weight == b.clip(index).weight);
    }

    SUBCASE("one step of another length")
    {
        a.step(1.0 / 60.0);
        b.step(1.0 / 30.0);
        CHECK(hashOf(a.player) != hashOf(b.player));
    }
    SUBCASE("a parameter set and not yet stepped")
    {
        b.player.setSource(X, 1.0f);
        CHECK(hashOf(a.player) != hashOf(b.player));
    }
    SUBCASE("an override, and its going")
    {
        b.player.setOverride(Y, 2.0f);
        CHECK(hashOf(a.player) != hashOf(b.player));
        b.player.clearOverride(Y);
        CHECK(hashOf(a.player) == hashOf(b.player));
    }
    SUBCASE("a trigger fired and not yet stepped")
    {
        b.player.fire(Attack);
        CHECK(hashOf(a.player) != hashOf(b.player));
    }
    SUBCASE("a transition one took and the other did not")
    {
        b.player.setOverride(Go, 1.0f);
        a.step(1.0 / 60.0);
        b.step(1.0 / 60.0);
        b.player.clearOverride(Go);
        CHECK(hashOf(a.player) != hashOf(b.player));
    }
    SUBCASE("a copy is the same player")
    {
        GraphPlayer copy = a.player;
        std::vector<Signal> signals;
        a.step(1.0 / 60.0);
        copy.step(a.graph, 1.0 / 60.0, a.lengths, signals);
        CHECK(hashOf(a.player) == hashOf(copy));
    }
}
