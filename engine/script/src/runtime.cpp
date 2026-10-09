#include "engine/script/runtime.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/random.h"
#include "engine/scene/character_replay.h"
#include "engine/script/builtins.h"
#include "engine/script/bytecode.h"
#include "engine/script/crypto_service.h"
#include "engine/script/datatypes.h"
#include "engine/script/debugger.h"
#include "engine/script/input_events.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/net_module.h"
#include "engine/script/optional_service.h"
#include "engine/script/sandbox.h"
#include "engine/script/save_service.h"
#include "engine/script/scenes.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"
#include "engine/script/tasks.h"
#include "engine/script/tweens.h"
#include "print_tree.h"

namespace engine::script {
namespace {

// Fires for the life of the VM rather than only during boot -- every string the
// VM interns passes through it -- so the sink has to be reachable from a free
// function. It is reached through `lua_callbacks(L)->userdata` like every other
// binding: the callback is handed the `lua_State`, which is what makes a
// process-global unnecessary and the VM count irrelevant here.
int16_t internAtom(lua_State* L, const char* text, size_t length)
{
    void* stored = lua_callbacks(L)->userdata;
    if (stored == nullptr)
        return -1;

    VmContext& ctx = *static_cast<VmContext*>(stored);
    // **Looked up, never interned** (audit S6, S11). Luau asks the first time a
    // string names a member, and a key built at run time -- `workspace["x" ..
    // i]` -- interned into the world's table, which never frees, one name a
    // key for ever. A string naming nothing the world has is -1, and
    // `resolve` looks it up by its text if it comes to name something later.
    const core::NameAtom name = ctx.world->atoms().lookup(std::string_view{text, length});
    if (!name.valid())
        return -1;
    if (const auto known = ctx.nameToAtom.find(name.id); known != ctx.nameToAtom.end())
        return known->second;
    // Luau's atom is an int16, so there are 32767 of them; past that a name is
    // looked up by text each time, which is slower and still right.
    const auto atom = static_cast<int16_t>(ctx.atomToName.size());
    if (static_cast<usize>(atom) >= 32767u)
        return -1;
    ctx.atomToName.push_back(name.id);
    ctx.nameToAtom.emplace(name.id, atom);
    return atom;
}

// Concatenates the call's arguments the way `print` does -- tab-separated,
// `tostring` applied to each -- so that `warn` differs from `print` in severity
// and in nothing else.
//
// **A table prints as what is in it** (the owner, 2026-09-27), not as its
// address: `{a = 1, b = {...}}` on the line, and every field -- the tables
// inside it too -- in `tree` for the console to fold (`print_tree.h`). With
// more than one table among the arguments, each is a row of its own there,
// named by its position.
std::string concatArguments(lua_State* L, std::string* tree = nullptr)
{
    std::string line;
    const int count = lua_gettop(L);
    int tables = 0;
    for (int index = 1; index <= count; ++index)
        tables += printsAsTree(L, index) ? 1 : 0;
    for (int index = 1; index <= count; ++index) {
        if (index > 1)
            line += '\t';
        if (printsAsTree(L, index)) {
            line += printSummary(L, index);
            if (tree == nullptr)
                continue;
            if (tables == 1) {
                appendPrintTree(L, index, 0, *tree);
            }
            else {
                *tree += "0\x1F[" + std::to_string(index) + "]\x1F" + printSummary(L, index) + "\n";
                appendPrintTree(L, index, 1, *tree);
            }
            continue;
        }
        size_t length = 0;
        const char* text = luaL_tolstring(L, index, &length);
        line.append(text, length);
        lua_pop(L, 1);
    }
    return line;
}

// Where the script that called is, as the first row of a print's detail:
// `@<chunk> 0x1F <line>`. The console names it beside the line and stacks a
// message only with the same one from the same place (the owner).
std::string callerRow(lua_State* L)
{
    lua_Debug ar{};
    if (lua_getinfo(L, 1, "sl", &ar) == 0 || ar.source == nullptr || ar.currentline <= 0)
        return {};
    std::string_view chunk{ar.source};
    if (!chunk.empty() && (chunk.front() == '=' || chunk.front() == '@'))
        chunk.remove_prefix(1);
    return "@" + std::string(chunk) + "\x1F" + std::to_string(ar.currentline) + "\n";
}

// `warn` is a global api-design.md §1.1 lists and Luau does not define -- its
// base library has neither `warn` nor `collectgarbage`. Script output is the
// script author's own text and is NOT an engine message, so it passes through
// verbatim rather than through the catalog: R3 governs what the engine says,
// not what a game says.
int scriptWarn(lua_State* L)
{
    std::string tree = callerRow(L);
    const std::string line = concatArguments(L, &tree);
    core::logText(core::LogLevel::Warn, line, tree);
    // Exactly ONE deferred fire per call, carrying the text verbatim
    // (api-design.md §2.1). Not per argument, and not per log line.
    publishMessage(L, core::LogLevel::Warn, line);
    return 0;
}

int scriptPrint(lua_State* L)
{
    std::string tree = callerRow(L);
    const std::string line = concatArguments(L, &tree);
    core::logText(core::LogLevel::Info, line, tree);
    publishMessage(L, core::LogLevel::Info, line);
    return 0;
}

void installConsole(lua_State* L)
{
    lua_pushcfunction(L, scriptPrint, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, scriptWarn, "warn");
    lua_setglobal(L, "warn");
}

} // namespace

// The eight the engine reserves (architecture.md §6). Named here rather than in
// the header because the DebugShell reads a row's name off the runtime and
// nothing else needs the table.
constexpr std::string_view kReservedCategories[] = {
    "engine", "modules", "bindings", "signals+tasks", "ui", "net", "assets", "repl",
};

constexpr core::u32 kReplCategory = 7;
constexpr core::u32 kFirstScriptCategory = 32;
constexpr core::u32 kCategoryCount = 256;

// **The script heap, counted and capped** (audit S8): Luau's own allocator
// underneath, and the one place every byte a script holds passes through. An
// allocation that would cross the cap fails, which Luau turns into
// `LUA_ERRMEM` in the thread that asked -- the game goes on without that
// thread's work rather than the machine running out.
struct ScriptHeap
{
    core::usize used = 0;
    core::usize cap = 0;
};

void* cappedAlloc(void* user, void* block, size_t oldSize, size_t newSize)
{
    auto* heap = static_cast<ScriptHeap*>(user);
    const core::usize had = block != nullptr ? oldSize : 0;
    if (newSize == 0) {
        std::free(block);
        heap->used -= std::min(heap->used, had);
        return nullptr;
    }
    if (heap->cap != 0 && newSize > had && heap->used - std::min(heap->used, had) + newSize > heap->cap)
        return nullptr;
    void* grown = std::realloc(block, newSize);
    if (grown == nullptr)
        return nullptr;
    heap->used = heap->used - std::min(heap->used, had) + newSize;
    return grown;
}

// **The watchdog's check** (audit S5), at the VM's safepoints: a resume that
// has run past its limit is stopped by an error in the thread running now --
// the one that never yields. Read every 256 safepoints, because a clock read
// at every loop back edge is a cost a correct script would pay.
void watchdogInterrupt(lua_State* L, int gc)
{
    if (gc >= 0) {
        // A collector step: its start, then its end (H11).
        VmContext& ctx = context(L);
        if (!ctx.gcStepOpen)
            ENG_PROFILE_NEXT(ctx.gcSteps, "scripts.gc");
        else
            ctx.gcSteps.close();
        ctx.gcStepOpen = !ctx.gcStepOpen;
        return;
    }
    VmContext& ctx = context(L);
    if (ctx.killAfterNs == 0 || ctx.resumeStartedNs == 0 || (++ctx.interruptTicks & 255u) != 0)
        return;
    const u64 now = watchdogNow();
    if (now - ctx.resumeStartedNs < ctx.killAfterNs)
        return;
    // The next thread gets a whole budget: this one is the runaway.
    ctx.resumeStartedNs = now;
    const core::I18nArg args[] = {{"seconds", static_cast<f64>(ctx.killAfterNs) / 1e9}};
    raise(L, ENG_TR("script.err.script_timeout"), args);
}

// The runtime as the mirror's `scene::PredictedStepHost` (G37).
class PredictedHost final : public scene::PredictedStepHost
{
public:
    explicit PredictedHost(lua_State*& state) noexcept : m_state(state) {}

    void predictedStep(const scene::PredictedTick& tick) override
    {
        if (m_state != nullptr)
            script::runPredictedStep(m_state, tick);
    }
    [[nodiscard]] bool touchBound(core::InstanceId part) const override
    {
        return m_state != nullptr && script::predictedTouchBound(m_state, part);
    }
    void predictedTouch(const scene::PredictedTick& tick, core::InstanceId part) override
    {
        if (m_state != nullptr)
            script::runPredictedTouch(m_state, tick, part);
    }

private:
    lua_State*& m_state;
};

struct ScriptRuntime::Impl
{
    ScriptHeap heap;
    lua_State* state = nullptr;
    PredictedHost predicted{state};
    // Reached from every binding through `lua_callbacks(L)->userdata`. Owned
    // here, and by a stable address: the callbacks hold a pointer to it for the
    // life of the state.
    VmContext context;
    // Owned here rather than by the context, because this is the object whose
    // lifetime brackets the `lua_State` -- the context is a view the bindings
    // reach through `lua_callbacks`.
    SignalSystem signals;
    TaskScheduler tasks;
    ServiceState services;
    ModuleRegistry modules;

    // Which memory category each entry script's chunk was given, in assignment
    // order. architecture.md §6 reserves 0..7 for the engine and hands 32..255
    // out per Script instance; this is that pool, and it recycles from the
    // start when it runs out -- coalescing the oldest, which is §6's own word
    // for it, because a world that reloads two hundred scripts should not stop
    // attributing memory to any of them.
    std::vector<std::pair<std::string, core::u32>> scriptCategories;
    core::u32 nextCategory = kFirstScriptCategory;

    // The bag a host has not replaced. It dies with the VM, which is exactly
    // right for a world nobody is going to reload -- and it means `SaveState`
    // is never a documented no-op, which is the failure `DebugService` taught
    // us to design out (M2 Finding 15).
    ReloadState ownReload;

    // Owned here for the same reason everything above it is: this is the object
    // whose lifetime brackets the `lua_State`, and the debugger holds registry
    // references that die with it.
    Debugger debugger;
};

ScriptRuntime::ScriptRuntime(scene::World& world) : m_world(world), m_impl(std::make_unique<Impl>())
{
    m_impl->context.world = &world;
    m_impl->context.signals = &m_impl->signals;
    m_impl->context.tasks = &m_impl->tasks;
    m_impl->context.services = &m_impl->services;
    m_impl->context.modules = &m_impl->modules;
    m_impl->services.reload = &m_impl->ownReload;
    // This runtime drains the world's moved scripts (ADR 0137 §1).
    world.setTracksScripts(true);
}

void ScriptRuntime::constructStamps(bool editing)
{
    if (m_world.engineState().pendingConstructs.empty() &&
        (!editing || m_world.engineState().changedParameters.empty())) {
        m_world.engineState().changedParameters.clear();
        return;
    }
    constructPendingStamps(state(), editing);
}

void ScriptRuntime::setStampSource(std::function<std::optional<std::string>(std::string_view)> source)
{
    m_impl->context.stamps = std::move(source);
}

void ScriptRuntime::setClientWriteLog(
    std::function<bool(const scene::World&, core::InstanceId, std::string_view)> travels)
{
    m_impl->context.travels = std::move(travels);
    m_impl->context.toldClientWrites.clear();
}

void ScriptRuntime::setSaveStore(SaveStore* store) noexcept
{
    m_impl->services.saves = store;
}

void ScriptRuntime::setPlatformServices(platform::PlatformServiceConfiguration configuration)
{
    m_impl->services.platformServices = std::move(configuration);
}

void ScriptRuntime::setIntegrationProvider(std::string id, std::string library, std::string configuration)
{
    auto& entry = m_impl->services.integrations[std::move(id)];
    entry.library = std::move(library);
    entry.configuration = std::move(configuration);
}

ScriptRuntime::~ScriptRuntime()
{
    if (m_impl->state != nullptr) {
        lua_close(m_impl->state);
        m_impl->state = nullptr;
    }
}

std::optional<core::EngineError> ScriptRuntime::boot(core::InstanceId adoptDataModel)
{
    if (m_impl->state != nullptr)
        return std::nullopt;

    lua_State* L = lua_newstate(cappedAlloc, &m_impl->heap);
    if (L == nullptr)
        return core::makeError(ENG_TR("script.err.vm_create_failed"));

    m_impl->state = L;

    // BEFORE `luaL_openlibs`, and this is not a preference. The callback is
    // lazy and one-shot per string: a name interned while it is absent latches
    // at -1 for the life of the VM, and the symptom is a property lookup that
    // silently stops using its atom -- slower, still correct, and invisible.
    lua_callbacks(L)->userdata = &m_impl->context;
    lua_callbacks(L)->useratom = internAtom;
    installThreadSides(L);
    lua_callbacks(L)->interrupt = watchdogInterrupt;

    // Beside the other two, and before anything loads: the hooks are read by
    // the VM at every `BREAK` instruction, so they have to be there before a
    // chunk with one in it can run.
    m_impl->debugger.install(L);

    // Interned through the atom table rather than through the VM, because these
    // are compared against an engine `NameAtom` and not against a Luau one.
    m_impl->context.wellKnown.parent = m_world.atoms().intern("Parent");
    m_impl->context.wellKnown.cframe = m_world.atoms().intern("CFrame");
    m_impl->context.wellKnown.name = m_world.atoms().intern("Name");

    luaL_openlibs(L);

    // **`math.random` from the world, not from the clock** (audit S14):
    // `luaopen_math` seeds its generator from the time and an address, so the
    // same world ran differently on every run and every machine (R10). The
    // seed is drawn from a COPY of the world's stream, which leaves the
    // world's own draws exactly as they were.
    {
        core::Pcg32 derived = m_world.rng();
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "randomseed");
        lua_pushinteger(L, static_cast<int>(derived.nextU32() & 0x7FFFFFFFu));
        lua_call(L, 1, 0);
        lua_pop(L, 1);
    }

    // `luaL_sandbox` removes nothing (see sandbox.h). Everything api-design.md
    // §1.1 calls removed goes here, before the freeze.
    removeUnsafeGlobals(L);
    installDeterministicMath(L);

    // Globals belong between here and the seal. There is no second chance: the
    // sandbox marks the global table read-only and a later `lua_setglobal`
    // fails inside the VM rather than returning something a caller can check.
    // M0 got this wrong from the outside once, which is why the ordering lives
    // in one function instead of in a caller's head.
    installConsole(L);

    // Every tag metatable, then every global that constructs one. All of it
    // before the first `luau_load`: the fast dispatch opcodes are chosen once
    // per `Proto` at load time and a deopt is permanent (binding.h, rule 2).
    registerInstanceBinding(L);
    registerDatatypes(L);
    registerSignals(L);
    registerTasks(L);
    registerEnums(L);
    // Last of the registrations, because it creates the DataModel and its two
    // boot services -- which needs `pushInstance`, and therefore the Instance
    // metatable, to already exist.
    registerServices(L, adoptDataModel);
    registerRequire(L);
    // After `require` exists and before the sandbox seals: `@std/net` is
    // reached through `require` and through nothing else, which is what keeps
    // it off the exhaustive global list in api-design.md 1.1.
    registerStdModules(L);
    // `Collector` and `Promise`, written in Luau against every global above
    // and installed before the seal (ADR 0094).
    installBuiltins(L);

    sealGlobals(L);
    return std::nullopt;
}

void ScriptRuntime::setGizmoSink(const GizmoSink& sink)
{
    m_impl->services.gizmos = sink;
}

void ScriptRuntime::runRenderSteps(f64 dt)
{
    if (m_impl->state != nullptr)
        script::runRenderSteps(m_impl->state, dt);
}

void ScriptRuntime::runIntentWriters()
{
    if (m_impl->state != nullptr)
        script::runIntentWriters(m_impl->state);
}

scene::PredictedStepHost& ScriptRuntime::predictedStepHost() noexcept
{
    return m_impl->predicted;
}

void ScriptRuntime::setDrawnPoseSink(const DrawnPoseSink& sink)
{
    m_impl->services.drawnPoses = sink;
}

void ScriptRuntime::setPhysics(scene::PhysicsSync* physics)
{
    m_impl->services.physics = physics;
}

void ScriptRuntime::setPhysics2D(scene::PhysicsSync2D* physics)
{
    m_impl->services.physics2d = physics;
}

void ScriptRuntime::setNavigation(nav::INavigation* navigation)
{
    m_impl->services.navigation = navigation;
}

void ScriptRuntime::setInput(input::InputSystem* input)
{
    m_impl->services.input = input;
}

void ScriptRuntime::stepDetectors(core::f64 dt, std::span<const input::RawInputEvent> events)
{
    if (m_impl->state != nullptr)
        engine::script::stepDetectors(m_impl->state, dt, events);
}

void ScriptRuntime::fireInputDeviceEvents(std::span<const input::DeviceEvent> events)
{
    script::fireInputDeviceEvents(m_impl->state, events);
}

void ScriptRuntime::fireInputEvents(std::span<const input::RawInputEvent> events)
{
    engine::script::fireInputEvents(m_impl->state, events);
}

void ScriptRuntime::fireGestureEvents(std::span<const input::GestureEvent> events)
{
    script::fireGestureEvents(m_impl->state, events);
}

void ScriptRuntime::setAnimation(scene::AnimationHost* animation)
{
    m_impl->services.animation = animation;
}

void ScriptRuntime::setSkeleton(scene::SkeletonHost* skeleton)
{
    m_impl->services.skeleton = skeleton;
}

void ScriptRuntime::fireAnimationEnded(std::span<const scene::TrackId> ended)
{
    engine::script::fireAnimationEnded(m_impl->state, ended);
}

void ScriptRuntime::setReloadState(ReloadState* state)
{
    if (state != nullptr)
        m_impl->services.reload = state;
}

void ScriptRuntime::fireHotReload(bool before)
{
    if (m_impl->state == nullptr)
        return;
    fireHotReloadEvent(m_impl->state, before);
}

void ScriptRuntime::setModuleLoader(const ModuleLoader& loader)
{
    m_impl->modules.loader = loader;
}

MethodCoverage ScriptRuntime::methodCoverage() const noexcept
{
    return m_impl->state == nullptr ? MethodCoverage{} : script::methodCoverage(m_impl->state);
}

std::optional<core::EngineError> ScriptRuntime::evaluate(std::string_view source)
{
    return runSource(source, "repl", kReplCategory);
}

std::vector<ScriptRuntime::MemoryCategory> ScriptRuntime::memoryByCategory() const
{
    std::vector<MemoryCategory> rows;
    if (m_impl->state == nullptr)
        return rows;

    for (core::u32 category = 0; category < kCategoryCount; ++category) {
        const core::usize bytes = lua_totalbytes(m_impl->state, static_cast<int>(category));
        if (bytes == 0)
            continue;

        std::string_view name;
        if (category < std::size(kReservedCategories)) {
            name = kReservedCategories[category];
        }
        else {
            for (const auto& [chunk, assigned] : m_impl->scriptCategories) {
                if (assigned == category) {
                    name = chunk;
                    break;
                }
            }
        }
        rows.push_back(MemoryCategory{category, name, bytes});
    }
    return rows;
}

std::optional<core::EngineError> ScriptRuntime::runSource(std::string_view source, std::string_view chunkName)
{
    // An entry script gets a category of its own, from the pool §6 describes, so
    // that "which script is holding this memory" is a question the shell can
    // answer. Assigned by CHUNK NAME rather than per call, so a hot reload
    // re-uses the same row and the number stays comparable across one.
    core::u32 category = kFirstScriptCategory;
    bool found = false;
    for (const auto& [chunk, assigned] : m_impl->scriptCategories) {
        if (chunk == chunkName) {
            category = assigned;
            found = true;
            break;
        }
    }
    if (!found) {
        category = m_impl->nextCategory;
        m_impl->nextCategory =
            m_impl->nextCategory + 1 >= kCategoryCount ? kFirstScriptCategory : m_impl->nextCategory + 1;
        m_impl->scriptCategories.emplace_back(std::string(chunkName), category);
    }
    return runSource(source, chunkName, category);
}

std::optional<core::EngineError> ScriptRuntime::runSource(std::string_view source, std::string_view chunkName,
                                                          core::u32 category)
{
    lua_State* L = m_impl->state;
    if (L == nullptr)
        return core::makeError(ENG_TR("script.err.vm_not_booted"));

    // Source is compiled with the one set of options shared with the build-time
    // compile (ADR 0094); bytecode -- a packaged game's (ADR 0112) -- is taken
    // as it is, which is what lets a build with no compiler run one.
    std::string bytecode;
    if (auto refused = bytecodeOf(source, chunkName, bytecode))
        return std::move(*refused);

    // Each script runs on its own thread with its own globals table, which is
    // what "per-script sandboxing" means (api-design.md §3): a global one script
    // sets is not visible to another.
    //
    // The chunk is loaded ONTO the thread rather than onto the main state and
    // moved across. `lua_newthread` pushes the thread on top of whatever was
    // already there, so a load-then-move sequence moves the thread and leaves
    // the function behind -- which fails as a call on a non-function, some
    // distance from the mistake.
    lua_State* thread = lua_newthread(L);
    luaL_sandboxthread(thread);
    // Everything this thread allocates is attributed here, which is what makes
    // the DebugShell's memory table per-script rather than one number.
    lua_setmemcat(thread, static_cast<int>(category));

    const std::string chunk = "@" + std::string(chunkName);
    const int loadStatus = luau_load(thread, chunk.c_str(), bytecode.data(), bytecode.size(), 0);

    if (loadStatus != LUA_OK) {
        const char* message = lua_tostring(thread, -1);
        const std::string text = message == nullptr ? std::string{} : std::string(message);
        const core::I18nArg args[] = {{"source", chunkName}, {"message", text}};
        core::EngineError error = core::makeError(ENG_TR("script.err.syntax"), args);
        lua_pop(L, 1); // the thread
        return error;
    }

    if (!enterResume(L)) {
        lua_pop(L, 1); // the thread
        return core::makeError(ENG_TR("script.err.resume_too_deep"));
    }
    const int resumeStatus = lua_resume(thread, nullptr, 0);
    leaveResume(L, thread);
    // LUA_YIELD is the normal outcome for anything that calls `task.wait`: the
    // script has not finished, it is parked, and the scheduler will resume it.
    if (resumeStatus != LUA_OK && resumeStatus != LUA_YIELD) {
        const char* message = lua_tostring(thread, -1);
        const std::string text = message == nullptr ? std::string{} : std::string(message);
        const core::I18nArg args[] = {{"source", chunkName}, {"message", text}};
        core::EngineError error = core::makeError(ENG_TR("script.err.runtime"), args);
        lua_pop(L, 1); // the thread
        return error;
    }

    lua_pop(L, 1); // the thread
    return std::nullopt;
}

void ScriptRuntime::setWatchdog(core::f64 warnSeconds, core::f64 killSeconds) noexcept
{
    m_impl->context.warnAfterNs = static_cast<core::u64>(std::max(0.0, warnSeconds) * 1e9);
    m_impl->context.killAfterNs = static_cast<core::u64>(std::max(0.0, killSeconds) * 1e9);
}

void ScriptRuntime::setMemoryLimit(core::usize bytes) noexcept
{
    m_impl->heap.cap = bytes;
}

core::usize ScriptRuntime::memoryInUse() const noexcept
{
    return m_impl->heap.used;
}

void ScriptRuntime::stepTweens(f64 fixedDt)
{
    if (m_impl->state != nullptr)
        script::stepTweens(m_impl->state, fixedDt);
}

void ScriptRuntime::drain(core::Phase)
{
    if (m_impl->state == nullptr)
        return;

    // Anything the ENGINE raised outside a binding -- a scheduler write, a
    // future physics step. A script's own mutations were converted the moment
    // they happened, because a fire captures its connection list when it is
    // raised and not when it is drained (api-design.md §3.1).
    flushSceneChanges(m_impl->state);

    // **A script enabled since the last drain starts here** (ADR 0059 rule 3),
    // between the flush that noticed the write and the drain that runs what it
    // queues -- so the file scope runs in THIS drain, which is where boot
    // already starts scripts.
    //
    // Put into document order first. `collectDescendants` is depth-first
    // preorder, the same order `startScripts` uses; taking them in the order the
    // writes happened would let a pool artefact decide which of two scripts
    // enabled by one tick runs first, which R10 forbids.
    //
    // **And a script that became live or stopped being live since** (ADR 0137
    // §1): every `Script` a move carried -- a clone parented, a stamp placed, a
    // model put into storage or taken out of the world, a destroy -- starts or
    // stops here, in the same drain.
    reconcileScripts(m_impl->state, m_world.takeMovedScripts(), takeEnabledScripts(m_impl->state));

    (void)drainDeferred(m_impl->state);
    // What the runs that stopped this drain take with them: their
    // connections, their waits, their tables (the script-sides close, S4).
    finishEndedRuns(m_impl->state);

    // After the drain, which is what gives a `Destroying` handler a live handle
    // to work with and what makes every handle to the corpse stop resolving
    // afterwards (divergence #25).
    m_world.retireDestroyed();
}

void ScriptRuntime::resumeTimers()
{
    if (m_impl->state == nullptr)
        return;
    // Between `PostSimulation` and `Heartbeat` (architecture.md §3). Anything
    // these resumptions defer drains at `Heartbeat`, which is the drain that
    // follows this call.
    // Waiters BEFORE timers. A `WaitForChild` is satisfied by a tree state that
    // was already true when the tick began, while a timer is due only now -- so
    // a waiter resumed after the timer that observes it would make
    // `task.wait()` see a stale world one tick out of every one.
    resumeChildWaiters(m_impl->state);
    resumeDueTimers(m_impl->state, m_world.engineState().tick);
    // Saves (ADR 0111): the periodic write, and the threads waiting on a slot
    // or a write. Every tick, so shutdown's ticks finish a save too.
    resumeSaveWaiters(m_impl->state, m_world.engineState().fixedTimestep);
    // Passwords hashed on their own thread since the last tick (ADR 0151).
    resumeCryptoWaiters(m_impl->state);
    resumeIntegrationWaiters(m_impl->state);
}

void ScriptRuntime::firePhase(core::Phase phase, f64 delta)
{
    if (m_impl->state == nullptr)
        return;

    // Only five of the phases have a signal. The rest are engine-internal
    // resumption points and always will be: `FrameStart` is where a hot reload
    // lands and the parallel windows are the checker's, not a script's.
    const char* name = nullptr;
    switch (phase) {
    case core::Phase::PreRender:
        name = "PreRender";
        break;
    case core::Phase::PreAnimation:
        name = "PreAnimation";
        break;
    case core::Phase::PreSimulation:
        name = "PreSimulation";
        break;
    case core::Phase::PostSimulation:
        name = "PostSimulation";
        break;
    case core::Phase::Heartbeat:
        name = "Heartbeat";
        break;
    default:
        return;
    }

    fireRunServiceEvent(m_impl->state, m_world.atoms().intern(name), delta);
}

void ScriptRuntime::fireLoaded()
{
    if (m_impl->state != nullptr)
        fireDataModelLoaded(m_impl->state);
}

void ScriptRuntime::fireEvent(core::InstanceId instance, core::NameAtom event, f64 argument)
{
    if (m_impl->state == nullptr)
        return;

    const scene::EventDesc* descriptor = m_world.classes().findEvent(m_world.classOf(instance), event);
    if (descriptor == nullptr)
        return;

    lua_pushnumber(m_impl->state, argument);
    fireInstanceEvent(m_impl->state, instance, descriptor->slot, lua_gettop(m_impl->state), 1);
    lua_pop(m_impl->state, 1);
}

u32 ScriptRuntime::deferredDepth() const noexcept
{
    return m_impl->state == nullptr ? 0u : currentDepth(m_impl->state);
}

core::InstanceId ScriptRuntime::dataModel() const noexcept
{
    return m_impl->services.dataModel;
}

Debugger& ScriptRuntime::debugger() noexcept
{
    return m_impl->debugger;
}

const Debugger& ScriptRuntime::debugger() const noexcept
{
    return m_impl->debugger;
}

lua_State* ScriptRuntime::state() const noexcept
{
    return m_impl->state;
}

} // namespace engine::script
