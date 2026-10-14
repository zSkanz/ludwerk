#pragma once

// **A material graph** (ADR 0211): a surface shader drawn as nodes. The graph
// is a file, `*.surfacegraph.json`, and what it does is WRITE a surface
// shader (ADR 0091) beside itself -- `<name>.surface.hlsl`, which a material
// names exactly as it names one a person wrote. Nothing downstream knows a
// graph was there: the same compiler, the same reload, the same parameters
// in the material panel.
//
// **One way.** The graph writes the shader; a shader edited by hand is not
// read back. What makes that a fair exit and not a trap is that the written
// file is one a person could have written: a statement a node, in the order
// they feed each other, each local named after its node. Keep the file and
// delete the graph, and the surface is yours in code.
//
// **A line a node.** Every node's statement is one line of the written file,
// and the writer says which node each line is -- so the compiler's "line 31"
// is a node on the canvas, and its message is drawn there.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::asset {

inline constexpr std::string_view SurfaceGraphFormat = "surfacegraph";
inline constexpr core::i64 SurfaceGraphVersion = 1;
inline constexpr std::string_view SurfaceGraphSuffix = ".surfacegraph.json";

[[nodiscard]] bool isSurfaceGraphPath(std::string_view path) noexcept;
// `content/shaders/rust.surfacegraph.json` writes `content/shaders/rust.surface.hlsl`.
[[nodiscard]] std::string surfaceShaderPathOf(std::string_view graphPath);

// How wide a value is. `Widest` is an arithmetic node's: as wide as the
// widest thing put into it, a number widening to meet the rest.
enum class GraphWidth : core::u8
{
    Number = 1,
    Vector2 = 2,
    Vector3 = 3,
    Vector4 = 4,
    Widest = 5,
};

struct SurfaceNodePin
{
    std::string_view name;
    GraphWidth width = GraphWidth::Number;
    // What an input with nothing linked to it is, as the shader's text. Empty
    // for an input that has to be linked.
    std::string_view fallback;
};

struct SurfaceNodeField
{
    std::string_view name;
    std::string_view initial;
};

// Where a node may stand: some things exist only in one of a surface's two
// functions -- a derivative, an object-space vertex.
enum class GraphStage : core::u8
{
    Either,
    FragmentOnly,
    VertexOnly,
};

// A kind of node: what the canvas offers in its list.
struct SurfaceNodeKind
{
    std::string_view name;
    // The heading it is listed under: "Inputs", "Math", ...
    std::string_view group;
    std::span<const SurfaceNodePin> inputs;
    std::span<const SurfaceNodePin> outputs;
    std::span<const SurfaceNodeField> fields;
    GraphStage stage = GraphStage::Either;
};

// Every kind there is, in the order the list shows them.
[[nodiscard]] std::span<const SurfaceNodeKind> surfaceNodeKinds() noexcept;
[[nodiscard]] const SurfaceNodeKind* surfaceNodeKind(std::string_view name) noexcept;

// The two ends, which a graph has one of each at most.
inline constexpr std::string_view SurfaceEndNode = "Surface";
inline constexpr std::string_view VertexEndNode = "Vertex";

struct SurfaceGraphField
{
    std::string name;
    std::string value;

    [[nodiscard]] bool operator==(const SurfaceGraphField&) const noexcept = default;
};

struct SurfaceGraphNode
{
    core::u32 id = 0;
    std::string kind;
    // What a person called it; it names the local the node's statement makes.
    std::string title;
    core::f32 x = 0.0f;
    core::f32 y = 0.0f;
    std::vector<SurfaceGraphField> fields;

    [[nodiscard]] std::string_view field(std::string_view name) const noexcept;
    [[nodiscard]] bool operator==(const SurfaceGraphNode&) const noexcept = default;
};

// By the pins' names, so a file survives a kind gaining a pin.
struct SurfaceGraphLink
{
    core::u32 from = 0;
    std::string output;
    core::u32 to = 0;
    std::string input;

    [[nodiscard]] bool operator==(const SurfaceGraphLink&) const noexcept = default;
};

struct SurfaceGraph
{
    std::vector<SurfaceGraphNode> nodes;
    std::vector<SurfaceGraphLink> links;

    [[nodiscard]] const SurfaceGraphNode* node(core::u32 id) const noexcept;
    [[nodiscard]] bool operator==(const SurfaceGraph&) const noexcept = default;
};

enum class SurfaceGraphReadCode : core::u8
{
    None,
    Malformed,
    NotAGraph,
    UnsupportedVersion,
    UnknownKey,
    MissingKey,
    WrongType,
    DuplicateId,
    UnknownKind,
    UnknownNode,
};

struct SurfaceGraphReadError
{
    SurfaceGraphReadCode code = SurfaceGraphReadCode::None;
    // Where in the file -- `nodes[2].kind` -- and what was there.
    std::string where;
    std::string what;
};

[[nodiscard]] std::optional<SurfaceGraph> readSurfaceGraph(std::string_view json,
                                                           SurfaceGraphReadError* error = nullptr);
// The file's text: a node a few lines, a link a line, so a change to one node
// is a change to its lines.
[[nodiscard]] std::string writeSurfaceGraph(const SurfaceGraph& graph);

// What is wrong with a graph as a shader, each on the node it is wrong at.
enum class SurfaceGraphIssueCode : core::u8
{
    // An input that has to be linked is not.
    InputMissing,
    // Two widths that do not meet: three wide into two wide.
    WidthsDoNotFit,
    // A node that only one function has, reached from the other.
    WrongStage,
    // A parameter or a texture with no name, or one that is not a name.
    BadName,
    // Two parameters or textures of one name that are not the same thing.
    NameTaken,
    // A constant or a default that is not numbers.
    BadValue,
    // An expression with nothing in it.
    EmptyExpression,
    // A link to a pin its node does not have.
    NoSuchPin,
    // A value that depends on itself.
    Cycle,
    // A second Surface, or a second Vertex.
    TwoEnds,
};

struct SurfaceGraphIssue
{
    core::u32 node = 0;
    SurfaceGraphIssueCode code = SurfaceGraphIssueCode::InputMissing;
    // The pin or the field it is about, and what was found.
    std::string where;
    std::string what;
};

struct WrittenSurface
{
    // The surface shader. Complete even with issues: a node that could not
    // be written is written as nought, so the rest still compiles and shows.
    std::string hlsl;
    // For each line of `hlsl`, first line first, the node whose statement it
    // is; nought for a line that is nobody's.
    std::vector<core::u32> lineNodes;
    std::vector<SurfaceGraphIssue> issues;
    // Whether the surface has each of its two functions.
    bool vertex = false;
    bool fragment = false;
};

// Writes the shader. `graphFile` is the graph's own file name, said in the
// shader's first line.
[[nodiscard]] WrittenSurface writeSurfaceShader(const SurfaceGraph& graph, std::string_view graphFile);

// The width a node's output comes to in this graph, for the canvas to draw a
// pin by: an arithmetic node's is as wide as what is put into it. Nought
// where it cannot be said.
[[nodiscard]] core::u32 surfaceOutputWidth(const SurfaceGraph& graph, core::u32 node, std::string_view output);

} // namespace engine::asset
