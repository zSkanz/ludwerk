// **`NakamaService`** (ADR 0242, as amended): a player signs in to the
// game's Nakama over HTTP, and that session is turned into the engine's own
// ticket at the game's front door. An optional integration (ADR 0188): off
// unless the project turns it on.
#pragma once

struct lua_State;

namespace engine::script {

int nakamaServiceAuthenticateAsync(lua_State* L);
int nakamaServiceGetTicketAsync(lua_State* L);
int nakamaServiceQueueAsync(lua_State* L);
int nakamaServiceCancelQueueAsync(lua_State* L);

// Between ticks: Nakama's realtime answers resumed, a match told to the front
// door and `Matched` fired.
void pumpNakama(lua_State* L);

} // namespace engine::script
