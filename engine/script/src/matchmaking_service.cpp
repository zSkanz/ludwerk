#include "engine/script/matchmaking_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/party_service.h"
#include "engine/script/remote.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

using Answer = ServiceState::MatchWaiter::Answer;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// **The manager's**: on a server it started, never on a player's machine.
void requireManaged(lua_State* L)
{
    const scene::EngineState& state = world(L).engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.matchmaking_client"));
    if (state.managedWorld.empty())
        raise(L, ENG_TR("script.err.matchmaking_no_manager"));
}

[[nodiscard]] std::string checkText(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return std::string(text, length);
}

[[nodiscard]] std::string checkMember(lua_State* L, int index)
{
    const core::InstanceId id = checkInstance(L, index);
    const scene::PlayerComponent* player = world(L).players().find(id);
    if (player == nullptr)
        raise(L, ENG_TR("script.err.party_not_a_player"));
    return partyMemberOf(*player);
}

// A player here, or a member id for one anywhere.
[[nodiscard]] std::string checkWho(lua_State* L, int index)
{
    return lua_type(L, index) == LUA_TSTRING ? checkText(L, index) : checkMember(L, index);
}

// A string field of the table at `index`, or empty.
[[nodiscard]] std::string fieldText(lua_State* L, int index, const char* name)
{
    if (!lua_istable(L, index))
        return {};
    lua_getfield(L, index, name);
    std::string out;
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        out.assign(text, length);
    }
    lua_pop(L, 1);
    return out;
}

[[nodiscard]] std::optional<double> fieldNumber(lua_State* L, int index, const char* name)
{
    if (!lua_istable(L, index))
        return std::nullopt;
    lua_getfield(L, index, name);
    std::optional<double> out;
    if (lua_type(L, -1) == LUA_TNUMBER)
        out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return out;
}

[[nodiscard]] std::optional<bool> fieldBool(lua_State* L, int index, const char* name)
{
    if (!lua_istable(L, index))
        return std::nullopt;
    lua_getfield(L, index, name);
    std::optional<bool> out;
    if (lua_type(L, -1) == LUA_TBOOLEAN)
        out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return out;
}

// **One request to the manager**, and the thread waiting for its answer.
int ask(lua_State* L, core::JsonWriter& request, Answer answer, const char* method)
{
    requireManaged(L);
    requireYieldable(L, method);
    request.endObject();
    scene::EngineState& state = world(L).engineState();
    const core::u32 id = state.nextMatchOp++;
    state.matchOps.push_back(scene::EngineState::MatchOp{id, request.text()});
    lua_pushthread(L);
    services(L).matchWaiters.push_back(ServiceState::MatchWaiter{id, lua_ref(L, -1), answer});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

void begin(core::JsonWriter& request, std::string_view op)
{
    request.beginObject();
    request.field("op", op);
}

void pushStrings(lua_State* L, const core::JsonValue& list)
{
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (core::usize at = 0; at < list.size(); ++at) {
        const std::string_view one = list.at(at).asString();
        lua_pushlstring(L, one.data(), one.size());
        lua_rawseti(L, -2, static_cast<int>(at + 1));
    }
}

void pushText(lua_State* L, const core::JsonValue& value, const char* field)
{
    const std::string_view text = value.asString();
    lua_pushlstring(L, text.data(), text.size());
    lua_setfield(L, -2, field);
}

// `{ Id, Queue, Instance, Teams }`, or nil.
void pushMatch(lua_State* L, const core::JsonValue& match, const core::JsonValue& instance)
{
    if (match.type() != core::JsonType::Object) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 4);
    pushText(L, match["id"], "Id");
    pushText(L, match["queue"], "Queue");
    pushText(L, instance, "Instance");
    const core::JsonValue teams = match["teams"];
    lua_createtable(L, static_cast<int>(teams.size()), 0);
    for (core::usize at = 0; at < teams.size(); ++at) {
        pushStrings(L, teams.at(at));
        lua_rawseti(L, -2, static_cast<int>(at + 1));
    }
    lua_setfield(L, -2, "Teams");
}

// `{ Id, Code, Owner, Template, Size, Public, Members, Ready }`, or nil.
void pushLobby(lua_State* L, const core::JsonValue& lobby)
{
    if (lobby.type() != core::JsonType::Object) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 8);
    pushText(L, lobby["id"], "Id");
    pushText(L, lobby["code"], "Code");
    pushText(L, lobby["owner"], "Owner");
    pushText(L, lobby["template"], "Template");
    lua_pushnumber(L, lobby["size"].asNumber());
    lua_setfield(L, -2, "Size");
    lua_pushboolean(L, lobby["public"].asBool(false) ? 1 : 0);
    lua_setfield(L, -2, "Public");
    pushStrings(L, lobby["members"]);
    lua_setfield(L, -2, "Members");
    pushStrings(L, lobby["ready"]);
    lua_setfield(L, -2, "Ready");
}

void pushReason(lua_State* L, const core::JsonValue& root)
{
    const std::string_view reason = root["reason"].asString();
    if (reason.empty())
        lua_pushnil(L);
    else
        lua_pushlstring(L, reason.data(), reason.size());
}

// What the waiting thread is given back, by what it asked.
[[nodiscard]] int pushAnswer(lua_State* co, Answer kind, const core::JsonValue& root)
{
    switch (kind) {
    case Answer::Ticket: {
        const std::string_view ticket = root["ticket"].asString();
        if (root["ok"].asBool(false))
            lua_pushlstring(co, ticket.data(), ticket.size());
        else
            lua_pushnil(co);
        pushReason(co, root);
        return 2;
    }
    case Answer::Done:
        lua_pushboolean(co, root["ok"].asBool(false) ? 1 : 0);
        pushReason(co, root);
        return 2;
    case Answer::Rating:
        lua_pushnumber(co, root["rating"].asNumber(1500.0));
        return 1;
    case Answer::Nothing:
        return 0;
    case Answer::Lobby:
        pushLobby(co, root["lobby"]);
        pushReason(co, root);
        return 2;
    case Answer::Lobbies: {
        const core::JsonValue lobbies = root["lobbies"];
        lua_createtable(co, static_cast<int>(lobbies.size()), 0);
        for (core::usize at = 0; at < lobbies.size(); ++at) {
            pushLobby(co, lobbies.at(at));
            lua_rawseti(co, -2, static_cast<int>(at + 1));
        }
        return 1;
    }
    case Answer::Match:
        pushMatch(co, root["match"], root["instance"]);
        pushReason(co, root);
        return 2;
    }
    return 0;
}

[[nodiscard]] core::InstanceId serviceOf(scene::World& w, core::InstanceId dataModel)
{
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("MatchmakingService"));
    return w.findFirstChildOfClass(dataModel, serviceClass);
}

} // namespace

int matchmakingServiceDefineQueue(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string name = checkText(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const std::string templateName = fieldText(L, 3, "Template");
    if (name.empty() || templateName.empty())
        raise(L, ENG_TR("script.err.matchmaking_queue"));
    const scene::EngineState& state = world(L).engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.matchmaking_client"));
    // **Said where nothing matches** -- a world on its own -- it is kept by
    // nobody: the game's code runs the same everywhere, and asking is what
    // raises.
    if (state.managedWorld.empty())
        return 0;
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "define");
    request.field("queue", std::string_view{name});
    request.field("template", std::string_view{templateName});
    request.field("teams", fieldNumber(L, 3, "Teams").value_or(2.0));
    request.field("teamSize", fieldNumber(L, 3, "TeamSize").value_or(1.0));
    request.field("rating", std::string_view{fieldText(L, 3, "Rating")});
    lua_getfield(L, 3, "Widen");
    const int widen = lua_gettop(L);
    request.field("after", fieldNumber(L, widen, "After").value_or(10.0));
    request.field("step", fieldNumber(L, widen, "Step").value_or(100.0));
    request.field("most", fieldNumber(L, widen, "Most").value_or(600.0));
    lua_pop(L, 1);
    request.field("regions", fieldBool(L, 3, "Regions").value_or(true));
    request.endObject();
    // Asked, and never answered: nothing waits on it.
    scene::EngineState& changing = world(L).engineState();
    changing.matchOps.push_back(scene::EngineState::MatchOp{0, request.text()});
    return 0;
}

int matchmakingServiceQueueAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "queue");
    // A party by its id: its members online, wherever they are.
    if (lua_type(L, 2) == LUA_TSTRING)
        request.field("party", std::string_view{checkText(L, 2)});
    else
        request.field("member", std::string_view{checkMember(L, 2)});
    request.field("queue", std::string_view{checkText(L, 3)});
    request.field("region", std::string_view{fieldText(L, 4, "Region")});
    return ask(L, request, Answer::Ticket, "QueueAsync");
}

int matchmakingServiceCancelAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "cancel");
    request.field("ticket", std::string_view{checkText(L, 2)});
    return ask(L, request, Answer::Done, "CancelAsync");
}

int matchmakingServiceBackfillAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "backfill");
    request.field("match", std::string_view{checkText(L, 2)});
    request.field("team", static_cast<core::i64>(luaL_checkinteger(L, 3)) - 1);
    return ask(L, request, Answer::Ticket, "BackfillAsync");
}

int matchmakingServiceReportResultAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "report");
    request.field("match", std::string_view{checkText(L, 2)});
    request.field("team", static_cast<core::i64>(luaL_checkinteger(L, 3)) - 1);
    return ask(L, request, Answer::Done, "ReportResultAsync");
}

int matchmakingServiceGetRatingAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "getRating");
    request.field("member", std::string_view{checkWho(L, 2)});
    request.field("rating", std::string_view{checkText(L, 3)});
    return ask(L, request, Answer::Rating, "GetRatingAsync");
}

int matchmakingServiceSetRatingAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "setRating");
    request.field("member", std::string_view{checkWho(L, 2)});
    request.field("rating", std::string_view{checkText(L, 3)});
    request.field("value", static_cast<core::f64>(luaL_checknumber(L, 4)));
    return ask(L, request, Answer::Nothing, "SetRatingAsync");
}

int matchmakingServiceCreateLobbyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string owner = checkMember(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "createLobby");
    request.field("member", std::string_view{owner});
    request.field("template", std::string_view{fieldText(L, 3, "Template")});
    request.field("size", fieldNumber(L, 3, "Size").value_or(8.0));
    request.field("public", fieldBool(L, 3, "Public").value_or(false));
    return ask(L, request, Answer::Lobby, "CreateLobbyAsync");
}

int matchmakingServiceJoinLobbyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "joinLobby");
    request.field("member", std::string_view{checkMember(L, 2)});
    request.field("code", std::string_view{checkText(L, 3)});
    return ask(L, request, Answer::Lobby, "JoinLobbyAsync");
}

int matchmakingServiceLeaveLobbyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "leaveLobby");
    request.field("member", std::string_view{checkMember(L, 2)});
    return ask(L, request, Answer::Lobby, "LeaveLobbyAsync");
}

int matchmakingServiceSetReadyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "ready");
    request.field("member", std::string_view{checkMember(L, 2)});
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    request.field("ready", lua_toboolean(L, 3) != 0);
    return ask(L, request, Answer::Lobby, "SetReadyAsync");
}

int matchmakingServiceListLobbiesAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "listLobbies");
    request.field("template", std::string_view{fieldText(L, 2, "Template")});
    return ask(L, request, Answer::Lobbies, "ListLobbiesAsync");
}

int matchmakingServiceStartLobbyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    core::JsonWriter request(core::JsonLayout::Compact);
    begin(request, "startLobby");
    request.field("lobby", std::string_view{checkText(L, 2)});
    request.field("member", std::string_view{checkMember(L, 3)});
    return ask(L, request, Answer::Match, "StartLobbyAsync");
}

void fireMatchmakingEvents(lua_State* L)
{
    scene::World& w = world(L);
    scene::EngineState& state = w.engineState();
    ServiceState& kept = services(L);
    // **The load data of an instance given to a match**: `{ Match = info }`,
    // made here as a script's `Data` would be, and its scene loaded with it.
    if (state.pendingMatchLoad.has_value()) {
        const scene::EngineState::MatchLoad load = std::move(*state.pendingMatchLoad);
        state.pendingMatchLoad.reset();
        core::JsonDocument document;
        if (document.parse(load.match, "match-load")) {
            const int top = lua_gettop(L);
            lua_createtable(L, 0, 1);
            const core::JsonValue match = document.root();
            lua_createtable(L, 0, 3);
            pushText(L, match["id"], "Id");
            pushText(L, match["queue"], "Queue");
            const core::JsonValue teams = match["teams"];
            lua_createtable(L, static_cast<int>(teams.size()), 0);
            for (core::usize at = 0; at < teams.size(); ++at) {
                pushStrings(L, teams.at(at));
                lua_rawseti(L, -2, static_cast<int>(at + 1));
            }
            lua_setfield(L, -2, "Teams");
            lua_setfield(L, -2, "Match");
            std::vector<core::u8> payload;
            std::vector<core::InstanceId> refs;
            encodeRemoteArguments(L, top + 1, 1, payload, refs);
            lua_settop(L, top);
            state.pendingSceneLoad = scene::EngineState::PendingSceneLoad{load.scene, std::move(payload)};
        }
    }
    // **The manager's answers**, each to the thread that asked.
    std::vector<scene::EngineState::MatchResult> results;
    results.swap(state.matchResults);
    for (const scene::EngineState::MatchResult& result : results) {
        const auto waiter =
            std::find_if(kept.matchWaiters.begin(), kept.matchWaiters.end(),
                         [&](const ServiceState::MatchWaiter& waiting) { return waiting.id == result.id; });
        if (waiter == kept.matchWaiters.end())
            continue;
        const ServiceState::MatchWaiter waiting = *waiter;
        kept.matchWaiters.erase(waiter);
        // An answer that does not read is a refusal with no reason.
        core::JsonDocument document;
        if (!document.parse(result.answer, "match-result"))
            (void)document.parse("{}", "match-result");
        lua_getref(L, waiting.threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            const int count = pushAnswer(co, waiting.answer, document.root());
            (void)resumeScheduled(L, co, count);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, waiting.threadRef);
    }

    std::vector<std::string> made;
    made.swap(state.matchesMade);
    std::vector<std::string> lobbies;
    lobbies.swap(state.lobbiesChanged);
    if (made.empty() && lobbies.empty())
        return;
    const core::InstanceId service = serviceOf(w, kept.dataModel);
    if (!service.valid())
        return;
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("MatchmakingService"));
    // **`Matched`**, once a match in each world one of its players is in.
    if (const scene::EventDesc* event = w.classes().findEvent(serviceClass, w.atoms().intern("Matched"));
        event != nullptr) {
        for (const std::string& text : made) {
            core::JsonDocument document;
            if (!document.parse(text, "match-made"))
                continue;
            const int top = lua_gettop(L);
            pushMatch(L, document.root()["match"], document.root()["instance"]);
            fireInstanceEvent(L, service, event->slot, top + 1, 1);
            lua_settop(L, top);
        }
    }
    // **`LobbyChanged`**, in each world one of its members is in.
    if (const scene::EventDesc* event = w.classes().findEvent(serviceClass, w.atoms().intern("LobbyChanged"));
        event != nullptr) {
        for (const std::string& text : lobbies) {
            core::JsonDocument document;
            if (!document.parse(text, "lobby-changed"))
                continue;
            const int top = lua_gettop(L);
            pushLobby(L, document.root());
            fireInstanceEvent(L, service, event->slot, top + 1, 1);
            lua_settop(L, top);
        }
    }
}

} // namespace engine::script
