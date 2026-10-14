// The Timeline panel with no window (ADR 0204): the same drawing the editor's
// dock does, run against an ImGui that draws to nowhere, with a mouse this
// file moves. What it can say: the panel opens on the selected player, lays
// its tracks out without tripping ImGui's own checks, scrubs the scene from
// the ruler and moves a key that is dragged. What it cannot: what any of it
// looks like -- no pixel is made here.

#include <cmath>
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/app/debug_overlay.h"
#include "engine/app/editor.h"
#include "engine/app/inspector.h"
#include "engine/app/sequences.h"
#include "engine/app/timeline.h"
#include "engine/audio/scene_types.h"
#include "engine/core/i18n.h"
#include "engine/render/scene_types.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

constexpr const char* File = "asset://sequences/lamp.sequence.json";
constexpr const char* Lamp = R"({
  "format": "sequence", "version": 1, "bindings": ["Lamp"], "length": 4,
  "tracks": [
    { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
        { "time": 1, "value": 0 }, { "time": 3, "value": 4 } ] },
    { "type": "event", "keys": [ { "time": 2, "name": "Half" } ] },
    { "type": "camera", "keys": [ { "time": 0, "position": [0, 2, 8], "lookAt": [0, 1, 0], "cut": true } ] }
  ]
})";

struct Desk
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    std::map<std::string, std::string, std::less<>> files;
    app::Sequences sequences;
    app::Timeline timeline;
    app::Editor editor;
    app::Inspector inspector;
    core::InstanceId player;
    core::InstanceId light;
    // The mouse, as this file has it.
    float mouseX = -100.0f;
    float mouseY = -100.0f;
    bool down = false;

    Desk()
    {
        scene::generated::registerClasses(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        files[File] = Lamp;
        sequences.setSources(app::Sequences::Sources{
            .read = [this](std::string_view urn) -> std::optional<std::string> {
                const auto found = files.find(urn);
                return found == files.end() ? std::nullopt : std::optional<std::string>{found->second};
            },
            .lineSeconds = {},
        });
        const core::InstanceId lamp = make("Part", {}, "Lamp");
        light = make("PointLight", lamp, "Light");
        (void)world->setProperty(light, atoms.intern("Brightness"), scene::Value{2.0});
        player = make("SequencePlayer", {}, "Cutscene");
        world->sequencePlayers().find(player)->sequence = atoms.intern(File);
        const core::InstanceId binding = make("SequenceBinding", player, "Lamp");
        world->sequenceBindings().find(binding)->target = lamp;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400.0f, 600.0f);
        // The atlas, built with nobody to give it to.
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~Desk() { ImGui::DestroyContext(); }

    core::InstanceId make(const char* className, core::InstanceId parent, const char* name)
    {
        const core::InstanceId made = world->create(classes.findId(atoms.intern(className)));
        world->setName(made, atoms.intern(name));
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // One frame of the panel, in a window that fills the display.
    void frame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouseX, mouseY);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        if (ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
            app::drawTimelinePanel(timeline, sequences, &editor, *world, &inspector);
        ImGui::End();
        ImGui::EndFrame();
    }

    // A press at a point and a release at another, a few frames each.
    void drag(float fromX, float fromY, float toX, float toY)
    {
        mouseX = fromX;
        mouseY = fromY;
        down = false;
        frame();
        frame();
        down = true;
        frame();
        frame();
        for (int step = 1; step <= 4; ++step) {
            mouseX = fromX + (toX - fromX) * static_cast<float>(step) / 4.0f;
            mouseY = fromY + (toY - fromY) * static_cast<float>(step) / 4.0f;
            frame();
        }
        frame();
        down = false;
        frame();
        frame();
    }

    [[nodiscard]] double brightness() const
    {
        return std::get<double>(*world->getProperty(light, atoms.lookup("Brightness")));
    }

    // A press and a release at one point.
    void click(float x, float y) { drag(x, y, x, y); }

    // **Where the panel's button `label` is**, found by passing the mouse
    // over the panel until ImGui says that is what it is over: nothing is
    // pressed on the way.
    [[nodiscard]] ImVec2 button(const char* label)
    {
        down = false;
        frame();
        ImGuiWindow* panel = ImGui::FindWindowByName("Timeline");
        REQUIRE(panel != nullptr);
        const ImGuiID wanted = panel->GetID(label);
        for (float y = 4.0f; y < 200.0f; y += 4.0f) {
            for (float x = 4.0f; x < ImGui::GetIO().DisplaySize.x; x += 8.0f) {
                mouseX = x;
                mouseY = y;
                frame();
                if (ImGui::GetCurrentContext()->HoveredId == wanted)
                    return ImVec2(x, y);
            }
        }
        FAIL("no button ", label, " in the panel");
        return ImVec2();
    }

    // How many popups are open, and the window of the one on top.
    [[nodiscard]] static int popups() { return ImGui::GetCurrentContext()->OpenPopupStack.Size; }
    [[nodiscard]] static ImGuiWindow* topPopup()
    {
        const ImGuiContext& context = *ImGui::GetCurrentContext();
        return context.OpenPopupStack.Size == 0 ? nullptr : context.OpenPopupStack.back().Window;
    }

    // The middle of a popup's `line`th line of fields, one field to a line.
    [[nodiscard]] static ImVec2 field(int line)
    {
        const ImGuiWindow* window = topPopup();
        REQUIRE(window != nullptr);
        const float frame = ImGui::GetFrameHeight();
        return ImVec2(window->DC.CursorStartPos.x + 12.0f,
                      window->DC.CursorStartPos.y +
                          static_cast<float>(line) * (frame + ImGui::GetStyle().ItemSpacing.y) + frame * 0.5f);
    }

    // The `index`th entry of the list a combo opened, scrolled to first.
    void choose(int index)
    {
        ImGuiWindow* list = topPopup();
        REQUIRE(list != nullptr);
        const float line = ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y;
        for (int turn = 0; turn < 40; ++turn) {
            list = topPopup();
            const float y = list->DC.CursorStartPos.y + static_cast<float>(index) * line + ImGui::GetFontSize() * 0.5f;
            if (y > list->Pos.y + 2.0f && y < list->Pos.y + list->Size.y - 2.0f) {
                click(list->DC.CursorStartPos.x + 12.0f, y);
                return;
            }
            mouseX = list->Pos.x + list->Size.x * 0.5f;
            mouseY = list->Pos.y + list->Size.y * 0.5f;
            ImGui::GetIO().AddMouseWheelEvent(0.0f, y > list->Pos.y ? -1.0f : 1.0f);
            frame();
            frame();
        }
        FAIL("the list's entry ", index, " was never in view");
    }

    // Typed into the field `line` of the popup on top.
    void type(int line, const char* text)
    {
        const ImVec2 at = field(line);
        click(at.x, at.y);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        frame();
    }
};

} // namespace

TEST_CASE(
    "ADR 0204: the Timeline panel, driven with no window: it opens, scrubs from its ruler and moves a dragged key")
{
    Desk desk;
    // Nothing selected: a line of words, and nothing open.
    desk.frame();
    desk.frame();
    CHECK_FALSE(desk.timeline.isOpen());

    // The player selected: its sequence is what the panel is of.
    desk.inspector.select(desk.player);
    desk.frame();
    desk.frame();
    REQUIRE(desk.timeline.isOpen());
    CHECK(desk.timeline.player() == desk.player);
    CHECK(desk.timeline.sequence().tracks.size() == 3);
    // Shown at nought, before the lamp's first key: the lamp is the scene's.
    CHECK(desk.brightness() == doctest::Approx(2.0));

    // **The ruler is found by pressing down the panel** at one place across
    // until the playhead moves: where it is is the layout's to say, and this
    // asks rather than assumes.
    // To the right, where the bar above the tracks has run out of buttons.
    const float across = 1000.0f;
    float ruler = -1.0f;
    for (float y = 20.0f; y < 300.0f && ruler < 0.0f; y += 4.0f) {
        desk.drag(across, y, across, y);
        if (desk.timeline.time() > 0.0)
            ruler = y;
    }
    REQUIRE(ruler > 0.0f);
    const double first = desk.timeline.time();
    // Pressed further along, the playhead is later: and from the two, how
    // many pixels a second is and where nought is.
    desk.drag(across + 200.0f, ruler, across + 200.0f, ruler);
    const double second = desk.timeline.time();
    REQUIRE(second > first);
    const float perSecond = 200.0f / static_cast<float>(second - first);
    const float nought = across - static_cast<float>(first) * perSecond;

    // **Scrubbed to two seconds, the scene is where the sequence is**: half
    // way between the lamp's keys.
    desk.drag(nought + 2.0f * perSecond, ruler, nought + 2.0f * perSecond, ruler);
    CHECK(desk.timeline.time() == doctest::Approx(2.0).epsilon(0.02));
    CHECK(desk.brightness() == doctest::Approx(2.0).epsilon(0.05));
    desk.drag(nought + 3.0f * perSecond, ruler, nought + 3.0f * perSecond, ruler);
    CHECK(desk.brightness() == doctest::Approx(4.0).epsilon(0.02));
    // Dragged, the playhead follows the mouse.
    desk.drag(nought + 3.0f * perSecond, ruler, nought + 1.5f * perSecond, ruler);
    CHECK(desk.timeline.time() == doctest::Approx(1.5).epsilon(0.03));
    CHECK(desk.brightness() == doctest::Approx(1.0).epsilon(0.06));

    // **A key dragged along its lane moves in time.** The lamp's first key
    // is at a second, in the first lane under the ruler -- found the same
    // way, by trying each row down until a key moves.
    const float keyX = nought + 1.0f * perSecond;
    bool moved = false;
    for (float y = ruler + 8.0f; y < ruler + 120.0f && !moved; y += 4.0f) {
        desk.drag(keyX, y, keyX + 0.5f * perSecond, y);
        moved = desk.timeline.sequence().tracks[0].keys[0].time != 1.0f;
    }
    REQUIRE(moved);
    CHECK(static_cast<double>(desk.timeline.sequence().tracks[0].keys[0].time) == doctest::Approx(1.5).epsilon(0.03));
    CHECK(desk.timeline.dirty());
    // And the scene follows the key: three quarters of the way from where
    // the key now is to the next, the lamp is three quarters of the way up
    // -- and not, as it was before the key moved, all the way.
    desk.drag(nought + 2.625f * perSecond, ruler, nought + 2.625f * perSecond, ruler);
    CHECK(desk.brightness() == doctest::Approx(3.0).epsilon(0.05));

    // The game running, the panel is a picture of what plays and edits nothing.
    desk.editor.play(*desk.world);
    desk.frame();
    desk.frame();
    CHECK(desk.timeline.isOpen());
    CHECK(desk.timeline.sequence().tracks[0].keys.size() == 2);
}

TEST_CASE("ADR 0204: the Timeline's Add popup, pressed: a cast track, and a track on whoever it brings")
{
    // The popup is the editor's own window, and no test pressed it: five
    // kinds of track could not be added from it for a batch, because its
    // list stopped at the seventh.
    Desk desk;
    desk.inspector.select(desk.player);
    desk.frame();
    desk.frame();
    REQUIRE(desk.timeline.isOpen());
    const std::vector<asset::SequenceTrack>& tracks = desk.timeline.sequence().tracks;
    const std::size_t before = tracks.size();

    const ImVec2 add = desk.button(core::tr(ENG_TR("engine.editor.timeline.add_track")));
    desk.click(add.x, add.y);
    REQUIRE(Desk::popups() == 1);

    // The kind: the last of thirteen, somebody brought for the scene.
    ImVec2 at = Desk::field(0);
    desk.click(at.x, at.y);
    REQUIRE(Desk::popups() == 2);
    desk.choose(12);
    REQUIRE(Desk::popups() == 1);
    desk.type(1, "Guard");
    desk.type(2, "asset://stamps/guard.stamp.json");
    at = Desk::field(3);
    desk.click(at.x, at.y);
    CHECK(Desk::popups() == 0);
    REQUIRE(tracks.size() == before + 1);
    CHECK(tracks.back().type == asset::SequenceTrackType::Cast);
    CHECK(tracks.back().target == "Guard");
    CHECK(tracks.back().model == "asset://stamps/guard.stamp.json");

    // **A track on whoever it brings**: their name is in the list of
    // bindings, after the player's own, as a binding's is.
    desk.click(add.x, add.y);
    REQUIRE(Desk::popups() == 1);
    at = Desk::field(0);
    desk.click(at.x, at.y);
    desk.choose(1);
    at = Desk::field(1);
    desk.click(at.x, at.y);
    REQUIRE(Desk::popups() == 2);
    desk.choose(0);
    at = Desk::field(1);
    desk.click(at.x, at.y);
    REQUIRE(Desk::popups() == 2);
    desk.choose(1);
    at = Desk::field(3);
    desk.click(at.x, at.y);
    REQUIRE(tracks.size() == before + 2);
    CHECK(tracks.back().type == asset::SequenceTrackType::Animation);
    CHECK(tracks.back().target == "Guard");
    CHECK(desk.timeline.dirty());
}
