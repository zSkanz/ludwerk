#include "engine/platform/clipboard.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

namespace engine::platform {

std::optional<std::string> clipboardText()
{
    if (!SDL_HasClipboardText())
        return std::nullopt;
    char* text = SDL_GetClipboardText();
    if (text == nullptr)
        return std::nullopt;
    std::string out(text);
    SDL_free(text);
    return out;
}

bool setClipboardText(std::string_view text)
{
    // SDL takes a NUL-terminated string; a view may not be one.
    const std::string owned(text);
    return SDL_SetClipboardText(owned.c_str());
}

} // namespace engine::platform
