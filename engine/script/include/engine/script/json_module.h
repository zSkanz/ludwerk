// `@std/json` in the game VM (api-design.md §7, ADR 0030): `serialize`,
// `deserialize`, `null`, `object`, `asObject` and `asArray`, as Lute has them.
// See `json_module.cpp`.
#pragma once
#include <string>
#include <string_view>

struct lua_State;

namespace engine::script {

// The module's table. Registered by `registerStdModules`.
int openStdJson(lua_State* L);
// Shared with native platform requests; use the same bounded codec as @std/json.
void markJsonObject(lua_State* L);
[[nodiscard]] std::string serializeJsonValue(lua_State* L, int index);
void deserializeJsonValue(lua_State* L, std::string_view text, bool nullAsNil = false);

} // namespace engine::script
