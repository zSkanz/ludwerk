// The graphs open in the editor (ADR 0211), with no window: a material graph
// as its canvas and the shader it writes -- compiled by the real compiler --
// and an animation graph whose every edit is decided by its file's reader.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "engine/app/graph_editor.h"
#include "engine/app/surface_compiler.h"
#include "engine/asset/content.h"
#include "engine/core/i18n.h"

using namespace engine;
using app::AnimationDocument;
using app::CanvasEvent;
using app::CanvasLink;
using app::CanvasMetrics;
using app::CanvasNode;
using app::CanvasPointer;
using app::GraphChoice;
using app::GraphEditor;
using app::GraphLoose;
using app::LinkRefusal;
using app::PinType;
using app::SurfaceDocument;
using core::f32;
using core::u32;

namespace {

struct Folder
{
    std::filesystem::path root;
    Folder()
    {
        root = std::filesystem::temp_directory_path() /
               ("engine-graph-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "content" / "shaders");
        std::filesystem::create_directories(root / "content" / "anim");
    }
    ~Folder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    void write(const std::string& relative, const std::string& text) const
    {
        std::ofstream out(root / "content" / relative, std::ios::binary);
        out << text;
    }
    [[nodiscard]] std::string read(const std::string& relative) const
    {
        std::ifstream in(root / "content" / relative, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

// The example's graph: two parameters across a blend, three layers, an arrow
// from any state.
[[nodiscard]] std::string heroGraph()
{
    const std::filesystem::path file = std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() /
                                       "examples/37-character/content/anim/hero.animgraph.json";
    std::ifstream in(file, std::ios::binary);
    // As a hand wrote it, before anybody placed a state: what these cases
    // are about is what the canvas does with a graph that says no places.
    return asset::withoutGraphPlaces(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
}

[[nodiscard]] AnimationDocument heroDocument()
{
    const std::optional<asset::AnimationGraph> graph = asset::readAnimationGraph(heroGraph());
    REQUIRE(graph.has_value());
    AnimationDocument document;
    document.load(*graph);
    return document;
}

[[nodiscard]] std::optional<u32> pin(const std::vector<app::CanvasPin>& pins, std::string_view name)
{
    for (u32 index = 0; index < pins.size(); ++index) {
        if (pins[index].name == name)
            return index;
    }
    return std::nullopt;
}

// A link by the pins' names, as the file has it.
LinkRefusal join(SurfaceDocument& document, u32 from, std::string_view output, u32 to, std::string_view input)
{
    const CanvasNode* const a = document.canvas.node(from);
    const CanvasNode* const b = document.canvas.node(to);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const std::optional<u32> out = pin(a->outputs, output);
    const std::optional<u32> in = pin(b->inputs, input);
    REQUIRE(out.has_value());
    REQUIRE(in.has_value());
    const LinkRefusal refusal = document.canvas.link({from, *out, to, *in});
    if (refusal == LinkRefusal::None) {
        document.canvas.commit();
        document.refresh();
    }
    return refusal;
}

[[nodiscard]] SurfaceDocument newSurface()
{
    const std::optional<asset::SurfaceGraph> graph = asset::readSurfaceGraph(GraphEditor::newSurfaceGraph());
    REQUIRE(graph.has_value());
    SurfaceDocument document;
    document.load(*graph, "new.surfacegraph.json");
    return document;
}

[[nodiscard]] bool has(const std::vector<GraphChoice>& choices, std::string_view kind)
{
    return std::any_of(choices.begin(), choices.end(), [kind](const GraphChoice& held) { return held.kind == kind; });
}

// A press, the frames between and a release, in the panel's pixels.
CanvasEvent drag(app::NodeCanvas& canvas, const CanvasMetrics& metrics, f32 fromX, f32 fromY, f32 toX, f32 toY)
{
    CanvasPointer at;
    at.x = fromX;
    at.y = fromY;
    at.down = true;
    at.pressed = true;
    (void)canvas.pointer(at, metrics);
    at.pressed = false;
    for (int step = 1; step <= 4; ++step) {
        at.x = fromX + (toX - fromX) * static_cast<f32>(step) / 4.0f;
        at.y = fromY + (toY - fromY) * static_cast<f32>(step) / 4.0f;
        (void)canvas.pointer(at, metrics);
    }
    at.down = false;
    at.released = true;
    return canvas.pointer(at, metrics);
}

[[nodiscard]] const asset::GraphLayer& bodyOf(const AnimationDocument& document)
{
    return document.graph().layers[0];
}

[[nodiscard]] std::string stateName(const AnimationDocument& document, core::i32 state)
{
    return state < 0 ? "*" : bodyOf(document).states[static_cast<core::usize>(state)].name;
}

} // namespace

TEST_CASE("ADR 0211: a new material graph is a surface already, and the canvas is its file")
{
    SurfaceDocument document = newSurface();
    REQUIRE(document.canvas.nodes().size() == 2);
    // Under the ids the file gave them.
    REQUIRE(document.canvas.node(1) != nullptr);
    REQUIRE(document.canvas.node(2) != nullptr);
    CHECK(document.canvas.node(1)->kind == "Parameter");
    CHECK(document.canvas.node(2)->kind == asset::SurfaceEndNode);
    CHECK(document.canvas.links().size() == 1);
    CHECK_FALSE(document.canvas.dirty());
    CHECK(document.written().issues.empty());
    CHECK(document.written().fragment);
    CHECK_FALSE(document.written().vertex);
    CHECK(document.written().hlsl.find("ENG_PARAM(float3, Tint") != std::string::npos);
    CHECK(document.written().hlsl.starts_with("// Written by new.surfacegraph.json"));

    // The file it would save is the file it was opened from.
    CHECK(asset::writeSurfaceGraph(document.graph()) == GraphEditor::newSurfaceGraph());

    // A press brings a node to the front; the file keeps its order.
    const u32 added = document.addNode("Time", {0.0f, 200.0f});
    CHECK(added == 3);
    CHECK(document.canvas.dirty());
    document.canvas.select(1);
    CanvasPointer press;
    press.x = 20.0f;
    press.y = 10.0f;
    press.down = true;
    press.pressed = true;
    (void)document.canvas.pointer(press, CanvasMetrics{});
    CHECK(document.canvas.nodes().back().id == 1);
    const asset::SurfaceGraph graph = document.graph();
    REQUIRE(graph.nodes.size() == 3);
    CHECK(graph.nodes[0].id == 1);
    CHECK(graph.nodes[2].id == 3);

    // A line of the shader is a node, or nobody's.
    CHECK(document.nodeOfLine(0) == 0);
    CHECK(document.nodeOfLine(100000) == 0);
    bool found = false;
    for (u32 line = 1; line <= document.written().lineNodes.size(); ++line)
        found = found || document.nodeOfLine(line) == 2;
    CHECK(found);
}

TEST_CASE("ADR 0211: a width that follows from what is put in is said on the pin, and a link that cannot be is refused")
{
    SurfaceDocument document = newSurface();
    const u32 multiply = document.addNode("Multiply", {160.0f, 0.0f});
    const u32 panner = document.addNode("Panner", {160.0f, 200.0f});
    // With nothing in it, nobody can say how wide it is.
    REQUIRE(document.canvas.node(multiply) != nullptr);
    CHECK(document.canvas.node(multiply)->outputs[0].type == PinType::Number);
    CHECK(join(document, 1, "Out", multiply, "A") == LinkRefusal::None);
    // Three wide in: three wide out, and not into two.
    CHECK(document.canvas.node(multiply)->outputs[0].type == PinType::Vector3);
    CHECK(join(document, multiply, "Out", panner, "Uv") == LinkRefusal::TypesDoNotFit);
    CHECK(join(document, multiply, "Out", 2, "Emissive") == LinkRefusal::None);
    CHECK(document.written().issues.empty());
    CHECK(document.written().hlsl.find("surface.Emissive = ") != std::string::npos);

    // One step back: the link, and the pin's width with it.
    CHECK(document.undo());
    CHECK(document.written().hlsl.find("surface.Emissive = ") == std::string::npos);
    CHECK(document.undo());
    CHECK(document.canvas.node(multiply)->outputs[0].type == PinType::Number);
    CHECK(document.redo());
    CHECK(document.canvas.node(multiply)->outputs[0].type == PinType::Vector3);
}

TEST_CASE(
    "ADR 0211: the list a node is added from is searched as it is typed, and a link in hand narrows it to what fits")
{
    SurfaceDocument document = newSurface();
    const app::GraphLabel shouted = [](std::string_view kind) {
        return kind == "OneMinus" ? std::string("One minus") : std::string(kind);
    };
    const std::vector<GraphChoice> all = document.choices("", nullptr, shouted);
    CHECK(all.size() == asset::surfaceNodeKinds().size());
    CHECK(all.front().kind == asset::surfaceNodeKinds().front().name);
    CHECK(all.front().pin == -1);

    // Typed in any case, against the word shown and the kind's own name.
    const std::vector<GraphChoice> typed = document.choices("MUL", nullptr, shouted);
    REQUIRE(typed.size() == 1);
    CHECK(typed[0].kind == "Multiply");
    CHECK(typed[0].group == "Math");
    CHECK(has(document.choices("one m", nullptr, shouted), "OneMinus"));
    CHECK(has(document.choices("oneminus", nullptr, shouted), "OneMinus"));
    CHECK(document.choices("no such node", nullptr, shouted).empty());

    // A colour's output in hand: what has an input three wide, or of any
    // width, and on which pin.
    const GraphLoose colour{1, 0, true};
    const std::vector<GraphChoice> fits = document.choices("", &colour, shouted);
    CHECK(has(fits, "Multiply"));
    CHECK(has(fits, "Fresnel"));
    CHECK(has(fits, "Surface"));
    CHECK_FALSE(has(fits, "Panner"));
    CHECK_FALSE(has(fits, "Time"));
    const auto fresnel =
        std::find_if(fits.begin(), fits.end(), [](const GraphChoice& held) { return held.kind == "Fresnel"; });
    REQUIRE(fresnel != fits.end());
    CHECK(fresnel->pin == 0);
    // And both at once.
    const std::vector<GraphChoice> both = document.choices("fres", &colour, shouted);
    REQUIRE(both.size() == 1);

    // An input in hand: what has an output it takes. The surface's metallic
    // is a number.
    const CanvasNode* const end = document.canvas.node(2);
    const GraphLoose metallic{2, *pin(end->inputs, "Metallic"), false};
    const std::vector<GraphChoice> numbers = document.choices("", &metallic, shouted);
    CHECK(has(numbers, "Time"));
    CHECK(has(numbers, "Dot"));
    CHECK(has(numbers, "Multiply"));
    CHECK_FALSE(has(numbers, "WorldNormal"));
    CHECK_FALSE(has(numbers, "Surface"));

    // Choosing adds the node and makes the link, as one step.
    const u32 before = static_cast<u32>(document.canvas.links().size());
    const u32 made = document.choose(*fresnel, {300.0f, 300.0f}, &colour);
    REQUIRE(made != 0);
    CHECK(document.canvas.node(made)->kind == "Fresnel");
    CHECK(document.canvas.selected(made));
    CHECK(document.canvas.links().size() == before + 1);
    CHECK(document.canvas.linkInto(made, 0).has_value());
    CHECK(document.undo());
    CHECK(document.canvas.node(made) == nullptr);
    CHECK(document.canvas.links().size() == before);
}

TEST_CASE("ADR 0211: a selection is copied as a graph's own text, and pasted into another graph")
{
    SurfaceDocument document = newSurface();
    const u32 multiply = document.addNode("Multiply", {160.0f, 40.0f});
    CHECK(join(document, 1, "Out", multiply, "A") == LinkRefusal::None);
    CHECK(join(document, multiply, "Out", 2, "BaseColor") == LinkRefusal::None);
    document.canvas.selectNone();
    CHECK(document.copy().empty());
    document.canvas.select(1);
    document.canvas.select(multiply, true);
    const std::string text = document.copy();
    // A graph's file, of the two nodes and the one link between them.
    const std::optional<asset::SurfaceGraph> copied = asset::readSurfaceGraph(text);
    REQUIRE(copied.has_value());
    CHECK(copied->nodes.size() == 2);
    REQUIRE(copied->links.size() == 1);
    CHECK(copied->links[0].input == "A");

    SurfaceDocument other = newSurface();
    CHECK(other.paste("not a graph", {0.0f, 0.0f}) == 0);
    CHECK(other.paste(text, {500.0f, 400.0f}) == 2);
    CHECK(other.canvas.nodes().size() == 4);
    CHECK(other.canvas.links().size() == 2);
    REQUIRE(other.canvas.selection().size() == 2);
    // Ids of its own, and where the pointer was.
    f32 left = 1.0e9f;
    f32 top = 1.0e9f;
    for (const u32 id : other.canvas.selection()) {
        CHECK(id > 2);
        left = std::min(left, other.canvas.node(id)->x);
        top = std::min(top, other.canvas.node(id)->y);
    }
    CHECK(std::fabs(left - 500.0f) < 1.0e-3f);
    CHECK(std::fabs(top - 400.0f) < 1.0e-3f);
    // Two parameters of one name and one kind are the one parameter.
    CHECK(other.written().issues.empty());
    CHECK(other.undo());
    CHECK(other.canvas.nodes().size() == 2);

    // And into the graph it came from.
    CHECK(document.paste(text, {0.0f, 300.0f}) == 2);
    CHECK(document.duplicateSelected() == 2);
    CHECK(document.removeSelected() == 2);
    CHECK(document.canvas.nodes().size() == 5);
}

TEST_CASE("ADR 0211: a material graph is saved as itself and as the shader beside it")
{
    const Folder folder;
    folder.write("shaders/rust.surfacegraph.json", GraphEditor::newSurfaceGraph());
    folder.write("shaders/broken.surfacegraph.json", R"({"format":"surfacegraph","version":1,"nodes":[{"id":1}]})");
    GraphEditor graphs;
    CHECK(GraphEditor::opens("shaders/rust.surfacegraph.json"));
    CHECK(GraphEditor::opens("anim/hero.animgraph.json"));
    CHECK_FALSE(GraphEditor::opens("shaders/rust.surface.hlsl"));

    std::string error;
    CHECK_FALSE(
        graphs
            .open(folder.root / "content/shaders/broken.surfacegraph.json", "shaders/broken.surfacegraph.json", &error)
            .has_value());
    CHECK_FALSE(error.empty());
    CHECK(graphs.count() == 0);

    const std::optional<core::usize> opened =
        graphs.open(folder.root / "content/shaders/rust.surfacegraph.json", "shaders/rust.surfacegraph.json");
    REQUIRE(opened.has_value());
    CHECK(graphs.takeFocusRequest() == opened);
    CHECK_FALSE(graphs.takeFocusRequest().has_value());
    // Opened again, it is the one that is open.
    CHECK(graphs.open(folder.root / "content/shaders/rust.surfacegraph.json", "shaders/rust.surfacegraph.json") ==
          opened);
    CHECK(graphs.count() == 1);

    app::GraphDocument* const document = graphs.at(*opened);
    REQUIRE(document != nullptr);
    CHECK(document->title == "rust.surfacegraph.json");
    CHECK_FALSE(document->dirty());
    const u32 time = document->surface.addNode("Time", {0.0f, 160.0f});
    CHECK(join(document->surface, time, "Out", 2, "Metallic") == LinkRefusal::None);
    CHECK(document->dirty());
    REQUIRE(graphs.save(*opened));
    CHECK_FALSE(document->dirty());

    const std::optional<asset::SurfaceGraph> saved =
        asset::readSurfaceGraph(folder.read("shaders/rust.surfacegraph.json"));
    REQUIRE(saved.has_value());
    CHECK(*saved == document->surface.graph());
    const std::string shader = folder.read("shaders/rust.surface.hlsl");
    CHECK(shader == document->surface.written().hlsl);
    // The first line says what wrote it, and that the graph is what to edit.
    CHECK(shader.starts_with("// Written by rust.surfacegraph.json -- edit the graph, not this file"));
    CHECK(shader.find("surface.Metallic = inputs.Time;") != std::string::npos);

    graphs.close(*opened);
    CHECK(graphs.count() == 0);
    CHECK_FALSE(graphs.active().has_value());
}

TEST_CASE("ADR 0211: every kind of node writes a shader the real compiler takes, and its error lands on the node")
{
    if (std::string_view(ENG_TEST_SHADERCROSS).empty() || !std::filesystem::exists(ENG_TEST_SHADERCROSS)) {
        MESSAGE("ENG_TEST_SKIP: no shader toolchain on this host");
        return;
    }
    REQUIRE(core::engineCatalog().loadFromFile(ENG_TEST_CATALOG).ok);
    const Folder folder;
    folder.write("shaders/every.surfacegraph.json", GraphEditor::newSurfaceGraph());
    GraphEditor graphs;
    const std::optional<core::usize> opened =
        graphs.open(folder.root / "content/shaders/every.surfacegraph.json", "shaders/every.surfacegraph.json");
    REQUIRE(opened.has_value());
    SurfaceDocument& document = graphs.at(*opened)->surface;

    // One of each width, to put into whatever has to be linked.
    const u32 number = document.addNode("Time", {-600.0f, 0.0f});
    const u32 two = document.addNode("Uv0", {-600.0f, 80.0f});
    const u32 three = document.addNode("WorldPosition", {-600.0f, 160.0f});
    const u32 four = document.addNode("VertexColor", {-600.0f, 240.0f});
    const auto sourceFor = [&](PinType type) {
        switch (type) {
        case PinType::Number:
            return number;
        case PinType::Vector2:
            return two;
        case PinType::Vector4:
            return four;
        default:
            return three;
        }
    };

    // Every kind once, what it must be given given to it, and its first
    // output measured: the lengths are summed into the surface's roughness,
    // so nothing is left out of the shader for being unused.
    u32 sum = 0;
    u32 expression = 0;
    u32 vertexNormal = 0;
    f32 y = 0.0f;
    for (const asset::SurfaceNodeKind& kind : asset::surfaceNodeKinds()) {
        if (kind.name == asset::SurfaceEndNode || kind.name == asset::VertexEndNode)
            continue;
        const u32 node = document.addNode(kind.name, {-300.0f, y});
        y += 120.0f;
        REQUIRE(node != 0);
        if (kind.name == "Expression") {
            expression = node;
            CHECK(document.setField(node, "Code", "a * b + 0.5"));
            CHECK(document.setField(node, "Type", "vector3"));
        }
        for (const asset::SurfaceNodePin& input : kind.inputs) {
            // Left unlinked where the kind says what it then reads -- that
            // is part of what is compiled here -- and where an expression
            // does not name the letter.
            if (!input.fallback.empty() || (kind.name == "Expression" && input.name != "a" && input.name != "b"))
                continue;
            const CanvasNode* const held = document.canvas.node(node);
            const PinType type = held->inputs[*pin(held->inputs, input.name)].type;
            CHECK(join(document, sourceFor(type), "Out", node, input.name) == LinkRefusal::None);
        }
        if (kind.stage == asset::GraphStage::VertexOnly) {
            vertexNormal = node;
            continue;
        }
        const u32 length = document.addNode("Length", {0.0f, y});
        CHECK(join(document, node, kind.outputs[0].name, length, "A") == LinkRefusal::None);
        if (sum == 0) {
            sum = length;
            continue;
        }
        const u32 add = document.addNode("Add", {200.0f, y});
        CHECK(join(document, sum, "Out", add, "A") == LinkRefusal::None);
        CHECK(join(document, length, "Out", add, "B") == LinkRefusal::None);
        sum = add;
    }
    REQUIRE(expression != 0);
    REQUIRE(vertexNormal != 0);
    const u32 clamped = document.addNode("Saturate", {400.0f, 0.0f});
    CHECK(join(document, sum, "Out", clamped, "A") == LinkRefusal::None);
    CHECK(join(document, clamped, "Out", 2, "Roughness") == LinkRefusal::None);
    // And the other function: a vertex moved along its normal by the wind.
    const u32 vertex = document.addNode(asset::VertexEndNode, {400.0f, 400.0f});
    const u32 wind = document.addNode("Wind", {0.0f, -200.0f});
    const u32 sway = document.addNode("Multiply", {200.0f, -200.0f});
    CHECK(join(document, vertexNormal, "Out", sway, "A") == LinkRefusal::None);
    CHECK(join(document, wind, "Out", sway, "B") == LinkRefusal::None);
    CHECK(join(document, sway, "Out", vertex, "Offset") == LinkRefusal::None);

    for (const asset::SurfaceGraphIssue& issue : document.written().issues)
        MESSAGE("issue at node " << issue.node << ": " << issue.where << " " << issue.what);
    REQUIRE(document.written().issues.empty());
    CHECK(document.written().vertex);
    CHECK(document.written().fragment);
    REQUIRE(graphs.save(*opened));

    asset::ContentMounts mounts;
    mounts.mountDirectory(folder.root / "content");
    app::SurfaceCompiler compiler(mounts, ENG_TEST_SHADERCROSS, ENG_TEST_SHADER_INCLUDE, folder.root / "cache");
    REQUIRE(compiler.available());
    const std::string urn = "asset://shaders/every.surface.hlsl";
    const render::SurfaceProgram* program = nullptr;
    const auto settle = [&] {
        (void)compiler.find(urn, rhi::ShaderFormat::SpirV, program);
        compiler.drain();
        return compiler.find(urn, rhi::ShaderFormat::SpirV, program);
    };
    const render::SurfaceStatus compiled = settle();
    for (const app::SurfaceError& error : compiler.errors(urn))
        MESSAGE("line " << error.line << ": " << error.message);
    if (compiled != render::SurfaceStatus::Ready)
        MESSAGE(document.written().hlsl);
    REQUIRE(compiled == render::SurfaceStatus::Ready);
    REQUIRE(program != nullptr);
    CHECK(program->reflection.param("Tint") != nullptr);

    // A line a person typed wrong: the compiler's error is on that node.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(document.setField(expression, "Code", "a * nothingCalledThis"));
    REQUIRE(graphs.save(*opened));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    REQUIRE(settle() == render::SurfaceStatus::Failed);
    const std::vector<app::SurfaceError> errors = compiler.errors(urn);
    REQUIRE_FALSE(errors.empty());
    CHECK(document.nodeOfLine(errors.front().line) == expression);
}

TEST_CASE("ADR 0211: an animation graph's page is a picture of its layer, and a layer is a page")
{
    AnimationDocument document = heroDocument();
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.canUndo());
    REQUIRE(document.graph().layers.size() == 3);
    CHECK(document.page() == 0);

    // Body: three states, the box "from any" arrows leave, and an arrow for
    // each pair of states with a transition between them.
    REQUIRE(document.canvas.nodes().size() == 4);
    CHECK(document.anyNode() == 4);
    CHECK(document.canvas.node(1)->title == "Move");
    CHECK(document.canvas.node(1)->kind == app::GraphStateNode);
    CHECK(document.canvas.node(4)->kind == app::GraphAnyNode);
    CHECK(document.stateOf(2) == 1u);
    CHECK_FALSE(document.stateOf(4).has_value());
    CHECK_FALSE(document.stateOf(0).has_value());
    CHECK(document.nodeOf(2) == 3);
    CHECK(document.canvas.links().size() == 4);
    CHECK(document.transitionsBetween(0, 1).size() == 1);
    CHECK(document.transitionsBetween(-1, 0).size() == 1);
    CHECK(document.transitionsBetween(1, 0).empty());
    // Every state of it can be come to.
    for (u32 state = 0; state < 3; ++state)
        CHECK(document.reachable(state));
    // Nobody placed them: they are laid out apart, and the file has no place.
    CHECK(document.canvas.node(1)->x != document.canvas.node(2)->x);
    CHECK_FALSE(bodyOf(document).states[0].place.set);

    // The arms, and the lean: a page each, with no such box.
    document.setPage(1);
    CHECK(document.page() == 1);
    CHECK(document.canvas.nodes().size() == 4);
    CHECK(document.anyNode() == 0);
    CHECK(document.canvas.links().size() == 5);
    document.setPage(2);
    CHECK(document.canvas.nodes().size() == 1);
    document.setPage(7);
    CHECK(document.page() == 2);
}

TEST_CASE("ADR 0211: an edit the graph's reader would not have is refused in its words, and nothing changed")
{
    AnimationDocument document = heroDocument();
    const asset::AnimationGraph before = document.graph();

    // Two states of one name.
    CHECK_FALSE(document.edit([](asset::AnimationGraph& graph) { graph.layers[0].states[1].name = "Move"; }));
    CHECK(document.refusal().code == asset::GraphReadCode::DuplicateName);
    CHECK(document.refusal().what == "Move");
    CHECK(document.graph() == before);
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.canUndo());
    // A fade that runs backwards.
    CHECK_FALSE(document.edit([](asset::AnimationGraph& graph) { graph.layers[0].transitions[0].fade = -1.0f; }));
    CHECK(document.refusal().code == asset::GraphReadCode::OutOfRange);
    CHECK(document.refusal().where == "layers[0].transitions[0].fade");
    // A blend along a trigger.
    CHECK_FALSE(document.edit(
        [](asset::AnimationGraph& graph) { graph.layers[0].states[0].blendX = graph.parameterNamed("Attack"); }));
    CHECK(document.refusal().code == asset::GraphReadCode::ParameterKind);
    // A parameter something still reads: said with its name and where.
    CHECK_FALSE(document.removeParameter(static_cast<u32>(before.parameterNamed("Grounded"))));
    CHECK(document.refusal().code == asset::GraphReadCode::UnknownParameter);
    CHECK(document.refusal().what == "Grounded");
    CHECK(document.refusal().where == "layers[0].transitions[0].when[0]");
    CHECK(document.graph() == before);

    // One that reads is the graph, a step of undo, and the refusal is gone.
    CHECK(document.edit([](asset::AnimationGraph& graph) { graph.layers[0].states[1].name = "Leap"; }));
    CHECK(document.refusal().code == asset::GraphReadCode::None);
    CHECK(document.dirty());
    CHECK(document.canvas.node(2)->title == "Leap");
    // The arrows name their states by where they are: they followed.
    CHECK(document.transitionsBetween(0, 1).size() == 1);
    CHECK(asset::writeAnimationGraph(document.graph()).find(R"("to": "Leap")") != std::string::npos);
    CHECK(document.undo());
    CHECK(document.graph() == before);
    CHECK_FALSE(document.dirty());
    CHECK(document.redo());
    CHECK(document.canvas.node(2)->title == "Leap");
}

TEST_CASE("ADR 0211: states and arrows are added on the canvas, and removed with what named them")
{
    AnimationDocument document = heroDocument();
    const CanvasMetrics metrics;

    // A new state: named, placed, and with no way in.
    const std::optional<u32> made = document.addState({600.0f, 300.0f});
    REQUIRE(made.has_value());
    CHECK(*made == 3);
    CHECK(bodyOf(document).states[3].name == "State");
    CHECK(bodyOf(document).states[3].kind == asset::GraphStateKind::None);
    CHECK(bodyOf(document).states[3].place == asset::GraphPlace{600.0f, 300.0f, true});
    CHECK(document.canvas.selected(document.nodeOf(3)));
    CHECK_FALSE(document.reachable(3));
    // The box "from any" arrows leave kept its number's worth: one past the states.
    CHECK(document.anyNode() == 5);
    // A second is called something else.
    REQUIRE(document.addState({600.0f, 500.0f}).has_value());
    CHECK(bodyOf(document).states[4].name == "State 2");
    CHECK(document.undo());

    // An arrow, drawn from Fall's edge to the new state.
    const app::CanvasRect fall = nodeRect(*document.canvas.node(document.nodeOf(2)), metrics);
    const app::CanvasRect fresh = nodeRect(*document.canvas.node(document.nodeOf(3)), metrics);
    const CanvasEvent drawn = drag(document.canvas, metrics, fall.x + fall.width, fall.y + fall.height * 0.5f,
                                   fresh.x + fresh.width * 0.5f, fresh.y + fresh.height * 0.5f);
    REQUIRE(drawn.kind == CanvasEvent::Kind::LinkMade);
    document.took(drawn);
    REQUIRE(document.transitionsBetween(2, 3).size() == 1);
    const asset::GraphTransition& arrow = bodyOf(document).transitions[document.transitionsBetween(2, 3)[0]];
    CHECK(arrow.when.empty());
    CHECK(arrow.fade > 0.0f);
    CHECK(document.reachable(3));
    // It is what is selected, to be given its conditions.
    REQUIRE(document.canvas.selectedLink().has_value());
    CHECK(document.canvas.selectedLink()->to == document.nodeOf(3));
    // Drawn again, it is the one there is.
    const CanvasEvent again = drag(document.canvas, metrics, fall.x + fall.width, fall.y + fall.height * 0.5f,
                                   fresh.x + fresh.width * 0.5f, fresh.y + fresh.height * 0.5f);
    document.took(again);
    CHECK(document.transitionsBetween(2, 3).size() == 1);

    // Moved by hand: every state of the page is then where it is drawn.
    const CanvasEvent moved =
        drag(document.canvas, metrics, fresh.x + fresh.width * 0.5f, fresh.y + fresh.height * 0.5f,
             fresh.x + fresh.width * 0.5f + 40.0f, fresh.y + fresh.height * 0.5f + 20.0f);
    REQUIRE(moved.kind == CanvasEvent::Kind::NodesMoved);
    document.took(moved);
    CHECK(bodyOf(document).states[3].place == asset::GraphPlace{640.0f, 320.0f, true});
    CHECK(bodyOf(document).states[0].place.set);
    CHECK(bodyOf(document).anyPlace.set);
    CHECK(asset::writeAnimationGraph(document.graph()).find("\"editor\"") != std::string::npos);

    // The arrow removed by itself.
    document.canvas.selectLink({document.nodeOf(2), 0, document.nodeOf(3), 0});
    CHECK(document.removeSelected());
    CHECK(document.transitionsBetween(2, 3).empty());
    CHECK(bodyOf(document).states.size() == 4);

    // Jump removed: the arrows into it and out of it go, and the rest still
    // name the states they named.
    document.canvas.select(document.nodeOf(1));
    CHECK(document.removeSelected());
    REQUIRE(bodyOf(document).states.size() == 3);
    CHECK(bodyOf(document).states[1].name == "Fall");
    REQUIRE(bodyOf(document).transitions.size() == 2);
    CHECK(stateName(document, bodyOf(document).transitions[0].from) == "Move");
    CHECK(stateName(document, static_cast<core::i32>(bodyOf(document).transitions[0].to)) == "Fall");
    CHECK(stateName(document, bodyOf(document).transitions[1].from) == "*");
    CHECK(stateName(document, static_cast<core::i32>(bodyOf(document).transitions[1].to)) == "Move");
    // Its clip went with it, and the others kept theirs.
    CHECK(app::graphedit::clipsOf(document.graph(), 0, 1).at(0).clip == "Fall");
    CHECK(app::graphedit::clipsOf(document.graph(), 0, 0).size() == 6);
    CHECK(app::graphedit::clipsOf(document.graph(), 1, 1).at(0).clip == "Slash");

    // The box of "from any" removed takes its arrows.
    document.canvas.select(document.anyNode());
    CHECK(document.removeSelected());
    CHECK(document.anyNode() == 0);
    CHECK(bodyOf(document).transitions.size() == 1);

    // The last state of a layer cannot go: a layer has one.
    document.setPage(2);
    document.canvas.select(document.nodeOf(0));
    CHECK_FALSE(document.removeSelected());
    CHECK(document.refusal().code == asset::GraphReadCode::Empty);
}

TEST_CASE("ADR 0211: what a state plays is changed whole, and the run of clips stays true")
{
    AnimationDocument document = heroDocument();
    // Fall, one clip, made a blend along the first number there is.
    CHECK(document.edit([](asset::AnimationGraph& graph) {
        app::graphedit::setKind(graph, 0, 2, asset::GraphStateKind::Blend1);
        std::vector<asset::GraphClip> clips = app::graphedit::clipsOf(graph, 0, 2);
        asset::GraphClip fast;
        fast.clip = "FallFast";
        fast.at[0] = 4.0f;
        clips.push_back(fast);
        asset::GraphClip slow;
        slow.clip = "Glide";
        slow.at[0] = -1.0f;
        clips.push_back(slow);
        app::graphedit::setClips(graph, 0, 2, std::move(clips));
    }));
    const asset::GraphState& fall = bodyOf(document).states[2];
    CHECK(fall.kind == asset::GraphStateKind::Blend1);
    CHECK(fall.blendX == document.graph().parameterNamed("Speed"));
    // Sorted along the parameter, as the reader has them.
    const std::vector<asset::GraphClip> clips = app::graphedit::clipsOf(document.graph(), 0, 2);
    REQUIRE(clips.size() == 3);
    CHECK(clips[0].clip == "Glide");
    CHECK(clips[1].clip == "Fall");
    CHECK(clips[2].clip == "FallFast");
    // The layers after it still play what they played.
    CHECK(app::graphedit::clipsOf(document.graph(), 1, 2).at(0).clip == "Shoot");
    CHECK(app::graphedit::clipsOf(document.graph(), 2, 0).at(0).clip == "AimUp");
    // And the events a clip has stay on it.
    CHECK(app::graphedit::clipsOf(document.graph(), 1, 1).at(0).events.size() == 1);

    // A state with nothing made to play one clip plays the clip of its name.
    REQUIRE(document.addState({0.0f, 400.0f}).has_value());
    CHECK(document.edit([](asset::AnimationGraph& graph) {
        graph.layers[0].states[3].name = "Land";
        app::graphedit::setKind(graph, 0, 3, asset::GraphStateKind::Clip);
    }));
    CHECK(app::graphedit::clipsOf(document.graph(), 0, 3).at(0).clip == "Land");
    // And back to nothing.
    CHECK(document.edit(
        [](asset::AnimationGraph& graph) { app::graphedit::setKind(graph, 0, 3, asset::GraphStateKind::None); }));
    CHECK(app::graphedit::clipsOf(document.graph(), 0, 3).empty());
    CHECK(app::graphedit::clipsOf(document.graph(), 1, 1).at(0).clip == "Slash");

    // Parameters: added under a name nothing has, and one nothing reads removed
    // from under the ones that are read.
    CHECK(document.addParameter(asset::GraphParameterKind::Boolean));
    CHECK(document.graph().parameters.back().name == "Parameter");
    CHECK(document.graph().parameters.back().kind == asset::GraphParameterKind::Boolean);
    CHECK(document.edit(
        [](asset::AnimationGraph& graph) { graph.layers[0].states[2].blendX = graph.parameterNamed("Rise"); }));
    const core::i32 speed = document.graph().parameterNamed("Speed");
    REQUIRE(speed == 0);
    CHECK_FALSE(app::graphedit::parameterUsed(document.graph(), 0));
    CHECK(document.removeParameter(0));
    CHECK(document.graph().parameterNamed("Speed") == -1);
    // What read MoveX reads MoveX.
    CHECK(bodyOf(document).states[0].blendX == document.graph().parameterNamed("MoveX"));
    CHECK(document.graph().layers[2].weightParameter == document.graph().parameterNamed("Aim"));
    const asset::GraphCondition& grounded = bodyOf(document).transitions[0].when[0];
    CHECK(document.graph().parameters[grounded.parameter].name == "Grounded");

    // Layers: a new one is a page with a state that plays nothing.
    CHECK(document.addLayer());
    CHECK(document.page() == 3);
    CHECK(document.graph().layers[3].name == "Layer");
    CHECK(document.canvas.nodes().size() == 1);
    CHECK(document.removeLayer(1));
    REQUIRE(document.graph().layers.size() == 3);
    CHECK(document.graph().layers[1].name == "Lean");
    CHECK(app::graphedit::clipsOf(document.graph(), 1, 0).at(0).clip == "AimUp");
    CHECK(document.page() == 2);
}

TEST_CASE("ADR 0211: an animation page's list offers a state, and the box of from any once")
{
    AnimationDocument document = heroDocument();
    const app::GraphLabel label = [](std::string_view kind) {
        return kind == app::GraphAnyNode ? std::string("From any state") : std::string(kind);
    };
    // Body has the box already.
    std::vector<GraphChoice> offered = document.choices("", nullptr, label);
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].kind == app::GraphStateNode);
    document.setPage(1);
    offered = document.choices("", nullptr, label);
    CHECK(offered.size() == 2);
    CHECK(document.choices("from any", nullptr, label).size() == 1);
    CHECK(document.choices("zzz", nullptr, label).empty());

    // The box, put down; then an arrow from it let go over nothing ends in a
    // new state, as one step.
    REQUIRE(document.choose(document.choices("any", nullptr, label)[0], {-300.0f, 200.0f}, nullptr));
    REQUIRE(document.anyNode() != 0);
    CHECK(document.choices("", nullptr, label).size() == 1);
    const GraphLoose loose{document.anyNode(), 0, true};
    offered = document.choices("", &loose, label);
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].pin == 0);
    const core::usize states = document.graph().layers[1].states.size();
    REQUIRE(document.choose(offered[0], {300.0f, 300.0f}, &loose));
    CHECK(document.graph().layers[1].states.size() == states + 1);
    CHECK(document.transitionsBetween(-1, static_cast<u32>(states)).size() == 1);
    CHECK(document.reachable(static_cast<u32>(states)));
    CHECK(document.undo());
    CHECK(document.graph().layers[1].states.size() == states);
    CHECK(document.transitionsBetween(-1, static_cast<u32>(states)).empty());
}

TEST_CASE("ADR 0211: states are copied as a graph's own text, and pasted into another graph with what they read")
{
    AnimationDocument document = heroDocument();
    // Move and Jump, with the arrow between them; Fall is left.
    document.canvas.select(document.nodeOf(0));
    document.canvas.select(document.nodeOf(1), true);
    const std::string text = document.copy();
    const std::optional<asset::AnimationGraph> copied = asset::readAnimationGraph(text);
    REQUIRE(copied.has_value());
    REQUIRE(copied->layers.size() == 1);
    CHECK(copied->layers[0].states.size() == 2);
    REQUIRE(copied->layers[0].transitions.size() == 1);
    CHECK(copied->layers[0].transitions[0].when.size() == 2);
    CHECK(app::graphedit::clipsOf(*copied, 0, 0).size() == 6);

    // Into a graph that has none of it.
    const std::optional<asset::AnimationGraph> empty = asset::readAnimationGraph(GraphEditor::newAnimationGraph());
    REQUIRE(empty.has_value());
    AnimationDocument other;
    other.load(*empty);
    CHECK(other.paste("{}", {0.0f, 0.0f}) == 0);
    CHECK(other.paste(text, {400.0f, 100.0f}) == 2);
    const asset::GraphLayer& layer = other.graph().layers[0];
    REQUIRE(layer.states.size() == 3);
    CHECK(layer.states[1].name == "Move");
    CHECK(layer.states[1].kind == asset::GraphStateKind::Blend2);
    CHECK(std::fabs(layer.states[1].place.x - 400.0f) < 1.0e-3f);
    // What they read came with them, by name: and nothing they do not read.
    CHECK(other.graph().parameterNamed("MoveX") >= 0);
    CHECK(other.graph().parameterNamed("Grounded") >= 0);
    CHECK(other.graph().parameterNamed("Rise") >= 0);
    CHECK(other.graph().parameterNamed("Attack") == -1);
    CHECK(layer.states[1].blendX == other.graph().parameterNamed("MoveX"));
    REQUIRE(layer.transitions.size() == 1);
    CHECK(layer.transitions[0].from == 1);
    CHECK(layer.transitions[0].to == 2);
    CHECK(other.graph().parameters[layer.transitions[0].when[1].parameter].name == "Rise");
    CHECK(app::graphedit::clipsOf(other.graph(), 0, 1).size() == 6);
    CHECK(other.canvas.selection().size() == 2);
    // One step.
    CHECK(other.undo());
    CHECK(other.graph().layers[0].states.size() == 1);

    // Into the graph it came from: under names nothing has.
    CHECK(document.paste(text, {0.0f, 600.0f}) == 2);
    CHECK(bodyOf(document).states[3].name == "Move 2");
    CHECK(bodyOf(document).states[4].name == "Jump 2");
    CHECK(document.graph().parameters.size() == 8);
    CHECK(document.transitionsBetween(3, 4).size() == 1);
}

TEST_CASE("ADR 0211: an animation graph is saved with where its states are, and opens as it was left")
{
    const Folder folder;
    folder.write("anim/hero.animgraph.json", heroGraph());
    folder.write("anim/new.animgraph.json", GraphEditor::newAnimationGraph());
    GraphEditor graphs;
    const std::optional<core::usize> fresh =
        graphs.open(folder.root / "content/anim/new.animgraph.json", "anim/new.animgraph.json");
    REQUIRE(fresh.has_value());
    CHECK(graphs.at(*fresh)->kind == app::GraphKind::Animation);
    CHECK(graphs.at(*fresh)->animation.graph().layers[0].states[0].name == "Idle");

    const std::optional<core::usize> opened =
        graphs.open(folder.root / "content/anim/hero.animgraph.json", "anim/hero.animgraph.json");
    REQUIRE(opened.has_value());
    CHECK(graphs.active() == opened);
    app::GraphDocument* const document = graphs.at(*opened);
    CHECK_FALSE(document->dirty());
    REQUIRE(document->animation.addState({512.0f, 256.0f}).has_value());
    CHECK(document->dirty());
    REQUIRE(graphs.save(*opened));
    CHECK_FALSE(document->dirty());

    const std::optional<asset::AnimationGraph> saved =
        asset::readAnimationGraph(folder.read("anim/hero.animgraph.json"));
    REQUIRE(saved.has_value());
    CHECK(*saved == document->animation.graph());
    CHECK(saved->layers[0].states[3].place == asset::GraphPlace{512.0f, 256.0f, true});
    // What the game reads of it is what it read before, and one state more.
    CHECK(saved->layers[0].transitions.size() == 4);
    CHECK(saved->clips.size() == 12);

    graphs.close(*fresh);
    CHECK(graphs.count() == 1);
    CHECK(graphs.at(0)->relative == "anim/hero.animgraph.json");
}

TEST_CASE("ADR 0211: every material graph in the examples writes a shader the real compiler takes")
{
    if (std::string_view(ENG_TEST_SHADERCROSS).empty() || !std::filesystem::exists(ENG_TEST_SHADERCROSS)) {
        MESSAGE("ENG_TEST_SKIP: no shader toolchain on this host");
        return;
    }
    REQUIRE(core::engineCatalog().loadFromFile(ENG_TEST_CATALOG).ok);
    const std::filesystem::path examples =
        std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() / "examples";
    std::error_code ec;
    int found = 0;
    for (std::filesystem::recursive_directory_iterator it(examples, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string path = it->path().generic_string();
        if (!it->is_regular_file() || !asset::isSurfaceGraphPath(path))
            continue;
        CAPTURE(path);
        // In a folder of its own, written by the graph as the editor writes
        // it: what is compiled is what a save makes, not what the tree has.
        const Folder folder;
        const std::string name = it->path().filename().generic_string();
        std::filesystem::copy_file(it->path(), folder.root / "content" / "shaders" / name);
        GraphEditor graphs;
        const std::optional<core::usize> opened =
            graphs.open(folder.root / "content" / "shaders" / name, "shaders/" + name);
        REQUIRE(opened.has_value());
        REQUIRE(graphs.save(*opened));
        asset::ContentMounts mounts;
        mounts.mountDirectory(folder.root / "content");
        app::SurfaceCompiler compiler(mounts, ENG_TEST_SHADERCROSS, ENG_TEST_SHADER_INCLUDE, folder.root / "cache");
        const std::string urn = "asset://shaders/" + asset::surfaceShaderPathOf(name);
        const render::SurfaceProgram* program = nullptr;
        (void)compiler.find(urn, rhi::ShaderFormat::SpirV, program);
        compiler.drain();
        const render::SurfaceStatus compiled = compiler.find(urn, rhi::ShaderFormat::SpirV, program);
        for (const app::SurfaceError& error : compiler.errors(urn))
            MESSAGE("line " << error.line << ": " << error.message);
        CHECK(compiled == render::SurfaceStatus::Ready);
        found += 1;
    }
    CHECK(found >= 1);
}
