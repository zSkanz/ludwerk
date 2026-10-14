// AppContainer does not spawn desktop tools, own a console or expose a desktop
// clipboard. Capability queries report that absence instead of emulating it.
#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <windows.h>

#include "engine/platform/clipboard.h"
#include "engine/platform/console.h"
#include "engine/platform/crash.h"
#include "engine/platform/process.h"
#include "engine/platform/stop_signal.h"
// Generated Windows SDK delegates use COM lifetime rather than virtual destructors.
#pragma warning(push)
#pragma warning(disable : 4265)
#include <winrt/Windows.System.Profile.h>
#pragma warning(pop)

namespace engine::platform {
namespace {
std::filesystem::path crashDirectory;
std::atomic<bool> stopped{false};
} // namespace
ProcessResult runProcess(const std::vector<std::string>&)
{
    return {};
}
ProcessResult runProcess(const std::vector<std::string>&, const ProcessOptions&)
{
    return {};
}
unsigned long processId() noexcept
{
    return GetCurrentProcessId();
}
unsigned long windowsBuild() noexcept
{
    try {
        const auto version = std::stoull(
            winrt::to_string(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion()));
        return static_cast<unsigned long>((version >> 16) & 65535);
    } catch (...) {
        return 0;
    }
}
std::string strongestGraphicsCard()
{
    return {};
}
bool graphicsDriverNamed() noexcept
{
    return false;
}
std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>&)
{
    return {};
}
std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>&, const ProcessOptions&)
{
    return {};
}
ChildProcess::~ChildProcess() = default;
std::string ChildProcess::readAvailable()
{
    return {};
}
bool ChildProcess::running()
{
    return false;
}
void ChildProcess::kill()
{}
void writeConsole(ConsoleStream stream, std::string_view text)
{
    const std::string terminated(text);
    OutputDebugStringA(terminated.c_str());
    (void)std::fwrite(text.data(), 1, text.size(), stream == ConsoleStream::Out ? stdout : stderr);
}
bool consoleClosesWithProcess() noexcept
{
    return false;
}
bool outputGoesUnread() noexcept
{
    return true;
}
bool installCrashHandler(const std::filesystem::path& directory)
{
    crashDirectory = directory;
    std::set_terminate([] {
        try {
            std::ofstream file(crashNotePath(), std::ios::app);
            file << "Unhandled C++ exception\n";
            if (const auto error = std::current_exception())
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& e) {
                    file << e.what() << '\n';
                } catch (...) {
                    file << "Non-standard exception\n";
                }
        } catch (...) {
        }
        std::abort();
    });
    return true;
}
std::filesystem::path crashArtifactPath()
{
    return crashNotePath();
}
std::filesystem::path crashNotePath()
{
    return crashNotePathOf(crashDirectory, processId());
}
std::filesystem::path crashNotePathOf(const std::filesystem::path& directory, unsigned long process)
{
    return directory / ("engine-crash-" + std::to_string(process) + ".txt");
}
std::filesystem::path crashDumpPathOf(const std::filesystem::path&, unsigned long)
{
    return {};
}
void installStopSignals()
{}
bool stopRequested() noexcept
{
    return stopped.load();
}
void clearStopRequest() noexcept
{
    stopped = false;
}
double stopDeadlineSeconds() noexcept
{
    return (std::numeric_limits<double>::infinity)();
}
void stopFinished() noexcept
{}
bool simulateConsoleClose(double) noexcept
{
    return false;
}
std::optional<std::string> clipboardText()
{
    return std::nullopt;
}
bool setClipboardText(std::string_view)
{
    return false;
}
std::optional<std::string> clipboardTextFrom(const ClipboardSource& source)
{
    for (int attempt = 1;; ++attempt) {
        if (!source.hasText())
            return std::nullopt;
        auto text = source.read();
        if (!text || !text->empty() || attempt >= ClipboardAttempts)
            return text;
        source.wait();
    }
}
} // namespace engine::platform
