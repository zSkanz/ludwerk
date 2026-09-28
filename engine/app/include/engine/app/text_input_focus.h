// **Typing reaches a game's `TextInput`** (D204).
//
// SDL delivers no characters until something asks for them, and on a phone the
// asking is what raises the on-screen keyboard. The editor's ImGui asks for its
// own fields; nothing asked for a game's, so in an exported game a focused
// `TextInput` showed its caret and never received a letter.
#pragma once

#include <optional>

namespace engine::app {

class TextInputFocus
{
public:
    // What to set the platform's text input to this frame, or nothing when it
    // stays as it is. **Only on a change of the game's focus**: ImGui switches
    // the same thing for its own fields, and a switch forced every frame would
    // turn their typing off. `editorTyping` -- an editor field has the keyboard
    // -- keeps it on when the game's box lets go.
    [[nodiscard]] std::optional<bool> follow(bool gameFocused, bool editorTyping) noexcept
    {
        if (gameFocused == m_focused)
            return std::nullopt;
        m_focused = gameFocused;
        if (!gameFocused && editorTyping)
            return std::nullopt;
        return gameFocused;
    }

private:
    bool m_focused = false;
};

} // namespace engine::app
