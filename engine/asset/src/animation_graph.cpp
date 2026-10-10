#include "engine/asset/animation_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>

#include "engine/core/json.h"

namespace engine::asset {

namespace {

using core::f32;
using core::f64;
using core::i32;
using core::JsonType;
using core::JsonValue;
using core::u32;
using core::usize;

[[nodiscard]] std::string member(std::string_view path, std::string_view key)
{
    std::string out(path);
    if (!out.empty())
        out.push_back('.');
    out.append(key);
    return out;
}

[[nodiscard]] std::string element(std::string_view path, usize index)
{
    std::string out(path);
    out.push_back('[');
    out.append(std::to_string(index));
    out.push_back(']');
    return out;
}

[[nodiscard]] std::string numberText(f64 value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

// --- The triangles of a blend across two parameters ---------------------------

struct Point
{
    f64 x = 0.0;
    f64 y = 0.0;
};

[[nodiscard]] f64 cross(Point a, Point b, Point c) noexcept
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Whether some edge of `a` has all of `b` on its outer side. Both wind
// counter-clockwise; `slack` is an area, so two triangles that share an edge
// or a corner are apart.
[[nodiscard]] bool hasSeparatingEdge(const std::array<Point, 3>& a, const std::array<Point, 3>& b, f64 slack) noexcept
{
    for (usize edge = 0; edge < 3; ++edge) {
        const Point from = a[edge];
        const Point to = a[(edge + 1) % 3];
        if (cross(from, to, b[0]) <= slack && cross(from, to, b[1]) <= slack && cross(from, to, b[2]) <= slack)
            return true;
    }
    return false;
}

// **The Delaunay triangles of a handful of points**, by the definition: a
// triangle is one when no other point is inside the circle through its
// corners. Every triple is tried, in index order, which is the fourth power
// of a number that is five or nine -- and has no structure to get wrong.
//
// Points on one circle (a square's corners) pass that test in every
// combination, so a triangle is kept only if it does not lie over one kept
// before it: the first in index order wins, and the same file always gives
// the same triangles. That rule is also what makes two clips at one point
// harmless -- the second one's triangles lie over the first one's.
[[nodiscard]] std::vector<std::array<u32, 3>> triangulate(std::span<const Point> points)
{
    std::vector<std::array<u32, 3>> out;
    const usize count = points.size();
    if (count < 3)
        return out;

    Point low = points[0];
    Point high = points[0];
    for (const Point point : points) {
        low.x = std::min(low.x, point.x);
        low.y = std::min(low.y, point.y);
        high.x = std::max(high.x, point.x);
        high.y = std::max(high.y, point.y);
    }
    const f64 extent = std::max(high.x - low.x, high.y - low.y);
    if (!(extent > 0.0))
        return out;
    // Relative to the points' own size, so a blend over 0..1 and one over
    // 0..600 are judged alike.
    const f64 areaSlack = extent * extent * 1e-9;

    std::vector<std::array<Point, 3>> kept;
    for (usize i = 0; i < count; ++i) {
        for (usize j = i + 1; j < count; ++j) {
            for (usize k = j + 1; k < count; ++k) {
                const f64 area = cross(points[i], points[j], points[k]);
                if (std::abs(area) <= areaSlack)
                    continue;

                // The circle through the three, from the first corner.
                const Point b{points[j].x - points[i].x, points[j].y - points[i].y};
                const Point c{points[k].x - points[i].x, points[k].y - points[i].y};
                const f64 b2 = b.x * b.x + b.y * b.y;
                const f64 c2 = c.x * c.x + c.y * c.y;
                const Point centre{(c.y * b2 - b.y * c2) / (2.0 * area), (b.x * c2 - c.x * b2) / (2.0 * area)};
                const f64 radius2 = centre.x * centre.x + centre.y * centre.y;
                const f64 slack = std::max(radius2, extent * extent) * 1e-9;

                bool empty = true;
                for (usize other = 0; other < count && empty; ++other) {
                    if (other == i || other == j || other == k)
                        continue;
                    const f64 dx = points[other].x - points[i].x - centre.x;
                    const f64 dy = points[other].y - points[i].y - centre.y;
                    empty = dx * dx + dy * dy >= radius2 - slack;
                }
                if (!empty)
                    continue;

                const std::array<u32, 3> indices =
                    area > 0.0 ? std::array<u32, 3>{static_cast<u32>(i), static_cast<u32>(j), static_cast<u32>(k)}
                               : std::array<u32, 3>{static_cast<u32>(i), static_cast<u32>(k), static_cast<u32>(j)};
                const std::array<Point, 3> corners{points[indices[0]], points[indices[1]], points[indices[2]]};
                bool clear = true;
                for (const std::array<Point, 3>& before : kept) {
                    if (!hasSeparatingEdge(corners, before, areaSlack) &&
                        !hasSeparatingEdge(before, corners, areaSlack)) {
                        clear = false;
                        break;
                    }
                }
                if (!clear)
                    continue;
                kept.push_back(corners);
                out.push_back(indices);
            }
        }
    }
    return out;
}

// --- The reader ----------------------------------------------------------------

constexpr std::array<std::string_view, 6> BodySources{"Speed", "VerticalSpeed", "MoveX", "MoveZ", "Grounded", "State"};
constexpr std::string_view BodyPrefix = "CharacterBody.";
constexpr std::string_view AttributePrefix = "Attribute.";

struct OperatorName
{
    std::string_view text;
    GraphOp op;
};

constexpr std::array<OperatorName, 7> Operators{
    OperatorName{"==", GraphOp::Equal},        OperatorName{"~=", GraphOp::NotEqual},
    OperatorName{"!=", GraphOp::NotEqual},     OperatorName{"<", GraphOp::Less},
    OperatorName{"<=", GraphOp::LessEqual},    OperatorName{">", GraphOp::Greater},
    OperatorName{">=", GraphOp::GreaterEqual},
};

[[nodiscard]] bool isSource(std::string_view from) noexcept
{
    if (from.starts_with(BodyPrefix)) {
        const std::string_view what = from.substr(BodyPrefix.size());
        return std::find(BodySources.begin(), BodySources.end(), what) != BodySources.end();
    }
    return from.starts_with(AttributePrefix) && from.size() > AttributePrefix.size();
}

class Reader
{
public:
    [[nodiscard]] bool read(const JsonValue& root);

    AnimationGraph graph;
    GraphReadError error;

private:
    // Always false, so a check reads `return fail(...)`.
    bool fail(GraphReadCode code, std::string where, std::string what = {})
    {
        error = GraphReadError{code, std::move(where), std::move(what)};
        return false;
    }

    // An object whose keys are all among `allowed`, each once.
    [[nodiscard]] bool keys(const JsonValue& object, std::string_view path, std::span<const std::string_view> allowed)
    {
        if (object.type() != JsonType::Object)
            return fail(GraphReadCode::WrongType, std::string(path), "object");
        for (usize index = 0; index < object.size(); ++index) {
            const std::string_view key = object.keyAt(index);
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                return fail(GraphReadCode::UnknownKey, member(path, key), std::string(key));
            for (usize before = 0; before < index; ++before) {
                if (object.keyAt(before) == key)
                    return fail(GraphReadCode::DuplicateName, member(path, key), std::string(key));
            }
        }
        return true;
    }

    // An object whose keys are names: any, but none empty and none twice.
    [[nodiscard]] bool names(const JsonValue& object, std::string_view path)
    {
        if (object.type() != JsonType::Object)
            return fail(GraphReadCode::WrongType, std::string(path), "object");
        for (usize index = 0; index < object.size(); ++index) {
            const std::string_view key = object.keyAt(index);
            if (key.empty())
                return fail(GraphReadCode::Empty, member(path, key));
            for (usize before = 0; before < index; ++before) {
                if (object.keyAt(before) == key)
                    return fail(GraphReadCode::DuplicateName, member(path, key), std::string(key));
            }
        }
        return true;
    }

    [[nodiscard]] bool present(const JsonValue& object, std::string_view path, std::string_view key)
    {
        return object.has(key) || fail(GraphReadCode::MissingKey, member(path, key), std::string(key));
    }

    [[nodiscard]] bool text(const JsonValue& json, std::string where, std::string& out)
    {
        if (json.type() != JsonType::String)
            return fail(GraphReadCode::WrongType, std::move(where), "string");
        if (json.asString().empty())
            return fail(GraphReadCode::Empty, std::move(where));
        out = std::string(json.asString());
        return true;
    }

    [[nodiscard]] bool flag(const JsonValue& json, std::string where, bool& out)
    {
        if (json.type() != JsonType::Boolean)
            return fail(GraphReadCode::WrongType, std::move(where), "boolean");
        out = json.asBool();
        return true;
    }

    [[nodiscard]] bool number(const JsonValue& json, std::string where, f32& out)
    {
        if (json.type() != JsonType::Number)
            return fail(GraphReadCode::WrongType, std::move(where), "number");
        const f64 value = json.asNumber();
        // Asked before the cast: a double no f32 holds is not one to convert.
        if (!(std::abs(value) <= static_cast<f64>(std::numeric_limits<f32>::max())))
            return fail(GraphReadCode::OutOfRange, std::move(where), numberText(value));
        out = static_cast<f32>(value);
        return true;
    }

    // A number that is neither negative nor above `most`.
    [[nodiscard]] bool within(const JsonValue& json, const std::string& where, f32 most, f32& out)
    {
        if (!number(json, where, out))
            return false;
        if (out < 0.0f || out > most)
            return fail(GraphReadCode::OutOfRange, where, numberText(json.asNumber()));
        return true;
    }

    [[nodiscard]] bool parameter(const JsonValue& json, std::string where, u32& out)
    {
        std::string name;
        if (!text(json, where, name))
            return false;
        const i32 found = graph.parameterNamed(name);
        if (found < 0)
            return fail(GraphReadCode::UnknownParameter, std::move(where), std::move(name));
        out = static_cast<u32>(found);
        return true;
    }

    [[nodiscard]] bool state(const GraphLayer& layer, const JsonValue& json, std::string where, u32& out)
    {
        std::string name;
        if (!text(json, where, name))
            return false;
        for (usize index = 0; index < layer.states.size(); ++index) {
            if (layer.states[index].name == name) {
                out = static_cast<u32>(index);
                return true;
            }
        }
        return fail(GraphReadCode::UnknownState, std::move(where), std::move(name));
    }

    [[nodiscard]] bool readParameters(const JsonValue& json);
    [[nodiscard]] bool readEvents(const JsonValue& json);
    [[nodiscard]] bool readLayer(const JsonValue& json, const std::string& path);
    [[nodiscard]] bool readState(const JsonValue& json, const std::string& path, u32 index, GraphState& out);
    [[nodiscard]] bool readTransition(const JsonValue& json, const std::string& path, const GraphLayer& layer,
                                      GraphTransition& out);

    struct ClipEvents
    {
        std::string clip;
        std::vector<GraphEvent> events;
        bool used = false;
    };
    std::vector<ClipEvents> m_events;
};

bool Reader::readParameters(const JsonValue& json)
{
    static constexpr std::array<std::string_view, 4> Keys{"number", "boolean", "trigger", "from"};
    const std::string_view path = "parameters";
    if (!names(json, path))
        return false;
    for (usize index = 0; index < json.size(); ++index) {
        GraphParameter out;
        out.name = std::string(json.keyAt(index));
        const std::string here = member(path, out.name);
        const JsonValue value = json[out.name];
        if (!keys(value, here, Keys))
            return false;

        const int kinds =
            (value.has("number") ? 1 : 0) + (value.has("boolean") ? 1 : 0) + (value.has("trigger") ? 1 : 0);
        if (kinds != 1)
            return fail(GraphReadCode::BadShape, here, out.name);
        if (value.has("number")) {
            out.kind = GraphParameterKind::Number;
            if (!number(value["number"], member(here, "number"), out.rest))
                return false;
        }
        else if (value.has("boolean")) {
            bool rest = false;
            out.kind = GraphParameterKind::Boolean;
            if (!flag(value["boolean"], member(here, "boolean"), rest))
                return false;
            out.rest = rest ? 1.0f : 0.0f;
        }
        else {
            // `"trigger": true` says what it is and nothing of its value: a
            // trigger is at rest until something fires it.
            bool marker = false;
            out.kind = GraphParameterKind::Trigger;
            if (!flag(value["trigger"], member(here, "trigger"), marker))
                return false;
        }
        if (value.has("from")) {
            if (!text(value["from"], member(here, "from"), out.from))
                return false;
            if (!isSource(out.from))
                return fail(GraphReadCode::BadSource, member(here, "from"), out.from);
        }
        graph.parameters.push_back(std::move(out));
    }
    return true;
}

bool Reader::readEvents(const JsonValue& json)
{
    static constexpr std::array<std::string_view, 2> Keys{"at", "name"};
    const std::string_view path = "events";
    if (!names(json, path))
        return false;
    for (usize index = 0; index < json.size(); ++index) {
        ClipEvents out;
        out.clip = std::string(json.keyAt(index));
        const std::string here = member(path, out.clip);
        const JsonValue list = json[out.clip];
        if (list.type() != JsonType::Array)
            return fail(GraphReadCode::WrongType, here, "array");
        for (usize item = 0; item < list.size(); ++item) {
            const std::string at = element(here, item);
            const JsonValue value = list.at(item);
            GraphEvent event;
            if (!keys(value, at, Keys) || !present(value, at, "at") || !present(value, at, "name") ||
                !within(value["at"], member(at, "at"), 1.0f, event.at) ||
                !text(value["name"], member(at, "name"), event.name))
                return false;
            out.events.push_back(std::move(event));
        }
        std::stable_sort(out.events.begin(), out.events.end(),
                         [](const GraphEvent& a, const GraphEvent& b) { return a.at < b.at; });
        m_events.push_back(std::move(out));
    }
    return true;
}

bool Reader::readState(const JsonValue& json, const std::string& path, u32 index, GraphState& out)
{
    static constexpr std::array<std::string_view, 6> Keys{"clip", "blend", "clips", "loop", "sync", "speed"};
    static constexpr std::array<std::string_view, 2> ClipKeys{"clip", "at"};
    if (!keys(json, path, Keys))
        return false;
    if (json.has("loop") && !flag(json["loop"], member(path, "loop"), out.loop))
        return false;
    if (json.has("sync") && !flag(json["sync"], member(path, "sync"), out.sync))
        return false;
    if (json.has("speed") && !within(json["speed"], member(path, "speed"), std::numeric_limits<f32>::max(), out.speed))
        return false;

    // The layer being read is not in the graph until it has read whole.
    const auto layer = static_cast<u32>(graph.layers.size());
    out.firstClip = static_cast<u32>(graph.clips.size());

    if (json.has("clip")) {
        if (json.has("blend") || json.has("clips"))
            return fail(GraphReadCode::BadShape, path, "clip, blend");
        GraphClip clip;
        clip.layer = layer;
        clip.state = index;
        if (!text(json["clip"], member(path, "clip"), clip.clip))
            return false;
        out.kind = GraphStateKind::Clip;
        out.clipCount = 1;
        graph.clips.push_back(std::move(clip));
        return true;
    }
    if (!json.has("blend")) {
        // `{}`, or one that only says how a clip it does not have would play.
        if (json.has("clips"))
            return fail(GraphReadCode::MissingKey, member(path, "blend"), "blend");
        out.kind = GraphStateKind::None;
        return true;
    }
    if (!present(json, path, "clips"))
        return false;

    const JsonValue blend = json["blend"];
    const std::string blendPath = member(path, "blend");
    u32 x = 0;
    u32 y = 0;
    if (blend.type() == JsonType::String) {
        out.kind = GraphStateKind::Blend1;
        if (!parameter(blend, blendPath, x))
            return false;
    }
    else if (blend.type() == JsonType::Array && blend.size() == 2) {
        out.kind = GraphStateKind::Blend2;
        if (!parameter(blend.at(0), element(blendPath, 0), x) || !parameter(blend.at(1), element(blendPath, 1), y))
            return false;
    }
    else {
        return fail(GraphReadCode::WrongType, blendPath, "string or [string, string]");
    }
    const bool two = out.kind == GraphStateKind::Blend2;
    // A blend slides between clips by how much; a boolean or a trigger has no
    // "between".
    if (graph.parameters[x].kind != GraphParameterKind::Number)
        return fail(GraphReadCode::ParameterKind, two ? element(blendPath, 0) : blendPath, graph.parameters[x].name);
    if (two && graph.parameters[y].kind != GraphParameterKind::Number)
        return fail(GraphReadCode::ParameterKind, element(blendPath, 1), graph.parameters[y].name);
    out.blendX = static_cast<i32>(x);
    out.blendY = two ? static_cast<i32>(y) : -1;

    const JsonValue list = json["clips"];
    const std::string listPath = member(path, "clips");
    if (list.type() != JsonType::Array)
        return fail(GraphReadCode::WrongType, listPath, "array");
    if (list.size() == 0)
        return fail(GraphReadCode::Empty, listPath);
    std::vector<GraphClip> clips;
    for (usize item = 0; item < list.size(); ++item) {
        const std::string here = element(listPath, item);
        const JsonValue value = list.at(item);
        GraphClip clip;
        clip.layer = layer;
        clip.state = index;
        if (!keys(value, here, ClipKeys) || !present(value, here, "clip") || !present(value, here, "at") ||
            !text(value["clip"], member(here, "clip"), clip.clip))
            return false;
        const JsonValue at = value["at"];
        const std::string atPath = member(here, "at");
        if (two) {
            if (at.type() != JsonType::Array || at.size() != 2)
                return fail(GraphReadCode::WrongType, atPath, "[number, number]");
            if (!number(at.at(0), element(atPath, 0), clip.at[0]) || !number(at.at(1), element(atPath, 1), clip.at[1]))
                return false;
        }
        else if (!number(at, atPath, clip.at[0])) {
            return false;
        }
        clips.push_back(std::move(clip));
    }

    if (two) {
        std::vector<Point> points;
        points.reserve(clips.size());
        for (const GraphClip& clip : clips)
            points.push_back(Point{static_cast<f64>(clip.at[0]), static_cast<f64>(clip.at[1])});
        out.triangles = triangulate(points);
    }
    else {
        // The evaluator walks them looking for the two either side of the
        // parameter; clips written at one value keep the file's order.
        std::stable_sort(clips.begin(), clips.end(),
                         [](const GraphClip& a, const GraphClip& b) { return a.at[0] < b.at[0]; });
    }
    out.clipCount = static_cast<u32>(clips.size());
    for (GraphClip& clip : clips)
        graph.clips.push_back(std::move(clip));
    return true;
}

bool Reader::readTransition(const JsonValue& json, const std::string& path, const GraphLayer& layer,
                            GraphTransition& out)
{
    static constexpr std::array<std::string_view, 5> Keys{"from", "to", "when", "after", "fade"};
    if (!keys(json, path, Keys) || !present(json, path, "from") || !present(json, path, "to"))
        return false;

    if (json["from"].type() == JsonType::String && json["from"].asString() == "*") {
        out.from = -1;
    }
    else {
        u32 from = 0;
        if (!state(layer, json["from"], member(path, "from"), from))
            return false;
        out.from = static_cast<i32>(from);
    }
    if (!state(layer, json["to"], member(path, "to"), out.to))
        return false;

    const f32 most = std::numeric_limits<f32>::max();
    if (json.has("after") && !within(json["after"], member(path, "after"), most, out.after))
        return false;
    if (json.has("fade") && !within(json["fade"], member(path, "fade"), most, out.fade))
        return false;

    if (!json.has("when"))
        return true;
    const JsonValue when = json["when"];
    const std::string whenPath = member(path, "when");
    if (when.type() != JsonType::Array)
        return fail(GraphReadCode::WrongType, whenPath, "array");
    for (usize item = 0; item < when.size(); ++item) {
        const std::string here = element(whenPath, item);
        const JsonValue value = when.at(item);
        if (value.type() != JsonType::Array)
            return fail(GraphReadCode::WrongType, here, "array");
        if (value.size() != 1 && value.size() != 3)
            return fail(GraphReadCode::BadShape, here, std::to_string(value.size()));
        GraphCondition condition;
        if (!parameter(value.at(0), element(here, 0), condition.parameter))
            return false;
        if (value.size() == 3) {
            const JsonValue op = value.at(1);
            if (op.type() != JsonType::String)
                return fail(GraphReadCode::WrongType, element(here, 1), "string");
            const auto found = std::find_if(Operators.begin(), Operators.end(),
                                            [&](const OperatorName& name) { return name.text == op.asString(); });
            if (found == Operators.end())
                return fail(GraphReadCode::BadOperator, element(here, 1), std::string(op.asString()));
            condition.op = found->op;

            const JsonValue against = value.at(2);
            if (against.type() == JsonType::Boolean)
                condition.value = against.asBool() ? 1.0f : 0.0f;
            else if (against.type() != JsonType::Number)
                return fail(GraphReadCode::WrongType, element(here, 2), "number or boolean");
            else if (!number(against, element(here, 2), condition.value))
                return false;
        }
        out.when.push_back(condition);
    }
    return true;
}

bool Reader::readLayer(const JsonValue& json, const std::string& path)
{
    static constexpr std::array<std::string_view, 7> Keys{"name",  "mask",   "additive",   "weight",
                                                          "start", "states", "transitions"};
    GraphLayer out;
    if (!keys(json, path, Keys) || !present(json, path, "name") || !present(json, path, "start") ||
        !present(json, path, "states") || !text(json["name"], member(path, "name"), out.name))
        return false;
    if (graph.layerNamed(out.name) >= 0)
        return fail(GraphReadCode::DuplicateName, member(path, "name"), out.name);

    if (json.has("mask")) {
        const JsonValue mask = json["mask"];
        const std::string maskPath = member(path, "mask");
        if (mask.type() != JsonType::Array)
            return fail(GraphReadCode::WrongType, maskPath, "array");
        for (usize item = 0; item < mask.size(); ++item) {
            std::string joint;
            if (!text(mask.at(item), element(maskPath, item), joint))
                return false;
            out.mask.push_back(std::move(joint));
        }
    }
    if (json.has("additive") && !flag(json["additive"], member(path, "additive"), out.additive))
        return false;
    if (json.has("weight")) {
        const JsonValue weight = json["weight"];
        const std::string weightPath = member(path, "weight");
        if (weight.type() == JsonType::String) {
            u32 found = 0;
            if (!parameter(weight, weightPath, found))
                return false;
            // A boolean is a layer switched on and off. A trigger is true for
            // one step, which is no weight at all.
            if (graph.parameters[found].kind == GraphParameterKind::Trigger)
                return fail(GraphReadCode::ParameterKind, weightPath, graph.parameters[found].name);
            out.weightParameter = static_cast<i32>(found);
        }
        else if (weight.type() != JsonType::Number) {
            return fail(GraphReadCode::WrongType, weightPath, "number or string");
        }
        else if (!within(weight, weightPath, 1.0f, out.weight)) {
            return false;
        }
    }

    const JsonValue states = json["states"];
    const std::string statesPath = member(path, "states");
    if (!names(states, statesPath))
        return false;
    if (states.size() == 0)
        return fail(GraphReadCode::Empty, statesPath);
    for (usize index = 0; index < states.size(); ++index) {
        GraphState made;
        made.name = std::string(states.keyAt(index));
        if (!readState(states[made.name], member(statesPath, made.name), static_cast<u32>(index), made))
            return false;
        out.states.push_back(std::move(made));
    }
    if (!state(out, json["start"], member(path, "start"), out.start))
        return false;

    if (json.has("transitions")) {
        const JsonValue transitions = json["transitions"];
        const std::string transitionsPath = member(path, "transitions");
        if (transitions.type() != JsonType::Array)
            return fail(GraphReadCode::WrongType, transitionsPath, "array");
        for (usize index = 0; index < transitions.size(); ++index) {
            GraphTransition transition;
            if (!readTransition(transitions.at(index), element(transitionsPath, index), out, transition))
                return false;
            out.transitions.push_back(std::move(transition));
        }
    }
    graph.layers.push_back(std::move(out));
    return true;
}

bool Reader::read(const JsonValue& root)
{
    static constexpr std::array<std::string_view, 6> Keys{"format",     "version", "library",
                                                          "parameters", "events",  "layers"};
    // What it is first, so another kind of file is told it is not a graph
    // rather than that its first key is unknown.
    if (root.type() != JsonType::Object)
        return fail(GraphReadCode::NotAGraph, "");
    if (root["format"].type() != JsonType::String || root["format"].asString() != AnimationGraphFormat)
        return fail(GraphReadCode::NotAGraph, "format", std::string(root["format"].asString()));
    if (!present(root, "", "version"))
        return false;
    if (root["version"].type() != JsonType::Number)
        return fail(GraphReadCode::WrongType, "version", "number");
    if (root["version"].asNumber() != static_cast<f64>(AnimationGraphVersion))
        return fail(GraphReadCode::UnsupportedVersion, "version", numberText(root["version"].asNumber()));
    if (!keys(root, "", Keys))
        return false;

    if (root.has("library")) {
        if (root["library"].type() != JsonType::String)
            return fail(GraphReadCode::WrongType, "library", "string");
        graph.library = std::string(root["library"].asString());
    }
    if (root.has("parameters") && !readParameters(root["parameters"]))
        return false;
    if (root.has("events") && !readEvents(root["events"]))
        return false;

    if (!present(root, "", "layers"))
        return false;
    const JsonValue layers = root["layers"];
    if (layers.type() != JsonType::Array)
        return fail(GraphReadCode::WrongType, "layers", "array");
    if (layers.size() == 0)
        return fail(GraphReadCode::Empty, "layers");
    for (usize index = 0; index < layers.size(); ++index) {
        if (!readLayer(layers.at(index), element("layers", index)))
            return false;
    }

    for (GraphClip& clip : graph.clips) {
        for (ClipEvents& events : m_events) {
            if (events.clip == clip.clip) {
                clip.events = events.events;
                events.used = true;
                break;
            }
        }
    }
    // A moment of a clip nothing plays would never fire, and is far more
    // often a name spelt two ways than something meant.
    for (const ClipEvents& events : m_events) {
        if (!events.used)
            return fail(GraphReadCode::UnknownClip, member("events", events.clip), events.clip);
    }
    return true;
}

} // namespace

bool isAnimationGraphPath(std::string_view path) noexcept
{
    return path.size() > AnimationGraphSuffix.size() && path.ends_with(AnimationGraphSuffix);
}

core::i32 AnimationGraph::parameterNamed(std::string_view name) const noexcept
{
    for (usize index = 0; index < parameters.size(); ++index) {
        if (parameters[index].name == name)
            return static_cast<i32>(index);
    }
    return -1;
}

core::i32 AnimationGraph::layerNamed(std::string_view name) const noexcept
{
    for (usize index = 0; index < layers.size(); ++index) {
        if (layers[index].name == name)
            return static_cast<i32>(index);
    }
    return -1;
}

std::optional<AnimationGraph> readAnimationGraph(std::string_view json, GraphReadError* error)
{
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "animgraph"); !parsed) {
        if (error != nullptr)
            *error = GraphReadError{GraphReadCode::Malformed, "", parsed.diagnostic};
        return std::nullopt;
    }
    Reader reader;
    if (!reader.read(document.root())) {
        if (error != nullptr)
            *error = std::move(reader.error);
        return std::nullopt;
    }
    return std::move(reader.graph);
}

} // namespace engine::asset
