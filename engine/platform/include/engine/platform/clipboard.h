// The operating system's clipboard, as UTF-8 text (ADR 0139).
//
// What a `TextInput`'s Ctrl+C and Ctrl+V go through, so a game's field and every
// other application on the machine share one clipboard. The editor's own panels
// do not: they go through ImGui, which has its own path to the same clipboard.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace engine::platform {

// The clipboard's text, or nothing when it holds none -- an image, or an empty
// clipboard -- or the platform has no clipboard.
[[nodiscard]] std::optional<std::string> clipboardText();

// Puts `text` on the clipboard. False when the platform refused.
bool setClipboardText(std::string_view text);

// **How many times a read or a write is tried** (D361). Another program --
// a clipboard history, a launcher -- opens the clipboard for a moment after
// every change, and while it has it open nobody else can: SDL gives up after
// 30 ms and a read answers "" as though the clipboard were empty. So a read
// that finds text there but gets none, or a write that is refused, is tried
// again every 10 ms, up to a quarter of a second -- longer than such a program
// holds it, and short enough that a clipboard really holding "" costs a
// paste no more than that.
inline constexpr int ClipboardAttempts = 25;

// The reading rule over a clipboard, so it can be tested without one.
struct ClipboardSource
{
    std::function<bool()> hasText;
    std::function<std::optional<std::string>()> read;
    std::function<void()> wait;
};
[[nodiscard]] std::optional<std::string> clipboardTextFrom(const ClipboardSource& source);

} // namespace engine::platform
