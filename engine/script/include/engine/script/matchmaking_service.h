// **`MatchmakingService`** (ADR 0243): a script asks, the manager's queues
// and lobbies answer, and every world a matched player or a lobby's member is
// in hears of it.
#pragma once

struct lua_State;

namespace engine::script {

int matchmakingServiceDefineQueue(lua_State* L);
int matchmakingServiceQueueAsync(lua_State* L);
int matchmakingServiceCancelAsync(lua_State* L);
int matchmakingServiceBackfillAsync(lua_State* L);
int matchmakingServiceReportResultAsync(lua_State* L);
int matchmakingServiceGetRatingAsync(lua_State* L);
int matchmakingServiceSetRatingAsync(lua_State* L);
int matchmakingServiceCreateLobbyAsync(lua_State* L);
int matchmakingServiceJoinLobbyAsync(lua_State* L);
int matchmakingServiceLeaveLobbyAsync(lua_State* L);
int matchmakingServiceSetReadyAsync(lua_State* L);
int matchmakingServiceListLobbiesAsync(lua_State* L);
int matchmakingServiceStartLobbyAsync(lua_State* L);

// Between ticks: the manager's answers resumed, and `Matched` and
// `LobbyChanged`.
void fireMatchmakingEvents(lua_State* L);

} // namespace engine::script
