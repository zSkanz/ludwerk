// Native Windows presentation boundary. Only platform and native RHI glue use
// this header; no Windows/WinRT/SDL types enter the portable window API.
#pragma once
#include "engine/platform/window.h"
namespace engine::platform {
enum class WindowsSurfaceKind : core::u8
{
    None,
    Hwnd,
    CoreWindow
};
struct WindowsSurface
{
    WindowsSurfaceKind kind = WindowsSurfaceKind::None;
    void* object = nullptr;
};
[[nodiscard]] WindowsSurface windowsSurface(const Window& window) noexcept;
#if ENG_PLATFORM_UWP
// Called once by the CoreApplication host on its view thread.
void bindCoreWindow(void* coreWindow);
#endif
} // namespace engine::platform
