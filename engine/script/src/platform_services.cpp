#include "engine/script/platform_services.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>

#include "engine/script/binding.h"
#include "engine/script/json_module.h"
#include "engine/script/optional_service.h"
#include "engine/script/services.h"

namespace engine::script {
namespace {
struct Selection
{
    std::string_view id;
    platform::GameIntegration* provider = nullptr;
    const char* failure = "IntegrationUnavailable";
};
Selection select(lua_State* L, std::string_view service, std::string_view operation)
{
    auto& state = *context(L).services;
    const auto choice = state.platformServices.providers.find(service);
    const std::string_view selected =
        choice == state.platformServices.providers.end() ? std::string_view("Auto") : std::string_view(choice->second);
    const bool automatic = selected == "Auto";
    const auto& enabled = context(L).world->engineState().enabledIntegrations;
    Selection result;
    for (const auto& [id, entry] : state.integrations) {
        (void)entry;
        if ((!automatic && selected != id) || std::find(enabled.begin(), enabled.end(), id) == enabled.end())
            continue;
        result.failure = "ProviderUnavailable";
        auto* native = integrationProvider(L, id);
        if (!native || !native->available())
            continue;
        result.failure = "NotSupported";
        if (native->supports(operation))
            return {id, native, nullptr};
    }
    if (!automatic && result.failure == std::string_view("IntegrationUnavailable") && !enabled.empty())
        result.failure = "ProviderUnavailable";
    return result;
}
int failed(lua_State* L, const char* code, IntegrationResponse response = IntegrationResponse::Action)
{
    if (response == IntegrationResponse::Action)
        lua_pushboolean(L, false);
    else
        lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}
int identityAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "identity", "Identity").provider != nullptr);
    return 1;
}
int identitySignedIn(lua_State* L)
{
    const auto selected = select(L, "identity", "Identity");
    lua_pushboolean(L, selected.provider && selected.provider->signedIn());
    return 1;
}
int localUser(lua_State* L)
{
    const auto selected = select(L, "identity", "Identity");
    if (!selected.provider || !selected.provider->signedIn()) {
        lua_pushnil(L);
        return 1;
    }
    const auto user = selected.provider->user();
    const std::string id = std::string(selected.id) + ":" + user.id;
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "Id");
    lua_pushlstring(L, user.name.data(), user.name.size());
    lua_setfield(L, -2, "DisplayName");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "IsSignedIn");
    lua_pushlstring(L, selected.id.data(), selected.id.size());
    lua_setfield(L, -2, "Platform");
    lua_setreadonly(L, -1, true);
    return 1;
}
int signIn(lua_State* L)
{
    const auto selected = select(L, "identity", "SignIn");
    if (!selected.provider)
        return failed(L, selected.failure);
    return beginIntegrationAction(L, selected.id, "SignIn", "", IntegrationResponse::Action, true);
}
int achievementAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "achievements", "UnlockAchievement").provider != nullptr);
    return 1;
}
int achievementSupports(lua_State* L)
{
    const std::string_view feature = luaL_checkstring(L, 2);
    const std::string_view operation = feature == "Unlock"        ? "UnlockAchievement"
                                       : feature == "GetProgress" ? "GetAchievementProgress"
                                       : feature == "SetProgress" ? "SetAchievementProgress"
                                                                  : "";
    lua_pushboolean(L, !operation.empty() && select(L, "achievements", operation).provider != nullptr);
    return 1;
}
bool validIdentifier(std::string_view id)
{
    return !id.empty() && id.size() <= 128 && id.find('\0') == std::string_view::npos;
}
int achievementOperation(lua_State* L, std::string_view operation, IntegrationResponse response)
{
    size_t length = 0;
    const char* id = luaL_checklstring(L, 2, &length);
    const std::string_view logicalId(id, length);
    if (!validIdentifier(logicalId))
        return failed(L, "InvalidArgument", response);
    const auto selected = select(L, "achievements", operation);
    if (!selected.provider)
        return failed(L, selected.failure, response);
    const auto& maps = context(L).services->platformServices.ids;
    const auto mapping = maps.find("achievements/" + std::string(selected.id));
    if (mapping == maps.end())
        return failed(L, "UnknownAchievement", response);
    const auto nativeId = mapping->second.find(logicalId);
    if (nativeId == mapping->second.end() || !validIdentifier(nativeId->second))
        return failed(L, "UnknownAchievement", response);
    std::string argument = nativeId->second;
    if (operation != "UnlockAchievement") {
        lua_createtable(L, 0, 2);
        markJsonObject(L);
        lua_pushlstring(L, argument.data(), argument.size());
        lua_setfield(L, -2, "id");
        if (operation == "SetAchievementProgress") {
            const double progress = luaL_checknumber(L, 3);
            if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
                lua_pop(L, 1);
                return failed(L, "InvalidArgument", response);
            }
            lua_pushnumber(L, progress);
            lua_setfield(L, -2, "progress");
        }
        argument = serializeJsonValue(L, -1);
        lua_pop(L, 1);
    }
    return beginIntegrationAction(L, selected.id, operation, argument, response, true);
}
int unlock(lua_State* L)
{
    return achievementOperation(L, "UnlockAchievement", IntegrationResponse::Action);
}
int getProgress(lua_State* L)
{
    return achievementOperation(L, "GetAchievementProgress", IntegrationResponse::Number);
}
int setProgress(lua_State* L)
{
    return achievementOperation(L, "SetAchievementProgress", IntegrationResponse::Action);
}
int storeAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "store", "GetProduct").provider != nullptr);
    return 1;
}
int storeSupports(lua_State* L)
{
    const std::string_view feature = luaL_checkstring(L, 2);
    const std::string_view operation = feature == "GetProduct"            ? "GetProduct"
                                       : feature == "Purchase"            ? "Purchase"
                                       : feature == "Owns"                ? "OwnsProduct"
                                       : feature == "RefreshEntitlements" ? "RefreshEntitlements"
                                                                          : "";
    lua_pushboolean(L, !operation.empty() && select(L, "store", operation).provider != nullptr);
    return 1;
}
// All new operations use the same small JSON request contract. Only legacy
// SignIn/Unlock retain the original ABI argument convention.
enum class RequestExtras
{
    None,
    Data,
    Score,
    Count
};
int mappedRequest(lua_State* L, std::string_view service, std::string_view operation, IntegrationResponse response,
                  bool mappedId, bool hasId = true, RequestExtras extras = RequestExtras::None)
{
    size_t length = 0;
    const char* text = hasId ? luaL_checklstring(L, 2, &length) : "";
    const std::string_view logicalId(text, length);
    if (hasId && !validIdentifier(logicalId))
        return failed(L, "InvalidArgument", response);
    const auto selected = select(L, service, operation);
    if (!selected.provider)
        return failed(L, selected.failure, response);
    std::string nativeId(logicalId);
    if (service == "social" && hasId) {
        const std::string prefix = std::string(selected.id) + ":";
        if (!logicalId.starts_with(prefix) || logicalId.size() == prefix.size())
            return failed(L, "InvalidUser", response);
        nativeId = logicalId.substr(prefix.size());
    }
    if (mappedId) {
        const auto& maps = context(L).services->platformServices.ids;
        const auto mapping = maps.find(std::string(service) + "/" + std::string(selected.id));
        if (mapping == maps.end())
            return failed(L, "UnknownId", response);
        const auto found = mapping->second.find(logicalId);
        if (found == mapping->second.end() || !validIdentifier(found->second))
            return failed(L, "UnknownId", response);
        nativeId = found->second;
    }
    lua_createtable(L, 0, 3);
    markJsonObject(L);
    if (hasId) {
        lua_pushlstring(L, nativeId.data(), nativeId.size());
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, logicalId.data(), logicalId.size());
        lua_setfield(L, -2, "logicalId");
    }
    if (extras == RequestExtras::Data) {
        size_t dataLength = 0;
        const char* data = luaL_checklstring(L, 3, &dataLength);
        if (dataLength > 1024 * 1024) {
            lua_pop(L, 1);
            return failed(L, "QuotaExceeded", response);
        }
        lua_pushlstring(L, data, dataLength);
        lua_setfield(L, -2, "data");
    }
    if (extras == RequestExtras::Score || extras == RequestExtras::Count) {
        const double number = luaL_checknumber(L, 3);
        const bool valid =
            std::isfinite(number) &&
            (extras == RequestExtras::Count ? number >= 1.0 && number <= 100.0 && std::floor(number) == number
                                            : std::abs(number) <= 9007199254740991.0);
        if (!valid) {
            lua_pop(L, 1);
            return failed(L, "InvalidArgument", response);
        }
        lua_pushnumber(L, number);
        lua_setfield(L, -2, extras == RequestExtras::Score ? "score" : "count");
    }
    const std::string argument = serializeJsonValue(L, -1);
    lua_pop(L, 1);
    return beginIntegrationAction(L, selected.id, operation, argument, response, true);
}
int getProduct(lua_State* L)
{
    return mappedRequest(L, "store", "GetProduct", IntegrationResponse::Product, true);
}
int purchase(lua_State* L)
{
    return mappedRequest(L, "store", "Purchase", IntegrationResponse::Action, true);
}
int owns(lua_State* L)
{
    return mappedRequest(L, "store", "OwnsProduct", IntegrationResponse::Boolean, true);
}
int refreshEntitlements(lua_State* L)
{
    return mappedRequest(L, "store", "RefreshEntitlements", IntegrationResponse::Action, false, false);
}
int cloudAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "cloud_save", "CloudRead").provider != nullptr);
    return 1;
}
int cloudSupports(lua_State* L)
{
    const std::string_view feature = luaL_checkstring(L, 2);
    const std::string_view operation = feature == "Read"     ? "CloudRead"
                                       : feature == "Write"  ? "CloudWrite"
                                       : feature == "Delete" ? "CloudDelete"
                                       : feature == "Exists" ? "CloudExists"
                                                             : "";
    lua_pushboolean(L, !operation.empty() && select(L, "cloud_save", operation).provider != nullptr);
    return 1;
}
int cloudWrite(lua_State* L)
{
    return mappedRequest(L, "cloud_save", "CloudWrite", IntegrationResponse::Action, false, true, RequestExtras::Data);
}
int cloudRead(lua_State* L)
{
    return mappedRequest(L, "cloud_save", "CloudRead", IntegrationResponse::String, false);
}
int cloudDelete(lua_State* L)
{
    return mappedRequest(L, "cloud_save", "CloudDelete", IntegrationResponse::Action, false);
}
int cloudExists(lua_State* L)
{
    return mappedRequest(L, "cloud_save", "CloudExists", IntegrationResponse::Boolean, false);
}
int leaderboardAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "leaderboards", "GetLeaderboardTop").provider != nullptr);
    return 1;
}
int leaderboardSupports(lua_State* L)
{
    const std::string_view feature = luaL_checkstring(L, 2);
    const std::string_view operation = feature == "SubmitScore"       ? "SubmitLeaderboardScore"
                                       : feature == "GetTop"          ? "GetLeaderboardTop"
                                       : feature == "GetAroundPlayer" ? "GetLeaderboardAroundPlayer"
                                                                      : "";
    lua_pushboolean(L, !operation.empty() && select(L, "leaderboards", operation).provider != nullptr);
    return 1;
}
int submitScore(lua_State* L)
{
    return mappedRequest(L, "leaderboards", "SubmitLeaderboardScore", IntegrationResponse::Action, true, true,
                         RequestExtras::Score);
}
int getTop(lua_State* L)
{
    return mappedRequest(L, "leaderboards", "GetLeaderboardTop", IntegrationResponse::LeaderboardEntries, true, true,
                         RequestExtras::Count);
}
int getAround(lua_State* L)
{
    return mappedRequest(L, "leaderboards", "GetLeaderboardAroundPlayer", IntegrationResponse::LeaderboardEntries, true,
                         true, RequestExtras::Count);
}
int socialAvailable(lua_State* L)
{
    lua_pushboolean(L, select(L, "social", "GetFriends").provider != nullptr);
    return 1;
}
int socialSupports(lua_State* L)
{
    const std::string_view feature = luaL_checkstring(L, 2);
    const std::string_view operation = feature == "GetFriends"       ? "GetFriends"
                                       : feature == "GetPresence"    ? "GetPresence"
                                       : feature == "IsBlocked"      ? "IsBlocked"
                                       : feature == "CanCommunicate" ? "CanCommunicate"
                                                                     : "";
    lua_pushboolean(L, !operation.empty() && select(L, "social", operation).provider != nullptr);
    return 1;
}
int getFriends(lua_State* L)
{
    return mappedRequest(L, "social", "GetFriends", IntegrationResponse::UserList, false, false);
}
int getPresence(lua_State* L)
{
    return mappedRequest(L, "social", "GetPresence", IntegrationResponse::Presence, false);
}
int isBlocked(lua_State* L)
{
    return mappedRequest(L, "social", "IsBlocked", IntegrationResponse::Boolean, false);
}
int canCommunicate(lua_State* L)
{
    return mappedRequest(L, "social", "CanCommunicate", IntegrationResponse::Boolean, false);
}
const InstanceMethodBinding Methods[] = {
    {"IdentityService", "IsAvailable", identityAvailable},
    {"IdentityService", "IsSignedIn", identitySignedIn},
    {"IdentityService", "GetLocalUser", localUser},
    {"IdentityService", "SignInAsync", signIn},
    {"AchievementService", "IsAvailable", achievementAvailable},
    {"AchievementService", "Supports", achievementSupports},
    {"AchievementService", "UnlockAsync", unlock},
    {"AchievementService", "GetProgressAsync", getProgress},
    {"AchievementService", "SetProgressAsync", setProgress},
    {"StoreService", "IsAvailable", storeAvailable},
    {"StoreService", "Supports", storeSupports},
    {"StoreService", "GetProductAsync", getProduct},
    {"StoreService", "PurchaseAsync", purchase},
    {"StoreService", "OwnsAsync", owns},
    {"StoreService", "RefreshEntitlementsAsync", refreshEntitlements},
    {"CloudSaveService", "IsAvailable", cloudAvailable},
    {"CloudSaveService", "Supports", cloudSupports},
    {"CloudSaveService", "WriteAsync", cloudWrite},
    {"CloudSaveService", "ReadAsync", cloudRead},
    {"CloudSaveService", "DeleteAsync", cloudDelete},
    {"CloudSaveService", "ExistsAsync", cloudExists},
    {"LeaderboardService", "IsAvailable", leaderboardAvailable},
    {"LeaderboardService", "Supports", leaderboardSupports},
    {"LeaderboardService", "SubmitScoreAsync", submitScore},
    {"LeaderboardService", "GetTopAsync", getTop},
    {"LeaderboardService", "GetAroundPlayerAsync", getAround},
    {"SocialService", "IsAvailable", socialAvailable},
    {"SocialService", "Supports", socialSupports},
    {"SocialService", "GetFriendsAsync", getFriends},
    {"SocialService", "GetPresenceAsync", getPresence},
    {"SocialService", "IsBlockedAsync", isBlocked},
    {"SocialService", "CanCommunicateWithAsync", canCommunicate},
};
} // namespace
std::span<const InstanceMethodBinding> platformServiceMethods()
{
    return Methods;
}
} // namespace engine::script
