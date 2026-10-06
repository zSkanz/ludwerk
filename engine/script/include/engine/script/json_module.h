// `@std/json` in the game VM (api-design.md §7, ADR 0030): `serialize`,
// `deserialize`, `null`, `object`, `asObject` and `asArray`, as Lute has them.
// See `json_module.cpp`.
#pragma once

struct lua_State;

namespace engine::script {

// The module's table. Registered by `registerStdModules`.
int openStdJson(lua_State* L);

} // namespace engine::script
