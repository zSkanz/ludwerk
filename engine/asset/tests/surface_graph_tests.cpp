// A material graph (ADR 0211): its file read and written, and the surface
// shader it writes -- one a person could have written, a line a node.
#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/surface_graph.h"

namespace {

using engine::asset::SurfaceGraph;
using engine::asset::SurfaceGraphIssueCode;
using engine::asset::SurfaceGraphNode;
using engine::asset::SurfaceGraphReadCode;
using engine::asset::SurfaceGraphReadError;
using engine::asset::WrittenSurface;
using engine::core::u32;

// Builds a graph a node and a link at a time.
struct Building
{
    SurfaceGraph graph;

    u32 add(const char* kind, const char* title = "",
            std::initializer_list<std::pair<const char*, const char*>> fields = {})
    {
        SurfaceGraphNode node;
        node.id = static_cast<u32>(graph.nodes.size()) + 1;
        node.kind = kind;
        node.title = title;
        for (const auto& [name, value] : fields)
            node.fields.push_back({name, value});
        graph.nodes.push_back(node);
        return node.id;
    }

    void link(u32 from, const char* output, u32 to, const char* input)
    {
        graph.links.push_back({from, output, to, input});
    }
};

[[nodiscard]] std::vector<std::string> linesOf(const std::string& text)
{
    std::vector<std::string> out;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            out.push_back(line);
            line.clear();
        }
        else {
            line.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] bool has(const WrittenSurface& written, SurfaceGraphIssueCode code, u32 node)
{
    return std::any_of(written.issues.begin(), written.issues.end(),
                       [&](const auto& issue) { return issue.code == code && issue.node == node; });
}

// The rusting surface every test of the writer starts from.
struct Rust
{
    Building b;
    u32 base = 0;
    u32 map = 0;
    u32 patches = 0;
    u32 colour = 0;
    u32 rough = 0;
    u32 surface = 0;

    Rust()
    {
        base = b.add("Parameter", "Base", {{"Name", "Base"}, {"Type", "colour"}, {"Default", "0.5, 0.5, 0.55"}});
        map = b.add("Texture", "rust", {{"Name", "RustMap"}});
        patches = b.add("Noise", "patches");
        colour = b.add("Lerp", "colour");
        rough = b.add("Constant", "", {{"Type", "number"}, {"Value", "0.6"}});
        surface = b.add("Surface");
        b.link(base, "Out", colour, "A");
        b.link(map, "RGB", colour, "B");
        b.link(patches, "Out", colour, "T");
        b.link(colour, "Out", surface, "BaseColor");
        b.link(rough, "Out", surface, "Roughness");
    }
};

} // namespace

TEST_CASE("ADR 0211: every kind of node is found by its name, and a graph has its two ends")
{
    using namespace engine::asset;
    CHECK(surfaceNodeKinds().size() > 30);
    for (const SurfaceNodeKind& kind : surfaceNodeKinds()) {
        CHECK(surfaceNodeKind(kind.name) != nullptr);
        CHECK_FALSE(kind.group.empty());
    }
    REQUIRE(surfaceNodeKind(SurfaceEndNode) != nullptr);
    REQUIRE(surfaceNodeKind(VertexEndNode) != nullptr);
    CHECK(surfaceNodeKind(SurfaceEndNode)->outputs.empty());
    CHECK(surfaceNodeKind("NormalMap")->stage == GraphStage::FragmentOnly);
    CHECK(surfaceNodeKind("Wind") != nullptr);
    CHECK(surfaceNodeKind("Blur") == nullptr);
    CHECK(isSurfaceGraphPath("content/shaders/rust.surfacegraph.json"));
    CHECK_FALSE(isSurfaceGraphPath("content/shaders/rust.surface.hlsl"));
    CHECK(surfaceShaderPathOf("content/shaders/rust.surfacegraph.json") == "content/shaders/rust.surface.hlsl");
}

TEST_CASE("ADR 0211: a graph written to its file and read back is the same graph")
{
    Rust rust;
    rust.b.graph.nodes[2].x = 120.5f;
    rust.b.graph.nodes[2].y = -40.0f;
    const std::string text = engine::asset::writeSurfaceGraph(rust.b.graph);
    SurfaceGraphReadError error;
    const std::optional<SurfaceGraph> read = engine::asset::readSurfaceGraph(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error.where << ": " << error.what);
    CHECK(*read == rust.b.graph);
    // Written again it is the same text: a save that changed nothing changes nothing.
    CHECK(engine::asset::writeSurfaceGraph(*read) == text);
}

TEST_CASE("ADR 0211: a file that is not a graph is refused with where and why")
{
    using engine::asset::readSurfaceGraph;
    SurfaceGraphReadError error;
    CHECK_FALSE(readSurfaceGraph("{", &error).has_value());
    CHECK(error.code == SurfaceGraphReadCode::Malformed);
    CHECK_FALSE(readSurfaceGraph(R"({"format": "animgraph", "version": 1})", &error).has_value());
    CHECK(error.code == SurfaceGraphReadCode::NotAGraph);
    CHECK_FALSE(readSurfaceGraph(R"({"format": "surfacegraph", "version": 2})", &error).has_value());
    CHECK(error.code == SurfaceGraphReadCode::UnsupportedVersion);
    CHECK_FALSE(readSurfaceGraph(R"({"format": "surfacegraph", "version": 1, "colour": 3})", &error).has_value());
    CHECK(error.code == SurfaceGraphReadCode::UnknownKey);
    CHECK(error.where == "colour");
    CHECK_FALSE(readSurfaceGraph(R"({"format": "surfacegraph", "version": 1,
        "nodes": [{"id": 1, "kind": "Blur"}]})",
                                 &error)
                    .has_value());
    CHECK(error.code == SurfaceGraphReadCode::UnknownKind);
    CHECK(error.where == "nodes[0].kind");
    CHECK_FALSE(readSurfaceGraph(R"({"format": "surfacegraph", "version": 1,
        "nodes": [{"id": 1, "kind": "Add"}, {"id": 1, "kind": "Add"}]})",
                                 &error)
                    .has_value());
    CHECK(error.code == SurfaceGraphReadCode::DuplicateId);
    CHECK_FALSE(readSurfaceGraph(R"({"format": "surfacegraph", "version": 1,
        "nodes": [{"id": 1, "kind": "Add"}],
        "links": [{"from": 1, "output": "Out", "to": 9, "input": "A"}]})",
                                 &error)
                    .has_value());
    CHECK(error.code == SurfaceGraphReadCode::UnknownNode);
    CHECK(error.where == "links[0].to");
    // An empty graph is a graph.
    CHECK(readSurfaceGraph(R"({"format": "surfacegraph", "version": 1})", &error).has_value());
}

TEST_CASE("ADR 0211: the shader a graph writes is one a person could have written, a line a node")
{
    Rust rust;
    const WrittenSurface written = engine::asset::writeSurfaceShader(rust.b.graph, "rust.surfacegraph.json");
    CHECK(written.issues.empty());
    CHECK(written.fragment);
    CHECK_FALSE(written.vertex);
    const std::vector<std::string> lines = linesOf(written.hlsl);
    REQUIRE(lines.size() == written.lineNodes.size());

    // It says first what wrote it, and that the way is one way.
    CHECK(lines[0] == "// Written by rust.surfacegraph.json -- edit the graph, not this file: it is written");
    const auto at = [&lines](std::string_view text) {
        const auto found = std::find(lines.begin(), lines.end(), text);
        return found == lines.end() ? -1 : static_cast<int>(found - lines.begin());
    };
    CHECK(at("#define ENG_NO_VERTEX") >= 0);
    CHECK(at("#define ENG_NO_FRAGMENT") < 0);
    CHECK(at("#include \"engine/surface.hlsli\"") > at("#define ENG_NO_VERTEX"));
    // What the material says, as the contract writes it.
    CHECK(at("ENG_PARAM(float3, Base, float3(0.5, 0.5, 0.55), colour)") >= 0);
    CHECK(at("ENG_TEXTURE(RustMap)") >= 0);
    // The noise is written once, because a node asked for it.
    CHECK(at("float graphNoise(float3 p)") >= 0);

    // **The function**: a statement a node, in the order they feed each
    // other, each local named after its node.
    const int found = at("void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)");
    REQUIRE(found >= 0);
    const std::size_t open = static_cast<std::size_t>(found);
    REQUIRE(lines.size() >= open + 8);
    CHECK(lines[open + 1] == "{");
    CHECK(lines[open + 2] == "    const float4 rust = ENG_SAMPLE(RustMap, inputs.Uv0);");
    CHECK(lines[open + 3] == "    const float patches = graphNoise(inputs.WorldPosition * 3.0f);");
    CHECK(lines[open + 4] == "    const float3 colour = lerp(Base, rust.rgb, patches);");
    CHECK(lines[open + 5] == "    surface.BaseColor = colour;");
    CHECK(lines[open + 6] == "    surface.Roughness = 0.6f;");
    CHECK(lines[open + 7] == "}");

    // **And each line knows its node**, so a compiler's line is a node.
    CHECK(written.lineNodes[open + 2] == rust.map);
    CHECK(written.lineNodes[open + 3] == rust.patches);
    CHECK(written.lineNodes[open + 4] == rust.colour);
    CHECK(written.lineNodes[open + 5] == rust.surface);
    CHECK(written.lineNodes[open] == 0);
    CHECK(written.lineNodes[0] == 0);

    // What the canvas draws a pin by.
    CHECK(engine::asset::surfaceOutputWidth(rust.b.graph, rust.colour, "Out") == 3);
    CHECK(engine::asset::surfaceOutputWidth(rust.b.graph, rust.map, "A") == 1);
    CHECK(engine::asset::surfaceOutputWidth(rust.b.graph, rust.patches, "Out") == 1);
}

TEST_CASE("ADR 0211: a node nothing uses writes nothing, and two locals of one name are told apart")
{
    Rust rust;
    // A stray node, and a second node a person also called "colour".
    (void)rust.b.add("Sine", "unused");
    const u32 again = rust.b.add("Saturate", "colour");
    rust.b.link(rust.colour, "Out", again, "A");
    rust.b.graph.links.erase(rust.b.graph.links.begin() + 3);
    rust.b.link(again, "Out", rust.surface, "BaseColor");
    const WrittenSurface written = engine::asset::writeSurfaceShader(rust.b.graph, "rust.surfacegraph.json");
    CHECK(written.issues.empty());
    CHECK(written.hlsl.find("unused") == std::string::npos);
    CHECK(written.hlsl.find("    const float3 colour = lerp(Base, rust.rgb, patches);") != std::string::npos);
    CHECK(written.hlsl.find("    const float3 colour2 = saturate(colour);") != std::string::npos);
    CHECK(written.hlsl.find("    surface.BaseColor = colour2;") != std::string::npos);
}

TEST_CASE("ADR 0211: the vertex end writes the other function, and the wind sways a banner")
{
    Building b;
    const u32 wind = b.add("Wind");
    const u32 sway =
        b.add("Parameter", "Sway", {{"Name", "Sway"}, {"Type", "number"}, {"Default", "0.1"}, {"Range", "0, 1"}});
    const u32 height = b.add("Split", "at");
    const u32 place = b.add("ObjectPosition");
    const u32 push = b.add("Multiply", "push");
    const u32 scaled = b.add("Multiply", "offset");
    const u32 vertex = b.add("Vertex");
    b.link(place, "Out", height, "A");
    b.link(wind, "Out", push, "A");
    b.link(sway, "Out", push, "B");
    b.link(push, "Out", scaled, "A");
    b.link(height, "Y", scaled, "B");
    b.link(scaled, "Out", vertex, "Offset");
    const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "banner.surfacegraph.json");
    CHECK(written.issues.empty());
    CHECK(written.vertex);
    CHECK_FALSE(written.fragment);
    CHECK(written.hlsl.find("#define ENG_NO_FRAGMENT") != std::string::npos);
    CHECK(written.hlsl.find("ENG_PARAM(float, Sway, 0.1, range(0, 1))") != std::string::npos);
    CHECK(written.hlsl.find("void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)") !=
          std::string::npos);
    CHECK(written.hlsl.find("    const float3 push = inputs.Wind * Sway;") != std::string::npos);
    CHECK(written.hlsl.find("    const float3 at = vertex.Position;") != std::string::npos);
    CHECK(written.hlsl.find("    const float3 offset = push * at.y;") != std::string::npos);
    CHECK(written.hlsl.find("    vertex.Position += offset;") != std::string::npos);
}

TEST_CASE("ADR 0211: an expression is its line, with what is linked put in for the letters")
{
    Building b;
    const u32 time = b.add("Time");
    const u32 uv = b.add("Uv0");
    const u32 code = b.add("Expression", "stripe", {{"Code", "saturate(a * 2.0 - b.x) * a"}, {"Type", "number"}});
    const u32 surface = b.add("Surface");
    b.link(time, "Out", code, "a");
    b.link(uv, "Out", code, "b");
    b.link(code, "Out", surface, "Alpha");
    WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "stripe.surfacegraph.json");
    CHECK(written.issues.empty());
    // `b.x` is b's x, and the `a` after the dot of nothing is still a.
    CHECK(written.hlsl.find("    const float stripe = saturate(inputs.Time * 2.0 - inputs.Uv0.x) * inputs.Time;") !=
          std::string::npos);

    // A letter the line uses with nothing linked to it is said on the node.
    b.graph.links.erase(b.graph.links.begin() + 1);
    written = engine::asset::writeSurfaceShader(b.graph, "stripe.surfacegraph.json");
    CHECK(has(written, SurfaceGraphIssueCode::InputMissing, code));
    // And an empty line is.
    b.graph.nodes[2].fields[0].value = "  ";
    written = engine::asset::writeSurfaceShader(b.graph, "stripe.surfacegraph.json");
    CHECK(has(written, SurfaceGraphIssueCode::EmptyExpression, code));
}

TEST_CASE("ADR 0211: what is wrong with a graph is said on the node it is wrong at, and the shader is still whole")
{
    // An input that has to be linked and is not.
    {
        Building b;
        const u32 lone = b.add("Saturate");
        const u32 surface = b.add("Surface");
        b.link(lone, "Out", surface, "Alpha");
        const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json");
        CHECK(has(written, SurfaceGraphIssueCode::InputMissing, lone));
        // Written as nought, so the rest compiles and shows.
        CHECK(written.hlsl.find("saturate(0.0f)") != std::string::npos);
    }
    // Two wide and three wide do not meet.
    {
        Building b;
        const u32 uv = b.add("Uv0");
        const u32 normal = b.add("WorldNormal");
        const u32 sum = b.add("Add");
        const u32 surface = b.add("Surface");
        b.link(uv, "Out", sum, "A");
        b.link(normal, "Out", sum, "B");
        b.link(sum, "Out", surface, "BaseColor");
        const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json");
        CHECK(has(written, SurfaceGraphIssueCode::WidthsDoNotFit, sum));
        // Nor does two wide go into a colour.
        Building c;
        const u32 two = c.add("Uv0");
        const u32 end = c.add("Surface");
        c.link(two, "Out", end, "BaseColor");
        CHECK(has(engine::asset::writeSurfaceShader(c.graph, "g.surfacegraph.json"),
                  SurfaceGraphIssueCode::WidthsDoNotFit, end));
    }
    // A normal map has no place in the vertex function.
    {
        Building b;
        const u32 map = b.add("NormalMap", "", {{"Name", "Bumps"}});
        const u32 vertex = b.add("Vertex");
        b.link(map, "Out", vertex, "Normal");
        CHECK(has(engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json"), SurfaceGraphIssueCode::WrongStage,
                  map));
    }
    // A value that depends on itself.
    {
        Building b;
        const u32 one = b.add("Add", "one");
        const u32 two = b.add("Add", "two");
        const u32 surface = b.add("Surface");
        b.link(one, "Out", two, "A");
        b.link(two, "Out", one, "A");
        b.link(two, "Out", surface, "Alpha");
        const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json");
        CHECK((has(written, SurfaceGraphIssueCode::Cycle, one) || has(written, SurfaceGraphIssueCode::Cycle, two)));
    }
    // A name that is not a name, and one that two different things want.
    {
        Building b;
        const u32 bad = b.add("Parameter", "", {{"Name", "2 fast"}});
        const u32 number = b.add("Parameter", "", {{"Name", "Tint"}, {"Type", "number"}});
        const u32 colour = b.add("Parameter", "", {{"Name", "Tint"}, {"Type", "colour"}, {"Default", "1"}});
        const u32 sum = b.add("Add");
        const u32 more = b.add("Multiply");
        const u32 surface = b.add("Surface");
        b.link(bad, "Out", sum, "A");
        b.link(number, "Out", sum, "B");
        b.link(sum, "Out", more, "A");
        b.link(colour, "Out", more, "B");
        b.link(more, "Out", surface, "BaseColor");
        const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json");
        CHECK(has(written, SurfaceGraphIssueCode::BadName, bad));
        CHECK(has(written, SurfaceGraphIssueCode::NameTaken, colour));
        // The same parameter named twice the same way is one parameter.
        Building c;
        const u32 first = c.add("Parameter", "", {{"Name", "Glow"}});
        const u32 second = c.add("Parameter", "", {{"Name", "Glow"}});
        const u32 both = c.add("Add");
        const u32 end = c.add("Surface");
        c.link(first, "Out", both, "A");
        c.link(second, "Out", both, "B");
        c.link(both, "Out", end, "Alpha");
        const WrittenSurface once = engine::asset::writeSurfaceShader(c.graph, "g.surfacegraph.json");
        CHECK(once.issues.empty());
        CHECK(once.hlsl.find("ENG_PARAM(float, Glow, 0.0)") != std::string::npos);
        CHECK(once.hlsl.find("ENG_PARAM(float, Glow, 0.0)") == once.hlsl.rfind("ENG_PARAM(float, Glow, 0.0)"));
    }
    // A constant that is not numbers, and a second Surface.
    {
        Building b;
        const u32 words = b.add("Constant", "", {{"Type", "vector3"}, {"Value", "red"}});
        const u32 surface = b.add("Surface");
        const u32 another = b.add("Surface");
        b.link(words, "Out", surface, "BaseColor");
        const WrittenSurface written = engine::asset::writeSurfaceShader(b.graph, "g.surfacegraph.json");
        CHECK(has(written, SurfaceGraphIssueCode::BadValue, words));
        CHECK(has(written, SurfaceGraphIssueCode::TwoEnds, another));
    }
}

TEST_CASE("ADR 0211: every material graph in the examples is in the writer's own form, and the shader beside it is "
          "the one it writes")
{
    const auto textOf = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        // A checkout may have given the lines their other ending.
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    };
    std::error_code ec;
    int found = 0;
    for (std::filesystem::recursive_directory_iterator it(std::filesystem::path(ENG_ASSET_TEST_EXAMPLES), ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string path = it->path().generic_string();
        if (!it->is_regular_file() || !engine::asset::isSurfaceGraphPath(path))
            continue;
        CAPTURE(path);
        const std::string text = textOf(it->path());
        engine::asset::SurfaceGraphReadError error;
        const std::optional<engine::asset::SurfaceGraph> graph = engine::asset::readSurfaceGraph(text, &error);
        CAPTURE(error.where);
        REQUIRE(graph.has_value());
        // Opened and saved with nothing changed, the file is the same file.
        CHECK(engine::asset::writeSurfaceGraph(*graph) == text);
        const engine::asset::WrittenSurface written =
            engine::asset::writeSurfaceShader(*graph, it->path().filename().generic_string());
        for (const engine::asset::SurfaceGraphIssue& issue : written.issues)
            MESSAGE("issue at node " << issue.node << ": " << issue.where << " " << issue.what);
        CHECK(written.issues.empty());
        // And the shader a material names is what the graph says today: a
        // graph changed and not saved through the editor is found here.
        const std::filesystem::path shader =
            it->path().parent_path() / engine::asset::surfaceShaderPathOf(it->path().filename().generic_string());
        CHECK(written.hlsl == textOf(shader));
        found += 1;
    }
    CHECK(found >= 1);
}
