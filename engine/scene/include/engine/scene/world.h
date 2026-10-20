// The scene: an ECS underneath, an Instance tree on top (architecture.md §4,
// ADR 0026, ADR 0028).
//
// `World` owns entity lifetime, the hierarchy, names, attributes, tags and the
// change queue. It holds no `lua_State` and includes no Luau header; the
// bindings that turn a `Value` into a Luau value live in `script` (L5), and the
// facts this produces mean something in a headless world with no VM at all.
//
// Tree mutation is **synchronous** and its notifications are **deferred**
// (api-design.md §3.1). `destroy` removes the subtree before it returns; what
// waits for the next resumption point is the telling, not the doing. Getting
// that backwards would make `part.Parent = nil` followed by a `GetChildren`
// read inconsistent, which is the kind of thing scripts are written against
// without anyone thinking about it.
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/asset/material.h"
#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/core/random.h"
#include "engine/core/slotmap.h"
#include "engine/core/text_key.h"
#include "engine/scene/change_queue.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/collision_groups.h"
#include "engine/scene/component_pool.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/graphics_model.h"
#include "engine/scene/localization.h"
#include "engine/scene/screen_split.h"
#include "engine/scene/sequence_files.h"
#include "engine/scene/tween_clock.h"
#include "engine/scene/types.h"
#include "engine/scene/value.h"

namespace engine::scene {

// Intrusive links plus the duplicate-name chain (ADR 0026). `lastChild` exists
// so that appending is O(1) -- child order is parenting order, and every parent
// operation appends.
struct InstanceRecord
{
    ClassId classId = InvalidClass;
    core::NameAtom name;

    core::InstanceId parent;
    core::InstanceId firstChild;
    core::InstanceId lastChild;
    core::InstanceId prevSibling;
    core::InstanceId nextSibling;
    u32 childCount = 0;

    // The next sibling sharing this instance's name, in child order. Eight
    // bytes per instance to make `FindFirstChild` O(1) on a parent with three
    // children called "Tree" -- the case a plain name map would silently
    // clobber (ADR 0026).
    core::InstanceId nextSameName;

    // Set by `destroy` and never cleared. The handle still resolves until the
    // end of the drain that carries its `Destroying`, and during that window
    // `Parent` is locked (api-design.md divergence #25).
    bool destroyed = false;

    // **Created by a system rather than authored by a person**, and therefore
    // not part of a scene.
    //
    // A streamed chunk is the case that forced this: `StreamingGlue` puts a
    // `Folder` of real `Part`s into `Workspace` as a focus moves, and a save
    // taken at that moment wrote a recording of where the streaming happened to
    // be -- 40 chunks and 1.4 MB of terrain that already has a format of its
    // own, frozen into a file that is supposed to describe a project. Unity
    // spells the same idea `HideFlags.DontSave`.
    //
    // It is deliberately NOT in the world hash. The hash asks what the
    // simulation is, and a streamed part is as real to a tick as an authored
    // one; this asks what a person wrote down, which is a different question
    // with a different answer.
    bool generated = false;

    // **Made by a script that does not run on the server side** (ADR 0186),
    // and so this machine's alone: never sent to another, with everything
    // under it, wherever it is parented -- a listen host included, whose
    // client scripts share one world with its server's.
    //
    // A fact about who made it, set when it is made and never after. Not in
    // the world hash, for the reason `generated` is not: a part a client
    // script made ticks like any other, and what is asked here is who else
    // is told about it.
    bool local = false;

    // **When a property of it was last written through the world**: the
    // world's count of mutations at that write (`World::mutations`), 0 for
    // never. What a reader that keeps a copy of an instance asks before it
    // trusts the copy -- the replication capture, which else learnt of a
    // write that changed no byte it compares only when it next read the
    // instance for no reason. A stamp and not a list of the written: any
    // number of readers each keep their own place, and nothing grows.
    //
    // Not in the world hash: it says when something was written, and the
    // hash asks what is there. A quiet write straight into a component (the
    // physics mirror's) does not stamp; a reader that cares compares bytes.
    core::u64 written = 0;

    // **The STAMP this instance was made from**, or an empty atom (ADR 0049).
    //
    // The same kind of fact as `generated` above and stored beside it for the
    // same reasons: it is what a PERSON wrote down rather than what the
    // simulation is, it travels in a snapshot so undo and stop keep it, and it
    // is deliberately not in the world hash -- a stamped part ticks like any
    // other, and two worlds that differ only in where their parts came from are
    // the same world to a solver.
    //
    // An atom rather than a string because it is a path repeated across every
    // instance of one stamp: forty lamp posts intern one name.
    core::NameAtom stamp;

    // **Which node of its stamp this is** (ADR 0155 §2): eight hex digits in
    // the file, given once and never changed, so a copy's overrides name the
    // node whatever it is called and wherever it sits. Zero for a node no stamp
    // made. Not in the world hash, for the reason the mark is not.
    u32 stampSid = 0;

    // **A stamp's declared parameters** (ADR 0155 §6), on a copy's root: the
    // file's `parameters` array, as its text. An atom for the economy the mark
    // has -- forty copies of one stamp intern one declaration.
    core::NameAtom stampParameters;

    // **Overrides a copy holds for nodes its stamp no longer has** (ADR 0155
    // §14), as the file's JSON object, key by key: kept, so a save does not
    // lose them and a stamp that brings the node back finds them, and listed,
    // so a person can clean them up.
    core::NameAtom stampOrphans;

    // **Built by its stamp's `Construct`** (ADR 0155 §8): never saved, never an
    // override, and built again from the parameters each time. Kept through a
    // clone, so a copy of a copy is constructed as the first was.
    bool constructed = false;

    // **Where this instance was authored** (ADR 0138 §6): the tree a scene read
    // made it in (`scene:Workspace`, `scene:ReplicatedStorage`, ...) or the stamp
    // `Instance.stamp` placed it from (`stamp:doors/front`), and its place in
    // that tree's preorder. A replica, which is never sent a script, finds the
    // scripts its own package holds for an instance the authority sends by
    // this pair. Kept through a clone, so a copy of a door is a door; not in
    // the world hash, and not saved.
    core::NameAtom origin;
    u32 originIndex = 0;

    // **Made from a file under `src/`** (ADR 0092): a `Script` the mount
    // read, or a `Folder` it made to hold one. The FILE is its source, so a
    // scene does not write it -- but what somebody put inside it is theirs and
    // is written, hung on a mark that finds it again at the next open. The
    // same kind of fact as `generated`: what a person wrote down, carried by a
    // snapshot, and not in the world hash. A clone does not inherit it -- a copy
    // of a file's script is a script of its own.
    bool mounted = false;

    // **Made by the engine and not to be moved** (ADR 0105): the `Server`,
    // `Client` and `Shared` folders of `GlobalScriptService`, whose place in the
    // tree is what decides where the code inside them runs. A rename, a
    // reparent or a destroy of one is refused with a keyed error. Not in the
    // world hash, and not inherited by a clone.
    bool fixed = false;

    // Which of the class's first 64 properties have a listener. A write to an
    // unsubscribed property enqueues nothing, which is what lets 10k parts move
    // every tick for free while nobody is watching (architecture.md §4). Past
    // 64 properties the mask saturates and every write enqueues -- correct, and
    // slower, for a class nothing in v1 has.
    u64 subscribedProperties = 0;
};

// Insertion-ordered rather than hashed: `GetAttributes` and the world hash both
// walk this, and R10 forbids either observing a container's own order.
using AttributeMap = std::vector<std::pair<core::NameAtom, Value>>;
using TagSet = std::vector<core::NameAtom>;

// Engine state that a *service* property reads.
//
// `RunService.SimTime` and `PhysicsService.FixedTimestep` are produced by the
// frame scheduler, which lives in `app` at L6 -- and the descriptors that
// expose them are scene's, at L3, which cannot see it. So the scheduler writes
// here and the accessors read here. One instance per world rather than a
// component per service, because there is exactly one of each service in a
// world and a component would be ceremony around a single struct.
// The four postures a process can run in (ADR 0070), as `NetworkService`
// reports them. Numbered as `Enum.NetworkTopology` is, and as
// `replication::Topology` is -- `scene` cannot see that module (it is L4), so
// the numbers are the agreement.
enum class NetworkTopology : u8
{
    Solo = 0,
    Host = 1,
    Dedicated = 2,
    Replica = 3,
};

// A message a script sent through a `RemoteEvent` (ADR 0077), on its way.
//
// **The payload is the script module's and nobody else's.** Only it can read a
// Luau value, so it encodes the arguments and decodes them; everything between
// -- this queue, the replication engine, the wire -- carries bytes it never
// parses. The instances the arguments name ride beside them in `refs`, because
// an instance id means nothing on another machine and the replication engine is
// what translates it.
struct RemoteMessage
{
    core::InstanceId remote;
    // True for `FireServer` -- on its way to, or arrived at, the authority.
    bool toServer = false;
    // Leaving an authority: the user id it is for, 0 for every player.
    u32 userId = 0;
    // Arrived at an authority: the player who sent it.
    core::InstanceId player;
    std::vector<u8> payload;
    std::vector<core::InstanceId> refs;
    // `RemoteFunction` (ADR 0079): the caller's number for this question, zero
    // for a `RemoteEvent` message. With `reply`, this is the answer to it, and
    // with `failed` the answer is the error the handler raised, as one string.
    u32 call = 0;
    bool reply = false;
    bool failed = false;
    // From an `UnreliableRemoteEvent` (ADR 0161): sent once, on a channel
    // that is never sent again, and never held for anything.
    bool unreliable = false;
    // How many sends it has waited through for its event to reach the network:
    // an event created since the authority last captured has no network id
    // yet, and a replica not yet welcomed has nobody to send to.
    u16 held = 0;
};

// ADR 0126: a click, on its way from a replica to the authority or arrived at
// one -- which checks it and fires it with the player.
struct DetectorMessage
{
    enum class Kind : u8
    {
        Click,
        RightClick,
    };
    // The `ClickDetector`.
    core::InstanceId detector;
    // Arrived at an authority: the player who sent it.
    core::InstanceId player;
    Kind kind = Kind::Click;
    // Sends waited through for the detector to get a network id.
    u16 held = 0;
};

class World;

// **The player's own sound and subtitle settings** (ADR 0200): the four
// volumes of `AudioService` and the three subtitle settings of
// `DialogueService`. Saved with the player's preferences, only where the
// player changed them.
struct PlayerSound
{
    f32 playerVolume = 1.0f;
    f32 musicVolume = 1.0f;
    f32 effectsVolume = 1.0f;
    f32 voiceVolume = 1.0f;
    // `Enum.SubtitleMode`: 0 Auto, 1 On, 2 Off.
    i32 subtitles = 0;
    f32 subtitleScale = 1.0f;
    f32 subtitleBackground = 0.6f;
    // Which of the three the player has said: what goes in their file.
    bool subtitlesSaid = false;
    bool subtitleScaleSaid = false;
    bool subtitleBackgroundSaid = false;

    [[nodiscard]] bool operator==(const PlayerSound&) const noexcept = default;
};

// **What a machine's mixer can say of the sound it is playing** (ADR 0200
// section 9): how loud the recording is at the play position, and how much of
// it is low, middle and high. Answered by the audio system, which `scene`
// cannot see; asked by `Sound.Loudness`, `Sound:GetBands` and
// `AudioService:GetBands`. Picture, each machine's own: nothing a tick reads.
class SoundMeter
{
public:
    virtual ~SoundMeter() = default;
    // 0 to 1; 0 for a sound that is not playing or has no recording here.
    [[nodiscard]] virtual f32 loudness(const World& world, core::InstanceId sound) = 0;
    // Low, middle, high, each 0 to 1.
    [[nodiscard]] virtual std::array<f32, 3> bands(const World& world, core::InstanceId sound) = 0;
    // The same of every audible sound of one `Enum.SoundCategory`, mixed.
    [[nodiscard]] virtual std::array<f32, 3> categoryBands(const World& world, i32 category) = 0;
    // The language a sound is heard in on this machine: the locale of the
    // file it resolves to, empty for the default language's -- and nothing at
    // all for a sound with no recording in any, which is a line that is only
    // its caption. What "is this in the language being read?" is asked of.
    [[nodiscard]] virtual std::optional<std::string> spokenIn(const World& world, core::InstanceId sound) = 0;
};

struct EngineState
{
    // Guest ids occupy a separate range from transport-assigned peer ids.
    u32 nextLocalPlayerId = 0x80000000u;
    // Host configuration, not replicated simulation state (ADR 0188).
    std::vector<std::string> enabledIntegrations;
    // **The game's Nakama** (ADR 0242), where its integration is on: the
    // server's address and its client key, from `[integrations.nakama]
    // configuration`; and the session this machine signed in with.
    std::string nakamaUrl;
    std::string nakamaServerKey;
    std::string nakamaSession;

    // Seconds, constant for the whole tick and advanced by the scheduler
    // (api-design.md §2.1).
    f64 simTime = 0.0;
    // The tick index `simTime` corresponds to. Held as an integer because every
    // `task` deadline is computed in whole ticks and never by comparing floats:
    // at dt = 1/60, `1 / (1/60)` is 60.000000000000007, and a deadline derived
    // from that float is a `task.wait(1)` that lasts 61 ticks (api-design.md
    // §3.2).
    u64 tick = 0;
    // The tick the scheduler is running on. Changed only at a FrameStart safe
    // point, never mid-frame: the accumulator, the timer wheel and the solver
    // all read it, and a value that changed between two of those reads inside
    // one frame is a class of bug worth designing out (api-design.md §2.1).
    f64 fixedTimestep = 1.0 / 60.0;
    // What `PhysicsService.FixedTimestep` was last written to, and what reading
    // it gives back. The scheduler copies this into `fixedTimestep` at the next
    // safe point, so a write round-trips immediately and takes effect one frame
    // later -- which is what the property's Doc promises.
    f64 requestedFixedTimestep = 1.0 / 60.0;
    bool paused = false;
    bool overlayVisible = false;

    // `StreamingService`'s knobs. Here rather than in a component for the
    // same reason `FixedTimestep` is: a service with one instance and no
    // hierarchy of its own has nothing a component would buy, and the host
    // reads these once per frame to build the manager's focus list.
    //
    // The radii are in metres and the pair is a RANGE rather than a value: the
    // engine keeps chunks inside `streamingLoadRadius` and guarantees the ones
    // inside `streamingMinRadius` before a focus may advance into them.
    bool streamingEnabled = true;
    f64 streamingLoadRadius = 1024.0;
    // **What no player is near sleeps** (ADR 0234), on a dedicated server:
    // `[server] dormant_radius`, metres from every player's focus; nought
    // never, below nought twice `streamingLoadRadius`. And the players' foci
    // as the physics step last took them, with whether one of them had none
    // -- which keeps everything awake.
    f64 dormantRadius = -1.0;
    // `[server] physics_threads`: the worker threads this world's physics
    // steps on, beside the tick's own (ADR 0228). Four unless a server says.
    u32 physicsThreads = 4;
    // **How fast this world's clock runs against the wall's** (ADR 0233): 1
    // at full speed; less while an overloaded authority is slowed, which its
    // replicas run at too. How often ticks run, never what one does.
    f64 timeDilation = 1.0;
    // **How fast a replica's own clock runs against that rate**: 1, or a few
    // percent less while its authority holds more of its intents than it
    // means to -- a clock that runs ahead is slowed, never its intents
    // dropped. Not a rate scripts are told of.
    f64 clockPace = 1.0;
    // **The authority's tick for a tick this machine steps** (ADR 0254): its
    // own tick plus this, which a replica learns from each answer; and
    // whether it steps ahead of an authority at all. Zero and false on the
    // authority and alone. What a world step is told its tick is.
    i64 worldStepOffset = 0;
    bool worldStepAhead = false;
    // What the world steps took on the last tick, replays of it included, in
    // milliseconds: the physics mirror's measure, for the network's stats.
    f64 worldStepMs = 0.0;
    // **A tween on a part's place, as its replicas are to know it** (ADR
    // 0254 §2.1): its timing and its clock as of `tick`, after that tick's
    // step, the two ends of this traversal and where the part is now -- from
    // which a replica takes the same steps to any later tick. Made by
    // `TweenService` when a traversal begins or the tween's state changes;
    // `kind` 0 says the tween let go of the part. The last second's: the
    // authority takes each once, at its next snapshot.
    struct MovingCurve
    {
        core::InstanceId part;
        // 0 none, 1 its `CFrame`, 2 its `Position`.
        u8 kind = 0;
        u64 tick = 0;
        TweenTiming timing;
        TweenClock clock;
        core::CFrameD start;
        core::CFrameD goal;
        core::CFrameD now;
    };
    std::vector<MovingCurve> movingCurvesOut;
    // **The phases this world has named** (ADR 0231), each a bit: its place
    // here. At most `MostPhases`.
    std::vector<core::NameAtom> phaseNames;
    // **Where what can be shot was** (ADR 0235), on an authority: each tick's
    // transforms of every player's character and every `LagCompensated` part,
    // oldest first, as many ticks as `rewindCapMs` reaches; and that cap,
    // `[network] rewind_cap_ms`. The host's, never the simulation's.
    struct RewindEntry
    {
        core::InstanceId id;
        core::CFrameD cframe;
    };
    struct RewindFrame
    {
        u64 tick = 0;
        std::vector<RewindEntry> entries;
    };
    std::deque<RewindFrame> rewindFrames;
    f64 rewindCapMs = 200.0;
    std::vector<core::DVec3> playerFoci;
    bool focusless = false;
    f64 streamingMinRadius = 512.0;
    bool streamingPauseOutsideLoadedArea = false;

    // The other two size classes' radii (ADR 0053). A cell's `layer` is its
    // class -- 0 detail, 1 structures, 2 terrain features -- and the pair above
    // is layer 0's, which is why a world whose cells are all layer 0 needs
    // nothing here and behaves exactly as it did.
    //
    // **Zero means "follow the pair above"**, rather than "a radius of zero".
    // The alternative is defaulting them to 512 and 1024 as well, and then a
    // person who raises `LoadRadius` alone finds their terrain no further away
    // than before -- a knob that silently stops applying is worse than one that
    // has to be read about.
    f64 streamingStructureMinRadius = 0.0;
    f64 streamingStructureLoadRadius = 0.0;
    f64 streamingTerrainMinRadius = 0.0;
    f64 streamingTerrainLoadRadius = 0.0;

    // `InputService`'s own state. The pointer position is a SNAPSHOT taken
    // once per frame, like the physics work's keyboard: two reads inside one tick agree, and a
    // recorded input stream can hand the same answer back with no mouse.
    core::Vec2 pointerPosition;
    // **Where the frame last drew the current camera** (ADR 0134): the ray
    // under the pointer is the one through the picture the player clicked,
    // not the tick's camera, which a following camera leaves up to a tick
    // ahead of it. Written by a frame; a world no frame draws has none, and
    // its pointer goes through the tick's camera.
    core::CFrameD drawnCamera;
    bool drawnCameraValid = false;
    // **A sequence has the view** (ADR 0204): the main view is drawn through
    // its camera track and not through `viewTakenCamera`, the workspace's
    // current one, which nothing wrote. Where that is, for
    // `Camera:GetRenderCFrame` in a render phase. Written by a frame.
    bool viewTaken = false;
    core::InstanceId viewTakenCamera;
    core::CFrameD viewTakenCFrame;
    // **A render phase is running** (ADR 0136): a render step, `PreRender`, or
    // an action at `Rate = Render`. A camera written now is presented, and
    // `GetRenderCFrame` answers where things are drawn. Never true without a
    // window, so nothing a replay or a gate runs sees it.
    bool renderPhase = false;
    bool pointerLocked = false;
    bool pointerVisible = true;
    // `Enum.InputDeviceType`: 0 KeyboardMouse, 1 Gamepad, 2 Touch.
    i32 lastInputDeviceType = 0;
    i32 preferredGamepadType = 0;
    u32 preferredGamepadId = 0;
    // Players taken out of the list whose `PlayerRemoving` is still to be
    // heard: destroyed at the start of the next tick.
    std::vector<core::InstanceId> leavingPlayers;
    // **What this machine has to play with**, before anybody has used
    // any of it: `InputService.TouchAvailable`, `KeyboardAvailable` and
    // `GamepadAvailable`. Written by the host before the first script runs
    // and kept up as devices come and go; a world with no host has none.
    // `InputService.SwipeThreshold`, in millimetres.
    f32 swipeThreshold = 6.0f;
    bool touchAvailable = false;
    bool keyboardAvailable = false;
    bool gamepadAvailable = false;

    // `UIService`'s two read-only numbers. Both are properties of the
    // WINDOW, written by the host each frame, and both are zero-ish on a
    // desktop -- which is exactly why a HUD that ignores the insets looks fine
    // until somebody runs it on a phone.
    core::Rect safeAreaInsets;
    f32 displayScale = 1.0f;
    // `UIService.ScreenOrientation`: `Enum.ScreenOrientation`'s value, which the
    // host applies to the window whenever it changes. 2 is LandscapeSensor.
    i32 screenOrientation = 2;
    // **`GraphicsService`'s settings** (ADR 0147): the layers and what a
    // script wrote into them. The host puts its own layers back every frame
    // and applies what changed. `graphicsDisplay` is whether there is a
    // display to apply them to -- a dedicated server has none, and a write
    // there is refused. `graphicsQualityChanged` asks for `QualityChanged`.
    GraphicsModel graphics;
    bool graphicsDisplay = true;
    // **`LocalizationService.Locale`** (ADR 0154): the locale text is asked
    // for in. The player's, as the settings above are: set by the host at the
    // start and by a script from a language menu, and kept by the host.
    std::string locale = "en";
    // `UIService.SelectedObject` and `AutoSelect` (ADR 0128): what a gamepad
    // or the arrow keys have selected. Here rather than beside the hover and
    // the focus because a script writes it.
    core::InstanceId uiSelected;
    bool uiAutoSelect = true;
    core::u32 uiGamepadId = 0;
    // `Camera.ViewportSize`: what the world is drawn into this frame, in pixels
    // -- the window, or the editor's Viewport panel. Written by the host.
    core::Vec2 viewportSize;
    // **The views the window is shared between this frame** (ADR 0205): none
    // while it is one picture. Written by the host once a frame, and read by
    // whatever must agree with the picture -- a camera's
    // `WorldToViewportPoint`, the pointer's ray, a player's interface.
    std::array<ScreenView, 4> screenViews{};
    core::u32 screenViewCount = 0;

    // `AudioService.MasterVolume`. Here for the same reason
    // `PhysicsService.FixedTimestep` is: one of each service per world, and a
    // component around a single float would be ceremony.
    f32 masterVolume = 1.0f;
    // `AudioService.MusicUnderVoice` (ADR 0200): what music is multiplied by
    // while a voice is heard. The game's, as the master is.
    f32 musicUnderVoice = 0.4f;

    // **How the player hears and reads** (ADR 0200): the voice language, the
    // four volumes, the subtitles. Each machine's own, as `locale` is -- the
    // host sets them at the start and keeps what a script writes; none of it
    // is hashed, saved with a scene or sent.
    //
    // `voiceChoice` is what the player chose, empty while the voice follows
    // the text language; `voiceLocale` is the language in force, which the
    // setters and the host keep narrowed to `voiceLocales`. `voiceLocales` is
    // what this machine can play -- the project's default first -- and
    // `systemLocales` the platform's own list, in its order.
    std::string voiceChoice;
    std::string voiceLocale = "en";
    std::vector<std::string> voiceLocales;
    std::vector<std::string> systemLocales;
    PlayerSound playerSound;
    // **The captions current on this machine**, in the order their sounds
    // started: written by the frame from what is heard here, read by
    // `DialogueService:GetCaptions`. Never by a tick.
    std::vector<core::InstanceId> captions;
    std::string engineVersion;
    std::string luauVersion;

    // **What `NetworkService` reports**. Written by the host from the
    // replication module's status, once a frame, and never by a script. All
    // three are facts about the process rather than the world, so none of them
    // reaches the world hash: a trace recorded as a host replays solo.
    //
    // Solo's values are the defaults, and they are the solo truth.
    NetworkTopology networkTopology = NetworkTopology::Solo;
    u64 networkServerTick = 0;
    // The authority's tick now, as far as this machine can tell: on a
    // replica, counted on between the snapshots that put it right.
    u64 networkServerClock = 0;
    // **Whether this machine works `IrradianceVolume`s out, and takes
    // `ReflectionProbe`s' pictures** (ADRs 0202, 0203): said by whatever does,
    // every frame. False where nothing does -- a server, a run with nothing
    // drawn, a machine below the level -- and there a volume and a probe are
    // ready the moment they are asked, because nothing more will be done.
    bool volumesWorked = false;
    bool probesTaken = false;
    u32 networkPeerCount = 0;
    // The port of this machine's match, hosted or joined; zero with none.
    u16 networkPort = 0;

    // **How the connection is doing**, for
    // `NetworkService:GetStats` and the overlay's Network panel. Facts about
    // the process, as the three above; zero solo.
    struct NetworkStats
    {
        // Whether the match is encrypted (ADR 0224): a development run that is
        // not says so where its numbers are read.
        bool encrypted = true;
        // What the socket carried, headers and encryption in it.
        u64 wireBytesSent = 0;
        u64 wireBytesReceived = 0;
        // **What the players' budgets held back** (ADR 0229), on an
        // authority: updates left for later in the last second, and how much
        // of the budgets that second spent, 0 to 1.
        u32 deferredPerSecond = 0;
        f64 budgetUsed = 0.0;
        f64 pingMs = 0.0;
        f64 jitterMs = 0.0;
        f64 lossPercent = 0.0;
        f64 snapshotsPerSecond = 0.0;
        f64 correctionsPerSecond = 0.0;
        // Every correction since the session began, counted, not a rate: what
        // a test or a game's own counter reads exactly.
        u64 corrections = 0;
        f64 lastCorrectionMetres = 0.0;
        // **Each player's round trip** (`Player:GetNetworkPing`), in
        // milliseconds, by `UserId`, sorted: on an authority every player's,
        // on a replica this machine's own players'.
        std::vector<std::pair<u32, u32>> playerPings;
        // The furthest a predicted part was from the authority's at the last
        // answer (ADR 0213), and which part that was.
        f64 predictedDriftMetres = 0.0;
        core::InstanceId predictedDriftPart;
        // **The same, a player at this machine** (ADR 0208): its first
        // player, then its guests by seat, each with a character of its own
        // that this machine predicts. A correction is counted for the
        // character that was wrong and for no other, so "it is laggy for
        // player two only" is something the overlay shows and a log says.
        struct LocalPlayer
        {
            bool predicted = false;
            u32 userId = 0;
            u64 corrections = 0;
            f64 correctionsPerSecond = 0.0;
            f64 lastCorrectionMetres = 0.0;
        };
        std::array<LocalPlayer, 4> localPlayers{};
        u32 inputBufferDepth = 0;
        u64 inputStarvations = 0;
        // How many times the authority anchored this player's input again
        // because its clock had moved, and how far in the past a
        // client draws the others, in milliseconds, as the link has made it.
        u64 inputReanchors = 0;
        f64 interpolationDelayMs = 0.0;
        // A replica's own simulation (ADR 0133): the loose parts it predicts,
        // the re-simulations a second and the ticks they stepped, and what
        // one took on average over that second, in milliseconds.
        u32 predictedParts = 0;
        // ADR 0254: the parts moving on their own drawn where the authority
        // will have them, and what this machine's world step took a tick,
        // replays in it, in milliseconds.
        u32 movingParts = 0;
        f64 worldStepMs = 0.0;
        f64 resimulationsPerSecond = 0.0;
        f64 resimulatedTicksPerSecond = 0.0;
        f64 resimulationMs = 0.0;
        // `UnreliableRemoteEvent` messages (ADR 0161): what this machine sent,
        // took in, and refused or dropped itself. What the network lost in
        // between is the loss above.
        u64 unreliableSent = 0;
        u64 unreliableReceived = 0;
        u64 unreliableDropped = 0;
        // Everything this machine's session sent and took in, in bytes, since
        // it began; and of those, a replicated swarm's positions (ADR 0162).
        u64 bytesSent = 0;
        u64 bytesReceived = 0;
        u64 swarmBytes = 0;
        // **Where those bytes went**, sent and taken in together -- each kind
        // travels one way: the world's state (snapshots), attributes, a
        // game's `RemoteEvent`s and `RemoteFunction`s, its
        // `UnreliableRemoteEvent`s, and what a client says of its own input
        // and of what it owns. What is left of the total is the comings and
        // goings of instances, the ground and the handshake.
        u64 snapshotBytes = 0;
        u64 attributeBytes = 0;
        u64 remoteBytes = 0;
        u64 unreliableBytes = 0;
        u64 inputBytes = 0;
        // How the other end was reached (ADR 0178): 0 nobody, 1 the same
        // network, 2 across the internet, 3 through the relay.
        u8 path = 0;
    };
    NetworkStats networkStats;

    // `RemoteEvent` messages: those a script sent, waiting for the
    // replication engine, and those that arrived, waiting for the tick that
    // fires them. Transport, not world state, so neither reaches the hash.
    std::vector<RemoteMessage> remoteOutbox;
    std::vector<RemoteMessage> remoteInbox;
    // **What a player did past a limit, or what a game found suspicious**
    // (ADR 0241 §3), newest last and bounded: the console and `/metrics`
    // read it. Never acted on by itself.
    struct Suspicion
    {
        core::InstanceId player;
        // `account:` and an account, or `key:` and a key, as a ban names a
        // player, so a line still names them after they left; empty when
        // nobody can be named.
        std::string who;
        std::string kind;
        std::string detail;
        u64 tick = 0;
    };
    // **The headset, as the host read it this frame** (ADR 0245): whether one
    // is drawing, the head and the hands in the play space, and what a script
    // asked of each controller's vibration. Presentation: never hashed, and
    // in the simulation only through what a script does with it.
    struct Headset
    {
        bool enabled = false;
        core::CFrameD head;
        std::array<core::CFrameD, 2> hands{};
        std::array<bool, 2> handsTracked{};
        // **Each hand's 26 joints** (ADR 0245 §4), where it is tracked as a
        // hand -- no controller in it -- in OpenXR's order.
        std::array<std::array<core::CFrameD, 26>, 2> joints{};
        std::array<bool, 2> jointsTracked{};
        std::array<f32, 2> vibration{};
        // `XRService.Space` (`Enum.XRSpace`) and `SessionState`
        // (`Enum.XRSessionState`), and whether the headset is the simulated
        // one, which alone takes `SimulatePose`.
        u8 space = 0;
        u8 sessionState = 0;
        bool simulated = false;
        // **Where `Recenter` put the play space's origin**, in the space the
        // headset gives: every pose a script reads and the picture are
        // measured from it. Asked by a script, made by the host at the next
        // frame from where the head is then.
        core::CFrameD recenter;
        bool recenterAsked = false;
        // The floor's rectangle, width and depth, where the headset knows one.
        std::optional<std::array<f32, 2>> playArea;
        // What `SimulatePose` put -- the head's, each hand's, then each index
        // fingertip's -- for the host to hand the simulated headset at the
        // next frame.
        std::array<std::optional<core::CFrameD>, 5> simulatedPoses{};
        // **Each `ScreenGui` a panel in front of the player** (ADR 0246 §1),
        // while a headset draws: the screen shows none of them then. Where
        // each is, in the play space a script reads and in the world, and its
        // canvas.
        bool panelsShown = false;
        // **The room seen through** (ADR 0247 §1): what `XRService.Passthrough`
        // asks for, and whether the headset can.
        bool passthrough = false;
        bool passthroughSupported = false;
        // Whether the headset's runtime makes frames between (ADR 0252).
        bool spaceWarpSupported = false;
        // **A cut said** (`XRService:MarkCut`): the player's space moved in a
        // way nothing is to be made from, this frame.
        bool cutAsked = false;
        // **What the window shows while the headset draws**
        // (`XRService.SpectatorCamera`, ADR 0250): a camera of the game's, or
        // none for the left eye.
        core::InstanceId spectatorCamera{};
        // **How the eyes are drawn** (ADR 0250): the project's `[xr]` values,
        // put here by the host before the first script runs, and what a
        // script sets over them, read each frame the eyes are drawn.
        // `XRService`'s quality properties.
        struct Quality
        {
            u32 msaa = 1;
            f32 renderScale = 1.0f;
            u32 shadowResolution = 0;
            f32 shadowDistance = 0.0f;
            u32 shadowCascades = 0;
            f32 shadowFirstSplit = 2.0f;
            bool screenEffects = false;
            // Frames made between the drawn ones (`XRService.SpaceWarp`, ADR 0252).
            bool spaceWarp = false;
            u8 foveation = 0;
            f32 refreshRate = 0.0f;
        };
        Quality quality;
        // **How the headset's frames go**, a second at a time: the time
        // between them, the runtime's GPU time of them (nought where it does
        // not measure), and the frames not shown on time since the session
        // began. `XRService.FrameTime`, `GpuTime`, `DroppedFrames`.
        f32 frameTimeMs = 0.0f;
        f32 gpuTimeMs = 0.0f;
        i64 droppedFrames = 0;
        // **The room** (ADR 0247 §2): whether a script waits for it, and the
        // headset's last answer -- whether it knows one, and each plane in the
        // play space a script reads, its width and height as x and y.
        struct RoomPlane
        {
            u8 kind = 7;
            core::CFrameD cframe;
            core::Vec3 size{};
        };
        bool roomAsked = false;
        bool roomKnown = false;
        std::vector<RoomPlane> room;
        // **Anchors** (ADR 0247 §3): each `XRAnchor` with the headset's handle
        // for it, its id, and where it is now in the play space a script
        // reads -- one whose instance is gone is let go of by the host; what
        // scripts asked of the headset (to make one, 0, at `cframe`; to save
        // one, 1, by its handle; to load them, 2), for the host to hand over;
        // and its answers, for the scripts: whether it did, and each anchor
        // made or found, by handle and id.
        struct Anchor
        {
            core::InstanceId instance;
            u64 handle = 0;
            std::string id;
            core::CFrameD cframe;
            bool tracked = false;
        };
        std::vector<Anchor> anchors;
        struct AnchorAsk
        {
            u32 waiter = 0;
            u8 what = 0;
            core::CFrameD cframe;
            u64 handle = 0;
        };
        std::vector<AnchorAsk> anchorAsks;
        struct AnchorAnswer
        {
            u32 waiter = 0;
            u8 what = 0;
            bool ok = false;
            core::CFrameD cframe;
            std::vector<std::pair<u64, std::string>> found;
        };
        std::vector<AnchorAnswer> anchorAnswers;
        u32 nextAnchorWaiter = 1;
        // **The pointer** (ADR 0246 §2): where each hand points, in the play
        // space a script reads; where the play space is in the world this
        // frame (the camera's `CFrame`); `XRService.PointerHand`,
        // `PointerVisible` and `PointerLength`; how far along its ray the
        // pointer met an interface this frame, if it met one; and whether its
        // trigger or pinch went down or up this frame.
        std::array<core::CFrameD, 2> aims{};
        std::array<bool, 2> aimsTracked{};
        core::CFrameD playSpaceWorld;
        u8 pointerHand = 1;
        bool pointerVisible = true;
        f32 pointerLength = 5.0f;
        std::optional<f32> pointerHit;
        bool pointerDown = false;
        bool pointerPressed = false;
        bool pointerReleased = false;
        // **The poke**: what a fingertip is pressing, if one is.
        core::InstanceId poked;
        // What `SimulateInput` holds the simulated headset's controls at, by
        // their place among the `XR` key codes.
        std::array<std::optional<f32>, 19> simulatedKeys{};
        struct Panel
        {
            core::InstanceId gui;
            core::CFrameD space;
            core::CFrameD world;
            core::Vec2 canvas{};
            f32 pixelsPerMetre = 1000.0f;
        };
        std::vector<Panel> panels;
    };
    Headset headset;
    std::vector<Suspicion> suspicions;
    u64 suspicionsTotal = 0;
    // Every suspicion ever, by kind, for `/metrics`.
    std::map<std::string, u64, std::less<>> suspicionsByKind;
    // `[network] kick_over_limit` (ADR 0241 §1): a player whose calls go past
    // the remotes' limits this many times in a second is removed; nought for
    // never.
    u32 kickOverLimit = 0;
    // **Who the console has muted** (ADR 0241 §4), as a ban names them --
    // `key:` and a key, or `account:` and an account -- written by the
    // console each frame, read by `NetworkService:IsMuted`.
    std::vector<std::string> muted;

    // **`SceneService`** (ADR 0106). The scene the world came from,
    // content-relative, and what the last `LoadScene` handed the new one,
    // encoded as a `RemoteEvent` argument is. Which file a world came from is
    // not what the world is, so none of it reaches the hash.
    std::string currentScene;
    std::vector<u8> sceneLoadData;
    // **How many scenes this world has loaded**: raised by every load,
    // the same scene again included, so a replica can tell the next match in
    // the same scene from the scene it joined.
    u32 sceneLoads = 0;
    // **How many holds the scripts have on the loading curtain** (ADR 0159):
    // `SceneService:HoldLoading` raises it, `ReleaseLoading` lowers it, and
    // the host lifts the curtain over a new scene only at none -- or when it
    // has waited long enough, and then sets it back to none and says so.
    u32 loadingHolds = 0;

    // **Stamp copies whose `Construct` is to run** (ADR 0155 §8): queued as a
    // copy is built, and run by the script runtime before any script sees it
    // -- after a scene is read, at the start of a tick, and in the editor's
    // frame. `Instance.stamp` runs its copy's at once and takes it off.
    std::vector<core::InstanceId> pendingConstructs;
    // Copies with a `Construct` whose parameters were written: built again in
    // the editor, and NOT in play, where a construction script runs at
    // construction only.
    std::vector<core::InstanceId> changedParameters;

    // **`SaveService.Version`** (ADR 0111): the game's save layout, set by a
    // script before its first slot. A fact about the game's files, not about
    // the world, so it stays out of the hash.
    f64 saveVersion = 1.0;
    // A `LoadScene` waiting for the safe point between ticks. The host takes
    // it; nothing else acts on it.
    struct PendingSceneLoad
    {
        std::string path;
        std::vector<u8> data;
    };
    std::optional<PendingSceneLoad> pendingSceneLoad;

    // **Sub-worlds** (ADR 0107 §3). What this world's scripts asked of the
    // `SubWorld`s in it, carried out by the host at the end of the tick, and
    // what crossed back. The worlds themselves are the host's.
    //
    // Whether this world runs inside another's `SubWorld` -- which may not
    // hold one of its own -- and how many it may run.
    bool subWorld = false;
    u32 maxSubWorlds = 2;

    // The clicks crossing to the authority (ADR 0126).
    std::vector<DetectorMessage> detectorOutbox;
    std::vector<DetectorMessage> detectorInbox;
    // `Load()`ed and not `Unload()`ed, in the order they were asked for; and
    // those running now.
    std::vector<core::InstanceId> subWorldsWanted;
    std::vector<core::InstanceId> subWorldsLoaded;
    struct SubWorldInput
    {
        core::InstanceId subWorld{};
        std::string action{};
        core::Vec3 value{};
        bool pressed = false;
    };
    std::vector<SubWorldInput> subWorldInputs;
    // Plain values in the `RemoteEvent` encoding: `Send()` on the way in, and
    // `SendToHost()` arrived, for `Received`.
    struct SubWorldMessage
    {
        core::InstanceId subWorld{};
        std::vector<u8> payload{};
    };
    std::vector<SubWorldMessage> subWorldOutbox;
    std::vector<SubWorldMessage> subWorldInbox;
    // Inside a sub-world: what the host sent, for `SceneService.HostMessageReceived`,
    // and what `SendToHost` is sending out.
    std::vector<std::vector<u8>> hostInbox;
    std::vector<std::vector<u8>> hostOutbox;

    // **`NetworkService` at run time** (ADR 0106): `State`, as
    // `Enum.NetworkState`'s value; `[network] server`, where `Join` with no
    // address goes; and a `Join`, `Host` or `Disconnect` a script asked for,
    // waiting for the safe point. Facts about the process, not the world.
    i32 networkState = 0;
    // **The scene's client code is held**: a join is under way, and the
    // world a `ClientScriptService` script would run in is about to be
    // replaced by the server's. Such a script is not live while this is set
    // (`script::scriptLive`), so it starts once -- in the world it will play
    // in, joined or solo -- where it started in one and woke up in another
    // holding references to the first. `GlobalScriptService` is not held: a
    // menu and its music live across every world.
    bool sceneClientHeld = false;
    std::string defaultServer;
    // **Reaching a host behind a NAT** (ADR 0178): `[network] relay`, where a
    // `Host` registers and a `Join` by code asks when the call names none;
    // and, while hosting, where this machine stands with its relay
    // (`Enum.RelayState`'s value) and the code its joiners type.
    std::string defaultRelay;
    i32 networkRelayState = 0;
    std::string networkJoinCode;
    struct NetworkRequest
    {
        enum class Kind : u8
        {
            Join,
            Host,
            Disconnect,
        };
        Kind kind = Kind::Join;
        // A join: `host`, `host:port`, or -- with `relay` set -- a host's code.
        std::string address;
        u16 port = 0;
        // The relay to register with (`Host`) or to ask (`Join` by code);
        // empty for neither.
        std::string relay;
        // A join by code: whether a path each to the other is tried before
        // the relay carries the match.
        bool relayDirect = true;
        // A join: the ticket its game presents (ADR 0226), empty for none.
        std::string ticket;
    };
    std::optional<NetworkRequest> pendingNetwork;
    // **Hosts whose pinned key a script forgot** (`ForgetServerKey`, ADR
    // 0224), as it named them: done by the session at its safe point.
    std::vector<std::string> forgottenServerKeys;

    // --- A server's console (ADR 0218) ------------------------------------
    // **A command the game registered** (`ConsoleService:Register`): what the
    // console parses a line by and lists in `help`. The function is the
    // script runtime's; this is what the engine reads of it.
    struct ConsoleParameter
    {
        enum class Type : u8
        {
            String,
            Number,
            Boolean,
            Player,
        };
        std::string name;
        Type type = Type::String;
        bool optional = false;
    };
    struct ConsoleCommand
    {
        std::string name;
        // A key of the game's catalog, or text.
        std::string description;
        std::vector<ConsoleParameter> parameters;
        // 0 to 3: who may run it. The local console is 3.
        u8 permission = 2;
    };
    std::vector<ConsoleCommand> consoleCommands;
    // Who ran a command: the local console, or an administrator (ADR 0219).
    struct ConsoleCaller
    {
        bool local = true;
        std::string name;
        std::string key;
        u8 permission = 3;
    };
    // **One parsed call of a game's command**, for the scripts to run between
    // ticks. An argument left out is `present = false`; a player is their
    // `UserId`.
    struct ConsoleArgument
    {
        bool present = false;
        std::string text;
        f64 number = 0.0;
        bool flag = false;
        u32 userId = 0;
    };
    struct ConsoleCall
    {
        u32 id = 0;
        std::string name;
        std::vector<ConsoleArgument> arguments;
        ConsoleCaller caller;
    };
    std::vector<ConsoleCall> consoleCalls;
    // And what each said back, by its `id`: its return value as text, or the
    // error it raised.
    struct ConsoleAnswer
    {
        u32 id = 0;
        std::string text;
        bool failed = false;
    };
    std::vector<ConsoleAnswer> consoleAnswers;
    // **A line a script asked the console to run** (`ConsoleService:
    // ExecuteAsync`), as the local console with the script as caller; its
    // answer comes back in `consoleAnswers` under the same `id`, which the
    // script runtime numbers from the top of the range.
    struct ConsoleLine
    {
        u32 id = 0;
        std::string line;
    };
    std::vector<ConsoleLine> consoleLines;
    // What the console answered those lines, by their `id`.
    std::vector<ConsoleAnswer> consoleLineAnswers;
    // --- Who may join (ADR 0226) ---------------------------------------
    // **A joiner the game's `OnJoinRequest` is asked about**, as the session
    // knows them before they are a player; and its answer, by `id`.
    struct JoinValidation
    {
        u32 id = 0;
        std::string publicKey;
        std::string address;
        std::string ticket;
        // What a manager's pass named: `joiner.AccountId` before the game's
        // function sets it.
        std::string accountId;
    };
    std::vector<JoinValidation> joinValidations;
    struct JoinVerdict
    {
        u32 id = 0;
        bool admit = false;
        std::string reason;
        std::string accountId;
    };
    std::vector<JoinVerdict> joinVerdicts;
    // Whether a script set `NetworkService.OnJoinRequest`: none admits everybody.
    bool validateJoinSet = false;
    // `[network] ticket_public_key` and `[project] id`: what `VerifyTicket`
    // checks a ticket against.
    std::optional<std::array<u8, 32>> ticketPublicKey;
    std::string projectId;

    // --- Worlds under a manager (ADRs 0220 to 0223) ------------------------
    // This world's name, as `servers.toml` gives it, and every world the
    // manager has; empty where no manager started this one.
    std::string managedWorld;
    std::vector<std::string> managerWorlds;
    // **Players a script sent elsewhere** (`Travel`, `TravelGroup`), for the
    // manager to give passes for: who, where, and what they carry, as a save
    // writes it.
    struct TravelRequest
    {
        u32 id = 0;
        std::vector<u32> userIds;
        std::string target;
        std::string data;
        // ADR 0230: a channel of `target` asked for by its number, or the
        // player -- by key -- whose world and channel to go to.
        u32 channel = 0;
        std::string toPlayer;
    };
    std::vector<TravelRequest> travelRequests;
    u32 nextTravel = 1;
    // **Channels** (ADR 0230): which this world is, 1 for a world without;
    // what a script asked the manager of them, and its answers; and each
    // warning that this channel is to be merged, in seconds.
    u32 networkChannel = 1;
    std::vector<u32> channelRequests;
    // **The data service** (ADR 0238): whether this world has a database to
    // ask -- the manager's, or a server's own; the records scripts asked for;
    // the writes of records it holds, each letting it go or not; and the
    // answers. The host carries them each frame.
    bool dataAvailable = false;
    // `DataService.Version` (ADR 0238 §2): the records' format, as the
    // game's code knows it.
    f64 dataVersion = 1.0;
    struct DataLoad
    {
        u32 id = 0;
        std::string account;
    };
    struct DataWrite
    {
        std::string account;
        std::string body;
        f64 version = 1.0;
        bool release = false;
    };
    struct DataAnswer
    {
        u32 id = 0;
        bool ok = false;
        bool busy = false;
        bool found = false;
        std::string body;
        f64 version = 1.0;
    };
    std::vector<DataLoad> dataLoads;
    std::vector<DataWrite> dataWrites;
    std::vector<DataAnswer> dataAnswers;
    // **Items, money and transactions** (ADR 0238 §3): each a request and its
    // answer as JSON, by the number the script waits on.
    struct DataOp
    {
        u32 id = 0;
        std::string request;
    };
    struct DataResult
    {
        u32 id = 0;
        std::string answer;
    };
    std::vector<DataOp> dataOps;
    std::vector<DataResult> dataResults;
    u32 nextDataLoad = 1;
    struct ChannelInfo
    {
        u32 channel = 0;
        u32 players = 0;
        u32 maxPlayers = 0;
    };
    struct ChannelAnswer
    {
        u32 id = 0;
        std::vector<ChannelInfo> channels;
    };
    std::vector<ChannelAnswer> channelAnswers;
    u32 nextChannelRequest = 1;
    // **Signing in at a front door** (ADR 0237): what a script asked, which
    // the network session carries to the door and answers. `failure` is 0
    // signed in, else an `Enum.SignInFailure`'s value.
    struct SignInAsk
    {
        u32 id = 0;
        std::string address;
        std::string name;
        std::string password;
        bool registering = false;
        // A session of the game's Nakama in `password` (ADR 0242); and with
        // it a match of Nakama's matchmaker, its id in `name`.
        bool nakama = false;
        bool nakamaMatch = false;
    };
    std::vector<SignInAsk> signInAsks;
    struct SignInAnswer
    {
        u32 id = 0;
        u8 failure = 0;
        std::string ticket;
        u32 retryAfter = 0;
    };
    std::vector<SignInAnswer> signInAnswers;
    u32 nextSignIn = 1;
    // **Parties** (ADR 0240 §1): what a script asked, as the text the party
    // book reads, and its answers; what the book said of a party since, for
    // `Changed`, and of an invitation, for `Invited`; and the parties this
    // world was told of, by id, with their members -- what `TravelGroup` and
    // `CreateInstanceAsync` read a party's id by.
    struct PartyOp
    {
        u32 id = 0;
        std::string request;
    };
    std::vector<PartyOp> partyOps;
    struct PartyResult
    {
        u32 id = 0;
        std::string answer;
    };
    std::vector<PartyResult> partyResults;
    u32 nextPartyOp = 1;
    std::vector<std::string> partyChanged;
    struct PartyInvite
    {
        std::string member;
        std::string party;
        std::string by;
    };
    std::vector<PartyInvite> partyInvites;
    std::map<std::string, std::vector<std::string>, std::less<>> partyMembers;
    // `TravelGroup` given a party's id: its members, wherever they are.
    struct PartyTravel
    {
        std::string party;
        std::string target;
        std::string data;
    };
    std::vector<PartyTravel> partyTravels;
    // **Matchmaking and lobbies** (ADR 0243): what a script asked of the
    // manager's queues, as text, and the answers; and what the manager said
    // since -- a match its players here are in, a lobby that changed or is
    // gone -- as the JSON it sent, for `Matched` and `LobbyChanged`.
    struct MatchOp
    {
        u32 id = 0;
        std::string request;
    };
    std::vector<MatchOp> matchOps;
    struct MatchResult
    {
        u32 id = 0;
        std::string answer;
    };
    std::vector<MatchResult> matchResults;
    u32 nextMatchOp = 1;
    std::vector<std::string> matchesMade;
    std::vector<std::string> lobbiesChanged;
    // **An instance given to a match**: its scene, and the match as the
    // manager sent it, made into the scene's load data -- `{ Match = info }`
    // as a `RemoteEvent` carries it -- where there is Luau to make it with.
    struct MatchLoad
    {
        std::string scene;
        std::string match;
    };
    std::optional<MatchLoad> pendingMatchLoad;
    std::vector<f64> channelMerging;
    // **In line at the front door** (ADR 0232): each place this machine was
    // told, with the seconds until its turn, below 0 for none.
    struct QueueUpdate
    {
        u32 position = 0;
        i64 estimate = -1;
    };
    std::vector<QueueUpdate> queueUpdates;
    // **Who arrived with a pass**, for `TravelArrived`: by key, with what
    // they carry and the world they left.
    struct TravelArrival
    {
        std::string publicKey;
        std::string origin;
        std::string data;
        u32 waited = 0;
        // Why they came back, when not of their own accord (`InstanceFailed`).
        std::string reason;
    };
    std::vector<TravelArrival> travelArrivals;
    // A client sent elsewhere: where, for `TravelStarted`.
    std::vector<std::string> travelStarted;
    // **Instances** (ADR 0221): what this world's scripts asked of the
    // manager -- a copy of a template for a group, in its `Data` as a remote
    // event's arguments written -- and its answers, by request.
    struct InstanceRequest
    {
        u32 id = 0;
        std::string templateName;
        std::vector<std::string> keys;
        std::vector<u8> data;
        // A party's id (ADR 0240 §1): its members, wherever they are, too.
        std::string party;
    };
    std::vector<InstanceRequest> instanceRequests;
    u32 nextInstanceRequest = 1;
    struct InstanceAnswer
    {
        u32 id = 0;
        std::string instanceId;
        std::string error;
    };
    std::vector<InstanceAnswer> instanceAnswers;
    // And what an instance's own scripts say of it.
    struct InstanceControl
    {
        enum class Kind : u8
        {
            Allow,
            Disallow,
            Close,
            KeepOpen,
        };
        Kind kind = Kind::Close;
        std::string key;
        f64 seconds = 0.0;
    };
    std::vector<InstanceControl> instanceControls;
    // **Events between worlds** (`CrossWorldService`, ADR 0223): what this
    // world published for the manager, what arrived for its callbacks -- as a
    // save writes a value -- the topics it began or stopped hearing, and how
    // many arrived too late or too many and were dropped.
    struct CrossWorldMessage
    {
        std::string topic;
        std::string message;
        std::string origin;
    };
    std::vector<CrossWorldMessage> crossWorldOut;
    std::deque<CrossWorldMessage> crossWorldIn;
    std::vector<std::pair<std::string, bool>> crossWorldTopics;
    u64 crossWorldDropped = 0;
    // **Instances this world made that are gone**, and why (`InstanceClosed`).
    struct InstanceClosed
    {
        std::string id;
        std::string reason;
    };
    std::vector<InstanceClosed> instancesClosed;
    // The manager said this instance closes: `Closing` fires once.
    bool instanceClosing = false;
    bool instanceClosingFired = false;

    // `[permissions] websocket_client` (ADR 0223 §2): a client's game may open
    // `net.websocket` too.
    bool websocketClientAllowed = false;

    // `say` (ADR 0218): lines a server's operator sent every player. On a
    // client, what arrived; the scripts fire `Announced` for each.
    std::vector<std::string> serverMessages;
    // **How many play, the machine's own player among them**
    // (`NetworkService.MaxPlayers`, ADR 0167): what a script wrote, or zero
    // for the session's own limit; and what the session takes now, which is
    // what the property reads on an authority.
    u32 networkMaxPlayersWanted = 0;
    u32 networkMaxPlayers = 0;
    // Players a script removed (`Player:Kick`), each by its user id with the
    // words for why, until the session lets them go at the frame's safe point.
    std::vector<std::pair<u32, std::string>> pendingRemovals;
};

// Per-parent name index. Held in its own pool rather than inline in the record
// so that a leaf instance -- most of them -- pays nothing for it.
struct NameIndex
{
    std::unordered_map<u32, core::InstanceId> firstByName;
};

// Every component pool a `World` owns, named ONCE.
//
// The world's members, a snapshot's storage and the copy between them are all
// generated from this list, so a pool that is not here does not exist and a
// pool that is here but nowhere else fails to compile. The alternative is three
// hand-kept lists that agree until the day somebody adds a component and
// updates two of them -- and the symptom of that is a component that quietly
// stops surviving a restore, which no test asks about unless somebody thought
// to write one.
// clang-format off: the list is one pool a line, which clang-format 18 does not
// keep once it grows past a length -- it re-flows it differently on every pass.
#define ENG_SCENE_POOL_LIST(X)                                                                                         \
    X(PVComponent, pvInstances)                                                                                        \
    X(PartComponent, parts)                                                                                            \
    X(RigidBodyComponent, rigidBodies)                                                                                 \
    X(CharacterBodyComponent, characterBodies)                                                                         \
    X(WeldComponent, welds)                                                                                            \
    X(AttachmentComponent, attachments)                                                                                \
    X(ConstraintComponent, constraints)                                                                                \
    X(MoverComponent, movers)                                                                                          \
    X(NoCollisionComponent, noCollisions)                                                                              \
    X(VehicleControllerComponent, vehicleControllers)                                                                  \
    X(VehicleWheelComponent, vehicleWheels)                                                                            \
    X(RagdollComponent, ragdolls)                                                                                      \
    X(SpringBoneComponent, springBones)                                                                                \
    X(ClothComponent, cloths)                                                                                          \
    X(SpringColliderComponent, springColliders)                                                                        \
    X(IKControlComponent, ikControls)                                                                                  \
    X(FootPlacementComponent, footPlacements)                                                                          \
    X(LipSyncComponent, lipSyncs)                                                                                      \
    X(ReflectionProbeComponent, reflectionProbes)                                                                      \
    X(IrradianceVolumeComponent, irradianceVolumes)                                                                    \
    X(SequencePlayerComponent, sequencePlayers)                                                                        \
    X(SequenceBindingComponent, sequenceBindings)                                                                      \
    X(WorkspaceComponent, workspaces)                                                                                  \
    X(TerrainComponent, terrains)                                                                                      \
    X(VoxelComponent, voxels)                                                                                          \
    X(PlayerComponent, players)                                                                                        \
    X(ModelComponent, models)                                                                                          \
    X(ScriptComponent, scripts)                                                                                        \
    X(SoundComponent, sounds)                                                                                          \
    X(AudioGroupComponent, audioGroups)                                                                                \
    X(SoundEffectComponent, soundEffects)                                                                              \
    X(CaptionComponent, captions)                                                                                      \
    X(ScreenGuiComponent, screenGuis)                                                                                  \
    X(BillboardGuiComponent, billboardGuis)                                                                            \
    X(SurfaceGuiComponent, surfaceGuis)                                                                                \
    X(UIObjectComponent, uiObjects)                                                                                    \
    X(TextLabelComponent, textLabels)                                                                                  \
    X(TextInputComponent, textInputs)                                                                                  \
    X(ImageLabelComponent, imageLabels)                                                                                \
    X(ScrollFrameComponent, scrollFrames)                                                                              \
    X(UIListLayoutComponent, listLayouts)                                                                              \
    X(UIPaddingComponent, uiPaddings)                                                                                  \
    X(UICornerComponent, uiCorners)                                                                                    \
    X(UIGradientComponent, uiGradients)                                                                                \
    X(UIStrokeComponent, uiStrokes)                                                                                    \
    X(UIGridLayoutComponent, uiGridLayouts)                                                                            \
    X(UIPageLayoutComponent, uiPageLayouts)                                                                            \
    X(UIFlexItemComponent, uiFlexItems)                                                                                \
    X(UIScaleComponent, uiScales)                                                                                      \
    X(UIAspectRatioConstraintComponent, uiAspectRatioConstraints)                                                      \
    X(UISizeConstraintComponent, uiSizeConstraints)                                                                    \
    X(UITextSizeConstraintComponent, uiTextSizeConstraints)                                                            \
    X(UIDragDetectorComponent, uiDragDetectors)                                                                        \
    X(CanvasGroupComponent, canvasGroups)                                                                              \
    X(InputContextComponent, inputContexts)                                                                            \
    X(InputActionComponent, inputActions)                                                                              \
    X(InputBindingComponent, inputBindings)                                                                            \
    X(MeshPartComponent, meshParts)                                                                                    \
    X(CameraComponent, cameras)                                                                                        \
    X(PointLightComponent, pointLights)                                                                                \
    X(ParticleEmitterComponent, particleEmitters)                                                                      \
    X(DecalComponent, decals)                                                                                          \
    X(HighlightComponent, highlights)                                                                                  \
    X(AdornmentComponent, adornments)                                                                                  \
    X(SwarmComponent, swarms)                                                                                          \
    X(AnimationPlayerComponent, animationPlayers)                                                                      \
    X(BeamComponent, beams)                                                                                            \
    X(TrailComponent, trails)                                                                                          \
    X(CameraTextureComponent, cameraTextures)                                                                          \
    X(SubWorldComponent, subWorlds)                                                                                    \
    X(ViewportFrameComponent, viewportFrames)                                                                          \
    X(Part2DComponent, parts2d)                                                                                        \
    X(Tilemap2DComponent, tilemaps2d)                                                                                  \
    X(Constraint2DComponent, constraints2d)                                                                            \
    X(SpriteAnimatorComponent, spriteAnimators)                                                                        \
    X(NavigationComponent, navigation)                                                                                 \
    X(NavigationAreaComponent, navigationAreas)                                                                        \
    X(NavigationLinkComponent, navigationLinks)                                                                        \
    X(NavigationAgentComponent, navigationAgents)                                                                      \
    X(SpotLightComponent, spotLights)                                                                                  \
    X(LightingComponent, lighting)                                                                                     \
    X(PostEffectComponent, postEffects)                                                                                \
    X(BloomEffectComponent, bloomEffects)                                                                              \
    X(ColorCorrectionEffectComponent, colorCorrectionEffects)                                                          \
    X(BlurEffectComponent, blurEffects)                                                                                \
    X(DepthOfFieldEffectComponent, depthOfFieldEffects)                                                                \
    X(SunRaysEffectComponent, sunRaysEffects)                                                                          \
    X(AtmosphereComponent, atmospheres)                                                                                \
    X(FoliageLayerComponent, foliageLayers)                                                                            \
    X(FoliageMeshComponent, foliageMeshes)                                                                             \
    X(ClickDetectorComponent, clickDetectors)                                                                          \
    X(WaterComponent, waters)                                                                                          \
    X(WaterWaveComponent, waterWaves)                                                                                  \
    X(WaterPointComponent, waterPoints)                                                                                \
    X(SkyComponent, skies)                                                                                             \
    X(NameIndex, nameIndices)                                                                                          \
    X(AttributeMap, attributes)                                                                                        \
    X(PhaseComponent, phases)                                                                                          \
    X(LagCompensatedComponent, lagCompensated)                                                                         \
    X(TagSet, tags)
// clang-format on

// A runtime copy of a material asset (ADR 0090): made by `material:Clone()`,
// owned by nobody, never saved, and released when nothing points at it.
//
// **What it holds is what the run changed**, over the asset it came from: a
// field whose bit is not in `set` reads through to the asset, so a clone of a
// material whose file is edited follows the edit everywhere the clone did not.
// That is also exactly what the world hash and the wire need to carry -- the
// source is an input, and the changes are state the run produced.
struct MaterialClone
{
    core::NameAtom source;
    asset::MaterialFieldMask set = 0;
    asset::MaterialProperties values;
};

// By id, which is creation order: an ordered map, so a walk over it is in the
// order the run made them (R10).
using MaterialClones = std::map<u32, MaterialClone>;

// **A part's own surface shader parameters** (ADR 0091): the ones its material
// declares in `instanceParameters`, each by name, sorted. A table beside the
// pools rather than a field of `PartComponent`, because a component is copied
// as bytes and a name is not one. Keyed by the instance's index and
// generation, so a walk is in a fixed order (R10).
using PartShaderParameters = std::map<u64, std::vector<asset::ShaderParameter>>;

// A whole world's state, held in memory (ADR 0016's "snapshottable POD ECS
// pools" -- foundations, not rollback).
//
// What it is for is the editor's Play and Stop: the world is copied when Play
// is pressed and put back when Stop is, so a play session leaves nothing
// behind. It is **not** a file format. Nothing here is versioned, nothing is
// byte-stable across builds, and a snapshot means something only to the `World`
// it came from, with the same `ClassRegistry`, `EnumRegistry` and `AtomTable`
// still alive behind it. Serialization is a separate thing with separate
// problems.
//
// Copying rather than journalling is the whole design, and it is why the
// module was shaped the way it was: every component is POD with no invariants
// of its own (components.h), every pool is a dense array whose removals leave
// holes rather than reordering (component_pool.h), the hierarchy is intrusive
// links inside the records, and an enum is `{enumId, value}` rather than a
// pointer (value.h, enum_registry.h). So a snapshot is a per-pool copy and
// never a traversal, and a restore is the same copy the other way.
struct WorldSnapshot
{
#define ENG_SCENE_POOL_SNAPSHOT_MEMBER(Type, Name) ComponentPool<Type> Name;
    ENG_SCENE_POOL_LIST(ENG_SCENE_POOL_SNAPSHOT_MEMBER)
#undef ENG_SCENE_POOL_SNAPSHOT_MEMBER

    // Generations and the free list included, which is what makes an
    // `InstanceId` mean the same thing after a restore as before it.
    core::SlotMap<InstanceRecord> instances;
    std::unordered_map<u32, std::vector<core::InstanceId>> tagged;
    std::vector<core::InstanceId> pendingRetire;
    std::vector<core::InstanceId> streamingFoci;
    // `CollisionGroups` has no default constructor because a world's table
    // always has a `Default` group; a snapshot's is overwritten before anyone
    // reads it, so the atom it is seeded with is never observed.
    CollisionGroups collisionGroups{core::NameAtom{}};
    EngineState engineState;
    // The generator's state rather than the generator, so restoring it is the
    // same `setState` a `Random:Clone` performs (core/random.h).
    u64 rngState = 0;
    u64 rngIncrement = 1;
    MaterialClones materialClones;
    u32 lastMaterialClone = 0;
    PartShaderParameters partShaderParameters;
};

class World
{
public:
    World(ClassRegistry& classes, EnumRegistry& enums, core::AtomTable& atoms, u64 seed);

    // --- Lifetime ------------------------------------------------------------

    // Fails (returns an invalid id) for an unknown, abstract or non-creatable
    // class; the caller raises the key. `Name` starts at the class's
    // `defaultName` and the instance starts unparented.
    [[nodiscard]] core::InstanceId create(ClassId classId);

    // Synchronous: the subtree is out of the tree when this returns. Enqueues
    // `Destroying` for the instance and each descendant, clears their tags, and
    // locks `Parent`. The generation bump -- which is what stops the handle
    // resolving -- happens in `retireDestroyed`, called at the end of the drain.
    bool destroy(core::InstanceId id);

    // Called by the scheduler after a drain completes. Splitting this from
    // `destroy` is what gives `Destroying` handlers a live handle to work with,
    // which is the whole reason the signal exists.
    void retireDestroyed();

    // **The holes destroyed instances left in the component pools, closed**
    // -- a pool's, once they are half of it and at least 64. Live components
    // keep their order, so iteration and the world's hash are what they were;
    // they move, so this runs only where nothing holds a component across it:
    // the start of a tick. Without it every instance a scene ever had kept a
    // slot in each pool it was in, and every walk of a pool walked them.
    void compactPools();

    [[nodiscard]] bool alive(core::InstanceId id) const noexcept;

    // Whether `destroy` has been called and the retirement has not happened
    // yet. `alive` is deliberately true through that window so a `Destroying`
    // handler has a handle to work with, so the two questions are different and
    // a caller that means "is this still part of the world" wants this one.
    //
    // The renderer is the first to need it: a camera destroyed mid-drain is
    // still `alive`, and rendering through it would be drawing a frame from a
    // viewpoint the world has already let go of.
    [[nodiscard]] bool destroyed(core::InstanceId id) const noexcept;
    [[nodiscard]] ClassId classOf(core::InstanceId id) const noexcept;
    [[nodiscard]] bool isA(core::InstanceId id, ClassId base) const noexcept;

    // --- Naming --------------------------------------------------------------

    [[nodiscard]] core::NameAtom name(core::InstanceId id) const noexcept;

    // Marks an instance as made by a system rather than authored, so a scene
    // does not record it. Applies to the whole subtree at write time -- marking
    // a chunk's folder is enough, and marking every part inside it would be the
    // same statement a thousand times.
    void setGenerated(core::InstanceId id, bool generated) noexcept;
    [[nodiscard]] bool generated(core::InstanceId id) const noexcept;

    // Marks an instance, and with `subtree` everything under it now, as this
    // machine's alone (ADR 0186): what `Instance.new` and `Clone` say of what
    // a client-side script makes. The whole of a clone is marked, so a piece
    // of it moved out from under its root is still nobody else's.
    void setLocal(core::InstanceId id, bool subtree = false);
    [[nodiscard]] bool local(core::InstanceId id) const noexcept;

    // **Where streamed content is born**. A partitioned scene marks
    // each instance whose children left for a cell with a number, and reading
    // it records which instance that became -- once, as the scene is read,
    // before any script has run. So a streamed part comes back under exactly
    // the instance it was authored under, however many siblings share its
    // name and whatever a script has added or destroyed since; and one whose
    // parent a script destroyed does not come back at all, because what it
    // was part of is gone (`StreamingGlue::materialize`).
    void setStreamAnchor(core::u32 anchor, core::InstanceId id);
    // The instance, or none when the number names nothing or it was destroyed.
    [[nodiscard]] core::InstanceId streamAnchor(core::u32 anchor) const noexcept;
    void clearStreamAnchors() noexcept { m_streamAnchors.clear(); }

    // Marks an instance as made from a file under `src/` (ADR 0092, 0105).
    // Unlike `generated` it is NOT inherited by a subtree: a file's script may
    // hold instances somebody authored, and those are saved.
    void setMounted(core::InstanceId id, bool mounted) noexcept;

    // **What a scene put here that this world could not build** -- a stamp
    // whose file is gone, a class this build does not have -- kept as the
    // JSON the file held, and written back under the same parent at the next
    // save. Reading a scene is allowed to leave something out; saving it is
    // not allowed to make that permanent. Not the
    // world's state: never hashed, never replicated, never in a snapshot.
    void keepUnread(core::InstanceId parent, std::string json);
    [[nodiscard]] std::vector<std::string_view> unreadUnder(core::InstanceId parent) const;
    [[nodiscard]] bool hasUnread(core::InstanceId parent) const noexcept;
    [[nodiscard]] bool mounted(core::InstanceId id) const noexcept;

    // Marks an instance the engine made to stay where it is (ADR 0105).
    // `setParent` refuses to move it; the script binding refuses its rename
    // and its destroy.
    void setFixed(core::InstanceId id, bool fixed) noexcept;
    [[nodiscard]] bool fixed(core::InstanceId id) const noexcept;

    // The stamp this instance was made from, or an empty atom (ADR 0049).
    //
    // **Only the root of a stamped subtree carries it.** Its children are the
    // stamp's contents, not instances of it, and marking each of them would be
    // the same statement a hundred times -- the economy `generated` already
    // uses for a streamed chunk. `stampRootOf` is how a caller asks "am I
    // inside one", because that is the question the break rule actually needs.
    void setStamp(core::InstanceId id, core::NameAtom stamp) noexcept;
    [[nodiscard]] core::NameAtom stampOf(core::InstanceId id) const noexcept;

    // The node of its stamp an instance is, its stamp's parameters and whether
    // its stamp's `Construct` built it (ADR 0155; see `InstanceRecord`).
    void setStampSid(core::InstanceId id, u32 sid) noexcept;
    [[nodiscard]] u32 stampSid(core::InstanceId id) const noexcept;
    void setStampParameters(core::InstanceId id, core::NameAtom parameters) noexcept;
    [[nodiscard]] core::NameAtom stampParameters(core::InstanceId id) const noexcept;
    void setStampOrphans(core::InstanceId id, core::NameAtom orphans) noexcept;
    [[nodiscard]] core::NameAtom stampOrphans(core::InstanceId id) const noexcept;
    void setConstructed(core::InstanceId id, bool constructed) noexcept;
    [[nodiscard]] bool constructed(core::InstanceId id) const noexcept;

    // Where an instance was authored, and its place there (ADR 0138 §6; see
    // `InstanceRecord::origin`). An empty atom for one made at run time.
    struct Origin
    {
        core::NameAtom asset;
        u32 index = 0;
    };
    void setOrigin(core::InstanceId id, Origin origin) noexcept;
    [[nodiscard]] Origin originOf(core::InstanceId id) const noexcept;
    // Every instance of the subtree at `root` given `asset` and its preorder
    // place, counting on from `next`, which it returns advanced. A scene read
    // calls it per top-level node of a tree, a stamp once for its root.
    u32 numberOrigins(core::InstanceId root, core::NameAtom asset, u32 next);

    // The nearest ancestor-or-self that carries a stamp, or an invalid id.
    //
    // The break rule is "everything inside a stamped subtree except the root's
    // own transform and name", so every caller that asks about an edit has to
    // walk up. Answering it here rather than in the editor keeps one definition
    // of "inside a stamp" for the editor, the serializer and the panel.
    [[nodiscard]] core::InstanceId stampRootOf(core::InstanceId id) const noexcept;

    // Relinks the parent's name chains, so a rename can satisfy a
    // `WaitForChild` that was parked on the new name (api-design.md §2.2).
    void setName(core::InstanceId id, core::NameAtom newName);

    // --- Hierarchy -----------------------------------------------------------

    [[nodiscard]] core::InstanceId parentOf(core::InstanceId id) const noexcept;
    [[nodiscard]] core::InstanceId firstChild(core::InstanceId id) const noexcept;
    [[nodiscard]] core::InstanceId nextSibling(core::InstanceId id) const noexcept;
    [[nodiscard]] u32 childCount(core::InstanceId id) const noexcept;

    // Returns the error key on refusal and leaves the tree untouched:
    // `scene.err.parent_cycle` for a cycle, `scene.err.parent_locked` for a
    // destroyed instance. Re-assigning the current parent is a no-op and does
    // NOT reorder the child (api-design.md §2.2).
    std::optional<core::TextKey> setParent(core::InstanceId id, core::InstanceId newParent);

    // Where `id` sits among its parent's children, counting from zero. Nullopt
    // when it has no parent and when the handle resolves to nothing -- an
    // unparented instance has no siblings, and answering "first" would be
    // indistinguishable from a real position.
    //
    // Beside `moveChild` because a caller that has to decide whether a move
    // would change anything BEFORE it commits to an undo step needs the current
    // position, and an editor that walked the list itself would be a second
    // definition of "position among siblings".
    [[nodiscard]] std::optional<u32> siblingIndex(core::InstanceId id) const noexcept;

    // What `moveChild` did.
    //
    // An enum rather than an error key, for the reason `SetResult` below is
    // one: the two refusals want different messages, and a move that changes
    // nothing is neither a refusal nor a change -- which `std::optional<TextKey>`
    // has no way to say.
    enum class MoveResult : u8
    {
        Moved,
        // Already at that index. Nothing was touched and nothing was enqueued:
        // a step that changes nothing is a step that eats an undo, and
        // recording one clears the redo stack with it.
        Unchanged,
        // `child` is not a child of `parent` -- it belongs to something else,
        // it has no parent at all, or one of the two handles no longer
        // resolves. One answer for three, because each is the same statement
        // about the same list; a caller that needs them apart has `parentOf`.
        //
        // A destroyed instance arrives here rather than at a `parent_locked` of
        // its own: `destroy` unparents the whole subtree before it marks it, so
        // there is no list left to hold a place in.
        NotAChild,
        // `index` is not a place in that child list. Refused rather than
        // clamped: the index is computed from where a person let go of a row,
        // and a clamp would put the instance somewhere else and report success.
        IndexOutOfRange,
    };

    // Moves an existing child to `index` among its parent's children, counting
    // from zero, where `index` is the place it will OCCUPY -- the child comes
    // out of the list and goes back in at that position, which is how Unity's
    // `SetSiblingIndex` and Godot's `move_child` both read. Reading it as
    // "before whatever stands at `index` now" would land a downward drag one
    // place short, every time.
    //
    // The one hierarchy verb that is not a re-parent. `setParent` appends and
    // deliberately does not reorder (api-design.md §2.2), which is why dropping
    // a row BETWEEN two rows in the editor's Explorer had nothing to call. It
    // is engine-side: the script-facing API has no reorder and no signal for
    // one, and a scene file is the only thing that records the result.
    //
    // **It enqueues nothing, and that is a decision rather than an omission.**
    // `ChangeKind` has no reorder, and the six signals api-design.md §2.2 lists
    // contain none this could feed -- so the only entries available would be a
    // `ChildRemoved` and a `ChildAdded` for a child that never left its parent,
    // which is two false statements to any handler that looks at the tree
    // during the drain. The world hash sees the move regardless, because it
    // walks the child list in sibling order; so does the serializer, which
    // makes the same walk. That is what keeps a rearranged scene saving as what
    // is on screen.
    //
    // **Cost is O(the parent's children), and it is the editor that pays it.**
    // The links are per record, so the relink itself is a handful of stores;
    // what is linear is finding the child's own position, finding the instance
    // then standing at `index`, and putting the child back in the right place
    // in its duplicate-name chain. A parent with a thousand children costs
    // three walks of a thousand, once per drop, on no tick path. The
    // alternative is a position stored per record, which every append would
    // then have to maintain -- and the 10k-parts benchmark would pay for that
    // on every parenting, to make a gesture nobody performs in a loop faster.
    MoveResult moveChild(core::InstanceId parent, core::InstanceId child, u32 index);

    // O(1) and first in child order, duplicate names included (ADR 0026).
    [[nodiscard]] core::InstanceId findFirstChild(core::InstanceId parent, core::NameAtom childName) const noexcept;
    // Exact `ClassName` match, so asking for `BasePart` never finds a `Part`.
    [[nodiscard]] core::InstanceId findFirstChildOfClass(core::InstanceId parent, ClassId classId) const noexcept;
    // Matches through the hierarchy, so an abstract base name is accepted.
    [[nodiscard]] core::InstanceId findFirstChildWhichIsA(core::InstanceId parent, ClassId base) const noexcept;
    [[nodiscard]] core::InstanceId findFirstAncestor(core::InstanceId id, core::NameAtom ancestorName) const noexcept;
    [[nodiscard]] core::InstanceId findFirstAncestorOfClass(core::InstanceId id, ClassId classId) const noexcept;

    // Strict: an instance is neither its own ancestor nor its own descendant.
    [[nodiscard]] bool isAncestorOf(core::InstanceId id, core::InstanceId descendant) const noexcept;

    // Appends children in child order. Fresh vectors are the caller's, which is
    // what makes destroying during iteration safe (api-design.md §3.1).
    void collectChildren(core::InstanceId id, std::vector<core::InstanceId>& out) const;
    // Depth-first preorder -- the same document order the Find family
    // tie-breaks on.
    void collectDescendants(core::InstanceId id, std::vector<core::InstanceId>& out) const;

    // Deep copy, unparented. References that point inside the copied subtree
    // are rewired to the copies; references that point outside it are left on
    // the originals (api-design.md §2.6).
    [[nodiscard]] core::InstanceId clone(core::InstanceId id);

    // --- Properties ----------------------------------------------------------

    // Nullopt for a name the class does not have; the caller raises
    // `scene.err.unknown_member`.
    [[nodiscard]] std::optional<Value> getProperty(core::InstanceId id, core::NameAtom property) const;

    // The result distinguishes the three ways a write can fail, because each
    // raises a different key.
    enum class SetResult : u8
    {
        Changed,
        // Written, but equal to what was already there, so nothing was
        // enqueued (api-design.md §3.1).
        Unchanged,
        UnknownProperty,
        ReadOnly,
        InvalidValue,
    };
    SetResult setProperty(core::InstanceId id, core::NameAtom property, const Value& value);

    // `script` maintains this as connections come and go. It is the switch that
    // decides whether a property write is quiet.
    void setPropertySubscribed(core::InstanceId id, core::NameAtom property, bool subscribed);

    // --- Attributes and tags -------------------------------------------------

    [[nodiscard]] Value getAttribute(core::InstanceId id, core::NameAtom attribute) const;
    // A `Nil` value removes the attribute. Rejects a value outside the
    // documented domain, which the caller reports as `scene.err.attribute_type`.
    bool setAttribute(core::InstanceId id, core::NameAtom attribute, const Value& value);
    void collectAttributes(core::InstanceId id, AttributeMap& out) const;
    // Whether the instance has an attribute or a tag at all: what something
    // that reads every instance's, every tick, asks first -- most have none.
    [[nodiscard]] bool carriesAttributesOrTags(core::InstanceId id) const noexcept;

    bool addTag(core::InstanceId id, core::NameAtom tag);
    bool removeTag(core::InstanceId id, core::NameAtom tag);
    [[nodiscard]] bool hasTag(core::InstanceId id, core::NameAtom tag) const noexcept;
    void collectTags(core::InstanceId id, TagSet& out) const;
    // Tagging is independent of the tree, so a nil-parented instance is listed.
    void collectTagged(core::NameAtom tag, std::vector<core::InstanceId>& out) const;
    void collectAllTags(TagSet& out) const;

    // --- Materials (ADR 0090) ------------------------------------------------

    // **What a URN a part wears means**: the host's library, borrowed. `scene`
    // has no filesystem; it holds a URN, and this answers what the URN is. A
    // world with none resolves every material to the engine default, which is
    // what a headless world with no content should draw.
    void setMaterialLibrary(asset::MaterialLibrary* library) noexcept { m_materialLibrary = library; }
    // **The game's catalogs** (ADR 0154), the host's: what `LocalizationService`
    // reads. Null where nobody gave one -- a test -- and then a key is asked
    // of the engine's own text alone.
    void setLocalization(const Localization* localization) noexcept { m_localization = localization; }
    [[nodiscard]] const Localization* localization() const noexcept { return m_localization; }
    // **The game's lines of dialogue** (ADR 0200), the host's: what
    // `DialogueService:Say` looks a line up in. Null where nobody gave one.
    void setDialogue(const DialogueLines* dialogue) noexcept { m_dialogue = dialogue; }
    [[nodiscard]] const DialogueLines* dialogue() const noexcept { return m_dialogue; }
    // **How long a sequence's file is** (ADR 0204), the host's: what
    // `SequencePlayer.Length` answers the moment `Sequence` is set, before
    // any tick has visited the player. Null where nobody gave one, and then
    // `Length` is what the last tick wrote.
    void setSequenceFiles(SequenceFiles* files) noexcept { m_sequenceFiles = files; }
    [[nodiscard]] SequenceFiles* sequenceFiles() const noexcept { return m_sequenceFiles; }
    // **What this machine's mixer can say of a sound** (ADR 0200 section 9),
    // the host's audio system: what `Sound.Loudness` and `GetBands` ask. Null
    // where there is none, and then every answer is silence.
    void setSoundMeter(SoundMeter* meter) noexcept { m_soundMeter = meter; }
    [[nodiscard]] SoundMeter* soundMeter() const noexcept { return m_soundMeter; }
    [[nodiscard]] asset::MaterialLibrary* materialLibrary() const noexcept { return m_materialLibrary; }

    // The asset behind a URN, folded flat; the engine default for an invalid
    // atom, a missing file or no library. A reference into the library's cache,
    // valid until the library forgets something.
    [[nodiscard]] const asset::ResolvedMaterial& resolveMaterialAsset(core::NameAtom urn) const;
    // What a material handle describes: its asset, and a clone's own changes
    // over it.
    [[nodiscard]] asset::ResolvedMaterial resolveMaterial(core::NameAtom urn, u32 clone) const;
    // **What a part draws with**: its material, then the overrides that
    // material declares. `instanceParameters` is what it declares, so a caller
    // can tell a kept-and-ignored override from an applied one.
    [[nodiscard]] asset::ResolvedMaterial surfaceOf(const PartComponent& part) const;

    // A part's own surface shader parameters, or null for one that sets none.
    // Kept whatever its material declares, as a built-in override is, so a
    // part put back into a material that declares them looks as it did.
    [[nodiscard]] const std::vector<asset::ShaderParameter>* partShaderParameters(core::InstanceId id) const noexcept;
    // Sets one by name. Counted as a mutation.
    void setPartShaderParameter(core::InstanceId id, asset::ShaderParameter parameter);
    // Whether there was one to clear.
    bool clearPartShaderParameter(core::InstanceId id, std::string_view name);
    [[nodiscard]] const PartShaderParameters& allPartShaderParameters() const noexcept
    {
        return m_partShaderParameters;
    }
    // A part's parameters applied over what its material says: only the ones
    // `material` declares, as `applyOverrides` does for the built-in fields.
    void applyPartShaderParameters(core::InstanceId id, asset::ResolvedMaterial& material) const;

    // A new runtime clone of `source` (an asset URN) carrying `set` changes.
    // Its id is the next in creation order, which is part of world state.
    [[nodiscard]] u32 cloneMaterial(core::NameAtom source, asset::MaterialFieldMask set,
                                    const asset::MaterialProperties& values);
    [[nodiscard]] const MaterialClone* materialClone(u32 id) const noexcept;
    // **A clone somebody else made** -- a replica's copy of one the authority
    // cloned (ADR 0090). Made under the id given, or found there, and pointed at
    // `source`; the counter `cloneMaterial` numbers from is not touched, so a
    // replica's own scripts cannot collide with a range they never reach.
    MaterialClone& adoptMaterialClone(u32 id, core::NameAtom source);
    // Counted as a mutation: every part wearing the clone changes with it.
    [[nodiscard]] MaterialClone* writeMaterialClone(u32 id) noexcept;
    [[nodiscard]] const MaterialClones& materialClones() const noexcept { return m_materialClones; }
    // Counted on each time a material copy is made, handed out to be written,
    // dropped or put back. Every part wearing one changes with it, and nothing
    // of the part's own says so: what reads parts and keeps what it read asks
    // this too.
    [[nodiscard]] u32 materialClonesRevision() const noexcept { return m_materialClonesRevision; }

    // **A script's handle keeps a clone alive**; a part wearing it does too,
    // and the sweep asks the parts. Holds are not world state -- they are the
    // VM's, and a restore does not touch them.
    void holdMaterialClone(u32 id);
    void releaseMaterialClone(u32 id);
    // Drops every clone nothing holds and no part wears. Cheap when nothing was
    // released; run by `retireDestroyed`, which every drain ends with.
    void sweepMaterialClones();
    // Says a part stopped wearing a clone, so the next sweep asks.
    void requestMaterialSweep() noexcept { m_sweepMaterials = true; }

    // --- Snapshot and restore ------------------------------------------------

    // Everything `worldHash` calls observable -- the instance records, the
    // hierarchy including sibling order, names, attributes, tags and every
    // component pool -- plus the state the hash does not reach: `EngineState`,
    // the collision-group table, the streaming foci and the world's RNG
    // position.
    [[nodiscard]] WorldSnapshot snapshot() const;

    // Puts this world back. Instances created since the snapshot are gone,
    // instances destroyed since it are back with their components, and an
    // `InstanceId` taken BEFORE the snapshot resolves to the same instance
    // afterwards -- which is what lets an editor hold a selection across a Stop.
    // An id handed out DURING the play session resolves to nothing, and is
    // never handed out again to mean something else (`SlotMap::restoreFrom`
    // explains the generation bookkeeping that costs).
    //
    // Call it at a frame boundary with no drain in flight, the same safe point
    // `ComponentPool::compact` asks for. The change queue is CLEARED rather
    // than restored: its entries are facts about a world that no longer exists,
    // and their only consumer is the VM the caller is about to rebuild.
    //
    // **What a restore cannot put back, and what the caller therefore owes.**
    //
    //   * **The Luau VM.** Script variables, connections, coroutines and the
    //     task scheduler's timers are not this module's state and are not in
    //     the snapshot -- ADR 0016 names Luau-state restoration an explicit
    //     non-goal of v1, and `change_queue.h` explains why `scene` holds no
    //     reference into the VM to restore in the first place. The caller
    //     rebuilds the runtime. `InstanceRecord::subscribedProperties` comes
    //     back as it was at snapshot time and the rebuilt VM re-subscribes what
    //     it actually connects; a bit left set for a connection that no longer
    //     exists costs an enqueue nobody consumes, which is why the mask is
    //     restored rather than cleared.
    //   * **The physics mirror.** `PhysicsSync` keeps a body per instance slot
    //     and a character per id, and the backend behind it holds contacts,
    //     velocities and sleep state. The solver CAN be restored now (ADR
    //     0101), but only into the same bodies, and a world snapshot brings
    //     back a different set: the caller destroys the mirror and builds a
    //     new one over the restored tree.
    //   * **Everything else derived and keyed by instance.**
    //     `render::AnimationSystem` (a track per player, a pose per mesh part),
    //     `render::TransformHistory` (last frame's transform, which motion
    //     vectors read), the audio mixer's voices and the UI's layout cache are
    //     all rebuilt from the tree rather than restored. Each already retires
    //     what stops resolving; what a restore adds is instances that REAPPEAR,
    //     which nothing retires. Safe order: restore, then rebuild.
    //   * **The atom table.** Names interned during the play session stay
    //     interned, because the table is shared and append-only. An atom's
    //     number is never observable -- the world hash hashes text for exactly
    //     this reason -- so a table that grew is a few bytes and not a
    //     difference.
    void restore(const WorldSnapshot& snapshot);

    // **How many restores this world has had**, counted up and never restored
    // itself. What a restore puts back includes every revision counter a
    // component carries -- a tilemap's, a terrain's -- so a mirror that
    // remembers "built at revision 6" can meet a DIFFERENT revision 6 after an
    // undo and a fresh edit. A mirror that also remembers this number knows
    // when to stop trusting what it remembers. Not state: not in the snapshot,
    // not hashed, and nothing a script can see.
    [[nodiscard]] core::u64 restores() const noexcept { return m_restores; }

    // --- Ground not loaded yet ------------------------------
    //
    // **Loads the ground over a square now, where a streamer holds it on
    // disk.** An edit into a cell not loaded made a chunk of only the edit,
    // which then shadowed the file's chunk for good -- a cube of hillside gone
    // when the cell came in, and a dig there did nothing at all. Every verb
    // that writes the ground asks for its square first. Set by the host, which
    // owns the streamers; a world nobody gave one has all its ground in memory.
    // Cells are columns, so the square is `low` to `high` in x and z, world
    // space.
    //
    // **All of it or none of it**: true when every cell
    // the square touches is in memory now. A square touching more than
    // `maxCells` cells not loaded yet reads none of them and answers false --
    // an edit then refuses before it touches anything, where a cap that read
    // the first 256 and let the edit go on wrote only the edit over the rest,
    // and a save kept that loss.
    using GroundLoader = std::function<bool(core::DVec3 low, core::DVec3 high, core::u32 maxCells)>;
    void setGroundLoader(GroundLoader loader) { m_groundLoader = std::move(loader); }
    // The most cells one call loads: a square four kilometres a side of 64 m
    // cells.
    static constexpr core::u32 MaxGroundCells = 4096;
    [[nodiscard]] bool loadGround(core::DVec3 low, core::DVec3 high, core::u32 maxCells = MaxGroundCells) const
    {
        return !m_groundLoader || m_groundLoader(low, high, maxCells);
    }

    // **How many writes this world has taken** through its own verbs --
    // `create`, `destroy`, `setParent`, `setProperty` -- counted up and never
    // restored. Not state, on the same terms as `restores`: what it is for is a
    // cache that must not answer from before a script's write in the same tick,
    // and must not rebuild a thousand times in a tick when nothing was written.
    // A quiet write straight into a component (the physics mirror's) does not
    // count; a reader that cares about those says so.
    [[nodiscard]] core::u64 mutations() const noexcept { return m_mutations; }

    // What `mutations` read when a property of this instance was last changed
    // through `setProperty`; 0 for never, and for an instance that is gone.
    [[nodiscard]] core::u64 writtenAt(core::InstanceId id) const noexcept
    {
        const InstanceRecord* record = m_instances.find(id);
        return record != nullptr ? record->written : 0;
    }

    // **Every `Script` moved since the last take** (ADR 0137 §1): each one in a
    // subtree `setParent` moved -- a clone parented, a stamp placed, a model
    // moved into storage or out of the world, a destroy. What the script
    // runtime checks against "live" at its next drain, starting or stopping
    // each. Found in the walk of the subtree `setParent` already makes, so a
    // subtree with no script in it costs a class compare per instance.
    // Kept only while a script runtime drains it:
    // the editor's stages, the preview renderer and the partitioner build
    // scripts into worlds nothing drains, where the queue grew for ever.
    void setTracksScripts(bool tracks) noexcept
    {
        m_tracksScripts = tracks;
        if (!tracks)
            m_movedScripts.clear();
    }
    [[nodiscard]] std::vector<core::InstanceId> takeMovedScripts()
    {
        std::vector<core::InstanceId> taken;
        taken.swap(m_movedScripts);
        return taken;
    }

    // --- Frame plumbing ------------------------------------------------------

    [[nodiscard]] ChangeQueue& changes() noexcept { return m_changes; }

    // **Who listens for the tree's per-member events**. A move
    // enqueued `DescendantAdded`/`DescendantRemoving` for every ancestor times
    // every member moved, and `AncestryChanged` for every member -- and a
    // destroy moves each member of its subtree in turn, so a deep tree was
    // quadratic in entries nobody read. Marked when a script reaches the
    // event (`instance.DescendantAdded`), and never unmarked: a listener that
    // disconnects costs what it cost before. Not world state -- a snapshot
    // does not carry it, so an undo does not forget a listener still there.
    enum class TreeListen : u8
    {
        Descendants = 1,
        Ancestry = 2,
    };
    void listenForTree(core::InstanceId id, TreeListen what);
    [[nodiscard]] bool listensForTree(core::InstanceId id, TreeListen what) const noexcept;

    // xxh3 over a canonical walk of the simulation-relevant state
    // (architecture.md §9). Never touches an atom's numeric value, which
    // depends on intern order; it hashes the text.
    [[nodiscard]] u64 worldHash() const;

    // The world's own deterministic stream. `Random.new()` without a seed does
    // NOT come from here and is not legal in simulation code (api-design.md
    // §2.3).
    [[nodiscard]] core::Pcg32& rng() noexcept { return m_rng; }

    [[nodiscard]] usize instanceCount() const noexcept { return m_instances.size(); }
    [[nodiscard]] EngineState& engineState() noexcept { return m_engineState; }
    [[nodiscard]] const EngineState& engineState() const noexcept { return m_engineState; }

    // The collision-group table (api-design.md §2.1). World state rather than
    // the physics backend's, because a script writes it, reads it back and
    // replays it -- and because `BasePart.CollisionGroup` is validated against
    // it on every write, which a scene-level accessor cannot do if the table
    // lives below the seam.
    [[nodiscard]] CollisionGroups& collisionGroups() noexcept { return m_collisionGroups; }
    [[nodiscard]] const CollisionGroups& collisionGroups() const noexcept { return m_collisionGroups; }

    // `StreamingService`'s focus set. A sorted vector rather than a set:
    // it holds two or three entries, the host walks it every frame, and R10
    // forbids an unordered container's order reaching observable output -- the
    // order foci are scored in decides which chunk wins a tie.
    [[nodiscard]] std::vector<core::InstanceId>& streamingFoci() noexcept { return m_streamingFoci; }
    [[nodiscard]] const std::vector<core::InstanceId>& streamingFoci() const noexcept { return m_streamingFoci; }

    [[nodiscard]] core::AtomTable& atoms() noexcept { return m_atoms; }
    // A property getter takes a `const World&`, and resolving an atom to text
    // is the one thing it routinely needs the table for.
    [[nodiscard]] const core::AtomTable& atoms() const noexcept { return m_atoms; }

private:
    std::vector<core::InstanceId> m_streamingFoci;

public:
    [[nodiscard]] const ClassRegistry& classes() const noexcept { return m_classes; }
    // Non-const, so a caller holding one world can build a SECOND against the
    // same registries -- which the prefab stage does, and which the serializer
    // does to diff a stamped instance against the stamp it came from. A
    // registry is a build-time fact shared by every world in a process; two
    // copies of one would be two answers to "what is a Part".
    [[nodiscard]] ClassRegistry& classes() noexcept { return m_classes; }
    // Held here rather than reached separately because every consumer that has
    // one reflection table wants the other: a property write validates an enum
    // value, and the binding that pushes one back out has to name its item.
    [[nodiscard]] const EnumRegistry& enums() const noexcept { return m_enums; }
    [[nodiscard]] EnumRegistry& enums() noexcept { return m_enums; }

    // Component storage the generated property accessors read and write. Public
    // because those accessors are free functions in generated code rather than
    // members -- the alternative is a friend declaration per class, generated.
    //
    // Named per component rather than a `pool<T>()` template, because every
    // class in the kernel's surface is scene's own. Architecture §2 rule 3 has higher
    // modules register their components into scene, and the type-erased storage
    // that needs is a problem to solve when the first renderer brings the first one, not to
    // guess at now.
    [[nodiscard]] ComponentPool<PartComponent>& parts() noexcept { return m_parts; }
    [[nodiscard]] const ComponentPool<PartComponent>& parts() const noexcept { return m_parts; }
    [[nodiscard]] ComponentPool<RigidBodyComponent>& rigidBodies() noexcept { return m_rigidBodies; }
    [[nodiscard]] const ComponentPool<RigidBodyComponent>& rigidBodies() const noexcept { return m_rigidBodies; }
    [[nodiscard]] ComponentPool<WeldComponent>& welds() noexcept { return m_welds; }
    [[nodiscard]] const ComponentPool<WeldComponent>& welds() const noexcept { return m_welds; }
    // The world's ground (ADR 0067). Hand-written like every other pair: there
    // is no generic `pool<T>()`, and this header records that the generic one
    // was deferred and never built.
    [[nodiscard]] ComponentPool<TerrainComponent>& terrains() noexcept { return m_terrains; }
    [[nodiscard]] const ComponentPool<TerrainComponent>& terrains() const noexcept { return m_terrains; }
    [[nodiscard]] ComponentPool<VoxelComponent>& voxels() noexcept { return m_voxels; }
    [[nodiscard]] const ComponentPool<VoxelComponent>& voxels() const noexcept { return m_voxels; }
    [[nodiscard]] ComponentPool<PlayerComponent>& players() noexcept { return m_players; }
    [[nodiscard]] const ComponentPool<PlayerComponent>& players() const noexcept { return m_players; }
    // `Instance.Phase` (ADR 0231): the few instances that have one.
    [[nodiscard]] ComponentPool<PhaseComponent>& phases() noexcept { return m_phases; }
    [[nodiscard]] const ComponentPool<PhaseComponent>& phases() const noexcept { return m_phases; }
    // `BasePart.LagCompensated` (ADR 0235): the few parts that are.
    [[nodiscard]] ComponentPool<LagCompensatedComponent>& lagCompensated() noexcept { return m_lagCompensated; }
    [[nodiscard]] const ComponentPool<LagCompensatedComponent>& lagCompensated() const noexcept
    {
        return m_lagCompensated;
    }

    [[nodiscard]] ComponentPool<AttachmentComponent>& attachments() noexcept { return m_attachments; }
    [[nodiscard]] const ComponentPool<AttachmentComponent>& attachments() const noexcept { return m_attachments; }
    [[nodiscard]] ComponentPool<ConstraintComponent>& constraints() noexcept { return m_constraints; }
    [[nodiscard]] const ComponentPool<ConstraintComponent>& constraints() const noexcept { return m_constraints; }
    [[nodiscard]] ComponentPool<MoverComponent>& movers() noexcept { return m_movers; }
    [[nodiscard]] const ComponentPool<MoverComponent>& movers() const noexcept { return m_movers; }
    [[nodiscard]] ComponentPool<NoCollisionComponent>& noCollisions() noexcept { return m_noCollisions; }
    [[nodiscard]] const ComponentPool<NoCollisionComponent>& noCollisions() const noexcept { return m_noCollisions; }
    [[nodiscard]] ComponentPool<VehicleControllerComponent>& vehicleControllers() noexcept
    {
        return m_vehicleControllers;
    }
    [[nodiscard]] const ComponentPool<VehicleControllerComponent>& vehicleControllers() const noexcept
    {
        return m_vehicleControllers;
    }
    [[nodiscard]] ComponentPool<VehicleWheelComponent>& vehicleWheels() noexcept { return m_vehicleWheels; }
    [[nodiscard]] const ComponentPool<VehicleWheelComponent>& vehicleWheels() const noexcept { return m_vehicleWheels; }
    [[nodiscard]] ComponentPool<RagdollComponent>& ragdolls() noexcept { return m_ragdolls; }
    [[nodiscard]] const ComponentPool<RagdollComponent>& ragdolls() const noexcept { return m_ragdolls; }
    [[nodiscard]] ComponentPool<SpringBoneComponent>& springBones() noexcept { return m_springBones; }
    [[nodiscard]] const ComponentPool<SpringBoneComponent>& springBones() const noexcept { return m_springBones; }
    [[nodiscard]] ComponentPool<ClothComponent>& cloths() noexcept { return m_cloths; }
    [[nodiscard]] const ComponentPool<ClothComponent>& cloths() const noexcept { return m_cloths; }
    [[nodiscard]] ComponentPool<SpringColliderComponent>& springColliders() noexcept { return m_springColliders; }
    [[nodiscard]] const ComponentPool<SpringColliderComponent>& springColliders() const noexcept
    {
        return m_springColliders;
    }
    [[nodiscard]] ComponentPool<IKControlComponent>& ikControls() noexcept { return m_ikControls; }
    [[nodiscard]] const ComponentPool<IKControlComponent>& ikControls() const noexcept { return m_ikControls; }
    [[nodiscard]] ComponentPool<FootPlacementComponent>& footPlacements() noexcept { return m_footPlacements; }
    [[nodiscard]] const ComponentPool<FootPlacementComponent>& footPlacements() const noexcept
    {
        return m_footPlacements;
    }
    [[nodiscard]] ComponentPool<LipSyncComponent>& lipSyncs() noexcept { return m_lipSyncs; }
    [[nodiscard]] const ComponentPool<LipSyncComponent>& lipSyncs() const noexcept { return m_lipSyncs; }
    [[nodiscard]] ComponentPool<ReflectionProbeComponent>& reflectionProbes() noexcept { return m_reflectionProbes; }
    [[nodiscard]] const ComponentPool<ReflectionProbeComponent>& reflectionProbes() const noexcept
    {
        return m_reflectionProbes;
    }
    [[nodiscard]] ComponentPool<IrradianceVolumeComponent>& irradianceVolumes() noexcept { return m_irradianceVolumes; }
    [[nodiscard]] const ComponentPool<IrradianceVolumeComponent>& irradianceVolumes() const noexcept
    {
        return m_irradianceVolumes;
    }
    [[nodiscard]] ComponentPool<SequencePlayerComponent>& sequencePlayers() noexcept { return m_sequencePlayers; }
    [[nodiscard]] const ComponentPool<SequencePlayerComponent>& sequencePlayers() const noexcept
    {
        return m_sequencePlayers;
    }
    [[nodiscard]] ComponentPool<SequenceBindingComponent>& sequenceBindings() noexcept { return m_sequenceBindings; }
    [[nodiscard]] const ComponentPool<SequenceBindingComponent>& sequenceBindings() const noexcept
    {
        return m_sequenceBindings;
    }
    [[nodiscard]] ComponentPool<CharacterBodyComponent>& characterBodies() noexcept { return m_characterBodies; }
    [[nodiscard]] const ComponentPool<CharacterBodyComponent>& characterBodies() const noexcept
    {
        return m_characterBodies;
    }
    [[nodiscard]] ComponentPool<WorkspaceComponent>& workspaces() noexcept { return m_workspaces; }
    [[nodiscard]] const ComponentPool<WorkspaceComponent>& workspaces() const noexcept { return m_workspaces; }
    [[nodiscard]] ComponentPool<PVComponent>& pvInstances() noexcept { return m_pvInstances; }
    [[nodiscard]] const ComponentPool<PVComponent>& pvInstances() const noexcept { return m_pvInstances; }
    [[nodiscard]] ComponentPool<ModelComponent>& models() noexcept { return m_models; }
    [[nodiscard]] const ComponentPool<ModelComponent>& models() const noexcept { return m_models; }
    [[nodiscard]] ComponentPool<ScriptComponent>& scripts() noexcept { return m_scripts; }
    [[nodiscard]] const ComponentPool<ScriptComponent>& scripts() const noexcept { return m_scripts; }

    // The render module's classes. Their storage is here and their meaning
    // is not: `scene` never includes `render`, and these five pools hold POD it
    // does not interpret. See components.h for why they are not behind a
    // type-erased registry.
    [[nodiscard]] ComponentPool<MeshPartComponent>& meshParts() noexcept { return m_meshParts; }
    [[nodiscard]] const ComponentPool<MeshPartComponent>& meshParts() const noexcept { return m_meshParts; }
    [[nodiscard]] ComponentPool<CameraComponent>& cameras() noexcept { return m_cameras; }
    [[nodiscard]] const ComponentPool<CameraComponent>& cameras() const noexcept { return m_cameras; }
    [[nodiscard]] ComponentPool<DecalComponent>& decals() noexcept { return m_decals; }
    [[nodiscard]] const ComponentPool<DecalComponent>& decals() const noexcept { return m_decals; }
    [[nodiscard]] ComponentPool<ViewportFrameComponent>& viewportFrames() noexcept { return m_viewportFrames; }
    [[nodiscard]] const ComponentPool<ViewportFrameComponent>& viewportFrames() const noexcept
    {
        return m_viewportFrames;
    }
    [[nodiscard]] ComponentPool<CameraTextureComponent>& cameraTextures() noexcept { return m_cameraTextures; }
    [[nodiscard]] const ComponentPool<CameraTextureComponent>& cameraTextures() const noexcept
    {
        return m_cameraTextures;
    }
    [[nodiscard]] ComponentPool<SubWorldComponent>& subWorlds() noexcept { return m_subWorlds; }
    [[nodiscard]] const ComponentPool<SubWorldComponent>& subWorlds() const noexcept { return m_subWorlds; }
    // The 2D layer.
    [[nodiscard]] ComponentPool<Part2DComponent>& parts2d() noexcept { return m_parts2d; }
    [[nodiscard]] const ComponentPool<Part2DComponent>& parts2d() const noexcept { return m_parts2d; }
    [[nodiscard]] ComponentPool<NavigationComponent>& navigation() noexcept { return m_navigation; }
    [[nodiscard]] const ComponentPool<NavigationComponent>& navigation() const noexcept { return m_navigation; }
    [[nodiscard]] ComponentPool<NavigationAreaComponent>& navigationAreas() noexcept { return m_navigationAreas; }
    [[nodiscard]] const ComponentPool<NavigationAreaComponent>& navigationAreas() const noexcept
    {
        return m_navigationAreas;
    }
    [[nodiscard]] ComponentPool<NavigationLinkComponent>& navigationLinks() noexcept { return m_navigationLinks; }
    [[nodiscard]] const ComponentPool<NavigationLinkComponent>& navigationLinks() const noexcept
    {
        return m_navigationLinks;
    }
    [[nodiscard]] ComponentPool<NavigationAgentComponent>& navigationAgents() noexcept { return m_navigationAgents; }
    [[nodiscard]] const ComponentPool<NavigationAgentComponent>& navigationAgents() const noexcept
    {
        return m_navigationAgents;
    }
    [[nodiscard]] ComponentPool<Tilemap2DComponent>& tilemaps2d() noexcept { return m_tilemaps2d; }
    [[nodiscard]] const ComponentPool<Tilemap2DComponent>& tilemaps2d() const noexcept { return m_tilemaps2d; }
    [[nodiscard]] ComponentPool<Constraint2DComponent>& constraints2d() noexcept { return m_constraints2d; }
    [[nodiscard]] const ComponentPool<Constraint2DComponent>& constraints2d() const noexcept { return m_constraints2d; }
    [[nodiscard]] ComponentPool<SpriteAnimatorComponent>& spriteAnimators() noexcept { return m_spriteAnimators; }
    [[nodiscard]] const ComponentPool<SpriteAnimatorComponent>& spriteAnimators() const noexcept
    {
        return m_spriteAnimators;
    }
    [[nodiscard]] ComponentPool<SwarmComponent>& swarms() noexcept { return m_swarms; }
    [[nodiscard]] const ComponentPool<SwarmComponent>& swarms() const noexcept { return m_swarms; }
    [[nodiscard]] ComponentPool<AnimationPlayerComponent>& animationPlayers() noexcept { return m_animationPlayers; }
    [[nodiscard]] const ComponentPool<AnimationPlayerComponent>& animationPlayers() const noexcept
    {
        return m_animationPlayers;
    }
    [[nodiscard]] ComponentPool<HighlightComponent>& highlights() noexcept { return m_highlights; }
    [[nodiscard]] const ComponentPool<HighlightComponent>& highlights() const noexcept { return m_highlights; }
    [[nodiscard]] ComponentPool<AdornmentComponent>& adornments() noexcept { return m_adornments; }
    [[nodiscard]] const ComponentPool<AdornmentComponent>& adornments() const noexcept { return m_adornments; }
    [[nodiscard]] ComponentPool<BeamComponent>& beams() noexcept { return m_beams; }
    [[nodiscard]] const ComponentPool<BeamComponent>& beams() const noexcept { return m_beams; }
    [[nodiscard]] ComponentPool<TrailComponent>& trails() noexcept { return m_trails; }
    [[nodiscard]] const ComponentPool<TrailComponent>& trails() const noexcept { return m_trails; }
    [[nodiscard]] ComponentPool<SoundEffectComponent>& soundEffects() noexcept { return m_soundEffects; }
    [[nodiscard]] const ComponentPool<SoundEffectComponent>& soundEffects() const noexcept { return m_soundEffects; }
    [[nodiscard]] ComponentPool<ParticleEmitterComponent>& particleEmitters() noexcept { return m_particleEmitters; }
    [[nodiscard]] const ComponentPool<ParticleEmitterComponent>& particleEmitters() const noexcept
    {
        return m_particleEmitters;
    }
    [[nodiscard]] ComponentPool<PointLightComponent>& pointLights() noexcept { return m_pointLights; }
    [[nodiscard]] const ComponentPool<PointLightComponent>& pointLights() const noexcept { return m_pointLights; }
    [[nodiscard]] ComponentPool<SpotLightComponent>& spotLights() noexcept { return m_spotLights; }
    [[nodiscard]] const ComponentPool<SpotLightComponent>& spotLights() const noexcept { return m_spotLights; }
    [[nodiscard]] ComponentPool<LightingComponent>& lighting() noexcept { return m_lighting; }
    [[nodiscard]] const ComponentPool<LightingComponent>& lighting() const noexcept { return m_lighting; }
    // The look of a world (ADR 0096): what `render::resolveLook` reads.
    [[nodiscard]] ComponentPool<PostEffectComponent>& postEffects() noexcept { return m_postEffects; }
    [[nodiscard]] const ComponentPool<PostEffectComponent>& postEffects() const noexcept { return m_postEffects; }
    [[nodiscard]] ComponentPool<BloomEffectComponent>& bloomEffects() noexcept { return m_bloomEffects; }
    [[nodiscard]] const ComponentPool<BloomEffectComponent>& bloomEffects() const noexcept { return m_bloomEffects; }
    [[nodiscard]] ComponentPool<ColorCorrectionEffectComponent>& colorCorrectionEffects() noexcept
    {
        return m_colorCorrectionEffects;
    }
    [[nodiscard]] const ComponentPool<ColorCorrectionEffectComponent>& colorCorrectionEffects() const noexcept
    {
        return m_colorCorrectionEffects;
    }
    [[nodiscard]] ComponentPool<BlurEffectComponent>& blurEffects() noexcept { return m_blurEffects; }
    [[nodiscard]] const ComponentPool<BlurEffectComponent>& blurEffects() const noexcept { return m_blurEffects; }
    [[nodiscard]] ComponentPool<DepthOfFieldEffectComponent>& depthOfFieldEffects() noexcept
    {
        return m_depthOfFieldEffects;
    }
    [[nodiscard]] const ComponentPool<DepthOfFieldEffectComponent>& depthOfFieldEffects() const noexcept
    {
        return m_depthOfFieldEffects;
    }
    [[nodiscard]] ComponentPool<SunRaysEffectComponent>& sunRaysEffects() noexcept { return m_sunRaysEffects; }
    [[nodiscard]] const ComponentPool<SunRaysEffectComponent>& sunRaysEffects() const noexcept
    {
        return m_sunRaysEffects;
    }
    [[nodiscard]] ComponentPool<AtmosphereComponent>& atmospheres() noexcept { return m_atmospheres; }
    [[nodiscard]] const ComponentPool<AtmosphereComponent>& atmospheres() const noexcept { return m_atmospheres; }
    [[nodiscard]] ComponentPool<FoliageLayerComponent>& foliageLayers() noexcept { return m_foliageLayers; }
    [[nodiscard]] const ComponentPool<FoliageLayerComponent>& foliageLayers() const noexcept { return m_foliageLayers; }
    [[nodiscard]] ComponentPool<FoliageMeshComponent>& foliageMeshes() noexcept { return m_foliageMeshes; }
    [[nodiscard]] const ComponentPool<FoliageMeshComponent>& foliageMeshes() const noexcept { return m_foliageMeshes; }
    [[nodiscard]] ComponentPool<ClickDetectorComponent>& clickDetectors() noexcept { return m_clickDetectors; }
    [[nodiscard]] const ComponentPool<ClickDetectorComponent>& clickDetectors() const noexcept
    {
        return m_clickDetectors;
    }
    [[nodiscard]] ComponentPool<WaterComponent>& waters() noexcept { return m_waters; }
    [[nodiscard]] const ComponentPool<WaterComponent>& waters() const noexcept { return m_waters; }
    [[nodiscard]] ComponentPool<WaterWaveComponent>& waterWaves() noexcept { return m_waterWaves; }
    [[nodiscard]] const ComponentPool<WaterWaveComponent>& waterWaves() const noexcept { return m_waterWaves; }
    [[nodiscard]] ComponentPool<WaterPointComponent>& waterPoints() noexcept { return m_waterPoints; }
    [[nodiscard]] const ComponentPool<WaterPointComponent>& waterPoints() const noexcept { return m_waterPoints; }
    [[nodiscard]] ComponentPool<SkyComponent>& skies() noexcept { return m_skies; }
    [[nodiscard]] const ComponentPool<SkyComponent>& skies() const noexcept { return m_skies; }

    // The input module's classes. Same arrangement as the render pools
    // above: the storage is here, the meaning is not.
    [[nodiscard]] ComponentPool<InputContextComponent>& inputContexts() noexcept { return m_inputContexts; }
    [[nodiscard]] const ComponentPool<InputContextComponent>& inputContexts() const noexcept { return m_inputContexts; }
    [[nodiscard]] ComponentPool<InputActionComponent>& inputActions() noexcept { return m_inputActions; }
    [[nodiscard]] const ComponentPool<InputActionComponent>& inputActions() const noexcept { return m_inputActions; }
    [[nodiscard]] ComponentPool<InputBindingComponent>& inputBindings() noexcept { return m_inputBindings; }
    [[nodiscard]] const ComponentPool<InputBindingComponent>& inputBindings() const noexcept { return m_inputBindings; }

    // The ui module's classes.
    [[nodiscard]] ComponentPool<ScreenGuiComponent>& screenGuis() noexcept { return m_screenGuis; }
    [[nodiscard]] const ComponentPool<ScreenGuiComponent>& screenGuis() const noexcept { return m_screenGuis; }
    [[nodiscard]] ComponentPool<BillboardGuiComponent>& billboardGuis() noexcept { return m_billboardGuis; }
    [[nodiscard]] const ComponentPool<BillboardGuiComponent>& billboardGuis() const noexcept { return m_billboardGuis; }
    [[nodiscard]] ComponentPool<SurfaceGuiComponent>& surfaceGuis() noexcept { return m_surfaceGuis; }
    [[nodiscard]] const ComponentPool<SurfaceGuiComponent>& surfaceGuis() const noexcept { return m_surfaceGuis; }
    [[nodiscard]] ComponentPool<UIObjectComponent>& uiObjects() noexcept { return m_uiObjects; }
    [[nodiscard]] const ComponentPool<UIObjectComponent>& uiObjects() const noexcept { return m_uiObjects; }
    [[nodiscard]] ComponentPool<TextLabelComponent>& textLabels() noexcept { return m_textLabels; }
    [[nodiscard]] const ComponentPool<TextLabelComponent>& textLabels() const noexcept { return m_textLabels; }
    [[nodiscard]] ComponentPool<TextInputComponent>& textInputs() noexcept { return m_textInputs; }
    [[nodiscard]] const ComponentPool<TextInputComponent>& textInputs() const noexcept { return m_textInputs; }
    [[nodiscard]] ComponentPool<ImageLabelComponent>& imageLabels() noexcept { return m_imageLabels; }
    [[nodiscard]] const ComponentPool<ImageLabelComponent>& imageLabels() const noexcept { return m_imageLabels; }
    [[nodiscard]] ComponentPool<ScrollFrameComponent>& scrollFrames() noexcept { return m_scrollFrames; }
    [[nodiscard]] const ComponentPool<ScrollFrameComponent>& scrollFrames() const noexcept { return m_scrollFrames; }
    [[nodiscard]] ComponentPool<UIListLayoutComponent>& listLayouts() noexcept { return m_listLayouts; }
    [[nodiscard]] const ComponentPool<UIListLayoutComponent>& listLayouts() const noexcept { return m_listLayouts; }
    [[nodiscard]] ComponentPool<UIPaddingComponent>& uiPaddings() noexcept { return m_uiPaddings; }
    [[nodiscard]] const ComponentPool<UIPaddingComponent>& uiPaddings() const noexcept { return m_uiPaddings; }
    [[nodiscard]] ComponentPool<UICornerComponent>& uiCorners() noexcept { return m_uiCorners; }
    [[nodiscard]] const ComponentPool<UICornerComponent>& uiCorners() const noexcept { return m_uiCorners; }
    [[nodiscard]] ComponentPool<UIGradientComponent>& uiGradients() noexcept { return m_uiGradients; }
    [[nodiscard]] const ComponentPool<UIGradientComponent>& uiGradients() const noexcept { return m_uiGradients; }
    [[nodiscard]] ComponentPool<UIStrokeComponent>& uiStrokes() noexcept { return m_uiStrokes; }
    [[nodiscard]] const ComponentPool<UIStrokeComponent>& uiStrokes() const noexcept { return m_uiStrokes; }
    [[nodiscard]] ComponentPool<UIGridLayoutComponent>& uiGridLayouts() noexcept { return m_uiGridLayouts; }
    [[nodiscard]] const ComponentPool<UIGridLayoutComponent>& uiGridLayouts() const noexcept { return m_uiGridLayouts; }
    [[nodiscard]] ComponentPool<UIPageLayoutComponent>& uiPageLayouts() noexcept { return m_uiPageLayouts; }
    [[nodiscard]] const ComponentPool<UIPageLayoutComponent>& uiPageLayouts() const noexcept { return m_uiPageLayouts; }
    [[nodiscard]] ComponentPool<UIFlexItemComponent>& uiFlexItems() noexcept { return m_uiFlexItems; }
    [[nodiscard]] const ComponentPool<UIFlexItemComponent>& uiFlexItems() const noexcept { return m_uiFlexItems; }
    [[nodiscard]] ComponentPool<UIScaleComponent>& uiScales() noexcept { return m_uiScales; }
    [[nodiscard]] const ComponentPool<UIScaleComponent>& uiScales() const noexcept { return m_uiScales; }
    [[nodiscard]] ComponentPool<UIAspectRatioConstraintComponent>& uiAspectRatioConstraints() noexcept
    {
        return m_uiAspectRatioConstraints;
    }
    [[nodiscard]] const ComponentPool<UIAspectRatioConstraintComponent>& uiAspectRatioConstraints() const noexcept
    {
        return m_uiAspectRatioConstraints;
    }
    [[nodiscard]] ComponentPool<UISizeConstraintComponent>& uiSizeConstraints() noexcept { return m_uiSizeConstraints; }
    [[nodiscard]] const ComponentPool<UISizeConstraintComponent>& uiSizeConstraints() const noexcept
    {
        return m_uiSizeConstraints;
    }
    [[nodiscard]] ComponentPool<UITextSizeConstraintComponent>& uiTextSizeConstraints() noexcept
    {
        return m_uiTextSizeConstraints;
    }
    [[nodiscard]] const ComponentPool<UITextSizeConstraintComponent>& uiTextSizeConstraints() const noexcept
    {
        return m_uiTextSizeConstraints;
    }
    [[nodiscard]] ComponentPool<UIDragDetectorComponent>& uiDragDetectors() noexcept { return m_uiDragDetectors; }
    [[nodiscard]] const ComponentPool<UIDragDetectorComponent>& uiDragDetectors() const noexcept
    {
        return m_uiDragDetectors;
    }
    [[nodiscard]] ComponentPool<CanvasGroupComponent>& canvasGroups() noexcept { return m_canvasGroups; }
    [[nodiscard]] const ComponentPool<CanvasGroupComponent>& canvasGroups() const noexcept { return m_canvasGroups; }

    // The audio module's classes.
    [[nodiscard]] ComponentPool<SoundComponent>& sounds() noexcept { return m_sounds; }
    [[nodiscard]] const ComponentPool<SoundComponent>& sounds() const noexcept { return m_sounds; }
    [[nodiscard]] ComponentPool<AudioGroupComponent>& audioGroups() noexcept { return m_audioGroups; }
    [[nodiscard]] const ComponentPool<AudioGroupComponent>& audioGroups() const noexcept { return m_audioGroups; }
    // What a sound says (ADR 0200).
    [[nodiscard]] ComponentPool<CaptionComponent>& captions() noexcept { return m_captions; }
    [[nodiscard]] const ComponentPool<CaptionComponent>& captions() const noexcept { return m_captions; }

private:
    // By anchor number (`setStreamAnchor`). Not in a snapshot: the instances
    // it names keep their ids across one, and a scene read anew fills it anew.
    std::vector<core::InstanceId> m_streamAnchors;

    // The pool walk `snapshot` and `restore` share, as `fn(worldPool,
    // snapshotPool)`. Templated on both sides so one body serves a `const
    // World&` copying out and a `World&` copying back, and each caller's lambda
    // decides the direction.
    template <class WorldRef, class SnapshotRef, class Fn>
    static void eachPool(WorldRef& world, SnapshotRef& snapshot, Fn&& fn)
    {
#define ENG_SCENE_POOL_VISIT(Type, Name) fn(world.m_##Name, snapshot.Name);
        ENG_SCENE_POOL_LIST(ENG_SCENE_POOL_VISIT)
#undef ENG_SCENE_POOL_VISIT
    }

    void linkChild(InstanceRecord& parentRecord, core::InstanceId parentId, core::InstanceId childId);
    // The general form: `beforeId` is the sibling the child is inserted ahead
    // of, and an invalid one means the end. `linkChild` is this with the end,
    // so an append and an insert cannot disagree about `firstChild`,
    // `lastChild` or the count.
    void linkChildBefore(InstanceRecord& parentRecord, core::InstanceId parentId, core::InstanceId childId,
                         core::InstanceId beforeId);
    void unlinkChild(core::InstanceId childId);
    void indexName(core::InstanceId parentId, core::InstanceId childId);
    // The same, for a rename: a renamed child may belong in the middle of a
    // chain, which the append above cannot express.
    void indexNameInChildOrder(core::InstanceId parentId, core::InstanceId childId);
    void unindexName(core::InstanceId parentId, core::InstanceId childId);

    ClassRegistry& m_classes;
    EnumRegistry& m_enums;
    core::AtomTable& m_atoms;
    core::Pcg32 m_rng;
    // Interned once so that `clone` can skip the one property that is structure
    // rather than a value, without a string compare per property per instance.
    // Declared after `m_atoms` because it is initialised from it, and the
    // initialiser list has to run in declaration order (-Wreorder-ctor).
    core::NameAtom m_parentProperty;
    // Declared after `m_atoms` for the same reason: its one group is named
    // "Default" and the atom for it comes from the table.
    CollisionGroups m_collisionGroups;

    core::SlotMap<InstanceRecord> m_instances;

    // Declared from the same list a snapshot's storage is, so the two cannot
    // drift. Declaration order follows the list, which is the order the pools
    // were added in over the kernel through the streaming work.
#define ENG_SCENE_POOL_MEMBER(Type, Name) ComponentPool<Type> m_##Name;
    ENG_SCENE_POOL_LIST(ENG_SCENE_POOL_MEMBER)
#undef ENG_SCENE_POOL_MEMBER

    // Insertion-ordered per tag, so `GetTagged` never leaks a hash order.
    std::unordered_map<u32, std::vector<core::InstanceId>> m_tagged;

    // Destroyed but not yet retired: their handles still resolve until the
    // drain that carries their `Destroying` finishes.
    std::vector<core::InstanceId> m_pendingRetire;

    EngineState m_engineState;
    ChangeQueue m_changes;
    struct TreeListener
    {
        u32 generation = 0;
        u8 bits = 0;
    };
    std::vector<TreeListener> m_treeListeners;
    core::u64 m_restores = 0;
    // Past every ground revision a restore has seen: where the next restore's
    // count on from (`World::restore`). Not part of a snapshot.
    core::u64 m_groundRevisionFloor = 0;
    GroundLoader m_groundLoader;
    core::u64 m_mutations = 0;
    std::vector<core::InstanceId> m_movedScripts;
    bool m_tracksScripts = false;
    // `Script`'s class, once the registry has it: `setParent` asks per move.
    ClassId m_scriptClass = InvalidClass;

    asset::MaterialLibrary* m_materialLibrary = nullptr;
    const Localization* m_localization = nullptr;
    const DialogueLines* m_dialogue = nullptr;
    SequenceFiles* m_sequenceFiles = nullptr;
    SoundMeter* m_soundMeter = nullptr;
    MaterialClones m_materialClones;
    u32 m_lastMaterialClone = 0;
    u32 m_materialClonesRevision = 0;
    PartShaderParameters m_partShaderParameters;
    std::map<u32, u32> m_materialHolds;
    bool m_sweepMaterials = false;

private:
    std::vector<std::pair<core::InstanceId, std::string>> m_unread;
};

// **A property left out of the world hash and the scene file at its default**
// (ADR 0138 §7): `Script.RunContext` at `Shared`. So every world hashed and
// every scene written before the property existed hashes and reads as it did,
// and no determinism trace and no scene moves -- the voxel fluid fields'
// precedent, as a rule both writers share rather than two copies of it.
//
// **Works out the voice language in force again** (ADR 0200) -- from what the
// player chose, the text language and what this machine can play -- and says
// `VoiceLocaleChanged` on `service`, the world's `LocalizationService`, when
// it moved. Called by the two setters and by a host that changed the list.
void refreshVoiceLocale(World& world, core::InstanceId service);

// The same for the properties that arrived after it, each at the value that
// is the picture before it existed: `Lighting.ExposureMin` and `ExposureMax`,
// `ImageLabel.ImageTransparency`. And **`UIObject.Active` until somebody
// writes it**: what it reads is what the object is, which a file must
// not freeze -- a frame saved while clear would stay out of the pointer's way
// after it was given a background.
[[nodiscard]] bool quietAtDefault(const World& world, core::InstanceId id, const PropertyDesc& property,
                                  const Value& value);

// **Says from when, at the moment it is asked** (protocol 47): where a
// `SequencePlayer` is and at which of the authority's ticks, written with the
// ask itself -- `Play`, `Pause`, a write of `TimePosition` or of `Speed`.
//
// The host writes the anchor too, in the tick, when it has done what was
// asked. Written only there, a snapshot taken between the ask and that tick
// said "playing" with the anchor of the time before -- for a sequence never
// played, the tick nought -- and a replica read the present off the two: the
// whole of the session on, which is the end. It said every event at once and
// was put back a snapshot later. Nothing on a replica, which decides none of
// this, but for a player it made itself.
inline void anchorSequence(const World& world, core::InstanceId id, SequencePlayerComponent& player) noexcept
{
    const EngineState& state = world.engineState();
    if (state.networkTopology == NetworkTopology::Replica && !world.local(id))
        return;
    player.anchorTime = static_cast<f32>(player.timePosition);
    player.anchorTick = static_cast<u32>(std::max(state.networkServerClock, state.networkServerTick));
}

// **A stop says from when too**. Stopped, a sequence is at nought:
// its time is the host's to put back, a tick later, and a snapshot taken
// between the ask and that tick carried "stopped" beside the anchor of the
// time it was playing from. Where it had got to is left as it is -- the host
// ends it by that -- and the anchor alone says nought, now.
inline void anchorSequenceStopped(const World& world, core::InstanceId id, SequencePlayerComponent& player) noexcept
{
    const EngineState& state = world.engineState();
    if (state.networkTopology == NetworkTopology::Replica && !world.local(id))
        return;
    player.anchorTime = 0.0f;
    player.anchorTick = static_cast<u32>(std::max(state.networkServerClock, state.networkServerTick));
}

} // namespace engine::scene
