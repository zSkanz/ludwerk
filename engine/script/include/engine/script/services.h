// The DataModel, the services, and the globals that reach them (api-design.md
// §2.1, §1.2).
//
// The tree the world boots with is `game`, with every service under it
// (`registerServices` says why), and `GlobalScriptService`'s three fixed
// folders (ADR 0105). A service is a singleton, an ordinary child of `game`,
// and that rule lives here rather than in `scene` because `scene` has no reason
// to know that some of its classes are singletons.
//
// The phase signals are fired from here too. `RunService.Heartbeat` and its four
// siblings are engine-raised events with no POD fact behind them: nothing in
// `scene` changed, the frame merely advanced, so there is no `Change` for the
// drain to convert and the scheduler enqueues them directly.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/log.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/input/input.h"
#include "engine/nav/nav.h"
#include "engine/net/async_client.h"
#include "engine/net/relay_ping.h"
#include "engine/platform/event.h"
#include "engine/platform/game_integration.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/morph_host.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/physics_sync_2d.h"
#include "engine/scene/skeleton_host.h"
#include "engine/scene/world.h"
#include "engine/script/animation.h"
#include "engine/script/binding.h"
#include "engine/script/content_provider.h"
#include "engine/script/crypto_service.h"
#include "engine/script/detectors.h"
#include "engine/script/optional_service.h"
#include "engine/script/reload_state.h"
#include "engine/script/save_store.h"
#include "engine/script/scenes.h"
#include "engine/script/tweens.h"

struct lua_State;

namespace engine::script {

// Where a gizmo call goes. Null in a headless run, which is why
// `DebugService:DrawLine` is a silent no-op there rather than an error --
// debug drawing left in shared code must not fail a headless test.
//
// A sink rather than a direct call into `render`, because `script` does not
// depend on it: the renderer is L4 and this is L5, and the dependency would run
// the right way but would put a graphics API behind a scripting header for the
// sake of three functions.
struct GizmoSink
{
    void* user = nullptr;
    void (*line)(void* user, core::Vec3 a, core::Vec3 b, core::Color3 color) = nullptr;
    void (*box)(void* user, const core::CFrameD& frame, core::Vec3 size, core::Color3 color) = nullptr;
    void (*sphere)(void* user, core::Vec3 position, f32 radius, core::Color3 color) = nullptr;
};

// **Where things are drawn this frame** (ADR 0136), for `GetRenderCFrame`:
// set by the host for the length of a render phase and cleared after it, so a
// simulation phase -- and every headless run -- has none. The script module
// does not link the renderer; the host answers from its `DrawPoses`.
enum class DrawnKind : core::u8
{
    Part,
    Attachment,
    Camera,
};
struct DrawnPoseSink
{
    void* user = nullptr;
    bool (*pose)(void* user, core::InstanceId id, DrawnKind kind, core::CFrameD& out) = nullptr;
};

// Named instrumentation counters. `GetStat` raises for a name nothing has
// published rather than answering zero: a misspelt stat is a bug in the caller,
// and a debug surface that answers zero hides that bug in the one place people
// are already confused.
struct StatTable
{
    // Insertion-ordered rather than hashed. Nothing iterates it today, but a
    // stats panel will, and R10 forbids a container from deciding what a panel
    // shows first.
    std::vector<std::pair<core::NameAtom, f64>> entries;
};

// The engine's own per-frame instrumentation, published by the host once a
// frame (architecture.md §9). Separate from `StatTable` because these are
// engine facts with fixed names rather than whatever a game chose to publish,
// and because a game must not be able to overwrite one -- `GetStat("FPS")`
// reads the engine's number or nothing at all.
//
// **Wall-clock derived, and therefore never readable by simulation code.** R10
// forbids the sim from seeing a clock; these exist for a human looking at an
// overlay, and a script that fed one back into the world would make its replay
// diverge. `ludwerk check` flags that in M3.
struct FrameStats
{
    f64 fps = 0.0;
    f64 frameTimeMs = 0.0;
    f64 drawCalls = 0.0;
    f64 physicsBodies = 0.0;
    f64 luaMemoryKb = 0.0;
    // The mixer's own two, and the first of them is the M6 gate's number: the
    // roadmap asks for "buffer underrun counter zero in a 60 s soak", and a
    // counter a script cannot read is a gate a human has to take on trust.
    f64 audioUnderruns = 0.0;
    f64 audioVoices = 0.0;
    f64 audioClipsLoaded = 0.0;
    f64 audioClipsMissing = 0.0;
    // Of the loaded ones, how many are long enough to stream (D129).
    f64 audioClipsStreamed = 0.0;

    // How many of this frame's draws used a level of detail COARSER than zero
    // (roadmap M7: "basic LOD switching").
    //
    // Here because a selector nobody can see is a selector nobody can tell is
    // working. It costs nothing to compute -- the frame already walks its draws
    // to count them -- and it is the difference between "LOD switching shipped"
    // and "LOD switching shipped and is choosing something".
    f64 meshLodDraws = 0.0;
    // How many objects the camera could see, and how many of the frame's calls
    // were instanced. `drawCalls` and `visibleObjects` were the same number
    // until M7.5 gave the renderer a way to draw a run of them at once; the
    // roadmap's gate for that item is the pair.
    f64 visibleObjects = 0.0;
    f64 instancedDraws = 0.0;
};

// A coroutine parked on `WaitForChild`. Kept apart from the timer list because
// the contract is about a *state* -- "a child of that name exists" -- and not
// about a deadline: a sibling renamed into the awaited name satisfies a waiter
// exactly as a newly parented child does, so nothing about it can be scheduled.
struct ChildWaiter
{
    core::InstanceId parent;
    core::NameAtom name;
    int threadRef = -1;

    bool hasTimeout = false;
    u64 deadlineTick = 0;

    u64 scheduledTick = 0;
    // The unbounded form warns once after five sim-seconds and keeps waiting.
    // The timeout form never warns, however long its timeout: you said how long
    // you were prepared to wait.
    bool warned = false;
};

// Per-VM. Held by `VmContext` as a pointer and owned by `ScriptRuntime`.
class ServiceState
{
public:
    // Invalid until `registerServices` runs. Every service is a child of it,
    // and `game` is the only way a script reaches it.
    core::InstanceId dataModel;
    struct OptionalIntegration
    {
        std::string library;
        std::string configuration;
        std::unique_ptr<platform::GameIntegration> provider;
        bool attempted = false;
        bool warned = false;
        int waiter = -1;
        IntegrationResponse response = IntegrationResponse::Action;
        bool generic = false;
    };
    std::map<std::string, OptionalIntegration, std::less<>> integrations;
    platform::PlatformServiceConfiguration platformServices;

    GizmoSink gizmos;
    DrawnPoseSink drawnPoses;
    // A render phase has run in this VM: a window draws it (ADR 0136). What
    // tells a `GetRenderCFrame` in a simulation phase that it is a mistake
    // rather than a headless run, where it is the only answer there is.
    bool renderPhasesRun = false;
    // The scripts already warned about that, once each.
    std::vector<core::InstanceId> warnedRenderCFrame;
    StatTable stats;
    FrameStats frameStats;

    // The overlay panels a script has opened, in the order it opened them. The
    // host reads this when it draws; a name outside the documented set never
    // reaches here, because `ShowPanel` raises on one.
    std::vector<core::NameAtom> openPanels;

    // `game:BindToClose` callbacks, in registration order, each with the
    // script that registered it: a scene's scripts' go with the scene (ADR 0124).
    std::vector<OwnedHandler> closeHandlers;

    // The close handlers that yielded and are still parked, as thread refs.
    // Held only between `runCloseHandlers` and the end of the grace period.
    std::vector<int> closePending;
    bool shutdown = false;

    // `InputService:SetClipboard` (ADR 0177): what a script asked to be put on
    // the clipboard, waiting for the host to take it -- the clipboard is the
    // window system's, which this module never reaches.
    std::optional<std::string> clipboard;

    // Scenes as scripts see them: which is open, `scene:BindToClose`, and the
    // two mailboxes (ADR 0124).
    SceneState scenes;

    // `ContentProvider:PreloadAsync` (ADR 0131 §3): the calls parked on it.
    PreloadState preloads;

    // `ClickDetector` and `ProximityPrompt` on this machine (ADR 0126).
    DetectorState detectors;

    std::vector<ChildWaiter> childWaiters;

    // `RemoteFunction` (ADR 0079). Each handler by its instance, as a
    // registry ref; the callers parked in `InvokeServerAsync` by call number;
    // and the handlers that yielded, until they finish and their answer goes.
    struct InvokeHandler
    {
        core::InstanceId remote;
        int functionRef = -1;
    };
    std::vector<InvokeHandler> invokeHandlers;
    // **Calls that arrived at a `RemoteEvent` nobody was listening to yet**
    // (D451), oldest first, for the first connection to be handed in order.
    // And the remotes already warned about for reaching the bound.
    std::vector<scene::RemoteMessage> heldRemoteCalls;
    std::vector<core::InstanceId> heldRemoteWarned;
    struct InvokeWaiter
    {
        core::u32 call = 0;
        int threadRef = -1;
    };
    std::vector<InvokeWaiter> invokeWaiters;
    struct InvokeRunning
    {
        int threadRef = -1;
        core::InstanceId remote;
        core::InstanceId player;
        core::u32 call = 0;
    };
    std::vector<InvokeRunning> invokeRunning;
    core::u32 nextInvoke = 1;

    // `StreamingService.LoadAreaAsync` (M7), the same shape as a child waiter
    // and for the same reason: the caller is parked on a condition the world
    // will satisfy later, and the host is what notices. What a teleport calls
    // before it moves the character, so the destination exists on arrival.
    struct AreaWaiter
    {
        core::DVec3 position;
        core::f64 radius = 0.0;
        int threadRef = -1;
        u64 scheduledTick = 0;
    };
    std::vector<AreaWaiter> areaWaiters;

    // **`Terrain:WaitForMeshAsync`** (ADR 0159), parked the same way: whether
    // the ground is drawn as it will be is the renderer's to know, and the
    // host answers it at the frame's end. `meshed` is the same question asked
    // at once, for `Terrain:IsMeshed`; unset, every area is meshed -- a world
    // with no renderer has nothing to wait for.
    struct MeshWaiter
    {
        core::InstanceId terrain;
        core::DVec3 position;
        core::f64 radius = 0.0;
        // The tick it gives up at, answering false.
        u64 deadlineTick = 0;
        int threadRef = -1;
    };
    std::vector<MeshWaiter> meshWaiters;
    std::function<bool(core::InstanceId, core::DVec3, core::f64)> meshed;

    // `GraphicsService:SaveAsync` and `LoadAsync` (ADR 0147), parked the same
    // way: the player's file is the host's to write and read, at the end of
    // the frame, and the caller is given whether it did.
    struct GraphicsWaiter
    {
        int threadRef = -1;
        // Waiting on a load rather than a save.
        bool load = false;
    };
    std::vector<GraphicsWaiter> graphicsWaiters;

    // The keys `LocalizationService:Translate` has already said nobody has
    // (ADR 0154): each is warned about once, not once a frame.
    std::vector<std::string> missingTranslations;

    // `@std/net.request` (api-design.md 7), the same parking shape as the two
    // waiters above and for the same reason -- except that what satisfies it is
    // a socket rather than the world, so the condition is a TICKET rather than a
    // predicate over world state.
    struct NetWaiter
    {
        net::NetTicket ticket;
        int threadRef = -1;
    };
    std::vector<NetWaiter> netWaiters;

    // Created on the first `@std/net.request` and never before: it owns worker
    // THREADS, and a project that never touches the network should not be paying
    // for two of them. Owned here so it dies with the VM -- the coroutines
    // waiting on its tickets die with the VM too.
    std::unique_ptr<net::AsyncClient> netClient;

    // `NetworkService:PingRelayAsync` (ADR 0178, amended): the same parking
    // shape -- a ticket and the thread that waits on it -- and a pinger made
    // on the first ask and never before, for the reason the client above is:
    // it owns a socket and a thread.
    struct RelayPingWaiter
    {
        net::RelayPingTicket ticket;
        int threadRef = -1;
    };
    std::vector<RelayPingWaiter> relayPingWaiters;
    std::unique_ptr<net::RelayPinger> relayPinger;

    // Resolved once at boot. `fireRunServiceEvent` runs four times a tick and
    // `publishMessage` runs per `print`; hashing a string literal on either path
    // is a cost with no reason to exist.
    core::NameAtom messageOut;
    core::NameAtom loaded;
    core::NameAtom preReload;
    core::NameAtom postReload;
    core::NameAtom instanceStreamedOut;
    core::NameAtom areaLoaded;
    scene::ClassId runServiceClass = 0;
    scene::ClassId tagServiceClass = 0;
    scene::ClassId debugServiceClass = 0;
    scene::ClassId hotReloadServiceClass = 0;
    scene::ClassId streamingServiceClass = 0;

    // The hot-reload bag (ADR 0024). Never null: `ScriptRuntime` owns one so
    // that `SaveState` is never a silent no-op, and the host substitutes its
    // own -- which outlives `WorldHost` -- when it intends the values to
    // survive a reload.
    ReloadState* reload = nullptr;

    // The physics mirror, or null in a build with no physics backend. Set by
    // the host, which owns it: `scene::PhysicsSync` is L3 and reachable from
    // here, but the instance belongs to the world's lifetime.
    //
    // Null is a real state and not an error -- `Workspace:Raycast` answers nil,
    // which is the same answer an empty world gives -- so every reader checks
    // rather than assuming.
    scene::PhysicsSync* physics = nullptr;
    // The plane's mirror (the 2D layer), on the same terms: null answers
    // `Workspace:Raycast2D` with nil.
    scene::PhysicsSync2D* physics2d = nullptr;
    // Navigation over the world (ADR 0089), or null in a build without it:
    // `NavigationService`'s queries then answer nothing, as an empty world's do.
    nav::INavigation* navigation = nullptr;

    // The input system `InputService` reads and, in exactly one place, writes:
    // `SetVirtualState` drives the sixteen virtual channels, which is the seam that
    // lets a HUD button feed an action without becoming a second input model.
    // Null before the host hands it over. Same arrangement as `physics` and
    // `animation`: the system belongs to the host's lifetime and this is a view
    // onto it.
    input::InputSystem* input = nullptr;

    // The animation host, or null in a build with no render module. Same
    // arrangement and same rule as `physics` above: null is a real state, and a
    // track that has no host plays nothing rather than raising.
    scene::AnimationHost* animation = nullptr;

    // The same object as `animation` in every build that has one, through the
    // narrower seam that names joints -- `AnimationHost` is about tracks and
    // clips, and `Ragdoll:Build` needs to ask where an elbow is.
    //
    // A second pointer rather than a cast, because the two are separate
    // interfaces on purpose and a host is free to implement one and not the
    // other. Null is a real state, and `Build` says so rather than raising a
    // sentence about a renderer nobody asked for.
    scene::SkeletonHost* skeleton = nullptr;
    // And the third, for a mesh's morph targets (ADR 0196): what
    // `MeshPart:SetMorphWeight` reaches. Null where nothing plays animation,
    // and then a weight set is a weight nobody would have drawn.
    scene::MorphHost* morph = nullptr;

    // Every `AnimationTrack` handle this VM has handed out. Append-only: a
    // record is four bytes and a signal id, and `LoadAnimation` is a load-once
    // call rather than a per-frame one -- which is what its own doc says.
    std::vector<TrackRecord> animationTracks;

    // Every live tween in this VM (api-design.md §2.1). Per-VM rather than
    // process-global for the same reason everything else here is: two worlds in
    // one process must not share one.
    TweenSystem tweens;

    // **`SaveService`** (ADR 0111). The store is the host's and outlives this
    // VM; null in a run that keeps no saves, where every call raises.
    SaveStore* saves = nullptr;
    // A thread waiting for a slot, and the migration it waits behind.
    struct SlotWaiter
    {
        SaveSlotData* slot = nullptr;
        int threadRef = -1;
        int migrateRef = -1;
    };
    std::vector<SlotWaiter> slotWaiters;
    // A thread waiting for `SaveAsync`'s write.
    struct SaveWaiter
    {
        u64 ticket = 0;
        int threadRef = -1;
        std::string slot;
    };
    std::vector<SaveWaiter> saveWaiters;
    // `SaveService.OnMigrate`, or -1.
    int migrateHandler = -1;
    // Each slot's `Changed`, by name, made when first asked for.
    std::map<std::string, SignalId, std::less<>> slotSignals;

    // **`CryptoService`** (ADR 0151): the thread passwords are hashed on, made
    // by the first one, and the threads waiting on it.
    struct PasswordWaiter
    {
        u64 ticket = 0;
        int threadRef = -1;
    };
    std::vector<PasswordWaiter> passwordWaiters;
    std::unique_ptr<PasswordWorker> passwords;
};

// Creates `game` and the two services that exist from boot, installs the
// `game` and `workspace` globals, and binds every service method. Runs during
// boot, before the sandbox.
// `adopt` is a `DataModel` this world already has, and it is what makes a VM
// replaceable without replacing the world (ADR 0058: stop tears the runtime down
// and builds a fresh one). Invalid -- the default, and every boot -- creates one.
//
// Adopting rather than creating is the whole difference, because everything
// under it is FOUND: `getServiceOfClass` has always been find-or-create, so a
// second boot over the same tree re-binds `Workspace`, `Lighting` and the rest
// instead of building a second set beside them.
void registerServices(lua_State* L, core::InstanceId adopt = {});

// What the host publishes each frame. Called between frames rather than inside
// one, so a stat never changes halfway through a tick that might read it.
void publishFrameStats(lua_State* L, const FrameStats& stats);

// `DebugService.MessageOut`, which carries every console message with its level
// (api-design.md §2.1). Called beside the log rather than from a log sink: the
// sink is a process-wide slot the host owns, and a signal system that competed
// for it would silently lose to whoever installed one last.
void publishMessage(lua_State* L, core::LogLevel level, std::string_view text);

// `StreamingService.InstanceStreamedOut`, one fire per instance that became a
// husk. A husk is REPARENTED TO NIL rather than destroyed, so the handle a
// script holds still resolves when the handler reads it (§4).
void fireStreamedOut(lua_State* L, core::InstanceId instance);

// `SceneService.SceneLoaded` (ADR 0106), fired by the host once the new scene
// is in place and its scripts are started -- deferred, so it runs after their
// first resumption.
void fireSceneLoaded(lua_State* L, std::string_view path);
// `SceneLoadFailed` (audit A8): a `LoadScene` refused before anything closed.
void fireSceneLoadFailed(lua_State* L, std::string_view path, std::string_view message);
// `SceneLoading`, fired by the host when a prepared scene's switch begins
// (ADR 0125 §3); `LoadScene` fires it at the call.
void fireSceneLoading(lua_State* L, std::string_view path);

// **What crossed between two worlds** (ADR 0107 §3), fired once a tick at its
// start beside the network's messages: each `SubWorld`'s `Received`, with what
// its sub-world sent out, and -- inside a sub-world -- `SceneService.
// HostMessageReceived`, with what the world running it sent in.
void fireSubWorldMessages(lua_State* L);

// One of `NetworkService`'s `Connected`, `JoinFailed` or `Disconnected` (ADR
// 0106), fired by the host when the connection it manages changes; the last
// two carry a readable reason.
void fireNetworkEvent(lua_State* L, std::string_view eventName, std::optional<std::string_view> reason);

// Wakes every `GraphicsService:SaveAsync` (or, with `load`, `LoadAsync`) with
// whether the host did it (ADR 0147).
void resumeGraphicsWaiters(lua_State* L, bool load, bool done);

// Wakes every `LoadAreaAsync` whose area is now resident and fires
// `AreaLoaded` for it. `resident` is the host's answer, because whether a
// region is loaded is a question only the streaming host can answer and this
// module must not learn what a chunk is.
void resumeAreaWaiters(lua_State* L, const std::function<bool(core::DVec3, core::f64)>& resident);

// **The host's answer to "is this ground meshed"** (ADR 0159): kept for
// `Terrain:IsMeshed`, and asked of every `WaitForMeshAsync` parked -- resumed
// with true when its area is, false once its deadline passes.
void setTerrainMeshed(lua_State* L, std::function<bool(core::InstanceId, core::DVec3, core::f64)> meshed);
void resumeMeshWaiters(lua_State* L);

// Enqueues one of `RunService`'s phase signals with its delta in seconds. The
// scheduler calls this at each resumption point; the fire drains like any other.
void fireRunServiceEvent(lua_State* L, core::NameAtom event, f64 delta);

// Enqueues `game.Loaded`. Fires once, after every entry script has had its first
// resumption (api-design.md §3).
void fireDataModelLoaded(lua_State* L);

// Enqueues `HotReloadService.PreReload` on the outgoing world, or `PostReload`
// on the world a reload built. A no-op when no script ever asked for the
// service, because then there is no instance and nothing can have connected.
void fireHotReloadEvent(lua_State* L, bool before);

// Points the bindings at the bag the host owns. Called before any script runs;
// the runtime's own bag is what they use until it is.
void setReloadState(lua_State* L, ReloadState* state);

// Wakes every `WaitForChild` whose awaited child now exists, and expires the
// ones whose timeout has passed. Called from the task-resume phase, because a
// waiter is a pending resumption like any other -- but keyed on a tree state
// rather than on a deadline, so it cannot live in the timer list.
void resumeChildWaiters(lua_State* L);

// Resumes every thread whose `PingRelayAsync` has its answer, or has waited
// its time out -- at the frame's safe point, as `resumeNetWaiters` is.
void resumeRelayPings(lua_State* L);

// Whether a `BindToClose` callback asked the run to end, or a script called
// `Shutdown`. The host polls it; nothing here can end a process.
[[nodiscard]] bool shutdownRequested(lua_State* L);

// The text `InputService:SetClipboard` was last given, once: the host takes
// it after the tick and hands it to the window system. Nothing where no script
// asked.
[[nodiscard]] std::optional<std::string> takeClipboardText(lua_State* L);

// Runs the registered `BindToClose` callbacks. They are given their timeout by
// the host and the shutdown proceeds when it expires, finished or not: a close
// handler is a chance to finish, never a veto.
void runCloseHandlers(lua_State* L);

// Whether any close handler is still parked. The host polls this while it
// spends the grace period `architecture.md` §app promises -- before M5 a
// handler that yielded was simply cut off at the next drain (D016).
[[nodiscard]] bool closeHandlersPending(lua_State* L);

// Whether a script has asked for an overlay panel by name
// (`DebugService:ShowPanel`). The host reads it to decide what to draw; M5's
// first reader is the physics wireframe, which is expensive enough that
// drawing it unasked would be a frame cost nobody chose.
[[nodiscard]] bool panelOpen(lua_State* L, std::string_view name);

// Lets go of whatever is still parked when the grace period runs out. The
// process is going away; what this releases is the reference, so the VM can be
// torn down without a live thread rooted in the registry.
void abandonCloseHandlers(lua_State* L);

} // namespace engine::script
