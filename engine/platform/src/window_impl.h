// Internal to the platform module: this is where `Window` -- incomplete in the
// public header on purpose -- actually gains an SDL type.
#pragma once

#include <SDL3/SDL_video.h>

#include "engine/platform/window.h"

namespace engine::platform {

// The event pump's fiber left where it stands, when it is inside the system's
// loop for a window that is going (D535; see events.cpp). Nothing otherwise.
void abandonHeldPump() noexcept;

class Window
{
public:
    explicit Window(SDL_Window* handle) noexcept : handle_(handle) {}

    ~Window()
    {
        abandonHeldPump();
        if (handle_ != nullptr)
            SDL_DestroyWindow(handle_);
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] SDL_Window* handle() const noexcept { return handle_; }

private:
    SDL_Window* handle_ = nullptr;
};

} // namespace engine::platform
