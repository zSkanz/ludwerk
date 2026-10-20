#include "engine/script/party_service.h"

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
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

void requireServer(lua_State* L)
{
    if (world(L).engineState().networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.party_client"));
}

[[nodiscard]] std::string checkMember(lua_State* L, int index)
{
    const core::InstanceId id = checkInstance(L, index);
    const scene::PlayerComponent* player = world(L).players().find(id);
    if (player == nullptr)
        raise(L, ENG_TR("script.err.party_not_a_player"));
    return partyMemberOf(*player);
}

[[nodiscard]] std::string checkText(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return std::string(text, length);
}

// **One request to the book**, and the thread waiting for its answer.
int ask(lua_State* L, std::string_view op, std::string_view party, std::string_view member, std::string_view by,
        const char* method)
{
    requireServer(L);
    requireYieldable(L, method);
    scene::EngineState& state = world(L).engineState();
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", op);
    request.field("party", party);
    request.field("member", member);
    request.field("by", by);
    request.endObject();
    const core::u32 id = state.nextPartyOp++;
    state.partyOps.push_back(scene::EngineState::PartyOp{id, request.text()});
    lua_pushthread(L);
    services(L).partyWaiters.push_back(ServiceState::PartyWaiter{id, lua_ref(L, -1), op == "get"});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

void pushParty(lua_State* L, const core::JsonValue& party)
{
    if (party.type() != core::JsonType::Object) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 3);
    const std::string_view id = party["id"].asString();
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "Id");
    const std::string_view leader = party["leader"].asString();
    lua_pushlstring(L, leader.data(), leader.size());
    lua_setfield(L, -2, "Leader");
    const core::JsonValue members = party["members"];
    lua_createtable(L, static_cast<int>(members.size()), 0);
    for (core::usize at = 0; at < members.size(); ++at) {
        const std::string_view member = members.at(at).asString();
        lua_pushlstring(L, member.data(), member.size());
        lua_rawseti(L, -2, static_cast<int>(at + 1));
    }
    lua_setfield(L, -2, "Members");
}

// The party a world now knows of, for `TravelGroup` and an instance.
void remember(scene::EngineState& state, const core::JsonValue& party)
{
    if (party.type() != core::JsonType::Object)
        return;
    std::vector<std::string>& members = state.partyMembers[std::string(party["id"].asString())];
    members.clear();
    const core::JsonValue listed = party["members"];
    for (core::usize at = 0; at < listed.size(); ++at)
        members.emplace_back(listed.at(at).asString());
}

[[nodiscard]] core::InstanceId partyServiceOf(scene::World& w, core::InstanceId dataModel)
{
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("PartyService"));
    return w.findFirstChildOfClass(dataModel, serviceClass);
}

} // namespace

std::string partyMemberOf(const scene::PlayerComponent& player)
{
    if (!player.accountId.empty())
        return player.accountId;
    if (!player.publicKey.empty())
        return player.publicKey;
    return "user:" + std::to_string(player.userId);
}

int partyServiceMemberIdOf(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string member = checkMember(L, 2);
    lua_pushlstring(L, member.data(), member.size());
    return 1;
}

int partyServiceCreateAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string leader = checkMember(L, 2);
    return ask(L, "create", {}, leader, {}, "CreateAsync");
}

int partyServiceInviteAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string party = checkText(L, 2);
    const std::string by = checkMember(L, 3);
    // A player here, or a member id: a friend in another world.
    const std::string invitee = lua_type(L, 4) == LUA_TSTRING ? checkText(L, 4) : checkMember(L, 4);
    return ask(L, "invite", party, invitee, by, "InviteAsync");
}

int partyServiceAcceptAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string party = checkText(L, 2);
    const std::string invitee = checkMember(L, 3);
    return ask(L, "accept", party, invitee, {}, "AcceptAsync");
}

int partyServiceDeclineAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string party = checkText(L, 2);
    const std::string invitee = checkMember(L, 3);
    return ask(L, "decline", party, invitee, {}, "DeclineAsync");
}

int partyServiceLeaveAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string member = checkMember(L, 2);
    return ask(L, "leave", {}, member, {}, "LeaveAsync");
}

int partyServiceKickAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string party = checkText(L, 2);
    const std::string member = checkText(L, 3);
    return ask(L, "kick", party, member, {}, "KickAsync");
}

int partyServiceSetLeaderAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string party = checkText(L, 2);
    const std::string member = checkText(L, 3);
    return ask(L, "leader", party, member, {}, "SetLeaderAsync");
}

int partyServiceGetPartyAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string member = checkMember(L, 2);
    return ask(L, "get", {}, member, {}, "GetPartyAsync");
}

void firePartyEvents(lua_State* L)
{
    scene::World& w = world(L);
    scene::EngineState& state = w.engineState();
    ServiceState& kept = services(L);
    // **The book's answers**: the party, or nil and why.
    std::vector<scene::EngineState::PartyResult> results;
    results.swap(state.partyResults);
    for (const scene::EngineState::PartyResult& result : results) {
        const auto waiter =
            std::find_if(kept.partyWaiters.begin(), kept.partyWaiters.end(),
                         [&](const ServiceState::PartyWaiter& waiting) { return waiting.id == result.id; });
        if (waiter == kept.partyWaiters.end())
            continue;
        const ServiceState::PartyWaiter waiting = *waiter;
        kept.partyWaiters.erase(waiter);
        core::JsonDocument document;
        const bool read = static_cast<bool>(document.parse(result.answer, "party-result"));
        if (read)
            remember(state, document.root()["party"]);
        lua_getref(L, waiting.threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            std::string_view reason = "Invalid";
            if (read) {
                pushParty(co, document.root()["party"]);
                reason = document.root()["reason"].asString();
            }
            else {
                lua_pushnil(co);
            }
            int count = 1;
            if (!waiting.partyOnly) {
                if (reason.empty())
                    lua_pushnil(co);
                else
                    lua_pushlstring(co, reason.data(), reason.size());
                count = 2;
            }
            (void)resumeScheduled(L, co, count);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, waiting.threadRef);
    }

    const core::InstanceId service = partyServiceOf(w, kept.dataModel);
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("PartyService"));
    // **`Changed`**, once a party for each change.
    std::vector<std::string> changed;
    changed.swap(state.partyChanged);
    const scene::EventDesc* changedEvent =
        service.valid() ? w.classes().findEvent(serviceClass, w.atoms().intern("Changed")) : nullptr;
    for (const std::string& party : changed) {
        if (changedEvent == nullptr)
            break;
        const int top = lua_gettop(L);
        lua_pushlstring(L, party.data(), party.size());
        fireInstanceEvent(L, service, changedEvent->slot, top + 1, 1);
        lua_settop(L, top);
    }
    // **`Invited`**, in the world the invitee is in.
    std::vector<scene::EngineState::PartyInvite> invites;
    invites.swap(state.partyInvites);
    const scene::EventDesc* invitedEvent =
        service.valid() ? w.classes().findEvent(serviceClass, w.atoms().intern("Invited")) : nullptr;
    if (invitedEvent == nullptr || invites.empty())
        return;
    const core::InstanceId network = scene::networkServiceOf(w, kept.dataModel);
    for (const scene::EngineState::PartyInvite& invite : invites) {
        for (core::InstanceId child = network.valid() ? w.firstChild(network) : core::InstanceId{}; child.valid();
             child = w.nextSibling(child)) {
            const scene::PlayerComponent* player = w.players().find(child);
            if (player == nullptr || partyMemberOf(*player) != invite.member)
                continue;
            const int top = lua_gettop(L);
            pushInstance(L, child);
            lua_pushlstring(L, invite.party.data(), invite.party.size());
            lua_pushlstring(L, invite.by.data(), invite.by.size());
            fireInstanceEvent(L, service, invitedEvent->slot, top + 1, 3);
            lua_settop(L, top);
            break;
        }
    }
}

} // namespace engine::script
