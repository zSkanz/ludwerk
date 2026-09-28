// **D204**: a game's focused `TextInput` asks the platform for characters.
#include <doctest/doctest.h>
#include <optional>

#include "engine/app/text_input_focus.h"

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
