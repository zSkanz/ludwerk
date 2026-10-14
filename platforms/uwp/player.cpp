// Native CoreApplication entry into the SAME player command line and engine
// loop used by desktop/Android. The game is the exported pack under game/.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

#include "engine/platform/platform.h"
#include "engine/platform/windows_interop.h"
// Generated Windows SDK delegates use COM lifetime rather than virtual destructors.
#pragma warning(push)
#pragma warning(disable : 4265)
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#pragma warning(pop)

int engineHostMain(int argc, char** argv);
namespace engine::uwp {
namespace W = winrt::Windows;
struct Player : winrt::implements<Player, W::ApplicationModel::Core::IFrameworkViewSource,
                                  W::ApplicationModel::Core::IFrameworkView>
{
    W::UI::Core::CoreWindow window{nullptr};
    W::ApplicationModel::Core::IFrameworkView CreateView() { return *this; }
    void Initialize(const W::ApplicationModel::Core::CoreApplicationView& view)
    {
        view.Activated([this](const auto&, const auto&) { window.Activate(); });
    }
    void SetWindow(const W::UI::Core::CoreWindow& value)
    {
        window = value;
        platform::bindCoreWindow(winrt::get_abi(window));
    }
    void Load(const winrt::hstring&) {}
    void Run()
    {
        std::ofstream(platform::paths().userDir / "player-entry.txt")
            << "Entering engineHostMain\n"
            << "memoryBudgetBytes=" << platform::systemMemoryBytes() << '\n'
            << "memoryUsageBytes=" << platform::residentBytes() << '\n';
        const auto game = platform::paths().executableDir / "game";
        const auto utf8 = game.u8string();
        std::vector<std::string> arguments{
            "engine-host", std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), "--rhi=d3d12"};
        std::vector<char*> argv;
        for (auto& argument : arguments)
            argv.push_back(argument.data());
        const int code = engineHostMain(static_cast<int>(argv.size()), argv.data());
        std::ofstream(platform::paths().userDir / "player-result.txt") << code << '\n';
        W::ApplicationModel::Core::CoreApplication::Exit();
    }
    void Uninitialize() {}
};
} // namespace engine::uwp
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        winrt::init_apartment();
        winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<engine::uwp::Player>());
        return 0;
    } catch (const winrt::hresult_error& error) {
        try {
            const std::filesystem::path directory(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
            std::ofstream(directory / "player-fatal.txt", std::ios::app)
                << error.code().value << ' ' << winrt::to_string(error.message()) << '\n';
        } catch (...) {
        }
        return 1;
    } catch (const std::exception& error) {
        try {
            const std::filesystem::path directory(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
            std::ofstream(directory / "player-fatal.txt", std::ios::app) << error.what() << '\n';
        } catch (...) {
        }
        return 1;
    }
}
