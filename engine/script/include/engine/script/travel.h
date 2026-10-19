// **A player sent to another world** (ADR 0222): `NetworkService:Travel` and
// `TravelGroup` on the server that sends, `TravelArrived` on the one that
// takes them, and `TravelStarted` on the machine that goes.
#pragma once

struct lua_State;

namespace engine::script {

int networkServiceTravel(lua_State* L);
int networkServiceTravelGroup(lua_State* L);
// ADR 0230: a channel by its number, the world and channel a player is in,
// and the list of this world's channels.
int networkServiceSwitchChannel(lua_State* L);
int networkServiceTravelToPlayer(lua_State* L);
int networkServiceGetChannelsAsync(lua_State* L);
// `NetworkService:SignInAsync` and `RegisterAsync` (ADR 0237).
int networkServiceSignInAsync(lua_State* L);
int networkServiceRegisterAsync(lua_State* L);

// Between ticks: who arrived, once their player is in the world, and a
// client sent elsewhere.
void fireTravelEvents(lua_State* L);

} // namespace engine::script
