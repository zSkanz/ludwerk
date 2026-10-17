#include "engine/script/join_request.h"

#include <lua.h>
#include <lualib.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <variant>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/ticket.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

constexpr std::string_view JoinCallback = "OnJoinRequest";
// The words a refusal is shown in, at most.
constexpr std::size_t MaxReasonBytes = 256;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

[[nodiscard]] bool isNetworkService(const scene::World& w, core::InstanceId id) noexcept
{
    if (!w.alive(id))
        return false;
    const scene::ClassDescriptor* descriptor = w.classes().find(w.classOf(id));
    return descriptor != nullptr && w.atoms().text(descriptor->name) == "NetworkService";
}

[[nodiscard]] std::string fieldText(lua_State* L, int table, const char* name)
{
    lua_getfield(L, table, name);
    std::string out;
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        out.assign(text, length);
    }
    lua_pop(L, 1);
    return out;
}

// A request's thread has stopped: its answer, and the account it named.
void finish(lua_State* L, lua_State* co, int status, int requestRef, core::u32 id)
{
    scene::EngineState::JoinVerdict verdict;
    verdict.id = id;
    if (status == LUA_OK) {
        const int count = lua_gettop(co);
        verdict.admit = count >= 1 && lua_toboolean(co, 1) != 0;
        if (count >= 2 && lua_type(co, 2) == LUA_TSTRING)
            verdict.reason = std::string(lua_tostring(co, 2)).substr(0, MaxReasonBytes);
        lua_getref(L, requestRef);
        verdict.accountId = fieldText(L, lua_gettop(L), "AccountId");
        lua_pop(L, 1);
    }
    else {
        // An error is a refusal, said in the log and not to the joiner.
        const char* message = lua_tostring(co, -1);
        const std::array<core::I18nArg, 1> args{
            core::I18nArg{"message", std::string_view{message != nullptr ? message : ""}}};
        core::log(core::LogLevel::Warn, ENG_TR("net.warn.join_request_failed"), args);
    }
    world(L).engineState().joinVerdicts.push_back(std::move(verdict));
}

void start(lua_State* L, const scene::EngineState::JoinValidation& asked)
{
    ServiceState& state = services(L);
    if (state.joinRequestHandler < 0) {
        // Unset since it was asked: everybody is let in.
        world(L).engineState().joinVerdicts.push_back(
            scene::EngineState::JoinVerdict{.id = asked.id, .admit = true, .reason = {}, .accountId = {}});
        return;
    }
    lua_State* co = lua_newthread(L);
    const int threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, asked.publicKey.data(), asked.publicKey.size());
    lua_setfield(L, -2, "PublicKey");
    lua_pushlstring(L, asked.address.data(), asked.address.size());
    lua_setfield(L, -2, "Address");
    lua_pushlstring(L, asked.ticket.data(), asked.ticket.size());
    lua_setfield(L, -2, "Ticket");
    const int requestRef = lua_ref(L, -1);
    lua_getref(L, state.joinRequestHandler);
    lua_xmove(L, co, 1);
    lua_xmove(L, co, 1);
    const int status = startScheduled(L, co, 1);
    if (status == LUA_YIELD || status == LUA_BREAK) {
        state.joinRunning.push_back(ServiceState::JoinRunning{threadRef, requestRef, asked.id});
        return;
    }
    finish(L, co, status, requestRef, asked.id);
    (void)lua_unref(L, requestRef);
    (void)lua_unref(L, threadRef);
}

} // namespace

bool joinRequestCallbackGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    if (key != JoinCallback || !isNetworkService(world(L), id))
        return false;
    const int handler = services(L).joinRequestHandler;
    if (handler >= 0)
        lua_getref(L, handler);
    else
        lua_pushnil(L);
    return true;
}

bool joinRequestCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex)
{
    if (key != JoinCallback || !isNetworkService(world(L), id))
        return false;
    // **The server's to say**, as the console is (ADR 0218, 0226).
    if (world(L).engineState().networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.join_request_client"));
    if (!lua_isnil(L, valueIndex) && !lua_isfunction(L, valueIndex))
        raise(L, ENG_TR("script.err.join_request_function"));
    ServiceState& state = services(L);
    if (state.joinRequestHandler >= 0) {
        (void)lua_unref(L, state.joinRequestHandler);
        state.joinRequestHandler = -1;
    }
    if (lua_isfunction(L, valueIndex)) {
        lua_pushvalue(L, valueIndex);
        state.joinRequestHandler = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    world(L).engineState().validateJoinSet = state.joinRequestHandler >= 0;
    return true;
}

int networkServiceVerifyTicket(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const scene::EngineState& engine = world(L).engineState();
    if (!engine.ticketPublicKey.has_value())
        raise(L, ENG_TR("script.err.ticket_no_key"));
    // A wall clock, not the simulation's: a ticket's life is the backend's
    // seconds, and what it decides is who joins, an input like a packet's.
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string why;
    const std::optional<core::TicketClaims> claims = core::verifyTicket(
        std::string_view{text, length}, *engine.ticketPublicKey, engine.projectId, now, &services(L).ticketReplay, why);
    if (!claims.has_value()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"why", std::string_view{why}}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.ticket_refused"), args);
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, static_cast<int>(claims->all.size()));
    for (const auto& [name, value] : claims->all) {
        if (const std::string* said = std::get_if<std::string>(&value))
            lua_pushlstring(L, said->data(), said->size());
        else if (const core::f64* number = std::get_if<core::f64>(&value))
            lua_pushnumber(L, *number);
        else
            lua_pushboolean(L, std::get<bool>(value) ? 1 : 0);
        lua_setfield(L, -2, name.c_str());
    }
    return 1;
}

void runJoinRequests(lua_State* L)
{
    ServiceState& state = services(L);
    for (std::size_t index = 0; index < state.joinRunning.size();) {
        const ServiceState::JoinRunning entry = state.joinRunning[index];
        lua_getref(L, entry.threadRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int status = co != nullptr ? lua_status(co) : LUA_ERRRUN;
        if (status == LUA_YIELD || status == LUA_BREAK) {
            ++index;
            continue;
        }
        state.joinRunning.erase(state.joinRunning.begin() + static_cast<std::ptrdiff_t>(index));
        if (co != nullptr)
            finish(L, co, status, entry.requestRef, entry.id);
        (void)lua_unref(L, entry.requestRef);
        (void)lua_unref(L, entry.threadRef);
    }
    std::vector<scene::EngineState::JoinValidation> asked;
    asked.swap(world(L).engineState().joinValidations);
    for (const scene::EngineState::JoinValidation& one : asked)
        start(L, one);
}

} // namespace engine::script
