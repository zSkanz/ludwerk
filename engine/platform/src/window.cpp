#include "engine/platform/window.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <algorithm>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/platform/platform.h"
#include "engine/platform/sdl_interop.h"

#if defined(_WIN32)
// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format on
#endif

#include <span>
#include <string>
#include <string_view>

#include "engine/platform/windows_interop.h"
#include "window_impl.h"

namespace engine::platform {

WindowsSurface windowsSurface(const Window& window) noexcept
{
#if defined(_WIN32)
    return {WindowsSurfaceKind::Hwnd, SDL_GetPointerProperty(SDL_GetWindowProperties(nativeWindow(window)),
                                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)};
#else
    (void)window;
    return {};
#endif
}

namespace {

// **Windows 11 rounds every window's corners, and this engine draws into them.**
//
// The rounding is the shell's frame, not ours: the editor's dockspace, the
// launcher's panel and a game's framebuffer all reach the edge of the client
// area, so a rounded corner clips content the APPLICATION drew rather than
// softening chrome the SYSTEM drew. The shell's own theme has had a rounding of
// zero from the day it was data (ADR 0056); this is the half of that decision
// the window manager owns.
//
// Asked for through `dwmapi.dll` at runtime rather than by linking it: the
// attribute arrived in Windows 11 and does not exist on 10, so a link-time
// dependency would be a hard requirement bought for a preference, and the
// version check is the call failing. Every failure here is silent and
// survivable -- a rounded corner is a cosmetic loss and refusing to open a
// window is not.
void squareTheCorners([[maybe_unused]] SDL_Window* handle)
{
#if defined(_WIN32)
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(handle), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd == nullptr)
        return;

    // DWMWA_WINDOW_CORNER_PREFERENCE and DWMWCP_DONOTROUND, by value: the
    // enumerators are only declared by a recent Windows SDK, and this file is
    // compiled against whichever one the machine has.
    constexpr int kCornerPreference = 33;
    constexpr int kDoNotRound = 1;

    using SetAttribute = long(__stdcall*)(void*, unsigned long, const void*, unsigned long);
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr)
        return;
    if (const auto set =
            reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmSetWindowAttribute")));
        set != nullptr) {
        const int preference = kDoNotRound;
        (void)set(hwnd, kCornerPreference, &preference, sizeof(preference));
    }
    // Left loaded: the window outlives this call and DWM is in every process
    // that has one anyway, so unloading would be returning a reference the
    // system had already given us.
#endif
}

} // namespace

void WindowDeleter::operator()(Window* window) const noexcept
{
    delete window;
}

WindowPtr createWindow(const WindowDesc& desc, core::EngineError* outError)
{
    if (!isInitialized()) {
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("platform.err.window_failed"), {}, "platform::init() has not run");
        return {};
    }

    SDL_WindowFlags flags = 0;
    if (desc.resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (!desc.visible)
        flags |= SDL_WINDOW_HIDDEN;

    const std::string title =
        desc.title.empty() ? core::engineCatalog().format(desc.titleKey, desc.titleArgs) : std::string(desc.title);

    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, desc.width);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, desc.height);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
#if defined(__APPLE__)
    // **A headless window on macOS claims no graphics API of its own** (D166).
    // SDL gives every macOS window that names no backend `SDL_WINDOW_OPENGL`
    // by default, and the offscreen driver implements OpenGL through EGL, which
    // a Mac does not have -- so the window failed to exist at all. Nothing
    // draws into a headless window through OpenGL, so it is told a graphics
    // context comes from elsewhere, which is exactly true: the GPU device's.
    if (const char* driver = SDL_GetCurrentVideoDriver(); driver != nullptr && std::string_view(driver) == "offscreen")
        SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
#endif
#if defined(SDL_PLATFORM_ANDROID)
    // **The window is made the way up the package was started** (D446). The
    // manifest holds the activity at the start scene's orientation, and making
    // a window undid it: SDL hands the activity a requested orientation at
    // every window's creation, and for a resizable window with no hint that is
    // "any way the user holds it". So a landscape game started on a phone held
    // upright turned upright -- 1080 by 2340 for its first frames -- until the
    // first frame applied `UIService.ScreenOrientation` and turned it back,
    // and whatever a script measured at start was measured sideways.
    //
    // The display is the shape the manifest gave the activity, so the hint is
    // that shape until the world says which of the five it wants.
    if (const char* hint = SDL_GetHint(SDL_HINT_ORIENTATIONS); hint == nullptr || hint[0] == '\0') {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
            mode != nullptr && mode->w > 0 && mode->h > 0)
            SDL_SetHint(SDL_HINT_ORIENTATIONS, mode->w >= mode->h ? "LandscapeLeft LandscapeRight" : "Portrait");
    }
#endif
    SDL_Window* handle = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (handle == nullptr) {
        if (outError != nullptr)
            *outError = core::makeError(ENG_TR("platform.err.window_failed"), {}, SDL_GetError());
        return {};
    }

    squareTheCorners(handle);

    return WindowPtr(new Window(handle));
}

bool setPointerLocked(Window& window, bool locked)
{
    return SDL_SetWindowRelativeMouseMode(window.handle(), locked);
}

void setPointerPosition(Window& window, f32 x, f32 y)
{
    SDL_WarpMouseInWindow(window.handle(), x, y);
}

void setPointerVisible(bool visible)
{
    // Not per window: SDL's cursor is the process's. Failure is ignored because
    // there is nothing a game could do about it and nothing a player would see
    // beyond the cursor they already have.
    if (visible)
        (void)SDL_ShowCursor();
    else
        (void)SDL_HideCursor();
}

bool setWindowIcon(Window& window, std::span<const std::byte> rgba, i32 width, i32 height)
{
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
        return false;
    }

    // `SDL_CreateSurfaceFrom` does not copy, and `SDL_SetWindowIcon` does --
    // SDL duplicates the pixels into its own storage -- so the surface and the
    // caller's bytes may both go away as soon as this returns.
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<std::byte*>(rgba.data()), width * 4);
    if (surface == nullptr)
        return false;

    const bool ok = SDL_SetWindowIcon(window.handle(), surface);
    SDL_DestroySurface(surface);
    return ok;
}

u32 windowId(const Window& window) noexcept
{
    return SDL_GetWindowID(window.handle());
}

WindowPlacement windowPlacement(const Window& window) noexcept
{
    WindowPlacement placement;
    SDL_GetWindowPosition(window.handle(), &placement.x, &placement.y);
    SDL_GetWindowSize(window.handle(), &placement.width, &placement.height);
    placement.maximized = (SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_MAXIMIZED) != 0;
    return placement;
}

void setWindowPlacement(Window& window, const WindowPlacement& placement)
{
    if (placement.width > 0 && placement.height > 0) {
        (void)SDL_SetWindowSize(window.handle(), placement.width, placement.height);
        (void)SDL_SetWindowPosition(window.handle(), placement.x, placement.y);
    }
    if (placement.maximized)
        (void)SDL_MaximizeWindow(window.handle());
}

void setWindowFullscreen(Window& window, bool fullscreen)
{
    (void)SDL_SetWindowFullscreen(window.handle(), fullscreen);
}

std::string windowTitle(const Window& window)
{
    const char* title = SDL_GetWindowTitle(window.handle());
    return title != nullptr ? std::string(title) : std::string();
}

void setWindowTitle(Window& window, std::string_view title)
{
    (void)SDL_SetWindowTitle(window.handle(), std::string(title).c_str());
}

std::vector<DisplayInfo> displays()
{
    std::vector<DisplayInfo> found;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids == nullptr)
        return found;
    for (int index = 0; index < count; ++index) {
        DisplayInfo info;
        if (const char* name = SDL_GetDisplayName(ids[index]); name != nullptr)
            info.name = name;
        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[index]); mode != nullptr) {
            // In pixels: a doubled display's desktop mode is in points.
            info.width = static_cast<i32>(static_cast<f32>(mode->w) * mode->pixel_density);
            info.height = static_cast<i32>(static_cast<f32>(mode->h) * mode->pixel_density);
            info.refreshRate = mode->refresh_rate;
        }
        found.push_back(std::move(info));
    }
    SDL_free(ids);
    return found;
}

namespace {

// The system's id of display `index`, or zero.
[[nodiscard]] SDL_DisplayID displayAt(core::usize index)
{
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids == nullptr)
        return 0;
    const SDL_DisplayID id = index < static_cast<core::usize>(count) ? ids[index] : 0;
    SDL_free(ids);
    return id;
}

} // namespace

std::vector<DisplayMode> displayModes(core::usize index)
{
    std::vector<DisplayMode> found;
    const SDL_DisplayID display = displayAt(index);
    if (display == 0)
        return found;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (modes == nullptr)
        return found;
    // The library lists them largest first, and a size once per rate and
    // format: the first of each size is its best.
    for (int at = 0; at < count; ++at) {
        const SDL_DisplayMode& mode = *modes[at];
        DisplayMode entry;
        entry.width = static_cast<i32>(static_cast<f32>(mode.w) * mode.pixel_density);
        entry.height = static_cast<i32>(static_cast<f32>(mode.h) * mode.pixel_density);
        entry.refreshRate = mode.refresh_rate;
        bool listed = false;
        for (DisplayMode& earlier : found) {
            if (earlier.width == entry.width && earlier.height == entry.height) {
                earlier.refreshRate = std::max(earlier.refreshRate, entry.refreshRate);
                listed = true;
                break;
            }
        }
        if (!listed)
            found.push_back(entry);
    }
    SDL_free(modes);
    return found;
}

core::usize windowDisplay(const Window& window)
{
    const SDL_DisplayID current = SDL_GetDisplayForWindow(window.handle());
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    core::usize index = 0;
    if (ids != nullptr) {
        for (int at = 0; at < count; ++at) {
            if (ids[at] == current)
                index = static_cast<core::usize>(at);
        }
        SDL_free(ids);
    }
    return index;
}

void setWindowMode(Window& window, WindowMode mode, core::usize display, i32 width, i32 height)
{
    SDL_Window* handle = window.handle();
    SDL_DisplayID target = displayAt(display);
    if (target == 0)
        target = SDL_GetDisplayForWindow(handle);

    if (mode == WindowMode::Windowed) {
        (void)SDL_SetWindowFullscreen(handle, false);
        if (width > 0 && height > 0)
            (void)SDL_SetWindowSize(handle, width, height);
        // On the display that was asked for, when it is another.
        if (target != 0 && target != SDL_GetDisplayForWindow(handle)) {
            (void)SDL_SetWindowPosition(handle, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(target)),
                                        static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(target)));
        }
        return;
    }

    // A window goes fullscreen on the display it is on: moved first.
    if (target != 0 && target != SDL_GetDisplayForWindow(handle)) {
        (void)SDL_SetWindowFullscreen(handle, false);
        (void)SDL_SetWindowPosition(handle, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(target)),
                                    static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(target)));
    }

    SDL_DisplayMode closest{};
    const bool exclusive = mode == WindowMode::Fullscreen && width > 0 && height > 0 && target != 0 &&
                           SDL_GetClosestFullscreenDisplayMode(target, width, height, 0.0f, true, &closest);
    // No mode of its own is the desktop's: the borderless kind, and what
    // fullscreen falls back to when the display has nothing near what was
    // asked.
    (void)SDL_SetWindowFullscreenMode(handle, exclusive ? &closest : nullptr);
    (void)SDL_SetWindowFullscreen(handle, true);
}

WindowPlacement usableDisplayArea() noexcept
{
    WindowPlacement area;
    SDL_Rect bounds{};
    if (const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        display != 0 && SDL_GetDisplayUsableBounds(display, &bounds)) {
        area.x = bounds.x;
        area.y = bounds.y;
        area.width = bounds.w;
        area.height = bounds.h;
    }
    return area;
}

WindowSize windowPixelSize(const Window& window) noexcept
{
    WindowSize size;
    // The out-params are left untouched on failure, which only happens for an
    // invalid window; a zero size is then the honest answer rather than stale
    // numbers from a previous call.
    SDL_GetWindowSizeInPixels(window.handle(), &size.width, &size.height);
    return size;
}

f32 windowDisplayScale(const Window& window) noexcept
{
    // Zero is SDL's failure answer, and a scale of zero would multiply every
    // measurement a game makes to nothing. One is the honest fallback: it says
    // "logical pixels are device pixels", which is what an unscaled display is.
    const float scale = SDL_GetWindowDisplayScale(window.handle());
    return scale > 0.0f ? scale : 1.0f;
}

bool windowFocused(const Window& window) noexcept
{
    return (SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool windowMinimized(const Window& window) noexcept
{
    return (SDL_GetWindowFlags(window.handle()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
}

f32 windowRefreshRate(const Window& window) noexcept
{
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window.handle());
    if (display == 0)
        return 0.0f;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    return mode != nullptr && mode->refresh_rate > 0.0f ? mode->refresh_rate : 0.0f;
}

WindowInsets windowSafeAreaInsets(const Window& window) noexcept
{
    WindowInsets insets;

    // SDL reports the safe area as a RECTANGLE inside the window; what a UI
    // wants is how much each edge takes away, so it is converted here rather
    // than in four places downstream.
    SDL_Rect safe{};
    if (!SDL_GetWindowSafeArea(window.handle(), &safe))
        return insets;

    WindowSize size;
    SDL_GetWindowSize(window.handle(), &size.width, &size.height);
    if (size.width <= 0 || size.height <= 0)
        return insets;

    insets.left = safe.x;
    insets.top = safe.y;
    insets.right = size.width - (safe.x + safe.w);
    insets.bottom = size.height - (safe.y + safe.h);
    if (insets.right < 0)
        insets.right = 0;
    if (insets.bottom < 0)
        insets.bottom = 0;
    return insets;
}

SDL_Window* nativeWindow(const Window& window) noexcept
{
    return window.handle();
}

} // namespace engine::platform
