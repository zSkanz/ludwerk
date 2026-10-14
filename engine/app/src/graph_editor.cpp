#include "engine/app/graph_editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "engine/platform/file.h"

namespace engine::app {

using core::f32;
using core::i32;
using core::u32;
using core::usize;

namespace {

// How far apart the canvas puts states a file has no place for.
constexpr f32 kStateStepX = 360.0f;
constexpr f32 kStateStepY = 170.0f;
// How long a new arrow's fade is: long enough to see, short enough to keep.
constexpr f32 kNewFade = 0.2f;

[[nodiscard]] PinType pinOf(asset::GraphWidth width) noexcept
{
    switch (width) {
    case asset::GraphWidth::Number:
        return PinType::Number;
    case asset::GraphWidth::Vector2:
        return PinType::Vector2;
    case asset::GraphWidth::Vector3:
        return PinType::Vector3;
    case asset::GraphWidth::Vector4:
        return PinType::Vector4;
    case asset::GraphWidth::Widest:
        break;
    }
    return PinType::Any;
}

[[nodiscard]] PinType pinOfWidth(u32 width) noexcept
{
    switch (width) {
    case 1:
        return PinType::Number;
    case 2:
        return PinType::Vector2;
    case 3:
        return PinType::Vector3;
    case 4:
        return PinType::Vector4;
    default:
        break;
    }
    return PinType::Any;
}

// Whether `text` has `filter` in it, whatever the case of either. Only the
// letters a keyboard's first row has are folded: a label in another script is
// matched as it is typed.
[[nodiscard]] bool hasIn(std::string_view text, std::string_view filter) noexcept
{
    if (filter.empty())
        return true;
    if (filter.size() > text.size())
        return false;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (usize at = 0; at + filter.size() <= text.size(); ++at) {
        usize same = 0;
        while (same < filter.size() && lower(text[at + same]) == lower(filter[same]))
            ++same;
        if (same == filter.size())
            return true;
    }
    return false;
}

[[nodiscard]] CanvasNode canvasNodeOf(const asset::SurfaceNodeKind& kind, std::string_view title, CanvasPoint at)
{
    CanvasNode node;
    node.kind = std::string(kind.name);
    node.title = std::string(title);
    node.x = at.x;
    node.y = at.y;
    for (const asset::SurfaceNodePin& pin : kind.inputs)
        node.inputs.push_back({std::string(pin.name), pinOf(pin.width)});
    for (const asset::SurfaceNodePin& pin : kind.outputs)
        node.outputs.push_back({std::string(pin.name), pinOf(pin.width)});
    for (const asset::SurfaceNodeField& field : kind.fields)
        node.fields.push_back({std::string(field.name), std::string(field.initial)});
    return node;
}

[[nodiscard]] std::optional<u32> pinNamed(const std::vector<CanvasPin>& pins, std::string_view name) noexcept
{
    for (u32 index = 0; index < pins.size(); ++index) {
        if (pins[index].name == name)
            return index;
    }
    return std::nullopt;
}

// Puts a graph's nodes and links on a canvas. `keepIds` is a file being
// opened; otherwise each node is given an id of the canvas's own, and
// `made` says which.
void put(NodeCanvas& canvas, const asset::SurfaceGraph& graph, bool keepIds, CanvasPoint shift,
         std::vector<std::pair<u32, u32>>* made)
{
    std::vector<std::pair<u32, u32>> ids;
    for (const asset::SurfaceGraphNode& node : graph.nodes) {
        const asset::SurfaceNodeKind* const kind = asset::surfaceNodeKind(node.kind);
        if (kind == nullptr)
            continue;
        CanvasNode added = canvasNodeOf(*kind, node.title, {node.x + shift.x, node.y + shift.y});
        for (CanvasField& field : added.fields)
            field.value = std::string(node.field(field.name));
        ids.emplace_back(node.id, canvas.add(std::move(added), keepIds ? node.id : 0));
    }
    const auto idOf = [&ids](u32 was) -> u32 {
        for (const auto& [from, to] : ids) {
            if (from == was)
                return to;
        }
        return 0;
    };
    for (const asset::SurfaceGraphLink& link : graph.links) {
        const CanvasNode* const from = canvas.node(idOf(link.from));
        const CanvasNode* const to = canvas.node(idOf(link.to));
        if (from == nullptr || to == nullptr)
            continue;
        const std::optional<u32> output = pinNamed(from->outputs, link.output);
        const std::optional<u32> input = pinNamed(to->inputs, link.input);
        if (output.has_value() && input.has_value())
            (void)canvas.link({from->id, *output, to->id, *input});
    }
    if (made != nullptr)
        *made = std::move(ids);
}

// A canvas's nodes -- all, or those selected -- as a graph's file has them.
[[nodiscard]] asset::SurfaceGraph graphOf(const NodeCanvas& canvas, bool selectedOnly)
{
    asset::SurfaceGraph graph;
    for (const CanvasNode& node : canvas.nodes()) {
        if (selectedOnly && !canvas.selected(node.id))
            continue;
        asset::SurfaceGraphNode out;
        out.id = node.id;
        out.kind = node.kind;
        out.title = node.title;
        // To the canvas's own unit: a pixel at a zoom of one, and nothing
        // finer is a place anybody put a node.
        out.x = std::round(node.x);
        out.y = std::round(node.y);
        for (const CanvasField& field : node.fields)
            out.fields.push_back({field.name, field.value});
        graph.nodes.push_back(std::move(out));
    }
    // **By id, not by the order they are drawn in**: a press brings a node
    // to the front, and a file that changed its order for that would be a
    // different file for no reason.
    std::sort(graph.nodes.begin(), graph.nodes.end(),
              [](const asset::SurfaceGraphNode& a, const asset::SurfaceGraphNode& b) { return a.id < b.id; });
    for (const CanvasLink& link : canvas.links()) {
        const CanvasNode* const from = canvas.node(link.from);
        const CanvasNode* const to = canvas.node(link.to);
        if (from == nullptr || to == nullptr || link.output >= from->outputs.size() || link.input >= to->inputs.size())
            continue;
        if (selectedOnly && (!canvas.selected(link.from) || !canvas.selected(link.to)))
            continue;
        graph.links.push_back({link.from, from->outputs[link.output].name, link.to, to->inputs[link.input].name});
    }
    std::sort(graph.links.begin(), graph.links.end(),
              [](const asset::SurfaceGraphLink& a, const asset::SurfaceGraphLink& b) {
                  if (a.to != b.to)
                      return a.to < b.to;
                  if (a.input != b.input)
                      return a.input < b.input;
                  return a.from != b.from ? a.from < b.from : a.output < b.output;
              });
    return graph;
}

} // namespace

// --- A material graph ----------------------------------------------------------

void SurfaceDocument::load(const asset::SurfaceGraph& graph, std::string_view graphFile)
{
    m_file = std::string(graphFile);
    canvas = NodeCanvas{};
    put(canvas, graph, true, {}, nullptr);
    canvas.commit();
    canvas.markSaved();
    refresh();
}

asset::SurfaceGraph SurfaceDocument::graph() const
{
    return graphOf(canvas, false);
}

void SurfaceDocument::refresh()
{
    const asset::SurfaceGraph held = graph();
    m_written = asset::writeSurfaceShader(held, m_file);
    for (const asset::SurfaceGraphNode& node : held.nodes) {
        const asset::SurfaceNodeKind* const kind = asset::surfaceNodeKind(node.kind);
        if (kind == nullptr)
            continue;
        for (u32 pin = 0; pin < kind->outputs.size(); ++pin) {
            if (kind->outputs[pin].width != asset::GraphWidth::Widest)
                continue;
            (void)canvas.setPinType(node.id, true, pin,
                                    pinOfWidth(asset::surfaceOutputWidth(held, node.id, kind->outputs[pin].name)));
        }
    }
}

u32 SurfaceDocument::nodeOfLine(u32 line) const noexcept
{
    return line >= 1 && line <= m_written.lineNodes.size() ? m_written.lineNodes[line - 1] : 0;
}

u32 SurfaceDocument::addNode(std::string_view kindName, CanvasPoint at)
{
    const asset::SurfaceNodeKind* const kind = asset::surfaceNodeKind(kindName);
    if (kind == nullptr)
        return 0;
    const u32 id = canvas.add(canvasNodeOf(*kind, {}, at));
    canvas.select(id);
    canvas.commit();
    refresh();
    return id;
}

bool SurfaceDocument::setField(u32 node, std::string_view name, std::string_view value)
{
    const CanvasNode* const held = canvas.node(node);
    if (held == nullptr)
        return false;
    if (const std::string* const was = held->field(name); was != nullptr && *was == value)
        return true;
    (void)canvas.setField(node, name, value);
    canvas.commit();
    refresh();
    return true;
}

bool SurfaceDocument::rename(u32 node, std::string_view title)
{
    const CanvasNode* const held = canvas.node(node);
    if (held == nullptr)
        return false;
    if (held->title == title)
        return true;
    (void)canvas.rename(node, title);
    canvas.commit();
    refresh();
    return true;
}

u32 SurfaceDocument::removeSelected()
{
    const u32 removed = canvas.removeSelected();
    if (removed != 0) {
        canvas.commit();
        refresh();
    }
    return removed;
}

u32 SurfaceDocument::duplicateSelected()
{
    const u32 copied = canvas.duplicateSelected();
    if (copied != 0) {
        canvas.commit();
        refresh();
    }
    return copied;
}

bool SurfaceDocument::undo()
{
    const bool done = canvas.undo();
    if (done)
        refresh();
    return done;
}

bool SurfaceDocument::redo()
{
    const bool done = canvas.redo();
    if (done)
        refresh();
    return done;
}

void SurfaceDocument::took(const CanvasEvent& event)
{
    // The canvas has kept its own step of undo for each of these already.
    if (event.kind == CanvasEvent::Kind::LinkMade || event.kind == CanvasEvent::Kind::NodesMoved)
        refresh();
}

std::vector<GraphChoice> SurfaceDocument::choices(std::string_view filter, const GraphLoose* loose,
                                                  const GraphLabel& label) const
{
    std::vector<GraphChoice> out;
    const CanvasNode* const held = loose != nullptr ? canvas.node(loose->node) : nullptr;
    for (const asset::SurfaceNodeKind& kind : asset::surfaceNodeKinds()) {
        i32 fits = -1;
        if (loose != nullptr) {
            if (held == nullptr)
                break;
            if (loose->fromOutput && loose->pin < held->outputs.size()) {
                const PinType carried = held->outputs[loose->pin].type;
                for (u32 pin = 0; pin < kind.inputs.size() && fits < 0; ++pin) {
                    if (pinAccepts(pinOf(kind.inputs[pin].width), carried))
                        fits = static_cast<i32>(pin);
                }
            }
            else if (!loose->fromOutput && loose->pin < held->inputs.size()) {
                const PinType taken = held->inputs[loose->pin].type;
                for (u32 pin = 0; pin < kind.outputs.size() && fits < 0; ++pin) {
                    if (pinAccepts(taken, pinOf(kind.outputs[pin].width)))
                        fits = static_cast<i32>(pin);
                }
            }
            if (fits < 0)
                continue;
        }
        const std::string shown = label ? label(kind.name) : std::string(kind.name);
        if (!hasIn(shown, filter) && !hasIn(kind.name, filter))
            continue;
        out.push_back({std::string(kind.name), std::string(kind.group), fits});
    }
    return out;
}

u32 SurfaceDocument::choose(const GraphChoice& choice, CanvasPoint at, const GraphLoose* loose)
{
    const asset::SurfaceNodeKind* const kind = asset::surfaceNodeKind(choice.kind);
    if (kind == nullptr)
        return 0;
    const u32 id = canvas.add(canvasNodeOf(*kind, {}, at));
    if (loose != nullptr && choice.pin >= 0) {
        const u32 pin = static_cast<u32>(choice.pin);
        (void)canvas.link(loose->fromOutput ? CanvasLink{loose->node, loose->pin, id, pin}
                                            : CanvasLink{id, pin, loose->node, loose->pin});
    }
    canvas.select(id);
    canvas.commit();
    refresh();
    return id;
}

std::string SurfaceDocument::copy() const
{
    if (canvas.selection().empty())
        return {};
    return asset::writeSurfaceGraph(graphOf(canvas, true));
}

u32 SurfaceDocument::paste(std::string_view text, CanvasPoint at)
{
    const std::optional<asset::SurfaceGraph> copied = asset::readSurfaceGraph(text);
    if (!copied.has_value() || copied->nodes.empty())
        return 0;
    // Its top-left corner goes where the pointer is.
    f32 left = copied->nodes[0].x;
    f32 top = copied->nodes[0].y;
    for (const asset::SurfaceGraphNode& node : copied->nodes) {
        left = std::min(left, node.x);
        top = std::min(top, node.y);
    }
    std::vector<std::pair<u32, u32>> made;
    put(canvas, *copied, false, {at.x - left, at.y - top}, &made);
    canvas.selectNone();
    for (const auto& [was, is] : made)
        canvas.select(is, true);
    canvas.commit();
    refresh();
    return static_cast<u32>(made.size());
}

// --- An animation graph's edits --------------------------------------------------

namespace graphedit {

namespace {

using asset::AnimationGraph;
using asset::GraphClip;
using asset::GraphLayer;
using asset::GraphState;
using asset::GraphStateKind;
using asset::GraphTransition;

// Every state's clips, by layer and state: the run the graph keeps, taken
// apart so a state or a layer can be put in or taken out of it.
using AllClips = std::vector<std::vector<std::vector<GraphClip>>>;

[[nodiscard]] AllClips takeAll(const AnimationGraph& graph)
{
    AllClips all(graph.layers.size());
    for (usize layer = 0; layer < graph.layers.size(); ++layer) {
        all[layer].resize(graph.layers[layer].states.size());
        for (usize state = 0; state < graph.layers[layer].states.size(); ++state) {
            const GraphState& held = graph.layers[layer].states[state];
            for (u32 index = 0; index < held.clipCount; ++index) {
                const usize at = static_cast<usize>(held.firstClip) + index;
                if (at < graph.clips.size())
                    all[layer][state].push_back(graph.clips[at]);
            }
        }
    }
    return all;
}

void putAll(AnimationGraph& graph, const AllClips& all)
{
    graph.clips.clear();
    for (usize layer = 0; layer < graph.layers.size() && layer < all.size(); ++layer) {
        for (usize state = 0; state < graph.layers[layer].states.size(); ++state) {
            GraphState& held = graph.layers[layer].states[state];
            held.firstClip = static_cast<u32>(graph.clips.size());
            held.clipCount = 0;
            held.triangles.clear();
            if (state >= all[layer].size())
                continue;
            for (GraphClip clip : all[layer][state]) {
                clip.layer = static_cast<u32>(layer);
                clip.state = static_cast<u32>(state);
                graph.clips.push_back(std::move(clip));
                held.clipCount += 1;
            }
        }
    }
}

// The first parameter that is a number, from `after` on; -1 for none.
[[nodiscard]] i32 numberParameter(const AnimationGraph& graph, i32 after) noexcept
{
    for (usize index = static_cast<usize>(std::max(after, 0)); index < graph.parameters.size(); ++index) {
        if (graph.parameters[index].kind == asset::GraphParameterKind::Number)
            return static_cast<i32>(index);
    }
    return -1;
}

} // namespace

std::vector<GraphClip> clipsOf(const AnimationGraph& graph, usize layer, u32 state)
{
    const AllClips all = takeAll(graph);
    return layer < all.size() && state < all[layer].size() ? all[layer][state] : std::vector<GraphClip>{};
}

void setClips(AnimationGraph& graph, usize layer, u32 state, std::vector<GraphClip> clips)
{
    AllClips all = takeAll(graph);
    if (layer >= all.size() || state >= all[layer].size())
        return;
    all[layer][state] = std::move(clips);
    putAll(graph, all);
}

void setKind(AnimationGraph& graph, usize layer, u32 state, GraphStateKind kind)
{
    if (layer >= graph.layers.size() || state >= graph.layers[layer].states.size())
        return;
    GraphState& held = graph.layers[layer].states[state];
    std::vector<GraphClip> clips = clipsOf(graph, layer, state);
    // A state that is to play something and has nothing plays the clip of
    // its own name: what a state is most often called after.
    if (kind != GraphStateKind::None && clips.empty()) {
        GraphClip clip;
        clip.clip = held.name;
        clips.push_back(std::move(clip));
    }
    switch (kind) {
    case GraphStateKind::None:
        clips.clear();
        held.blendX = -1;
        held.blendY = -1;
        break;
    case GraphStateKind::Clip:
        clips.resize(1);
        clips[0].at[0] = 0.0f;
        clips[0].at[1] = 0.0f;
        held.blendX = -1;
        held.blendY = -1;
        break;
    case GraphStateKind::Blend1:
        if (held.blendX < 0)
            held.blendX = numberParameter(graph, 0);
        held.blendY = -1;
        for (GraphClip& clip : clips)
            clip.at[1] = 0.0f;
        break;
    case GraphStateKind::Blend2:
        if (held.blendX < 0)
            held.blendX = numberParameter(graph, 0);
        if (held.blendY < 0)
            held.blendY = numberParameter(graph, held.blendX + 1);
        if (held.blendY < 0)
            held.blendY = held.blendX;
        break;
    }
    held.kind = kind;
    setClips(graph, layer, state, std::move(clips));
}

void removeState(AnimationGraph& graph, usize layer, u32 state)
{
    if (layer >= graph.layers.size() || state >= graph.layers[layer].states.size())
        return;
    AllClips all = takeAll(graph);
    all[layer].erase(all[layer].begin() + state);
    GraphLayer& held = graph.layers[layer];
    held.states.erase(held.states.begin() + state);
    // Every arrow that left it or entered it goes with it; the others name
    // their states by where they are, and those above it moved down one.
    const i32 gone = static_cast<i32>(state);
    std::erase_if(held.transitions,
                  [gone, state](const GraphTransition& arrow) { return arrow.from == gone || arrow.to == state; });
    for (GraphTransition& arrow : held.transitions) {
        if (arrow.from > gone)
            arrow.from -= 1;
        if (arrow.to > state)
            arrow.to -= 1;
    }
    if (held.start == state)
        held.start = 0;
    else if (held.start > state)
        held.start -= 1;
    putAll(graph, all);
}

void removeLayer(AnimationGraph& graph, usize layer)
{
    if (layer >= graph.layers.size())
        return;
    AllClips all = takeAll(graph);
    all.erase(all.begin() + static_cast<std::ptrdiff_t>(layer));
    graph.layers.erase(graph.layers.begin() + static_cast<std::ptrdiff_t>(layer));
    putAll(graph, all);
}

bool parameterUsed(const AnimationGraph& graph, u32 parameter) noexcept
{
    const i32 index = static_cast<i32>(parameter);
    for (const GraphLayer& layer : graph.layers) {
        if (layer.weightParameter == index)
            return true;
        for (const GraphState& state : layer.states) {
            const bool blends = state.kind == GraphStateKind::Blend1 || state.kind == GraphStateKind::Blend2;
            if (blends && (state.blendX == index || (state.kind == GraphStateKind::Blend2 && state.blendY == index)))
                return true;
        }
        for (const GraphTransition& arrow : layer.transitions) {
            for (const asset::GraphCondition& condition : arrow.when) {
                if (condition.parameter == parameter)
                    return true;
            }
        }
    }
    return false;
}

void removeParameter(AnimationGraph& graph, u32 parameter)
{
    if (parameter >= graph.parameters.size())
        return;
    graph.parameters.erase(graph.parameters.begin() + parameter);
    const i32 gone = static_cast<i32>(parameter);
    const auto shift = [gone](i32& index) {
        if (index > gone)
            index -= 1;
        else if (index == gone)
            index = -1;
    };
    for (GraphLayer& layer : graph.layers) {
        shift(layer.weightParameter);
        for (GraphState& state : layer.states) {
            shift(state.blendX);
            shift(state.blendY);
        }
        for (GraphTransition& arrow : layer.transitions) {
            for (asset::GraphCondition& condition : arrow.when) {
                if (condition.parameter > parameter)
                    condition.parameter -= 1;
                else if (condition.parameter == parameter)
                    condition.parameter = std::numeric_limits<u32>::max();
            }
        }
    }
}

std::string freeName(std::string_view stem, const std::vector<std::string>& taken)
{
    const auto has = [&taken](const std::string& name) {
        return std::find(taken.begin(), taken.end(), name) != taken.end();
    };
    std::string name(stem);
    for (u32 count = 2; has(name); ++count)
        name = std::string(stem) + " " + std::to_string(count);
    return name;
}

} // namespace graphedit

namespace {

[[nodiscard]] CanvasRules arrowRules() noexcept
{
    CanvasRules rules;
    rules.cycles = true;
    rules.manyIntoAnInput = true;
    rules.arrows = true;
    return rules;
}

[[nodiscard]] std::vector<std::string> stateNames(const asset::GraphLayer& layer)
{
    std::vector<std::string> names;
    for (const asset::GraphState& state : layer.states)
        names.push_back(state.name);
    return names;
}

// Where the first thing that still reads a parameter is, as the reader would
// say it.
[[nodiscard]] std::string firstUse(const asset::AnimationGraph& graph, u32 parameter)
{
    const i32 index = static_cast<i32>(parameter);
    for (usize layer = 0; layer < graph.layers.size(); ++layer) {
        const asset::GraphLayer& held = graph.layers[layer];
        const std::string here = "layers[" + std::to_string(layer) + "]";
        if (held.weightParameter == index)
            return here + ".weight";
        for (const asset::GraphState& state : held.states) {
            const bool blends =
                state.kind == asset::GraphStateKind::Blend1 || state.kind == asset::GraphStateKind::Blend2;
            if (blends &&
                (state.blendX == index || (state.kind == asset::GraphStateKind::Blend2 && state.blendY == index)))
                return here + ".states." + state.name + ".blend";
        }
        for (usize arrow = 0; arrow < held.transitions.size(); ++arrow) {
            for (usize condition = 0; condition < held.transitions[arrow].when.size(); ++condition) {
                if (held.transitions[arrow].when[condition].parameter == parameter)
                    return here + ".transitions[" + std::to_string(arrow) + "].when[" + std::to_string(condition) + "]";
            }
        }
    }
    return {};
}

} // namespace

// --- An animation graph ----------------------------------------------------------

AnimationDocument::AnimationDocument() : canvas(arrowRules())
{}

void AnimationDocument::load(asset::AnimationGraph graph)
{
    m_graph = std::move(graph);
    m_saved = m_graph;
    m_undo.assign(1, m_graph);
    m_redo.clear();
    m_refusal = {};
    m_page = 0;
    canvas = NodeCanvas(arrowRules());
    picture();
}

void AnimationDocument::setPage(usize layer)
{
    if (layer >= m_graph.layers.size() || layer == m_page)
        return;
    m_page = layer;
    canvas.selectNone();
    picture();
}

// The page, drawn again from the graph: every state under the number its
// place in the layer gives it, so what was selected is still selected.
void AnimationDocument::picture()
{
    const CanvasPoint pan = canvas.pan();
    const f32 zoom = canvas.zoom();
    const std::vector<u32> selection = canvas.selection();
    const std::optional<CanvasLink> arrow = canvas.selectedLink();
    canvas = NodeCanvas(arrowRules());
    canvas.setView(pan, zoom);
    m_anyNode = 0;
    if (m_graph.layers.empty())
        return;
    m_page = std::min(m_page, m_graph.layers.size() - 1);
    const asset::GraphLayer& layer = m_graph.layers[m_page];

    const u32 count = static_cast<u32>(layer.states.size());
    const u32 columns = std::max<u32>(1, static_cast<u32>(std::ceil(std::sqrt(static_cast<f32>(count)))));
    for (u32 state = 0; state < count; ++state) {
        const asset::GraphState& held = layer.states[state];
        CanvasNode node;
        node.kind = std::string(GraphStateNode);
        node.title = held.name;
        // One a file has no place for is put in a block with the others.
        node.x = held.place.set ? held.place.x : static_cast<f32>(state % columns) * kStateStepX;
        node.y = held.place.set ? held.place.y : static_cast<f32>(state / columns) * kStateStepY;
        node.inputs.push_back({"In", PinType::Flow});
        node.outputs.push_back({"Out", PinType::Flow});
        (void)canvas.add(std::move(node), state + 1);
    }
    const bool leaves = std::any_of(layer.transitions.begin(), layer.transitions.end(),
                                    [](const asset::GraphTransition& held) { return held.from < 0; });
    if (leaves || layer.anyPlace.set) {
        CanvasNode node;
        node.kind = std::string(GraphAnyNode);
        node.x = layer.anyPlace.set ? layer.anyPlace.x : -kStateStepX;
        node.y = layer.anyPlace.set ? layer.anyPlace.y : 0.0f;
        node.outputs.push_back({"Out", PinType::Flow});
        m_anyNode = canvas.add(std::move(node), count + 1);
    }
    // One arrow for however many transitions there are between two states.
    for (const asset::GraphTransition& held : layer.transitions) {
        const u32 from = held.from < 0 ? m_anyNode : static_cast<u32>(held.from) + 1;
        (void)canvas.link({from, 0, held.to + 1, 0});
    }
    for (const u32 id : selection)
        canvas.select(id, true);
    if (arrow.has_value())
        canvas.selectLink(*arrow);
}

bool AnimationDocument::edit(const std::function<void(asset::AnimationGraph&)>& change)
{
    asset::AnimationGraph next = m_graph;
    change(next);
    // Written and read back: the file's own reader says whether it is still
    // a graph, sorts what it sorts and works out what it works out.
    asset::GraphReadError error;
    std::optional<asset::AnimationGraph> read = asset::readAnimationGraph(asset::writeAnimationGraph(next), &error);
    if (!read.has_value()) {
        m_refusal = std::move(error);
        picture();
        return false;
    }
    m_refusal = {};
    if (!(*read == m_graph)) {
        m_graph = std::move(*read);
        m_undo.push_back(m_graph);
        m_redo.clear();
    }
    picture();
    return true;
}

bool AnimationDocument::undo()
{
    if (m_undo.size() < 2)
        return false;
    m_redo.push_back(std::move(m_undo.back()));
    m_undo.pop_back();
    m_graph = m_undo.back();
    m_refusal = {};
    picture();
    return true;
}

bool AnimationDocument::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(std::move(m_redo.back()));
    m_redo.pop_back();
    m_graph = m_undo.back();
    m_refusal = {};
    picture();
    return true;
}

std::optional<u32> AnimationDocument::stateOf(u32 node) const noexcept
{
    if (m_page >= m_graph.layers.size() || node == 0 || node > m_graph.layers[m_page].states.size())
        return std::nullopt;
    return node - 1;
}

bool AnimationDocument::reachable(u32 state) const
{
    if (m_page >= m_graph.layers.size())
        return false;
    const asset::GraphLayer& layer = m_graph.layers[m_page];
    std::vector<bool> reached(layer.states.size(), false);
    std::vector<u32> open;
    const auto reach = [&](u32 index) {
        if (index < reached.size() && !reached[index]) {
            reached[index] = true;
            open.push_back(index);
        }
    };
    reach(layer.start);
    // A "from any" arrow is taken from wherever the layer is.
    for (const asset::GraphTransition& held : layer.transitions) {
        if (held.from < 0)
            reach(held.to);
    }
    while (!open.empty()) {
        const u32 at = open.back();
        open.pop_back();
        for (const asset::GraphTransition& held : layer.transitions) {
            if (held.from == static_cast<i32>(at))
                reach(held.to);
        }
    }
    return state < reached.size() && reached[state];
}

std::vector<u32> AnimationDocument::transitionsBetween(i32 from, u32 to) const
{
    std::vector<u32> out;
    if (m_page >= m_graph.layers.size())
        return out;
    const asset::GraphLayer& layer = m_graph.layers[m_page];
    for (u32 index = 0; index < layer.transitions.size(); ++index) {
        const asset::GraphTransition& held = layer.transitions[index];
        if (held.to == to && (from < 0 ? held.from < 0 : held.from == from))
            out.push_back(index);
    }
    return out;
}

void AnimationDocument::took(const CanvasEvent& event)
{
    switch (event.kind) {
    case CanvasEvent::Kind::NodesMoved: {
        // Once one is moved by hand, every one is where it is drawn: a page
        // half placed would shift the rest the next time it was laid out.
        const std::vector<CanvasNode> nodes = canvas.nodes();
        const u32 any = m_anyNode;
        const usize page = m_page;
        (void)edit([&nodes, any, page](asset::AnimationGraph& graph) {
            asset::GraphLayer& layer = graph.layers[page];
            for (const CanvasNode& node : nodes) {
                if (node.id == any)
                    layer.anyPlace = {std::round(node.x), std::round(node.y), true};
                else if (node.id >= 1 && node.id <= layer.states.size())
                    layer.states[node.id - 1].place = {std::round(node.x), std::round(node.y), true};
            }
        });
        break;
    }
    case CanvasEvent::Kind::LinkMade: {
        const std::optional<u32> to = stateOf(event.link.to);
        const std::optional<u32> from = stateOf(event.link.from);
        if (to.has_value() && (from.has_value() || event.link.from == m_anyNode)) {
            const CanvasLink made = event.link;
            if (addTransition(from.has_value() ? static_cast<i32>(*from) : -1, *to))
                canvas.selectLink(made);
        }
        else {
            picture();
        }
        break;
    }
    case CanvasEvent::Kind::LinkRefused:
        // The arrow is there already: it is what was meant.
        if (event.refusal == LinkRefusal::AlreadyMade)
            canvas.selectLink(event.link);
        break;
    case CanvasEvent::Kind::None:
    case CanvasEvent::Kind::LinkDropped:
    case CanvasEvent::Kind::SelectionChanged:
        break;
    }
}

std::optional<u32> AnimationDocument::addState(CanvasPoint at)
{
    if (m_page >= m_graph.layers.size())
        return std::nullopt;
    const usize page = m_page;
    const std::string name = graphedit::freeName("State", stateNames(m_graph.layers[page]));
    const bool made = edit([&](asset::AnimationGraph& graph) {
        asset::GraphState state;
        state.name = name;
        state.place = {at.x, at.y, true};
        graph.layers[page].states.push_back(std::move(state));
    });
    if (!made)
        return std::nullopt;
    const u32 index = static_cast<u32>(m_graph.layers[page].states.size() - 1);
    canvas.select(nodeOf(index));
    return index;
}

bool AnimationDocument::addAny(CanvasPoint at)
{
    if (m_page >= m_graph.layers.size())
        return false;
    const usize page = m_page;
    const bool made = edit([&](asset::AnimationGraph& graph) { graph.layers[page].anyPlace = {at.x, at.y, true}; });
    if (made && m_anyNode != 0)
        canvas.select(m_anyNode);
    return made;
}

bool AnimationDocument::addTransition(i32 from, u32 to)
{
    if (m_page >= m_graph.layers.size())
        return false;
    const usize page = m_page;
    return edit([&](asset::AnimationGraph& graph) {
        asset::GraphTransition arrow;
        arrow.from = from;
        arrow.to = to;
        arrow.fade = kNewFade;
        graph.layers[page].transitions.push_back(std::move(arrow));
    });
}

bool AnimationDocument::removeSelected()
{
    if (m_page >= m_graph.layers.size())
        return false;
    const usize page = m_page;
    if (const std::optional<CanvasLink> arrow = canvas.selectedLink(); arrow.has_value()) {
        const std::optional<u32> to = stateOf(arrow->to);
        const std::optional<u32> from = stateOf(arrow->from);
        if (!to.has_value())
            return false;
        const i32 left = from.has_value() ? static_cast<i32>(*from) : -1;
        return edit([&](asset::AnimationGraph& graph) {
            std::erase_if(graph.layers[page].transitions, [&](const asset::GraphTransition& held) {
                return held.to == *to && (left < 0 ? held.from < 0 : held.from == left);
            });
        });
    }
    std::vector<u32> states;
    bool any = false;
    for (const u32 id : canvas.selection()) {
        if (const std::optional<u32> state = stateOf(id); state.has_value())
            states.push_back(*state);
        else if (id == m_anyNode)
            any = true;
    }
    if (states.empty() && !any)
        return false;
    // Said here: the reader would first miss the layer's `start`, which is
    // true and not what happened.
    if (states.size() == m_graph.layers[page].states.size()) {
        m_refusal =
            asset::GraphReadError{asset::GraphReadCode::Empty, "layers[" + std::to_string(page) + "].states", {}};
        return false;
    }
    // From the last, so each is still where its number says when it goes.
    std::sort(states.rbegin(), states.rend());
    const bool removed = edit([&](asset::AnimationGraph& graph) {
        for (const u32 state : states)
            graphedit::removeState(graph, page, state);
        if (any) {
            std::erase_if(graph.layers[page].transitions,
                          [](const asset::GraphTransition& held) { return held.from < 0; });
            graph.layers[page].anyPlace = {};
        }
    });
    if (removed)
        canvas.selectNone();
    return removed;
}

bool AnimationDocument::addLayer()
{
    std::vector<std::string> names;
    for (const asset::GraphLayer& layer : m_graph.layers)
        names.push_back(layer.name);
    const std::string name = graphedit::freeName("Layer", names);
    const bool made = edit([&name](asset::AnimationGraph& graph) {
        asset::GraphLayer layer;
        layer.name = name;
        // A layer has a state, and one that plays nothing lets what is under
        // it show: a new layer changes nothing until something is put on it.
        asset::GraphState state;
        state.name = "None";
        layer.states.push_back(std::move(state));
        graph.layers.push_back(std::move(layer));
    });
    if (made)
        setPage(m_graph.layers.size() - 1);
    return made;
}

bool AnimationDocument::removeLayer(usize layer)
{
    if (layer >= m_graph.layers.size())
        return false;
    const bool removed = edit([layer](asset::AnimationGraph& graph) { graphedit::removeLayer(graph, layer); });
    if (removed) {
        m_page = std::min(m_page, m_graph.layers.size() - 1);
        canvas.selectNone();
        picture();
    }
    return removed;
}

bool AnimationDocument::addParameter(asset::GraphParameterKind kind)
{
    std::vector<std::string> names;
    for (const asset::GraphParameter& parameter : m_graph.parameters)
        names.push_back(parameter.name);
    const std::string name = graphedit::freeName("Parameter", names);
    return edit([&](asset::AnimationGraph& graph) {
        asset::GraphParameter parameter;
        parameter.name = name;
        parameter.kind = kind;
        graph.parameters.push_back(std::move(parameter));
    });
}

bool AnimationDocument::removeParameter(u32 parameter)
{
    if (parameter >= m_graph.parameters.size())
        return false;
    // Said here, with the name: the reader would find the hole it left and
    // could only say that nothing is called nothing.
    if (graphedit::parameterUsed(m_graph, parameter)) {
        m_refusal = asset::GraphReadError{asset::GraphReadCode::UnknownParameter, firstUse(m_graph, parameter),
                                          m_graph.parameters[parameter].name};
        return false;
    }
    return edit([parameter](asset::AnimationGraph& graph) { graphedit::removeParameter(graph, parameter); });
}

std::vector<GraphChoice> AnimationDocument::choices(std::string_view filter, const GraphLoose* loose,
                                                    const GraphLabel& label) const
{
    std::vector<GraphChoice> out;
    const auto offer = [&](std::string_view kind, i32 pin) {
        const std::string shown = label ? label(kind) : std::string(kind);
        if (hasIn(shown, filter) || hasIn(kind, filter))
            out.push_back({std::string(kind), "States", pin});
    };
    // An arrow let go over nothing ends in a new state.
    offer(GraphStateNode, loose != nullptr ? 0 : -1);
    // One box of "from any" a page: every such arrow leaves the same one.
    if (loose == nullptr && m_anyNode == 0)
        offer(GraphAnyNode, -1);
    return out;
}

bool AnimationDocument::choose(const GraphChoice& choice, CanvasPoint at, const GraphLoose* loose)
{
    if (m_page >= m_graph.layers.size())
        return false;
    if (choice.kind == GraphAnyNode)
        return addAny(at);
    if (choice.kind != GraphStateNode)
        return false;
    if (loose == nullptr)
        return addState(at).has_value();
    // The state and the arrow to it are one edit, and one step of undo.
    const usize page = m_page;
    const std::optional<u32> left = stateOf(loose->node);
    if (!left.has_value() && loose->node != m_anyNode)
        return false;
    const std::string name = graphedit::freeName("State", stateNames(m_graph.layers[page]));
    const bool made = edit([&](asset::AnimationGraph& graph) {
        asset::GraphLayer& layer = graph.layers[page];
        asset::GraphState state;
        state.name = name;
        state.place = {at.x, at.y, true};
        layer.states.push_back(std::move(state));
        asset::GraphTransition arrow;
        arrow.from = left.has_value() ? static_cast<i32>(*left) : -1;
        arrow.to = static_cast<u32>(layer.states.size() - 1);
        arrow.fade = kNewFade;
        layer.transitions.push_back(std::move(arrow));
    });
    if (made)
        canvas.select(nodeOf(static_cast<u32>(m_graph.layers[page].states.size() - 1)));
    return made;
}

std::string AnimationDocument::copy() const
{
    if (m_page >= m_graph.layers.size())
        return {};
    const asset::GraphLayer& layer = m_graph.layers[m_page];
    std::vector<u32> states;
    for (u32 state = 0; state < layer.states.size(); ++state) {
        if (canvas.selected(nodeOf(state)))
            states.push_back(state);
    }
    if (states.empty())
        return {};
    const auto copyOf = [&states](u32 state) -> i32 {
        const auto found = std::find(states.begin(), states.end(), state);
        return found != states.end() ? static_cast<i32>(found - states.begin()) : -1;
    };
    // A graph of its own: every parameter, so what the states read by name
    // is said, and the one layer of what was selected.
    asset::AnimationGraph out;
    out.library = m_graph.library;
    out.parameters = m_graph.parameters;
    asset::GraphLayer page;
    page.name = layer.name;
    std::vector<std::vector<asset::GraphClip>> clips;
    for (const u32 state : states) {
        page.states.push_back(layer.states[state]);
        clips.push_back(graphedit::clipsOf(m_graph, m_page, state));
    }
    const bool any = m_anyNode != 0 && canvas.selected(m_anyNode);
    for (const asset::GraphTransition& held : layer.transitions) {
        const i32 to = copyOf(held.to);
        const i32 from = held.from < 0 ? -1 : copyOf(static_cast<u32>(held.from));
        if (to < 0 || (held.from < 0 ? !any : from < 0))
            continue;
        asset::GraphTransition arrow = held;
        arrow.from = from;
        arrow.to = static_cast<u32>(to);
        page.transitions.push_back(std::move(arrow));
    }
    if (any)
        page.anyPlace = layer.anyPlace;
    out.layers.push_back(std::move(page));
    for (u32 state = 0; state < clips.size(); ++state)
        graphedit::setClips(out, 0, state, std::move(clips[state]));
    return asset::writeAnimationGraph(out);
}

u32 AnimationDocument::paste(std::string_view text, CanvasPoint at)
{
    const std::optional<asset::AnimationGraph> copied = asset::readAnimationGraph(text);
    if (!copied.has_value() || copied->layers.empty() || m_page >= m_graph.layers.size())
        return 0;
    const asset::GraphLayer& from = copied->layers[0];
    const usize page = m_page;
    const u32 before = static_cast<u32>(m_graph.layers[page].states.size());

    // Its top-left corner goes where the pointer is; states the copy has no
    // place for are stepped down from there.
    f32 left = 0.0f;
    f32 top = 0.0f;
    bool placed = false;
    for (const asset::GraphState& state : from.states) {
        if (!state.place.set)
            continue;
        left = placed ? std::min(left, state.place.x) : state.place.x;
        top = placed ? std::min(top, state.place.y) : state.place.y;
        placed = true;
    }
    const bool made = edit([&](asset::AnimationGraph& graph) {
        // What the copy reads by name is found by name here, and added where
        // this graph has none of that name.
        std::vector<u32> parameters(copied->parameters.size(), 0);
        for (u32 index = 0; index < copied->parameters.size(); ++index) {
            const i32 found = graph.parameterNamed(copied->parameters[index].name);
            const bool read = graphedit::parameterUsed(*copied, index);
            if (found >= 0) {
                parameters[index] = static_cast<u32>(found);
            }
            else if (read) {
                parameters[index] = static_cast<u32>(graph.parameters.size());
                graph.parameters.push_back(copied->parameters[index]);
            }
        }
        const auto mapped = [&parameters](i32 index) -> i32 {
            return index >= 0 && static_cast<usize>(index) < parameters.size()
                       ? static_cast<i32>(parameters[static_cast<usize>(index)])
                       : -1;
        };
        asset::GraphLayer& layer = graph.layers[page];
        std::vector<std::string> names = stateNames(layer);
        const u32 first = static_cast<u32>(layer.states.size());
        std::vector<std::vector<asset::GraphClip>> clips;
        for (u32 index = 0; index < from.states.size(); ++index) {
            asset::GraphState state = from.states[index];
            state.name = graphedit::freeName(state.name, names);
            names.push_back(state.name);
            state.blendX = mapped(state.blendX);
            state.blendY = mapped(state.blendY);
            const f32 x = state.place.set ? state.place.x - left : 0.0f;
            const f32 y = state.place.set ? state.place.y - top : static_cast<f32>(index) * kStateStepY;
            state.place = {at.x + x, at.y + y, true};
            clips.push_back(graphedit::clipsOf(*copied, 0, index));
            layer.states.push_back(std::move(state));
        }
        for (const asset::GraphTransition& held : from.transitions) {
            asset::GraphTransition arrow = held;
            arrow.from = held.from < 0 ? -1 : held.from + static_cast<i32>(first);
            arrow.to = held.to + first;
            for (asset::GraphCondition& condition : arrow.when)
                condition.parameter = static_cast<u32>(std::max(mapped(static_cast<i32>(condition.parameter)), 0));
            layer.transitions.push_back(std::move(arrow));
        }
        for (u32 index = 0; index < clips.size(); ++index)
            graphedit::setClips(graph, page, first + index, std::move(clips[index]));
    });
    if (!made)
        return 0;
    const u32 after = static_cast<u32>(m_graph.layers[page].states.size());
    canvas.selectNone();
    for (u32 state = before; state < after; ++state)
        canvas.select(nodeOf(state), true);
    return after - before;
}

// --- The open documents --------------------------------------------------------

bool GraphEditor::opens(std::string_view path) noexcept
{
    return asset::isSurfaceGraphPath(path) || asset::isAnimationGraphPath(path);
}

std::optional<usize> GraphEditor::open(const std::filesystem::path& file, std::string_view relative, std::string* error)
{
    for (usize index = 0; index < m_documents.size(); ++index) {
        if (m_documents[index]->relative == relative) {
            m_focus = index;
            m_active = index;
            return index;
        }
    }
    const auto fail = [error](std::string why) -> std::optional<usize> {
        if (error != nullptr)
            *error = std::move(why);
        return std::nullopt;
    };
    std::string text;
    if (!platform::readTextFile(file, text))
        return fail({});

    auto document = std::make_unique<GraphDocument>();
    document->file = file;
    document->relative = std::string(relative);
    document->title = file.filename().generic_string();
    if (asset::isSurfaceGraphPath(relative)) {
        asset::SurfaceGraphReadError refused;
        const std::optional<asset::SurfaceGraph> graph = asset::readSurfaceGraph(text, &refused);
        if (!graph.has_value())
            return fail(refused.where + (refused.what.empty() ? "" : ": " + refused.what));
        document->kind = GraphKind::Surface;
        document->surface.load(*graph, document->title);
    }
    else if (asset::isAnimationGraphPath(relative)) {
        asset::GraphReadError refused;
        std::optional<asset::AnimationGraph> graph = asset::readAnimationGraph(text, &refused);
        if (!graph.has_value())
            return fail(refused.where + (refused.what.empty() ? "" : ": " + refused.what));
        document->kind = GraphKind::Animation;
        document->animation.load(std::move(*graph));
    }
    else {
        return fail({});
    }
    m_documents.push_back(std::move(document));
    m_active = m_documents.size() - 1;
    m_focus = m_active;
    return m_active;
}

bool GraphEditor::save(usize index)
{
    GraphDocument* const document = at(index);
    if (document == nullptr)
        return false;
    if (document->kind == GraphKind::Animation) {
        if (!platform::writeTextFile(document->file, asset::writeAnimationGraph(document->animation.graph())))
            return false;
        document->animation.markSaved();
        return true;
    }
    // The graph, and the shader it is for: a material names the shader, and
    // nothing downstream knows there was a graph.
    document->surface.refresh();
    const std::filesystem::path shader =
        document->file.parent_path() / asset::surfaceShaderPathOf(document->file.filename().generic_string());
    if (!platform::writeTextFile(document->file, asset::writeSurfaceGraph(document->surface.graph())) ||
        !platform::writeTextFile(shader, document->surface.written().hlsl))
        return false;
    document->surface.canvas.markSaved();
    return true;
}

void GraphEditor::close(usize index)
{
    if (index >= m_documents.size())
        return;
    m_documents.erase(m_documents.begin() + static_cast<std::ptrdiff_t>(index));
    m_focus.reset();
    if (m_documents.empty())
        m_active.reset();
    else if (m_active.has_value() && *m_active >= index && *m_active > 0)
        m_active = *m_active - 1;
}

GraphDocument* GraphEditor::at(usize index) noexcept
{
    return index < m_documents.size() ? m_documents[index].get() : nullptr;
}

const GraphDocument* GraphEditor::at(usize index) const noexcept
{
    return index < m_documents.size() ? m_documents[index].get() : nullptr;
}

void GraphEditor::setActive(usize index) noexcept
{
    if (index < m_documents.size())
        m_active = index;
}

std::optional<usize> GraphEditor::takeFocusRequest() noexcept
{
    const std::optional<usize> asked = m_focus;
    m_focus.reset();
    return asked;
}

std::string GraphEditor::newSurfaceGraph()
{
    // A graph that is a surface already: a colour a material can set, into
    // the surface's colour.
    asset::SurfaceGraph graph;
    asset::SurfaceGraphNode tint;
    tint.id = 1;
    tint.kind = "Parameter";
    tint.title = "Tint";
    tint.x = 0.0f;
    tint.y = 0.0f;
    tint.fields = {{"Name", "Tint"}, {"Type", "colour"}, {"Default", "1 1 1"}, {"Range", ""}};
    asset::SurfaceGraphNode end;
    end.id = 2;
    end.kind = std::string(asset::SurfaceEndNode);
    end.x = 320.0f;
    end.y = 0.0f;
    graph.nodes = {std::move(tint), std::move(end)};
    graph.links = {{1, "Out", 2, "BaseColor"}};
    return asset::writeSurfaceGraph(graph);
}

std::string GraphEditor::newAnimationGraph()
{
    asset::AnimationGraph graph;
    asset::GraphLayer layer;
    layer.name = "Body";
    asset::GraphState state;
    state.name = "Idle";
    layer.states.push_back(std::move(state));
    graph.layers.push_back(std::move(layer));
    return asset::writeAnimationGraph(graph);
}

} // namespace engine::app
