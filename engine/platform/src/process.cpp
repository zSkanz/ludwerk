#include "engine/platform/process.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dxgi1_6.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace engine::platform {
namespace {

// One process, its output a pipe this side reads, with the options applied.
[[nodiscard]] SDL_Process* createProcess(const std::vector<std::string>& arguments, const ProcessOptions& options)
{
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    const std::string directory = options.workingDirectory.string();
    if (!directory.empty())
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, directory.c_str());
    // This process's environment, copied, with the extras laid over it.
    SDL_Environment* environment = nullptr;
    if (!options.environment.empty()) {
        environment = SDL_CreateEnvironment(true);
        for (const auto& [name, value] : options.environment)
            (void)SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    }
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (environment != nullptr)
        SDL_DestroyEnvironment(environment);
    return process;
}

} // namespace

unsigned long processId() noexcept
{
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

unsigned long windowsBuild() noexcept
{
#if defined(_WIN32)
    // **A build to answer as**, for exercising what an older Windows is given
    // on a machine that is not one: there is no other way to run that branch
    // short of owning the system.
    char assumed[16];
    const DWORD length = GetEnvironmentVariableA("ENG_ASSUME_WINDOWS_BUILD", assumed, sizeof(assumed));
    if (length != 0 && length < sizeof(assumed)) {
        const unsigned long build = std::strtoul(assumed, nullptr, 10);
        if (build != 0)
            return build;
    }
    // `RtlGetVersion`, from the system's own library: `GetVersionEx` answers
    // "Windows 8" to any program whose manifest does not name a later one.
    struct Version
    {
        unsigned long size;
        unsigned long major;
        unsigned long minor;
        unsigned long build;
        unsigned long platform;
        wchar_t servicePack[128];
    };
    using Ask = long(__stdcall*)(Version*);
    const HMODULE system = GetModuleHandleW(L"ntdll.dll");
    if (system == nullptr)
        return 0;
    const auto ask = reinterpret_cast<Ask>(reinterpret_cast<void*>(GetProcAddress(system, "RtlGetVersion")));
    if (ask == nullptr)
        return 0;
    Version version{};
    version.size = sizeof(Version);
    return ask(&version) == 0 ? version.build : 0;
#else
    return 0;
#endif
}

std::string strongestGraphicsCard()
{
#if defined(_WIN32)
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory))) || factory == nullptr)
        return {};
    std::string name;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                      reinterpret_cast<void**>(&adapter))) &&
        adapter != nullptr) {
        DXGI_ADAPTER_DESC1 described{};
        if (SUCCEEDED(adapter->GetDesc1(&described))) {
            char utf8[512];
            const int written = WideCharToMultiByte(CP_UTF8, 0, described.Description, -1, utf8,
                                                    static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (written > 1)
                name.assign(utf8, static_cast<std::size_t>(written - 1));
        }
        adapter->Release();
    }
    factory->Release();
    return name;
#else
    return {};
#endif
}

bool graphicsDriverNamed() noexcept
{
#if defined(_WIN32)
    char named[64];
    const DWORD length = GetEnvironmentVariableA("SDL_GPU_DRIVER", named, sizeof(named));
    return length != 0;
#else
    const char* named = std::getenv("SDL_GPU_DRIVER");
    return named != nullptr && named[0] != 0;
#endif
}

ProcessResult runProcess(const std::vector<std::string>& arguments)
{
    return runProcess(arguments, ProcessOptions{});
}

ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options)
{
    ProcessResult result;
    if (arguments.empty())
        return result;
    SDL_Process* process = createProcess(arguments, options);
    if (process == nullptr)
        return result;
    result.started = true;

    std::size_t size = 0;
    int exitCode = -1;
    // Reads until the program closes its output, then waits for it.
    void* data = SDL_ReadProcess(process, &size, &exitCode);
    if (data != nullptr) {
        result.output.assign(static_cast<const char*>(data), size);
        SDL_free(data);
    }
    result.exitCode = exitCode;
    SDL_DestroyProcess(process);
    return result;
}

std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>& arguments)
{
    return start(arguments, ProcessOptions{});
}

std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>& arguments,
                                                  const ProcessOptions& options)
{
    if (arguments.empty())
        return nullptr;
    SDL_Process* process = createProcess(arguments, options);
    if (process == nullptr)
        return nullptr;
    return std::unique_ptr<ChildProcess>(new ChildProcess(process));
}

ChildProcess::~ChildProcess()
{
    kill();
    if (m_process != nullptr)
        SDL_DestroyProcess(static_cast<SDL_Process*>(m_process));
}

std::string ChildProcess::readAvailable()
{
    std::string text;
    if (m_process == nullptr)
        return text;
    SDL_IOStream* output = SDL_GetProcessOutput(static_cast<SDL_Process*>(m_process));
    if (output == nullptr)
        return text;
    // The pipe is non-blocking: a read with nothing waiting returns zero and
    // says NOT_READY, which is the end of this call rather than an error.
    char buffer[4096];
    for (;;) {
        const std::size_t read = SDL_ReadIO(output, buffer, sizeof(buffer));
        if (read == 0)
            break;
        text.append(buffer, read);
    }
    return text;
}

bool ChildProcess::running()
{
    if (m_process == nullptr || m_exited)
        return false;
    int code = -1;
    if (SDL_WaitProcess(static_cast<SDL_Process*>(m_process), false, &code)) {
        m_exited = true;
        m_exitCode = code;
        return false;
    }
    return true;
}

void ChildProcess::kill()
{
    if (m_process == nullptr || !running())
        return;
    (void)SDL_KillProcess(static_cast<SDL_Process*>(m_process), true);
    int code = -1;
    (void)SDL_WaitProcess(static_cast<SDL_Process*>(m_process), true, &code);
    m_exited = true;
    m_exitCode = code;
}

} // namespace engine::platform
