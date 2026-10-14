#include "engine/asset/surface_graph.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"

namespace engine::asset {

namespace {

using core::f32;
using core::f64;
using core::JsonType;
using core::JsonValue;
using core::u32;
using core::usize;

// --- The kinds ---------------------------------------------------------------------
//
// A recipe is a kind and how its statement is written: the expression, with
// `{Pin}` for each input, and for each output what follows the local's name
// (`.rgb`, `.x`) -- nothing for a node that has one output and is it.
struct Recipe
{
    SurfaceNodeKind kind;
    std::string_view expression;
    std::span<const std::string_view> suffixes;
    // What the statement's own local is, where that is not the output: a
    // texture is read four wide and handed out as its colour and its alpha.
    GraphWidth statement = GraphWidth::Widest;
    // An input: read off `inputs`, with no statement at all.
    std::string_view reads;
};

using Pin = SurfaceNodePin;
using Field = SurfaceNodeField;

constexpr Pin kOutNumber[] = {{"Out", GraphWidth::Number, ""}};
constexpr Pin kOut2[] = {{"Out", GraphWidth::Vector2, ""}};
constexpr Pin kOut3[] = {{"Out", GraphWidth::Vector3, ""}};
constexpr Pin kOut4[] = {{"Out", GraphWidth::Vector4, ""}};
constexpr Pin kOutWidest[] = {{"Out", GraphWidth::Widest, ""}};

constexpr Pin kTwoZero[] = {{"A", GraphWidth::Widest, "0.0f"}, {"B", GraphWidth::Widest, "0.0f"}};
constexpr Pin kTwoOne[] = {{"A", GraphWidth::Widest, "1.0f"}, {"B", GraphWidth::Widest, "1.0f"}};
constexpr Pin kOne[] = {{"A", GraphWidth::Widest, ""}};
constexpr Pin kLerp[] = {
    {"A", GraphWidth::Widest, "0.0f"}, {"B", GraphWidth::Widest, "1.0f"}, {"T", GraphWidth::Widest, "0.5f"}};
constexpr Pin kStep[] = {{"Edge", GraphWidth::Widest, "0.5f"}, {"X", GraphWidth::Widest, ""}};
constexpr Pin kSmoothstep[] = {
    {"Low", GraphWidth::Widest, "0.0f"}, {"High", GraphWidth::Widest, "1.0f"}, {"X", GraphWidth::Widest, ""}};
constexpr Pin kTwoRequired[] = {{"A", GraphWidth::Widest, ""}, {"B", GraphWidth::Widest, ""}};

constexpr Field kParameterFields[] = {{"Name", "Value"}, {"Type", "number"}, {"Default", "0"}, {"Range", ""}};
constexpr Field kConstantFields[] = {{"Type", "number"}, {"Value", "0"}};
constexpr Field kTextureFields[] = {{"Name", "Map"}};
constexpr Pin kTextureIn[] = {{"Uv", GraphWidth::Vector2, "inputs.Uv0"}};
constexpr Pin kTextureOut[] = {{"RGB", GraphWidth::Vector3, ""}, {"A", GraphWidth::Number, ""}};
constexpr std::string_view kTextureSuffixes[] = {".rgb", ".a"};
constexpr Field kNormalMapFields[] = {{"Name", "NormalMap"}};
constexpr Pin kNormalMapIn[] = {{"Uv", GraphWidth::Vector2, "inputs.Uv0"}, {"Strength", GraphWidth::Number, "1.0f"}};

constexpr Pin kSplitOut[] = {{"X", GraphWidth::Number, ""},
                             {"Y", GraphWidth::Number, ""},
                             {"Z", GraphWidth::Number, ""},
                             {"W", GraphWidth::Number, ""}};
constexpr std::string_view kSplitSuffixes[] = {".x", ".y", ".z", ".w"};
constexpr Pin kCombineIn[] = {{"X", GraphWidth::Number, "0.0f"},
                              {"Y", GraphWidth::Number, "0.0f"},
                              {"Z", GraphWidth::Number, "0.0f"},
                              {"W", GraphWidth::Number, "0.0f"}};
constexpr Field kCombineFields[] = {{"Width", "3"}};

constexpr Pin kFresnelIn[] = {{"Normal", GraphWidth::Vector3, "inputs.WorldNormal"},
                              {"Power", GraphWidth::Number, "5.0f"}};
constexpr Pin kPannerIn[] = {{"Uv", GraphWidth::Vector2, "inputs.Uv0"},
                             {"Speed", GraphWidth::Vector2, "float2(0.1f, 0.0f)"}};
constexpr Pin kNoiseIn[] = {{"Position", GraphWidth::Vector3, "inputs.WorldPosition"},
                            {"Scale", GraphWidth::Number, "3.0f"}};

constexpr Pin kExpressionIn[] = {{"a", GraphWidth::Widest, ""},
                                 {"b", GraphWidth::Widest, ""},
                                 {"c", GraphWidth::Widest, ""},
                                 {"d", GraphWidth::Widest, ""}};
constexpr Field kExpressionFields[] = {{"Code", "a"}, {"Type", "number"}};

constexpr Pin kSurfaceIn[] = {{"BaseColor", GraphWidth::Vector3, ""}, {"Alpha", GraphWidth::Number, ""},
                              {"Metallic", GraphWidth::Number, ""},   {"Roughness", GraphWidth::Number, ""},
                              {"Normal", GraphWidth::Vector3, ""},    {"Emissive", GraphWidth::Vector3, ""}};
constexpr Pin kVertexIn[] = {{"Offset", GraphWidth::Vector3, ""}, {"Normal", GraphWidth::Vector3, ""}};

constexpr Recipe kRecipes[] = {
    // What a surface reads: no statement, the thing itself.
    {{"Uv0", "Inputs", {}, kOut2, {}}, "", {}, GraphWidth::Vector2, "inputs.Uv0"},
    {{"Uv1", "Inputs", {}, kOut2, {}}, "", {}, GraphWidth::Vector2, "inputs.Uv1"},
    {{"WorldPosition", "Inputs", {}, kOut3, {}}, "", {}, GraphWidth::Vector3, "inputs.WorldPosition"},
    {{"WorldNormal", "Inputs", {}, kOut3, {}}, "", {}, GraphWidth::Vector3, "inputs.WorldNormal"},
    {{"VertexColor", "Inputs", {}, kOut4, {}}, "", {}, GraphWidth::Vector4, "inputs.VertexColor"},
    {{"Time", "Inputs", {}, kOutNumber, {}}, "", {}, GraphWidth::Number, "inputs.Time"},
    {{"CameraPosition", "Inputs", {}, kOut3, {}}, "", {}, GraphWidth::Vector3, "inputs.CameraPosition"},
    {{"ScreenUv", "Inputs", {}, kOut2, {}}, "", {}, GraphWidth::Vector2, "inputs.ScreenUv"},
    {{"Wind", "Inputs", {}, kOut3, {}}, "", {}, GraphWidth::Vector3, "inputs.Wind"},
    {{"ObjectPosition", "Inputs", {}, kOut3, {}, GraphStage::VertexOnly},
     "",
     {},
     GraphWidth::Vector3,
     "vertex.Position"},
    {{"ObjectNormal", "Inputs", {}, kOut3, {}, GraphStage::VertexOnly}, "", {}, GraphWidth::Vector3, "vertex.Normal"},

    // What a material says: written in code, each its own way.
    {{"Parameter", "Values", {}, kOutWidest, kParameterFields}, "", {}, GraphWidth::Widest, ""},
    {{"Constant", "Values", {}, kOutWidest, kConstantFields}, "", {}, GraphWidth::Widest, ""},
    {{"Texture", "Values", kTextureIn, kTextureOut, kTextureFields}, "", kTextureSuffixes, GraphWidth::Vector4, ""},
    {{"NormalMap", "Values", kNormalMapIn, kOut3, kNormalMapFields, GraphStage::FragmentOnly},
     "",
     {},
     GraphWidth::Vector3,
     ""},

    {{"Add", "Math", kTwoZero, kOutWidest, {}}, "{A} + {B}", {}, GraphWidth::Widest, ""},
    {{"Subtract", "Math", kTwoZero, kOutWidest, {}}, "{A} - {B}", {}, GraphWidth::Widest, ""},
    {{"Multiply", "Math", kTwoOne, kOutWidest, {}}, "{A} * {B}", {}, GraphWidth::Widest, ""},
    {{"Divide", "Math", kTwoOne, kOutWidest, {}}, "{A} / {B}", {}, GraphWidth::Widest, ""},
    {{"Lerp", "Math", kLerp, kOutWidest, {}}, "lerp({A}, {B}, {T})", {}, GraphWidth::Widest, ""},
    {{"Saturate", "Math", kOne, kOutWidest, {}}, "saturate({A})", {}, GraphWidth::Widest, ""},
    {{"OneMinus", "Math", kOne, kOutWidest, {}}, "1.0f - {A}", {}, GraphWidth::Widest, ""},
    {{"Power", "Math", kTwoOne, kOutWidest, {}}, "pow({A}, {B})", {}, GraphWidth::Widest, ""},
    {{"Min", "Math", kTwoRequired, kOutWidest, {}}, "min({A}, {B})", {}, GraphWidth::Widest, ""},
    {{"Max", "Math", kTwoRequired, kOutWidest, {}}, "max({A}, {B})", {}, GraphWidth::Widest, ""},
    {{"Abs", "Math", kOne, kOutWidest, {}}, "abs({A})", {}, GraphWidth::Widest, ""},
    {{"Step", "Math", kStep, kOutWidest, {}}, "step({Edge}, {X})", {}, GraphWidth::Widest, ""},
    {{"Smoothstep", "Math", kSmoothstep, kOutWidest, {}}, "smoothstep({Low}, {High}, {X})", {}, GraphWidth::Widest, ""},
    {{"Sine", "Math", kOne, kOutWidest, {}}, "sin({A})", {}, GraphWidth::Widest, ""},
    {{"Fraction", "Math", kOne, kOutWidest, {}}, "frac({A})", {}, GraphWidth::Widest, ""},
    {{"Normalize", "Math", kOne, kOutWidest, {}}, "normalize({A})", {}, GraphWidth::Widest, ""},
    {{"Dot", "Math", kTwoRequired, kOutNumber, {}}, "dot({A}, {B})", {}, GraphWidth::Number, ""},
    {{"Length", "Math", kOne, kOutNumber, {}}, "length({A})", {}, GraphWidth::Number, ""},

    {{"Split", "Shape", kOne, kSplitOut, {}}, "{A}", kSplitSuffixes, GraphWidth::Widest, ""},
    {{"Combine", "Shape", kCombineIn, kOutWidest, kCombineFields}, "", {}, GraphWidth::Widest, ""},

    {{"Fresnel", "Patterns", kFresnelIn, kOutNumber, {}},
     "pow(1.0f - saturate(dot(normalize({Normal}), normalize(inputs.CameraPosition - inputs.WorldPosition))), {Power})",
     {},
     GraphWidth::Number,
     ""},
    {{"Panner", "Patterns", kPannerIn, kOut2, {}}, "{Uv} + {Speed} * inputs.Time", {}, GraphWidth::Vector2, ""},
    {{"Noise", "Patterns", kNoiseIn, kOutNumber, {}}, "graphNoise({Position} * {Scale})", {}, GraphWidth::Number, ""},

    {{"Expression", "Code", kExpressionIn, kOutWidest, kExpressionFields}, "", {}, GraphWidth::Widest, ""},

    {{"Surface", "Ends", kSurfaceIn, {}, {}, GraphStage::FragmentOnly}, "", {}, GraphWidth::Number, ""},
    {{"Vertex", "Ends", kVertexIn, {}, {}, GraphStage::VertexOnly}, "", {}, GraphWidth::Number, ""},
};

constexpr usize kKindCount = std::size(kRecipes);

[[nodiscard]] const Recipe* recipeOf(std::string_view name) noexcept
{
    for (const Recipe& recipe : kRecipes) {
        if (recipe.kind.name == name)
            return &recipe;
    }
    return nullptr;
}

[[nodiscard]] std::span<const SurfaceNodeKind> allKinds() noexcept
{
    static const std::array<SurfaceNodeKind, kKindCount> kinds = [] {
        std::array<SurfaceNodeKind, kKindCount> out{};
        for (usize index = 0; index < kKindCount; ++index)
            out[index] = kRecipes[index].kind;
        return out;
    }();
    return kinds;
}

// The noise every graph that asks for one is given: value noise on a
// lattice, smoothed. Written into the shader once, above its functions.
constexpr std::string_view kNoiseHelpers[] = {
    "float graphHash(float3 p)",
    "{",
    "    p = frac(p * 0.3183099f + 0.1f);",
    "    p *= 17.0f;",
    "    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));",
    "}",
    "",
    "// Value noise on a lattice, smoothed: nought to one.",
    "float graphNoise(float3 p)",
    "{",
    "    const float3 cell = floor(p);",
    "    const float3 f = frac(p);",
    "    const float3 u = f * f * (3.0f - 2.0f * f);",
    "    return lerp(lerp(lerp(graphHash(cell), graphHash(cell + float3(1, 0, 0)), u.x),",
    "                     lerp(graphHash(cell + float3(0, 1, 0)), graphHash(cell + float3(1, 1, 0)), u.x), u.y),",
    "                lerp(lerp(graphHash(cell + float3(0, 0, 1)), graphHash(cell + float3(1, 0, 1)), u.x),",
    "                     lerp(graphHash(cell + float3(0, 1, 1)), graphHash(cell + float3(1, 1, 1)), u.x), u.y),",
    "                u.z);",
    "}",
};

// --- Small text ---------------------------------------------------------------------

[[nodiscard]] std::string_view typeName(u32 width) noexcept
{
    switch (width) {
    case 2:
        return "float2";
    case 3:
        return "float3";
    case 4:
        return "float4";
    default:
        return "float";
    }
}

// A width a person names: "number", "vector3", "colour" ... Nought for
// something that is none of them.
[[nodiscard]] u32 widthNamed(std::string_view name) noexcept
{
    if (name == "number" || name == "toggle")
        return 1;
    if (name == "vector2")
        return 2;
    if (name == "vector3" || name == "colour" || name == "color")
        return 3;
    if (name == "vector4")
        return 4;
    return 0;
}

[[nodiscard]] bool isName(std::string_view text) noexcept
{
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'))
        return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// One number as the shader writes it: `1.0f`, `0.25f`.
[[nodiscard]] std::string numberLiteral(f64 value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    std::string text(buffer);
    if (text.find_first_of(".einf") == std::string::npos)
        text += ".0";
    return text + "f";
}

// "1, 0.5 0" as numbers. Nothing for text that is not numbers.
[[nodiscard]] std::optional<std::vector<f64>> numbersOf(std::string_view text)
{
    std::vector<f64> out;
    usize at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == ',' || text[at] == '\t'))
            ++at;
        if (at >= text.size())
            break;
        usize end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != ',' && text[end] != '\t')
            ++end;
        // `strtod`, on a copy that ends where the word does: the library a
        // phone is built with has no `from_chars` for a number with a
        // point in it.
        const std::string word(text.substr(at, end - at));
        char* stopped = nullptr;
        const f64 value = std::strtod(word.c_str(), &stopped);
        if (stopped != word.c_str() + word.size() || !std::isfinite(value))
            return std::nullopt;
        out.push_back(value);
        at = end;
    }
    return out;
}

// Numbers as a value `width` wide: one number fills every part.
[[nodiscard]] std::optional<std::string> literalOf(std::string_view text, u32 width)
{
    const std::optional<std::vector<f64>> numbers = numbersOf(text);
    if (!numbers.has_value() || numbers->empty() || (numbers->size() != 1 && numbers->size() != width))
        return std::nullopt;
    if (width == 1)
        return numberLiteral((*numbers)[0]);
    std::string out(typeName(width));
    out.push_back('(');
    for (u32 part = 0; part < width; ++part) {
        if (part != 0)
            out += ", ";
        out += numberLiteral((*numbers)[numbers->size() == 1 ? 0 : part]);
    }
    out.push_back(')');
    return out;
}

// A local's name from what a person called the node: letters and digits,
// the first of them small.
[[nodiscard]] std::string localFrom(std::string_view title, std::string_view kind)
{
    std::string out;
    bool raise = false;
    for (const char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(raise ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
            raise = false;
        }
        else if (!out.empty()) {
            raise = true;
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])) != 0)
        out = std::string(kind) + out;
    out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    return out;
}

constexpr std::string_view kTaken[] = {
    "float",     "float2",  "float3",  "float4", "int",      "bool",     "const",     "return",     "if",
    "else",      "for",     "while",   "do",     "switch",   "case",     "break",     "void",       "true",
    "false",     "inputs",  "surface", "vertex", "lerp",     "saturate", "dot",       "pow",        "min",
    "max",       "abs",     "sin",     "cos",    "frac",     "floor",    "step",      "smoothstep", "normalize",
    "length",    "cross",   "mul",     "struct", "static",   "inout",    "out",       "in",         "graphNoise",
    "graphHash", "discard", "clip",    "sample", "texture",  "line",     "point",     "triangle",   "matrix",
    "vector",    "double",  "half",    "uint",   "register", "pass",     "technique",
};

// --- Reading -------------------------------------------------------------------------

struct Reader
{
    SurfaceGraph graph;
    SurfaceGraphReadError error;

    bool fail(SurfaceGraphReadCode code, std::string where, std::string what = {})
    {
        error = {code, std::move(where), std::move(what)};
        return false;
    }

    bool keys(const JsonValue& json, const std::string& path, std::span<const std::string_view> known)
    {
        for (usize index = 0; index < json.size(); ++index) {
            const std::string_view key = json.keyAt(index);
            if (std::find(known.begin(), known.end(), key) == known.end())
                return fail(SurfaceGraphReadCode::UnknownKey,
                            path.empty() ? std::string(key) : path + "." + std::string(key));
        }
        return true;
    }

    bool readNode(const JsonValue& json, const std::string& path)
    {
        static constexpr std::array<std::string_view, 5> Keys{"id", "kind", "title", "at", "fields"};
        if (json.type() != JsonType::Object)
            return fail(SurfaceGraphReadCode::WrongType, path, "object");
        if (!keys(json, path, Keys))
            return false;
        SurfaceGraphNode node;
        if (json["id"].type() != JsonType::Number || json["id"].asInteger() <= 0)
            return fail(json.has("id") ? SurfaceGraphReadCode::WrongType : SurfaceGraphReadCode::MissingKey,
                        path + ".id", "a whole number above nought");
        node.id = static_cast<u32>(json["id"].asInteger());
        if (graph.node(node.id) != nullptr)
            return fail(SurfaceGraphReadCode::DuplicateId, path + ".id", std::to_string(node.id));
        if (json["kind"].type() != JsonType::String)
            return fail(json.has("kind") ? SurfaceGraphReadCode::WrongType : SurfaceGraphReadCode::MissingKey,
                        path + ".kind", "string");
        node.kind = std::string(json["kind"].asString());
        if (recipeOf(node.kind) == nullptr)
            return fail(SurfaceGraphReadCode::UnknownKind, path + ".kind", node.kind);
        if (json.has("title")) {
            if (json["title"].type() != JsonType::String)
                return fail(SurfaceGraphReadCode::WrongType, path + ".title", "string");
            node.title = std::string(json["title"].asString());
        }
        if (json.has("at")) {
            const JsonValue at = json["at"];
            if (at.type() != JsonType::Array || at.size() != 2 || at.at(0).type() != JsonType::Number ||
                at.at(1).type() != JsonType::Number)
                return fail(SurfaceGraphReadCode::WrongType, path + ".at", "two numbers");
            node.x = static_cast<f32>(at.at(0).asNumber());
            node.y = static_cast<f32>(at.at(1).asNumber());
        }
        if (json.has("fields")) {
            const JsonValue fields = json["fields"];
            if (fields.type() != JsonType::Object)
                return fail(SurfaceGraphReadCode::WrongType, path + ".fields", "object");
            for (usize index = 0; index < fields.size(); ++index) {
                const std::string_view key = fields.keyAt(index);
                if (fields[key].type() != JsonType::String)
                    return fail(SurfaceGraphReadCode::WrongType, path + ".fields." + std::string(key), "string");
                node.fields.push_back({std::string(key), std::string(fields[key].asString())});
            }
        }
        graph.nodes.push_back(std::move(node));
        return true;
    }

    bool readLink(const JsonValue& json, const std::string& path)
    {
        static constexpr std::array<std::string_view, 4> Keys{"from", "output", "to", "input"};
        if (json.type() != JsonType::Object)
            return fail(SurfaceGraphReadCode::WrongType, path, "object");
        if (!keys(json, path, Keys))
            return false;
        SurfaceGraphLink link;
        for (const std::string_view end : {std::string_view("from"), std::string_view("to")}) {
            if (json[end].type() != JsonType::Number)
                return fail(json.has(end) ? SurfaceGraphReadCode::WrongType : SurfaceGraphReadCode::MissingKey,
                            path + "." + std::string(end), "number");
            const u32 id = static_cast<u32>(json[end].asInteger());
            if (graph.node(id) == nullptr)
                return fail(SurfaceGraphReadCode::UnknownNode, path + "." + std::string(end), std::to_string(id));
            (end == "from" ? link.from : link.to) = id;
        }
        for (const std::string_view pin : {std::string_view("output"), std::string_view("input")}) {
            if (json[pin].type() != JsonType::String)
                return fail(json.has(pin) ? SurfaceGraphReadCode::WrongType : SurfaceGraphReadCode::MissingKey,
                            path + "." + std::string(pin), "string");
            (pin == "output" ? link.output : link.input) = std::string(json[pin].asString());
        }
        graph.links.push_back(std::move(link));
        return true;
    }

    bool read(const JsonValue& root)
    {
        static constexpr std::array<std::string_view, 4> Keys{"format", "version", "nodes", "links"};
        if (root.type() != JsonType::Object)
            return fail(SurfaceGraphReadCode::NotAGraph, "");
        if (root["format"].type() != JsonType::String || root["format"].asString() != SurfaceGraphFormat)
            return fail(SurfaceGraphReadCode::NotAGraph, "format", std::string(root["format"].asString()));
        if (root["version"].type() != JsonType::Number)
            return fail(root.has("version") ? SurfaceGraphReadCode::WrongType : SurfaceGraphReadCode::MissingKey,
                        "version", "number");
        if (root["version"].asNumber() != static_cast<f64>(SurfaceGraphVersion))
            return fail(SurfaceGraphReadCode::UnsupportedVersion, "version",
                        std::to_string(root["version"].asInteger()));
        if (!keys(root, "", Keys))
            return false;
        if (root.has("nodes")) {
            const JsonValue nodes = root["nodes"];
            if (nodes.type() != JsonType::Array)
                return fail(SurfaceGraphReadCode::WrongType, "nodes", "array");
            for (usize index = 0; index < nodes.size(); ++index) {
                if (!readNode(nodes.at(index), "nodes[" + std::to_string(index) + "]"))
                    return false;
            }
        }
        if (root.has("links")) {
            const JsonValue links = root["links"];
            if (links.type() != JsonType::Array)
                return fail(SurfaceGraphReadCode::WrongType, "links", "array");
            for (usize index = 0; index < links.size(); ++index) {
                if (!readLink(links.at(index), "links[" + std::to_string(index) + "]"))
                    return false;
            }
        }
        return true;
    }
};

// --- Writing the shader -----------------------------------------------------------

// One function of the surface being written.
enum class Stage : core::u8
{
    Vertex,
    Fragment,
};

struct Writer
{
    explicit Writer(const SurfaceGraph& of) : graph(of) {}

    const SurfaceGraph& graph;
    WrittenSurface out;
    // What each parameter and texture was declared as, by name: two nodes of
    // one name are one parameter if they say the same thing.
    std::vector<std::pair<std::string, std::string>> declared;
    std::vector<std::string> declarations;
    bool noise = false;

    // In the function being written.
    Stage stage = Stage::Fragment;
    std::vector<std::pair<u32, std::string>> locals;
    std::vector<std::pair<u32, u32>> widths;
    std::vector<u32> visiting;
    std::vector<std::string> taken;
    std::vector<std::pair<std::string, u32>> lines;

    void issue(u32 node, SurfaceGraphIssueCode code, std::string where = {}, std::string what = {})
    {
        for (const SurfaceGraphIssue& held : out.issues) {
            if (held.node == node && held.code == code && held.where == where)
                return;
        }
        out.issues.push_back({node, code, std::move(where), std::move(what)});
    }

    [[nodiscard]] const SurfaceGraphLink* linkInto(u32 node, std::string_view input) const noexcept
    {
        for (const SurfaceGraphLink& link : graph.links) {
            if (link.to == node && link.input == input)
                return &link;
        }
        return nullptr;
    }

    // A parameter or a texture, declared once. False where its name is taken
    // by something that is not the same thing.
    bool declare(u32 node, const std::string& name, std::string declaration)
    {
        if (!isName(name) || std::find(std::begin(kTaken), std::end(kTaken), name) != std::end(kTaken)) {
            issue(node, SurfaceGraphIssueCode::BadName, "Name", name);
            return false;
        }
        for (const auto& [held, text] : declared) {
            if (held != name)
                continue;
            if (text != declaration) {
                issue(node, SurfaceGraphIssueCode::NameTaken, "Name", name);
                return false;
            }
            return true;
        }
        declared.emplace_back(name, declaration);
        declarations.push_back(std::move(declaration));
        return true;
    }

    [[nodiscard]] std::string freshLocal(const SurfaceGraphNode& node)
    {
        std::string base = localFrom(node.title, node.kind);
        const auto used = [this](const std::string& name) {
            if (std::find(std::begin(kTaken), std::end(kTaken), name) != std::end(kTaken))
                return true;
            if (std::find(taken.begin(), taken.end(), name) != taken.end())
                return true;
            return std::any_of(declared.begin(), declared.end(),
                               [&name](const auto& held) { return held.first == name; });
        };
        std::string name = base;
        for (u32 suffix = 2; used(name); ++suffix)
            name = base + std::to_string(suffix);
        taken.push_back(name);
        return name;
    }

    struct Value
    {
        std::string text;
        u32 width = 1;
    };

    // What an output of a node is, in this function: the text that names it
    // and how wide it is. Writes the node's statement the first time.
    Value output(u32 id, std::string_view pin)
    {
        const SurfaceGraphNode* const node = graph.node(id);
        const Recipe* const recipe = node != nullptr ? recipeOf(node->kind) : nullptr;
        if (recipe == nullptr)
            return {"0.0f", 1};
        u32 index = 0;
        for (; index < recipe->kind.outputs.size(); ++index) {
            if (recipe->kind.outputs[index].name == pin)
                break;
        }
        if (index >= recipe->kind.outputs.size()) {
            issue(id, SurfaceGraphIssueCode::NoSuchPin, std::string(pin));
            return {"0.0f", 1};
        }
        if ((recipe->kind.stage == GraphStage::VertexOnly && stage != Stage::Vertex) ||
            (recipe->kind.stage == GraphStage::FragmentOnly && stage != Stage::Fragment)) {
            issue(id, SurfaceGraphIssueCode::WrongStage, node->kind);
            return {"0.0f", 1};
        }
        // Read off the surface's inputs: no statement.
        if (!recipe->reads.empty())
            return {std::string(recipe->reads), static_cast<u32>(recipe->statement)};
        if (node->kind == "Parameter")
            return parameter(*node);
        if (node->kind == "Constant")
            return constant(*node);

        if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) {
            issue(id, SurfaceGraphIssueCode::Cycle);
            return {"0.0f", 1};
        }
        std::string local;
        u32 width = 0;
        for (const auto& [held, name] : locals) {
            if (held == id)
                local = name;
        }
        for (const auto& [held, wide] : widths) {
            if (held == id)
                width = wide;
        }
        if (local.empty()) {
            visiting.push_back(id);
            const Value made = statement(*node, *recipe);
            visiting.pop_back();
            local = made.text;
            width = made.width;
            locals.emplace_back(id, local);
            widths.emplace_back(id, width);
        }
        if (recipe->suffixes.empty())
            return {local, width};
        // A part of what the statement made.
        const GraphWidth part = recipe->kind.outputs[index].width;
        const u32 needed = node->kind == "Split" ? index + 1 : 0;
        if (needed > width) {
            issue(id, SurfaceGraphIssueCode::WidthsDoNotFit, std::string(pin), std::string(typeName(width)));
            return {"0.0f", 1};
        }
        return {local + std::string(recipe->suffixes[index]), static_cast<u32>(part)};
    }

    Value parameter(const SurfaceGraphNode& node)
    {
        const std::string name(node.field("Name"));
        const std::string_view type = node.field("Type").empty() ? std::string_view("number") : node.field("Type");
        const u32 width = widthNamed(type);
        if (width == 0) {
            issue(node.id, SurfaceGraphIssueCode::BadValue, "Type", std::string(type));
            return {"0.0f", 1};
        }
        std::string declaration = "ENG_PARAM(";
        if (type == "toggle") {
            const std::string_view initial = node.field("Default");
            declaration += "bool, " + name + ", " +
                           (initial == "0" || initial == "false" || initial.empty() ? "false" : "true") + ", toggle)";
        }
        else {
            const std::optional<std::string> initial =
                literalOf(node.field("Default").empty() ? std::string_view("0") : node.field("Default"), width);
            if (!initial.has_value()) {
                issue(node.id, SurfaceGraphIssueCode::BadValue, "Default", std::string(node.field("Default")));
                return {"0.0f", 1};
            }
            // The contract's defaults are plain numbers, with no suffix.
            std::string plain = *initial;
            std::erase(plain, 'f');
            // ... but `float3(` keeps its own f.
            if (width > 1)
                plain = std::string(typeName(width)) + plain.substr(plain.find('('));
            declaration += std::string(typeName(width)) + ", " + name + ", " + plain;
            if (type == "colour" || type == "color")
                declaration += ", colour";
            else if (width == 1 && !node.field("Range").empty()) {
                const std::optional<std::vector<f64>> range = numbersOf(node.field("Range"));
                if (!range.has_value() || range->size() != 2 || !((*range)[0] < (*range)[1])) {
                    issue(node.id, SurfaceGraphIssueCode::BadValue, "Range", std::string(node.field("Range")));
                    return {"0.0f", 1};
                }
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), ", range(%g, %g)", (*range)[0], (*range)[1]);
                declaration += buffer;
            }
            declaration += ")";
        }
        if (!declare(node.id, name, std::move(declaration)))
            return {"0.0f", 1};
        if (type == "toggle")
            return {"(" + name + " ? 1.0f : 0.0f)", 1};
        return {name, width};
    }

    Value constant(const SurfaceGraphNode& node)
    {
        const std::string_view type = node.field("Type").empty() ? std::string_view("number") : node.field("Type");
        const u32 width = widthNamed(type);
        const std::optional<std::string> literal = width != 0 ? literalOf(node.field("Value"), width) : std::nullopt;
        if (!literal.has_value()) {
            issue(node.id, SurfaceGraphIssueCode::BadValue, width == 0 ? "Type" : "Value",
                  std::string(width == 0 ? type : node.field("Value")));
            return {"0.0f", 1};
        }
        return {*literal, width};
    }

    // What goes into an input: what is linked to it, or what it is with
    // nothing linked.
    Value input(const SurfaceGraphNode& node, const SurfaceNodePin& pin, bool required = true)
    {
        Value value;
        if (const SurfaceGraphLink* const link = linkInto(node.id, pin.name); link != nullptr) {
            value = output(link->from, link->output);
        }
        else if (!pin.fallback.empty()) {
            value.text = std::string(pin.fallback);
            // A number, unless it reads something off the surface or is
            // written as a vector: then it is as wide as its pin.
            const bool wide = value.text.starts_with("inputs.") || value.text.starts_with("float");
            value.width = wide && pin.width != GraphWidth::Widest ? static_cast<u32>(pin.width) : 1u;
        }
        else {
            if (required)
                issue(node.id, SurfaceGraphIssueCode::InputMissing, std::string(pin.name));
            return {"0.0f", 1};
        }
        // A pin of a width takes that width, or a number, which widens.
        if (pin.width != GraphWidth::Widest && value.width != 1 && value.width != static_cast<u32>(pin.width)) {
            issue(node.id, SurfaceGraphIssueCode::WidthsDoNotFit, std::string(pin.name),
                  std::string(typeName(value.width)));
            return {"0.0f", 1};
        }
        return value;
    }

    // Wrapped where it is more than a name, so it can be put inside another.
    [[nodiscard]] static std::string wrapped(const std::string& text)
    {
        const bool simple = std::all_of(text.begin(), text.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
        });
        const bool call = !text.empty() && text.back() == ')' && text.find(' ') == std::string::npos &&
                          std::count(text.begin(), text.end(), '(') == 1;
        return simple || call ? text : "(" + text + ")";
    }

    // Writes one node's statement and says the local it made.
    Value statement(const SurfaceGraphNode& node, const Recipe& recipe)
    {
        std::string expression;
        u32 width = 1;

        if (node.kind == "Texture" || node.kind == "NormalMap") {
            const std::string name(node.field("Name"));
            const bool normal = node.kind == "NormalMap";
            if (!declare(node.id, name, "ENG_TEXTURE(" + name + (normal ? ", normal)" : ")")))
                return {"0.0f", 1};
            const Value uv = input(node, recipe.kind.inputs[0]);
            const std::string sample = stage == Stage::Vertex ? "ENG_SAMPLE_LEVEL(" + name + ", " + uv.text + ", 0.0f)"
                                                              : "ENG_SAMPLE(" + name + ", " + uv.text + ")";
            if (normal) {
                const Value strength = input(node, recipe.kind.inputs[1]);
                expression = "surfaceNormalFromMap(" + sample + ".rgb, " + strength.text + ", inputs)";
                width = 3;
            }
            else {
                expression = sample;
                width = 4;
            }
        }
        else if (node.kind == "Combine") {
            const std::optional<std::vector<f64>> asked =
                numbersOf(node.field("Width").empty() ? "3" : node.field("Width"));
            width = asked.has_value() && asked->size() == 1 ? static_cast<u32>((*asked)[0]) : 0u;
            if (width < 2 || width > 4) {
                issue(node.id, SurfaceGraphIssueCode::BadValue, "Width", std::string(node.field("Width")));
                return {"0.0f", 1};
            }
            expression = std::string(typeName(width)) + "(";
            for (u32 part = 0; part < width; ++part) {
                if (part != 0)
                    expression += ", ";
                expression += input(node, recipe.kind.inputs[part]).text;
            }
            expression += ")";
        }
        else if (node.kind == "Expression") {
            const std::string code(node.field("Code"));
            const std::string_view type = node.field("Type").empty() ? std::string_view("number") : node.field("Type");
            width = widthNamed(type);
            if (width == 0) {
                issue(node.id, SurfaceGraphIssueCode::BadValue, "Type", std::string(type));
                return {"0.0f", 1};
            }
            if (code.find_first_not_of(" \t") == std::string::npos) {
                issue(node.id, SurfaceGraphIssueCode::EmptyExpression, "Code");
                return {"0.0f", 1};
            }
            // The line as it was typed, with each of its inputs' names put in
            // for the letter that stands for it -- a letter by itself, and
            // not the one after a dot.
            usize at = 0;
            while (at < code.size()) {
                const char c = code[at];
                if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
                    usize end = at;
                    while (end < code.size() &&
                           (std::isalnum(static_cast<unsigned char>(code[end])) != 0 || code[end] == '_'))
                        ++end;
                    const std::string_view word(code.data() + at, end - at);
                    const bool member = at > 0 && code[at - 1] == '.';
                    const SurfaceNodePin* pin = nullptr;
                    for (const SurfaceNodePin& held : recipe.kind.inputs) {
                        if (held.name == word && !member)
                            pin = &held;
                    }
                    if (pin != nullptr)
                        expression += wrapped(input(node, *pin).text);
                    else
                        expression.append(word);
                    at = end;
                }
                else {
                    expression.push_back(c);
                    ++at;
                }
            }
        }
        else {
            // The recipe's own expression, each `{Pin}` what is put into it;
            // as wide as the widest of them where the node is that.
            u32 widest = 1;
            bool fits = true;
            usize at = 0;
            const std::string_view form = recipe.expression;
            while (at < form.size()) {
                if (form[at] != '{') {
                    expression.push_back(form[at++]);
                    continue;
                }
                const usize end = form.find('}', at);
                const std::string_view name = form.substr(at + 1, end - at - 1);
                at = end + 1;
                const SurfaceNodePin* pin = nullptr;
                for (const SurfaceNodePin& held : recipe.kind.inputs) {
                    if (held.name == name)
                        pin = &held;
                }
                if (pin == nullptr)
                    continue;
                const Value value = input(node, *pin);
                if (pin->width == GraphWidth::Widest && value.width != 1) {
                    if (widest != 1 && widest != value.width)
                        fits = false;
                    widest = std::max(widest, value.width);
                }
                // Alone in the expression it needs no brackets of its own.
                expression += form == "{A}" ? value.text : wrapped(value.text);
            }
            if (!fits) {
                issue(node.id, SurfaceGraphIssueCode::WidthsDoNotFit, "", std::string(typeName(widest)));
                return {"0.0f", 1};
            }
            width = recipe.statement == GraphWidth::Widest ? widest : static_cast<u32>(recipe.statement);
            if (node.kind == "Noise")
                noise = true;
        }

        const std::string local = freshLocal(node);
        lines.emplace_back("    const " + std::string(typeName(width)) + " " + local + " = " + expression + ";",
                           node.id);
        return {local, width};
    }

    // One of the surface's two functions, from its end back. Nothing where
    // the graph has no such end or nothing goes into it.
    [[nodiscard]] std::vector<std::pair<std::string, u32>> function(Stage which, const SurfaceGraphNode* end)
    {
        stage = which;
        locals.clear();
        widths.clear();
        visiting.clear();
        taken.clear();
        lines.clear();
        if (end == nullptr)
            return {};
        const Recipe& recipe = *recipeOf(end->kind);
        std::vector<std::pair<std::string, u32>> ends;
        for (const SurfaceNodePin& pin : recipe.kind.inputs) {
            if (linkInto(end->id, pin.name) == nullptr)
                continue;
            const Value value = input(*end, pin);
            if (which == Stage::Vertex) {
                ends.emplace_back(pin.name == "Offset" ? "    vertex.Position += " + value.text + ";"
                                                       : "    vertex.Normal = normalize(" + value.text + ");",
                                  end->id);
            }
            else {
                ends.emplace_back("    surface." + std::string(pin.name) + " = " + value.text + ";", end->id);
            }
        }
        std::vector<std::pair<std::string, u32>> body = lines;
        body.insert(body.end(), ends.begin(), ends.end());
        return body;
    }

    void line(std::string text, u32 node = 0)
    {
        out.hlsl += text;
        out.hlsl.push_back('\n');
        out.lineNodes.push_back(node);
    }
};

} // namespace

bool isSurfaceGraphPath(std::string_view path) noexcept
{
    return path.size() > SurfaceGraphSuffix.size() && path.ends_with(SurfaceGraphSuffix);
}

std::string surfaceShaderPathOf(std::string_view graphPath)
{
    if (!isSurfaceGraphPath(graphPath))
        return {};
    return std::string(graphPath.substr(0, graphPath.size() - SurfaceGraphSuffix.size())) + ".surface.hlsl";
}

std::span<const SurfaceNodeKind> surfaceNodeKinds() noexcept
{
    return allKinds();
}

const SurfaceNodeKind* surfaceNodeKind(std::string_view name) noexcept
{
    const Recipe* const recipe = recipeOf(name);
    return recipe != nullptr ? &recipe->kind : nullptr;
}

std::string_view SurfaceGraphNode::field(std::string_view name) const noexcept
{
    for (const SurfaceGraphField& held : fields) {
        if (held.name == name)
            return held.value;
    }
    // What the kind starts a new node's field at.
    if (const SurfaceNodeKind* const known = surfaceNodeKind(kind); known != nullptr) {
        for (const SurfaceNodeField& held : known->fields) {
            if (held.name == name)
                return held.initial;
        }
    }
    return {};
}

const SurfaceGraphNode* SurfaceGraph::node(core::u32 id) const noexcept
{
    for (const SurfaceGraphNode& held : nodes) {
        if (held.id == id)
            return &held;
    }
    return nullptr;
}

std::optional<SurfaceGraph> readSurfaceGraph(std::string_view json, SurfaceGraphReadError* error)
{
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json); !parsed.ok) {
        if (error != nullptr)
            *error = {SurfaceGraphReadCode::Malformed, "", parsed.diagnostic};
        return std::nullopt;
    }
    Reader reader;
    if (!reader.read(document.root())) {
        if (error != nullptr)
            *error = reader.error;
        return std::nullopt;
    }
    return std::move(reader.graph);
}

std::string writeSurfaceGraph(const SurfaceGraph& graph)
{
    core::JsonWriter json(core::JsonLayout::Indented);
    json.beginObject();
    json.field("format", SurfaceGraphFormat);
    json.field("version", SurfaceGraphVersion);
    json.key("nodes");
    json.beginArray();
    for (const SurfaceGraphNode& node : graph.nodes) {
        json.beginObject();
        json.field("id", static_cast<core::i64>(node.id));
        json.field("kind", node.kind);
        if (!node.title.empty())
            json.field("title", node.title);
        // A whole number of the canvas's units as a whole number: `200`,
        // where the shortest text a float goes back from is `2e+02`.
        const auto place = [&json](core::f32 at) {
            if (std::isfinite(at) && std::abs(at) < 1.0e9f && at == std::round(at))
                json.value(static_cast<core::i64>(at));
            else
                json.valueFloat(at);
        };
        json.key("at");
        json.beginInlineArray();
        place(node.x);
        place(node.y);
        json.endArray();
        if (!node.fields.empty()) {
            json.key("fields");
            json.beginObject();
            for (const SurfaceGraphField& field : node.fields)
                json.field(field.name, field.value);
            json.endObject();
        }
        json.endObject();
    }
    json.endArray();
    json.key("links");
    json.beginArray();
    for (const SurfaceGraphLink& link : graph.links) {
        json.beginObject();
        json.field("from", static_cast<core::i64>(link.from));
        json.field("output", link.output);
        json.field("to", static_cast<core::i64>(link.to));
        json.field("input", link.input);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    return json.text() + "\n";
}

WrittenSurface writeSurfaceShader(const SurfaceGraph& graph, std::string_view graphFile)
{
    Writer writer(graph);
    const SurfaceGraphNode* surfaceEnd = nullptr;
    const SurfaceGraphNode* vertexEnd = nullptr;
    for (const SurfaceGraphNode& node : graph.nodes) {
        const SurfaceGraphNode*& end = node.kind == SurfaceEndNode ? surfaceEnd : vertexEnd;
        if (node.kind != SurfaceEndNode && node.kind != VertexEndNode)
            continue;
        if (end != nullptr)
            writer.issue(node.id, SurfaceGraphIssueCode::TwoEnds, node.kind);
        else
            end = &node;
    }
    // A link to a pin its node does not have is said, wherever it is.
    for (const SurfaceGraphLink& link : graph.links) {
        const SurfaceGraphNode* const to = graph.node(link.to);
        const SurfaceNodeKind* const kind = to != nullptr ? surfaceNodeKind(to->kind) : nullptr;
        if (kind == nullptr)
            continue;
        const bool known = std::any_of(kind->inputs.begin(), kind->inputs.end(),
                                       [&link](const SurfaceNodePin& pin) { return pin.name == link.input; });
        if (!known)
            writer.issue(link.to, SurfaceGraphIssueCode::NoSuchPin, link.input);
    }

    // Both functions are worked out before a line is written: what they
    // declare -- parameters, textures, the noise -- stands above them.
    const std::vector<std::pair<std::string, u32>> vertex = writer.function(Stage::Vertex, vertexEnd);
    const std::vector<std::pair<std::string, u32>> fragment = writer.function(Stage::Fragment, surfaceEnd);
    writer.out.vertex = !vertex.empty();
    writer.out.fragment = !fragment.empty();

    writer.line("// Written by " + std::string(graphFile) + " -- edit the graph, not this file: it is written");
    writer.line("// again whenever the graph is saved, and a change made here is lost. To keep");
    writer.line("// the surface as code, delete the graph; this file is then yours.");
    writer.line("");
    if (!writer.out.vertex)
        writer.line("#define ENG_NO_VERTEX");
    if (!writer.out.fragment)
        writer.line("#define ENG_NO_FRAGMENT");
    writer.line("#include \"engine/surface.hlsli\"");
    if (!writer.declarations.empty()) {
        writer.line("");
        for (const std::string& declaration : writer.declarations)
            writer.line(declaration);
    }
    if (writer.noise) {
        writer.line("");
        for (const std::string_view helper : kNoiseHelpers)
            writer.line(std::string(helper));
    }
    if (writer.out.vertex) {
        writer.line("");
        writer.line("void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)");
        writer.line("{");
        for (const auto& [text, node] : vertex)
            writer.line(text, node);
        writer.line("}");
    }
    if (writer.out.fragment) {
        writer.line("");
        writer.line("void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)");
        writer.line("{");
        for (const auto& [text, node] : fragment)
            writer.line(text, node);
        writer.line("}");
    }
    return std::move(writer.out);
}

core::u32 surfaceOutputWidth(const SurfaceGraph& graph, core::u32 node, std::string_view output)
{
    const SurfaceGraphNode* const held = graph.node(node);
    const SurfaceNodeKind* const kind = held != nullptr ? surfaceNodeKind(held->kind) : nullptr;
    if (kind == nullptr)
        return 0;
    // Asked of whichever function the node may stand in, issues and all
    // thrown away: only the width is wanted.
    Writer writer(graph);
    writer.stage = kind->stage == GraphStage::VertexOnly ? Stage::Vertex : Stage::Fragment;
    const Writer::Value value = writer.output(node, output);
    for (const SurfaceGraphIssue& issue : writer.out.issues) {
        if (issue.node == node)
            return 0;
    }
    return value.width;
}

} // namespace engine::asset
