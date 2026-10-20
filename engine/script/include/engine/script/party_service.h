// **`PartyService`** (ADR 0240 §1): a script asks, the party book -- the
// manager's, or a lone server's -- answers, and every world a member is in
// hears what changed.
#pragma once

#include <string>

struct lua_State;

namespace engine::scene {
struct PlayerComponent;
}

namespace engine::script {

// **Who a player is in a party**, and in a queue: their account, else their
// key; a player of a match with neither -- one machine, nothing encrypted --
// by their number.
[[nodiscard]] std::string partyMemberOf(const scene::PlayerComponent& player);

int partyServiceMemberIdOf(lua_State* L);
int partyServiceCreateAsync(lua_State* L);
int partyServiceInviteAsync(lua_State* L);
int partyServiceAcceptAsync(lua_State* L);
int partyServiceDeclineAsync(lua_State* L);
int partyServiceLeaveAsync(lua_State* L);
int partyServiceKickAsync(lua_State* L);
int partyServiceSetLeaderAsync(lua_State* L);
int partyServiceGetPartyAsync(lua_State* L);

// Between ticks: the book's answers resumed, and `Changed` and `Invited`.
void firePartyEvents(lua_State* L);

} // namespace engine::script
