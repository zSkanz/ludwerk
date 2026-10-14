// Private GDK adapter. SDK headers and handles never enter the engine or Luau API.
// SDK headers require the Win32 calling-convention and HRESULT definitions first.
// clang-format off
#include <windows.h>
// clang-format on
#include <XAsync.h>
#include <XGameRuntimeInit.h>
#include <XTaskQueue.h>
#include <XUser.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <xsapi-c/services_c.h>

#include "engine/platform/game_integration_abi.h"
namespace {
std::mutex RuntimeMutex;
unsigned RuntimeUsers = 0;
std::string RuntimeScid;
struct Provider
{
    XTaskQueueHandle queue = nullptr;
    XUserHandle player = nullptr;
    XblContextHandle live = nullptr;
    XAsyncBlock async{};
    bool ready = false;
    bool pending = false;
    bool login = false;
    std::string id, name, failure;
};
std::string code(HRESULT result)
{
    if (result == E_ABORT)
        return "Canceled";
    char value[32]{};
    std::snprintf(value, sizeof(value), "NativeFailure:0x%08lX", static_cast<unsigned long>(result));
    return value;
}
void closePlayer(Provider& p)
{
    if (p.live)
        XblContextCloseHandle(p.live);
    if (p.player)
        XUserCloseHandle(p.player);
    p.live = nullptr;
    p.player = nullptr;
    p.id.clear();
    p.name.clear();
}
void* create(const char* scid)
{
    auto p = std::make_unique<Provider>();
    if (scid == nullptr || scid[0] == '\0')
        return p.release();
    const std::lock_guard lock(RuntimeMutex);
    if (RuntimeUsers != 0 && RuntimeScid != scid)
        return p.release();
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &p->queue)))
        return p.release();
    if (RuntimeUsers == 0) {
        if (FAILED(XGameRuntimeInitialize())) {
            XTaskQueueCloseHandle(p->queue);
            p->queue = nullptr;
            return p.release();
        }
        XblInitArgs args{};
        args.queue = nullptr;
        args.scid = scid;
        if (FAILED(XblInitialize(&args))) {
            XGameRuntimeUninitialize();
            XTaskQueueCloseHandle(p->queue);
            p->queue = nullptr;
            return p.release();
        }
        RuntimeScid = scid;
    }
    ++RuntimeUsers;
    p->ready = true;
    return p.release();
}
void destroy(void* state)
{
    std::unique_ptr<Provider> p(static_cast<Provider*>(state));
    if (p->pending) {
        XAsyncCancel(&p->async);
        (void)XAsyncGetStatus(&p->async, true);
    }
    closePlayer(*p);
    const std::lock_guard lock(RuntimeMutex);
    if (p->ready && --RuntimeUsers == 0) {
        XAsyncBlock cleanup{};
        cleanup.queue = p->queue;
        if (SUCCEEDED(XblCleanupAsync(&cleanup)))
            (void)XAsyncGetStatus(&cleanup, true);
        XGameRuntimeUninitialize();
        RuntimeScid.clear();
    }
    if (p->queue)
        XTaskQueueCloseHandle(p->queue);
}
bool available(void* state)
{
    return static_cast<Provider*>(state)->ready;
}
bool signedIn(void* state)
{
    auto& p = *static_cast<Provider*>(state);
    XUserState status{};
    return p.player && SUCCEEDED(XUserGetState(p.player, &status)) && status == XUserState::SignedIn;
}
const char* userId(void* state)
{
    return static_cast<Provider*>(state)->id.c_str();
}
const char* userName(void* state)
{
    return static_cast<Provider*>(state)->name.c_str();
}
const char* begin(void* state, const char* operation, const char* argument)
{
    auto& p = *static_cast<Provider*>(state);
    if (!p.ready)
        return "BackendUnavailable";
    if (p.pending)
        return "Busy";
    p.async = {};
    p.async.queue = p.queue;
    HRESULT result = E_NOTIMPL;
    p.login = std::strcmp(operation, "SignIn") == 0;
    if (p.login) {
        closePlayer(p);
        result = XUserAddAsync(XUserAddOptions::AddDefaultUserAllowingUI, &p.async);
    }
    else if (std::strcmp(operation, "UnlockAchievement") == 0) {
        if (!signedIn(state) || !p.live)
            return "NotSignedIn";
        uint64_t id = 0;
        result = XUserGetId(p.player, &id);
        if (SUCCEEDED(result))
            result = XblAchievementsUpdateAchievementAsync(p.live, id, argument, 100, &p.async);
    }
    else
        return "UnsupportedOperation";
    if (FAILED(result)) {
        p.failure = code(result);
        return p.failure.c_str();
    }
    p.pending = true;
    return "";
}
bool poll(void* state, bool* success, const char** failure)
{
    auto& p = *static_cast<Provider*>(state);
    if (!p.pending)
        return false;
    HRESULT result = XAsyncGetStatus(&p.async, false);
    if (result == E_PENDING)
        return false;
    p.pending = false;
    if (SUCCEEDED(result) && p.login) {
        result = XUserAddResult(&p.async, &p.player);
        if (SUCCEEDED(result))
            result = XblContextCreateHandle(p.player, &p.live);
        uint64_t id = 0;
        if (SUCCEEDED(result))
            result = XUserGetId(p.player, &id);
        char tag[XUserGamertagComponentUniqueModernMaxBytes]{};
        if (SUCCEEDED(result))
            result = XUserGetGamertag(p.player, XUserGamertagComponent::UniqueModern, sizeof(tag), tag, nullptr);
        if (SUCCEEDED(result)) {
            p.id = std::to_string(id);
            p.name = tag;
        }
        else
            closePlayer(p);
    }
    *success = SUCCEEDED(result);
    p.failure = *success ? "" : code(result);
    *failure = p.failure.c_str();
    return true;
}
const engine::platform::GameIntegrationApi Api{
    engine::platform::GameIntegrationAbi, create, destroy, available, signedIn, userId, userName, begin, poll};
} // namespace
extern "C" __declspec(dllexport) const engine::platform::GameIntegrationApi* engineGameIntegration()
{
    return &Api;
}
extern "C" __declspec(dllexport) const engine::platform::GameIntegrationCapabilitiesApi*
engineGameIntegrationCapabilities()
{
    static const engine::platform::GameIntegrationCapabilitiesApi capabilities{
        1,
        [](void*, const char* operation) {
            return std::strcmp(operation, "Identity") == 0 || std::strcmp(operation, "SignIn") == 0 ||
                   std::strcmp(operation, "UnlockAchievement") == 0;
        },
        [](void*) -> const char* { return ""; }};
    return &capabilities;
}
