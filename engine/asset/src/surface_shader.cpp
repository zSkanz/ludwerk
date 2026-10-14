#include "engine/asset/surface_shader.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>

#include "engine/core/number_parse.h"

namespace engine::asset {

using core::f32;
using core::u32;

namespace {

// The source with every comment replaced by spaces, newlines kept, so a match's
// position is still its line.
[[nodiscard]] std::string withoutComments(std::string_view source)
{
    std::string out(source);
    for (std::size_t at = 0; at < out.size(); ++at) {
        if (out[at] == '"') {
            for (++at; at < out.size() && out[at] != '"' && out[at] != '\n'; ++at) {
            }
            continue;
        }
        if (out[at] == '/' && at + 1 < out.size() && out[at + 1] == '/') {
            for (; at < out.size() && out[at] != '\n'; ++at)
                out[at] = ' ';
            continue;
        }
        if (out[at] == '/' && at + 1 < out.size() && out[at + 1] == '*') {
            out[at] = ' ';
            out[at + 1] = ' ';
            for (at += 2; at < out.size(); ++at) {
                if (out[at] == '*' && at + 1 < out.size() && out[at + 1] == '/') {
                    out[at] = ' ';
                    out[at + 1] = ' ';
                    ++at;
                    break;
                }
                if (out[at] != '\n')
                    out[at] = ' ';
            }
        }
    }
    return out;
}

[[nodiscard]] bool identifierChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] bool isIdentifier(std::string_view text) noexcept
{
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0)
        return false;
    return std::all_of(text.begin(), text.end(), identifierChar);
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] u32 lineAt(std::string_view text, std::size_t at) noexcept
{
    return 1u + static_cast<u32>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

// Every whole-word occurrence of `word`, as offsets.
[[nodiscard]] std::vector<std::size_t> occurrences(std::string_view text, std::string_view word)
{
    std::vector<std::size_t> found;
    for (std::size_t at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1)) {
        const bool before = at == 0 || !identifierChar(text[at - 1]);
        const bool after = at + word.size() >= text.size() || !identifierChar(text[at + word.size()]);
        if (before && after)
            found.push_back(at);
    }
    return found;
}

// The arguments of a call whose name ends at `at`, split at top-level commas;
// empty when there is no well-formed parenthesised list.
[[nodiscard]] std::optional<std::vector<std::string_view>> argumentsAt(std::string_view text, std::size_t at)
{
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
        ++at;
    if (at >= text.size() || text[at] != '(')
        return std::nullopt;
    std::vector<std::string_view> arguments;
    int depth = 0;
    std::size_t start = at + 1;
    for (std::size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        }
        else if (c == ')') {
            if (--depth == 0) {
                arguments.push_back(trimmed(text.substr(start, i - start)));
                return arguments;
            }
        }
        else if (c == ',' && depth == 1) {
            arguments.push_back(trimmed(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<f32> number(std::string_view text) noexcept
{
    text = trimmed(text);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
        text.remove_suffix(1);
    if (text.empty())
        return std::nullopt;
    if (text.front() == '+')
        text.remove_prefix(1);
    // Not `std::from_chars`: its floating-point overloads are missing from the
    // Android NDK's standard library (see `core/number_parse.h`).
    core::f64 value = 0.0;
    if (text.empty() || !core::decimalToDouble(text, value))
        return std::nullopt;
    return static_cast<f32>(value);
}

[[nodiscard]] u32 componentsOf(SurfaceParamType type) noexcept
{
    switch (type) {
    case SurfaceParamType::Float2:
        return 2;
    case SurfaceParamType::Float3:
        return 3;
    case SurfaceParamType::Float4:
        return 4;
    default:
        return 1;
    }
}

[[nodiscard]] std::string_view hlslType(SurfaceParamType type) noexcept
{
    switch (type) {
    case SurfaceParamType::Float2:
        return "float2";
    case SurfaceParamType::Float3:
        return "float3";
    case SurfaceParamType::Float4:
        return "float4";
    case SurfaceParamType::Int:
        return "int";
    case SurfaceParamType::Bool:
        return "bool";
    default:
        return "float";
    }
}

[[nodiscard]] std::optional<SurfaceParamType> typeNamed(std::string_view name) noexcept
{
    if (name == "float")
        return SurfaceParamType::Float;
    if (name == "float2")
        return SurfaceParamType::Float2;
    if (name == "float3")
        return SurfaceParamType::Float3;
    if (name == "float4")
        return SurfaceParamType::Float4;
    if (name == "int")
        return SurfaceParamType::Int;
    if (name == "bool")
        return SurfaceParamType::Bool;
    return std::nullopt;
}

// A default: `0.5`, `true`, `3`, or `float3(0, 0.1, 0.2)` for its own type.
[[nodiscard]] bool parseDefault(std::string_view text, SurfaceParam& param)
{
    text = trimmed(text);
    if (param.type == SurfaceParamType::Bool) {
        if (text == "true" || text == "1") {
            param.value[0] = 1.0f;
            return true;
        }
        if (text == "false" || text == "0") {
            param.value[0] = 0.0f;
            return true;
        }
        return false;
    }
    const u32 components = componentsOf(param.type);
    if (components == 1) {
        const std::optional<f32> value = number(text);
        if (!value.has_value())
            return false;
        if (param.type == SurfaceParamType::Int && *value != static_cast<f32>(static_cast<int>(*value)))
            return false;
        param.value[0] = *value;
        return true;
    }
    const std::string_view prefix = hlslType(param.type);
    if (text.substr(0, prefix.size()) != prefix)
        return false;
    const std::optional<std::vector<std::string_view>> parts = argumentsAt(text, prefix.size());
    if (!parts.has_value() || parts->size() != components)
        return false;
    for (u32 index = 0; index < components; ++index) {
        const std::optional<f32> value = number((*parts)[index]);
        if (!value.has_value())
            return false;
        param.value[index] = *value;
    }
    return true;
}

[[nodiscard]] bool parseAnnotation(std::string_view text, SurfaceParam& param)
{
    text = trimmed(text);
    if (text == "colour" || text == "color") {
        param.annotation = SurfaceAnnotation::Colour;
        return param.type == SurfaceParamType::Float3 || param.type == SurfaceParamType::Float4;
    }
    if (text == "toggle") {
        param.annotation = SurfaceAnnotation::Toggle;
        return param.type == SurfaceParamType::Bool;
    }
    if (text.substr(0, 5) == "range") {
        const std::optional<std::vector<std::string_view>> bounds = argumentsAt(text, 5);
        if (!bounds.has_value() || bounds->size() != 2)
            return false;
        const std::optional<f32> low = number((*bounds)[0]);
        const std::optional<f32> high = number((*bounds)[1]);
        if (!low.has_value() || !high.has_value() || !(*low < *high))
            return false;
        param.annotation = SurfaceAnnotation::Range;
        param.minimum = *low;
        param.maximum = *high;
        return param.type == SurfaceParamType::Float || param.type == SurfaceParamType::Int;
    }
    return false;
}

// Tokens a surface must not write: the layout is the engine's.
constexpr std::string_view Forbidden[] = {
    "register",   "cbuffer",      "tbuffer",   "SamplerState", "SamplerComparisonState",
    "Texture2D",  "TextureCube",  "Texture3D", "RWTexture2D",  "StructuredBuffer",
    "VertexMain", "FragmentMain",
};

} // namespace

const SurfaceParam* SurfaceReflection::param(std::string_view name) const noexcept
{
    for (const SurfaceParam& entry : params) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

SurfaceReflection reflectSurface(std::string_view source)
{
    SurfaceReflection reflection;
    const std::string text = withoutComments(source);
    const std::string_view view = text;
    const auto error = [&](std::size_t at, std::string_view key, std::string_view subject) {
        reflection.errors.push_back(SurfaceDiagnostic{lineAt(view, at), std::string(key), std::string(subject)});
    };

    for (const std::string_view word : Forbidden) {
        for (const std::size_t at : occurrences(view, word))
            error(at, "asset.err.surface_forbidden", word);
    }

    // **Parameters, in the order written**, packed as a cbuffer packs them: no
    // value straddles a 16-byte row.
    u32 offset = SurfaceBlockHeaderBytes;
    for (const std::size_t at : occurrences(view, "ENG_PARAM")) {
        // The one in the contract header's own `#define` is not a declaration.
        const std::size_t lineStart = view.rfind('\n', at);
        const std::string_view before =
            trimmed(view.substr(lineStart == std::string_view::npos ? 0 : lineStart + 1,
                                at - (lineStart == std::string_view::npos ? 0 : lineStart + 1)));
        if (before == "#define")
            continue;
        const std::optional<std::vector<std::string_view>> arguments =
            argumentsAt(view, at + std::string_view("ENG_PARAM").size());
        if (!arguments.has_value() || arguments->size() < 3 || arguments->size() > 4) {
            error(at, "asset.err.surface_param_shape", "ENG_PARAM");
            continue;
        }
        SurfaceParam param;
        param.line = lineAt(view, at);
        const std::optional<SurfaceParamType> type = typeNamed((*arguments)[0]);
        if (!type.has_value()) {
            error(at, "asset.err.surface_param_type", (*arguments)[0]);
            continue;
        }
        param.type = *type;
        param.name = std::string((*arguments)[1]);
        if (!isIdentifier(param.name) || param.name.rfind("Engine", 0) == 0 ||
            reflection.param(param.name) != nullptr) {
            error(at, "asset.err.surface_param_name", param.name);
            continue;
        }
        if (!parseDefault((*arguments)[2], param)) {
            error(at, "asset.err.surface_param_default", param.name);
            continue;
        }
        if (arguments->size() == 4 && !parseAnnotation((*arguments)[3], param)) {
            error(at, "asset.err.surface_param_annotation", param.name);
            continue;
        }
        const u32 bytes = 4u * componentsOf(param.type);
        if ((offset % 16u) + bytes > 16u)
            offset = (offset + 15u) & ~15u;
        param.offset = offset;
        offset += bytes;
        reflection.params.push_back(std::move(param));
    }
    reflection.blockBytes = (offset + 15u) & ~15u;
    if (reflection.blockBytes > MaxSurfaceBlockBytes)
        error(0, "asset.err.surface_block_size", std::to_string(reflection.blockBytes));

    for (const std::size_t at : occurrences(view, "ENG_TEXTURE")) {
        const std::size_t lineStart = view.rfind('\n', at);
        const std::size_t from = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        if (trimmed(view.substr(from, at - from)) == "#define")
            continue;
        const std::optional<std::vector<std::string_view>> arguments =
            argumentsAt(view, at + std::string_view("ENG_TEXTURE").size());
        if (!arguments.has_value() || arguments->empty() || arguments->size() > 2) {
            error(at, "asset.err.surface_param_shape", "ENG_TEXTURE");
            continue;
        }
        SurfaceTexture texture;
        texture.line = lineAt(view, at);
        texture.name = std::string((*arguments)[0]);
        const bool taken = reflection.param(texture.name) != nullptr ||
                           std::any_of(reflection.textures.begin(), reflection.textures.end(),
                                       [&](const SurfaceTexture& other) { return other.name == texture.name; });
        if (!isIdentifier(texture.name) || texture.name.rfind("Engine", 0) == 0 || taken) {
            error(at, "asset.err.surface_param_name", texture.name);
            continue;
        }
        if (arguments->size() == 2) {
            const std::string_view fallback = (*arguments)[1];
            if (fallback == "black")
                texture.fallback = SurfaceTextureDefault::Black;
            else if (fallback == "normal")
                texture.fallback = SurfaceTextureDefault::Normal;
            else if (fallback != "white") {
                error(at, "asset.err.surface_param_annotation", texture.name);
                continue;
            }
        }
        if (reflection.textures.size() == MaxSurfaceTextures) {
            error(at, "asset.err.surface_texture_count", texture.name);
            continue;
        }
        reflection.textures.push_back(std::move(texture));
    }

    const auto defines = [&](std::string_view function) {
        for (const std::size_t at : occurrences(view, function)) {
            const std::size_t lineStart = view.rfind('\n', at);
            const std::size_t from = lineStart == std::string_view::npos ? 0 : lineStart + 1;
            const std::string_view head = trimmed(view.substr(from, at - from));
            if (head == "void" && argumentsAt(view, at + function.size()).has_value())
                return true;
        }
        return false;
    };
    reflection.hasVertex = defines("surfaceVertex");
    reflection.hasFragment = defines("surfaceFragment");
    return reflection;
}

namespace {

void appendBlock(std::string& out, const SurfaceReflection& reflection, std::string_view registerName)
{
    out += "cbuffer EngineSurfaceBlock : register(";
    out += registerName;
    out += ")\n{\n    float4 EngineSurfaceClock;\n    uint4 EngineSurfaceTextures;\n"
           "    float4 EngineSurfaceWind;\n    float4 EngineSurfaceWindShape;\n";
    for (const SurfaceParam& param : reflection.params) {
        out += "    ";
        out += hlslType(param.type);
        out += ' ';
        out += param.name;
        out += ";\n";
    }
    out += "};\n\n";
}

void appendTextures(std::string& out, const SurfaceReflection& reflection, bool fragment, std::string_view space)
{
    for (u32 index = 0; index < reflection.textures.size(); ++index) {
        const std::string slot = std::to_string(fragment ? surfaceFragmentSlot(index) : index);
        const std::string& name = reflection.textures[index].name;
        out += "static const uint " + name + "Bit = " + std::to_string(1u << index) + "u;\n";
        out += "Texture2D " + name + " : register(t" + slot + ", " + std::string(space) + ");\n";
        out += "SamplerState " + name + "Sampler : register(s" + slot + ", " + std::string(space) + ");\n";
    }
    out += '\n';
}

[[nodiscard]] bool instanced(SurfaceVariant variant) noexcept
{
    return variant == SurfaceVariant::ForwardInstanced || variant == SurfaceVariant::DepthInstanced;
}

[[nodiscard]] bool depthOnly(SurfaceVariant variant) noexcept
{
    return variant == SurfaceVariant::Depth || variant == SurfaceVariant::DepthInstanced;
}

constexpr std::string_view VertexInput = R"(struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Tangent : TEXCOORD2;
    float2 Uv : TEXCOORD3;
#if defined(ENG_SURFACE_INSTANCED)
    float4 ModelColumn0 : TEXCOORD4;
    float4 ModelColumn1 : TEXCOORD5;
    float4 ModelColumn2 : TEXCOORD6;
    float4 ModelColumn3 : TEXCOORD7;
    float4 InstanceAlphaTint : TEXCOORD8;
#endif
};

// The vertex as the mesh gave it, and what a surface reads about it before it
// moves.
SurfaceVertex engineSurfaceVertex(VertexInput input)
{
    SurfaceVertex vertex;
    vertex.Position = input.Position;
    vertex.Normal = input.Normal;
    vertex.Tangent = input.Tangent;
    vertex.Uv0 = input.Uv;
    vertex.Uv1 = input.Uv;
    vertex.Color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    return vertex;
}

SurfaceInputs engineVertexInputs(SurfaceVertex vertex, float4x4 model)
{
    SurfaceInputs inputs;
    inputs.Time = EngineSurfaceClock.x;
    inputs.CameraPosition = EngineSurfaceClock.yzw;
    inputs.ObjectToWorld = model;
    // The draw is relative to the camera (the renderer's floating origin); the
    // world is not.
    inputs.WorldPosition = mul(model, float4(vertex.Position, 1.0f)).xyz + EngineSurfaceClock.yzw;
    inputs.WorldNormal = normalize(mul((float3x3)model, vertex.Normal));
    inputs.WorldTangent = float4(mul((float3x3)model, vertex.Tangent.xyz), vertex.Tangent.w);
    inputs.Uv0 = vertex.Uv0;
    inputs.Uv1 = vertex.Uv1;
    inputs.VertexColor = vertex.Color;
    inputs.ScreenUv = float2(0.0f, 0.0f);
    inputs.ScreenPosition = float4(0.0f, 0.0f, 0.0f, 0.0f);
    inputs.SceneDepth = 3.0e38f;
    inputs.SceneColor = float3(0.0f, 0.0f, 0.0f);
    inputs.Wind = engineWindAt(EngineSurfaceWind.xyz, EngineSurfaceWind.w, EngineSurfaceWindShape.x,
                               inputs.WorldPosition, inputs.Time);
    return inputs;
}

float4x4 engineModel(VertexInput input)
{
#if defined(ENG_SURFACE_INSTANCED)
    return transpose(float4x4(input.ModelColumn0, input.ModelColumn1, input.ModelColumn2, input.ModelColumn3));
#elif defined(ENG_SURFACE_DEPTH)
    return ShadowModel;
#else
    return Model;
#endif
}

)";

constexpr std::string_view ForwardVertex = R"(Interpolants VertexMain(VertexInput input)
{
    const float4x4 model = engineModel(input);
    SurfaceVertex vertex = engineSurfaceVertex(input);
    surfaceVertex(vertex, engineVertexInputs(vertex, model));

    Interpolants output;
    const float4 shadingPosition = mul(model, float4(vertex.Position, 1.0f));
    output.ShadingPosition = shadingPosition.xyz;
    output.Position = mul(ViewProjection, shadingPosition);
    output.ViewDepth = output.Position.w;
#if defined(ENG_SURFACE_INSTANCED)
    const float3 a = input.ModelColumn0.xyz;
    const float3 b = input.ModelColumn1.xyz;
    const float3 c = input.ModelColumn2.xyz;
    // Transposed: the cross products are the cofactor's columns and the
    // constructor fills rows. Without it an instanced part's normal
    // was turned by the inverse of the part's rotation.
    output.Normal = mul(transpose(float3x3(cross(b, c), cross(c, a), cross(a, b))), vertex.Normal);
    output.InstanceAlpha = input.InstanceAlphaTint.x;
#else
    output.Normal = mul((float3x3)NormalMatrix, vertex.Normal);
    output.InstanceAlpha = InstanceAlphaUnused.x;
#endif
    output.Tangent = float4(mul((float3x3)model, vertex.Tangent.xyz), vertex.Tangent.w);
    output.Uv = vertex.Uv0;
    return output;
}
)";

constexpr std::string_view DepthVertex = R"(struct Interpolants
{
    float4 Position : SV_Position;
};

Interpolants VertexMain(VertexInput input)
{
    const float4x4 model = engineModel(input);
    SurfaceVertex vertex = engineSurfaceVertex(input);
    surfaceVertex(vertex, engineVertexInputs(vertex, model));
    Interpolants output;
    output.Position = mul(LightViewProjection, mul(model, float4(vertex.Position, 1.0f)));
    return output;
}
)";

constexpr std::string_view SceneReflection =
    R"(// **The scene in a mirror** (ADR 0209, contract 1.2). A blended surface is
// not in the depth the screen's own pass marches, but it knows where it is and
// which way it faces: the ray is marched here, through the scene's depth as it
// was copied for this pass, and what it lands on is read from the scene's
// colour of THIS frame -- which is there, since everything opaque is drawn.
float4 sceneReflectionAt(SurfaceInputs inputs, float3 normal)
{
    // **The setting's, as the screen's own pass is**: no steps is no march,
    // and the surface keeps the sky and the probes
    // (`GraphicsService.ScreenSpaceReflections`).
    const int steps = min(int(ScreenLightParams.w + 0.5f), 32);
    if (steps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    // And the material's: one that did not ask for the scene's colour
    // (`readsSceneColor`) is handed a black pixel in its place, and a ray
    // that landed would land on black. No colour to read is no answer.
    uint colourWidth = 0;
    uint colourHeight = 0;
    EngineSceneColor.GetDimensions(colourWidth, colourHeight);
    if (colourWidth <= 1u)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    // The world as the camera has it at its origin.
    const float3 origin = inputs.WorldPosition - inputs.CameraPosition;
    const float3 incoming = normalize(origin);
    const float3 ray = reflect(incoming, normalize(normal));
    // **A ray leaves the surface on the side it faces, or the screen has no
    // answer for it.** A wave's slope turns some rays down into the water,
    // and marched they land on the lake's own bed, which the scene behind
    // the water holds: sand-coloured streaks lying on the water away from
    // any shore. Such a ray would meet the water again, which is not in the
    // scene behind the water. The surface's own plane is the mesh's, not the
    // wave's.
    if (dot(ray, normalize(inputs.WorldNormal)) <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float distance = max(inputs.ScreenPosition.w, 0.1f);
    // A lake's far bank is far: the ray reaches further than a room's.
    const float reach = min(300.0f, 30.0f + distance * 6.0f);
    // **Stepped along the screen, not along the ray.** A step that is so many
    // metres is, far out, a step of many pixels: the ray strode over a post
    // on the far bank or happened to stop behind it, by which pixel it began
    // at, and the post stood in the water as a stack of chips. The ray is a
    // line on the screen too; walked there in even steps, each is the same
    // few pixels wherever it is, and along it one over the depth runs even
    // as well, which is all a ray's depth at a place on the screen is.
    const float4 from = mul(ScreenViewProjection, float4(origin, 1.0f));
    float4 to = mul(ScreenViewProjection, float4(origin + ray * reach, 1.0f));
    if (from.w <= 0.05f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    // Held in front of the camera: a ray that comes back past it ends there.
    if (to.w < 0.05f)
        to = lerp(from, to, (from.w - 0.05f) / max(from.w - to.w, 1e-6f));
    const float2 startAt = from.xy / from.w;
    const float2 across = to.xy / to.w - startAt;
    const float startOver = 1.0f / from.w;
    const float endOver = 1.0f / to.w;
    // And no further than the picture's edge: every step is one on the screen.
    float most = 1.0f;
    if (across.x > 1e-6f)
        most = min(most, (1.0f - startAt.x) / across.x);
    else if (across.x < -1e-6f)
        most = min(most, (-1.0f - startAt.x) / across.x);
    if (across.y > 1e-6f)
        most = min(most, (1.0f - startAt.y) / across.y);
    else if (across.y < -1e-6f)
        most = min(most, (-1.0f - startAt.y) / across.y);
    most = saturate(most);
    const float jitter = frac(52.9829189f * frac(dot(inputs.ScreenPosition.xy, float2(0.06711056f, 0.00583715f))));
    float before = 0.0f;
    float depthBefore = from.w;
    float landed = -1.0f;
    [loop]
    for (int i = 0; i < 32; ++i)
    {
        if (i >= steps)
            break;
        const float along = most * (float(i) + jitter) / float(steps);
        const float2 ndc = startAt + across * along;
        const float depth = 1.0f / lerp(startOver, endOver, along);
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        const float behind = depth - sceneDepthAt(uv);
        // Behind something, and by no more than a step could have carried it.
        if (behind > 0.0f && behind < 0.5f + depth * 0.1f + abs(depth - depthBefore))
        {
            landed = along;
            break;
        }
        before = along;
        depthBefore = depth;
    }
    if (landed < 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float near = before;
    float far = landed;
    [unroll]
    for (int refine = 0; refine < 5; ++refine)
    {
        const float middle = 0.5f * (near + far);
        const float2 ndc = startAt + across * middle;
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (1.0f / lerp(startOver, endOver, middle) - sceneDepthAt(uv) > 0.0f)
            far = middle;
        else
            near = middle;
    }
    const float2 ndc = startAt + across * far;
    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    const float depth = 1.0f / lerp(startOver, endOver, far);
    // **A landing is ON something**: halved, the ray and the scene's depth
    // under it meet where the ray crosses a surface -- or, where it only
    // passed BEHIND the bank, at the bank's edge with the depth far in front
    // of the ray. That is the bank's colour laid across the water it is not
    // reflected in, and a lake striped with its own shore is worse than one
    // that shows the sky.
    if (abs(depth - sceneDepthAt(uv)) > 0.3f + depth * 0.03f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float2 inside = min(uv, 1.0f - uv);
    const float edge = saturate(min(inside.x, inside.y) * 12.0f);
    // How far along the ray itself that is, for the fade at its reach's end.
    const float span = to.w - from.w;
    const float fraction = saturate(abs(span) > 1e-4f ? (depth - from.w) / span : far);
    return float4(min(sceneColorAt(uv), 32.0f), edge * (1.0f - fraction * fraction));
}

)";

constexpr std::string_view ForwardFragment = R"(float4 FragmentMain(Interpolants input) : SV_Target0
{
    const float3 geometricNormal = normalize(input.Normal);
    SurfaceInputs inputs;
    inputs.Time = EngineSurfaceClock.x;
    inputs.CameraPosition = EngineSurfaceClock.yzw;
    inputs.ObjectToWorld = float4x4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f);
    inputs.WorldPosition = input.ShadingPosition + EngineSurfaceClock.yzw;
    inputs.WorldNormal = geometricNormal;
    inputs.WorldTangent = input.Tangent;
    inputs.Uv0 = input.Uv;
    inputs.Uv1 = input.Uv;
    inputs.VertexColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
    inputs.ScreenUv = input.Position.xy * ViewportParams.zw;
    inputs.ScreenPosition = float4(input.Position.xy, 0.0f, input.ViewDepth);
#if defined(ENG_SURFACE_BLENDED)
    inputs.SceneDepth = EngineSceneDepth.SampleLevel(EngineSceneDepthSampler, inputs.ScreenUv, 0.0f);
    inputs.SceneColor = EngineSceneColor.SampleLevel(EngineSceneColorSampler, inputs.ScreenUv, 0.0f).rgb;
#else
    inputs.SceneDepth = 3.0e38f;
    inputs.SceneColor = float3(0.0f, 0.0f, 0.0f);
#endif
    inputs.Wind = engineWindAt(EngineSurfaceWind.xyz, EngineSurfaceWind.w, EngineSurfaceWindShape.x,
                               inputs.WorldPosition, inputs.Time);

    SurfaceOutput surface;
    surface.BaseColor = float3(1.0f, 1.0f, 1.0f);
    surface.Alpha = 1.0f;
    surface.Metallic = 0.0f;
    surface.Roughness = 0.7f;
    surface.Normal = geometricNormal;
    surface.Emissive = float3(0.0f, 0.0f, 0.0f);
    surface.Reflection = float4(0.0f, 0.0f, 0.0f, 0.0f);
    surfaceFragment(inputs, surface);

    // The draw's own alpha -- a part's transparency -- on top of the surface's.
    const float alpha = surface.Alpha * input.InstanceAlpha;
    clip(alpha - MetallicRoughnessNormalCutoff.w);
    // **Every interpolant is read**, whatever the surface reads: D3D12 links a
    // pixel shader's inputs to the vertex shader's outputs by position, and a
    // surface that never touched its uv had them stripped and its pipeline
    // refused. A test no value can pass keeps them without costing a thing --
    // the clip above already makes this a shader that may discard.
    if (input.Uv.x == -3.0e38f && input.Tangent.w == -3.0e38f)
        discard;
    const float3 normal = normalize(surface.Normal);
    Surface lit = makeSurface(input.ShadingPosition, normal, surface.BaseColor, saturate(surface.Metallic),
                              saturate(surface.Roughness));
    lit.Alpha = antiAliasedAlpha(lit.Alpha, geometricNormal);
    EngineScreenFlatness = screenFlatness(geometricNormal, input.ShadingPosition);
    EngineGeometricNormal = geometricNormal;
    // What it mirrors of the scene takes the place of that share of what it
    // would reflect of the sky and the probes (contract 1.2).
    EngineSurfaceReflection = float4(surface.Reflection.rgb, saturate(surface.Reflection.a));
    float3 color = lightSurface(lit, input.ShadingPosition, normal, input.ViewDepth, input.Position.xy);
    color += surface.Emissive;
    color = applyFog(color, FogColor.rgb, FogRange, length(input.ShadingPosition));
    return float4(color, alpha);
}
)";

} // namespace

std::string surfaceWrapper(const SurfaceReflection& reflection, SurfaceVariant variant, SurfaceStage stage,
                           std::string_view userInclude)
{
    std::string out;
    out += "// GENERATED from a surface shader by the engine (ADR 0091). Do not edit.\n";
    out += "#define ENG_SURFACE_WRAPPER\n";
    if (instanced(variant))
        out += "#define ENG_SURFACE_INSTANCED\n";
    if (variant == SurfaceVariant::ForwardBlended)
        out += "#define ENG_SURFACE_BLENDED\n";
    if (depthOnly(variant)) {
        out += "#define ENG_SURFACE_DEPTH\n#define ENG_UNIFORMS_SHADOW\n#include \"engine_pbr.hlsli\"\n";
        // The lighting functions a fragment function may call exist only in the
        // forward headers; a depth pass never runs one.
    }
    else {
        out += "#define ENG_UNIFORMS_OBJECT\n#define ENG_UNIFORMS_FRAME\n#define ENG_UNIFORMS_MATERIAL\n";
        // What it reads of the screen (ADR 0209): an opaque surface, in the
        // two slots a blended one reads the scene behind it from; a blended
        // one, nothing -- there is no depth of its own to march from, and it
        // has `sceneDepthAt` and `sceneColorAt` for what it wants of it.
        if (variant == SurfaceVariant::ForwardBlended)
            out += "#define ENG_NO_SCREEN_LIGHT\n";
        else
            out += "#define ENG_SCREEN_REFLECTION_REGISTERS t14, s14\n#define ENG_SCREEN_BOUNCE_REGISTERS t15, s15\n";
        out += "#include \"engine_forward.hlsli\"\n";
    }
    out += "#include \"engine/surface.hlsli\"\n\n";

    if (stage == SurfaceStage::Vertex) {
        appendBlock(out, reflection, "b1, space1");
        appendTextures(out, reflection, false, "space0");
    }
    else {
        appendBlock(out, reflection, "b2, space3");
        appendTextures(out, reflection, true, "space2");
    }
    // In both stages' text, since both carry both entry points; the vertex
    // stage never reads them and the compiler drops them there.
    if (variant == SurfaceVariant::ForwardBlended) {
        out += "Texture2D<float> EngineSceneDepth : register(t14, space2);\n";
        out += "SamplerState EngineSceneDepthSampler : register(s14, space2);\n";
        out += "Texture2D EngineSceneColor : register(t15, space2);\n";
        out += "SamplerState EngineSceneColorSampler : register(s15, space2);\n\n";
        out += "float sceneDepthAt(float2 uv)\n{\n"
               "    return EngineSceneDepth.SampleLevel(EngineSceneDepthSampler, uv, 0.0f);\n}\n";
        out += "float3 sceneColorAt(float2 uv)\n{\n"
               "    return EngineSceneColor.SampleLevel(EngineSceneColorSampler, uv, 0.0f).rgb;\n}\n\n";
        // The scene in a mirror, in the fragment stage alone: it marches the
        // scene with a loop a vertex stage has no use for.
        if (stage == SurfaceStage::Fragment)
            out += SceneReflection;
        else
            out += "float4 sceneReflectionAt(SurfaceInputs inputs, float3 normal)\n{\n"
                   "    return float4(0.0f, 0.0f, 0.0f, 0.0f);\n}\n\n";
    }
    else {
        // Every variant compiles the same user file, so the functions exist in
        // every variant; outside a blended surface there is no scene to read.
        out += "float sceneDepthAt(float2 uv)\n{\n    return 3.0e38f;\n}\n";
        out += "float3 sceneColorAt(float2 uv)\n{\n    return float3(0.0f, 0.0f, 0.0f);\n}\n\n";
        out += "float4 sceneReflectionAt(SurfaceInputs inputs, float3 normal)\n{\n"
               "    return float4(0.0f, 0.0f, 0.0f, 0.0f);\n}\n\n";
    }

    out += "#include \"";
    out += userInclude;
    out += "\"\n\n";
    if (!reflection.hasVertex)
        out += "void surfaceVertex(inout SurfaceVertex vertex, SurfaceInputs inputs)\n{\n}\n\n";
    if (!reflection.hasFragment)
        out += "void surfaceFragment(SurfaceInputs inputs, inout SurfaceOutput surface)\n{\n}\n\n";

    out += VertexInput;
    if (depthOnly(variant)) {
        out += DepthVertex;
        out += "\nvoid FragmentMain()\n{\n}\n";
    }
    else {
        out += ForwardVertex;
        out += '\n';
        out += ForwardFragment;
    }
    return out;
}

SurfaceResourceCounts surfaceResourceCounts(const SurfaceReflection& reflection, SurfaceVariant variant,
                                            SurfaceStage stage) noexcept
{
    const u32 textures = static_cast<u32>(reflection.textures.size());
    if (stage == SurfaceStage::Vertex)
        return SurfaceResourceCounts{textures, 2};
    if (depthOnly(variant))
        return SurfaceResourceCounts{0, 0};
    // Every slot up to the last: the scene behind a blended surface, and
    // what an opaque one reads of the screen (ADR 0209).
    (void)textures;
    return SurfaceResourceCounts{SceneColorSlot + 1, 3};
}

void writeSurfaceParam(const SurfaceParam& param, std::span<const f32> value, std::span<core::u8> block) noexcept
{
    const u32 components = componentsOf(param.type);
    if (param.offset + 4u * components > block.size())
        return;
    for (u32 index = 0; index < components; ++index) {
        const f32 component = index < value.size() ? value[index] : param.value[index];
        core::u8* at = block.data() + param.offset + 4u * index;
        if (param.type == SurfaceParamType::Int) {
            const auto whole = static_cast<core::i32>(component);
            std::memcpy(at, &whole, 4);
        }
        else if (param.type == SurfaceParamType::Bool) {
            const core::u32 flag = component != 0.0f ? 1u : 0u;
            std::memcpy(at, &flag, 4);
        }
        else {
            std::memcpy(at, &component, 4);
        }
    }
}

} // namespace engine::asset
