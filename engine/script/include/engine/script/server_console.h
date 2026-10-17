// **A server's console, as the scripts see it** (ADR 0218): a game's own
// commands, registered beside the engine's, and a script that runs a line as
// the operator would.
#pragma once

struct lua_State;

namespace engine::script {

int serverConsoleRegister(lua_State* L);
int serverConsoleUnregister(lua_State* L);
int serverConsoleExecuteAsync(lua_State* L);

// **Between ticks, where a message from a player becomes one**: the calls of
// the game's commands the console parsed since the last tick are run, each on
// a thread of its own; those that yielded and have since finished say what
// they answered; and a script waiting in `ExecuteAsync` is given its answer.
void runConsoleCalls(lua_State* L);

// `NetworkService.Announced` (ADR 0218): what a server's operator said to
// every player, fired once each, on the machine that heard it.
void fireServerMessages(lua_State* L);

} // namespace engine::script
