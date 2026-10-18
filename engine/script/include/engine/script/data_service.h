// `DataService` (ADR 0238): a player's record, held by this world while the
// player is here, read from the game's database and written behind.
#pragma once

#include "engine/core/types.h"

struct lua_State;

namespace engine::script {

int dataServiceLoadAsync(lua_State* L);
int dataServiceTransactAsync(lua_State* L);
int dataServiceCreateItemAsync(lua_State* L);
int dataServiceDestroyItemAsync(lua_State* L);
int dataServiceGetItemsAsync(lua_State* L);
int dataServiceGetBalanceAsync(lua_State* L);

// The `DataRecord` value type's members. Once, at boot, before the sandbox.
void registerDataTypes(lua_State* L);

// Every tick: the records the database answered, handed to the threads that
// asked; each record written behind every 30 seconds; and the records of
// players who left, written and let go.
void resumeDataWaiters(lua_State* L, core::f64 dt);

// **The world stops**: every record it holds, written and let go.
void releaseDataRecords(lua_State* L);

} // namespace engine::script
