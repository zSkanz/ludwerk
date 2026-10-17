// `SaveService` and `SaveSlot`, bound into the VM (ADR 0111). The slots
// themselves are `SaveStore`'s, which the host owns; this is the Luau face of
// them and the parking of the threads that wait on one.
#pragma once

#include <string>
#include <string_view>

#include "engine/core/id.h"
#include "engine/core/types.h"

struct lua_State;

namespace engine::script {

int saveServiceGetSlotAsync(lua_State* L);
int saveServiceListSlots(lua_State* L);
int saveServiceDeleteSlot(lua_State* L);

// `SaveService.OnMigrate`, read and assigned as a member of the service, as a
// `RemoteFunction`'s `OnServerInvoke` is. False when `key` is not it.
bool saveCallbackGet(lua_State* L, core::InstanceId id, std::string_view key);
bool saveCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex);

// The `SaveSlot` value type's members. Once, at boot, before the sandbox.
void registerSaveTypes(lua_State* L);

// Every tick: the store's periodic write, then every thread whose slot is
// read and migrated or whose save is on disk, resumed.
void resumeSaveWaiters(lua_State* L, core::f64 dt);

// **A value as a save holds it, as text** (ADR 0222): what a traveller's
// `data` crosses between worlds as. Raises for a value a save cannot hold,
// or one larger than `limit` bytes once written.
[[nodiscard]] std::string encodeSavedValue(lua_State* L, int index, core::usize limit);
// And back, pushed; nil for text that is not one.
void pushSavedValue(lua_State* L, std::string_view text);

} // namespace engine::script
