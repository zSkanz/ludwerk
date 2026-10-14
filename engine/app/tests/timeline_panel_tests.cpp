// The Timeline panel with no window (ADR 0204): the same drawing the editor's
// dock does, run against an ImGui that draws to nowhere, with a mouse this
// file moves. What it can say: the panel opens on the selected player, lays
// its tracks out without tripping ImGui's own checks, scrubs the scene from
// the ruler and moves a key that is dragged. What it cannot: what any of it
// looks like -- no pixel is made here.

#include <cmath>
#include <doctest/doctest.h>
#include <imgui.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "class_descriptors.gen.h"
#include "engine/app/debug_overlay.h"
#include "engine/app/editor.h"
#include "engine/app/inspector.h"
#include "engine/app/sequences.h"
#include "engine/app/timeline.h"
#include "engine/audio/scene_types.h"
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
