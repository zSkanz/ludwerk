// Native UWP feasibility host (ADR 0191). This is not the engine's player/RHI.
// All WinRT interfaces stay here; the existing VM and catalog are reused.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

#include "Luau/Compiler.h"
#include "engine/core/i18n.h"
#include "lua.h"
#include "lualib.h"

namespace engine::uwp {
using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::UI::Core;

struct Probe : implements<Probe, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }

    void Initialize(const CoreApplicationView& view)
    {
        m_local = std::filesystem::path(ApplicationDataPath());
        log("UWP_HOST_INITIALIZED");
        view.Activated([this](const auto&, const Windows::ApplicationModel::Activation::IActivatedEventArgs& args) {
            const auto launch = args.try_as<Windows::ApplicationModel::Activation::LaunchActivatedEventArgs>();
            m_selfTest = launch && launch.Arguments() == L"--self-test";
            m_window.Activate();
        });
        m_suspending = CoreApplication::Suspending(auto_revoke, [this](const auto&, const SuspendingEventArgs& args) {
            const auto deferral = args.SuspendingOperation().GetDeferral();
            save();
            if (m_device)
                m_device.as<IDXGIDevice3>()->Trim();
            log("UWP_SUSPENDED");
            deferral.Complete();
        });
        m_resuming = CoreApplication::Resuming(auto_revoke, [this](const auto&, const auto&) {
            ++m_resumes;
            m_clock = std::chrono::steady_clock::now();
            log("UWP_RESUMED");
        });
    }

    void SetWindow(const CoreWindow& window)
    {
        log("UWP_WINDOW_READY");
        m_window = window;
        // Xbox maps B to system Back as well as gamepad input. Own that action
        // inside this view so resetting the probe does not navigate out of it.
        m_back = SystemNavigationManager::GetForCurrentView().BackRequested(
            auto_revoke, [](const auto&, const BackRequestedEventArgs& args) { args.Handled(true); });
        window.Closed([this](const auto&, const auto&) { m_closed = true; });
        window.VisibilityChanged([this](const auto&, const VisibilityChangedEventArgs& args) {
            m_visible = args.Visible();
            m_clock = std::chrono::steady_clock::now();
        });
        window.SizeChanged([this](const auto&, const auto&) { m_resize = true; });
        // Do not use Escape as a quit shortcut: UWP also maps gamepad B to it.
    }

    void Load(const hstring&)
    {
        log("UWP_LOAD_BEGIN");
        m_local = std::filesystem::path(ApplicationDataPath());
        const std::filesystem::path installed(Package::Current().InstalledLocation().Path().c_str());
        const auto loaded = core::engineCatalog().loadFromFile(installed / "i18n/en.json");
        if (!loaded)
            throw hresult_error(E_FAIL, to_hstring(loaded.diagnostic));
        std::ifstream file(installed / "probe.luau", std::ios::binary);
        if (!file)
            throw hresult_error(E_FAIL, L"probe.luau");
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        m_vm.reset(luaL_newstate());
        if (!m_vm)
            throw hresult_error(E_OUTOFMEMORY);
        luaL_openlibs(m_vm.get());
        luaL_sandbox(m_vm.get());
        lua_State* thread = lua_newthread(m_vm.get());
        luaL_sandboxthread(thread);
        const auto bytecode = Luau::compile(source);
        if (luau_load(thread, "@probe.luau", bytecode.data(), bytecode.size(), 0) != 0 ||
            lua_resume(thread, nullptr, 0) != 0 || !lua_isfunction(thread, -1))
            throw hresult_error(E_FAIL, L"probe.luau: invalid entry point");
        m_update = lua_ref(thread, -1);
        std::ifstream saved(m_local / "position.txt");
        float x = 0.5f;
        float y = 0.5f;
        if (saved >> x >> y && std::isfinite(x) && std::isfinite(y)) {
            m_x = std::clamp(x, 0.05f, 0.95f);
            m_y = std::clamp(y, 0.3f, 0.9f);
        }
        createDevice();
        log("UWP_LUAU_ASSET_READY");
    }

    void Run()
    {
        m_clock = std::chrono::steady_clock::now();
        while (!m_closed) {
            m_window.Dispatcher().ProcessEvents(m_visible ? CoreProcessEventsOption::ProcessAllIfPresent
                                                          : CoreProcessEventsOption::ProcessOneAndAllPending);
            if (!m_visible || m_closed)
                continue;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - m_clock).count(), 0.05f);
            m_clock = now;
            update(dt);
            if (m_resize)
                createDevice();
            draw();
            if (++m_frames == 1)
                log("UWP_PRESENT_READY");
            if (m_selfTest && m_frames == 120) {
                log("UWP_DESKTOP_SMOKE_OK");
                m_closed = true;
            }
        }
        save();
        CoreApplication::Exit();
    }

    void Uninitialize() { save(); }

private:
    static std::wstring ApplicationDataPath()
    {
        return Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
    }

    void log(const char* marker)
    {
        if (!m_local.empty())
            std::ofstream(m_local / "probe.log", std::ios::app) << marker << '\n';
    }

    void save()
    {
        if (!m_local.empty())
            std::ofstream(m_local / "position.txt") << m_x << ' ' << m_y << '\n';
    }

    void update(float dt)
    {
        float ax = 0;
        float ay = 0;
        bool reset = false;
        bool gamepadInput = false;
        m_action = false;
        const auto pads = Gamepad::Gamepads();
        m_padConnected = pads.Size() != 0;
        for (const auto& pad : pads) {
            const auto reading = pad.GetCurrentReading();
            const float x = static_cast<float>(reading.LeftThumbstickX);
            const float y = static_cast<float>(reading.LeftThumbstickY);
            if (std::abs(x) > 0.2f || std::abs(y) > 0.2f || reading.Buttons != GamepadButtons::None) {
                gamepadInput = true;
                ax = std::abs(x) > 0.2f ? x : 0;
                ay = std::abs(y) > 0.2f ? y : 0;
                if ((reading.Buttons & GamepadButtons::DPadLeft) != GamepadButtons::None)
                    ax = -1;
                if ((reading.Buttons & GamepadButtons::DPadRight) != GamepadButtons::None)
                    ax = 1;
                if ((reading.Buttons & GamepadButtons::DPadUp) != GamepadButtons::None)
                    ay = 1;
                if ((reading.Buttons & GamepadButtons::DPadDown) != GamepadButtons::None)
                    ay = -1;
                reset = (reading.Buttons & GamepadButtons::B) != GamepadButtons::None;
                m_action = (reading.Buttons & GamepadButtons::A) != GamepadButtons::None;
                if (!m_seenInput) {
                    m_seenInput = true;
                    log("UWP_GAMEPAD_INPUT_READY");
                }
                break;
            }
        }
        const auto down = [&](Windows::System::VirtualKey key) {
            return (m_window.GetKeyState(key) & CoreVirtualKeyStates::Down) != CoreVirtualKeyStates::None;
        };
        // UWP maps gamepad buttons to keys too; do not process the same input twice.
        if (!gamepadInput) {
            if (down(Windows::System::VirtualKey::Left))
                ax = -1;
            if (down(Windows::System::VirtualKey::Right))
                ax = 1;
            if (down(Windows::System::VirtualKey::Up))
                ay = 1;
            if (down(Windows::System::VirtualKey::Down))
                ay = -1;
            reset = down(Windows::System::VirtualKey::Space);
        }
        lua_State* vm = m_vm.get();
        lua_getref(vm, m_update);
        lua_pushnumber(vm, m_x);
        lua_pushnumber(vm, m_y);
        lua_pushnumber(vm, ax);
        lua_pushnumber(vm, ay);
        lua_pushboolean(vm, reset);
        lua_pushnumber(vm, dt);
        if (lua_pcall(vm, 6, 2, 0) != 0) {
            log("UWP_SCRIPT_FAILED");
            throw hresult_error(E_FAIL);
        }
        m_x = static_cast<float>(lua_tonumber(vm, -2));
        m_y = static_cast<float>(lua_tonumber(vm, -1));
        lua_pop(vm, 2);
    }

    void createDevice()
    {
        m_resize = false;
        if (m_d2d)
            m_d2d->SetTarget(nullptr);
        m_bitmap = nullptr;
        m_swapchain = nullptr;
        m_brush = nullptr;
        m_d2d = nullptr;
        m_device = nullptr;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr,
                                        context.put()));
        m_device = device.as<ID3D11Device1>();
        const auto dxgi = m_device.as<IDXGIDevice>();
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgi->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        const auto bounds = m_window.Bounds();
        m_width = std::max(bounds.Width, 1.0f);
        m_height = std::max(bounds.Height, 1.0f);
        const float dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().LogicalDpi();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(std::max(1.0f, m_width * dpi / 96.0f));
        desc.Height = static_cast<UINT>(std::max(1.0f, m_height * dpi / 96.0f));
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        check_hresult(factory->CreateSwapChainForCoreWindow(m_device.get(), m_window.as<::IUnknown>().get(), &desc,
                                                            nullptr, m_swapchain.put()));
        com_ptr<ID2D1Factory1> d2dFactory;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()));
        com_ptr<ID2D1Device> d2dDevice;
        check_hresult(d2dFactory->CreateDevice(dxgi.get(), d2dDevice.put()));
        check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2d.put()));
        com_ptr<IDXGISurface> surface;
        check_hresult(m_swapchain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));
        const auto properties =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
        check_hresult(m_d2d->CreateBitmapFromDxgiSurface(surface.get(), &properties, m_bitmap.put()));
        m_d2d->SetTarget(m_bitmap.get());
        check_hresult(m_d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), m_brush.put()));
        com_ptr<IDWriteFactory> textFactory;
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<::IUnknown**>(textFactory.put())));
        m_text = nullptr;
        check_hresult(textFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f,
                                                    L"en-US", m_text.put()));
    }

    void text(core::TextKey key, float y, const D2D1_COLOR_F& color)
    {
        const auto value = to_hstring(core::tr(key));
        m_brush->SetColor(color);
        const auto box = D2D1::RectF(40, y, m_width - 40, y + 70);
        m_d2d->DrawText(value.c_str(), static_cast<UINT32>(value.size()), m_text.get(), box, m_brush.get());
    }

    void draw()
    {
        m_d2d->BeginDraw();
        m_d2d->Clear(D2D1::ColorF(0.035f, 0.055f, 0.09f));
        text(ENG_TR("uwp.probe.title"), 28, D2D1::ColorF(1, 1, 1));
        text(ENG_TR("uwp.probe.scope"), 75, D2D1::ColorF(0.7f, 0.75f, 0.85f));
        text(ENG_TR("uwp.probe.ready"), 120, D2D1::ColorF(0.3f, 0.9f, 0.65f));
        text(m_padConnected ? ENG_TR("uwp.probe.pad_connected") : ENG_TR("uwp.probe.pad_missing"), 165,
             D2D1::ColorF(0.7f, 0.75f, 0.85f));
        text(ENG_TR("uwp.probe.controls"), m_height - 100, D2D1::ColorF(0.7f, 0.75f, 0.85f));
        m_brush->SetColor(m_action ? D2D1::ColorF(1.0f, 0.7f, 0.25f) : D2D1::ColorF(0.3f, 0.9f, 0.65f));
        m_d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(m_x * m_width, m_y * m_height), 24, 24), m_brush.get());
        const HRESULT drawn = m_d2d->EndDraw();
        const HRESULT presented = SUCCEEDED(drawn) ? m_swapchain->Present(1, 0) : drawn;
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET ||
            presented == D2DERR_RECREATE_TARGET) {
            log("UWP_DEVICE_RECREATE");
            createDevice();
        }
        else
            check_hresult(presented);
    }

    CoreWindow m_window{nullptr};
    CoreApplication::Suspending_revoker m_suspending;
    CoreApplication::Resuming_revoker m_resuming;
    SystemNavigationManager::BackRequested_revoker m_back;
    std::filesystem::path m_local;
    std::unique_ptr<lua_State, decltype(&lua_close)> m_vm{nullptr, &lua_close};
    int m_update = 0;
    float m_x = 0.5f, m_y = 0.5f, m_width = 1, m_height = 1;
    bool m_closed = false, m_visible = true, m_resize = false, m_selfTest = false;
    bool m_padConnected = false, m_seenInput = false, m_action = false;
    unsigned m_frames = 0, m_resumes = 0;
    std::chrono::steady_clock::time_point m_clock;
    com_ptr<ID3D11Device1> m_device;
    com_ptr<IDXGISwapChain1> m_swapchain;
    com_ptr<ID2D1DeviceContext> m_d2d;
    com_ptr<ID2D1Bitmap1> m_bitmap;
    com_ptr<ID2D1SolidColorBrush> m_brush;
    com_ptr<IDWriteTextFormat> m_text;
};
} // namespace engine::uwp

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        winrt::init_apartment();
        winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<engine::uwp::Probe>());
        return 0;
    } catch (const winrt::hresult_error& error) {
        try {
            const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
            std::ofstream(std::filesystem::path(folder.c_str()) / "probe.log", std::ios::app)
                << "UWP_FAILED HRESULT=" << std::hex << static_cast<unsigned>(error.code().value) << '\n';
        } catch (...) {
        }
        return 1;
    }
}
