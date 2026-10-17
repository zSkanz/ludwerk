#include "engine/script/bot_behavior.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/platform.h"
#include "engine/script/bytecode.h"

namespace engine::script {

using core::f64;
using core::u32;
using core::u64;

namespace {

// How long `WalkTo` walks before it gives up, when the behavior names no time.
constexpr f64 WalkToSeconds = 30.0;
// Close enough, on the ground's plane, to have got there.
constexpr f64 ArrivedMetres = 2.0;
// The longest one bot's behavior may run without waiting: every bot of the
// process shares one thread, so one that never yields stops them all.
constexpr u64 MostRunNs = 250'000'000ull;

enum class Phase : core::u8
{
    // Not begun: it begins once the bot has a body.
    Fresh,
    Running,
    Waiting,
    Walking,
    Finished,
    Failed,
};

struct Run
{
    Phase phase = Phase::Fresh;
    // The thread and the `Bot` table it was given, held by the registry.
    int thread = LUA_NOREF;
    int bot = LUA_NOREF;
    lua_State* state = nullptr;
    f64 until = 0.0;
    core::DVec3 target{};
};

} // namespace

struct BotBehaviors::Impl
{
    lua_State* L = nullptr;
    std::string bytecode;
    std::string chunkName;
    std::vector<Run> runs;
    // What the thread being resumed is: read by `Bot`'s functions.
    u32 current = 0;
    const BotSight* sight = nullptr;
    const BotWorldView* view = nullptr;
    f64 seconds = 0.0;
    u64 resumedNs = 0;
    // A failure's text is said once, however many bots it stops.
    std::vector<std::string> said;
    u32 failedCount = 0;
    u32 finishedCount = 0;
};

namespace {

[[nodiscard]] BotBehaviors::Impl& implOf(lua_State* L)
{
    return *static_cast<BotBehaviors::Impl*>(lua_callbacks(L)->userdata);
}

[[noreturn]] void raise(lua_State* L, core::TextKey key, std::initializer_list<core::I18nArg> args = {})
{
    {
        const std::string text = core::tr(key, args);
        lua_pushlstring(L, text.data(), text.size());
    }
    lua_error(L);
}

void interrupt(lua_State* L, int)
{
    BotBehaviors::Impl& impl = implOf(L);
    if (impl.resumedNs == 0 || platform::nowNs() - impl.resumedNs < MostRunNs)
        return;
    impl.resumedNs = 0;
    raise(L, ENG_TR("bots.err.behavior_busy"), {{"seconds", static_cast<f64>(MostRunNs) / 1.0e9}});
}

[[nodiscard]] u32 botOf(lua_State* L)
{
    return static_cast<u32>(lua_tointeger(L, lua_upvalueindex(1)));
}

[[nodiscard]] Run& runOf(lua_State* L)
{
    BotBehaviors::Impl& impl = implOf(L);
    const u32 bot = botOf(L);
    // `Bot`'s functions belong to one bot; another's thread calling them
    // would move the wrong bot.
    if (bot != impl.current)
        raise(L, ENG_TR("bots.err.behavior_other_bot"));
    return impl.runs[bot];
}

void pushVector(lua_State* L, const core::DVec3& v)
{
    lua_pushvector(L, static_cast<LUA_VECTOR_TYPE>(v.x), static_cast<LUA_VECTOR_TYPE>(v.y),
                   static_cast<LUA_VECTOR_TYPE>(v.z));
}

int botWalkTo(lua_State* L)
{
    Run& run = runOf(L);
    const LUA_VECTOR_TYPE* to = lua_tovector(L, 2);
    if (to == nullptr)
        raise(L, ENG_TR("bots.err.behavior_argument"), {{"function", "WalkTo"}, {"expected", "vector"}});
    const f64 seconds = luaL_optnumber(L, 3, WalkToSeconds);
    if (!lua_isyieldable(L))
        raise(L, ENG_TR("bots.err.behavior_yield"), {{"function", "WalkTo"}});
    run.target = core::DVec3{static_cast<f64>(to[0]), static_cast<f64>(to[1]), static_cast<f64>(to[2])};
    run.until = implOf(L).seconds + std::max(0.0, seconds);
    run.phase = Phase::Walking;
    return lua_yield(L, 0);
}

int botWait(lua_State* L)
{
    Run& run = runOf(L);
    const f64 seconds = luaL_optnumber(L, 2, 0.0);
    if (!lua_isyieldable(L))
        raise(L, ENG_TR("bots.err.behavior_yield"), {{"function", "Wait"}});
    run.until = implOf(L).seconds + std::max(0.0, seconds);
    run.phase = Phase::Waiting;
    return lua_yield(L, 0);
}

int botGetPosition(lua_State* L)
{
    (void)runOf(L);
    const BotBehaviors::Impl& impl = implOf(L);
    if (impl.sight == nullptr || !impl.sight->hasBody) {
        lua_pushnil(L);
        return 1;
    }
    pushVector(L, impl.sight->position);
    return 1;
}

int botFindTagged(lua_State* L)
{
    (void)runOf(L);
    const BotBehaviors::Impl& impl = implOf(L);
    size_t length = 0;
    const char* tag = luaL_checklstring(L, 2, &length);
    std::vector<core::DVec3> found;
    if (impl.view != nullptr)
        impl.view->tagged(std::string_view(tag, length), found);
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (size_t at = 0; at < found.size(); ++at) {
        pushVector(L, found[at]);
        lua_rawseti(L, -2, static_cast<int>(at + 1));
    }
    return 1;
}

int botSay(lua_State* L)
{
    (void)runOf(L);
    size_t length = 0;
    const char* text = luaL_tolstring(L, 2, &length);
    const std::array<core::I18nArg, 2> args{core::I18nArg{"bot", static_cast<core::i64>(botOf(L))},
                                            core::I18nArg{"text", std::string_view(text, length)}};
    core::log(core::LogLevel::Info, ENG_TR("bots.said"), args);
    return 0;
}

// `require` in a behavior: `@engine/bot` is the bot, and nothing else is
// there to require -- a bot runs none of the game.
int botRequire(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    if (std::string_view(name) != "@engine/bot")
        raise(L, ENG_TR("bots.err.behavior_require"), {{"name", name}});
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

} // namespace

BotBehaviors::BotBehaviors() : m_impl(std::make_unique<Impl>())
{}

BotBehaviors::~BotBehaviors()
{
    if (m_impl->L != nullptr)
        lua_close(m_impl->L);
}

std::unique_ptr<BotBehaviors> BotBehaviors::create(std::string_view source, std::string_view chunkName,
                                                   std::string& outError)
{
    std::unique_ptr<BotBehaviors> made(new BotBehaviors());
    Impl& impl = *made->m_impl;
    if (const auto refused = bytecodeOf(source, chunkName, impl.bytecode)) {
        outError = refused->message;
        return nullptr;
    }
    impl.chunkName = "@" + std::string(chunkName);
    impl.L = luaL_newstate();
    if (impl.L == nullptr) {
        outError = core::tr(ENG_TR("script.err.vm_create_failed"));
        return nullptr;
    }
    lua_callbacks(impl.L)->userdata = &impl;
    lua_callbacks(impl.L)->interrupt = interrupt;
    luaL_openlibs(impl.L);
    luaL_sandbox(impl.L);
    // A syntax error compiles to bytecode that carries it: loaded once here,
    // so a behavior that cannot run is refused before any bot dials.
    if (luau_load(impl.L, impl.chunkName.c_str(), impl.bytecode.data(), impl.bytecode.size(), 0) != LUA_OK) {
        const char* message = lua_tostring(impl.L, -1);
        outError = message == nullptr ? std::string{} : std::string(message);
        return nullptr;
    }
    lua_pop(impl.L, 1);
    return made;
}

void BotBehaviors::restart(u32 bot)
{
    Impl& impl = *m_impl;
    if (impl.runs.size() <= bot)
        impl.runs.resize(bot + 1);
    Run& run = impl.runs[bot];
    if (run.thread != LUA_NOREF)
        lua_unref(impl.L, run.thread);
    if (run.bot != LUA_NOREF)
        lua_unref(impl.L, run.bot);
    run = Run{};
}

void BotBehaviors::step(u32 bot, const BotSight& sight, const BotWorldView& view, f64 seconds)
{
    Impl& impl = *m_impl;
    if (impl.runs.size() <= bot)
        restart(bot);
    Run& run = impl.runs[bot];
    impl.seconds = seconds;
    int arguments = 0;
    switch (run.phase) {
    case Phase::Finished:
    case Phase::Failed:
    case Phase::Running:
        return;
    case Phase::Waiting:
        if (seconds < run.until)
            return;
        break;
    case Phase::Walking: {
        if (!sight.hasBody)
            return;
        const f64 dx = run.target.x - sight.position.x;
        const f64 dz = run.target.z - sight.position.z;
        const bool arrived = dx * dx + dz * dz <= ArrivedMetres * ArrivedMetres;
        if (!arrived && seconds < run.until)
            return;
        lua_pushboolean(run.state, arrived ? 1 : 0);
        arguments = 1;
        break;
    }
    case Phase::Fresh: {
        if (!sight.hasBody)
            return;
        lua_State* L = impl.L;
        lua_State* thread = lua_newthread(L);
        run.thread = lua_ref(L, -1);
        lua_pop(L, 1);
        luaL_sandboxthread(thread);
        // **The bot**, made before the chunk loads: a sandboxed thread's
        // globals are resolved at load (safeenv), so `require` set after it
        // would be one the chunk never sees.
        lua_createtable(thread, 0, 8);
        const auto method = [&](const char* name, lua_CFunction function) {
            lua_pushinteger(thread, static_cast<int>(bot));
            lua_pushcclosurek(thread, function, name, 1, nullptr);
            lua_setfield(thread, -2, name);
        };
        method("WalkTo", botWalkTo);
        method("Wait", botWait);
        method("GetPosition", botGetPosition);
        method("FindTagged", botFindTagged);
        method("Say", botSay);
        pushVector(thread, sight.position);
        lua_setfield(thread, -2, "Spawn");
        lua_pushinteger(thread, static_cast<int>(bot));
        lua_setfield(thread, -2, "Index");
        lua_setreadonly(thread, -1, 1);
        lua_pushvalue(thread, -1);
        run.bot = lua_ref(thread, -1);
        lua_pop(thread, 1);
        lua_pushcclosurek(thread, botRequire, "require", 1, nullptr);
        lua_setglobal(thread, "require");
        if (luau_load(thread, impl.chunkName.c_str(), impl.bytecode.data(), impl.bytecode.size(), 0) != LUA_OK) {
            run.phase = Phase::Failed;
            impl.failedCount += 1;
            return;
        }
        run.state = thread;
        break;
    }
    }

    impl.current = bot;
    impl.sight = &sight;
    impl.view = &view;
    run.phase = Phase::Running;
    impl.resumedNs = platform::nowNs();
    const int status = lua_resume(run.state, nullptr, arguments);
    impl.resumedNs = 0;
    impl.sight = nullptr;
    impl.view = nullptr;
    if (status == LUA_YIELD) {
        // A bare `coroutine.yield` is a tick's pause, as a `Wait()` is.
        if (run.phase == Phase::Running) {
            run.phase = Phase::Waiting;
            run.until = seconds;
        }
        lua_settop(run.state, 0);
        return;
    }
    if (status == LUA_OK) {
        run.phase = Phase::Finished;
        impl.finishedCount += 1;
        return;
    }
    run.phase = Phase::Failed;
    impl.failedCount += 1;
    const char* message = lua_tostring(run.state, -1);
    const std::string text = message == nullptr ? std::string{} : std::string(message);
    if (std::find(impl.said.begin(), impl.said.end(), text) == impl.said.end()) {
        impl.said.push_back(text);
        const std::array<core::I18nArg, 2> args{core::I18nArg{"bot", static_cast<core::i64>(bot)},
                                                core::I18nArg{"error", text}};
        core::log(core::LogLevel::Warn, ENG_TR("bots.err.behavior_failed"), args);
    }
}

std::optional<core::DVec3> BotBehaviors::walkTarget(u32 bot) const
{
    if (m_impl->runs.size() <= bot || m_impl->runs[bot].phase != Phase::Walking)
        return std::nullopt;
    return m_impl->runs[bot].target;
}

u32 BotBehaviors::failed() const noexcept
{
    return m_impl->failedCount;
}

u32 BotBehaviors::finished() const noexcept
{
    return m_impl->finishedCount;
}

} // namespace engine::script
