// The Graph panel (ADR 0211): a canvas of nodes drawn on ImGui's own draw
// list, the fields of what is selected beside it, and the list a node is
// added from.
//
// **No node library.** A canvas is rectangles, circles, curves and text, and
// what is hard about one -- which link may be made, what a press lands on,
// undo -- is `NodeCanvas`, which draws nothing and is tested with no window.
// What is here is the picture of it and the reading of a mouse.
//
// A separate translation unit from `debug_overlay.cpp` for the reason the code
// pane is: that file is long, and this shares nothing of its anonymous
// namespace.
#include "engine/app/graph_panel.h"

#include "engine/app/graph_editor.h"
#include "engine/app/surface_compiler.h"
#include "engine/app/ui_theme.h"
#include "engine/core/i18n.h"

#if ENG_DEBUG_UI

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "engine/app/tip.h"

namespace engine::app {

namespace {

using core::f32;
using core::i32;
using core::u32;
using core::usize;

// The frame a graph's window last had the keyboard in.
int g_keyboardFrame = -10;
// The graph whose tab was closed with changes in it, while it is asked about.
std::string g_closing;

// How long a refusal, or an arrow just taken, stays on the canvas.
constexpr int kRefusedFrames = 240;
constexpr int kTakenFrames = 90;
// The side column, in font sizes.
constexpr float kSideWidth = 19.0f;

// --- Words -----------------------------------------------------------------------

struct KindWords
{
    std::string_view kind;
    core::TextKey label;
};

// What each kind of node is called in the list and on a node nobody named.
const std::array<KindWords, 43> kKindWords{{
    {"Uv0", ENG_TR("engine.editor.nodes.kind.uv0")},
    {"Uv1", ENG_TR("engine.editor.nodes.kind.uv1")},
    {"WorldPosition", ENG_TR("engine.editor.nodes.kind.world_position")},
    {"WorldNormal", ENG_TR("engine.editor.nodes.kind.world_normal")},
    {"VertexColor", ENG_TR("engine.editor.nodes.kind.vertex_color")},
    {"Time", ENG_TR("engine.editor.nodes.kind.time")},
    {"CameraPosition", ENG_TR("engine.editor.nodes.kind.camera_position")},
    {"ScreenUv", ENG_TR("engine.editor.nodes.kind.screen_uv")},
    {"Wind", ENG_TR("engine.editor.nodes.kind.wind")},
    {"ObjectPosition", ENG_TR("engine.editor.nodes.kind.object_position")},
    {"ObjectNormal", ENG_TR("engine.editor.nodes.kind.object_normal")},
    {"Parameter", ENG_TR("engine.editor.nodes.kind.parameter")},
    {"Constant", ENG_TR("engine.editor.nodes.kind.constant")},
    {"Texture", ENG_TR("engine.editor.nodes.kind.texture")},
    {"NormalMap", ENG_TR("engine.editor.nodes.kind.normal_map")},
    {"Add", ENG_TR("engine.editor.nodes.kind.add")},
    {"Subtract", ENG_TR("engine.editor.nodes.kind.subtract")},
    {"Multiply", ENG_TR("engine.editor.nodes.kind.multiply")},
    {"Divide", ENG_TR("engine.editor.nodes.kind.divide")},
    {"Lerp", ENG_TR("engine.editor.nodes.kind.lerp")},
    {"Saturate", ENG_TR("engine.editor.nodes.kind.saturate")},
    {"OneMinus", ENG_TR("engine.editor.nodes.kind.one_minus")},
    {"Power", ENG_TR("engine.editor.nodes.kind.power")},
    {"Min", ENG_TR("engine.editor.nodes.kind.min")},
    {"Max", ENG_TR("engine.editor.nodes.kind.max")},
    {"Abs", ENG_TR("engine.editor.nodes.kind.abs")},
    {"Step", ENG_TR("engine.editor.nodes.kind.step")},
    {"Smoothstep", ENG_TR("engine.editor.nodes.kind.smoothstep")},
    {"Sine", ENG_TR("engine.editor.nodes.kind.sine")},
    {"Fraction", ENG_TR("engine.editor.nodes.kind.fraction")},
    {"Normalize", ENG_TR("engine.editor.nodes.kind.normalize")},
    {"Dot", ENG_TR("engine.editor.nodes.kind.dot")},
    {"Length", ENG_TR("engine.editor.nodes.kind.length")},
    {"Split", ENG_TR("engine.editor.nodes.kind.split")},
    {"Combine", ENG_TR("engine.editor.nodes.kind.combine")},
    {"Fresnel", ENG_TR("engine.editor.nodes.kind.fresnel")},
    {"Panner", ENG_TR("engine.editor.nodes.kind.panner")},
    {"Noise", ENG_TR("engine.editor.nodes.kind.noise")},
    {"Expression", ENG_TR("engine.editor.nodes.kind.expression")},
    {"Surface", ENG_TR("engine.editor.nodes.kind.surface")},
    {"Vertex", ENG_TR("engine.editor.nodes.kind.vertex")},
    {"State", ENG_TR("engine.editor.nodes.kind.state")},
    {"Any", ENG_TR("engine.editor.nodes.kind.any")},
}};

[[nodiscard]] std::string labelOf(std::string_view kind)
{
    for (const KindWords& words : kKindWords) {
        if (words.kind == kind)
            return core::tr(words.label);
    }
    return std::string(kind);
}

[[nodiscard]] const char* groupLabel(std::string_view group)
{
    if (group == "Inputs")
        return core::tr(ENG_TR("engine.editor.nodes.group.inputs"));
    if (group == "Values")
        return core::tr(ENG_TR("engine.editor.nodes.group.values"));
    if (group == "Math")
        return core::tr(ENG_TR("engine.editor.nodes.group.math"));
    if (group == "Shape")
        return core::tr(ENG_TR("engine.editor.nodes.group.shape"));
    if (group == "Patterns")
        return core::tr(ENG_TR("engine.editor.nodes.group.patterns"));
    if (group == "Code")
        return core::tr(ENG_TR("engine.editor.nodes.group.code"));
    if (group == "Ends")
        return core::tr(ENG_TR("engine.editor.nodes.group.ends"));
    return core::tr(ENG_TR("engine.editor.nodes.group.states"));
}

// Why a link was not made.
[[nodiscard]] const char* refusalWords(LinkRefusal refusal)
{
    switch (refusal) {
    case LinkRefusal::NoSuchPin:
        return core::tr(ENG_TR("engine.editor.nodes.link.no_such_pin"));
    case LinkRefusal::ToItself:
        return core::tr(ENG_TR("engine.editor.nodes.link.to_itself"));
    case LinkRefusal::TypesDoNotFit:
        return core::tr(ENG_TR("engine.editor.nodes.link.types_do_not_fit"));
    case LinkRefusal::AlreadyMade:
        return core::tr(ENG_TR("engine.editor.nodes.link.already_made"));
    case LinkRefusal::Cycle:
        return core::tr(ENG_TR("engine.editor.nodes.link.cycle"));
    case LinkRefusal::None:
        break;
    }
    return "";
}

// What is wrong with a material graph at a node.
[[nodiscard]] std::string issueWords(const asset::SurfaceGraphIssue& issue)
{
    const std::initializer_list<core::I18nArg> args{{"where", std::string_view(issue.where)},
                                                    {"what", std::string_view(issue.what)}};
    switch (issue.code) {
    case asset::SurfaceGraphIssueCode::InputMissing:
        return core::tr(ENG_TR("engine.editor.nodes.issue.input_missing"), args);
    case asset::SurfaceGraphIssueCode::WidthsDoNotFit:
        return issue.where.empty() ? core::tr(ENG_TR("engine.editor.nodes.issue.widths_do_not_meet"), args)
                                   : core::tr(ENG_TR("engine.editor.nodes.issue.width_does_not_fit"), args);
    case asset::SurfaceGraphIssueCode::WrongStage:
        return core::tr(ENG_TR("engine.editor.nodes.issue.wrong_stage"), args);
    case asset::SurfaceGraphIssueCode::BadName:
        return core::tr(ENG_TR("engine.editor.nodes.issue.bad_name"), args);
    case asset::SurfaceGraphIssueCode::NameTaken:
        return core::tr(ENG_TR("engine.editor.nodes.issue.name_taken"), args);
    case asset::SurfaceGraphIssueCode::BadValue:
        return core::tr(ENG_TR("engine.editor.nodes.issue.bad_value"), args);
    case asset::SurfaceGraphIssueCode::EmptyExpression:
        return core::tr(ENG_TR("engine.editor.nodes.issue.empty_expression"), args);
    case asset::SurfaceGraphIssueCode::NoSuchPin:
        return core::tr(ENG_TR("engine.editor.nodes.issue.no_such_pin"), args);
    case asset::SurfaceGraphIssueCode::Cycle:
        return core::tr(ENG_TR("engine.editor.nodes.issue.cycle"), args);
    case asset::SurfaceGraphIssueCode::TwoEnds:
        return core::tr(ENG_TR("engine.editor.nodes.issue.two_ends"), args);
    }
    return {};
}

// Why an edit to an animation graph was refused: the reader's code, in words.
[[nodiscard]] std::string refusedWords(const asset::GraphReadError& error)
{
    const std::initializer_list<core::I18nArg> args{{"where", std::string_view(error.where)},
                                                    {"what", std::string_view(error.what)}};
    switch (error.code) {
    case asset::GraphReadCode::None:
        return {};
    case asset::GraphReadCode::DuplicateName:
        return core::tr(ENG_TR("engine.editor.nodes.refused.duplicate_name"), args);
    case asset::GraphReadCode::Empty:
        return core::tr(ENG_TR("engine.editor.nodes.refused.empty"), args);
    case asset::GraphReadCode::UnknownParameter:
        return core::tr(ENG_TR("engine.editor.nodes.refused.unknown_parameter"), args);
    case asset::GraphReadCode::UnknownState:
        return core::tr(ENG_TR("engine.editor.nodes.refused.unknown_state"), args);
    case asset::GraphReadCode::UnknownClip:
        return core::tr(ENG_TR("engine.editor.nodes.refused.unknown_clip"), args);
    case asset::GraphReadCode::ParameterKind:
        return core::tr(ENG_TR("engine.editor.nodes.refused.parameter_kind"), args);
    case asset::GraphReadCode::BadSource:
        return core::tr(ENG_TR("engine.editor.nodes.refused.bad_source"), args);
    case asset::GraphReadCode::OutOfRange:
        return core::tr(ENG_TR("engine.editor.nodes.refused.out_of_range"), args);
    case asset::GraphReadCode::Malformed:
    case asset::GraphReadCode::NotAGraph:
    case asset::GraphReadCode::UnsupportedVersion:
    case asset::GraphReadCode::UnknownKey:
    case asset::GraphReadCode::MissingKey:
    case asset::GraphReadCode::WrongType:
    case asset::GraphReadCode::BadShape:
    case asset::GraphReadCode::BadOperator:
        break;
    }
    // What an edit made here cannot come to; said with where, all the same.
    return core::tr(ENG_TR("engine.editor.nodes.refused.other"), args);
}

// --- Colour ------------------------------------------------------------------------

[[nodiscard]] ImU32 col(core::Color3 c, float alpha = 1.0f) noexcept
{
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, alpha));
}

// A pin is coloured by what it carries, so a link that will be refused is
// seen to be before it is tried.
[[nodiscard]] ImU32 pinColour(PinType type) noexcept
{
    switch (type) {
    case PinType::Number:
        return IM_COL32(190, 196, 204, 255);
    case PinType::Vector2:
        return IM_COL32(110, 200, 160, 255);
    case PinType::Vector3:
        return IM_COL32(232, 190, 92, 255);
    case PinType::Vector4:
        return IM_COL32(200, 132, 222, 255);
    case PinType::Any:
        return IM_COL32(128, 132, 142, 255);
    case PinType::Flow:
        break;
    }
    return IM_COL32(230, 230, 230, 255);
}

[[nodiscard]] ImU32 headerColour(std::string_view group) noexcept
{
    if (group == "Inputs")
        return IM_COL32(44, 96, 84, 255);
    if (group == "Values")
        return IM_COL32(128, 92, 40, 255);
    if (group == "Math")
        return IM_COL32(52, 82, 124, 255);
    if (group == "Shape")
        return IM_COL32(96, 70, 126, 255);
    if (group == "Patterns")
        return IM_COL32(120, 62, 84, 255);
    // Code reads as code: the ground a script is written on, not a card's.
    if (group == "Code")
        return IM_COL32(18, 18, 20, 255);
    return col(currentTheme().palette.accentFill);
}

// --- Fields ------------------------------------------------------------------------

// A line of text that is the graph's only once it is done with: while it is
// being typed the buffer is the field's own, so a name half typed is never
// the name of anything. Says the text when the edit ends.
[[nodiscard]] std::optional<std::string> textField(const char* id, std::string_view current, const char* hint = "",
                                                   bool code = false)
{
    static ImGuiID editing = 0;
    static char held[512]{};
    char buffer[512]{};
    const ImGuiID item = ImGui::GetID(id);
    const bool active = ImGui::GetActiveID() == item && editing == item;
    if (active)
        std::memcpy(buffer, held, sizeof(buffer));
    else
        std::memcpy(buffer, current.data(), std::min(current.size(), sizeof(buffer) - 1));
    if (code && codeFont() != nullptr)
        ImGui::PushFont(codeFont(), 0.0f);
    (void)ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer));
    if (code && codeFont() != nullptr)
        ImGui::PopFont();
    if (ImGui::IsItemActive()) {
        editing = item;
        std::memcpy(held, buffer, sizeof(held));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        editing = 0;
        return std::string(buffer);
    }
    return std::nullopt;
}

// A row of the side column: a name at the left, and what follows it filling
// the rest.
void fieldName(const char* name)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine(ImGui::GetFontSize() * 6.5f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void sectionTitle(const char* title)
{
    ImGui::Spacing();
    ImGui::TextColored(
        ImVec4(currentTheme().palette.accent.r, currentTheme().palette.accent.g, currentTheme().palette.accent.b, 1.0f),
        "%s", title);
    ImGui::Separator();
}

// One of a few words, as a combo. Says the index chosen.
[[nodiscard]] std::optional<usize> choiceField(const char* id, std::span<const char* const> choices, usize current)
{
    std::optional<usize> chosen;
    if (ImGui::BeginCombo(id, current < choices.size() ? choices[current] : "")) {
        for (usize index = 0; index < choices.size(); ++index) {
            if (ImGui::Selectable(choices[index], index == current) && index != current)
                chosen = index;
        }
        ImGui::EndCombo();
    }
    return chosen;
}

// --- The canvas ----------------------------------------------------------------------

struct Pane
{
    Pane(GraphDocument& of, usize at, const GraphLive& running, const GraphShaders& with, GraphPanelCommands& commands)
        : document(of), index(at), live(running), shaders(with), out(commands)
    {}

    GraphDocument& document;
    usize index;
    const GraphLive& live;
    const GraphShaders& shaders;
    GraphPanelCommands& out;
    // An animation graph while the game runs: looked at.
    bool readOnly = false;
    // What is wrong at each node of a material graph, in words.
    std::vector<std::pair<u32, std::string>> problems;
    // The shader's state, in a word, for the bar.
    std::string status;
    bool statusBad = false;
};

[[nodiscard]] CanvasMetrics metricsOf(bool arrows) noexcept
{
    const float font = ImGui::GetFontSize();
    CanvasMetrics metrics;
    metrics.nodeWidth = std::floor(font * (arrows ? 11.5f : 12.5f));
    metrics.titleHeight = std::floor(font * 1.8f);
    metrics.rowHeight = std::floor(font * 1.55f);
    metrics.pinReach = std::floor(font * 0.7f);
    return metrics;
}

// The layer of what is running that a page is a picture of.
[[nodiscard]] const GraphLive::Layer* liveLayer(const Pane& pane)
{
    if (pane.document.kind != GraphKind::Animation || !pane.live.playing || pane.live.graph != pane.document.relative)
        return nullptr;
    const asset::AnimationGraph& graph = pane.document.animation.graph();
    const usize page = pane.document.animation.page();
    if (page >= graph.layers.size())
        return nullptr;
    for (const GraphLive::Layer& layer : pane.live.layers) {
        if (layer.name == graph.layers[page].name)
            return &layer;
    }
    return nullptr;
}

void openList(GraphDocument& document, CanvasPoint at, std::optional<GraphLoose> loose)
{
    document.ask = GraphAsk{};
    document.ask.open = true;
    document.ask.at = at;
    document.ask.loose = loose;
}

[[nodiscard]] std::vector<GraphChoice> choicesOf(const GraphDocument& document)
{
    const GraphLoose* const loose = document.ask.loose.has_value() ? &*document.ask.loose : nullptr;
    const GraphLabel label = [](std::string_view kind) { return labelOf(kind); };
    return document.kind == GraphKind::Surface ? document.surface.choices(document.ask.filter, loose, label)
                                               : document.animation.choices(document.ask.filter, loose, label);
}

void choose(GraphDocument& document, const GraphChoice& choice)
{
    const GraphLoose* const loose = document.ask.loose.has_value() ? &*document.ask.loose : nullptr;
    if (document.kind == GraphKind::Surface)
        (void)document.surface.choose(choice, document.ask.at, loose);
    else
        (void)document.animation.choose(choice, document.ask.at, loose);
    document.ask = GraphAsk{};
    document.refocus = true;
}

// **The list a node is added from**: a field that is typed into and the
// kinds that have it in their name, under their headings; the arrows move
// along it and Enter takes the one they are on.
void drawList(Pane& pane, ImVec2 origin)
{
    GraphDocument& document = pane.document;
    if (!document.ask.open)
        return;
    const CanvasPoint at = document.canvas().toPanel(document.ask.at.x, document.ask.at.y);
    const float font = ImGui::GetFontSize();
    // Where the pointer was, brought in from the window's edges: a list
    // asked for near the foot of the screen is still a list that can be read.
    const ImVec2 listSize(font * 17.0f, font * 20.0f);
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const ImVec2 low = viewport->WorkPos;
    const ImVec2 high(viewport->WorkPos.x + viewport->WorkSize.x - listSize.x,
                      viewport->WorkPos.y + viewport->WorkSize.y - listSize.y);
    ImGui::SetNextWindowPos(ImVec2(std::clamp(origin.x + at.x, low.x, std::max(low.x, high.x)),
                                   std::clamp(origin.y + at.y, low.y, std::max(low.y, high.y))),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(listSize, ImGuiCond_Appearing);
    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
    char name[64];
    (void)std::snprintf(name, sizeof(name), "###graph-add-%zu", pane.index);
    if (ImGui::Begin(name, &open, flags)) {
        g_keyboardFrame = ImGui::GetFrameCount();
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        char buffer[96]{};
        std::memcpy(buffer, document.ask.filter.data(), std::min(document.ask.filter.size(), sizeof(buffer) - 1));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* const hint = document.ask.loose.has_value() ? core::tr(ENG_TR("engine.editor.nodes.list.hint_fits"))
                                                                : core::tr(ENG_TR("engine.editor.nodes.list.hint"));
        if (ImGui::InputTextWithHint("##filter", hint, buffer, sizeof(buffer))) {
            document.ask.filter = buffer;
            document.ask.row = 0;
        }
        const std::vector<GraphChoice> choices = choicesOf(document);
        const u32 rows = static_cast<u32>(choices.size());
        if (rows != 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                document.ask.row = (document.ask.row + 1) % rows;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                document.ask.row = (document.ask.row + rows - 1) % rows;
            document.ask.row = std::min(document.ask.row, rows - 1);
        }
        std::optional<u32> taken;
        if (rows != 0 &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            taken = document.ask.row;
        if (ImGui::BeginChild("##choices", ImVec2(0.0f, 0.0f))) {
            if (rows == 0)
                ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.list.nothing")));
            std::string_view heading;
            for (u32 row = 0; row < rows; ++row) {
                const GraphChoice& choice = choices[row];
                if (choice.group != heading) {
                    heading = choice.group;
                    ImGui::TextDisabled("%s", groupLabel(heading));
                }
                ImGui::PushID(static_cast<int>(row));
                // Under its heading, a step in.
                const std::string shown = "   " + labelOf(choice.kind);
                if (ImGui::Selectable(shown.c_str(), row == document.ask.row))
                    taken = row;
                if (row == document.ask.row &&
                    (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                    ImGui::SetScrollHereY();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (taken.has_value() && *taken < rows) {
            choose(document, choices[*taken]);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                 (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsWindowAppearing())) {
            // Escape, or a press anywhere else, and nothing is added.
            document.refocus = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            document.ask = GraphAsk{};
        }
    }
    ImGui::End();
    if (!open)
        document.ask = GraphAsk{};
}

// What is wrong at each node: the writer's own issues, and -- for a graph
// that is as it was saved -- what the compiler said of the shader, each
// error on the node whose line it is.
void gatherProblems(Pane& pane)
{
    if (pane.document.kind != GraphKind::Surface)
        return;
    const SurfaceDocument& surface = pane.document.surface;
    for (const asset::SurfaceGraphIssue& issue : surface.written().issues)
        pane.problems.emplace_back(issue.node, issueWords(issue));
    if (!pane.problems.empty()) {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.issues"),
                               {{"count", static_cast<core::i64>(pane.problems.size())}});
        pane.statusBad = true;
        return;
    }
    if (pane.document.dirty()) {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.unsaved"));
        return;
    }
    SurfaceCompiler* const compiler = pane.shaders.compiler;
    if (compiler == nullptr || !compiler->available() || pane.shaders.format == rhi::ShaderFormat::Unknown) {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.no_compiler"));
        return;
    }
    // Asked for here, so a graph nothing in the world wears yet is compiled
    // all the same: the person looking at it is who wants to know.
    const std::string urn = "asset://" + asset::surfaceShaderPathOf(pane.document.relative);
    const render::SurfaceProgram* program = nullptr;
    const render::SurfaceStatus status = compiler->find(urn, pane.shaders.format, program);
    if (status == render::SurfaceStatus::Pending) {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.compiling"));
    }
    else if (status == render::SurfaceStatus::Ready) {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.compiled"));
    }
    else {
        pane.status = core::tr(ENG_TR("engine.editor.nodes.status.does_not_compile"));
        pane.statusBad = true;
        for (const SurfaceError& error : compiler->errors(urn)) {
            // The compiler's own words: they name what it read, and are not ours to say again.
            pane.problems.emplace_back(surface.nodeOfLine(error.line), error.message);
        }
    }
}

struct CanvasView
{
    ImDrawList* draw = nullptr;
    ImVec2 origin;
    ImVec2 size;
    const NodeCanvas* canvas = nullptr;
    CanvasMetrics metrics;
    ImFont* font = nullptr;
    float fontSize = 0.0f;

    [[nodiscard]] ImVec2 at(f32 x, f32 y) const
    {
        const CanvasPoint panel = canvas->toPanel(x, y);
        return ImVec2(std::floor(origin.x + panel.x), std::floor(origin.y + panel.y));
    }
    [[nodiscard]] ImVec2 at(CanvasPoint point) const { return at(point.x, point.y); }
    [[nodiscard]] float scaled(float length) const { return length * canvas->zoom(); }
    void text(ImVec2 where, ImU32 colour, std::string_view words, ImFont* with = nullptr) const
    {
        draw->AddText(with != nullptr ? with : font, fontSize * canvas->zoom(), where, colour, words.data(),
                      words.data() + words.size());
    }
    [[nodiscard]] float widthOf(std::string_view words) const
    {
        return font->CalcTextSizeA(fontSize * canvas->zoom(), FLT_MAX, 0.0f, words.data(), words.data() + words.size())
            .x;
    }
};

void drawGrid(const CanvasView& view)
{
    const ThemePalette& palette = currentTheme().palette;
    view.draw->AddRectFilled(view.origin, ImVec2(view.origin.x + view.size.x, view.origin.y + view.size.y),
                             col(palette.surface));
    const float step = 32.0f * view.canvas->zoom();
    if (step < 8.0f)
        return;
    const CanvasPoint pan = view.canvas->pan();
    const ImU32 line = col(palette.border, 0.45f);
    for (float x = std::fmod(pan.x, step); x < view.size.x; x += step) {
        if (x >= 0.0f)
            view.draw->AddLine(ImVec2(view.origin.x + x, view.origin.y),
                               ImVec2(view.origin.x + x, view.origin.y + view.size.y), line);
    }
    for (float y = std::fmod(pan.y, step); y < view.size.y; y += step) {
        if (y >= 0.0f)
            view.draw->AddLine(ImVec2(view.origin.x, view.origin.y + y),
                               ImVec2(view.origin.x + view.size.x, view.origin.y + y), line);
    }
}

// A wire: out of an output to the right, into an input from the left. **It
// leaves a pin sideways before it turns**, and for longer the further it has
// to climb or the nearer its two ends are: a wire between two pins one above
// the other is an S and never a diagonal under the node it left.
void drawWire(const CanvasView& view, ImVec2 from, ImVec2 to, ImU32 colour, float thickness)
{
    const float climb = std::min(std::abs(to.y - from.y) * 0.5f, view.scaled(140.0f));
    const float reach = std::max({view.scaled(50.0f), std::abs(to.x - from.x) * 0.5f, climb});
    view.draw->AddBezierCubic(from, ImVec2(from.x + reach, from.y), ImVec2(to.x - reach, to.y), to, colour, thickness);
}

// An arrow: a line, and a head where it lands.
void drawArrow(const CanvasView& view, ImVec2 from, ImVec2 to, ImU32 colour, float thickness)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f)
        return;
    const float ux = dx / length;
    const float uy = dy / length;
    const float head = view.scaled(9.0f);
    const ImVec2 base(to.x - ux * head, to.y - uy * head);
    view.draw->AddLine(from, base, colour, thickness);
    view.draw->AddTriangleFilled(to, ImVec2(base.x - uy * head * 0.5f, base.y + ux * head * 0.5f),
                                 ImVec2(base.x + uy * head * 0.5f, base.y - ux * head * 0.5f), colour);
}

// What is wrong at a node, under it: at the size the panel's words are, not
// the canvas's -- it has to be read however far out the view is -- and on a
// ground of its own, since it lies over whatever is under the node.
void drawProblem(const CanvasView& view, const Pane& pane, const CanvasNode& node, ImVec2 low)
{
    const ThemePalette& palette = currentTheme().palette;
    float y = low.y + 4.0f;
    for (const auto& [id, words] : pane.problems) {
        if (id != node.id)
            continue;
        const ImVec2 size = ImGui::CalcTextSize(words.c_str());
        const ImVec2 high(low.x + size.x + 18.0f, y + size.y + 6.0f);
        view.draw->AddRectFilled(ImVec2(low.x, y), high, col(palette.background, 0.95f), 4.0f);
        // A bar of the colour down its left edge: what says, from across
        // the room, that these words are something wrong.
        view.draw->AddRectFilled(ImVec2(low.x, y), ImVec2(low.x + 6.0f, high.y), col(palette.danger), 4.0f,
                                 ImDrawFlags_RoundCornersLeft);
        view.draw->AddRect(ImVec2(low.x, y), high, col(palette.danger), 4.0f);
        view.draw->AddText(ImVec2(low.x + 12.0f, y + 3.0f), col(palette.danger), words.c_str());
        y += size.y + 9.0f;
    }
}

[[nodiscard]] bool hasProblem(const Pane& pane, u32 node)
{
    return std::any_of(pane.problems.begin(), pane.problems.end(),
                       [node](const std::pair<u32, std::string>& held) { return held.first == node; });
}

void drawSurfaceNodes(const CanvasView& view, const Pane& pane)
{
    const ThemePalette& palette = currentTheme().palette;
    const NodeCanvas& canvas = *view.canvas;
    const CanvasMetrics& metrics = view.metrics;
    const float pad = view.scaled(8.0f);
    const float textDrop = (view.scaled(metrics.rowHeight) - view.fontSize * canvas.zoom()) * 0.5f;
    const float radius = std::max(2.0f, view.scaled(4.5f));
    const std::optional<CanvasLink> chosen = canvas.selectedLink();

    for (const CanvasLink& link : canvas.links()) {
        const CanvasNode* const from = canvas.node(link.from);
        const CanvasNode* const to = canvas.node(link.to);
        if (from == nullptr || to == nullptr)
            continue;
        const bool selected = chosen.has_value() && *chosen == link;
        drawWire(view, view.at(outputPlace(*from, link.output, metrics)), view.at(inputPlace(*to, link.input, metrics)),
                 selected ? col(palette.accent) : pinColour(from->outputs[link.output].type), selected ? 3.0f : 2.0f);
    }

    for (const CanvasNode& node : canvas.nodes()) {
        const CanvasRect rect = nodeRect(node, metrics);
        const ImVec2 a = view.at(rect.x, rect.y);
        const ImVec2 b = view.at(rect.x + rect.width, rect.y + rect.height);
        if (b.x < view.origin.x || b.y < view.origin.y || a.x > view.origin.x + view.size.x ||
            a.y > view.origin.y + view.size.y)
            continue;
        const asset::SurfaceNodeKind* const kind = asset::surfaceNodeKind(node.kind);
        const std::string_view group = kind != nullptr ? kind->group : std::string_view();
        const bool code = group == "Code";
        const float rounding = view.scaled(5.0f);
        const float titleFoot = a.y + view.scaled(metrics.titleHeight);
        view.draw->AddRectFilled(a, b, col(palette.surfaceRaised), rounding);
        view.draw->AddRectFilled(a, ImVec2(b.x, titleFoot), headerColour(group), rounding, ImDrawFlags_RoundCornersTop);

        view.draw->PushClipRect(
            ImVec2(std::max(a.x, view.origin.x), std::max(a.y, view.origin.y)),
            ImVec2(std::min(b.x, view.origin.x + view.size.x), std::min(b.y, view.origin.y + view.size.y)), false);
        // What a person called it, and what it is: a node nobody named is
        // called by its kind.
        const std::string label = labelOf(node.kind);
        const float titleDrop = (view.scaled(metrics.titleHeight) - view.fontSize * canvas.zoom()) * 0.5f;
        const ImU32 titleColour = code ? col(palette.accent) : IM_COL32(255, 255, 255, 255);
        view.text(ImVec2(a.x + pad, a.y + titleDrop), titleColour, node.title.empty() ? label : node.title,
                  code ? codeFont() : nullptr);
        if (!node.title.empty()) {
            const float wide = view.widthOf(label);
            view.text(ImVec2(b.x - pad - wide, a.y + titleDrop), IM_COL32(255, 255, 255, 150), label);
        }
        for (u32 pin = 0; pin < node.inputs.size(); ++pin) {
            const ImVec2 place = view.at(inputPlace(node, pin, metrics));
            view.text(ImVec2(a.x + pad + radius, place.y - view.scaled(metrics.rowHeight) * 0.5f + textDrop),
                      col(palette.text), node.inputs[pin].name);
        }
        for (u32 pin = 0; pin < node.outputs.size(); ++pin) {
            const ImVec2 place = view.at(outputPlace(node, pin, metrics));
            const float wide = view.widthOf(node.outputs[pin].name);
            view.text(ImVec2(b.x - pad - radius - wide, place.y - view.scaled(metrics.rowHeight) * 0.5f + textDrop),
                      col(palette.text), node.outputs[pin].name);
        }
        // The fields, a row each under the pins: what a node is without
        // having to be selected to be read.
        usize row = std::max(node.inputs.size(), node.outputs.size());
        for (const CanvasField& held : node.fields) {
            // One with nothing in it has no row (`shownFields`).
            if (held.value.empty())
                continue;
            const float top = a.y + view.scaled(metrics.titleHeight + static_cast<f32>(row) * metrics.rowHeight);
            ++row;
            if (code && held.name == "Code") {
                // An expression is a line of code, and is drawn as one.
                view.draw->AddRectFilled(ImVec2(a.x + 1.0f, top),
                                         ImVec2(b.x - 1.0f, top + view.scaled(metrics.rowHeight)),
                                         IM_COL32(18, 18, 20, 255));
                view.text(ImVec2(a.x + pad, top + textDrop), col(currentTheme().syntax.identifier), held.value,
                          codeFont());
                continue;
            }
            const float nameWide = view.widthOf(held.name);
            view.text(ImVec2(a.x + pad, top + textDrop), col(palette.textMuted), held.name);
            view.text(ImVec2(a.x + pad + nameWide + view.scaled(8.0f), top + textDrop), col(palette.text), held.value);
        }
        view.draw->PopClipRect();

        const bool wrong = hasProblem(pane, node.id);
        const bool selected = canvas.selected(node.id);
        view.draw->AddRect(a, b, wrong ? col(palette.danger) : (selected ? col(palette.accent) : col(palette.border)),
                           rounding, 0, wrong || selected ? 2.0f : 1.0f);
        if (wrong && selected)
            view.draw->AddRect(ImVec2(a.x - 3.0f, a.y - 3.0f), ImVec2(b.x + 3.0f, b.y + 3.0f), col(palette.accent),
                               rounding + 3.0f, 0, 1.0f);
        // The pins over the edge: filled where something is linked.
        for (u32 pin = 0; pin < node.inputs.size(); ++pin) {
            const ImVec2 place = view.at(inputPlace(node, pin, metrics));
            const ImU32 colour = pinColour(node.inputs[pin].type);
            view.draw->AddCircleFilled(place, radius, col(palette.surface));
            if (canvas.linkInto(node.id, pin).has_value())
                view.draw->AddCircleFilled(place, radius, colour);
            view.draw->AddCircle(place, radius, colour, 0, 1.5f);
        }
        for (u32 pin = 0; pin < node.outputs.size(); ++pin) {
            const ImVec2 place = view.at(outputPlace(node, pin, metrics));
            view.draw->AddCircleFilled(place, radius, pinColour(node.outputs[pin].type));
        }
    }
    // What is wrong, after every node: over whatever is under the node it is
    // wrong at.
    for (const CanvasNode& node : canvas.nodes()) {
        if (!hasProblem(pane, node.id))
            continue;
        const CanvasRect rect = nodeRect(node, metrics);
        drawProblem(view, pane, node, view.at(rect.x, rect.y + rect.height));
    }
}

// What a state plays, in a few words, for the row under its name.
[[nodiscard]] std::string stateWords(const asset::AnimationGraph& graph, usize layer, u32 state)
{
    const asset::GraphState& held = graph.layers[layer].states[state];
    const auto parameter = [&graph](i32 index) -> std::string_view {
        return index >= 0 && static_cast<usize>(index) < graph.parameters.size()
                   ? std::string_view(graph.parameters[static_cast<usize>(index)].name)
                   : std::string_view("?");
    };
    switch (held.kind) {
    case asset::GraphStateKind::None:
        return core::tr(ENG_TR("engine.editor.nodes.state.plays_nothing"));
    case asset::GraphStateKind::Clip:
        // Said to be what it plays: a state is most often called after its
        // clip, and two names one under the other say nothing of which is which.
        return core::tr(
            ENG_TR("engine.editor.nodes.state.plays_clip"),
            {{"clip", held.firstClip < graph.clips.size() ? std::string_view(graph.clips[held.firstClip].clip)
                                                          : std::string_view()}});
    case asset::GraphStateKind::Blend1:
        return core::tr(ENG_TR("engine.editor.nodes.state.blend_along"),
                        {{"parameter", parameter(held.blendX)}, {"clips", static_cast<core::i64>(held.clipCount)}});
    case asset::GraphStateKind::Blend2:
        return core::tr(ENG_TR("engine.editor.nodes.state.blend_across"),
                        {{"x", parameter(held.blendX)},
                         {"y", parameter(held.blendY)},
                         {"clips", static_cast<core::i64>(held.clipCount)}});
    }
    return {};
}

// A condition as it is written: `Grounded == false`, `Attack`.
[[nodiscard]] std::string conditionWords(const asset::AnimationGraph& graph, const asset::GraphCondition& condition)
{
    static constexpr std::array<std::string_view, 7> kOperators{"", "==", "~=", "<", "<=", ">", ">="};
    if (condition.parameter >= graph.parameters.size())
        return "?";
    const asset::GraphParameter& parameter = graph.parameters[condition.parameter];
    if (condition.op == asset::GraphOp::IsSet)
        return parameter.name;
    char value[32];
    if (parameter.kind == asset::GraphParameterKind::Boolean)
        (void)std::snprintf(value, sizeof(value), "%s", condition.value != 0.0f ? "true" : "false");
    else
        (void)std::snprintf(value, sizeof(value), "%g", static_cast<double>(condition.value));
    return parameter.name + " " + std::string(kOperators[static_cast<usize>(condition.op)]) + " " + value;
}

void drawStateNodes(const CanvasView& view, Pane& pane)
{
    const ThemePalette& palette = currentTheme().palette;
    const NodeCanvas& canvas = *view.canvas;
    const CanvasMetrics& metrics = view.metrics;
    AnimationDocument& document = pane.document.animation;
    const asset::AnimationGraph& graph = document.graph();
    const usize page = document.page();
    if (page >= graph.layers.size())
        return;
    const asset::GraphLayer& layer = graph.layers[page];
    const GraphLive::Layer* const running = liveLayer(pane);
    const float pad = view.scaled(10.0f);
    const float zoom = canvas.zoom();
    const std::optional<CanvasLink> chosen = canvas.selectedLink();

    // How much of the picture each state is of, while the game runs: the
    // state the layer is in, whole; what it is fading out of, by its weight.
    const auto runs = [&](u32 state) -> float {
        if (running == nullptr || state >= layer.states.size())
            return 0.0f;
        if (running->state == layer.states[state].name)
            return 1.0f;
        for (const auto& [name, weight] : running->fading) {
            if (name == layer.states[state].name)
                return std::clamp(weight, 0.05f, 0.85f);
        }
        return 0.0f;
    };

    // What each arrow waits for, said at its middle -- kept until the states
    // are drawn, so a state standing near does not lie over the words.
    struct Label
    {
        ImVec2 where;
        float wide = 0.0f;
        std::string words;
        bool selected = false;
    };
    std::vector<Label> labels;

    for (const CanvasLink& link : canvas.links()) {
        const CanvasNode* const from = canvas.node(link.from);
        const CanvasNode* const to = canvas.node(link.to);
        if (from == nullptr || to == nullptr)
            continue;
        const CanvasLine line = arrowLine(*from, *to, metrics);
        const bool selected = chosen.has_value() && *chosen == link;
        const bool taken = pane.document.takenFrames > 0 && pane.document.taken == link;
        const ImU32 colour =
            taken ? IM_COL32(255, 255, 255, 255) : (selected ? col(palette.accent) : col(palette.textMuted));
        drawArrow(view, view.at(line.from), view.at(line.to), colour, taken ? 4.0f : (selected ? 3.0f : 1.6f));
        // What it waits for, at its middle, where there is room to read it.
        if (zoom < 0.7f)
            continue;
        const std::optional<u32> target = document.stateOf(link.to);
        const std::optional<u32> source = document.stateOf(link.from);
        if (!target.has_value())
            continue;
        const std::vector<u32> between =
            document.transitionsBetween(source.has_value() ? static_cast<i32>(*source) : -1, *target);
        if (between.empty())
            continue;
        const asset::GraphTransition& first = layer.transitions[between[0]];
        std::string words;
        for (const asset::GraphCondition& condition : first.when) {
            if (!words.empty())
                words += ", ";
            words += conditionWords(graph, condition);
        }
        if (words.empty() && first.after >= 0.0f)
            words = core::tr(ENG_TR("engine.editor.nodes.arrow.at_its_end"));
        if (between.size() > 1)
            words += core::tr(ENG_TR("engine.editor.nodes.arrow.and_more"),
                              {{"count", static_cast<core::i64>(between.size() - 1)}});
        if (words.empty())
            continue;
        const ImVec2 a = view.at(line.from);
        const ImVec2 b = view.at(line.to);
        const float wide = view.widthOf(words);
        const float tall = view.fontSize * zoom;
        // **Beside the line, never on it, and the same side always**: above
        // it, or to its right where it runs straight up and down. Far
        // enough out that the words' box clears the line whatever its lean.
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
        float nx = -dy / length;
        float ny = dx / length;
        if (ny > 0.0f || (ny == 0.0f && nx < 0.0f)) {
            nx = -nx;
            ny = -ny;
        }
        const float out = (std::abs(nx) * wide + std::abs(ny) * tall) * 0.5f + 6.0f;
        const ImVec2 where(std::floor((a.x + b.x) * 0.5f + nx * out - wide * 0.5f),
                           std::floor((a.y + b.y) * 0.5f + ny * out - tall * 0.5f));
        labels.push_back({where, wide, std::move(words), selected});
    }

    const auto drawLabels = [&] {
        for (const Label& label : labels) {
            view.draw->AddRectFilled(
                ImVec2(label.where.x - 4.0f, label.where.y - 1.0f),
                ImVec2(label.where.x + label.wide + 4.0f, label.where.y + view.fontSize * zoom + 1.0f),
                col(palette.surface, 0.9f), 3.0f);
            view.text(label.where, label.selected ? col(palette.accent) : col(palette.textMuted), label.words);
        }
    };

    for (const CanvasNode& node : canvas.nodes()) {
        const CanvasRect rect = nodeRect(node, metrics);
        const ImVec2 a = view.at(rect.x, rect.y);
        const ImVec2 b = view.at(rect.x + rect.width, rect.y + rect.height);
        const bool any = node.kind == GraphAnyNode;
        const std::optional<u32> state = document.stateOf(node.id);
        const bool selected = canvas.selected(node.id);
        const float rounding = any ? (b.y - a.y) * 0.5f : view.scaled(6.0f);
        const float titleDrop = (view.scaled(metrics.titleHeight) - view.fontSize * zoom) * 0.5f;

        if (any) {
            view.draw->AddRectFilled(a, b, col(palette.surfaceRaised), rounding);
            view.draw->AddRect(a, b, selected ? col(palette.accent) : col(palette.textMuted), rounding, 0,
                               selected ? 2.0f : 1.0f);
            const std::string label = labelOf(GraphAnyNode);
            const float wide = view.widthOf(label);
            view.text(
                ImVec2(std::floor((a.x + b.x - wide) * 0.5f), std::floor((a.y + b.y - view.fontSize * zoom) * 0.5f)),
                col(palette.text), label);
            continue;
        }
        if (!state.has_value())
            continue;
        const bool start = layer.start == *state;
        const bool reachable = document.reachable(*state);
        const float lit = runs(*state);
        // A state nothing leads to is drawn as something that is not there
        // yet: hollow, faint, its edge broken, and said to have no way in.
        const float alpha = reachable ? 1.0f : 0.45f;
        if (reachable)
            view.draw->AddRectFilled(a, b, col(palette.surfaceRaised), rounding);
        if (lit > 0.0f)
            view.draw->AddRectFilled(a, b, col(palette.accentFill, lit), rounding);
        if (start)
            view.draw->AddRectFilled(a, ImVec2(a.x + view.scaled(5.0f), b.y), col(palette.success), rounding,
                                     ImDrawFlags_RoundCornersLeft);
        view.draw->PushClipRect(
            ImVec2(std::max(a.x, view.origin.x), std::max(a.y, view.origin.y)),
            ImVec2(std::min(b.x, view.origin.x + view.size.x), std::min(b.y, view.origin.y + view.size.y)), false);
        view.text(ImVec2(a.x + pad, a.y + titleDrop), col(palette.text, alpha), node.title);
        if (start) {
            const std::string words = core::tr(ENG_TR("engine.editor.nodes.state.start"));
            const float wide = view.widthOf(words);
            view.text(ImVec2(b.x - pad - wide, a.y + titleDrop), col(palette.success), words);
        }
        const float rowTop = a.y + view.scaled(metrics.titleHeight);
        const float rowDrop = (view.scaled(metrics.rowHeight) - view.fontSize * zoom) * 0.5f;
        if (!reachable) {
            view.text(ImVec2(a.x + pad, rowTop + rowDrop - 2.0f), col(palette.warning),
                      core::tr(ENG_TR("engine.editor.nodes.state.no_way_in")));
        }
        else {
            view.text(ImVec2(a.x + pad, rowTop + rowDrop - 2.0f),
                      lit >= 1.0f ? col(palette.onAccent) : col(palette.textMuted), stateWords(graph, page, *state));
        }
        view.draw->PopClipRect();
        if (reachable) {
            view.draw->AddRect(a, b, selected ? col(palette.accent) : col(palette.border), rounding, 0,
                               selected ? 2.0f : 1.0f);
        }
        else {
            // A broken edge: dashes along the top and the foot.
            const ImU32 edge = selected ? col(palette.accent) : col(palette.warning, 0.8f);
            const float dash = view.scaled(8.0f);
            for (float x = a.x + rounding; x < b.x - rounding; x += dash * 2.0f) {
                const float end = std::min(x + dash, b.x - rounding);
                view.draw->AddLine(ImVec2(x, a.y), ImVec2(end, a.y), edge, 1.5f);
                view.draw->AddLine(ImVec2(x, b.y), ImVec2(end, b.y), edge, 1.5f);
            }
            for (float y = a.y + rounding; y < b.y - rounding; y += dash * 2.0f) {
                const float end = std::min(y + dash, b.y - rounding);
                view.draw->AddLine(ImVec2(a.x, y), ImVec2(a.x, end), edge, 1.5f);
                view.draw->AddLine(ImVec2(b.x, y), ImVec2(b.x, end), edge, 1.5f);
            }
        }
        // How far through it the layer is, along its foot.
        if (running != nullptr && lit >= 1.0f) {
            const float through = std::clamp(running->progress, 0.0f, 1.0f);
            const float foot = view.scaled(4.0f);
            view.draw->AddRectFilled(ImVec2(a.x + 2.0f, b.y - foot - 2.0f),
                                     ImVec2(a.x + 2.0f + (b.x - a.x - 4.0f) * through, b.y - 2.0f),
                                     IM_COL32(255, 255, 255, 230), 2.0f);
        }
    }
    drawLabels();
}

// What a running graph did since the last frame: the arrow into the state a
// layer has just come to is lit for a moment.
void followRunning(Pane& pane)
{
    GraphDocument& document = pane.document;
    if (document.takenFrames > 0)
        --document.takenFrames;
    if (document.kind != GraphKind::Animation || !pane.live.playing || pane.live.graph != document.relative) {
        document.ranStates.clear();
        return;
    }
    const AnimationDocument& animation = document.animation;
    const asset::AnimationGraph& graph = animation.graph();
    std::vector<std::string> now(graph.layers.size());
    for (usize layer = 0; layer < graph.layers.size(); ++layer) {
        for (const GraphLive::Layer& held : pane.live.layers) {
            if (held.name == graph.layers[layer].name)
                now[layer] = held.state;
        }
    }
    const usize page = animation.page();
    if (page < now.size() && page < document.ranStates.size() && !document.ranStates[page].empty() &&
        document.ranStates[page] != now[page]) {
        const asset::GraphLayer& layer = graph.layers[page];
        const auto indexOf = [&layer](const std::string& name) -> i32 {
            for (usize state = 0; state < layer.states.size(); ++state) {
                if (layer.states[state].name == name)
                    return static_cast<i32>(state);
            }
            return -1;
        };
        const i32 from = indexOf(document.ranStates[page]);
        const i32 to = indexOf(now[page]);
        if (to >= 0) {
            // The arrow between the two where there is one; otherwise the
            // one that is taken from anywhere.
            const bool direct = from >= 0 && !animation.transitionsBetween(from, static_cast<u32>(to)).empty();
            const u32 left = direct ? animation.nodeOf(static_cast<u32>(from)) : animation.anyNode();
            document.taken = CanvasLink{left, 0, animation.nodeOf(static_cast<u32>(to)), 0};
            document.takenFrames = kTakenFrames;
        }
    }
    document.ranStates = std::move(now);
}

void drawCanvas(Pane& pane, ImVec2 size)
{
    GraphDocument& document = pane.document;
    NodeCanvas& canvas = document.canvas();
    const ThemePalette& palette = currentTheme().palette;
    const bool arrows = document.kind == GraphKind::Animation;
    const CanvasMetrics metrics = metricsOf(arrows);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    if (!document.framed) {
        canvas.frameAll(size.x, size.y, metrics);
        document.framed = true;
    }
    document.canvasAt = {origin.x, origin.y};
    document.canvasSize = {size.x, size.y};
    canvas.setLocked(pane.readOnly);
    followRunning(pane);

    CanvasPointer pointer;
    pointer.x = io.MousePos.x - origin.x;
    pointer.y = io.MousePos.y - origin.y;
    const NodeCanvas::Dragging::Kind inHand = canvas.dragging().kind;
    // How far the other button has travelled since it went down: a press
    // that went nowhere asks for the list, and one that went somewhere moved
    // the view -- which is still known in the frame it comes up.
    const bool rightDragged = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] >= 16.0f;
    CanvasEvent event;
    if (!document.ask.open) {
        pointer.pressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        pointer.down =
            ImGui::IsMouseDown(ImGuiMouseButton_Left) && (active || inHand != NodeCanvas::Dragging::Kind::Nothing);
        pointer.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                           inHand != NodeCanvas::Dragging::Kind::Nothing && inHand != NodeCanvas::Dragging::Kind::View;
        pointer.panning = (hovered || inHand == NodeCanvas::Dragging::Kind::View) &&
                          (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                           (ImGui::IsMouseDown(ImGuiMouseButton_Right) && rightDragged));
        pointer.adding = io.KeyShift || io.KeyCtrl;
        pointer.wheel = hovered ? io.MouseWheel : 0.0f;
        // A refusal is read until the next thing is begun.
        if (pointer.pressed)
            document.refusedFrames = 0;
        event = canvas.pointer(pointer, metrics);
    }

    // What the pointer did, taken into the graph.
    if (event.kind == CanvasEvent::Kind::LinkRefused && event.refusal != LinkRefusal::AlreadyMade) {
        document.refused = event.refusal;
        document.refusedLink = event.link;
        document.refusedFrames = kRefusedFrames;
    }
    else if (event.kind == CanvasEvent::Kind::LinkMade || event.kind == CanvasEvent::Kind::LinkDropped) {
        document.refusedFrames = 0;
    }
    if (event.kind == CanvasEvent::Kind::LinkDropped) {
        const GraphLoose loose{event.fromOutput ? event.link.from : event.link.to,
                               event.fromOutput ? event.link.output : event.link.input, event.fromOutput};
        openList(document, event.at, loose);
    }
    else if (document.kind == GraphKind::Surface) {
        document.surface.took(event);
    }
    else if (!pane.readOnly) {
        document.animation.took(event);
    }

    // **A right press that was not a drag, or the space bar, on the canvas**:
    // the list of what can be put there.
    const CanvasPoint here = canvas.toCanvas(pointer.x, pointer.y);
    if (hovered && !document.ask.open && !pane.readOnly) {
        const bool rightClick = ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !rightDragged;
        const bool key = ImGui::IsKeyPressed(ImGuiKey_Space, false) && !io.WantTextInput;
        if ((rightClick || key) && !canvas.nodeAt(here.x, here.y, metrics).has_value())
            openList(document, here, std::nullopt);
    }

    // The keys, while this graph's window has the keyboard and no field does.
    if (document.refocus) {
        document.refocus = false;
        ImGui::SetWindowFocus();
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused)
        g_keyboardFrame = ImGui::GetFrameCount();
    if (focused && !io.WantTextInput && !document.ask.open && !ImGui::IsAnyItemActive()) {
        const bool surface = document.kind == GraphKind::Surface;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            pane.out.save = pane.index;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl)
            canvas.frameAll(size.x, size.y, metrics);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            const std::string text = surface ? document.surface.copy() : document.animation.copy();
            if (!text.empty())
                ImGui::SetClipboardText(text.c_str());
        }
        if (!pane.readOnly) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                if (surface)
                    (void)document.surface.removeSelected();
                else
                    (void)document.animation.removeSelected();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (io.KeyShift)
                    (void)(surface ? document.surface.redo() : document.animation.redo());
                else
                    (void)(surface ? document.surface.undo() : document.animation.undo());
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                (void)(surface ? document.surface.redo() : document.animation.redo());
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
                const char* const text = ImGui::GetClipboardText();
                const CanvasPoint where = hovered ? here : canvas.toCanvas(size.x * 0.5f, size.y * 0.5f);
                if (text != nullptr) {
                    if (surface)
                        (void)document.surface.paste(text, where);
                    else
                        (void)document.animation.paste(text, where);
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && surface)
                (void)document.surface.duplicateSelected();
        }
    }

    // --- The picture ---------------------------------------------------------
    CanvasView view;
    view.draw = ImGui::GetWindowDrawList();
    view.origin = origin;
    view.size = size;
    view.canvas = &canvas;
    view.metrics = metrics;
    view.font = ImGui::GetFont();
    view.fontSize = ImGui::GetFontSize();
    view.draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    drawGrid(view);
    if (document.kind == GraphKind::Surface)
        drawSurfaceNodes(view, pane);
    else
        drawStateNodes(view, pane);

    // **A state's edge says it is one**: under the pointer it is lit, and
    // the pointer is a hand -- a press there draws an arrow out, where a
    // press a little further in would move the state.
    if (arrows && hovered && !pane.readOnly && !document.ask.open &&
        canvas.dragging().kind == NodeCanvas::Dragging::Kind::Nothing) {
        if (const std::optional<u32> rim = canvas.rimAt(here.x, here.y, metrics); rim.has_value()) {
            const CanvasNode* const edge = canvas.node(*rim);
            if (edge != nullptr && !edge->outputs.empty()) {
                const CanvasRect rect = nodeRect(*edge, metrics);
                const ImVec2 a = view.at(rect.x, rect.y);
                const ImVec2 b = view.at(rect.x + rect.width, rect.y + rect.height);
                const float rounding = edge->kind == GraphAnyNode ? (b.y - a.y) * 0.5f : view.scaled(6.0f);
                view.draw->AddRect(ImVec2(a.x - 2.0f, a.y - 2.0f), ImVec2(b.x + 2.0f, b.y + 2.0f), col(palette.accent),
                                   rounding + 2.0f, 0, 3.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
    }

    // What is in hand.
    const NodeCanvas::Dragging& dragging = canvas.dragging();
    if (dragging.kind == NodeCanvas::Dragging::Kind::Link) {
        const CanvasNode* const from = canvas.node(dragging.node);
        if (from != nullptr && arrows) {
            const CanvasRect rect = nodeRect(*from, metrics);
            drawArrow(view, view.at(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f), view.at(dragging.to),
                      col(palette.accent), 2.0f);
        }
        else if (from != nullptr) {
            const ImVec2 pin = view.at(dragging.fromOutput ? outputPlace(*from, dragging.pin, metrics)
                                                           : inputPlace(*from, dragging.pin, metrics));
            const ImVec2 loose = view.at(dragging.to);
            // Over a pin it could not be made to, it is drawn as refused and
            // says why before it is let go.
            LinkRefusal refusal = LinkRefusal::None;
            if (const std::optional<NodeCanvas::PinAt> over = canvas.pinAt(dragging.to.x, dragging.to.y, metrics);
                over.has_value() && over->output != dragging.fromOutput) {
                refusal = canvas.canLink(dragging.fromOutput
                                             ? CanvasLink{dragging.node, dragging.pin, over->node, over->pin}
                                             : CanvasLink{over->node, over->pin, dragging.node, dragging.pin});
            }
            const ImU32 colour = refusal != LinkRefusal::None ? col(palette.danger) : col(palette.accent);
            if (dragging.fromOutput)
                drawWire(view, pin, loose, colour, 2.0f);
            else
                drawWire(view, loose, pin, colour, 2.0f);
            if (refusal != LinkRefusal::None)
                pointerTip("%s", refusalWords(refusal));
        }
    }
    else if (document.ask.open && document.ask.loose.has_value()) {
        // The link that was let go, held out to where the list is, until
        // something is chosen for its other end.
        const CanvasNode* const from = canvas.node(document.ask.loose->node);
        if (from != nullptr && arrows) {
            const CanvasRect rect = nodeRect(*from, metrics);
            drawArrow(view, view.at(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f), view.at(document.ask.at),
                      col(palette.accent), 2.0f);
        }
        else if (from != nullptr) {
            const bool output = document.ask.loose->fromOutput;
            const ImVec2 pin = view.at(output ? outputPlace(*from, document.ask.loose->pin, metrics)
                                              : inputPlace(*from, document.ask.loose->pin, metrics));
            if (output)
                drawWire(view, pin, view.at(document.ask.at), col(palette.accent), 2.0f);
            else
                drawWire(view, view.at(document.ask.at), pin, col(palette.accent), 2.0f);
        }
    }
    else if (dragging.kind == NodeCanvas::Dragging::Kind::Box) {
        const ImVec2 a = view.at(dragging.from);
        const ImVec2 b = view.at(dragging.to);
        view.draw->AddRectFilled(a, b, col(palette.accent, 0.12f));
        view.draw->AddRect(a, b, col(palette.accent, 0.8f));
    }

    // The last link that was refused: drawn where it would have been, with
    // why, for long enough to be read.
    if (document.refusedFrames > 0) {
        --document.refusedFrames;
        const CanvasNode* const from = canvas.node(document.refusedLink.from);
        const CanvasNode* const to = canvas.node(document.refusedLink.to);
        if (from != nullptr && to != nullptr) {
            ImVec2 a;
            ImVec2 b;
            if (arrows) {
                const CanvasLine line = arrowLine(*from, *to, metrics);
                a = view.at(line.from);
                b = view.at(line.to);
                drawArrow(view, a, b, col(palette.danger), 2.0f);
            }
            else {
                a = view.at(outputPlace(
                    *from, std::min<u32>(document.refusedLink.output, static_cast<u32>(from->outputs.size()) - 1),
                    metrics));
                b = view.at(inputPlace(*to, document.refusedLink.input, metrics));
                drawWire(view, a, b, col(palette.danger), 2.0f);
            }
            const std::string words = refusalWords(document.refused);
            const float wide = ImGui::CalcTextSize(words.c_str()).x;
            const ImVec2 where(std::floor((a.x + b.x - wide) * 0.5f), std::floor((a.y + b.y) * 0.5f + 8.0f));
            const ImVec2 low(where.x - 12.0f, where.y - 3.0f);
            const ImVec2 high(where.x + wide + 6.0f, where.y + ImGui::GetFontSize() + 3.0f);
            view.draw->AddRectFilled(low, high, col(palette.background, 0.95f), 4.0f);
            view.draw->AddRectFilled(low, ImVec2(low.x + 6.0f, high.y), col(palette.danger), 4.0f,
                                     ImDrawFlags_RoundCornersLeft);
            view.draw->AddRect(low, high, col(palette.danger), 4.0f);
            view.draw->AddText(where, col(palette.danger), words.c_str());
        }
    }

    // A graph that is running is looked at: said along the top, in words.
    if (pane.readOnly) {
        const char* const words = core::tr(ENG_TR("engine.editor.nodes.running_note"));
        const float tall = ImGui::GetFontSize() + 10.0f;
        view.draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + tall), col(palette.accentFill, 0.9f));
        view.draw->AddText(ImVec2(origin.x + 10.0f, origin.y + 5.0f), col(palette.onAccent), words);
    }
    view.draw->PopClipRect();
    drawList(pane, origin);
}

// --- The fields beside the canvas ------------------------------------------------------

constexpr std::array<std::string_view, 6> kParameterTypes{"number",  "vector2", "vector3",
                                                          "vector4", "colour",  "toggle"};
constexpr std::array<std::string_view, 4> kValueTypes{"number", "vector2", "vector3", "vector4"};

[[nodiscard]] const char* typeLabel(std::string_view type)
{
    if (type == "number")
        return core::tr(ENG_TR("engine.editor.nodes.type.number"));
    if (type == "vector2")
        return core::tr(ENG_TR("engine.editor.nodes.type.vector2"));
    if (type == "vector3")
        return core::tr(ENG_TR("engine.editor.nodes.type.vector3"));
    if (type == "vector4")
        return core::tr(ENG_TR("engine.editor.nodes.type.vector4"));
    if (type == "colour")
        return core::tr(ENG_TR("engine.editor.nodes.type.colour"));
    return core::tr(ENG_TR("engine.editor.nodes.type.toggle"));
}

void drawSurfaceFields(Pane& pane)
{
    SurfaceDocument& surface = pane.document.surface;
    const NodeCanvas& canvas = surface.canvas;
    const ThemePalette& palette = currentTheme().palette;

    sectionTitle(core::tr(ENG_TR("engine.editor.nodes.section.graph")));
    ImGui::PushTextWrapPos(0.0f);
    // What it writes, and that it is one way: said where the graph is, not
    // only in the manual.
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.writes"),
                                       {{"file", std::string_view(asset::surfaceShaderPathOf(pane.document.title))}})
                                  .c_str());
    ImGui::TextColored(ImVec4(pane.statusBad ? palette.danger.r : palette.textMuted.r,
                              pane.statusBad ? palette.danger.g : palette.textMuted.g,
                              pane.statusBad ? palette.danger.b : palette.textMuted.b, 1.0f),
                       "%s", pane.status.c_str());
    ImGui::PopTextWrapPos();

    if (canvas.selection().size() == 1) {
        const u32 id = canvas.selection()[0];
        const CanvasNode* const node = canvas.node(id);
        if (node != nullptr) {
            sectionTitle(labelOf(node->kind).c_str());
            ImGui::PushID(static_cast<int>(id));
            fieldName(core::tr(ENG_TR("engine.editor.nodes.field.name")));
            if (const std::optional<std::string> typed =
                    textField("##title", node->title, core::tr(ENG_TR("engine.editor.nodes.field.name_hint")));
                typed.has_value())
                (void)surface.rename(id, *typed);
            // A copy: an edit moves what `node` points into.
            const std::vector<CanvasField> fields = node->fields;
            const std::string kind = node->kind;
            for (const CanvasField& field : fields) {
                ImGui::PushID(field.name.c_str());
                fieldName(field.name.c_str());
                if (field.name == "Type") {
                    const std::span<const std::string_view> types =
                        kind == "Parameter" ? std::span<const std::string_view>(kParameterTypes)
                                            : std::span<const std::string_view>(kValueTypes);
                    if (ImGui::BeginCombo("##type", typeLabel(field.value))) {
                        for (const std::string_view type : types) {
                            if (ImGui::Selectable(typeLabel(type), type == field.value) && type != field.value)
                                (void)surface.setField(id, field.name, type);
                        }
                        ImGui::EndCombo();
                    }
                }
                else if (field.name == "Width") {
                    if (ImGui::BeginCombo("##width", field.value.c_str())) {
                        for (const char* const width : {"2", "3", "4"}) {
                            if (ImGui::Selectable(width, field.value == width) && field.value != width)
                                (void)surface.setField(id, field.name, width);
                        }
                        ImGui::EndCombo();
                    }
                }
                else if (const std::optional<std::string> typed =
                             textField("##value", field.value, "", field.name == "Code");
                         typed.has_value()) {
                    (void)surface.setField(id, field.name, *typed);
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }
    else if (canvas.selection().empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.hint.surface")));
        ImGui::PopTextWrapPos();
    }

    if (!pane.problems.empty()) {
        sectionTitle(core::tr(ENG_TR("engine.editor.nodes.section.problems")));
        ImGui::PushTextWrapPos(0.0f);
        int row = 0;
        for (const auto& [node, words] : pane.problems) {
            ImGui::PushID(row++);
            const CanvasNode* const held = canvas.node(node);
            const std::string name =
                held == nullptr ? std::string() : (held->title.empty() ? labelOf(held->kind) : held->title);
            ImGui::TextColored(ImVec4(palette.danger.r, palette.danger.g, palette.danger.b, 1.0f), "%s%s%s",
                               name.c_str(), name.empty() ? "" : ": ", words.c_str());
            // A press on what is wrong goes to where it is wrong.
            if (held != nullptr && ImGui::IsItemClicked())
                surface.canvas.select(node);
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
    }
}

// The points of a blend, drawn: along a line for one parameter, on a plane
// with its triangles for two -- and where the game has the two parameters
// now, while it runs.
void drawBlendPlot(const Pane& pane, const asset::GraphState& state, const std::vector<asset::GraphClip>& clips)
{
    const ThemePalette& palette = currentTheme().palette;
    const asset::AnimationGraph& graph = pane.document.animation.graph();
    const bool two = state.kind == asset::GraphStateKind::Blend2;
    const float wide = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(wide, two ? std::min(wide, ImGui::GetFontSize() * 13.0f) : ImGui::GetFontSize() * 3.2f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), col(palette.surface), 4.0f);
    if (clips.empty())
        return;
    f32 low[2] = {clips[0].at[0], clips[0].at[1]};
    f32 high[2] = {clips[0].at[0], clips[0].at[1]};
    for (const asset::GraphClip& clip : clips) {
        for (int axis = 0; axis < 2; ++axis) {
            low[axis] = std::min(low[axis], clip.at[axis]);
            high[axis] = std::max(high[axis], clip.at[axis]);
        }
    }
    const float margin = ImGui::GetFontSize() * 1.4f;
    const auto place = [&](f32 x, f32 y) {
        const f32 spanX = std::max(high[0] - low[0], 1.0e-3f);
        const f32 spanY = std::max(high[1] - low[1], 1.0e-3f);
        const float px = origin.x + margin + (x - low[0]) / spanX * (size.x - margin * 2.0f);
        // Up is forward: the plot is read as the stick is pushed.
        const float py = two ? origin.y + size.y - margin - (y - low[1]) / spanY * (size.y - margin * 2.0f)
                             : origin.y + size.y * 0.62f;
        return ImVec2(std::floor(px), std::floor(py));
    };
    if (two) {
        for (const std::array<u32, 3>& triangle : state.triangles) {
            if (triangle[0] >= clips.size() || triangle[1] >= clips.size() || triangle[2] >= clips.size())
                continue;
            draw->AddTriangle(place(clips[triangle[0]].at[0], clips[triangle[0]].at[1]),
                              place(clips[triangle[1]].at[0], clips[triangle[1]].at[1]),
                              place(clips[triangle[2]].at[0], clips[triangle[2]].at[1]), col(palette.textMuted, 0.55f),
                              1.0f);
        }
    }
    else {
        draw->AddLine(place(low[0], 0.0f), place(high[0], 0.0f), col(palette.border), 1.0f);
    }
    for (const asset::GraphClip& clip : clips) {
        const ImVec2 at = place(clip.at[0], clip.at[1]);
        draw->AddCircleFilled(at, 4.0f, col(palette.accent));
        const float textWide = ImGui::CalcTextSize(clip.clip.c_str()).x;
        const float x = std::clamp(at.x - textWide * 0.5f, origin.x + 2.0f, origin.x + size.x - textWide - 2.0f);
        draw->AddText(ImVec2(std::floor(x), at.y - ImGui::GetFontSize() - 5.0f), col(palette.textMuted),
                      clip.clip.c_str());
    }
    // Where the two parameters are in the game that is running.
    if (pane.live.playing && pane.live.graph == pane.document.relative) {
        const auto valueOf = [&](i32 index) -> std::optional<f32> {
            if (index < 0 || static_cast<usize>(index) >= graph.parameters.size())
                return std::nullopt;
            for (const GraphLive::Parameter& parameter : pane.live.parameters) {
                if (parameter.name == graph.parameters[static_cast<usize>(index)].name)
                    return parameter.value;
            }
            return std::nullopt;
        };
        const std::optional<f32> x = valueOf(state.blendX);
        const std::optional<f32> y = two ? valueOf(state.blendY) : std::optional<f32>(0.0f);
        if (x.has_value() && y.has_value()) {
            const ImVec2 at = place(std::clamp(*x, low[0], high[0]), std::clamp(*y, low[1], high[1]));
            draw->AddCircle(at, 7.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
        }
    }
}

// The parameters a number may be read from, as a combo. Says the one chosen.
[[nodiscard]] std::optional<i32> parameterField(const char* id, const asset::AnimationGraph& graph, i32 current,
                                                bool numbersOnly)
{
    std::optional<i32> chosen;
    const char* const shown = current >= 0 && static_cast<usize>(current) < graph.parameters.size()
                                  ? graph.parameters[static_cast<usize>(current)].name.c_str()
                                  : "";
    if (ImGui::BeginCombo(id, shown)) {
        for (usize index = 0; index < graph.parameters.size(); ++index) {
            if (numbersOnly && graph.parameters[index].kind != asset::GraphParameterKind::Number)
                continue;
            const bool is = static_cast<i32>(index) == current;
            if (ImGui::Selectable(graph.parameters[index].name.c_str(), is) && !is)
                chosen = static_cast<i32>(index);
        }
        ImGui::EndCombo();
    }
    return chosen;
}

void drawParameters(Pane& pane)
{
    AnimationDocument& document = pane.document.animation;
    const asset::AnimationGraph& graph = document.graph();
    const bool running = pane.live.playing && pane.live.graph == pane.document.relative;
    sectionTitle(core::tr(ENG_TR("engine.editor.nodes.section.parameters")));
    const std::array<const char*, 3> kinds{core::tr(ENG_TR("engine.editor.nodes.parameter.number")),
                                           core::tr(ENG_TR("engine.editor.nodes.parameter.boolean")),
                                           core::tr(ENG_TR("engine.editor.nodes.parameter.trigger"))};
    const float font = ImGui::GetFontSize();
    for (u32 index = 0; index < graph.parameters.size(); ++index) {
        // A copy: an edit replaces the graph this is read from.
        const asset::GraphParameter parameter = graph.parameters[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(font * 6.5f);
        if (const std::optional<std::string> typed = textField("##name", parameter.name); typed.has_value()) {
            (void)document.edit([&](asset::AnimationGraph& edited) { edited.parameters[index].name = *typed; });
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 5.5f);
        if (const std::optional<usize> kind = choiceField("##kind", kinds, static_cast<usize>(parameter.kind));
            kind.has_value()) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                edited.parameters[index].kind = static_cast<asset::GraphParameterKind>(*kind);
                edited.parameters[index].rest = 0.0f;
            });
        }
        ImGui::SameLine();
        if (running) {
            // What it is now, in the game.
            for (const GraphLive::Parameter& now : pane.live.parameters) {
                if (now.name != parameter.name)
                    continue;
                if (parameter.kind == asset::GraphParameterKind::Number)
                    ImGui::Text("%.2f", static_cast<double>(now.value));
                else
                    ImGui::TextUnformatted(now.value != 0.0f ? core::tr(ENG_TR("engine.editor.nodes.parameter.set"))
                                                             : core::tr(ENG_TR("engine.editor.nodes.parameter.unset")));
            }
        }
        else {
            if (parameter.kind == asset::GraphParameterKind::Number) {
                float rest = parameter.rest;
                ImGui::SetNextItemWidth(font * 4.0f);
                (void)ImGui::DragFloat("##rest", &rest, 0.05f, 0.0f, 0.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(rest))
                    (void)document.edit([&](asset::AnimationGraph& edited) { edited.parameters[index].rest = rest; });
            }
            else if (parameter.kind == asset::GraphParameterKind::Boolean) {
                bool rest = parameter.rest != 0.0f;
                if (ImGui::Checkbox("##rest", &rest)) {
                    (void)document.edit(
                        [&](asset::AnimationGraph& edited) { edited.parameters[index].rest = rest ? 1.0f : 0.0f; });
                }
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                (void)document.removeParameter(index);
            itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.parameter.remove_tip")));
        }
        // Where it is read from when nothing sets it.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (const std::optional<std::string> typed =
                textField("##from", parameter.from, core::tr(ENG_TR("engine.editor.nodes.parameter.from_hint")));
            typed.has_value()) {
            (void)document.edit([&](asset::AnimationGraph& edited) { edited.parameters[index].from = *typed; });
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.parameter.add_number"))))
        (void)document.addParameter(asset::GraphParameterKind::Number);
    ImGui::SameLine();
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.parameter.add_boolean"))))
        (void)document.addParameter(asset::GraphParameterKind::Boolean);
    ImGui::SameLine();
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.parameter.add_trigger"))))
        (void)document.addParameter(asset::GraphParameterKind::Trigger);
}

void drawStateFields(Pane& pane, u32 state)
{
    AnimationDocument& document = pane.document.animation;
    const asset::AnimationGraph& graph = document.graph();
    const usize page = document.page();
    // Copies: an edit replaces the graph these are read from.
    const asset::GraphState held = graph.layers[page].states[state];
    const std::vector<asset::GraphClip> clips = graphedit::clipsOf(graph, page, state);
    const bool start = graph.layers[page].start == state;

    sectionTitle(core::tr(ENG_TR("engine.editor.nodes.section.state")));
    fieldName(core::tr(ENG_TR("engine.editor.nodes.field.name")));
    if (const std::optional<std::string> typed = textField("##state-name", held.name); typed.has_value())
        (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].states[state].name = *typed; });
    bool starts = start;
    fieldName(core::tr(ENG_TR("engine.editor.nodes.state.starts_here")));
    if (ImGui::Checkbox("##start", &starts) && starts)
        (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].start = state; });
    if (!document.reachable(state)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(currentTheme().palette.warning.r, currentTheme().palette.warning.g,
                                  currentTheme().palette.warning.b, 1.0f),
                           "%s", core::tr(ENG_TR("engine.editor.nodes.state.no_way_in_long")));
        ImGui::PopTextWrapPos();
    }

    const std::array<const char*, 4> kinds{core::tr(ENG_TR("engine.editor.nodes.state.kind_nothing")),
                                           core::tr(ENG_TR("engine.editor.nodes.state.kind_clip")),
                                           core::tr(ENG_TR("engine.editor.nodes.state.kind_blend1")),
                                           core::tr(ENG_TR("engine.editor.nodes.state.kind_blend2"))};
    fieldName(core::tr(ENG_TR("engine.editor.nodes.state.plays")));
    if (const std::optional<usize> kind = choiceField("##kind", kinds, static_cast<usize>(held.kind));
        kind.has_value()) {
        (void)document.edit([&](asset::AnimationGraph& edited) {
            graphedit::setKind(edited, page, state, static_cast<asset::GraphStateKind>(*kind));
        });
    }

    const bool two = held.kind == asset::GraphStateKind::Blend2;
    const bool blend = two || held.kind == asset::GraphStateKind::Blend1;
    if (held.kind == asset::GraphStateKind::Clip && !clips.empty()) {
        fieldName(core::tr(ENG_TR("engine.editor.nodes.state.clip")));
        if (const std::optional<std::string> typed = textField("##clip", clips[0].clip); typed.has_value()) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                std::vector<asset::GraphClip> changed = graphedit::clipsOf(edited, page, state);
                if (!changed.empty()) {
                    changed[0].clip = *typed;
                    changed[0].events.clear();
                }
                graphedit::setClips(edited, page, state, std::move(changed));
            });
        }
    }
    if (blend) {
        fieldName(
            core::tr(two ? ENG_TR("engine.editor.nodes.state.across") : ENG_TR("engine.editor.nodes.state.along")));
        if (const std::optional<i32> chosen = parameterField("##blend-x", graph, held.blendX, true); chosen.has_value())
            (void)document.edit(
                [&](asset::AnimationGraph& edited) { edited.layers[page].states[state].blendX = *chosen; });
        if (two) {
            fieldName(core::tr(ENG_TR("engine.editor.nodes.state.and")));
            if (const std::optional<i32> chosen = parameterField("##blend-y", graph, held.blendY, true);
                chosen.has_value()) {
                (void)document.edit(
                    [&](asset::AnimationGraph& edited) { edited.layers[page].states[state].blendY = *chosen; });
            }
        }
        drawBlendPlot(pane, held, clips);
        // A row a clip: its name, and where it sits.
        const float font = ImGui::GetFontSize();
        for (u32 index = 0; index < clips.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetNextItemWidth(font * 8.0f);
            if (const std::optional<std::string> typed = textField("##clip", clips[index].clip); typed.has_value()) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    std::vector<asset::GraphClip> changed = graphedit::clipsOf(edited, page, state);
                    if (index < changed.size()) {
                        changed[index].clip = *typed;
                        changed[index].events.clear();
                    }
                    graphedit::setClips(edited, page, state, std::move(changed));
                });
            }
            ImGui::SameLine();
            float at[2] = {clips[index].at[0], clips[index].at[1]};
            ImGui::SetNextItemWidth(font * (two ? 8.0f : 4.5f));
            if (two)
                (void)ImGui::DragFloat2("##at", at, 0.05f, 0.0f, 0.0f, "%.2f");
            else
                (void)ImGui::DragFloat("##at", at, 0.05f, 0.0f, 0.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(at[0]) && std::isfinite(at[1])) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    std::vector<asset::GraphClip> changed = graphedit::clipsOf(edited, page, state);
                    if (index < changed.size()) {
                        changed[index].at[0] = at[0];
                        changed[index].at[1] = at[1];
                    }
                    graphedit::setClips(edited, page, state, std::move(changed));
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    std::vector<asset::GraphClip> changed = graphedit::clipsOf(edited, page, state);
                    if (index < changed.size())
                        changed.erase(changed.begin() + index);
                    graphedit::setClips(edited, page, state, std::move(changed));
                });
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.state.add_clip")))) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                std::vector<asset::GraphClip> changed = graphedit::clipsOf(edited, page, state);
                asset::GraphClip added;
                added.clip = changed.empty() ? held.name : changed.back().clip;
                // A step past the last, so it is not drawn on it.
                added.at[0] = changed.empty() ? 0.0f : changed.back().at[0] + 1.0f;
                added.at[1] = changed.empty() ? 0.0f : changed.back().at[1];
                changed.push_back(std::move(added));
                graphedit::setClips(edited, page, state, std::move(changed));
            });
        }
    }
    if (held.kind != asset::GraphStateKind::None) {
        bool loop = held.loop;
        fieldName(core::tr(ENG_TR("engine.editor.nodes.state.loop")));
        if (ImGui::Checkbox("##loop", &loop))
            (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].states[state].loop = loop; });
        if (blend) {
            bool sync = held.sync;
            fieldName(core::tr(ENG_TR("engine.editor.nodes.state.sync")));
            if (ImGui::Checkbox("##sync", &sync))
                (void)document.edit(
                    [&](asset::AnimationGraph& edited) { edited.layers[page].states[state].sync = sync; });
            itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.state.sync_tip")));
        }
        float speed = held.speed;
        fieldName(core::tr(ENG_TR("engine.editor.nodes.state.speed")));
        (void)ImGui::DragFloat("##speed", &speed, 0.01f, 0.0f, 10.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(speed))
            (void)document.edit(
                [&](asset::AnimationGraph& edited) { edited.layers[page].states[state].speed = speed; });
    }
}

void drawArrowFields(Pane& pane, const CanvasLink& link)
{
    AnimationDocument& document = pane.document.animation;
    const asset::AnimationGraph& graph = document.graph();
    const usize page = document.page();
    const std::optional<u32> to = document.stateOf(link.to);
    const std::optional<u32> from = document.stateOf(link.from);
    if (!to.has_value())
        return;
    const i32 left = from.has_value() ? static_cast<i32>(*from) : -1;
    const asset::GraphLayer& layer = graph.layers[page];
    const std::string title =
        core::tr(ENG_TR("engine.editor.nodes.section.arrow"),
                 {{"from", left < 0 ? std::string_view(core::tr(ENG_TR("engine.editor.nodes.kind.any")))
                                    : std::string_view(layer.states[static_cast<usize>(left)].name)},
                  {"to", std::string_view(layer.states[*to].name)}});
    sectionTitle(title.c_str());
    static constexpr std::array<const char*, 6> kOperators{"==", "~=", "<", "<=", ">", ">="};
    const float font = ImGui::GetFontSize();
    const std::vector<u32> between = document.transitionsBetween(left, *to);
    for (usize row = 0; row < between.size(); ++row) {
        const u32 index = between[row];
        // A copy: an edit replaces the graph it is read from.
        const asset::GraphTransition arrow = layer.transitions[index];
        ImGui::PushID(static_cast<int>(index));
        if (between.size() > 1) {
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.arrow.numbered"),
                                               {{"number", static_cast<core::i64>(row + 1)}})
                                          .c_str());
        }
        if (arrow.when.empty())
            ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.arrow.always")));
        for (u32 at = 0; at < arrow.when.size(); ++at) {
            const asset::GraphCondition condition = arrow.when[at];
            ImGui::PushID(static_cast<int>(at));
            ImGui::SetNextItemWidth(font * 7.0f);
            if (const std::optional<i32> chosen =
                    parameterField("##parameter", graph, static_cast<i32>(condition.parameter), false);
                chosen.has_value()) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    asset::GraphCondition& changed = edited.layers[page].transitions[index].when[at];
                    changed.parameter = static_cast<u32>(*chosen);
                    // A trigger is asked whether it fired, and nothing else.
                    if (edited.parameters[changed.parameter].kind == asset::GraphParameterKind::Trigger)
                        changed.op = asset::GraphOp::IsSet;
                });
            }
            const bool trigger = condition.parameter < graph.parameters.size() &&
                                 graph.parameters[condition.parameter].kind == asset::GraphParameterKind::Trigger;
            const bool boolean = condition.parameter < graph.parameters.size() &&
                                 graph.parameters[condition.parameter].kind == asset::GraphParameterKind::Boolean;
            if (!trigger) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(font * 3.4f);
                const usize current = condition.op == asset::GraphOp::IsSet ? 0 : static_cast<usize>(condition.op) - 1;
                if (const std::optional<usize> chosen = choiceField("##op", kOperators, current); chosen.has_value()) {
                    (void)document.edit([&](asset::AnimationGraph& edited) {
                        edited.layers[page].transitions[index].when[at].op = static_cast<asset::GraphOp>(*chosen + 1);
                    });
                }
                ImGui::SameLine();
                if (boolean) {
                    bool value = condition.value != 0.0f;
                    if (ImGui::Checkbox("##value", &value)) {
                        (void)document.edit([&](asset::AnimationGraph& edited) {
                            asset::GraphCondition& changed = edited.layers[page].transitions[index].when[at];
                            changed.value = value ? 1.0f : 0.0f;
                            if (changed.op == asset::GraphOp::IsSet)
                                changed.op = asset::GraphOp::Equal;
                        });
                    }
                }
                else {
                    float value = condition.value;
                    ImGui::SetNextItemWidth(font * 4.0f);
                    (void)ImGui::DragFloat("##value", &value, 0.05f, 0.0f, 0.0f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(value)) {
                        (void)document.edit([&](asset::AnimationGraph& edited) {
                            asset::GraphCondition& changed = edited.layers[page].transitions[index].when[at];
                            changed.value = value;
                            if (changed.op == asset::GraphOp::IsSet)
                                changed.op = asset::GraphOp::Greater;
                        });
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    std::vector<asset::GraphCondition>& when = edited.layers[page].transitions[index].when;
                    when.erase(when.begin() + at);
                });
            }
            ImGui::PopID();
        }
        if (!graph.parameters.empty() &&
            ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.arrow.add_condition")))) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                asset::GraphCondition added;
                added.parameter = 0;
                const asset::GraphParameterKind kind = edited.parameters[0].kind;
                added.op = kind == asset::GraphParameterKind::Trigger
                               ? asset::GraphOp::IsSet
                               : (kind == asset::GraphParameterKind::Boolean ? asset::GraphOp::Equal
                                                                             : asset::GraphOp::Greater);
                added.value = kind == asset::GraphParameterKind::Boolean ? 1.0f : 0.0f;
                edited.layers[page].transitions[index].when.push_back(added);
            });
        }
        // Not before this much of the state has played.
        bool waits = arrow.after >= 0.0f;
        fieldName(core::tr(ENG_TR("engine.editor.nodes.arrow.after")));
        if (ImGui::Checkbox("##waits", &waits)) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                edited.layers[page].transitions[index].after = waits ? 1.0f : -1.0f;
            });
        }
        itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.arrow.after_tip")));
        if (arrow.after >= 0.0f) {
            ImGui::SameLine();
            float after = arrow.after;
            ImGui::SetNextItemWidth(-FLT_MIN);
            (void)ImGui::DragFloat("##after", &after, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(after))
                (void)document.edit(
                    [&](asset::AnimationGraph& edited) { edited.layers[page].transitions[index].after = after; });
        }
        float fade = arrow.fade;
        fieldName(core::tr(ENG_TR("engine.editor.nodes.arrow.fade")));
        (void)ImGui::DragFloat("##fade", &fade, 0.01f, 0.0f, 5.0f, "%.2f s");
        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(fade))
            (void)document.edit(
                [&](asset::AnimationGraph& edited) { edited.layers[page].transitions[index].fade = fade; });
        if (between.size() > 1 && ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.arrow.remove")))) {
            (void)document.edit([&](asset::AnimationGraph& edited) {
                std::vector<asset::GraphTransition>& arrows = edited.layers[page].transitions;
                arrows.erase(arrows.begin() + index);
            });
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    // Another way from the one state to the other, with conditions of its own.
    if (ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.arrow.add_another"))))
        (void)document.addTransition(left, *to);
}

void drawLayerFields(Pane& pane)
{
    AnimationDocument& document = pane.document.animation;
    const asset::AnimationGraph& graph = document.graph();
    const usize page = document.page();
    if (page >= graph.layers.size())
        return;
    // A copy: an edit replaces the graph it is read from.
    const asset::GraphLayer layer = graph.layers[page];
    sectionTitle(core::tr(ENG_TR("engine.editor.nodes.section.layer")));
    fieldName(core::tr(ENG_TR("engine.editor.nodes.field.name")));
    if (const std::optional<std::string> typed = textField("##layer-name", layer.name); typed.has_value())
        (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].name = *typed; });
    bool additive = layer.additive;
    fieldName(core::tr(ENG_TR("engine.editor.nodes.layer.additive")));
    if (ImGui::Checkbox("##additive", &additive))
        (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].additive = additive; });
    itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.layer.additive_tip")));
    // Its weight: a number, or a parameter that says it.
    fieldName(core::tr(ENG_TR("engine.editor.nodes.layer.weight")));
    if (layer.weightParameter < 0) {
        float weight = layer.weight;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        (void)ImGui::DragFloat("##weight", &weight, 0.01f, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(weight))
            (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].weight = weight; });
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* const fixed = core::tr(ENG_TR("engine.editor.nodes.layer.weight_fixed"));
    if (ImGui::BeginCombo("##weight-from",
                          layer.weightParameter >= 0 &&
                                  static_cast<usize>(layer.weightParameter) < graph.parameters.size()
                              ? graph.parameters[static_cast<usize>(layer.weightParameter)].name.c_str()
                              : fixed)) {
        if (ImGui::Selectable(fixed, layer.weightParameter < 0) && layer.weightParameter >= 0)
            (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].weightParameter = -1; });
        for (usize index = 0; index < graph.parameters.size(); ++index) {
            if (graph.parameters[index].kind == asset::GraphParameterKind::Trigger)
                continue;
            const bool is = static_cast<i32>(index) == layer.weightParameter;
            if (ImGui::Selectable(graph.parameters[index].name.c_str(), is) && !is) {
                (void)document.edit([&](asset::AnimationGraph& edited) {
                    edited.layers[page].weightParameter = static_cast<i32>(index);
                });
            }
        }
        ImGui::EndCombo();
    }
    // The joints it moves, each with everything below it.
    std::string mask;
    for (const std::string& joint : layer.mask) {
        if (!mask.empty())
            mask += ", ";
        mask += joint;
    }
    fieldName(core::tr(ENG_TR("engine.editor.nodes.layer.mask")));
    if (const std::optional<std::string> typed =
            textField("##mask", mask, core::tr(ENG_TR("engine.editor.nodes.layer.mask_hint")));
        typed.has_value()) {
        std::vector<std::string> joints;
        std::string joint;
        for (const char c : *typed + ",") {
            if (c != ',') {
                if (c != ' ' || !joint.empty())
                    joint.push_back(c);
                continue;
            }
            while (!joint.empty() && joint.back() == ' ')
                joint.pop_back();
            if (!joint.empty())
                joints.push_back(std::move(joint));
            joint.clear();
        }
        (void)document.edit([&](asset::AnimationGraph& edited) { edited.layers[page].mask = joints; });
    }
    if (graph.layers.size() > 1 && ImGui::SmallButton(core::tr(ENG_TR("engine.editor.nodes.layer.remove"))))
        (void)document.removeLayer(page);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.hint.animation")));
    ImGui::PopTextWrapPos();
}

void drawAnimationFields(Pane& pane)
{
    AnimationDocument& document = pane.document.animation;
    const ThemePalette& palette = currentTheme().palette;
    // Why the last edit was not made, in the reader's words, until the next.
    if (const std::string refused = refusedWords(document.refusal()); !refused.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(palette.danger.r, palette.danger.g, palette.danger.b, 1.0f), "%s", refused.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(pane.readOnly);
    const NodeCanvas& canvas = document.canvas;
    const std::optional<CanvasLink> arrow = canvas.selectedLink();
    std::optional<u32> state;
    if (canvas.selection().size() == 1)
        state = document.stateOf(canvas.selection()[0]);
    if (arrow.has_value())
        drawArrowFields(pane, *arrow);
    else if (state.has_value())
        drawStateFields(pane, *state);
    else
        drawLayerFields(pane);
    drawParameters(pane);
    ImGui::EndDisabled();
}

// The bar over the canvas: what is done to the whole graph, and -- for an
// animation graph -- its layers, a page each.
void drawBar(Pane& pane)
{
    GraphDocument& document = pane.document;
    const bool surface = document.kind == GraphKind::Surface;
    ImGui::BeginDisabled(!document.dirty());
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.nodes.bar.save"))))
        pane.out.save = pane.index;
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canUndo = surface ? document.surface.canvas.canUndo() : document.animation.canUndo();
    const bool canRedo = surface ? document.surface.canvas.canRedo() : document.animation.canRedo();
    ImGui::BeginDisabled(!canUndo || pane.readOnly);
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.nodes.bar.undo"))))
        (void)(surface ? document.surface.undo() : document.animation.undo());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo || pane.readOnly);
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.nodes.bar.redo"))))
        (void)(surface ? document.surface.redo() : document.animation.redo());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.nodes.bar.frame_all"))))
        document.framed = false;
    itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.bar.frame_all_tip")));
    ImGui::SameLine();
    ImGui::BeginDisabled(pane.readOnly);
    if (ImGui::Button(core::tr(ENG_TR("engine.editor.nodes.bar.add")))) {
        const NodeCanvas& canvas = document.canvas();
        // Where the view's middle is, a little off for each already there.
        const CanvasPoint middle =
            canvas.toCanvas(ImGui::GetContentRegionAvail().x * 0.3f, ImGui::GetFontSize() * 8.0f);
        openList(document, middle, std::nullopt);
    }
    itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.bar.add_tip")));
    ImGui::EndDisabled();

    if (surface)
        return;
    // The layers: a page each, the bottom first as the file has them.
    AnimationDocument& animation = document.animation;
    const asset::AnimationGraph& graph = animation.graph();
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", core::tr(ENG_TR("engine.editor.nodes.bar.layers")));
    for (usize layer = 0; layer < graph.layers.size(); ++layer) {
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(layer));
        const bool shown = layer == animation.page();
        if (shown)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(graph.layers[layer].name.c_str()) && !shown) {
            animation.setPage(layer);
            document.framed = false;
            document.ranStates.clear();
        }
        if (shown)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(pane.readOnly);
    if (ImGui::Button("+##layer")) {
        if (animation.addLayer())
            document.framed = false;
    }
    itemTip("%s", core::tr(ENG_TR("engine.editor.nodes.bar.add_layer_tip")));
    ImGui::EndDisabled();
}

} // namespace

bool graphPaneHasKeyboard() noexcept
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetFrameCount() - g_keyboardFrame <= 1;
}

CanvasMetrics graphCanvasMetrics(bool arrows) noexcept
{
    return metricsOf(arrows);
}

void drawGraphPane(GraphDocument& document, std::size_t index, const GraphLive& live, const GraphShaders& shaders,
                   GraphPanelCommands& out)
{
    Pane pane(document, index, live, shaders, out);
    pane.readOnly = document.kind == GraphKind::Animation && live.playing;
    gatherProblems(pane);
    drawBar(pane);

    const float side = std::min(ImGui::GetFontSize() * kSideWidth, ImGui::GetContentRegionAvail().x * 0.45f);
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize(std::max(room.x - side - ImGui::GetStyle().ItemSpacing.x, 64.0f), std::max(room.y, 64.0f));
    drawCanvas(pane, canvasSize);
    ImGui::SameLine();
    if (ImGui::BeginChild("##fields", ImVec2(0.0f, canvasSize.y))) {
        if (document.kind == GraphKind::Surface)
            drawSurfaceFields(pane);
        else
            drawAnimationFields(pane);
    }
    ImGui::EndChild();
}

void drawGraphEditor(GraphEditor& graphs, core::u32 dockNode, const GraphLive& live, const GraphShaders& shaders,
                     GraphPanelCommands& out)
{
    const std::optional<usize> focus = graphs.takeFocusRequest();
    for (usize index = 0; index < graphs.count(); ++index) {
        GraphDocument* const document = graphs.at(index);
        if (document == nullptr)
            continue;
        // `Title###id`, so an unsaved mark changes the label and not the
        // window; the leading gap is the room the shell paints an icon in.
        char name[320]{};
        (void)std::snprintf(name, sizeof(name), "%s%s%s###graph:%s", tabIconPad().c_str(), document->title.c_str(),
                            document->dirty() ? " *" : "", document->relative.c_str());
        // Beside the Viewport as it appears, unless somebody has put it elsewhere.
        if (dockNode != 0) {
            const ImGuiID id = ImHashStr(name);
            const ImGuiWindow* const shown = ImGui::FindWindowByID(id);
            const ImGuiWindowSettings* const saved = ImGui::FindWindowSettingsByID(id);
            const bool appearing = shown == nullptr || !shown->WasActive;
            const bool docked = shown != nullptr ? shown->DockId != 0 : saved != nullptr && saved->DockId != 0;
            if (appearing && !docked)
                ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockNode), ImGuiCond_Always);
        }
        bool open = true;
        if (ImGui::Begin(name, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            if (ImGui::IsWindowAppearing() || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                graphs.setActive(index);
            drawGraphPane(*document, index, live, shaders, out);
        }
        ImGui::End();
        if (focus.has_value() && *focus == index)
            ImGui::SetWindowFocus(name);
        // Closed with changes in it: asked about, and nothing is lost by a
        // press on a cross.
        if (!open && document->dirty())
            g_closing = document->relative;
        else if (!open)
            out.close = index;
    }

    if (g_closing.empty())
        return;
    std::optional<usize> asked;
    for (usize index = 0; index < graphs.count(); ++index) {
        if (graphs.at(index)->relative == g_closing)
            asked = index;
    }
    if (!asked.has_value()) {
        g_closing.clear();
        return;
    }
    const std::string title = std::string(core::tr(ENG_TR("engine.editor.dialog.close_graph"))) + "###close-graph";
    if (!ImGui::IsPopupOpen("###close-graph"))
        ImGui::OpenPopup("###close-graph");
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_keyboardFrame = ImGui::GetFrameCount();
        ImGui::TextUnformatted(core::tr(ENG_TR("engine.editor.dialog.close_graph_unsaved"),
                                        {{"name", std::string_view(graphs.at(*asked)->title)}})
                                   .c_str());
        ImGui::Spacing();
        bool done = false;
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.dialog.close_graph_save")))) {
            out.save = *asked;
            out.close = *asked;
            done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.dialog.close_graph_discard")))) {
            out.close = *asked;
            done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(core::tr(ENG_TR("engine.editor.editor_dialogs.cancel"))) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            done = true;
        if (done) {
            g_closing.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace engine::app

#else

namespace engine::app {

// ADR 0011: a shipping build has no ImGui, so the panel has no body.
void drawGraphEditor(GraphEditor&, core::u32, const GraphLive&, const GraphShaders&, GraphPanelCommands&)
{}
void drawGraphPane(GraphDocument&, std::size_t, const GraphLive&, const GraphShaders&, GraphPanelCommands&)
{}
bool graphPaneHasKeyboard() noexcept
{
    return false;
}
CanvasMetrics graphCanvasMetrics(bool) noexcept
{
    return {};
}

} // namespace engine::app

#endif
