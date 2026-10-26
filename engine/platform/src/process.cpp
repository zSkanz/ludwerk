#include "engine/platform/process.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace engine::platform {

// **The output read as it comes**: a thread blocked on the pipe's own handle,
// which SDL made non-blocking and is made blocking again here, since this
// thread is now the only reader. It ends when the child's end of the pipe
// closes -- the child exits, or is killed.
struct ChildProcess::Reader
{
    std::mutex lock;
    std::string text;
    std::thread thread;

    ~Reader()
    {
        if (!thread.joinable())
            return;
#if defined(_WIN32)
        // A child that ended leaves its read ended too, unless another child
        // started meanwhile holds a copy of the pipe's other end: ended here
        // rather than waited on.
        (void)CancelSynchronousIo(static_cast<HANDLE>(thread.native_handle()));
#endif
        thread.join();
    }
};

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
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          options.pipeInput ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
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

std::vector<GraphicsCard> graphicsCards()
{
    std::vector<GraphicsCard> cards;
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr)
        return cards;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 described{};
        // The software rasteriser is not a card, flagged or not: a machine
        // with no GPU -- a virtual one -- lists Microsoft's Basic Render
        // Driver without the flag, by its vendor and device alone.
        if (SUCCEEDED(adapter->GetDesc1(&described)) && (described.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            !(described.VendorId == 0x1414 && described.DeviceId == 0x8C)) {
            char utf8[512];
            const int written = WideCharToMultiByte(CP_UTF8, 0, described.Description, -1, utf8,
                                                    static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (written > 1)
                cards.push_back({std::string(utf8, static_cast<std::size_t>(written - 1)),
                                 static_cast<unsigned long long>(described.DedicatedVideoMemory)});
        }
        adapter->Release();
    }
    factory->Release();
    // By name; and one card a system lists twice -- once per output, on some
    // drivers -- is one card.
    std::sort(cards.begin(), cards.end(), [](const GraphicsCard& a, const GraphicsCard& b) { return a.name < b.name; });
    cards.erase(std::unique(cards.begin(), cards.end(),
                            [](const GraphicsCard& a, const GraphicsCard& b) { return a.name == b.name; }),
                cards.end());
#endif
    return cards;
}

std::optional<bool> graphicsCardSharesMemory(std::string_view name)
{
#if defined(_WIN32)
    // Loaded, not linked: nothing else in this library draws.
    static const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (d3d12 == nullptr || name.empty())
        return std::nullopt;
    using Create = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    if (create == nullptr)
        return std::nullopt;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr)
        return std::nullopt;
    std::optional<bool> shares;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; !shares.has_value() && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 described{};
        char utf8[512];
        const bool named = SUCCEEDED(adapter->GetDesc1(&described)) &&
                           WideCharToMultiByte(CP_UTF8, 0, described.Description, -1, utf8,
                                               static_cast<int>(sizeof(utf8)), nullptr, nullptr) > 1 &&
                           std::string_view{utf8} == name;
        ID3D12Device* device = nullptr;
        if (named &&
            SUCCEEDED(
                create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) &&
            device != nullptr) {
            D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture))))
                shares = architecture.UMA != FALSE;
            device->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return shares;
#else
    (void)name;
    return std::nullopt;
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

std::optional<std::string> environmentValue(const char* name)
{
#if defined(_WIN32)
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0)
        return std::nullopt;
    std::string value(needed, ' ');
    const DWORD length = GetEnvironmentVariableA(name, value.data(), needed);
    value.resize(length);
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
#endif
}

bool environmentFlag(const char* name) noexcept
{
#if defined(_WIN32)
    char value[8];
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    // A value too long for the buffer is longer than `0`, and is set.
    return length >= sizeof(value) || (length != 0 && !(length == 1 && value[0] == '0'));
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != 0 && !(value[0] == '0' && value[1] == 0);
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
    std::unique_ptr<ChildProcess> child(new ChildProcess(process));
    if (!options.onOutput)
        return child;
    SDL_IOStream* output = SDL_GetProcessOutput(process);
    if (output == nullptr)
        return child;
    const SDL_PropertiesID streamProps = SDL_GetIOProperties(output);
    auto reader = std::make_shared<Reader>();
    const std::function<void()> told = options.onOutput;
#if defined(_WIN32)
    auto* pipe = static_cast<HANDLE>(
        SDL_GetPointerProperty(streamProps, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, INVALID_HANDLE_VALUE));
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr)
        return child;
    DWORD waiting = PIPE_WAIT;
    if (SetNamedPipeHandleState(pipe, &waiting, nullptr, nullptr) == 0)
        return child;
    reader->thread = std::thread([pipe, reader = reader.get(), told]() {
        char buffer[4096];
        for (;;) {
            DWORD got = 0;
            if (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) == 0 || got == 0)
                break;
            {
                const std::lock_guard<std::mutex> guard(reader->lock);
                reader->text.append(buffer, got);
            }
            told();
        }
    });
#else
    const auto pipe =
        static_cast<int>(SDL_GetNumberProperty(streamProps, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
    if (pipe < 0)
        return child;
    const int flags = fcntl(pipe, F_GETFL);
    if (flags < 0 || fcntl(pipe, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return child;
    reader->thread = std::thread([pipe, reader = reader.get(), told]() {
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(pipe, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            {
                const std::lock_guard<std::mutex> guard(reader->lock);
                reader->text.append(buffer, static_cast<std::size_t>(got));
            }
            told();
        }
    });
#endif
    child->m_reader = std::move(reader);
    return child;
}

ChildProcess::~ChildProcess()
{
    kill();
    // The reader ends with the child's end of the pipe, and before SDL closes
    // this end under it.
    m_reader.reset();
    if (m_process != nullptr)
        SDL_DestroyProcess(static_cast<SDL_Process*>(m_process));
}

std::string ChildProcess::readAvailable()
{
    std::string text;
    if (m_reader != nullptr) {
        const std::lock_guard<std::mutex> guard(m_reader->lock);
        text.swap(m_reader->text);
        return text;
    }
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

bool ChildProcess::write(std::string_view text)
{
    if (m_process == nullptr || m_inputClosed)
        return false;
    SDL_IOStream* input = SDL_GetProcessInput(static_cast<SDL_Process*>(m_process));
    if (input == nullptr)
        return false;
    // Written whole: a line half sent is a message the other side cannot
    // read. A full pipe makes this wait, which a child that reads its input
    // never lets last.
    std::size_t sent = 0;
    while (sent < text.size()) {
        const std::size_t wrote = SDL_WriteIO(input, text.data() + sent, text.size() - sent);
        if (wrote == 0 && SDL_GetIOStatus(input) != SDL_IO_STATUS_NOT_READY)
            return false;
        sent += wrote;
    }
    (void)SDL_FlushIO(input);
    return true;
}

void ChildProcess::closeInput()
{
    if (m_process == nullptr || m_inputClosed)
        return;
    m_inputClosed = true;
    if (SDL_IOStream* input = SDL_GetProcessInput(static_cast<SDL_Process*>(m_process)); input != nullptr)
        (void)SDL_CloseIO(input);
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
