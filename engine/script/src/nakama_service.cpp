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
#include "engine/script/signals.h"
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

int noTicket(lua_State* L, const char* why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

// A query string's value, escaped as RFC 3986 asks of one.
[[nodiscard]] std::string escaped(std::string_view text)
{
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(Hex[byte >> 4]);
        out.push_back(Hex[byte & 15]);
    }
    return out;
}

// A queue's name, as the servers define one: letters, digits, `_` and `-`.
[[nodiscard]] bool queueName(std::string_view name)
{
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// **This machine's socket to Nakama**, opened once with the session it
// signed in with: `ws://` for an `http://` Nakama, `wss://` for `https://`.
[[nodiscard]] net::WebSocketSession* socket(lua_State* L)
{
    ServiceState& kept = *context(L).services;
    if (kept.nakamaSocket != nullptr && kept.nakamaSocket->state() != net::WebSocketSession::State::Closed)
        return kept.nakamaSocket.get();
    const scene::EngineState& state = world(L).engineState();
    std::string url = state.nakamaUrl;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (url.starts_with("https://"))
        url = "wss://" + url.substr(8);
    else if (url.starts_with("http://"))
        url = "ws://" + url.substr(7);
    else
        return nullptr;
    url += "/ws?lang=en&status=false&token=" + escaped(state.nakamaSession);
    kept.nakamaSocket = net::WebSocketSession::open(std::move(url), {}, 1u << 20);
    return kept.nakamaSocket.get();
}

// A request on the socket, the thread waiting for the answer to its id.
int ask(lua_State* L, core::JsonWriter& body, bool cancelling)
{
    ServiceState& kept = *context(L).services;
    net::WebSocketSession* open = socket(L);
    const auto failed = [&](const char* why) {
        if (!cancelling)
            return noTicket(L, why);
        lua_pushboolean(L, 0);
        return 1;
    };
    if (open == nullptr)
        return failed("NotConfigured");
    // Nakama's envelope: the request's id, and the request under its name.
    const std::string cid = std::to_string(kept.nextNakamaCid++);
    const std::string envelope = std::string(R"({"cid":")") + cid + R"(",")" +
                                 (cancelling ? "matchmaker_remove" : "matchmaker_add") + R"(":)" + body.text() + "}";
    if (!open->send(net::WebSocketSession::Message{false, envelope}))
        return failed("Unreachable");
    lua_pushthread(L);
    kept.nakamaWaiters.push_back(ServiceState::NakamaWaiter{cid, lua_ref(L, -1), cancelling});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

[[nodiscard]] core::InstanceId serviceOf(scene::World& w, core::InstanceId dataModel)
{
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("NakamaService"));
    return w.findFirstChildOfClass(dataModel, serviceClass);
}

} // namespace

int nakamaServiceQueueAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t addressLength = 0;
    size_t queueLength = 0;
    const char* address = luaL_checklstring(L, 2, &addressLength);
    const char* queue = luaL_checklstring(L, 3, &queueLength);
    requireYieldable(L, "QueueAsync");
    if (!enabled(L))
        return noTicket(L, "IntegrationDisabled");
    if (world(L).engineState().nakamaSession.empty())
        return noTicket(L, "NotSignedIn");
    const std::string_view name(queue, queueLength);
    if (!queueName(name) || addressLength == 0)
        return noTicket(L, "InvalidArgument");
    double least = 2.0;
    double most = 2.0;
    std::string query = "+properties.queue:" + std::string(name);
    core::JsonWriter body(core::JsonLayout::Compact);
    body.beginObject();
    body.key("string_properties");
    body.beginObject();
    body.field("queue", name);
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "Min");
        least = luaL_optnumber(L, -1, least);
        lua_getfield(L, 4, "Max");
        most = luaL_optnumber(L, -1, most);
        lua_getfield(L, 4, "Query");
        if (lua_type(L, -1) == LUA_TSTRING)
            query = lua_tostring(L, -1);
        lua_pop(L, 3);
        lua_getfield(L, 4, "Properties");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING &&
                    std::string_view(lua_tostring(L, -2)) != "queue")
                    body.field(lua_tostring(L, -2), std::string_view(lua_tostring(L, -1)));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    body.endObject();
    if (!(least >= 2.0 && most >= least && most <= 100.0))
        return noTicket(L, "InvalidArgument");
    body.field("min_count", static_cast<core::i64>(least));
    body.field("max_count", static_cast<core::i64>(most));
    body.field("query", std::string_view{query});
    body.endObject();
    context(L).services->nakamaDoor.assign(address, addressLength);
    return ask(L, body, false);
}

int nakamaServiceCancelQueueAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* ticket = luaL_checklstring(L, 2, &length);
    requireYieldable(L, "CancelQueueAsync");
    if (!enabled(L) || context(L).services->nakamaSocket == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    core::JsonWriter body(core::JsonLayout::Compact);
    body.beginObject();
    body.field("ticket", std::string_view(ticket, length));
    body.endObject();
    return ask(L, body, true);
}

void pumpNakama(lua_State* L)
{
    ServiceState& kept = *context(L).services;
    if (kept.nakamaSocket == nullptr)
        return;
    scene::World& w = world(L);
    std::vector<net::WebSocketSession::Message> arrived;
    kept.nakamaSocket->take(arrived);
    const auto resume = [&](const std::string& cid, const core::JsonValue* answer) {
        const auto waiter = std::find_if(kept.nakamaWaiters.begin(), kept.nakamaWaiters.end(),
                                         [&](const ServiceState::NakamaWaiter& one) { return one.cid == cid; });
        if (waiter == kept.nakamaWaiters.end())
            return;
        const ServiceState::NakamaWaiter waiting = *waiter;
        kept.nakamaWaiters.erase(waiter);
        lua_getref(L, waiting.threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            int count = 1;
            if (waiting.cancelling) {
                lua_pushboolean(co, answer != nullptr && !answer->has("error") ? 1 : 0);
            }
            else if (answer == nullptr) {
                count = noTicket(co, "Unreachable");
            }
            else if (const std::string_view ticket = (*answer)["matchmaker_ticket"]["ticket"].asString();
                     !ticket.empty()) {
                lua_pushlstring(co, ticket.data(), ticket.size());
                lua_pushnil(co);
                count = 2;
            }
            else {
                count = noTicket(co, "Refused");
            }
            (void)resumeScheduled(L, co, count);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, waiting.threadRef);
    };
    for (const net::WebSocketSession::Message& message : arrived) {
        core::JsonDocument document;
        if (message.binary || !document.parse(message.bytes, "nakama"))
            continue;
        const core::JsonValue root = document.root();
        if (const std::string_view cid = root["cid"].asString(); !cid.empty()) {
            resume(std::string(cid), &root);
            continue;
        }
        const std::string_view match = root["matchmaker_matched"]["match_id"].asString();
        if (match.empty())
            continue;
        // **Matched**: the front door told, with the session -- it asks
        // Nakama itself -- and the game told, for what the player sees.
        scene::EngineState& state = w.engineState();
        scene::EngineState::SignInAsk told;
        told.id = state.nextSignIn++;
        told.address = kept.nakamaDoor;
        told.name = std::string(match);
        told.password = state.nakamaSession;
        told.nakama = true;
        told.nakamaMatch = true;
        state.signInAsks.push_back(std::move(told));
        const core::InstanceId service = serviceOf(w, kept.dataModel);
        const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("NakamaService"));
        if (const scene::EventDesc* event =
                service.valid() ? w.classes().findEvent(serviceClass, w.atoms().intern("Matched")) : nullptr;
            event != nullptr) {
            const int top = lua_gettop(L);
            lua_pushlstring(L, match.data(), match.size());
            fireInstanceEvent(L, service, event->slot, top + 1, 1);
            lua_settop(L, top);
        }
    }
    // **A socket that went** answers every request still waiting on it.
    if (kept.nakamaSocket->state() == net::WebSocketSession::State::Closed) {
        while (!kept.nakamaWaiters.empty())
            resume(kept.nakamaWaiters.front().cid, nullptr);
    }
}

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
    request.headers.push_back(
        {"Authorization", "Basic " + core::base64Encode(std::span<const core::u8>(
                                         reinterpret_cast<const core::u8*>(credentials.data()), credentials.size()))});
    request.headers.push_back({"Content-Type", "application/json"});
    scene::World* place = context(L).world;
    return awaitHttp(L, std::move(request), [place](lua_State* co, const net::NetResult& result) -> int {
        if (result.error.has_value() || !result.response.ok) {
            lua_pushboolean(co, 0);
            lua_pushstring(co, "Unreachable");
            return 2;
        }
        core::JsonDocument answer;
        const std::string_view token = result.response.statusCode == 200 && answer.parse(result.response.body, "nakama")
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
