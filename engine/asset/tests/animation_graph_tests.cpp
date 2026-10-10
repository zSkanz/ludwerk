// Animation graphs (ADR 0197): the file, read strictly, and the triangles of
// a blend across two parameters.
#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <optional>
// doctest prints a `string_view` it compared through `operator<<`.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/animation_graph.h"

using namespace engine;
using asset::AnimationGraph;
using asset::GraphOp;
using asset::GraphParameterKind;
using asset::GraphReadCode;
using asset::GraphReadError;
using asset::GraphStateKind;

namespace {

// The ADR's own example, whole.
constexpr std::string_view Example = R"({
  "format": "animgraph",
  "version": 1,
  "parameters": {
    "Speed":    { "number": 0,     "from": "CharacterBody.Speed" },
    "MoveX":    { "number": 0,     "from": "CharacterBody.MoveX" },
    "MoveZ":    { "number": 0,     "from": "CharacterBody.MoveZ" },
    "Grounded": { "boolean": true, "from": "CharacterBody.Grounded" },
    "Attack":   { "trigger": true, "from": "Attribute.Attack" },
    "Aim":      { "number": 0 }
  },
  "events": {
    "Slash": [ { "at": 0.35, "name": "Hit" } ],
    "Run":   [ { "at": 0.0, "name": "Step" }, { "at": 0.5, "name": "Step" } ]
  },
  "layers": [
    {
      "name": "Body",
      "start": "Move",
      "states": {
        "Move": { "blend": "Speed", "sync": true,
                  "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.6 }, { "clip": "Run", "at": 5 } ] },
        "Strafe": { "blend": [ "MoveX", "MoveZ" ], "sync": true,
                    "clips": [ { "clip": "Idle", "at": [0, 0] }, { "clip": "Forward", "at": [0, 1] },
                               { "clip": "Back", "at": [0, -1] }, { "clip": "Left", "at": [-1, 0] },
                               { "clip": "Right", "at": [1, 0] } ] },
        "Jump": { "clip": "Jump", "loop": false },
        "Fall": { "clip": "Fall" }
      },
      "transitions": [
        { "from": "Move", "to": "Jump", "when": [ ["Grounded", "==", false] ], "fade": 0.1 },
        { "from": "Jump", "to": "Fall", "after": 1.0, "fade": 0.15 },
        { "from": "*", "to": "Move", "when": [ ["Grounded", "==", true] ], "fade": 0.2 }
      ]
    },
    {
      "name": "Arms",
      "mask": [ "Spine1" ],
      "start": "None",
      "states": { "None": {}, "Slash": { "clip": "Slash", "loop": false } },
      "transitions": [
        { "from": "*", "to": "Slash", "when": [ ["Attack"] ], "fade": 0.05 },
        { "from": "Slash", "to": "None", "after": 1.0, "fade": 0.2 }
      ]
    },
    {
      "name": "Lean",
      "additive": true,
      "weight": "Aim",
      "start": "Up",
      "states": { "Up": { "clip": "AimUp" } }
    }
  ]
})";

// A graph of three parameters and whatever layers are given; `more` is put
// among its top-level keys.
[[nodiscard]] std::string graphOf(std::string_view layers, std::string_view more = {})
{
    std::string out = R"({"format":"animgraph","version":1,"parameters":{"Speed":{"number":0},)"
                      R"("Grounded":{"boolean":true},"Attack":{"trigger":true}},)";
    out.append(more);
    if (!more.empty())
        out.push_back(',');
    out.append(R"("layers":[)");
    out.append(layers);
    out.append("]}");
    return out;
}

// A layer of two one-clip states, `A` playing `Idle` and `B` playing `Run`,
// with `more` among its keys.
[[nodiscard]] std::string layerOf(std::string_view more = {}, std::string_view name = "Body")
{
    std::string out = R"({"name":")";
    out.append(name);
    out.append(R"(","start":"A","states":{"A":{"clip":"Idle"},"B":{"clip":"Run"}})");
    if (!more.empty())
        out.push_back(',');
    out.append(more);
    out.push_back('}');
    return out;
}

// A layer whose one state is `state`.
[[nodiscard]] std::string layerWithState(std::string_view state)
{
    std::string out = R"({"name":"Body","start":"A","states":{"A":)";
    out.append(state);
    out.append("}}");
    return out;
}

[[nodiscard]] std::string transitionOf(std::string_view more)
{
    std::string out = R"("transitions":[{"from":"A","to":"B")";
    if (!more.empty())
        out.push_back(',');
    out.append(more);
    out.append("}]");
    return out;
}

struct Refusal
{
    GraphReadCode code = GraphReadCode::None;
    std::string where;
    std::string what;
};

[[nodiscard]] Refusal refusal(const std::string& json)
{
    GraphReadError error;
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(json, &error);
    if (read.has_value())
        return Refusal{};
    return Refusal{error.code, error.where, error.what};
}

void checkRefused(const std::string& json, GraphReadCode code, std::string_view where, std::string_view what = {})
{
    CAPTURE(json);
    const Refusal got = refusal(json);
    CHECK(got.code == code);
    CHECK(got.where == where);
    if (!what.empty()) {
        CHECK(got.what == what);
    }
}

[[nodiscard]] bool has(const std::array<core::u32, 3>& triangle, core::u32 index)
{
    return std::find(triangle.begin(), triangle.end(), index) != triangle.end();
}

// The triangles of a two-parameter blend whose clips sit at `points`.
[[nodiscard]] std::vector<std::array<core::u32, 3>> trianglesOf(std::string_view points)
{
    std::string json = R"({"format":"animgraph","version":1,"parameters":{"X":{"number":0},"Y":{"number":0}},)"
                       R"("layers":[{"name":"L","start":"S","states":{"S":{"blend":["X","Y"],"clips":[)";
    json.append(points);
    json.append("]}}}]}");
    GraphReadError error;
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(json, &error);
    CAPTURE(error.where);
    REQUIRE(read.has_value());
    return read->layers[0].states[0].triangles;
}

} // namespace

TEST_CASE("an animation graph: the ADR's example reads whole")
{
    GraphReadError error;
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(Example, &error);
    CAPTURE(error.where);
    CAPTURE(error.what);
    REQUIRE(read.has_value());
    const AnimationGraph& graph = *read;
    CHECK(error.code == GraphReadCode::None);
    CHECK(graph.library.empty());

    // Parameters in the order the file wrote them.
    REQUIRE(graph.parameters.size() == 6);
    CHECK(graph.parameters[0].name == "Speed");
    CHECK(graph.parameters[0].kind == GraphParameterKind::Number);
    CHECK(graph.parameters[0].from == "CharacterBody.Speed");
    CHECK(graph.parameters[3].name == "Grounded");
    CHECK(graph.parameters[3].kind == GraphParameterKind::Boolean);
    CHECK(graph.parameters[3].rest == 1.0f);
    CHECK(graph.parameters[4].name == "Attack");
    CHECK(graph.parameters[4].kind == GraphParameterKind::Trigger);
    CHECK(graph.parameters[4].rest == 0.0f);
    CHECK(graph.parameters[4].from == "Attribute.Attack");
    CHECK(graph.parameters[5].name == "Aim");
    CHECK(graph.parameters[5].from.empty());
    CHECK(graph.parameterNamed("MoveZ") == 2);
    CHECK(graph.parameterNamed("Nothing") == -1);

    REQUIRE(graph.layers.size() == 3);
    CHECK(graph.layerNamed("Arms") == 1);
    CHECK(graph.layerNamed("Legs") == -1);

    // Every use of a clip, flattened: Move's three, Strafe's five, Jump, Fall,
    // Slash, AimUp.
    REQUIRE(graph.clips.size() == 12);

    const asset::GraphLayer& body = graph.layers[0];
    CHECK(body.name == "Body");
    CHECK(body.mask.empty());
    CHECK_FALSE(body.additive);
    CHECK(body.weight == 1.0f);
    CHECK(body.weightParameter == -1);
    CHECK(body.start == 0);
    REQUIRE(body.states.size() == 4);

    const asset::GraphState& move = body.states[0];
    CHECK(move.name == "Move");
    CHECK(move.kind == GraphStateKind::Blend1);
    CHECK(move.sync);
    CHECK(move.loop);
    CHECK(move.speed == 1.0f);
    CHECK(move.blendX == 0);
    CHECK(move.blendY == -1);
    CHECK(move.firstClip == 0);
    CHECK(move.clipCount == 3);
    CHECK(graph.clips[0].clip == "Idle");
    CHECK(graph.clips[1].clip == "Walk");
    CHECK(static_cast<double>(graph.clips[1].at[0]) == doctest::Approx(1.6));
    CHECK(graph.clips[2].clip == "Run");
    CHECK(graph.clips[2].layer == 0);
    CHECK(graph.clips[2].state == 0);

    const asset::GraphState& strafe = body.states[1];
    CHECK(strafe.kind == GraphStateKind::Blend2);
    CHECK(strafe.blendX == 1);
    CHECK(strafe.blendY == 2);
    CHECK(strafe.firstClip == 3);
    CHECK(strafe.clipCount == 5);
    CHECK(graph.clips[6].clip == "Left");
    CHECK(graph.clips[6].at[0] == -1.0f);
    CHECK(graph.clips[6].at[1] == 0.0f);
    CHECK(graph.clips[6].state == 1);
    // A cross: four triangles, each with the idle in the middle as a corner.
    REQUIRE(strafe.triangles.size() == 4);
    for (const std::array<core::u32, 3>& triangle : strafe.triangles)
        CHECK(has(triangle, 0));

    CHECK(body.states[2].name == "Jump");
    CHECK(body.states[2].kind == GraphStateKind::Clip);
    CHECK_FALSE(body.states[2].loop);
    CHECK(body.states[2].firstClip == 8);
    CHECK(body.states[2].clipCount == 1);
    CHECK(body.states[3].firstClip == 9);

    REQUIRE(body.transitions.size() == 3);
    CHECK(body.transitions[0].from == 0);
    CHECK(body.transitions[0].to == 2);
    REQUIRE(body.transitions[0].when.size() == 1);
    CHECK(body.transitions[0].when[0].parameter == 3);
    CHECK(body.transitions[0].when[0].op == GraphOp::Equal);
    CHECK(body.transitions[0].when[0].value == 0.0f);
    CHECK(body.transitions[0].after == -1.0f);
    CHECK(static_cast<double>(body.transitions[0].fade) == doctest::Approx(0.1));
    CHECK(body.transitions[1].from == 2);
    CHECK(body.transitions[1].to == 3);
    CHECK(body.transitions[1].when.empty());
    CHECK(body.transitions[1].after == 1.0f);
    CHECK(body.transitions[2].from == -1);
    CHECK(body.transitions[2].to == 0);
    CHECK(body.transitions[2].when[0].value == 1.0f);

    const asset::GraphLayer& arms = graph.layers[1];
    REQUIRE(arms.mask.size() == 1);
    CHECK(arms.mask[0] == "Spine1");
    CHECK(arms.start == 0);
    REQUIRE(arms.states.size() == 2);
    CHECK(arms.states[0].kind == GraphStateKind::None);
    CHECK(arms.states[0].clipCount == 0);
    CHECK(arms.states[1].firstClip == 10);
    CHECK(graph.clips[10].layer == 1);
    CHECK(graph.clips[10].state == 1);
    REQUIRE(arms.transitions.size() == 2);
    CHECK(arms.transitions[0].from == -1);
    CHECK(arms.transitions[0].to == 1);
    CHECK(arms.transitions[0].when[0].parameter == 4);
    CHECK(arms.transitions[0].when[0].op == GraphOp::IsSet);

    const asset::GraphLayer& lean = graph.layers[2];
    CHECK(lean.additive);
    CHECK(lean.weightParameter == 5);
    CHECK(graph.clips[11].clip == "AimUp");
    CHECK(graph.clips[11].layer == 2);

    // The events ride on the uses of their clips.
    REQUIRE(graph.clips[2].events.size() == 2);
    CHECK(graph.clips[2].events[0].at == 0.0f);
    CHECK(graph.clips[2].events[1].at == 0.5f);
    CHECK(graph.clips[2].events[1].name == "Step");
    REQUIRE(graph.clips[10].events.size() == 1);
    CHECK(graph.clips[10].events[0].name == "Hit");
    CHECK(graph.clips[0].events.empty());
}

TEST_CASE("an animation graph: what a file may also say")
{
    const std::string json =
        graphOf(layerWithState(R"({"blend":"Speed","speed":1.5,"loop":false,"clips":[)"
                               R"({"clip":"Run","at":5},{"clip":"Idle","at":0},{"clip":"Walk","at":1.6}]})"),
                R"("library":"asset://clips/humanoid.glb")");
    GraphReadError error;
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(json, &error);
    CAPTURE(error.where);
    REQUIRE(read.has_value());
    CHECK(read->library == "asset://clips/humanoid.glb");
    const asset::GraphState& state = read->layers[0].states[0];
    CHECK(state.speed == 1.5f);
    CHECK_FALSE(state.loop);
    CHECK_FALSE(state.sync);
    // A blend along one parameter is kept in the parameter's order, whatever
    // order the file wrote it in.
    REQUIRE(read->clips.size() == 3);
    CHECK(read->clips[0].clip == "Idle");
    CHECK(read->clips[1].clip == "Walk");
    CHECK(read->clips[2].clip == "Run");

    CHECK(asset::isAnimationGraphPath("characters/hero.animgraph.json"));
    CHECK_FALSE(asset::isAnimationGraphPath("characters/hero.json"));
    CHECK_FALSE(asset::isAnimationGraphPath(".animgraph.json"));
}

TEST_CASE("an animation graph: every operator a condition may use")
{
    const std::string json =
        graphOf(layerOf(transitionOf(R"("when":[["Speed","==",1],["Speed","~=",2],["Speed","!=",3],["Speed","<",4],)"
                                     R"(["Speed","<=",5],["Speed",">",6],["Speed",">=",7],["Grounded"],["Attack"]])")));
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(json);
    REQUIRE(read.has_value());
    const std::vector<asset::GraphCondition>& when = read->layers[0].transitions[0].when;
    REQUIRE(when.size() == 9);
    const std::array<GraphOp, 9> expected{GraphOp::Equal,        GraphOp::NotEqual,  GraphOp::NotEqual,
                                          GraphOp::Less,         GraphOp::LessEqual, GraphOp::Greater,
                                          GraphOp::GreaterEqual, GraphOp::IsSet,     GraphOp::IsSet};
    for (core::usize index = 0; index < when.size(); ++index) {
        CHECK(when[index].op == expected[index]);
        if (index < 7) {
            CHECK(when[index].value == static_cast<core::f32>(index + 1));
        }
    }
    CHECK(when[7].parameter == 1);
    CHECK(when[8].parameter == 2);
}

TEST_CASE("an animation graph: a file that is not one is refused as that")
{
    checkRefused("{ not json", GraphReadCode::Malformed, "");
    checkRefused("[]", GraphReadCode::NotAGraph, "");
    checkRefused(R"({"format":"material","version":1,"properties":{}})", GraphReadCode::NotAGraph, "format",
                 "material");
    checkRefused(R"({"version":1,"layers":[]})", GraphReadCode::NotAGraph, "format");
    checkRefused(R"({"format":"animgraph","layers":[]})", GraphReadCode::MissingKey, "version", "version");
    checkRefused(R"({"format":"animgraph","version":"1","layers":[]})", GraphReadCode::WrongType, "version");
    checkRefused(R"({"format":"animgraph","version":2,"layers":[]})", GraphReadCode::UnsupportedVersion, "version",
                 "2");
    checkRefused(R"({"format":"animgraph","version":1})", GraphReadCode::MissingKey, "layers", "layers");
    checkRefused(R"({"format":"animgraph","version":1,"layers":{}})", GraphReadCode::WrongType, "layers", "array");
    checkRefused(R"({"format":"animgraph","version":1,"layers":[]})", GraphReadCode::Empty, "layers");
    checkRefused(graphOf(layerOf(), R"("layer":[])"), GraphReadCode::UnknownKey, "layer", "layer");
    checkRefused(graphOf(layerOf(), R"("library":7)"), GraphReadCode::WrongType, "library", "string");

    // Nothing is left behind in the error of a read that worked.
    GraphReadError error;
    CHECK(asset::readAnimationGraph(graphOf(layerOf()), &error).has_value());
    CHECK(error.code == GraphReadCode::None);
    CHECK(error.where.empty());
    // And a caller that does not ask why is not owed a reason.
    CHECK_FALSE(asset::readAnimationGraph("[]").has_value());
}

TEST_CASE("an animation graph: a parameter that cannot be is refused where it is")
{
    const auto withParameters = [](std::string_view parameters) {
        std::string out = R"({"format":"animgraph","version":1,"parameters":)";
        out.append(parameters);
        out.append(R"(,"layers":[{"name":"L","start":"A","states":{"A":{}}}]})");
        return out;
    };
    CHECK(asset::readAnimationGraph(withParameters(R"({"Speed":{"number":2.5}})")).has_value());
    checkRefused(withParameters("[]"), GraphReadCode::WrongType, "parameters", "object");
    checkRefused(withParameters(R"({"Speed":3})"), GraphReadCode::WrongType, "parameters.Speed", "object");
    checkRefused(withParameters(R"({"Speed":{}})"), GraphReadCode::BadShape, "parameters.Speed", "Speed");
    checkRefused(withParameters(R"({"Speed":{"number":0,"boolean":true}})"), GraphReadCode::BadShape,
                 "parameters.Speed", "Speed");
    checkRefused(withParameters(R"({"Speed":{"number":true}})"), GraphReadCode::WrongType, "parameters.Speed.number",
                 "number");
    checkRefused(withParameters(R"({"On":{"boolean":1}})"), GraphReadCode::WrongType, "parameters.On.boolean",
                 "boolean");
    checkRefused(withParameters(R"({"Speed":{"number":1e60}})"), GraphReadCode::OutOfRange, "parameters.Speed.number",
                 "1e+60");
    checkRefused(withParameters(R"({"Speed":{"number":0,"form":"CharacterBody.Speed"}})"), GraphReadCode::UnknownKey,
                 "parameters.Speed.form", "form");
    checkRefused(withParameters(R"({"Speed":{"number":0},"Speed":{"number":1}})"), GraphReadCode::DuplicateName,
                 "parameters.Speed", "Speed");
    checkRefused(withParameters(R"({"":{"number":0}})"), GraphReadCode::Empty, "parameters.");

    // A source's shape, and no more than its shape.
    for (const std::string_view from :
         {"CharacterBody.Speed", "CharacterBody.VerticalSpeed", "CharacterBody.MoveX", "CharacterBody.MoveZ",
          "CharacterBody.Grounded", "CharacterBody.State", "Attribute.Attack", "Attribute.Anything at all"}) {
        CAPTURE(from);
        std::string parameters = R"({"P":{"number":0,"from":")";
        parameters.append(from);
        parameters.append(R"("}})");
        const std::optional<AnimationGraph> read = asset::readAnimationGraph(withParameters(parameters));
        REQUIRE(read.has_value());
        CHECK(read->parameters[0].from == from);
    }
    for (const std::string_view from : {"CharacterBody.Mood", "CharacterBody.", "Attribute.", "Speed", "Body.Speed"}) {
        std::string parameters = R"({"P":{"number":0,"from":")";
        parameters.append(from);
        parameters.append(R"("}})");
        checkRefused(withParameters(parameters), GraphReadCode::BadSource, "parameters.P.from", from);
    }
    checkRefused(withParameters(R"({"P":{"number":0,"from":""}})"), GraphReadCode::Empty, "parameters.P.from");
}

TEST_CASE("an animation graph: a layer that cannot be is refused where it is")
{
    checkRefused(graphOf("7"), GraphReadCode::WrongType, "layers[0]", "object");
    checkRefused(graphOf(R"({"start":"A","states":{"A":{}}})"), GraphReadCode::MissingKey, "layers[0].name", "name");
    checkRefused(graphOf(R"({"name":"L","states":{"A":{}}})"), GraphReadCode::MissingKey, "layers[0].start", "start");
    checkRefused(graphOf(R"({"name":"L","start":"A"})"), GraphReadCode::MissingKey, "layers[0].states", "states");
    checkRefused(graphOf(R"({"name":"","start":"A","states":{"A":{}}})"), GraphReadCode::Empty, "layers[0].name");
    checkRefused(graphOf(R"({"name":"L","start":"A","states":{}})"), GraphReadCode::Empty, "layers[0].states");
    checkRefused(graphOf(R"({"name":"L","start":"A","states":[]})"), GraphReadCode::WrongType, "layers[0].states",
                 "object");
    checkRefused(graphOf(R"({"name":"L","start":"A","states":{"A":{},"A":{}}})"), GraphReadCode::DuplicateName,
                 "layers[0].states.A", "A");
    checkRefused(graphOf(layerOf() + "," + layerOf()), GraphReadCode::DuplicateName, "layers[1].name", "Body");
    checkRefused(graphOf(R"({"name":"L","start":"Nowhere","states":{"A":{}}})"), GraphReadCode::UnknownState,
                 "layers[0].start", "Nowhere");
    checkRefused(graphOf(layerOf(R"("masks":[])")), GraphReadCode::UnknownKey, "layers[0].masks", "masks");
    checkRefused(graphOf(layerOf(R"("mask":"Spine")")), GraphReadCode::WrongType, "layers[0].mask", "array");
    checkRefused(graphOf(layerOf(R"("mask":["Spine",4])")), GraphReadCode::WrongType, "layers[0].mask[1]", "string");
    checkRefused(graphOf(layerOf(R"("additive":1)")), GraphReadCode::WrongType, "layers[0].additive", "boolean");

    // A weight is a number from 0 to 1, or a parameter that can be one.
    CHECK(asset::readAnimationGraph(graphOf(layerOf(R"("weight":0.25)"))).has_value());
    CHECK(asset::readAnimationGraph(graphOf(layerOf(R"("weight":"Grounded")"))).has_value());
    checkRefused(graphOf(layerOf(R"("weight":2)")), GraphReadCode::OutOfRange, "layers[0].weight", "2");
    checkRefused(graphOf(layerOf(R"("weight":-0.5)")), GraphReadCode::OutOfRange, "layers[0].weight", "-0.5");
    checkRefused(graphOf(layerOf(R"("weight":"Aim")")), GraphReadCode::UnknownParameter, "layers[0].weight", "Aim");
    checkRefused(graphOf(layerOf(R"("weight":"Attack")")), GraphReadCode::ParameterKind, "layers[0].weight", "Attack");
    checkRefused(graphOf(layerOf(R"("weight":true)")), GraphReadCode::WrongType, "layers[0].weight");

    // The second layer's errors say the second layer.
    checkRefused(graphOf(layerOf() + "," + layerOf(R"("start2":"A")", "Arms")), GraphReadCode::UnknownKey,
                 "layers[1].start2", "start2");
}

TEST_CASE("an animation graph: a state that cannot be is refused where it is")
{
    const std::string at = "layers[0].states.A";
    checkRefused(graphOf(layerWithState("3")), GraphReadCode::WrongType, at, "object");
    checkRefused(graphOf(layerWithState(R"({"clip":"Idle","loops":true})")), GraphReadCode::UnknownKey, at + ".loops",
                 "loops");
    checkRefused(graphOf(layerWithState(R"({"clip":7})")), GraphReadCode::WrongType, at + ".clip", "string");
    checkRefused(graphOf(layerWithState(R"({"clip":""})")), GraphReadCode::Empty, at + ".clip");
    checkRefused(graphOf(layerWithState(R"({"clip":"Idle","loop":1})")), GraphReadCode::WrongType, at + ".loop",
                 "boolean");
    checkRefused(graphOf(layerWithState(R"({"clip":"Idle","speed":-1})")), GraphReadCode::OutOfRange, at + ".speed",
                 "-1");
    checkRefused(graphOf(layerWithState(R"({"clip":"Idle","blend":"Speed","clips":[]})")), GraphReadCode::BadShape, at);
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed"})")), GraphReadCode::MissingKey, at + ".clips", "clips");
    checkRefused(graphOf(layerWithState(R"({"clips":[{"clip":"Idle","at":0}]})")), GraphReadCode::MissingKey,
                 at + ".blend", "blend");
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed","clips":[]})")), GraphReadCode::Empty, at + ".clips");
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed","clips":{}})")), GraphReadCode::WrongType, at + ".clips",
                 "array");
    checkRefused(graphOf(layerWithState(R"({"blend":"Pace","clips":[{"clip":"Idle","at":0}]})")),
                 GraphReadCode::UnknownParameter, at + ".blend", "Pace");
    checkRefused(graphOf(layerWithState(R"({"blend":4,"clips":[{"clip":"Idle","at":0}]})")), GraphReadCode::WrongType,
                 at + ".blend");
    checkRefused(graphOf(layerWithState(R"({"blend":["Speed"],"clips":[{"clip":"Idle","at":0}]})")),
                 GraphReadCode::WrongType, at + ".blend");

    // A blend slides along a number, and along nothing else.
    checkRefused(graphOf(layerWithState(R"({"blend":"Grounded","clips":[{"clip":"Idle","at":0}]})")),
                 GraphReadCode::ParameterKind, at + ".blend", "Grounded");
    checkRefused(graphOf(layerWithState(R"({"blend":["Speed","Attack"],"clips":[{"clip":"Idle","at":[0,0]}]})")),
                 GraphReadCode::ParameterKind, at + ".blend[1]", "Attack");
    checkRefused(graphOf(layerWithState(R"({"blend":["Speed","Pace"],"clips":[{"clip":"Idle","at":[0,0]}]})")),
                 GraphReadCode::UnknownParameter, at + ".blend[1]", "Pace");

    // `at` is a number along one parameter and a pair across two.
    checkRefused(
        graphOf(layerWithState(R"({"blend":"Speed","clips":[{"clip":"Idle","at":0},{"clip":"Run","at":[1,0]}]})")),
        GraphReadCode::WrongType, at + ".clips[1].at", "number");
    checkRefused(graphOf(layerWithState(R"({"blend":["Speed","Speed"],"clips":[{"clip":"Idle","at":0}]})")),
                 GraphReadCode::WrongType, at + ".clips[0].at", "[number, number]");
    checkRefused(graphOf(layerWithState(R"({"blend":["Speed","Speed"],"clips":[{"clip":"Idle","at":[0,"1"]}]})")),
                 GraphReadCode::WrongType, at + ".clips[0].at[1]", "number");
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed","clips":[{"clip":"Idle"}]})")), GraphReadCode::MissingKey,
                 at + ".clips[0].at", "at");
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed","clips":[{"at":0}]})")), GraphReadCode::MissingKey,
                 at + ".clips[0].clip", "clip");
    checkRefused(graphOf(layerWithState(R"({"blend":"Speed","clips":[{"clip":"Idle","at":0,"weight":1}]})")),
                 GraphReadCode::UnknownKey, at + ".clips[0].weight", "weight");
}

TEST_CASE("an animation graph: a transition that cannot be is refused where it is")
{
    const std::string at = "layers[0].transitions[0]";
    CHECK(asset::readAnimationGraph(graphOf(layerOf(transitionOf("")))).has_value());
    checkRefused(graphOf(layerOf(R"("transitions":{})")), GraphReadCode::WrongType, "layers[0].transitions", "array");
    checkRefused(graphOf(layerOf(R"("transitions":[{"to":"B"}])")), GraphReadCode::MissingKey, at + ".from", "from");
    checkRefused(graphOf(layerOf(R"("transitions":[{"from":"A"}])")), GraphReadCode::MissingKey, at + ".to", "to");
    checkRefused(graphOf(layerOf(R"("transitions":[{"from":"C","to":"B"}])")), GraphReadCode::UnknownState,
                 at + ".from", "C");
    checkRefused(graphOf(layerOf(R"("transitions":[{"from":"A","to":"C"}])")), GraphReadCode::UnknownState, at + ".to",
                 "C");
    // `*` is any state to leave, and not one to arrive at.
    checkRefused(graphOf(layerOf(R"("transitions":[{"from":"A","to":"*"}])")), GraphReadCode::UnknownState, at + ".to",
                 "*");
    checkRefused(graphOf(layerOf(transitionOf(R"("then":"B")"))), GraphReadCode::UnknownKey, at + ".then", "then");

    checkRefused(graphOf(layerOf(transitionOf(R"("fade":-0.1)"))), GraphReadCode::OutOfRange, at + ".fade", "-0.1");
    checkRefused(graphOf(layerOf(transitionOf(R"("fade":"slow")"))), GraphReadCode::WrongType, at + ".fade", "number");
    checkRefused(graphOf(layerOf(transitionOf(R"("fade":1e60)"))), GraphReadCode::OutOfRange, at + ".fade");
    checkRefused(graphOf(layerOf(transitionOf(R"("after":-1)"))), GraphReadCode::OutOfRange, at + ".after", "-1");

    checkRefused(graphOf(layerOf(transitionOf(R"("when":"Attack")"))), GraphReadCode::WrongType, at + ".when", "array");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":["Attack"])"))), GraphReadCode::WrongType, at + ".when[0]",
                 "array");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[["Speed",">"]])"))), GraphReadCode::BadShape, at + ".when[0]",
                 "2");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[[]])"))), GraphReadCode::BadShape, at + ".when[0]", "0");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[["Attack"],["Pace",">",1]])"))),
                 GraphReadCode::UnknownParameter, at + ".when[1][0]", "Pace");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[["Speed","=",1]])"))), GraphReadCode::BadOperator,
                 at + ".when[0][1]", "=");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[["Speed",1,1]])"))), GraphReadCode::WrongType,
                 at + ".when[0][1]", "string");
    checkRefused(graphOf(layerOf(transitionOf(R"("when":[["Speed","==","fast"]])"))), GraphReadCode::WrongType,
                 at + ".when[0][2]", "number or boolean");

    // A `*` that leads to itself is not an error: it is skipped while the
    // layer is in that state.
    const std::optional<AnimationGraph> star =
        asset::readAnimationGraph(graphOf(layerOf(R"("transitions":[{"from":"*","to":"A"}])")));
    REQUIRE(star.has_value());
    CHECK(star->layers[0].transitions[0].from == -1);
    CHECK(star->layers[0].transitions[0].to == 0);
}

TEST_CASE("an animation graph: events are sorted and ride on every use of their clip")
{
    const std::string layer = R"({"name":"Body","start":"A","states":{"A":{"clip":"Run"},"B":{"clip":"Idle"},)"
                              R"("C":{"blend":"Speed","clips":[{"clip":"Idle","at":0},{"clip":"Run","at":5}]}}})";
    const std::string events = R"("events":{"Run":[{"at":0.75,"name":"Right"},{"at":0.25,"name":"Left"},)"
                               R"({"at":0.25,"name":"Dust"},{"at":0,"name":"Start"},{"at":1,"name":"End"}]})";
    GraphReadError error;
    const std::optional<AnimationGraph> read = asset::readAnimationGraph(graphOf(layer, events), &error);
    CAPTURE(error.where);
    REQUIRE(read.has_value());
    REQUIRE(read->clips.size() == 4);
    for (const core::usize use : {core::usize{0}, core::usize{3}}) {
        const asset::GraphClip& clip = read->clips[use];
        CHECK(clip.clip == "Run");
        REQUIRE(clip.events.size() == 5);
        CHECK(clip.events[0].name == "Start");
        // Two at one moment keep the order they were written in.
        CHECK(clip.events[1].name == "Left");
        CHECK(clip.events[2].name == "Dust");
        CHECK(clip.events[3].name == "Right");
        CHECK(clip.events[4].name == "End");
        CHECK(clip.events[4].at == 1.0f);
    }
    CHECK(read->clips[1].events.empty());
    CHECK(read->clips[2].events.empty());

    checkRefused(graphOf(layerOf(), R"("events":[])"), GraphReadCode::WrongType, "events", "object");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":{}})"), GraphReadCode::WrongType, "events.Idle", "array");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"at":1.5,"name":"Late"}]})"), GraphReadCode::OutOfRange,
                 "events.Idle[0].at", "1.5");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"at":0,"name":"A"},{"at":-0.1,"name":"B"}]})"),
                 GraphReadCode::OutOfRange, "events.Idle[1].at", "-0.1");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"at":0.5}]})"), GraphReadCode::MissingKey,
                 "events.Idle[0].name", "name");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"name":"A"}]})"), GraphReadCode::MissingKey,
                 "events.Idle[0].at", "at");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"at":0.5,"name":""}]})"), GraphReadCode::Empty,
                 "events.Idle[0].name");
    checkRefused(graphOf(layerOf(), R"("events":{"Idle":[{"at":0.5,"name":"A","sound":"x"}]})"),
                 GraphReadCode::UnknownKey, "events.Idle[0].sound", "sound");
    // A moment of a clip that no state plays would never come.
    checkRefused(graphOf(layerOf(), R"("events":{"Jog":[{"at":0.5,"name":"Step"}]})"), GraphReadCode::UnknownClip,
                 "events.Jog", "Jog");
}

TEST_CASE("an animation graph: the triangles of a blend across two parameters")
{
    SUBCASE("a square with a point at its centre is four triangles round the centre")
    {
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(
            R"({"clip":"A","at":[-1,-1]},{"clip":"B","at":[1,-1]},{"clip":"C","at":[1,1]},{"clip":"D","at":[-1,1]},)"
            R"({"clip":"Centre","at":[0,0]})");
        REQUIRE(triangles.size() == 4);
        for (const std::array<core::u32, 3>& triangle : triangles)
            CHECK(has(triangle, 4));
        // Each side of the square is one triangle's.
        for (core::u32 corner = 0; corner < 4; ++corner) {
            const auto with = std::count_if(triangles.begin(), triangles.end(), [&](const auto& triangle) {
                return has(triangle, corner) && has(triangle, (corner + 1) % 4);
            });
            CHECK(with == 1);
        }
    }
    SUBCASE("four points on one circle are two triangles, not four lying over each other")
    {
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(
            R"({"clip":"A","at":[0,0]},{"clip":"B","at":[1,0]},{"clip":"C","at":[1,1]},{"clip":"D","at":[0,1]})");
        REQUIRE(triangles.size() == 2);
        // The two share a diagonal, and between them hold every corner.
        int shared = 0;
        for (core::u32 corner = 0; corner < 4; ++corner) {
            const bool first = has(triangles[0], corner);
            const bool second = has(triangles[1], corner);
            CHECK((first || second));
            shared += first && second ? 1 : 0;
        }
        CHECK(shared == 2);
    }
    SUBCASE("a grid of nine covers its square once, whichever way each cell is cut")
    {
        std::string points;
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                points += std::string(points.empty() ? "" : ",") + R"({"clip":"P","at":[)" + std::to_string(x) + "," +
                          std::to_string(y) + "]}";
            }
        }
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(points);
        // Every cell's corners are on one circle, so each could be cut either
        // way: two triangles a cell, and their areas the square's.
        REQUIRE(triangles.size() == 8);
        double area = 0.0;
        for (const std::array<core::u32, 3>& triangle : triangles) {
            const auto x = [&](core::usize corner) { return static_cast<double>(triangle[corner] % 3); };
            const auto y = [&](core::usize corner) { return static_cast<double>(triangle[corner] / 3); };
            const double doubled = (x(1) - x(0)) * (y(2) - y(0)) - (y(1) - y(0)) * (x(2) - x(0));
            CHECK(doubled == doctest::Approx(1.0));
            area += doubled / 2.0;
        }
        CHECK(area == doctest::Approx(4.0));
    }
    SUBCASE("a triangle with a point inside it is three")
    {
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(
            R"({"clip":"A","at":[0,0]},{"clip":"B","at":[4,0]},{"clip":"C","at":[0,4]},{"clip":"In","at":[1,1]})");
        REQUIRE(triangles.size() == 3);
        for (const std::array<core::u32, 3>& triangle : triangles)
            CHECK(has(triangle, 3));
    }
    SUBCASE("a point on a side splits the side")
    {
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(
            R"({"clip":"A","at":[0,0]},{"clip":"Mid","at":[1,0]},{"clip":"B","at":[2,0]},{"clip":"Top","at":[1,1]})");
        REQUIRE(triangles.size() == 2);
        for (const std::array<core::u32, 3>& triangle : triangles) {
            CHECK(has(triangle, 1));
            CHECK(has(triangle, 3));
        }
    }
    SUBCASE("triangles wind counter-clockwise")
    {
        const std::vector<std::array<core::u32, 3>> triangles =
            trianglesOf(R"({"clip":"A","at":[0,0]},{"clip":"B","at":[0,1]},{"clip":"C","at":[1,0]})");
        REQUIRE(triangles.size() == 1);
        CHECK(triangles[0] == std::array<core::u32, 3>{0, 2, 1});
    }
    SUBCASE("two clips at one point do not make triangles that lie over each other")
    {
        const std::vector<std::array<core::u32, 3>> triangles = trianglesOf(
            R"({"clip":"A","at":[0,0]},{"clip":"B","at":[1,0]},{"clip":"C","at":[0,1]},{"clip":"Again","at":[0,0]})");
        REQUIRE(triangles.size() == 1);
        CHECK_FALSE(has(triangles[0], 3));
    }
    SUBCASE("fewer than three points, or points on one line, are none")
    {
        CHECK(trianglesOf(R"({"clip":"A","at":[0,0]})").empty());
        CHECK(trianglesOf(R"({"clip":"A","at":[0,0]},{"clip":"B","at":[1,0]})").empty());
        CHECK(trianglesOf(R"({"clip":"A","at":[0,0]},{"clip":"B","at":[1,1]},{"clip":"C","at":[3,3]})").empty());
        CHECK(trianglesOf(R"({"clip":"A","at":[2,2]},{"clip":"B","at":[2,2]},{"clip":"C","at":[2,2]})").empty());
    }
    SUBCASE("the same file gives the same triangles")
    {
        const std::string_view points =
            R"({"clip":"A","at":[0,0]},{"clip":"B","at":[1,0]},{"clip":"C","at":[1,1]},{"clip":"D","at":[0,1]},)"
            R"({"clip":"E","at":[2,0.5]},{"clip":"F","at":[-1,0.5]})";
        CHECK(trianglesOf(points) == trianglesOf(points));
        CHECK(trianglesOf(points).size() == 4);
    }
}
