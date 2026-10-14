#include "engine/platform/clipboard.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

namespace engine::platform {

std::optional<std::string> clipboardTextFrom(const ClipboardSource& source)
{
    for (int attempt = 1;; ++attempt) {
        if (!source.hasText())
            return std::nullopt;
        std::optional<std::string> text = source.read();
        if (!text.has_value() || !text->empty() || attempt >= ClipboardAttempts)
            return text;
        source.wait();
    }
}

std::optional<std::string> clipboardText()
{
    ClipboardSource source;
    source.hasText = [] { return SDL_HasClipboardText(); };
    source.read = []() -> std::optional<std::string> {
        char* text = SDL_GetClipboardText();
        if (text == nullptr)
            return std::nullopt;
        std::string out(text);
        SDL_free(text);
        return out;
    };
    source.wait = [] { SDL_Delay(10); };
    return clipboardTextFrom(source);
}

bool setClipboardText(std::string_view text)
{
    // SDL takes a NUL-terminated string; a view may not be one.
    const std::string owned(text);
    for (int attempt = 1;; ++attempt) {
        if (SDL_SetClipboardText(owned.c_str()))
            return true;
        if (attempt >= ClipboardAttempts)
            return false;
        SDL_Delay(10);
    }
}

} // namespace engine::platform
