#include "engine/platform/platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "engine/core/text_key.h"
#include "engine/platform/event.h"
#include "engine/platform/file.h"
#include "engine/platform/windows_interop.h"
// Generated Windows SDK delegates use COM lifetime rather than virtual destructors.
#pragma warning(push)
#pragma warning(disable : 4265)
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#pragma warning(pop)

namespace engine::platform {
namespace W = winrt::Windows;
using W::UI::Core::CoreWindow;
class Window
{
public:
    CoreWindow native{nullptr};
    std::string title;
    bool focused = true, visible = true, textInput = false;
};
namespace {
CoreWindow coreWindow{nullptr};
Window* primary = nullptr;
bool initialized = false, wanted = true, focused = true;
Paths directories;
std::vector<Event> pending, events;
W::ApplicationModel::SuspendingDeferral suspendDeferral{nullptr};
struct Pad
{
    W::Gaming::Input::Gamepad native{nullptr};
    u32 id = 0;
    W::Gaming::Input::GamepadReading previous{};
};
std::vector<Pad> pads;
u32 nextPad = 1;
std::array<f32, 4> motors{};
VibrationSink* vibration = nullptr;
constexpr std::array<W::Gaming::Input::GamepadButtons, 14> buttonMasks{
    W::Gaming::Input::GamepadButtons::A,
    W::Gaming::Input::GamepadButtons::B,
    W::Gaming::Input::GamepadButtons::X,
    W::Gaming::Input::GamepadButtons::Y,
    W::Gaming::Input::GamepadButtons::View,
    W::Gaming::Input::GamepadButtons::Menu,
    W::Gaming::Input::GamepadButtons::LeftThumbstick,
    W::Gaming::Input::GamepadButtons::RightThumbstick,
    W::Gaming::Input::GamepadButtons::LeftShoulder,
    W::Gaming::Input::GamepadButtons::RightShoulder,
    W::Gaming::Input::GamepadButtons::DPadUp,
    W::Gaming::Input::GamepadButtons::DPadDown,
    W::Gaming::Input::GamepadButtons::DPadLeft,
    W::Gaming::Input::GamepadButtons::DPadRight};
constexpr std::array<GamepadButton, 14> buttons{
    GamepadButton::South,        GamepadButton::East,          GamepadButton::West,      GamepadButton::North,
    GamepadButton::Back,         GamepadButton::Start,         GamepadButton::LeftStick, GamepadButton::RightStick,
    GamepadButton::LeftShoulder, GamepadButton::RightShoulder, GamepadButton::DpadUp,    GamepadButton::DpadDown,
    GamepadButton::DpadLeft,     GamepadButton::DpadRight};
void reading(Pad& pad, const W::Gaming::Input::GamepadReading& value)
{
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        const auto mask = static_cast<u32>(buttonMasks[index]);
        const bool before = (static_cast<u32>(pad.previous.Buttons) & mask) != 0;
        const bool after = (static_cast<u32>(value.Buttons) & mask) != 0;
        if (before != after)
            events.push_back({.type = after ? EventType::GamepadButtonDown : EventType::GamepadButtonUp,
                              .gamepadId = pad.id,
                              .gamepadFamily = GamepadType::Xbox,
                              .gamepadButton = buttons[index]});
    }
    const std::array<double, 6> before{pad.previous.LeftThumbstickX,  -pad.previous.LeftThumbstickY,
                                       pad.previous.RightThumbstickX, -pad.previous.RightThumbstickY,
                                       pad.previous.LeftTrigger,      pad.previous.RightTrigger};
    const std::array<double, 6> after{value.LeftThumbstickX,   -value.LeftThumbstickY, value.RightThumbstickX,
                                      -value.RightThumbstickY, value.LeftTrigger,      value.RightTrigger};
    for (u32 index = 0; index < after.size(); ++index) {
        if (before[index] != after[index])
            events.push_back({.type = EventType::GamepadAxisMoved,
                              .gamepadId = pad.id,
                              .gamepadFamily = GamepadType::Xbox,
                              .gamepadAxis = static_cast<GamepadAxis>(index + 1),
                              .axisValue = static_cast<float>(after[index])});
    }
    pad.previous = value;
}
Key key(W::System::VirtualKey value)
{
    const auto code = static_cast<u32>(value);
    if (code >= 65 && code <= 90)
        return static_cast<Key>(static_cast<u32>(Key::A) + code - 65);
    if (code >= 48 && code <= 57)
        return static_cast<Key>(static_cast<u32>(Key::Digit0) + code - 48);
    switch (code) {
    case 27:
        return Key::Escape;
    case 13:
        return Key::Return;
    case 32:
        return Key::Space;
    case 9:
        return Key::Tab;
    case 8:
        return Key::Backspace;
    case 37:
        return Key::Left;
    case 38:
        return Key::Up;
    case 39:
        return Key::Right;
    case 40:
        return Key::Down;
    default:
        return Key::Unknown;
    }
}
void focus(bool value)
{
    focused = value;
    if (primary)
        primary->focused = value;
    pending.push_back({.type = value ? EventType::WindowFocusGained : EventType::WindowFocusLost, .windowId = 1});
    setVibrationFocus(value);
}
} // namespace

void bindCoreWindow(void* object)
{
    coreWindow = nullptr;
    winrt::copy_from_abi(coreWindow, object);
    coreWindow.Closed([](const auto&, const auto&) { pending.push_back({.type = EventType::Quit}); });
    coreWindow.VisibilityChanged([](const auto&, const W::UI::Core::VisibilityChangedEventArgs& e) {
        if (primary)
            primary->visible = e.Visible();
        focus(e.Visible());
    });
    coreWindow.Activated([](const auto&, const W::UI::Core::WindowActivatedEventArgs& e) {
        focus(e.WindowActivationState() != W::UI::Core::CoreWindowActivationState::Deactivated);
    });
    coreWindow.SizeChanged([](const auto&, const auto&) {
        if (!primary)
            return;
        const auto size = windowPixelSize(*primary);
        pending.push_back(
            {.type = EventType::WindowResized, .windowId = 1, .width = size.width, .height = size.height});
    });
    coreWindow.KeyDown([](const auto&, const W::UI::Core::KeyEventArgs& e) {
        const auto translated = key(e.VirtualKey());
        if (translated != Key::Unknown)
            pending.push_back(
                {.type = EventType::KeyDown, .windowId = 1, .key = translated, .repeat = e.KeyStatus().WasKeyDown});
    });
    coreWindow.KeyUp([](const auto&, const W::UI::Core::KeyEventArgs& e) {
        const auto translated = key(e.VirtualKey());
        if (translated != Key::Unknown)
            pending.push_back({.type = EventType::KeyUp, .windowId = 1, .key = translated});
    });
    coreWindow.CharacterReceived([](const auto&, const W::UI::Core::CharacterReceivedEventArgs& e) {
        if (!primary || !primary->textInput)
            return;
        const auto cp = e.KeyCode();
        if (cp < 32 || cp > 0x10FFFF)
            return;
        Event event{.type = EventType::TextInput, .windowId = 1};
        const std::wstring wide = cp <= 0xFFFF ? std::wstring(1, static_cast<wchar_t>(cp))
                                               : std::wstring{static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10)),
                                                              static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 1023))};
        const auto utf8 = winrt::to_string(wide);
        std::memcpy(event.text, utf8.data(), utf8.size());
        pending.push_back(event);
    });
    // B is delivered through WGI. Consuming the shell's separate back request
    // prevents it closing the app, without synthesizing a keyboard Escape.
    W::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [](const auto&, const W::UI::Core::BackRequestedEventArgs& e) { e.Handled(true); });
    W::ApplicationModel::Core::CoreApplication::Suspending(
        [](const auto&, const W::ApplicationModel::SuspendingEventArgs& e) {
            const auto deferral = e.SuspendingOperation().GetDeferral();
            (void)coreWindow.Dispatcher().RunAsync(W::UI::Core::CoreDispatcherPriority::High, [deferral] {
                suspendDeferral = deferral;
                pending.push_back({.type = EventType::WillEnterBackground});
                focus(false);
            });
        });
    W::ApplicationModel::Core::CoreApplication::Resuming([](const auto&, const auto&) {
        (void)coreWindow.Dispatcher().RunAsync(W::UI::Core::CoreDispatcherPriority::High, [] { focus(true); });
    });
}
std::optional<core::EngineError> init(const InitOptions& options)
{
    if (initialized)
        return std::nullopt;
    if (!coreWindow)
        return core::makeError(ENG_TR("platform.err.window_failed"), {}, "CoreApplication view is not bound");
    wanted = options.gamepads.value_or(!options.headless);
    initialized = true;
    return std::nullopt;
}
void shutdown()
{
    stopVibration();
    pads.clear();
    initialized = false;
}
bool isInitialized() noexcept
{
    return initialized;
}
u64 nowNs() noexcept
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
void sleepNs(u64 ns) noexcept
{
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
core::i64 threadCpuNs() noexcept
{
    return -1;
}
u64 residentBytes() noexcept
{
    try {
        return W::System::MemoryManager::AppMemoryUsage();
    } catch (...) {
        return 0;
    }
}
u64 systemMemoryBytes() noexcept
{
    try {
        return W::System::MemoryManager::AppMemoryUsageLimit();
    } catch (...) {
        return 0;
    }
}
bool raiseProcessPriority() noexcept
{
    return false;
}
std::vector<std::byte> applicationIconBytes()
{
    return {};
}
void setApplicationId(std::string_view)
{}
const Paths& paths()
{
    if (directories.executableDir.empty()) {
        directories.executableDir = W::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
        directories.contentDir = directories.executableDir / "content";
        directories.userDir = W::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        directories.reportDir = directories.userDir;
    }
    return directories;
}
std::filesystem::path documentsFolder()
{
    return {};
}
void* androidJavaEnv()
{
    return nullptr;
}
void requestDisplayFrameRate(float) noexcept
{}
bool startDetached(const std::vector<std::string>&)
{
    return false;
}
int askChoice(Window*, std::string_view, std::string_view, const std::vector<std::string>&)
{
    return -1;
}
void setScreenOrientation(Window&, int)
{}
InputDevices inputDevices() noexcept
{
    return {false, false, !pads.empty()};
}
bool canPickFolder()
{
    return false;
}
void pickFolder(Window&, std::string_view, std::function<void(std::filesystem::path)> done)
{
    done({});
}
void pickFiles(Window&, std::string_view, bool, std::function<void(std::vector<std::filesystem::path>)> done)
{
    done({});
}
std::span<const std::string> droppedFiles() noexcept
{
    return {};
}
std::vector<std::string> preferredLocales()
{
    std::vector<std::string> out;
    for (const auto& locale : W::Globalization::ApplicationLanguages::Languages())
        out.push_back(winrt::to_string(locale));
    return out;
}
void WindowDeleter::operator()(Window* window) const noexcept
{
    if (primary == window)
        primary = nullptr;
    delete window;
}
WindowPtr createWindow(const WindowDesc& desc, core::EngineError* error)
{
    if (!coreWindow || primary) {
        if (error)
            *error = core::makeError(ENG_TR("platform.err.window_failed"), {}, "UWP supports one primary CoreWindow");
        return {};
    }
    WindowPtr result(new Window);
    result->native = coreWindow;
    result->title = std::string(desc.title);
    primary = result.get();
    return result;
}
WindowsSurface windowsSurface(const Window& w) noexcept
{
    return {WindowsSurfaceKind::CoreWindow, winrt::get_abi(w.native)};
}
u32 windowId(const Window&) noexcept
{
    return 1;
}
f32 windowDisplayScale(const Window&) noexcept
{
    return static_cast<float>(W::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
}
WindowSize windowPixelSize(const Window& w) noexcept
{
    const auto bounds = w.native.Bounds();
    const auto scale = windowDisplayScale(w);
    return {static_cast<i32>(bounds.Width * scale), static_cast<i32>(bounds.Height * scale)};
}
bool windowFocused(const Window& w) noexcept
{
    return w.focused;
}
bool windowMinimized(const Window& w) noexcept
{
    return !w.visible;
}
f32 windowRefreshRate(const Window&) noexcept
{
    return 0;
}
WindowInsets windowSafeAreaInsets(const Window&) noexcept
{
    return {};
}
WindowPlacement windowPlacement(const Window& w) noexcept
{
    const auto b = w.native.Bounds();
    return {0, 0, static_cast<i32>(b.Width), static_cast<i32>(b.Height), true};
}
void setWindowPlacement(Window&, const WindowPlacement&)
{}
void setWindowFullscreen(Window&, bool value)
{
    if (value)
        (void)W::UI::ViewManagement::ApplicationView::GetForCurrentView().TryEnterFullScreenMode();
    else
        W::UI::ViewManagement::ApplicationView::GetForCurrentView().ExitFullScreenMode();
}
std::string windowTitle(const Window& w)
{
    return w.title;
}
void setWindowTitle(Window& w, std::string_view title)
{
    w.title = title;
}
std::vector<DisplayInfo> displays()
{
    if (!primary)
        return {};
    const auto s = windowPixelSize(*primary);
    return {{"", s.width, s.height, 0}};
}
std::vector<DisplayMode> displayModes(core::usize)
{
    if (!primary)
        return {};
    const auto s = windowPixelSize(*primary);
    return {{s.width, s.height, 0}};
}
core::usize windowDisplay(const Window&)
{
    return 0;
}
void setWindowMode(Window& w, WindowMode, core::usize, i32, i32)
{
    setWindowFullscreen(w, true);
}
WindowPlacement usableDisplayArea() noexcept
{
    return primary ? windowPlacement(*primary) : WindowPlacement{};
}
bool setPointerLocked(Window&, bool)
{
    return false;
}
void setPointerVisible(bool)
{}
void setPointerPosition(Window&, f32, f32)
{}
bool setWindowIcon(Window&, std::span<const std::byte>, i32, i32)
{
    return false;
}

std::span<const Event> pumpEvents()
{
    // Completes the prior suspension only after the engine consumed the
    // background event and saved state. CoreWindow pumping stays on its thread.
    if (suspendDeferral) {
        suspendDeferral.Complete();
        suspendDeferral = nullptr;
    }
    coreWindow.Dispatcher().ProcessEvents(W::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
    events.clear();
    events.swap(pending);
    pending.clear();
    if (!wanted)
        return events;
    const auto connected = W::Gaming::Input::Gamepad::Gamepads();
    for (auto iterator = pads.begin(); iterator != pads.end();) {
        bool present = false;
        for (const auto& pad : connected)
            if (pad == iterator->native)
                present = true;
        if (!present) {
            reading(*iterator, {});
            events.push_back(
                {.type = EventType::GamepadRemoved, .gamepadId = iterator->id, .gamepadFamily = GamepadType::Xbox});
            iterator = pads.erase(iterator);
        }
        else
            ++iterator;
    }
    for (const auto& native : connected) {
        auto found = std::find_if(pads.begin(), pads.end(), [&](const Pad& p) { return p.native == native; });
        if (found == pads.end()) {
            pads.push_back({native, nextPad++, {}});
            found = pads.end() - 1;
            events.push_back(
                {.type = EventType::GamepadAdded, .gamepadId = found->id, .gamepadFamily = GamepadType::Xbox});
        }
        try {
            reading(*found, focused ? native.GetCurrentReading() : W::Gaming::Input::GamepadReading{});
        } catch (const winrt::hresult_error&) {
            reading(*found, {});
        }
    }
    return events;
}
GamepadType gamepadType(u32 id) noexcept
{
    for (const auto& p : pads)
        if (p.id == id)
            return GamepadType::Xbox;
    return GamepadType::Unknown;
}
bool gamepadsAvailable() noexcept
{
    return true;
}
void setGamepadsWanted(bool value) noexcept
{
    wanted = value;
}
bool gamepadsStarted() noexcept
{
    return wanted && initialized;
}
u32 attachVirtualGamepad() noexcept
{
    return 0;
}
void detachVirtualGamepad(u32) noexcept
{}
void setVirtualGamepadButton(u32, GamepadButton, bool) noexcept
{}
void setVirtualGamepadAxis(u32, GamepadAxis, f32) noexcept
{}
VirtualRumble virtualGamepadRumble() noexcept
{
    return {};
}
bool pumpHeld() noexcept
{
    return false;
}
bool simulateWindowHold(u32, u32) noexcept
{
    return false;
}
void setTextInputEnabled(u32, bool value) noexcept
{
    if (primary)
        primary->textInput = value;
}
void setTextInputEnabled(u32 id, bool value, const TextInputOptions&) noexcept
{
    setTextInputEnabled(id, value);
}
bool textInputEnabled(u32) noexcept
{
    return primary && primary->textInput;
}
void setTextInputArea(u32, i32, i32, i32, i32, i32) noexcept
{}
void setVibrationSink(VibrationSink* sink) noexcept
{
    vibration = sink;
}
bool vibrationSupported(bool gamepad) noexcept
{
    return vibration ? (gamepad ? vibration->gamepad() : vibration->device()) : gamepad && !pads.empty();
}
bool vibrationMotorSupported(bool gamepad, VibrationMotor motor) noexcept
{
    return gamepad && !pads.empty() && static_cast<u32>(motor) < 4;
}
void setVibrationMotor(VibrationMotor motor, f32 value) noexcept
{
    if (static_cast<u32>(motor) < motors.size())
        motors[static_cast<u32>(motor)] = std::clamp(value, 0.0f, 1.0f);
}
void vibrateDevice(f32 strength, f32 seconds) noexcept
{
    if (vibration)
        vibration->vibrate(strength, seconds);
}
void setVibrationFocus(bool value) noexcept
{
    focused = value;
    if (!value)
        stopVibration();
}
void pumpVibration() noexcept
{
    const W::Gaming::Input::GamepadVibration state{focused ? motors[0] : 0, focused ? motors[1] : 0,
                                                   focused ? motors[2] : 0, focused ? motors[3] : 0};
    for (const auto& pad : pads)
        try {
            pad.native.Vibration(state);
        } catch (const winrt::hresult_error&) {
        }
}
void stopVibration() noexcept
{
    motors = {};
    pumpVibration();
    if (vibration)
        vibration->stop();
}
} // namespace engine::platform
