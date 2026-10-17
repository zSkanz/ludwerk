// **Who may join, as the game says** (ADR 0226): `NetworkService.OnJoinRequest`,
// asked between ticks about each joiner the session holds at the door, and
// `NetworkService:VerifyTicket`, which reads the ticket a joiner presented.
#pragma once

#include <string_view>

#include "engine/core/id.h"

struct lua_State;

namespace engine::script {

// The callback as a member of `NetworkService`: read back, and set -- by the
// server's side only.
bool joinRequestCallbackGet(lua_State* L, core::InstanceId id, std::string_view key);
bool joinRequestCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex);

int networkServiceVerifyTicket(lua_State* L);

// The joiners asked about since the last tick, each on a thread of its own;
// those that yielded and have finished, answered.
void runJoinRequests(lua_State* L);

} // namespace engine::script
