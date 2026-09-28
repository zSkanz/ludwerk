// **D204**: a game's focused `TextInput` asks the platform for characters.
#include <doctest/doctest.h>
#include <optional>

#include "engine/app/text_input_focus.h"
#include "engine/app/world_host.h"
#include "engine/scene/change_queue.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;

TEST_CASE("a game's text box turns typing on when it takes focus and off when it lets go")
{
    app::TextInputFocus focus;
    CHECK_FALSE(focus.follow(false, false).has_value());
    CHECK(focus.follow(true, false) == std::optional<bool>{true});
    // Held: nothing to switch, frame after frame.
    CHECK_FALSE(focus.follow(true, false).has_value());
    CHECK_FALSE(focus.follow(true, false).has_value());
    CHECK(focus.follow(false, false) == std::optional<bool>{false});
}

TEST_CASE("letting go never switches off an editor field's typing")
{
    // ImGui turns text input on for its own fields and off when they close; a
    // game box that let go while the Explorer's rename field had the keyboard
    // must leave that field typing.
    app::TextInputFocus focus;
    CHECK(focus.follow(true, false) == std::optional<bool>{true});
    CHECK_FALSE(focus.follow(false, /*editorTyping=*/true).has_value());
    // And the next focus turns it on again.
    CHECK(focus.follow(true, false) == std::optional<bool>{true});
}

TEST_CASE("FocusLost tells a handler whether Return left the field")
{
    // **D215.** The event was raised with no arguments, so `submitted` was nil
    // both ways and "press Enter to join" could not be written.
    app::testing::Captured log;
    app::testing::Project project;
    project.write("src/client/field.luau", R"(
        local field = Instance.new("TextInput")
        field.Name = "Field"
        field.Parent = game:GetService("Workspace")
        field.FocusLost:Connect(function(submitted: boolean)
            print(`focus-lost:{submitted}`)
        end)
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(app::testing::bootOptions(project.root)).has_value());
    host.tick();
    scene::World& world = host.world();
    const core::InstanceId field = world.findFirstChild(host.workspace(), world.atoms().intern("Field"));
    REQUIRE(field.valid());

    world.changes().push(scene::Change{scene::ChangeKind::InstanceEventBool, field, scene::eventFlag(true),
                                       world.atoms().intern("FocusLost")});
    host.tick();
    CHECK(log.contains("focus-lost:true"));
    world.changes().push(scene::Change{scene::ChangeKind::InstanceEventBool, field, scene::eventFlag(false),
                                       world.atoms().intern("FocusLost")});
    host.tick();
    CHECK(log.contains("focus-lost:false"));
}
