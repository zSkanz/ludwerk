// The Graph panel with no window (ADR 0211): the drawing the editor's dock
// does, run against an ImGui that draws to nowhere, with a mouse and a
// keyboard this file moves. What it can say: the panel draws both kinds of
// graph without tripping ImGui's own checks; the list opens from a right
// press and from the space bar, is typed into and adds what Enter is pressed
// on; a link is drawn from a pin to a pin, one that cannot be is refused and
// kept to be read, one let go over nothing asks what fits it; the keys are the
// canvas's; an animation page moves a state, draws an arrow from a state's
// edge, and is looked at and not changed while the game runs. What it cannot:
// what any of it looks like -- no pixel is made here; `tests/editorshots` is
// where one is.

#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <optional>
#include <string>

#include "engine/app/graph_editor.h"
#include "engine/app/graph_panel.h"
#include "engine/core/i18n.h"

using namespace engine;
using app::CanvasLink;
using app::CanvasMetrics;
using app::CanvasNode;
using app::CanvasPoint;
using app::LinkRefusal;
using core::f32;
using core::u32;

namespace {

struct Desk
{
    app::GraphDocument document;
    app::GraphLive live;
    app::GraphShaders shaders;
    app::GraphPanelCommands out;
    CanvasMetrics metrics;
    float mouseX = -100.0f;
    float mouseY = -100.0f;
    bool left = false;
    bool right = false;

    Desk()
    {
        REQUIRE(core::engineCatalog().loadFromFile(ENG_TEST_CATALOG).ok);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400.0f, 800.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~Desk() { ImGui::DestroyContext(); }
    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    // One frame of the pane, in a window that fills the display.
    void frame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouseX, mouseY);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        if (ImGui::Begin("Graph", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
            app::drawGraphPane(document, 0, live, shaders, out);
            metrics = app::graphCanvasMetrics(document.kind == app::GraphKind::Animation);
        }
        ImGui::End();
        ImGui::EndFrame();
    }

    void frames(int count)
    {
        for (int index = 0; index < count; ++index)
            frame();
    }

    // A place on the canvas, as a place in the window.
    [[nodiscard]] ImVec2 screen(f32 x, f32 y) const
    {
        const CanvasPoint panel = document.canvas().toPanel(x, y);
        return ImVec2(document.canvasAt.x + panel.x, document.canvasAt.y + panel.y);
    }
    [[nodiscard]] ImVec2 screen(CanvasPoint at) const { return screen(at.x, at.y); }

    void move(ImVec2 to)
    {
        mouseX = to.x;
        mouseY = to.y;
        frames(2);
    }

    void click(ImVec2 at)
    {
        move(at);
        left = true;
        frames(2);
        left = false;
        frames(2);
    }

    void rightClick(ImVec2 at)
    {
        move(at);
        right = true;
        frames(2);
        right = false;
        frames(2);
    }

    void drag(ImVec2 from, ImVec2 to)
    {
        move(from);
        left = true;
        frames(2);
        for (int step = 1; step <= 5; ++step) {
            mouseX = from.x + (to.x - from.x) * static_cast<float>(step) / 5.0f;
            mouseY = from.y + (to.y - from.y) * static_cast<float>(step) / 5.0f;
            frame();
        }
        frame();
        left = false;
        frames(2);
    }

    // `ctrl` is the key a person holds for a shortcut on the machine the test
    // runs on: Ctrl, and Cmd on a Mac -- where the library hands the two to
    // the panel the other way round, so that `io.KeyCtrl` is that key on both
    // (pressing Ctrl itself there was a shortcut nobody had pressed).
    void key(ImGuiKey which, bool ctrl = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        const ImGuiKey held = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        if (ctrl)
            io.AddKeyEvent(held, true);
        io.AddKeyEvent(which, true);
        frame();
        io.AddKeyEvent(which, false);
        if (ctrl)
            io.AddKeyEvent(held, false);
        frame();
    }

    void type(const char* text)
    {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frames(2);
    }

    [[nodiscard]] const CanvasNode* nodeOfKind(std::string_view kind) const
    {
        for (const CanvasNode& node : document.canvas().nodes()) {
            if (node.kind == kind)
                return &node;
        }
        return nullptr;
    }

    [[nodiscard]] ImVec2 outputOf(u32 node, u32 pin) const
    {
        return screen(outputPlace(*document.canvas().node(node), pin, metrics));
    }
    [[nodiscard]] ImVec2 inputOf(u32 node, u32 pin) const
    {
        return screen(inputPlace(*document.canvas().node(node), pin, metrics));
    }
};

[[nodiscard]] u32 pinNamed(const std::vector<app::CanvasPin>& pins, std::string_view name)
{
    for (u32 index = 0; index < pins.size(); ++index) {
        if (pins[index].name == name)
            return index;
    }
    FAIL("no pin called " << name);
    return 0;
}

[[nodiscard]] std::string heroGraph()
{
    const std::filesystem::path file = std::filesystem::path(ENG_TEST_CATALOG).parent_path().parent_path() /
                                       "examples/37-character/content/anim/hero.animgraph.json";
    std::ifstream in(file, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("ADR 0211: a material graph's pane, driven with no window: the list, links made and refused, the keys")
{
    Desk desk;
    desk.document.kind = app::GraphKind::Surface;
    desk.document.title = "new.surfacegraph.json";
    desk.document.relative = "shaders/new.surfacegraph.json";
    const std::optional<asset::SurfaceGraph> graph = asset::readSurfaceGraph(app::GraphEditor::newSurfaceGraph());
    REQUIRE(graph.has_value());
    desk.document.surface.load(*graph, desk.document.title);
    desk.frames(4);
    app::SurfaceDocument& surface = desk.document.surface;
    const app::NodeCanvas& canvas = surface.canvas;
    REQUIRE(desk.document.canvasSize.x > 200.0f);
    REQUIRE(desk.document.canvasSize.y > 200.0f);
    CHECK(desk.document.framed);
    CHECK_FALSE(desk.document.dirty());
    CHECK(app::graphPaneHasKeyboard());

    // **A right press on the canvas, where nothing is, opens the list**; what
    // is typed narrows it, and Enter adds the one the keys are on, there.
    desk.rightClick(desk.screen(60.0f, 260.0f));
    REQUIRE(desk.document.ask.open);
    CHECK_FALSE(desk.document.ask.loose.has_value());
    desk.type("mul");
    CHECK(desk.document.ask.filter == "mul");
    desk.key(ImGuiKey_Enter);
    CHECK_FALSE(desk.document.ask.open);
    const CanvasNode* const multiply = desk.nodeOfKind("Multiply");
    REQUIRE(multiply != nullptr);
    CHECK(std::abs(multiply->x - 60.0f) < 2.0f);
    CHECK(std::abs(multiply->y - 260.0f) < 2.0f);
    CHECK(canvas.selected(multiply->id));
    CHECK(desk.document.dirty());
    const u32 multiplyId = multiply->id;
    desk.frames(2);

    // **And the space bar does**, with the pointer over the canvas; Escape
    // shuts it and adds nothing.
    desk.move(desk.screen(60.0f, 420.0f));
    desk.key(ImGuiKey_Space);
    CHECK(desk.document.ask.open);
    desk.frames(2);
    desk.key(ImGuiKey_Escape);
    CHECK_FALSE(desk.document.ask.open);
    CHECK(canvas.nodes().size() == 3);
    desk.frames(2);

    // **A link, from a pin to a pin.** The colour into the multiply.
    const core::usize links = canvas.links().size();
    desk.drag(desk.outputOf(1, 0), desk.inputOf(multiplyId, 0));
    REQUIRE(canvas.links().size() == links + 1);
    CHECK(canvas.linkInto(multiplyId, 0).has_value());
    // It came out three wide, and is refused by a number: kept, to be read.
    const u32 alpha = pinNamed(canvas.node(2)->inputs, "Alpha");
    desk.drag(desk.outputOf(multiplyId, 0), desk.inputOf(2, alpha));
    CHECK(canvas.links().size() == links + 1);
    CHECK(desk.document.refused == LinkRefusal::TypesDoNotFit);
    CHECK(desk.document.refusedFrames > 0);
    CHECK(desk.document.refusedLink == CanvasLink{multiplyId, 0, 2, alpha});

    // **Let go over nothing, it asks what fits it**: the list again, narrowed
    // to what takes a colour, and the one chosen is linked as it is added.
    desk.drag(desk.outputOf(1, 0), desk.screen(60.0f, 520.0f));
    REQUIRE(desk.document.ask.open);
    REQUIRE(desk.document.ask.loose.has_value());
    CHECK(desk.document.ask.loose->node == 1);
    CHECK(desk.document.ask.loose->fromOutput);
    desk.type("fres");
    desk.key(ImGuiKey_Enter);
    const CanvasNode* const fresnel = desk.nodeOfKind("Fresnel");
    REQUIRE(fresnel != nullptr);
    CHECK(canvas.linkInto(fresnel->id, 0) == std::optional<CanvasLink>{CanvasLink{1, 0, fresnel->id, 0}});
    const u32 fresnelId = fresnel->id;
    desk.frames(3);

    // **The keys are the canvas's**: Delete takes what is selected, Ctrl+Z
    // gives it back, Ctrl+S asks for the graph to be written.
    CHECK(canvas.selected(fresnelId));
    desk.key(ImGuiKey_Delete);
    CHECK(canvas.node(fresnelId) == nullptr);
    desk.key(ImGuiKey_Z, true);
    CHECK(canvas.node(fresnelId) != nullptr);
    CHECK_FALSE(desk.out.save.has_value());
    desk.key(ImGuiKey_S, true);
    CHECK(desk.out.save == std::optional<std::size_t>{0});

    // A press on a node selects it, and a drag moves it.
    const CanvasNode* const tint = canvas.node(1);
    const f32 was = tint->x;
    const app::CanvasRect rect = nodeRect(*tint, desk.metrics);
    desk.drag(desk.screen(rect.x + rect.width * 0.5f, rect.y + 6.0f),
              desk.screen(rect.x + rect.width * 0.5f - 80.0f, rect.y + 6.0f));
    CHECK(canvas.selected(1));
    CHECK(std::abs(canvas.node(1)->x - (was - 80.0f)) < 2.0f);
}

TEST_CASE("ADR 0211: an animation graph's pane, driven with no window: a state moved, an arrow from an edge, and "
          "looked at while the game runs")
{
    Desk desk;
    desk.document.kind = app::GraphKind::Animation;
    desk.document.title = "hero.animgraph.json";
    desk.document.relative = "anim/hero.animgraph.json";
    std::optional<asset::AnimationGraph> graph = asset::readAnimationGraph(heroGraph());
    REQUIRE(graph.has_value());
    desk.document.animation.load(std::move(*graph));
    desk.frames(4);
    app::AnimationDocument& animation = desk.document.animation;
    const app::NodeCanvas& canvas = animation.canvas;
    const auto body = [&]() -> const asset::GraphLayer& { return animation.graph().layers[0]; };
    CHECK_FALSE(desk.document.dirty());
    const auto middle = [&](u32 state) {
        const app::CanvasRect rect = nodeRect(*canvas.node(animation.nodeOf(state)), desk.metrics);
        return CanvasPoint{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
    };

    // **A drag from a state's middle moves it**, and its place is the graph's.
    const asset::GraphPlace before = body().states[2].place;
    const CanvasPoint fall = middle(2);
    desk.drag(desk.screen(fall), desk.screen(fall.x, fall.y + 90.0f));
    CHECK(std::abs(body().states[2].place.y - (before.y + 90.0f)) < 2.0f);
    CHECK(desk.document.dirty());
    CHECK(body().transitions.size() == 4);

    // **A drag from its edge draws an arrow**: Fall to Jump, which had none.
    REQUIRE(animation.transitionsBetween(2, 1).empty());
    const app::CanvasRect rim = nodeRect(*canvas.node(animation.nodeOf(2)), desk.metrics);
    desk.drag(desk.screen(rim.x + rim.width * 0.5f, rim.y), desk.screen(middle(1)));
    CHECK(animation.transitionsBetween(2, 1).size() == 1);
    REQUIRE(canvas.selectedLink().has_value());
    CHECK(canvas.selectedLink()->from == animation.nodeOf(2));
    // Delete, with the arrow selected, takes the arrow.
    desk.key(ImGuiKey_Delete);
    CHECK(animation.transitionsBetween(2, 1).empty());
    CHECK(body().states.size() == 3);

    // The list offers a state; the page has its box of "from any" already.
    desk.rightClick(desk.screen(-200.0f, 300.0f));
    REQUIRE(desk.document.ask.open);
    desk.key(ImGuiKey_Enter);
    REQUIRE(body().states.size() == 4);
    CHECK(body().states[3].name == "State");
    CHECK_FALSE(animation.reachable(3));
    desk.frames(3);

    // **While the game runs it is looked at**: nothing moves, nothing is
    // added, and the keys that change it do nothing.
    const asset::AnimationGraph running = animation.graph();
    desk.live.playing = true;
    desk.live.graph = desk.document.relative;
    desk.live.layers = {
        {"Body", "Move", 0.4f, 1.0f, {}}, {"Arms", "None", 0.0f, 1.0f, {}}, {"Lean", "Up", 1.0f, 0.0f, {}}};
    desk.live.parameters = {{"Grounded", 1.0f, false}, {"Rise", 0.0f, false}};
    desk.frames(3);
    CHECK(canvas.locked());
    const CanvasPoint move = middle(0);
    desk.drag(desk.screen(move), desk.screen(move.x + 120.0f, move.y));
    desk.rightClick(desk.screen(-200.0f, 420.0f));
    CHECK_FALSE(desk.document.ask.open);
    desk.key(ImGuiKey_Delete);
    desk.key(ImGuiKey_Z, true);
    CHECK(animation.graph() == running);
    CHECK(desk.document.takenFrames == 0);

    // **The arrow it has just taken is lit for a moment**: Move to Fall, and
    // then back by the one that is taken from any state.
    desk.live.layers[0] = {"Body", "Fall", 0.1f, 1.0f, {{"Move", 0.6f}}};
    desk.frame();
    CHECK(desk.document.takenFrames > 0);
    CHECK(desk.document.taken == CanvasLink{animation.nodeOf(0), 0, animation.nodeOf(2), 0});
    desk.live.layers[0] = {"Body", "Move", 0.0f, 1.0f, {{"Fall", 0.8f}}};
    desk.frame();
    CHECK(desk.document.taken == CanvasLink{animation.anyNode(), 0, animation.nodeOf(0), 0});

    // Stopped: it can be changed again.
    desk.live = app::GraphLive{};
    desk.frames(2);
    CHECK_FALSE(canvas.locked());
}
