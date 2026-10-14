#include "engine/script/optional_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>

#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/script/binding.h"
#include "engine/script/json_module.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"
namespace engine::script {
namespace {
bool enabled(lua_State* L, std::string_view id)
{
    const auto& values = context(L).world->engineState().enabledIntegrations;
    return std::find(values.begin(), values.end(), id) != values.end();
}
platform::GameIntegration* provider(lua_State* L)
{
    const auto self = checkInstance(L, 1);
    const auto* descriptor = context(L).world->classes().find(context(L).world->classOf(self));
    warnOptionalIntegration(L, descriptor->integration, context(L).world->atoms().text(descriptor->name));
    if (!enabled(L, descriptor->integration))
        return nullptr;
    return integrationProvider(L, descriptor->integration);
}
int isAvailable(lua_State* L)
{
    auto* native = provider(L);
    lua_pushboolean(L, native != nullptr && native->available());
    return 1;
}
int isSignedIn(lua_State* L)
{
    auto* native = provider(L);
    lua_pushboolean(L, native != nullptr && native->signedIn());
    return 1;
}
int userValue(lua_State* L, bool name)
{
    auto* native = provider(L);
    if (native == nullptr || !native->signedIn()) {
        lua_pushnil(L);
        return 1;
    }
    const auto user = native->user();
    const std::string& value = name ? user.name : user.id;
    lua_pushlstring(L, value.data(), value.size());
    return 1;
}
int userId(lua_State* L)
{
    return userValue(L, false);
}
int gamertag(lua_State* L)
{
    return userValue(L, true);
}
int failure(lua_State* L, const char* code, IntegrationResponse response = IntegrationResponse::Action)
{
    if (response == IntegrationResponse::Action)
        lua_pushboolean(L, false);
    else
        lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}
std::string genericFailure(lua_State* L, std::string_view id, std::string reason)
{
    if (reason == "UnsupportedOperation")
        return "NotSupported";
    if (!reason.starts_with("NativeFailure:"))
        return reason;
    if (context(L).services->scenes.developer) {
        const core::I18nArg args[] = {{"integration", id}, {"error", reason}};
        core::log(core::LogLevel::Warn, ENG_TR("script.warn.integration_native_error"), args);
    }
    return "ProviderError";
}
int action(lua_State* L, std::string_view operation, std::string_view argument)
{
    (void)provider(L);
    const auto self = checkInstance(L, 1);
    const auto* descriptor = context(L).world->classes().find(context(L).world->classOf(self));
    return beginIntegrationAction(L, descriptor->integration, operation, argument);
}
int signIn(lua_State* L)
{
    return action(L, "SignIn", "");
}
int achievement(lua_State* L)
{
    size_t length = 0;
    const char* id = luaL_checklstring(L, 2, &length);
    if (length == 0 || length > 128 || std::string_view(id, length).find('\0') != std::string_view::npos)
        return failure(L, "InvalidArgument");
    return action(L, "UnlockAchievement", std::string_view(id, length));
}
const InstanceMethodBinding Methods[] = {
    {"XboxService", "IsAvailable", isAvailable}, {"XboxService", "IsSignedIn", isSignedIn},
    {"XboxService", "GetUserId", userId},        {"XboxService", "GetGamertag", gamertag},
    {"XboxService", "SignInAsync", signIn},      {"XboxService", "UnlockAchievementAsync", achievement},
};
} // namespace
platform::GameIntegration* integrationProvider(lua_State* L, std::string_view id)
{
    if (!enabled(L, id))
        return nullptr;
    auto& state = context(L).services->integrations[std::string(id)];
    if (!state.attempted) {
        state.attempted = true;
        if (!state.library.empty())
            state.provider = platform::loadGameIntegration(state.library, state.configuration);
    }
    return state.provider.get();
}
int beginIntegrationAction(lua_State* L, std::string_view id, std::string_view operation, std::string_view argument,
                           IntegrationResponse response, bool generic)
{
    if (!enabled(L, id))
        return failure(L, "IntegrationDisabled", response);
    auto* native = integrationProvider(L, id);
    if (!native || !native->available())
        return failure(L, "BackendUnavailable", response);
    auto& state = context(L).services->integrations[std::string(id)];
    if (state.waiter != -1)
        return failure(L, "Busy", response);
    if (!lua_isyieldable(L))
        return failure(L, "NotYieldable", response);
    const std::string error = native->begin(operation, argument);
    if (!error.empty())
        return failure(L, (generic ? genericFailure(L, id, error) : error).c_str(), response);
    lua_pushthread(L);
    state.waiter = lua_ref(L, -1);
    state.response = response;
    state.generic = generic;
    lua_pop(L, 1);
    return lua_yield(L, 0);
}
void warnOptionalIntegration(lua_State* L, std::string_view integration, std::string_view service)
{
    if (integration.empty() || enabled(L, integration) || !context(L).services->scenes.developer)
        return;
    auto& state = context(L).services->integrations[std::string(integration)];
    if (state.warned)
        return;
    state.warned = true;
    const core::I18nArg args[] = {{"service", service}, {"integration", integration}};
    core::log(core::LogLevel::Warn, ENG_TR("script.warn.integration_disabled"), args);
}
std::span<const InstanceMethodBinding> optionalServiceMethods()
{
    return Methods;
}
void resumeIntegrationWaiters(lua_State* L)
{
    struct Completed
    {
        int reference;
        bool success;
        std::string reason;
        std::string payload;
        IntegrationResponse response;
    };
    std::vector<Completed> completed;
    for (auto& [id, state] : context(L).services->integrations) {
        (void)id;
        if (state.waiter == -1 || !state.provider)
            continue;
        bool success = false;
        std::string reason;
        if (!state.provider->poll(success, reason))
            continue;
        if (!success && state.generic)
            reason = genericFailure(L, id, std::move(reason));
        std::string payload =
            success && state.response != IntegrationResponse::Action ? state.provider->response() : "";
        completed.push_back({state.waiter, success, std::move(reason), std::move(payload), state.response});
        state.waiter = -1;
    }
    // Resumed scripts may create another integration and rehash the state map.
    for (const auto& result : completed) {
        lua_getref(L, result.reference);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr) {
            bool success = result.success;
            if (result.response == IntegrationResponse::Action)
                lua_pushboolean(co, success);
            else if (success) {
                core::JsonDocument document;
                success = result.payload.size() <= 1024 * 1024 && static_cast<bool>(document.parse(result.payload));
                if (success) {
                    const auto kind = document.root().type();
                    success = (result.response == IntegrationResponse::Number && kind == core::JsonType::Number) ||
                              (result.response == IntegrationResponse::String && kind == core::JsonType::String) ||
                              (result.response == IntegrationResponse::Boolean && kind == core::JsonType::Boolean) ||
                              (result.response == IntegrationResponse::Table &&
                               (kind == core::JsonType::Object || kind == core::JsonType::Array));
                    if (result.response == IntegrationResponse::Product) {
                        const auto root = document.root();
                        const auto optionalString = [&](std::string_view key) {
                            return !root.has(key) || root[key].isNull() || root[key].type() == core::JsonType::String;
                        };
                        success = kind == core::JsonType::Object && root["Id"].type() == core::JsonType::String &&
                                  root["DisplayName"].type() == core::JsonType::String &&
                                  root["Description"].type() == core::JsonType::String && optionalString("Price") &&
                                  optionalString("Currency");
                    }
                    if (result.response == IntegrationResponse::LeaderboardEntries ||
                        result.response == IntegrationResponse::UserList) {
                        const auto root = document.root();
                        success = kind == core::JsonType::Array &&
                                  (result.response == IntegrationResponse::UserList || root.size() <= 100);
                        for (core::usize index = 0; success && index < root.size(); ++index) {
                            const auto row = root.at(index);
                            const auto id = row[result.response == IntegrationResponse::UserList ? "Id" : "UserId"];
                            success = row.type() == core::JsonType::Object && id.type() == core::JsonType::String &&
                                      id.asString().find(':') != std::string_view::npos &&
                                      row["DisplayName"].type() == core::JsonType::String;
                            if (result.response == IntegrationResponse::LeaderboardEntries)
                                success = success && row["Rank"].type() == core::JsonType::Number &&
                                          row["Rank"].asNumber() >= 1.0 &&
                                          row["Score"].type() == core::JsonType::Number;
                        }
                    }
                    if (result.response == IntegrationResponse::Presence) {
                        const auto root = document.root();
                        success = kind == core::JsonType::Object &&
                                  root["IsOnline"].type() == core::JsonType::Boolean &&
                                  (!root.has("Status") || root["Status"].isNull() ||
                                   root["Status"].type() == core::JsonType::String);
                    }
                }
                if (success)
                    deserializeJsonValue(co, result.payload, true);
                else
                    lua_pushnil(co);
            }
            else
                lua_pushnil(co);
            if (success)
                lua_pushnil(co);
            else
                lua_pushstring(co, result.success ? "InvalidProviderResponse" : result.reason.c_str());
            (void)resumeScheduled(L, co, 2);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, result.reference);
    }
}
} // namespace engine::script
