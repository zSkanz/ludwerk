#include "engine/script/nakama_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/base64.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/net_module.h"
#include "engine/script/optional_service.h"
#include "engine/script/services.h"
#include "engine/script/travel.h"

namespace engine::script {
namespace {

constexpr std::string_view Integration = "nakama";

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// **On where the project turns it on**: elsewhere it says so once, and
// answers rather than raises (ADR 0188).
[[nodiscard]] bool enabled(lua_State* L)
{
    const auto& on = world(L).engineState().enabledIntegrations;
    if (std::find(on.begin(), on.end(), Integration) != on.end())
        return true;
    warnOptionalIntegration(L, Integration, "NakamaService");
    return false;
}

int refused(lua_State* L, const char* why)
{
    lua_pushboolean(L, 0);
    lua_pushstring(L, why);
    return 2;
}

} // namespace

int nakamaServiceAuthenticateAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t kindLength = 0;
    size_t idLength = 0;
    const char* kind = luaL_checklstring(L, 2, &kindLength);
    const char* id = luaL_checklstring(L, 3, &idLength);
    const std::string_view asked(kind, kindLength);
    std::string password;
    if (lua_type(L, 4) == LUA_TSTRING) {
        size_t length = 0;
        const char* text = lua_tolstring(L, 4, &length);
        password.assign(text, length);
    }
    requireYieldable(L, "AuthenticateAsync");
    if (!enabled(L))
        return refused(L, "IntegrationDisabled");
    const scene::EngineState& state = world(L).engineState();
    if (state.nakamaUrl.empty())
        return refused(L, "NotConfigured");
    if ((asked != "device" && asked != "email" && asked != "custom") || idLength == 0 || idLength > 128 ||
        (asked == "email" && password.empty()))
        return refused(L, "InvalidArgument");

    // **Nakama's own authentication**: its client key as the basic
    // credentials, the account made the first time.
    core::JsonWriter body(core::JsonLayout::Compact);
    body.beginObject();
    body.field(asked == "email" ? "email" : "id", std::string_view(id, idLength));
    if (asked == "email")
        body.field("password", std::string_view{password});
    body.endObject();
    net::HttpRequest request;
    std::string url = state.nakamaUrl;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    request.url = url + "/v2/account/authenticate/" + std::string(asked) + "?create=true";
    request.method = "POST";
    request.body = body.text();
    const std::string credentials = state.nakamaServerKey + ":";
    request.headers.push_back({"Authorization",
                               "Basic " + core::base64Encode(std::span<const core::u8>(
                                              reinterpret_cast<const core::u8*>(credentials.data()),
                                              credentials.size()))});
    request.headers.push_back({"Content-Type", "application/json"});
    scene::World* place = context(L).world;
    return awaitHttp(L, std::move(request), [place](lua_State* co, const net::NetResult& result) -> int {
        if (result.error.has_value() || !result.response.ok) {
            lua_pushboolean(co, 0);
            lua_pushstring(co, "Unreachable");
            return 2;
        }
        core::JsonDocument answer;
        const std::string_view token =
            result.response.statusCode == 200 && answer.parse(result.response.body, "nakama")
                ? answer.root()["token"].asString()
                : std::string_view{};
        if (token.empty()) {
            lua_pushboolean(co, 0);
            lua_pushstring(co, "Refused");
            return 2;
        }
        place->engineState().nakamaSession = std::string(token);
        lua_pushboolean(co, 1);
        lua_pushnil(co);
        return 2;
    });
}

int nakamaServiceGetTicketAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t addressLength = 0;
    const char* address = luaL_checklstring(L, 2, &addressLength);
    requireYieldable(L, "GetTicketAsync");
    // A project without it, or a player not signed in to Nakama: the door
    // is asked with no session, and says so as it would.
    const std::string session = enabled(L) ? world(L).engineState().nakamaSession : std::string{};
    return askFrontDoor(L, std::string_view(address, addressLength), {}, session, false, true);
}

} // namespace engine::script
