// **Events between a game's worlds** (ADR 0223): `CrossWorldService`'s
// publish and subscribe, delivered by the manager, or to this world alone
// where no manager runs it.
#pragma once

struct lua_State;

namespace engine::script {

int crossWorldServicePublish(lua_State* L);
int crossWorldServiceSubscribe(lua_State* L);
int crossWorldServiceUnsubscribe(lua_State* L);

// Between ticks: each message that arrived, to every callback of its topic.
void fireCrossWorldMessages(lua_State* L);

} // namespace engine::script
