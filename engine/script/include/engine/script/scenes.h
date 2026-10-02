// A scene as scripts see it (ADR 0124): the `Scene` object and the `scene`
// global, the close handlers of a scene and of the game, owned by the script
// that registered them, and the two mailboxes, `game` and `scene`.
//
// **A scene is a serial here**, not a world: the host decides when one closes
// and the next opens, and this file only keeps what belongs to which. The open
// scene's path is the world's (`EngineState::currentScene`), because the editor
// changes it without closing anything; a closed or prepared scene keeps its own.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/types.h"
#include "engine/script/signals.h"

struct lua_State;

namespace engine::script {

// A function registered to run at a close, and the script whose thread
// registered it (ADR 0124 §3) -- not where its code lives: a module in a
// scene's storage, required by a global script, registers for the global one.
struct OwnedHandler
{
    int functionRef = -1;
    core::InstanceId owner;
    // The scene it closes with, for `scene:BindToClose`; unused for the game's.
    core::u32 scene = 0;
};

// `RunService:BindToRenderStep` (ADR 0136): a function run every drawn frame,
// in `priority` order and, between equals, the order bound (`order`), owned by
// the script that bound it (ADR 0124).
struct RenderStep
{
    std::string name;
    double priority = 0.0;
    core::u64 order = 0;
    int functionRef = -1;
    core::InstanceId owner;
};

struct MessageBinding
{
    // `GameMailbox`, or a scene's serial.
    core::u32 mailbox = 0;
    std::string topic;
    int functionRef = -1;
    core::InstanceId owner;
};

// A message to a scene that has not opened yet: its values, copied at the
// send, as a registry ref to `{ n = count, ... }`.
struct HeldMessage
{
    core::u32 mailbox = 0;
    std::string topic;
    int valuesRef = -1;
};

inline constexpr core::u32 GameMailbox = 0;

// `Enum.SceneLoadStatus`, by value (ADR 0125).
enum class SceneLoadStatus : core::u8
{
    Preparing = 0,
    Ready = 1,
    Activating = 2,
    Done = 3,
    Failed = 4,
    Cancelled = 5,
};

// One `LoadSceneAsync`, as its handle reads it. The host carries it out and
// writes back here; a record outlives its load, so a handle kept afterwards
// still answers.
struct SceneLoadRecord
{
    core::u32 id = 0;
    std::string path;
    std::vector<core::u8> data;
    // The prepared scene's serial: `SceneLoad.Scene`.
    core::u32 scene = 0;
    SceneLoadStatus status = SceneLoadStatus::Preparing;
    core::f64 progress = 0.0;
    std::string error;
    bool activate = true;
    SignalId ready;
    SignalId finished;
};

// What the host is asked to prepare.
struct SceneLoadRequest
{
    core::u32 id = 0;
    std::string path;
};

struct SceneState
{
    // The open scene. Serials start at 1 and are never reused, so a `Scene`
    // kept past its close can tell it is closed.
    core::u32 open = 1;
    core::u32 next = 2;
    // Every scene that is not open and was: closed ones, by the path they had.
    std::map<core::u32, std::string> closed;
    // A scene prepared and not yet open (ADR 0125), with its path; 0 for none.
    core::u32 prepared = 0;
    std::string preparedPath;

    std::vector<OwnedHandler> closeHandlers;
    std::vector<MessageBinding> bindings;
    std::vector<RenderStep> renderSteps;
    core::u64 nextRenderStep = 1;
    std::vector<HeldMessage> held;

    std::vector<SceneLoadRecord> loads;
    core::u32 nextLoad = 1;
    // The load in flight, or 0: one at a time (ADR 0125 §1).
    core::u32 activeLoad = 0;
    bool loadRequested = false;

    // The editor, `dev` and a match's windows: the warnings a person testing
    // a game wants and a player never reads (ADR 0124 §3, §6).
    bool developer = false;
};

// The `Scene` value type and the `scene` global. Once, at boot, before the
// sandbox.
void registerSceneTypes(lua_State* L);

// `RunService:BindToRenderStep` and `UnbindFromRenderStep` (ADR 0136), in
// `services.cpp`'s table, and the steps run, once a drawn frame, by the host
// through `ScriptRuntime::runRenderSteps`.
int runServiceBindToRenderStep(lua_State* L);
int runServiceUnbindFromRenderStep(lua_State* L);
void runRenderSteps(lua_State* L, double dt);

// `game:SendMessage` and `game:BindToMessage`, in `services.cpp`'s table.
int dataModelSendMessage(lua_State* L);
int dataModelBindToMessage(lua_State* L);

// `SceneService.CurrentScene`, which is a `Scene` rather than a scene value and
// is answered here. False when `key` is not it.
bool sceneMemberGet(lua_State* L, core::InstanceId id, std::string_view key);
bool sceneMemberSet(lua_State* L, core::InstanceId id, std::string_view key);

// Pushes the `Scene` for `serial`.
void pushScene(lua_State* L, core::u32 serial);

void setDeveloperWarnings(lua_State* L, bool developer);

// --- What the host does at a close ------------------------------------------

// The open scene's `scene:BindToClose` handlers, each in its own coroutine; one
// that yields is parked with the game's (`closeHandlersPending`).
void runSceneCloseHandlers(lua_State* L);

// **What `GetLoadData` answers, said as JSON** (D468): a run started in a
// scene by name -- `--scene=`, `--scene-data=` -- hands the scene what a
// `LoadScene` call would have. An object or an array becomes a table, the rest
// the plain values they are. False, and nothing changed, when the text is not
// JSON; `diagnostic` then says where.
[[nodiscard]] bool setSceneLoadData(lua_State* L, std::string_view json, std::string* diagnostic = nullptr);

// **The open scene is closed**, before its world is torn down: what its
// scripts registered leaves both close lists and both mailboxes -- a
// `game:BindToClose` of theirs dropped, with the editor's warning -- its own
// mailbox's bindings go, and `next` (a prepared scene's serial, or 0 for a new
// one) is the scene open from here. Returns the new serial.
core::u32 closeScene(lua_State* L, core::u32 next = 0);

// After the new scene's scripts are started: the messages sent to it while it
// was being prepared, delivered (deferred, so behind their first resumption
// and ahead of `SceneLoaded`).
void deliverHeldMessages(lua_State* L);

// A prepared scene (ADR 0125): reserved with its path, so a `Scene` for it can
// be handed out and messages to it held; released when it is cancelled.
[[nodiscard]] core::u32 reserveScene(lua_State* L, std::string_view path);
void releaseScene(lua_State* L, core::u32 serial);

// --- A scene prepared in the background (ADR 0125) ---------------------------

// `SceneService:LoadSceneAsync`, in `services.cpp`'s table.
int sceneServiceLoadSceneAsync(lua_State* L);

// Cancels the load in flight, if there is one, as a second load or a plain
// `LoadScene` does -- and says so in the log.
void cancelSceneLoad(lua_State* L);

// A load a script started since the host last asked, or nothing.
[[nodiscard]] std::optional<SceneLoadRequest> takeSceneLoadRequest(lua_State* L);

// The load in flight, or null: the host's view of what to go on with. A
// record that was cancelled or superseded is not in flight.
[[nodiscard]] SceneLoadRecord* activeSceneLoad(lua_State* L);

// What the host reports as it goes: `Progress`, `Ready` fired, a failure, the
// switch beginning and the switch done -- each firing what the handle promises.
void setSceneLoadProgress(lua_State* L, core::f64 progress);
void sceneLoadReady(lua_State* L);
void sceneLoadFailed(lua_State* L, std::string_view message);
void sceneLoadActivating(lua_State* L);
void sceneLoadDone(lua_State* L);

} // namespace engine::script
