#include "engine/script/server_console.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

using Command = scene::EngineState::ConsoleCommand;
using Parameter = scene::EngineState::ConsoleParameter;

// The engine's own commands (ADR 0218 §2): a game's may not take their names.
constexpr std::array<std::string_view, 9> EngineCommands{"help",  "status", "players", "kick", "ban",
                                                         "unban", "say",    "save",    "stop"};
// A line a script runs is numbered from here, apart from the console's own
// calls, which count up from one.
constexpr core::u32 FirstScriptLine = 0x80000000u;
// What a command's answer keeps: a page of text.
constexpr std::size_t MaxAnswerBytes = 8192;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// **The server's, and only the server's** (ADR 0218 §3, ADR 0138): a
// replica has no console, and neither has a script of the client's side on
// a machine that hosts.
void requireServerSide(lua_State* L)
{
    if (world(L).engineState().networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.server_console_client"));
}

[[nodiscard]] std::string checkName(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    std::string name(text, length);
    const bool word = !name.empty() && name.size() <= 32 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (!word) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.server_console_name"), args);
    }
    return name;
}

[[nodiscard]] Parameter::Type typeNamed(lua_State* L, std::string_view type)
{
    if (type == "string")
        return Parameter::Type::String;
    if (type == "number")
        return Parameter::Type::Number;
    if (type == "boolean")
        return Parameter::Type::Boolean;
    if (type == "Player")
        return Parameter::Type::Player;
    const std::array<core::I18nArg, 1> args{core::I18nArg{"type", type}};
    raise(L, ENG_TR("script.err.server_console_type"), args);
}

// The spec table at `index`: `Description`, `Parameters`, `Permission`.
[[nodiscard]] Command readSpec(lua_State* L, int index, std::string name)
{
    Command command;
    command.name = std::move(name);
    if (lua_isnoneornil(L, index))
        return command;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, "Description");
    if (lua_type(L, -1) == LUA_TSTRING)
        command.description = lua_tostring(L, -1);
    else if (!lua_isnil(L, -1))
        raise(L, ENG_TR("script.err.server_console_spec"));
    lua_pop(L, 1);
    lua_getfield(L, index, "Permission");
    if (!lua_isnil(L, -1)) {
        const double level = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
        if (level < 0.0 || level > 3.0 || level != static_cast<double>(static_cast<int>(level)))
            raise(L, ENG_TR("script.err.server_console_spec"));
        command.permission = static_cast<core::u8>(level);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "Parameters");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            raise(L, ENG_TR("script.err.server_console_spec"));
        const int count = lua_objlen(L, -1);
        bool optionalSeen = false;
        for (int at = 1; at <= count; ++at) {
            lua_rawgeti(L, -1, at);
            if (!lua_istable(L, -1))
                raise(L, ENG_TR("script.err.server_console_spec"));
            Parameter parameter;
            lua_getfield(L, -1, "Name");
            if (lua_type(L, -1) != LUA_TSTRING)
                raise(L, ENG_TR("script.err.server_console_spec"));
            parameter.name = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "Type");
            if (lua_type(L, -1) != LUA_TSTRING)
                raise(L, ENG_TR("script.err.server_console_spec"));
            parameter.type = typeNamed(L, lua_tostring(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "Optional");
            parameter.optional = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            // A required one after an optional one could never be reached.
            if (optionalSeen && !parameter.optional)
                raise(L, ENG_TR("script.err.server_console_spec"));
            optionalSeen = optionalSeen || parameter.optional;
            command.parameters.push_back(std::move(parameter));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return command;
}

void pushCaller(lua_State* L, const scene::EngineState::ConsoleCaller& caller)
{
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, caller.local ? 1 : 0);
    lua_setfield(L, -2, "Local");
    lua_pushlstring(L, caller.name.data(), caller.name.size());
    lua_setfield(L, -2, "Name");
    lua_pushlstring(L, caller.key.data(), caller.key.size());
    lua_setfield(L, -2, "Key");
    lua_pushnumber(L, static_cast<double>(caller.permission));
    lua_setfield(L, -2, "Permission");
}

// A player by their number, on this machine: nil when they have left since
// the line was read.
void pushPlayer(lua_State* L, core::u32 userId)
{
    scene::World& w = world(L);
    const core::InstanceId network = scene::networkServiceOf(w, services(L).dataModel);
    for (core::InstanceId child = network.valid() ? w.firstChild(network) : core::InstanceId{}; child.valid();
         child = w.nextSibling(child)) {
        const scene::PlayerComponent* player = w.players().find(child);
        if (player != nullptr && player->userId == userId && !w.destroyed(child)) {
            pushInstance(L, child);
            return;
        }
    }
    lua_pushnil(L);
}

[[nodiscard]] std::string answerOf(lua_State* co, int count)
{
    if (count <= 0 || lua_isnil(co, -count))
        return {};
    size_t length = 0;
    const char* text = luaL_tolstring(co, -count, &length);
    std::string out(text, std::min(length, MaxAnswerBytes));
    lua_pop(co, 1);
    return out;
}

void finish(lua_State* L, lua_State* co, int status, core::u32 id)
{
    scene::EngineState::ConsoleAnswer answer;
    answer.id = id;
    if (status == LUA_OK) {
        answer.text = answerOf(co, lua_gettop(co));
    }
    else {
        const char* message = lua_tostring(co, -1);
        answer.text = message != nullptr ? std::string(message).substr(0, MaxAnswerBytes) : std::string{};
        answer.failed = true;
    }
    world(L).engineState().consoleAnswers.push_back(std::move(answer));
}

void startCall(lua_State* L, const scene::EngineState::ConsoleCall& call)
{
    ServiceState& state = services(L);
    const auto handler = std::find_if(state.consoleHandlers.begin(), state.consoleHandlers.end(),
                                      [&](const ServiceState::ConsoleHandler& h) { return h.name == call.name; });
    if (handler == state.consoleHandlers.end()) {
        // Unregistered between the line and the tick.
        scene::EngineState::ConsoleAnswer answer;
        answer.id = call.id;
        answer.failed = true;
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{call.name}}};
        answer.text = core::engineCatalog().format(ENG_TR("console.err.unknown_command"), args);
        world(L).engineState().consoleAnswers.push_back(std::move(answer));
        return;
    }
    const auto command =
        std::find_if(world(L).engineState().consoleCommands.begin(), world(L).engineState().consoleCommands.end(),
                     [&](const Command& c) { return c.name == call.name; });
    lua_State* co = lua_newthread(L);
    const int threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_getref(L, handler->functionRef);
    lua_xmove(L, co, 1);
    pushCaller(co, call.caller);
    int count = 1;
    for (std::size_t at = 0; at < call.arguments.size(); ++at) {
        const scene::EngineState::ConsoleArgument& argument = call.arguments[at];
        const Parameter::Type type =
            command != world(L).engineState().consoleCommands.end() && at < command->parameters.size()
                ? command->parameters[at].type
                : Parameter::Type::String;
        if (!argument.present)
            lua_pushnil(co);
        else if (type == Parameter::Type::Number)
            lua_pushnumber(co, argument.number);
        else if (type == Parameter::Type::Boolean)
            lua_pushboolean(co, argument.flag ? 1 : 0);
        else if (type == Parameter::Type::Player)
            pushPlayer(co, argument.userId);
        else
            lua_pushlstring(co, argument.text.data(), argument.text.size());
        ++count;
    }
    const int status = startScheduled(L, co, count);
    if (status == LUA_YIELD || status == LUA_BREAK) {
        state.consoleRunning.push_back(ServiceState::ConsoleRunning{threadRef, call.id});
        return;
    }
    finish(L, co, status, call.id);
    (void)lua_unref(L, threadRef);
}

} // namespace

int serverConsoleRegister(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServerSide(L);
    std::string name = checkName(L, 2);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    if (std::find(EngineCommands.begin(), EngineCommands.end(), name) != EngineCommands.end()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.server_console_engine_name"), args);
    }
    Command command = readSpec(L, 3, name);
    ServiceState& state = services(L);
    // **Twice is a replacement**, as a hot reload registers again.
    std::vector<Command>& commands = world(L).engineState().consoleCommands;
    std::erase_if(commands, [&](const Command& c) { return c.name == name; });
    commands.push_back(std::move(command));
    for (auto at = state.consoleHandlers.begin(); at != state.consoleHandlers.end();) {
        if (at->name == name) {
            (void)lua_unref(L, at->functionRef);
            at = state.consoleHandlers.erase(at);
        }
        else {
            ++at;
        }
    }
    lua_pushvalue(L, 4);
    state.consoleHandlers.push_back(ServiceState::ConsoleHandler{std::move(name), lua_ref(L, -1)});
    lua_pop(L, 1);
    return 0;
}

int serverConsoleUnregister(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServerSide(L);
    const std::string name = checkName(L, 2);
    ServiceState& state = services(L);
    std::erase_if(world(L).engineState().consoleCommands, [&](const Command& c) { return c.name == name; });
    for (auto at = state.consoleHandlers.begin(); at != state.consoleHandlers.end();) {
        if (at->name == name) {
            (void)lua_unref(L, at->functionRef);
            at = state.consoleHandlers.erase(at);
        }
        else {
            ++at;
        }
    }
    return 0;
}

int serverConsoleExecuteAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServerSide(L);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    requireYieldable(L, "ExecuteAsync");
    ServiceState& state = services(L);
    if (state.nextConsoleLine < FirstScriptLine)
        state.nextConsoleLine = FirstScriptLine;
    const core::u32 id = state.nextConsoleLine++;
    world(L).engineState().consoleLines.push_back(scene::EngineState::ConsoleLine{id, std::string(text, length)});
    lua_pushthread(L);
    state.consoleWaiters.push_back(ServiceState::ConsoleWaiter{id, lua_ref(L, -1)});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

void runConsoleCalls(lua_State* L)
{
    ServiceState& state = services(L);
    scene::EngineState& engine = world(L).engineState();
    // Those that yielded and have finished since.
    for (std::size_t index = 0; index < state.consoleRunning.size();) {
        const ServiceState::ConsoleRunning entry = state.consoleRunning[index];
        lua_getref(L, entry.threadRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int status = co != nullptr ? lua_status(co) : LUA_ERRRUN;
        if (status == LUA_YIELD || status == LUA_BREAK) {
            ++index;
            continue;
        }
        state.consoleRunning.erase(state.consoleRunning.begin() + static_cast<std::ptrdiff_t>(index));
        if (co != nullptr)
            finish(L, co, status, entry.id);
        (void)lua_unref(L, entry.threadRef);
    }
    // The calls parsed since the last tick.
    std::vector<scene::EngineState::ConsoleCall> calls;
    calls.swap(engine.consoleCalls);
    for (const scene::EngineState::ConsoleCall& call : calls)
        startCall(L, call);
    // A script's own line, answered.
    for (auto answer = engine.consoleLineAnswers.begin(); answer != engine.consoleLineAnswers.end();) {
        const auto waiter = std::find_if(state.consoleWaiters.begin(), state.consoleWaiters.end(),
                                         [&](const ServiceState::ConsoleWaiter& w) { return w.id == answer->id; });
        if (waiter == state.consoleWaiters.end()) {
            ++answer;
            continue;
        }
        const int threadRef = waiter->threadRef;
        state.consoleWaiters.erase(waiter);
        lua_getref(L, threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr) {
            lua_pushlstring(co, answer->text.data(), answer->text.size());
            if (answer->failed)
                (void)resumeScheduledWithError(L, co);
            else
                (void)resumeScheduled(L, co, 1);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
        answer = engine.consoleLineAnswers.erase(answer);
    }
}

void fireServerMessages(lua_State* L)
{
    std::vector<std::string> said;
    said.swap(world(L).engineState().serverMessages);
    for (const std::string& text : said) {
        // In the log on every machine that heard it: a game that shows
        // nothing of it still has it said somewhere.
        const std::array<core::I18nArg, 1> args{core::I18nArg{"text", std::string_view{text}}};
        core::log(core::LogLevel::Info, ENG_TR("net.info.server_message"), args);
        fireNetworkEvent(L, "Announced", std::string_view{text});
    }
}

} // namespace engine::script
