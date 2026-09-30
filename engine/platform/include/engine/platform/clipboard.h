// The operating system's clipboard, as UTF-8 text (ADR 0139).
//
// What a `TextInput`'s Ctrl+C and Ctrl+V go through, so a game's field and every
// other application on the machine share one clipboard. The editor's own panels
// do not: they go through ImGui, which has its own path to the same clipboard.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace engine::platform {

// The clipboard's text, or nothing when it holds none -- an image, or an empty
// clipboard -- or the platform has no clipboard.
[[nodiscard]] std::optional<std::string> clipboardText();

// Puts `text` on the clipboard. False when the platform refused.
bool setClipboardText(std::string_view text);

} // namespace engine::platform
