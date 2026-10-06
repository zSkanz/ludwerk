#include "engine/platform/platform.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_touch.h>
#ifdef SDL_PLATFORM_ANDROID
#include <jni.h>
#endif
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/brand.h"
#include "engine/core/text_key.h"
#include "engine/platform/async_io.h"
#include "engine/platform/console.h"
#include "engine/platform/event.h"
#include "engine/platform/sdl_interop.h"
#include "window_impl.h"

#if defined(_WIN32)
// windows.h FIRST: psapi.h declares its functions with the Win32 typedefs and
// does not include them itself, so the other order is two hundred syntax errors
// inside a system header.
//
// `clang-format off` is what holds it. `IncludeBlocks: Regroup` merges blocks
// and sorts them, so a blank line does not survive and `psapi` sorts first --
// which is exactly the broken order, and the gate would reinstate it on every
// run.
// clang-format off
#include <windows.h>
#include <psapi.h>
// `SetCurrentProcessExplicitAppUserModelID` lives here, and shobjidl_core.h has
// the same ordering requirement psapi.h does.
#include <shobjidl_core.h>
#include <intrin.h>
// clang-format on
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace engine::platform {
namespace {

bool g_initialized = false;

// SDL_GetBasePath's result never changes for a process, and the derived paths
// are used per frame once shaders and content load, so they are resolved once.
Paths& pathsSlot()
{
    static Paths paths;
    return paths;
}

#ifdef SDL_PLATFORM_ANDROID
// **A packaged game, out of the APK and onto the filesystem.** The host reads
// its content and the game's project as ordinary files -- it walks
// `src/scripts`, mounts directories, stats what changed -- and an APK's assets
// are zip entries nothing but AAssetManager can open, with no directory
// listing. So a player APK carries `payload.txt`: a first line that names the
// build, then every file under `content/` and `game/`, one path a line. The
// first launch of a build copies them into the app's internal storage, and every
// launch after it finds the same first line there and copies nothing.
//
// False for an APK with no payload (the triangle sample reads its few assets in
// place), or when internal storage cannot be written; the caller then keeps the
// in-APK paths.
bool extractAndroidPayload(const std::filesystem::path& target)
{
    size_t size = 0;
    void* listed = SDL_LoadFile("payload.txt", &size);
    if (listed == nullptr)
        return false;
    const auto refuse = [](const std::string& what) {
        writeConsole(ConsoleStream::Err, std::string(core::kBrandShort) +
                                             ": the packaged game could not be extracted: " + what + " (" +
                                             std::string(SDL_GetError()) + ")\n");
        return false;
    };
    const std::string list(static_cast<const char*>(listed), size);
    SDL_free(listed);

    const std::size_t firstLine = list.find('\n');
    const std::string stamp = list.substr(0, firstLine);
    const std::filesystem::path stampFile = target / ".payload";
    std::error_code error;
    if (size_t had = 0; std::filesystem::exists(stampFile, error)) {
        if (void* old = SDL_LoadFile(stampFile.string().c_str(), &had); old != nullptr) {
            const bool same = std::string(static_cast<const char*>(old), had) == stamp;
            SDL_free(old);
            if (same)
                return true;
        }
    }

    // A different build: what the last one extracted goes, so a file the new
    // build dropped does not linger and get read.
    std::filesystem::remove_all(target / "content", error);
    std::filesystem::remove_all(target / "game", error);
    std::size_t start = firstLine == std::string::npos ? list.size() : firstLine + 1;
    while (start < list.size()) {
        std::size_t end = list.find('\n', start);
        if (end == std::string::npos)
            end = list.size();
        std::string relative = list.substr(start, end - start);
        start = end + 1;
        if (!relative.empty() && relative.back() == '\r')
            relative.pop_back();
        if (relative.empty())
            continue;
        size_t bytes = 0;
        void* data = SDL_LoadFile(relative.c_str(), &bytes);
        if (data == nullptr)
            return refuse("reading " + relative);
        const std::filesystem::path out = target / std::filesystem::path(relative);
        std::filesystem::create_directories(out.parent_path(), error);
        const bool written = SDL_SaveFile(out.string().c_str(), data, bytes);
        SDL_free(data);
        if (!written)
            return refuse("writing " + out.string());
    }
    if (!SDL_SaveFile(stampFile.string().c_str(), stamp.data(), stamp.size()))
        return refuse("writing " + stampFile.string());
    return true;
}
#endif

void resolvePaths()
{
    Paths& paths = pathsSlot();

#ifdef SDL_PLATFORM_ANDROID
    // A player APK's content, extracted to the app's own storage: from there on
    // everything is an ordinary file, and the host runs as it does on a desktop
    // -- `executableDir/game` is the packaged game.
    if (const char* external = SDL_GetAndroidExternalStoragePath(); external != nullptr && external[0] != 0) {
        paths.reportDir = std::filesystem::path(external);
        std::error_code error;
        std::filesystem::create_directories(paths.reportDir, error);
    }
    if (const char* internal = SDL_GetAndroidInternalStoragePath(); internal != nullptr) {
        const std::filesystem::path home = std::filesystem::path(internal) / "payload";
        if (extractAndroidPayload(home)) {
            paths.executableDir = home;
            paths.contentDir = home / "content";
            paths.userDir = std::filesystem::path(internal) / "user";
            std::error_code error;
            std::filesystem::create_directories(paths.userDir, error);
            return;
        }
    }

    // An APK has no executable directory and its assets are not a filesystem:
    // they are zip entries the package manager serves through AAssetManager.
    // SDL reaches them by opening a *relative* path -- an absolute one is sent
    // to the C runtime instead and finds nothing -- so the content directory is
    // deliberately the bare relative name that matches the Gradle project's
    // `assets/content/` tree, and everything that reads it must go through
    // platform::readFile (file.h).
    //
    // SDL_GetBasePath() answers "./" here. That is not used: the "./" prefix
    // survives into the lookup key and AAssetManager indexes exact names.
    paths.executableDir = std::filesystem::path(".");
    paths.contentDir = std::filesystem::path("content");
#else
    // Null on platforms that do not implement it; the working directory is the
    // same fallback the M0 host used, and is what CTest provides.
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        paths.executableDir = std::filesystem::path(base);
    }
    else {
        std::error_code ec;
        paths.executableDir = std::filesystem::current_path(ec);
    }

    paths.contentDir = paths.executableDir / "content";

    // **Per user and per machine, and it may legitimately be nowhere.** SDL
    // creates the directory as a side effect of answering, so this is also the
    // only place it gets made; a platform that has no such location answers null
    // and every caller keeps its state in memory for one session instead.
    if (char* pref = SDL_GetPrefPath(std::string(core::kBrandName).c_str(), std::string(core::kBrandName).c_str());
        pref != nullptr) {
        paths.userDir = std::filesystem::path(pref);
        SDL_free(pref);
    }

    // **Not derived from the one above, because they are different questions.**
    // `userDir` is where an application hides its own state; this is where a
    // person keeps their work, and offering to create somebody's game inside
    // `AppData` is offering to put it where they will never find it. SDL owns
    // this string and it is not freed.
    if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); documents != nullptr)
        paths.documentsDir = std::filesystem::path(documents);
#endif
}

} // namespace

std::optional<core::EngineError> init(const InitOptions& options)
{
    if (g_initialized)
        return std::nullopt;

    if (options.headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");

    // **A phone's back button is the game's** (ADR 0170). Left to the system
    // it finishes the activity: the app closed under a player who meant to
    // close a sheet. Asked for as a key instead, it arrives as `Escape`
    // (`translateScancode`), and a game leaves by `game:Shutdown()`. Android
    // alone reads the hint.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    // SDL_INIT_VIDEO implies SDL_INIT_EVENTS, so the window and the event pump
    // come up together -- which is the only combination this module offers.
    //
    // SDL_INIT_GAMEPAD is separate and is asked for as a SECOND call rather than
    // ORed into the first, because a machine with no controller subsystem --
    // which is every CI container -- must still get a window. A failure here is
    // logged by SDL and ignored: the engine runs, and no gamepad event arrives,
    // which is exactly what having no gamepad means.
    if (!SDL_Init(SDL_INIT_VIDEO))
        return core::makeError(ENG_TR("platform.err.init_failed"), {}, SDL_GetError());
    // **Gamepads are not started here** (D476; see `InitOptions::gamepads`):
    // starting the library's joystick subsystem costs about 95 ms on Windows
    // -- measured, and not a cost any one of its drivers' hints removes. A run
    // with hands starts it from the event pump, after its first frame; a run
    // without -- a dedicated server, a conformance run, a tool, a test --
    // never does. A virtual gamepad (`attachVirtualGamepad`) starts it for
    // itself.
    setGamepadsWanted(options.gamepads.value_or(!options.headless));

    resolvePaths();
    g_initialized = true;
    return std::nullopt;
}

void shutdown()
{
    if (!g_initialized)
        return;

    // BEFORE `SDL_Quit`, and it has to be: the async IO service holds an SDL
    // queue it must drain and destroy, and a submitter thread it must join.
    // Leaving that to the static destructor means joining a thread after SDL is
    // gone -- and, if nothing joins it at all, a joinable `std::thread` destroyed
    // at exit, which is `std::terminate` and reads as a crash.
    shutdownIo();

    abandonHeldPump();
    SDL_Quit();
    g_initialized = false;
}

bool isInitialized() noexcept
{
    return g_initialized;
}

u64 nowNs() noexcept
{
    // Deliberately not SDL_GetTicksNS: SDL's tick clock counts from SDL library
    // initialization and reads 0 before it, so anything measuring startup --
    // or any test that runs before a window exists -- would silently get zero
    // and a first frame of impossible duration. steady_clock is monotonic by
    // standard guarantee, needs no bring-up, and has the same resolution on
    // every platform we target.
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
}

void sleepNs(u64 ns) noexcept
{
    if (ns > 0)
        SDL_DelayPrecise(ns);
}

core::i64 threadCpuNs() noexcept
{
#if defined(_WIN32) && defined(_M_X64)
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles) == 0)
        return -1;
    // The thread's cycles are counted in timestamp-counter ticks, which run at
    // a constant rate; that rate is read off the wall clock since the first
    // call rather than off a table nobody keeps.
    static const u64 wallStart = nowNs();
    static const u64 ticksStart = __rdtsc();
    const u64 wall = nowNs() - wallStart;
    const u64 ticks = __rdtsc() - ticksStart;
    if (wall < 1'000'000 || ticks == 0)
        return -1;
    return static_cast<core::i64>(static_cast<double>(cycles) * static_cast<double>(wall) / static_cast<double>(ticks));
#elif defined(_WIN32)
    return -1;
#else
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
        return -1;
    return static_cast<core::i64>(now.tv_sec) * 1'000'000'000 + static_cast<core::i64>(now.tv_nsec);
#endif
}

u64 residentBytes() noexcept
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return static_cast<u64>(counters.WorkingSetSize);
#elif defined(__linux__)
    // `statm` rather than `status`: two integers on one line, in pages, with
    // no parsing beyond the second field. `status`'s VmRSS is the same number
    // behind a line search and a unit suffix.
    std::FILE* const statm = std::fopen("/proc/self/statm", "rb");
    if (statm == nullptr) {
        return 0;
    }
    unsigned long long total = 0;
    unsigned long long resident = 0;
    const int read = std::fscanf(statm, "%llu %llu", &total, &resident);
    (void)std::fclose(statm);
    if (read != 2) {
        return 0;
    }
    return static_cast<u64>(resident) * static_cast<u64>(::sysconf(_SC_PAGESIZE));
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return static_cast<u64>(info.resident_size);
#else
    return 0;
#endif
}

bool raiseProcessPriority() noexcept
{
#if defined(_WIN32)
    // ABOVE_NORMAL and not HIGH: enough to stop a busy desktop descheduling a
    // frame, and short of the class that starves the desktop's own input.
    return ::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
#else
    return false;
#endif
}

std::vector<std::byte> applicationIconBytes()
{
#if defined(_WIN32)
    // Ordinal 1, which is what `app.rc` names and what `ludwerk build` replaces.
    // The GROUP is what an executable's icon actually is: a directory of sizes,
    // each naming an `RT_ICON` of its own.
    const HRSRC groupHandle = ::FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14 /* RT_GROUP_ICON */));
    if (groupHandle == nullptr)
        return {};
    const HGLOBAL group = ::LoadResource(nullptr, groupHandle);
    if (group == nullptr)
        return {};
    const auto* directory = static_cast<const unsigned char*>(::LockResource(group));
    if (directory == nullptr)
        return {};

    const auto count = static_cast<u32>(directory[4]) | (static_cast<u32>(directory[5]) << 8);
    if (count == 0)
        return {};

    // The LARGEST entry, because this is a window icon and the compositor
    // downscales. A group directory entry is 14 bytes: width, height, colours,
    // reserved, planes, bit count, byte count, and the id of its `RT_ICON`.
    u32 bestId = 0;
    u32 bestPixels = 0;
    for (u32 index = 0; index < count; ++index) {
        const unsigned char* entry = directory + 6 + static_cast<std::size_t>(index) * 14u;
        // Zero means 256 in an icon directory, which is the size that matters
        // most here and the one a naive read discards.
        const u32 width = entry[0] == 0 ? 256u : entry[0];
        const u32 height = entry[1] == 0 ? 256u : entry[1];
        const u32 id = static_cast<u32>(entry[12]) | (static_cast<u32>(entry[13]) << 8);
        if (width * height > bestPixels) {
            bestPixels = width * height;
            bestId = id;
        }
    }
    if (bestId == 0)
        return {};

    const HRSRC iconHandle = ::FindResourceW(nullptr, MAKEINTRESOURCEW(bestId), MAKEINTRESOURCEW(3 /* RT_ICON */));
    if (iconHandle == nullptr)
        return {};
    const DWORD size = ::SizeofResource(nullptr, iconHandle);
    const HGLOBAL icon = ::LoadResource(nullptr, iconHandle);
    if (icon == nullptr || size == 0)
        return {};
    const auto* bytes = static_cast<const std::byte*>(::LockResource(icon));
    if (bytes == nullptr)
        return {};

    return std::vector<std::byte>(bytes, bytes + size);
#else
    return {};
#endif
}

void setApplicationId([[maybe_unused]] std::string_view id)
{
#if defined(_WIN32)
    if (id.empty())
        return;

    // The API takes UTF-16 and an id is ASCII reverse-DNS, so the widening is
    // the whole conversion. Failure is ignored on purpose: the shell has
    // already decided this process's identity by the time a window exists, and
    // a game that cannot set it still runs.
    std::wstring wide;
    wide.reserve(id.size());
    for (const char c : id)
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    (void)::SetCurrentProcessExplicitAppUserModelID(wide.c_str());
#endif
}

void* androidJavaEnv()
{
#ifdef SDL_PLATFORM_ANDROID
    // SDL attaches a thread it has not seen, and detaches it when it ends.
    return SDL_GetAndroidJNIEnv();
#else
    return nullptr;
#endif
}

void requestDisplayFrameRate(float hz) noexcept
{
#ifdef SDL_PLATFORM_ANDROID
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr)
        return;
    // A local reference of the caller's, given back below.
    auto* activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr)
        return;
    if (jclass type = env->GetObjectClass(activity); type != nullptr) {
        // Looked up each time: it is asked a few times a minute at most, and a
        // method id kept across an activity's recreation is a way to crash.
        if (jmethodID method = env->GetMethodID(type, "requestFrameRate", "(F)V"); method != nullptr)
            // A float through the ellipsis travels as a double, and the
            // virtual machine reads it back as the `F` the signature names.
            env->CallVoidMethod(activity, method, static_cast<jdouble>(hz));
        env->DeleteLocalRef(type);
    }
    // An activity without the method, or one that threw: no answer, and the
    // exception must not be left pending for the next call into Java.
    if (env->ExceptionCheck() == JNI_TRUE)
        env->ExceptionClear();
    env->DeleteLocalRef(activity);
#else
    (void)hz;
#endif
}

const Paths& paths()
{
    // Resolvable without a video subsystem, so a tool or a test that only wants
    // to find content does not have to bring up a window stack first.
    if (pathsSlot().executableDir.empty())
        resolvePaths();

    return pathsSlot();
}

bool startDetached(const std::vector<std::string>& args)
{
    if (args.empty())
        return false;

    // SDL wants a null-terminated array of C strings and keeps nothing, so the
    // pointers only have to outlive the call.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& argument : args)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);

    // `false`: the child inherits this process's streams rather than getting
    // pipes nobody is reading. A detached editor whose output went into a pipe
    // with no reader would block the first time it logged enough to fill it.
    SDL_Process* process = SDL_CreateProcess(argv.data(), false);
    if (process == nullptr)
        return false;

    // Destroying the handle does not kill the child -- SDL documents this as
    // releasing our side of it -- which is exactly what "detached" means here.
    SDL_DestroyProcess(process);
    return true;
}

InputDevices inputDevices() noexcept
{
    InputDevices devices;
    if (!isInitialized())
        return devices;
    int touches = 0;
    if (SDL_TouchID* ids = SDL_GetTouchDevices(&touches); ids != nullptr) {
        // A touchscreen, not a laptop's trackpad: an indirect device moves a
        // pointer and has nothing under the finger to press.
        for (int at = 0; at < touches; ++at)
            devices.touch = devices.touch || SDL_GetTouchDeviceType(ids[at]) == SDL_TOUCH_DEVICE_DIRECT;
        SDL_free(ids);
    }
#if defined(SDL_PLATFORM_ANDROID)
    // A phone IS one, whether or not a finger has yet said so: the system
    // lists the screen only once it has been touched on some devices.
    devices.touch = true;
#endif
    devices.keyboard = SDL_HasKeyboard();
    devices.gamepad = SDL_HasGamepad();
    return devices;
}

void setScreenOrientation([[maybe_unused]] Window& window, [[maybe_unused]] int orientation)
{
#if defined(SDL_PLATFORM_ANDROID)
    // SDL's names, which it maps onto the activity's requested orientation
    // (SDLActivity.setOrientationBis) -- for a resizable window, one landscape
    // alone is fixed and both are the sensor landscape.
    static constexpr const char* Hints[] = {
        "LandscapeLeft",
        "LandscapeRight",
        "LandscapeLeft LandscapeRight",
        "Portrait",
        "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown",
    };
    if (orientation < 0 || orientation > 4)
        return;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, Hints[orientation]);
    // SDL reads the hint when a window is made or its resizability changes,
    // and a change to the same value is not one -- so it is flipped and put
    // back, which is two requests and the last one is the one that lands.
    SDL_Window* native = nativeWindow(window);
    SDL_SetWindowResizable(native, false);
    SDL_SetWindowResizable(native, true);
#endif
}

int askChoice(Window* window, std::string_view title, std::string_view message, const std::vector<std::string>& buttons)
{
    std::vector<SDL_MessageBoxButtonData> native;
    native.reserve(buttons.size());
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        native.push_back(SDL_MessageBoxButtonData{
            .flags = index == 0 ? static_cast<SDL_MessageBoxButtonFlags>(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) : 0u,
            .buttonID = static_cast<int>(index),
            .text = buttons[index].c_str(),
        });
    }
    const std::string titleText(title);
    const std::string messageText(message);
    const SDL_MessageBoxData data{
        .flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT,
        .window = window != nullptr ? nativeWindow(*window) : nullptr,
        .title = titleText.c_str(),
        .message = messageText.c_str(),
        .numbuttons = static_cast<int>(native.size()),
        .buttons = native.data(),
        .colorScheme = nullptr,
    };
    int chosen = -1;
    if (!SDL_ShowMessageBox(&data, &chosen))
        return -1;
    return chosen;
}

bool canPickFolder()
{
#if defined(SDL_PLATFORM_ANDROID)
    return false;
#else
    // There is no compile-time symbol that says whether SDL was built with its
    // dialog subsystem, and asking at runtime is the honest form of the question
    // anyway: on Linux the picker is a portal or a helper program that may
    // simply not be installed. What this can say is that the build has the
    // entry point; a refusal at show time is reported through the callback.
    return true;
#endif
}

namespace {

// The callback SDL hands the result to, and the C++ one it forwards to. One at a
// time on purpose: two folder pickers open at once is not a state any of this
// has to represent, and the second `pickFolder` replaces the first's callback
// rather than leaking it.
std::function<void(std::filesystem::path)>& folderCallback()
{
    static std::function<void(std::filesystem::path)> callback;
    return callback;
}

void SDLCALL onFolderChosen([[maybe_unused]] void* userdata, const char* const* files, [[maybe_unused]] int filter)
{
    std::function<void(std::filesystem::path)> callback;
    callback.swap(folderCallback());
    if (!callback)
        return;

    // Null means the dialog failed; an empty first entry means it was
    // cancelled. Both are "no folder", which is what the caller asked about.
    if (files == nullptr || files[0] == nullptr) {
        callback(std::filesystem::path{});
        return;
    }
    callback(std::filesystem::path(files[0]));
}

} // namespace

void pickFolder(Window& window, std::string_view startIn, std::function<void(std::filesystem::path)> done)
{
    folderCallback() = std::move(done);

    const std::string start(startIn);
    SDL_ShowOpenFolderDialog(&onFolderChosen, nullptr, nativeWindow(window), start.empty() ? nullptr : start.c_str(),
                             false);
}

namespace {

// The C++ callback the file dialog forwards to. One at a time, like the folder
// picker's and for the same reason.
std::function<void(std::vector<std::filesystem::path>)>& fileCallback()
{
    static std::function<void(std::vector<std::filesystem::path>)> callback;
    return callback;
}

void SDLCALL onFilesChosen([[maybe_unused]] void* userdata, const char* const* files, [[maybe_unused]] int filter)
{
    std::function<void(std::vector<std::filesystem::path>)> callback;
    callback.swap(fileCallback());
    if (!callback)
        return;

    std::vector<std::filesystem::path> chosen;
    // Null means the dialog failed and an empty list means it was cancelled.
    // Both are "nothing was picked", which is what the caller asked about.
    for (const char* const* file = files; file != nullptr && *file != nullptr; ++file)
        chosen.emplace_back(*file);
    callback(std::move(chosen));
}

} // namespace

void pickFiles(Window& window, std::string_view startIn, bool allowMany,
               std::function<void(std::vector<std::filesystem::path>)> done)
{
    fileCallback() = std::move(done);

    const std::string start(startIn);
    SDL_ShowOpenFileDialog(&onFilesChosen, nullptr, nativeWindow(window), nullptr, 0,
                           start.empty() ? nullptr : start.c_str(), allowMany);
}

std::vector<std::string> preferredLocales()
{
    std::vector<std::string> names;
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (locales == nullptr)
        return names;
    for (int index = 0; index < count; ++index) {
        if (locales[index] == nullptr || locales[index]->language == nullptr)
            continue;
        std::string name = locales[index]->language;
        if (locales[index]->country != nullptr && locales[index]->country[0] != '\0') {
            name += '-';
            name += locales[index]->country;
        }
        names.push_back(std::move(name));
    }
    SDL_free(locales);
    return names;
}

} // namespace engine::platform
