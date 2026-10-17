// **Instances** (ADR 0221): a world asks its manager for a copy of a scene for
// one group, and an instance says who may enter and when it closes.
#pragma once

struct lua_State;

namespace engine::script {

int instanceServiceCreateInstanceAsync(lua_State* L);
int instanceServiceAllowPlayer(lua_State* L);
int instanceServiceDisallowPlayer(lua_State* L);
int instanceServiceClose(lua_State* L);
int instanceServiceKeepOpen(lua_State* L);

// Between ticks: the manager's answers to `CreateInstanceAsync`, and
// `Closing` once the manager says the instance closes.
void runInstanceEvents(lua_State* L);

} // namespace engine::script
