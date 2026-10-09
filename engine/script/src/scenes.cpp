#include "engine/script/scenes.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/core/content_path.h"
#include "engine/core/finite.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/random.h"
#include "engine/core/text_key.h"
#include "engine/input/input.h"
#include "engine/scene/character_replay.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/remote.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// The `scene` global: not a scene but whichever is open when it is read. A
// global is read once and cached by the compiler's import path, so it cannot
// be swapped at a change -- it resolves instead.
constexpr core::u32 OpenScene = 0xFFFFFFFFu;

// How deep a message's table may nest: the save's wall, for the same reason.
constexpr int MaxDepth = 32;

struct SceneUserdata
{
    core::u32 serial = 0;
};

struct SceneLoadUserdata
{
    core::u32 id = 0;
};

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] SceneState& scenes(lua_State* L) noexcept
{
    return context(L).services->scenes;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

[[nodiscard]] core::u32 resolve(lua_State* L, core::u32 serial) noexcept
{
    return serial == OpenScene ? scenes(L).open : serial;
}

[[nodiscard]] core::u32 checkScene(lua_State* L, int index)
{
    const auto* scene =
        static_cast<const SceneUserdata*>(luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::Scene)));
    return resolve(L, scene->serial);
}

[[nodiscard]] std::string pathOf(lua_State* L, core::u32 serial)
{
    const SceneState& state = scenes(L);
    if (serial == state.open)
        return world(L).engineState().currentScene;
    if (serial == state.prepared && state.prepared != 0)
        return state.preparedPath;
    if (const auto found = state.closed.find(serial); found != state.closed.end())
        return found->second;
    return {};
}

// `scenes/arena.scene.json` is `arena`: the file's name, without what every
// scene file ends in.
[[nodiscard]] std::string nameOf(std::string_view path)
{
    if (const std::size_t slash = path.find_last_of("/\\"); slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    for (const std::string_view suffix : {std::string_view{".scene.json"}, std::string_view{".json"}}) {
        if (path.size() > suffix.size() && path.substr(path.size() - suffix.size()) == suffix) {
            path.remove_suffix(suffix.size());
            break;
        }
    }
    return std::string(path);
}

// Open, or prepared and not open yet: a scene that will still close.
[[nodiscard]] bool alive(lua_State* L, core::u32 serial) noexcept
{
    const SceneState& state = scenes(L);
    return serial == state.open || (state.prepared != 0 && serial == state.prepared);
}

[[noreturn]] void raiseClosed(lua_State* L, core::u32 serial)
{
    const std::string name = nameOf(pathOf(L, serial));
    const core::I18nArg args[] = {{"scene", std::string_view{name}}};
    raise(L, ENG_TR("script.err.scene_closed"), args);
}

// Whether `owner` is one of the open scene's scripts. A thread with no script
// -- the engine's own, the command bar -- is the game's, and so is anything
// under `GlobalScriptService`; a script already gone was the scene's.
[[nodiscard]] bool ownedByScene(lua_State* L, core::InstanceId owner)
{
    if (!owner.valid())
        return false;
    scene::World& w = world(L);
    if (!w.alive(owner) || w.destroyed(owner))
        return true;
    const core::InstanceId global =
        w.findFirstChildOfClass(services(L).dataModel, w.classes().findId(w.atoms().lookup("GlobalScriptService")));
    return !(global.valid() && w.isAncestorOf(global, owner));
}

[[nodiscard]] std::string topicOf(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        luaL_typeerrorL(L, index, "string");
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    if (length == 0)
        raise(L, ENG_TR("script.err.message_topic"));
    return std::string(text, length);
}

// --- What a message carries (ADR 0124 §7) ------------------------------------

[[noreturn]] void raiseValue(lua_State* L, std::string_view what, const std::string& path)
{
    const core::I18nArg args[] = {{"valueType", what}, {"path", std::string_view{path}}};
    raise(L, ENG_TR("script.err.message_value"), args);
}

struct CopyContext
{
    std::vector<const void*> ancestors;
    bool metatable = false;
};

// Pushes a copy of the value at `index` onto `L`: equal for the value types,
// the same instance for an instance, a fresh table or buffer for those, and a
// keyed error naming `path` for what a message cannot carry.
void pushCopy(lua_State* L, int index, std::string& path, CopyContext& copy, int depth)
{
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
    case LUA_TBOOLEAN:
    case LUA_TNUMBER:
    case LUA_TSTRING:
    case LUA_TVECTOR:
        lua_pushvalue(L, index);
        return;
    case LUA_TBUFFER: {
        size_t length = 0;
        const void* bytes = lua_tobuffer(L, index, &length);
        void* made = lua_newbuffer(L, length);
        if (length > 0)
            std::memcpy(made, bytes, length);
        return;
    }
    case LUA_TUSERDATA:
        switch (static_cast<UserdataTag>(lua_userdatatag(L, index))) {
        // Values that cannot be changed in place: equal is the same one.
        case UserdataTag::Instance:
        case UserdataTag::CFrame:
        case UserdataTag::Color3:
        case UserdataTag::EnumItem:
        case UserdataTag::Vector2:
        case UserdataTag::UDim:
        case UserdataTag::UDim2:
        case UserdataTag::Rect:
        case UserdataTag::ColorSequence:
        case UserdataTag::NumberSequence:
            lua_pushvalue(L, index);
            return;
        default:
            // A live object: a signal, a connection, a tween, a scene. Its
            // data can be sent; it cannot.
            raiseValue(L, luaL_typename(L, index), path);
        }
    case LUA_TTABLE:
        break;
    default:
        // A function or a thread would carry its sender's variables past the
        // sender's scene.
        raiseValue(L, luaL_typename(L, index), path);
    }

    const void* identity = lua_topointer(L, index);
    if (std::find(copy.ancestors.begin(), copy.ancestors.end(), identity) != copy.ancestors.end())
        raiseValue(L, "a table that contains itself", path);
    if (depth >= MaxDepth)
        raiseValue(L, "a table nested too deep", path);
    if (lua_getmetatable(L, index) != 0) {
        lua_pop(L, 1);
        copy.metatable = true;
    }
    copy.ancestors.push_back(identity);

    lua_newtable(L);
    const int made = lua_gettop(L);
    const std::size_t before = path.size();
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        // Keys: names, or an array's positions.
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t length = 0;
            const char* key = lua_tolstring(L, -2, &length);
            path.append(".").append(key, length);
        }
        else if (lua_type(L, -2) == LUA_TNUMBER && lua_tonumber(L, -2) >= 1.0 &&
                 lua_tonumber(L, -2) == static_cast<double>(static_cast<core::i64>(lua_tonumber(L, -2)))) {
            path.append("[").append(std::to_string(static_cast<core::i64>(lua_tonumber(L, -2)))).append("]");
        }
        else {
            raiseValue(L, std::string(luaL_typename(L, -2)).append(" key"), path);
        }
        lua_pushvalue(L, -2);
        pushCopy(L, -2, path, copy, depth + 1);
        lua_rawset(L, made);
        lua_pop(L, 1);
        path.resize(before);
    }
    copy.ancestors.pop_back();
}

// `{ n = count, ... }` of copies of the `count` values from `first`.
void pushSnapshot(lua_State* L, int first, int count, bool warnMetatable)
{
    CopyContext copy;
    lua_createtable(L, count, 1);
    const int snapshot = lua_gettop(L);
    for (int at = 0; at < count; ++at) {
        std::string path = "argument " + std::to_string(at + 1);
        pushCopy(L, first + at, path, copy, 0);
        lua_rawseti(L, snapshot, at + 1);
    }
    lua_pushinteger(L, count);
    lua_setfield(L, snapshot, "n");
    if (warnMetatable && copy.metatable && scenes(L).developer)
        core::log(core::LogLevel::Warn, ENG_TR("script.warn.message_metatable"));
}

[[nodiscard]] std::string mailboxName(lua_State* L, core::u32 mailbox)
{
    if (mailbox == GameMailbox)
        return "game";
    return "scene " + nameOf(pathOf(L, mailbox));
}

// The deferred half of a send: the bindings are the ones present when it
// arrives, each handler on its own coroutine, belonging to the script that
// bound it, with its own copy of the values.
int deliverTrampoline(lua_State* L)
{
    const auto mailbox = static_cast<core::u32>(lua_tointeger(L, 1));
    size_t length = 0;
    const char* text = lua_tolstring(L, 2, &length);
    const std::string topic = text != nullptr ? std::string(text, length) : std::string{};
    const int values = 3;
    SceneState& state = scenes(L);

    if (mailbox != GameMailbox && mailbox != state.open) {
        if (state.developer) {
            const std::string name = mailboxName(L, mailbox);
            const core::I18nArg args[] = {{"topic", std::string_view{topic}}, {"mailbox", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("script.warn.message_scene_closed"), args);
        }
        return 0;
    }

    std::vector<MessageBinding> matching;
    for (const MessageBinding& binding : state.bindings) {
        if (binding.mailbox == mailbox && binding.topic == topic)
            matching.push_back(binding);
    }
    if (matching.empty()) {
        if (state.developer) {
            const std::string name = mailboxName(L, mailbox);
            const core::I18nArg args[] = {{"topic", std::string_view{topic}}, {"mailbox", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("script.warn.message_unheard"), args);
        }
        return 0;
    }

    lua_getfield(L, values, "n");
    const int count = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    for (const MessageBinding& binding : matching) {
        lua_getref(L, binding.functionRef);
        const void* env = runEnvOfFunction(L, -1);
        lua_pop(L, 1);
        if (suppressionFor(L, binding.owner, env) != SuppressReason::None)
            continue;
        lua_State* co = lua_newthread(L);
        // The binder's globals, so the thread is the binder's: suppressed with
        // its script, and `script` inside it is that script.
        lua_getref(L, binding.functionRef);
        lua_getfenv(L, -1);
        if (lua_istable(L, -1)) {
            lua_xmove(L, co, 1);
            lua_replace(co, LUA_GLOBALSINDEX);
        }
        else {
            lua_pop(L, 1);
        }
        lua_xmove(L, co, 1);
        CopyContext copy;
        for (int at = 1; at <= count; ++at) {
            lua_rawgeti(L, values, at);
            std::string path;
            pushCopy(L, -1, path, copy, 0);
            lua_remove(L, -2);
            lua_xmove(L, co, 1);
        }
        (void)resumeScheduled(L, co, count);
        lua_pop(L, 1);
    }
    return 0;
}

// Queues the delivery of the snapshot on top of the stack, which it pops.
void enqueueDelivery(lua_State* L, core::u32 mailbox, const std::string& topic)
{
    const int snapshot = lua_gettop(L);
    lua_pushinteger(L, static_cast<int>(mailbox));
    lua_pushlstring(L, topic.data(), topic.size());
    lua_pushvalue(L, snapshot);
    const u32 base = captureDeferredArguments(L, snapshot + 1, 3);
    lua_pop(L, 1);

    lua_State* thread = lua_newthread(L);
    lua_pushcfunction(thread, deliverTrampoline, "message");
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    if (!enqueueTaskCallback(L, ref, base, 3))
        (void)lua_unref(L, ref);
}

void send(lua_State* L, core::u32 mailbox)
{
    const std::string topic = topicOf(L, 2);
    SceneState& state = scenes(L);
    // Copied now, at the call, where a mistake is reported.
    pushSnapshot(L, 3, lua_gettop(L) - 2, true);

    if (!alive(L, mailbox) && mailbox != GameMailbox) {
        lua_pop(L, 1);
        if (state.developer) {
            const std::string name = mailboxName(L, mailbox);
            const core::I18nArg args[] = {{"topic", std::string_view{topic}}, {"mailbox", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("script.warn.message_scene_closed"), args);
        }
        return;
    }
    if (mailbox != GameMailbox && mailbox != state.open) {
        // Prepared and not open: held until its scripts have started.
        state.held.push_back(HeldMessage{mailbox, topic, lua_ref(L, -1)});
        lua_pop(L, 1);
        return;
    }
    enqueueDelivery(L, mailbox, topic);
}

void bind(lua_State* L, core::u32 mailbox)
{
    std::string topic = topicOf(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    scenes(L).bindings.push_back(MessageBinding{mailbox, std::move(topic), ref, scriptOfThread(L)});
}

// --- Scene members -------------------------------------------------------------

int sceneGetName(lua_State* L)
{
    const std::string name = nameOf(pathOf(L, checkScene(L, 1)));
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

int sceneGetPath(lua_State* L)
{
    const std::string path = pathOf(L, checkScene(L, 1));
    lua_pushlstring(L, path.data(), path.size());
    return 1;
}

int sceneIsOpen(lua_State* L)
{
    lua_pushboolean(L, checkScene(L, 1) == scenes(L).open);
    return 1;
}

int sceneBindToClose(lua_State* L)
{
    const core::u32 serial = checkScene(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!alive(L, serial))
        raiseClosed(L, serial);
    lua_pushvalue(L, 2);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    scenes(L).closeHandlers.push_back(OwnedHandler{ref, scriptOfThread(L), serial});
    return 0;
}

int sceneSendMessage(lua_State* L)
{
    send(L, checkScene(L, 1));
    return 0;
}

int sceneBindToMessage(lua_State* L)
{
    const core::u32 serial = checkScene(L, 1);
    if (!alive(L, serial))
        raiseClosed(L, serial);
    bind(L, serial);
    return 0;
}

int sceneEquals(lua_State* L)
{
    const auto* left =
        static_cast<const SceneUserdata*>(lua_touserdatatagged(L, 1, static_cast<int>(UserdataTag::Scene)));
    const auto* right =
        static_cast<const SceneUserdata*>(lua_touserdatatagged(L, 2, static_cast<int>(UserdataTag::Scene)));
    lua_pushboolean(L, left != nullptr && right != nullptr && resolve(L, left->serial) == resolve(L, right->serial));
    return 1;
}

int sceneTostring(lua_State* L)
{
    const std::string name = nameOf(pathOf(L, checkScene(L, 1)));
    lua_pushfstring(L, "Scene(%s)", name.c_str());
    return 1;
}

void pushSerial(lua_State* L, core::u32 serial)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(SceneUserdata), static_cast<int>(UserdataTag::Scene));
    static_cast<SceneUserdata*>(memory)->serial = serial;
}

void unref(lua_State* L, int ref)
{
    if (ref != -1)
        (void)lua_unref(L, ref);
}

// --- SceneLoad (ADR 0125) --------------------------------------------------------

[[nodiscard]] SceneLoadRecord* findLoad(lua_State* L, core::u32 id) noexcept
{
    for (SceneLoadRecord& record : scenes(L).loads) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

[[nodiscard]] SceneLoadRecord& checkLoad(lua_State* L, int index)
{
    const auto* load = static_cast<const SceneLoadUserdata*>(
        luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::SceneLoad)));
    SceneLoadRecord* record = findLoad(L, load->id);
    if (record == nullptr)
        luaL_argerrorL(L, index, "SceneLoad");
    return *record;
}

void pushLoad(lua_State* L, core::u32 id)
{
    void* memory =
        lua_newuserdatataggedwithmetatable(L, sizeof(SceneLoadUserdata), static_cast<int>(UserdataTag::SceneLoad));
    static_cast<SceneLoadUserdata*>(memory)->id = id;
}

// `Finished`, with whether the scene opened, and the handle let go of what it
// held -- the prepared scene's messages and registrations when it did not.
void finish(lua_State* L, SceneLoadRecord& record, SceneLoadStatus status)
{
    record.status = status;
    if (status != SceneLoadStatus::Done)
        releaseScene(L, record.scene);
    SceneState& state = scenes(L);
    if (state.activeLoad == record.id)
        state.activeLoad = 0;
    record.data.clear();
    lua_pushboolean(L, status == SceneLoadStatus::Done);
    fireSignal(L, record.finished, lua_gettop(L), 1);
    lua_pop(L, 1);
}

int loadGetPath(lua_State* L)
{
    const SceneLoadRecord& record = checkLoad(L, 1);
    lua_pushlstring(L, record.path.data(), record.path.size());
    return 1;
}

int loadGetProgress(lua_State* L)
{
    lua_pushnumber(L, checkLoad(L, 1).progress);
    return 1;
}

int loadGetStatus(lua_State* L)
{
    pushEnumItem(
        L, scene::EnumValue{scene::generated::SceneLoadStatusEnumId, static_cast<core::i32>(checkLoad(L, 1).status)});
    return 1;
}

int loadGetError(lua_State* L)
{
    const SceneLoadRecord& record = checkLoad(L, 1);
    if (record.status != SceneLoadStatus::Failed) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, record.error.data(), record.error.size());
    return 1;
}

int loadGetScene(lua_State* L)
{
    pushSerial(L, checkLoad(L, 1).scene);
    return 1;
}

int loadGetReady(lua_State* L)
{
    pushSignalObject(L, checkLoad(L, 1).ready);
    return 1;
}

int loadGetFinished(lua_State* L)
{
    pushSignalObject(L, checkLoad(L, 1).finished);
    return 1;
}

int loadActivate(lua_State* L)
{
    SceneLoadRecord& record = checkLoad(L, 1);
    if (record.status == SceneLoadStatus::Failed || record.status == SceneLoadStatus::Cancelled) {
        const core::I18nArg args[] = {{"path", std::string_view{record.path}}};
        raise(L, ENG_TR("script.err.scene_load_over"), args);
    }
    record.activate = true;
    return 0;
}

int loadCancel(lua_State* L)
{
    SceneLoadRecord& record = checkLoad(L, 1);
    // Once the switch has begun it is not a load to cancel any more.
    if (record.status == SceneLoadStatus::Preparing || record.status == SceneLoadStatus::Ready)
        finish(L, record, SceneLoadStatus::Cancelled);
    return 0;
}

int loadTostring(lua_State* L)
{
    const SceneLoadRecord& record = checkLoad(L, 1);
    lua_pushfstring(L, "SceneLoad(%s)", record.path.c_str());
    return 1;
}

} // namespace

void registerSceneTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& getters = ctx.getters[static_cast<core::usize>(UserdataTag::Scene)];
    addMember(getters, atoms, "Name", sceneGetName);
    addMember(getters, atoms, "Path", sceneGetPath);

    MemberTable& methods = ctx.methods[static_cast<core::usize>(UserdataTag::Scene)];
    addMember(methods, atoms, "IsOpen", sceneIsOpen);
    addMember(methods, atoms, "BindToClose", sceneBindToClose);
    addMember(methods, atoms, "SendMessage", sceneSendMessage);
    addMember(methods, atoms, "BindToMessage", sceneBindToMessage);

    installTagMetatable(L, UserdataTag::Scene, sceneEquals, sceneTostring);

    MemberTable& loadGetters = ctx.getters[static_cast<core::usize>(UserdataTag::SceneLoad)];
    addMember(loadGetters, atoms, "Path", loadGetPath);
    addMember(loadGetters, atoms, "Progress", loadGetProgress);
    addMember(loadGetters, atoms, "Status", loadGetStatus);
    addMember(loadGetters, atoms, "Error", loadGetError);
    addMember(loadGetters, atoms, "Scene", loadGetScene);
    addMember(loadGetters, atoms, "Ready", loadGetReady);
    addMember(loadGetters, atoms, "Finished", loadGetFinished);
    MemberTable& loadMethods = ctx.methods[static_cast<core::usize>(UserdataTag::SceneLoad)];
    addMember(loadMethods, atoms, "Activate", loadActivate);
    addMember(loadMethods, atoms, "Cancel", loadCancel);
    installTagMetatable(L, UserdataTag::SceneLoad, nullptr, loadTostring);

    pushSerial(L, OpenScene);
    lua_setglobal(L, "scene");
}

int dataModelSendMessage(lua_State* L)
{
    (void)checkInstance(L, 1);
    send(L, GameMailbox);
    return 0;
}

int dataModelBindToMessage(lua_State* L)
{
    (void)checkInstance(L, 1);
    bind(L, GameMailbox);
    return 0;
}

bool sceneMemberGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    scene::World& w = world(L);
    if (key != "CurrentScene" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "SceneService")
        return false;
    pushSerial(L, scenes(L).open);
    return true;
}

bool sceneMemberSet(lua_State* L, core::InstanceId id, std::string_view key)
{
    scene::World& w = world(L);
    if (key != "CurrentScene" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "SceneService")
        return false;
    const core::I18nArg args[] = {{"className", std::string_view{"SceneService"}},
                                  {"property", std::string_view{"CurrentScene"}}};
    raise(L, ENG_TR("scene.err.read_only_property"), args);
}

void pushScene(lua_State* L, core::u32 serial)
{
    pushSerial(L, serial);
}

void setDeveloperWarnings(lua_State* L, bool developer)
{
    scenes(L).developer = developer;
}

void runSceneCloseHandlers(lua_State* L)
{
    SceneState& state = scenes(L);
    // Taken out first: a handler may register another, which does not run in
    // this close (ADR 0124 §5).
    std::vector<OwnedHandler> running;
    std::erase_if(state.closeHandlers, [&](const OwnedHandler& handler) {
        if (handler.scene != state.open)
            return false;
        running.push_back(handler);
        return true;
    });
    for (const OwnedHandler& handler : running) {
        lua_State* co = lua_newthread(L);
        lua_getref(L, handler.functionRef);
        lua_xmove(L, co, 1);
        if (!resumeScheduled(L, co, 0)) {
            lua_pushvalue(L, -1);
            services(L).closePending.push_back(lua_ref(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, handler.functionRef);
    }
}

core::u32 closeScene(lua_State* L, core::u32 next)
{
    SceneState& state = scenes(L);
    ServiceState& service = services(L);
    scene::World& w = world(L);
    const core::u32 closing = state.open;
    const std::string closingPath = w.engineState().currentScene;
    const std::string closingName = nameOf(closingPath);

    // The game's close, asked for by a script that will not see it.
    std::vector<core::InstanceId> warned;
    std::erase_if(service.closeHandlers, [&](const OwnedHandler& handler) {
        if (!ownedByScene(L, handler.owner))
            return false;
        if (state.developer && std::find(warned.begin(), warned.end(), handler.owner) == warned.end()) {
            warned.push_back(handler.owner);
            const std::string script =
                w.alive(handler.owner) ? std::string(w.atoms().text(w.name(handler.owner))) : std::string{};
            const core::I18nArg args[] = {{"script", std::string_view{script}},
                                          {"scene", std::string_view{closingName}}};
            core::log(core::LogLevel::Warn, ENG_TR("script.warn.close_handler_dropped"), args);
        }
        unref(L, handler.functionRef);
        return true;
    });
    // This scene's own handlers ran already, or were never going to; and a
    // prepared scene's, registered by a script of this one, go with it.
    std::erase_if(state.closeHandlers, [&](const OwnedHandler& handler) {
        if (handler.scene != closing && !ownedByScene(L, handler.owner))
            return false;
        unref(L, handler.functionRef);
        return true;
    });
    std::erase_if(state.bindings, [&](const MessageBinding& binding) {
        if (binding.mailbox != closing && !ownedByScene(L, binding.owner))
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    // A render step goes with the script that bound it (ADR 0136, 0124), and
    // so does an intent writer (G38) and a predicted step or touch (G37).
    std::erase_if(state.renderSteps, [&](const RenderStep& step) {
        if (!ownedByScene(L, step.owner))
            return false;
        unref(L, step.functionRef);
        return true;
    });
    std::erase_if(state.intentBindings, [&](const IntentBinding& binding) {
        if (!ownedByScene(L, binding.owner))
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    for (std::vector<PredictedBinding>* bindings : {&state.predictedSteps, &state.predictedTouches}) {
        std::erase_if(*bindings, [&](const PredictedBinding& binding) {
            if (!ownedByScene(L, binding.owner))
                return false;
            unref(L, binding.functionRef);
            return true;
        });
    }

    state.closed[closing] = closingPath;
    if (next != 0 && next == state.prepared) {
        state.prepared = 0;
        state.preparedPath.clear();
    }
    else {
        next = state.next++;
    }
    state.open = next;
    return next;
}

// --- Render steps (ADR 0136) ---------------------------------------------------

int runServiceBindToRenderStep(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const double priority = luaL_checknumber(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    if (length == 0)
        raise(L, ENG_TR("script.err.render_step_name"));
    if (!std::isfinite(priority))
        raise(L, ENG_TR("script.err.render_step_priority"));
    const std::string name{text, length};
    SceneState& state = scenes(L);
    // A name already bound is replaced, keeping nothing of the old binding --
    // its order included, so a rebind goes after its equals as a new one does.
    std::erase_if(state.renderSteps, [&](const RenderStep& step) {
        if (step.name != name)
            return false;
        unref(L, step.functionRef);
        return true;
    });
    lua_pushvalue(L, 4);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    state.renderSteps.push_back(RenderStep{name, priority, state.nextRenderStep++, ref, scriptOfThread(L)});
    return 0;
}

int runServiceUnbindFromRenderStep(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const std::string_view name{text, length};
    std::erase_if(scenes(L).renderSteps, [&](const RenderStep& step) {
        if (step.name != name)
            return false;
        unref(L, step.functionRef);
        return true;
    });
    return 0;
}

void runRenderSteps(lua_State* L, double dt)
{
    services(L).renderPhasesRun = true;
    // Taken in order first: a step may bind, rebind or unbind, and what it
    // does takes effect from the next frame.
    std::vector<RenderStep> running = scenes(L).renderSteps;
    std::stable_sort(running.begin(), running.end(), [](const RenderStep& a, const RenderStep& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
    });
    for (const RenderStep& step : running) {
        // Still bound: a step earlier this frame may have unbound it.
        const std::vector<RenderStep>& bound = scenes(L).renderSteps;
        if (std::find_if(bound.begin(), bound.end(), [&](const RenderStep& now) { return now.order == step.order; }) ==
            bound.end())
            continue;
        // A step of a script that is disabled, destroyed, or of a run that has
        // ended does not run: it belongs to its run (ADR 0137 §2).
        lua_getref(L, step.functionRef);
        const SuppressReason reason = suppressionFor(L, scriptOfFunction(L, -1), runEnvOfFunction(L, -1));
        lua_pop(L, 1);
        if (reason != SuppressReason::None)
            continue;
        lua_State* co = lua_newthread(L);
        lua_getref(L, step.functionRef);
        lua_xmove(L, co, 1);
        lua_pushnumber(co, dt);
        (void)resumeScheduled(L, co, 1);
        lua_pop(L, 1);
    }
}

// --- Intents written by code (G38) ----------------------------------------------

int runServiceBindToIntent(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (length == 0)
        raise(L, ENG_TR("script.err.intent_binding_name"));
    core::InstanceId player;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
        player = checkInstance(L, 4);
        const auto& w = *context(L).world;
        const auto* participant = w.players().find(player);
        if (participant == nullptr || !participant->local || w.destroyed(player) ||
            std::find(w.engineState().leavingPlayers.begin(), w.engineState().leavingPlayers.end(), player) !=
                w.engineState().leavingPlayers.end())
            raise(L, ENG_TR("script.err.intent_local_player"));
    }
    const std::string name{text, length};
    SceneState& state = scenes(L);
    std::erase_if(state.intentBindings, [&](const IntentBinding& binding) {
        if (binding.name != name)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    lua_pushvalue(L, 3);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    state.intentBindings.push_back(IntentBinding{name, state.nextIntentBinding++, ref, scriptOfThread(L), player});
    return 0;
}

int runServiceUnbindFromIntent(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const std::string_view name{text, length};
    std::erase_if(scenes(L).intentBindings, [&](const IntentBinding& binding) {
        if (binding.name != name)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    return 0;
}

namespace {

struct IntentWriterUserdata
{
    core::u32 run = 0;
    core::InstanceId player;
};

// `IntentWriter:Set(action, value)`: the local player's intent named
// `action`, written over whatever the input made of it.
int intentWriterSet(lua_State* L)
{
    const auto* writer = static_cast<const IntentWriterUserdata*>(
        luaL_checkudatatagged(L, 1, static_cast<int>(UserdataTag::IntentWriter)));
    SceneState& state = scenes(L);
    if (writer->run == 0 || writer->run != state.intentRun)
        raise(L, ENG_TR("script.err.intent_writer_spent"));
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    scene::PlayerIntent intent;
    if (lua_isboolean(L, 3)) {
        intent.type = static_cast<i32>(input::ActionType::Bool);
        intent.pressed = lua_toboolean(L, 3) != 0;
        intent.axis = core::Vec3{intent.pressed ? 1.0f : 0.0f, 0.0f, 0.0f};
    }
    else if (lua_isnumber(L, 3)) {
        const double value = lua_tonumber(L, 3);
        if (!std::isfinite(value))
            raise(L, ENG_TR("script.err.not_finite"));
        intent.type = static_cast<i32>(input::ActionType::Direction1D);
        intent.axis = core::Vec3{static_cast<f32>(value), 0.0f, 0.0f};
        intent.pressed = value != 0.0;
    }
    else if (lua_isvector(L, 3)) {
        const core::Vec3 value = checkVector3(L, 3);
        if (!core::isFinite(value))
            raise(L, ENG_TR("script.err.not_finite"));
        intent.type = static_cast<i32>(input::ActionType::Direction3D);
        intent.axis = value;
        intent.pressed = value.x != 0.0f || value.y != 0.0f || value.z != 0.0f;
    }
    else if (lua_userdatatag(L, 3) == static_cast<int>(UserdataTag::Vector2)) {
        const core::Vec2 value = checkVector2(L, 3);
        if (!std::isfinite(value.x) || !std::isfinite(value.y))
            raise(L, ENG_TR("script.err.not_finite"));
        intent.type = static_cast<i32>(input::ActionType::Direction2D);
        intent.axis = core::Vec3{value.x, value.y, 0.0f};
        intent.pressed = value.x != 0.0f || value.y != 0.0f;
    }
    else {
        raise(L, ENG_TR("script.err.intent_value"));
    }

    scene::World& world = *context(L).world;
    intent.action = world.atoms().intern(std::string_view{text, length});
    scene::PlayerComponent* player = world.players().find(writer->player);
    if (player == nullptr)
        return 0;
    for (scene::PlayerIntent& held : player->intents) {
        if (held.action == intent.action) {
            held = intent;
            return 0;
        }
    }
    player->intents.push_back(intent);
    return 0;
}

} // namespace

void runIntentWriters(lua_State* L)
{
    SceneState& state = scenes(L);
    if (state.intentBindings.empty())
        return;
    // A run of its own, so a writer kept from this one writes nothing in the
    // next; taken in bind order first, as a writer may bind or unbind.
    state.intentRun = ++state.intentRuns == 0 ? ++state.intentRuns : state.intentRuns;
    std::vector<IntentBinding> running = state.intentBindings;
    std::stable_sort(running.begin(), running.end(),
                     [](const IntentBinding& a, const IntentBinding& b) { return a.order < b.order; });
    for (const IntentBinding& binding : running) {
        const std::vector<IntentBinding>& bound = scenes(L).intentBindings;
        if (std::find_if(bound.begin(), bound.end(),
                         [&](const IntentBinding& now) { return now.order == binding.order; }) == bound.end())
            continue;
        lua_getref(L, binding.functionRef);
        const SuppressReason reason = suppressionFor(L, scriptOfFunction(L, -1), runEnvOfFunction(L, -1));
        lua_pop(L, 1);
        if (reason != SuppressReason::None)
            continue;
        const auto& w = *context(L).world;
        const auto player = binding.player.valid() ? binding.player : scene::localPlayerOf(w);
        const auto* participant = w.players().find(player);
        if (binding.player.valid() &&
            (participant == nullptr || !participant->local || w.destroyed(player) ||
             std::find(w.engineState().leavingPlayers.begin(), w.engineState().leavingPlayers.end(), player) !=
                 w.engineState().leavingPlayers.end()))
            continue;
        lua_State* co = lua_newthread(L);
        lua_getref(L, binding.functionRef);
        lua_xmove(L, co, 1);
        void* memory = lua_newuserdatataggedwithmetatable(co, sizeof(IntentWriterUserdata),
                                                          static_cast<int>(UserdataTag::IntentWriter));
        static_cast<IntentWriterUserdata*>(memory)->run = scenes(L).intentRun;
        static_cast<IntentWriterUserdata*>(memory)->player = player;
        (void)resumeScheduled(L, co, 1);
        lua_pop(L, 1);
    }
    scenes(L).intentRun = 0;
}

// --- Scripts in the predicted step (G37) ------------------------------------------

void pushIntentValue(lua_State* L, const scene::PlayerIntent& intent)
{
    // The same switch `InputAction:GetState` answers through, so the two can
    // never disagree about what a value looks like.
    switch (static_cast<input::ActionType>(intent.type)) {
    case input::ActionType::Bool:
        lua_pushboolean(L, intent.pressed ? 1 : 0);
        return;
    case input::ActionType::Direction1D:
        lua_pushnumber(L, static_cast<double>(intent.axis.x));
        return;
    case input::ActionType::Direction2D:
    case input::ActionType::ViewportPosition:
        pushVector2(L, core::Vec2{intent.axis.x, intent.axis.y});
        return;
    case input::ActionType::Direction3D:
        pushVector3(L, intent.axis);
        return;
    }
    lua_pushboolean(L, 0);
}

int runServiceBindToPredictedStep(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (length == 0)
        raise(L, ENG_TR("script.err.intent_binding_name"));
    const std::string name{text, length};
    SceneState& state = scenes(L);
    std::erase_if(state.predictedSteps, [&](const PredictedBinding& binding) {
        if (binding.name != name)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    lua_pushvalue(L, 3);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    state.predictedSteps.push_back(PredictedBinding{name, state.nextPredicted++, ref, scriptOfThread(L), {}});
    return 0;
}

int runServiceUnbindFromPredictedStep(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const std::string_view name{text, length};
    std::erase_if(scenes(L).predictedSteps, [&](const PredictedBinding& binding) {
        if (binding.name != name)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    return 0;
}

int partBindToPredictedTouch(lua_State* L)
{
    const core::InstanceId part = checkInstance(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    SceneState& state = scenes(L);
    // One a part: bound again, the new function takes its place.
    std::erase_if(state.predictedTouches, [&](const PredictedBinding& binding) {
        if (binding.part != part && context(L).world->alive(binding.part))
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    lua_pushvalue(L, 2);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    state.predictedTouches.push_back(PredictedBinding{{}, state.nextPredicted++, ref, scriptOfThread(L), part});
    return 0;
}

int partUnbindFromPredictedTouch(lua_State* L)
{
    const core::InstanceId part = checkInstance(L, 1);
    std::erase_if(scenes(L).predictedTouches, [&](const PredictedBinding& binding) {
        if (binding.part != part)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
    return 0;
}

bool predictedTouchBound(lua_State* L, core::InstanceId part)
{
    const std::vector<PredictedBinding>& bound = scenes(L).predictedTouches;
    return std::any_of(bound.begin(), bound.end(),
                       [&](const PredictedBinding& binding) { return binding.part == part; });
}

namespace {

struct PredictedStepUserdata
{
    core::u32 run = 0;
};

// The step a `PredictedStep` stands for, or an error: one kept past its call
// reads nothing.
const scene::PredictedTick& checkPredicted(lua_State* L)
{
    const auto* step = static_cast<const PredictedStepUserdata*>(
        luaL_checkudatatagged(L, 1, static_cast<int>(UserdataTag::PredictedStep)));
    const SceneState& state = scenes(L);
    if (step->run == 0 || step->run != state.predictedRun || state.predictedNow == nullptr)
        raise(L, ENG_TR("script.err.predicted_step_spent"));
    return *state.predictedNow;
}

int predictedCharacter(lua_State* L)
{
    pushInstance(L, checkPredicted(L).character);
    return 1;
}

int predictedPlayer(lua_State* L)
{
    pushInstance(L, checkPredicted(L).player);
    return 1;
}

int predictedTickNumber(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(checkPredicted(L).tick));
    return 1;
}

int predictedDeltaTime(lua_State* L)
{
    lua_pushnumber(L, checkPredicted(L).dt);
    return 1;
}

int predictedIsReplay(lua_State* L)
{
    lua_pushboolean(L, checkPredicted(L).replay ? 1 : 0);
    return 1;
}

// **The same draws live and stepped again, on both ends**: seeded by the
// player and their tick, which is all a step is.
int predictedRandom(lua_State* L)
{
    const scene::PredictedTick& tick = checkPredicted(L);
    const core::u64 seed = (tick.tick * 0x9E3779B97F4A7C15ull) ^ (static_cast<core::u64>(tick.userId) << 1u);
    pushRandom(L, core::Pcg32{seed});
    return 1;
}

int predictedGetIntent(lua_State* L)
{
    const scene::PredictedTick& tick = checkPredicted(L);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    // **Interned here, by the code that reads it**, as `Player:GetIntent`
    // does: what makes a peer's intent of that name arrive.
    const core::NameAtom name = context(L).world->atoms().intern(std::string_view{text, length});
    for (const scene::PlayerIntent& intent : tick.intents) {
        if (name.id != 0 && intent.action == name) {
            pushIntentValue(L, intent);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

int predictedPressed(lua_State* L)
{
    const scene::PredictedTick& tick = checkPredicted(L);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const core::NameAtom name = context(L).world->atoms().intern(std::string_view{text, length});
    const bool pressed =
        name.id != 0 && std::find(tick.presses.begin(), tick.presses.end(), name) != tick.presses.end();
    lua_pushboolean(L, pressed ? 1 : 0);
    return 1;
}

// One predicted call: the bindings in the order bound, each handed what
// `push` pushes, with the step `tick` in force while they run.
template <typename Push>
void runPredicted(lua_State* L, const scene::PredictedTick& tick, const std::vector<PredictedBinding>& bindings,
                  Push push)
{
    SceneState& state = scenes(L);
    VmContext& ctx = context(L);
    // Kept for a nested call -- a step that steps the simulation itself -- and
    // put back after.
    const scene::PredictedTick* outer = state.predictedNow;
    const core::u32 outerRun = state.predictedRun;
    const core::InstanceId outerCharacter = ctx.predictedCharacter;
    state.predictedNow = &tick;
    state.predictedRun = ++state.predictedRuns == 0 ? ++state.predictedRuns : state.predictedRuns;
    ctx.predictedCharacter = tick.character;
    const core::u32 run = state.predictedRun;
    // Taken in bind order first: a function may bind or unbind, and what it
    // does takes effect from the next step.
    std::vector<PredictedBinding> running = bindings;
    std::stable_sort(running.begin(), running.end(),
                     [](const PredictedBinding& a, const PredictedBinding& b) { return a.order < b.order; });
    for (const PredictedBinding& binding : running) {
        lua_getref(L, binding.functionRef);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        const SuppressReason reason = suppressionFor(L, scriptOfFunction(L, -1), runEnvOfFunction(L, -1));
        lua_pop(L, 1);
        if (reason != SuppressReason::None)
            continue;
        lua_State* co = lua_newthread(L);
        lua_getref(L, binding.functionRef);
        lua_xmove(L, co, 1);
        const int count = push(co);
        void* memory = lua_newuserdatataggedwithmetatable(co, sizeof(PredictedStepUserdata),
                                                          static_cast<int>(UserdataTag::PredictedStep));
        static_cast<PredictedStepUserdata*>(memory)->run = run;
        (void)callUnyielding(L, co, count + 1);
        lua_pop(L, 1);
    }
    state.predictedNow = outer;
    state.predictedRun = outerRun;
    ctx.predictedCharacter = outerCharacter;
}

} // namespace

void runPredictedStep(lua_State* L, const scene::PredictedTick& tick)
{
    if (scenes(L).predictedSteps.empty())
        return;
    runPredicted(L, tick, scenes(L).predictedSteps, [](lua_State*) { return 0; });
}

void runPredictedTouch(lua_State* L, const scene::PredictedTick& tick, core::InstanceId part)
{
    std::vector<PredictedBinding> bound;
    for (const PredictedBinding& binding : scenes(L).predictedTouches) {
        if (binding.part == part)
            bound.push_back(binding);
    }
    if (bound.empty())
        return;
    runPredicted(L, tick, bound, [&](lua_State* co) {
        pushInstance(co, tick.character);
        return 1;
    });
}

void registerPredictedStep(lua_State* L)
{
    VmContext& ctx = context(L);
    MemberTable& getters = ctx.getters[static_cast<usize>(UserdataTag::PredictedStep)];
    addMember(getters, ctx.world->atoms(), "Character", predictedCharacter);
    addMember(getters, ctx.world->atoms(), "Player", predictedPlayer);
    addMember(getters, ctx.world->atoms(), "Tick", predictedTickNumber);
    addMember(getters, ctx.world->atoms(), "DeltaTime", predictedDeltaTime);
    addMember(getters, ctx.world->atoms(), "Replaying", predictedIsReplay);
    addMember(getters, ctx.world->atoms(), "Random", predictedRandom);
    MemberTable& methods = ctx.methods[static_cast<usize>(UserdataTag::PredictedStep)];
    addMember(methods, ctx.world->atoms(), "GetIntent", predictedGetIntent);
    addMember(methods, ctx.world->atoms(), "Pressed", predictedPressed);
    installTagMetatable(L, UserdataTag::PredictedStep, nullptr, nullptr);
}

void registerIntentWriter(lua_State* L)
{
    VmContext& ctx = context(L);
    MemberTable& methods = ctx.methods[static_cast<usize>(UserdataTag::IntentWriter)];
    addMember(methods, ctx.world->atoms(), "Set", intentWriterSet);
    installTagMetatable(L, UserdataTag::IntentWriter, nullptr, nullptr);
}

void deliverHeldMessages(lua_State* L)
{
    SceneState& state = scenes(L);
    std::vector<HeldMessage> held;
    held.swap(state.held);
    for (HeldMessage& message : held) {
        if (message.mailbox != state.open) {
            // For a prepared scene that is not this one: still held.
            if (message.mailbox == state.prepared && state.prepared != 0)
                state.held.push_back(std::move(message));
            else
                unref(L, message.valuesRef);
            continue;
        }
        lua_getref(L, message.valuesRef);
        (void)lua_unref(L, message.valuesRef);
        enqueueDelivery(L, message.mailbox, message.topic);
    }
}

core::u32 reserveScene(lua_State* L, std::string_view path)
{
    SceneState& state = scenes(L);
    if (state.prepared != 0)
        releaseScene(L, state.prepared);
    state.prepared = state.next++;
    state.preparedPath = std::string(path);
    return state.prepared;
}

void releaseScene(lua_State* L, core::u32 serial)
{
    SceneState& state = scenes(L);
    if (serial == 0 || serial != state.prepared)
        return;
    state.closed[serial] = state.preparedPath;
    state.prepared = 0;
    state.preparedPath.clear();
    std::erase_if(state.held, [&](const HeldMessage& message) {
        if (message.mailbox != serial)
            return false;
        unref(L, message.valuesRef);
        return true;
    });
    std::erase_if(state.closeHandlers, [&](const OwnedHandler& handler) {
        if (handler.scene != serial)
            return false;
        unref(L, handler.functionRef);
        return true;
    });
    std::erase_if(state.bindings, [&](const MessageBinding& binding) {
        if (binding.mailbox != serial)
            return false;
        unref(L, binding.functionRef);
        return true;
    });
}

int sceneServiceLoadSceneAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const std::string path{text, length};
    scene::World& w = world(L);
    if (w.engineState().networkTopology == scene::NetworkTopology::Replica)
        raise(L, ENG_TR("scene.err.scene_is_the_servers"));
    if (path.empty()) {
        const core::I18nArg args[] = {{"path", std::string_view{path}}};
        raise(L, ENG_TR("scene.err.scene_path_empty"), args);
    }
    if (!core::safeRelativePath(path).has_value()) {
        const core::I18nArg args[] = {{"path", std::string_view{path}}};
        raise(L, ENG_TR("scene.err.scene_path_invalid"), args);
    }

    bool activate = true;
    std::vector<core::u8> data;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "Activate");
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1))
                raise(L, ENG_TR("script.err.scene_load_options"));
            activate = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "Data");
        if (!lua_isnil(L, -1)) {
            std::vector<core::InstanceId> refs;
            encodeRemoteArguments(L, lua_gettop(L), 1, data, refs);
            if (!refs.empty())
                raise(L, ENG_TR("scene.err.load_data_instance"));
        }
        lua_pop(L, 1);
    }

    cancelSceneLoad(L);
    SceneState& state = scenes(L);
    SceneLoadRecord record;
    record.id = state.nextLoad++;
    record.path = path;
    record.data = std::move(data);
    record.activate = activate;
    record.scene = reserveScene(L, path);
    record.ready = createScriptSignal(L);
    record.finished = createScriptSignal(L);
    state.loads.push_back(std::move(record));
    state.activeLoad = state.loads.back().id;
    state.loadRequested = true;
    pushLoad(L, state.activeLoad);
    return 1;
}

void cancelSceneLoad(lua_State* L)
{
    SceneState& state = scenes(L);
    if (state.activeLoad == 0)
        return;
    SceneLoadRecord* record = findLoad(L, state.activeLoad);
    state.activeLoad = 0;
    state.loadRequested = false;
    if (record == nullptr || record->status == SceneLoadStatus::Activating)
        return;
    const core::I18nArg args[] = {{"path", std::string_view{record->path}}};
    core::log(core::LogLevel::Info, ENG_TR("scene.info.load_cancelled"), args);
    finish(L, *record, SceneLoadStatus::Cancelled);
}

std::optional<SceneLoadRequest> takeSceneLoadRequest(lua_State* L)
{
    SceneState& state = scenes(L);
    if (!state.loadRequested)
        return std::nullopt;
    state.loadRequested = false;
    const SceneLoadRecord* record = findLoad(L, state.activeLoad);
    if (record == nullptr)
        return std::nullopt;
    return SceneLoadRequest{record->id, record->path};
}

SceneLoadRecord* activeSceneLoad(lua_State* L)
{
    SceneState& state = scenes(L);
    return state.activeLoad == 0 ? nullptr : findLoad(L, state.activeLoad);
}

void setSceneLoadProgress(lua_State* L, core::f64 progress)
{
    if (SceneLoadRecord* record = activeSceneLoad(L))
        record->progress = std::clamp(std::max(record->progress, progress), 0.0, 1.0);
}

void sceneLoadReady(lua_State* L)
{
    SceneLoadRecord* record = activeSceneLoad(L);
    if (record == nullptr || record->status != SceneLoadStatus::Preparing)
        return;
    record->status = SceneLoadStatus::Ready;
    record->progress = 1.0;
    fireSignal(L, record->ready, lua_gettop(L) + 1, 0);
}

void sceneLoadFailed(lua_State* L, std::string_view message)
{
    SceneLoadRecord* record = activeSceneLoad(L);
    if (record == nullptr)
        return;
    record->error = std::string(message);
    finish(L, *record, SceneLoadStatus::Failed);
}

void sceneLoadActivating(lua_State* L)
{
    if (SceneLoadRecord* record = activeSceneLoad(L))
        record->status = SceneLoadStatus::Activating;
}

void sceneLoadDone(lua_State* L)
{
    if (SceneLoadRecord* record = activeSceneLoad(L))
        finish(L, *record, SceneLoadStatus::Done);
}

namespace {

// A JSON value as the Luau value it reads as. Bounded in depth: a document a
// thousand arrays deep is not load data, it is a stack.
void pushJson(lua_State* L, core::JsonValue value, int depth)
{
    lua_checkstack(L, 4);
    switch (value.type()) {
    case core::JsonType::Boolean:
        lua_pushboolean(L, value.asBool() ? 1 : 0);
        return;
    case core::JsonType::Number:
        lua_pushnumber(L, value.asNumber());
        return;
    case core::JsonType::String: {
        const std::string_view text = value.asString();
        lua_pushlstring(L, text.data(), text.size());
        return;
    }
    case core::JsonType::Array: {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        if (depth >= 32)
            return;
        for (core::usize index = 0; index < value.size(); ++index) {
            pushJson(L, value.at(index), depth + 1);
            lua_rawseti(L, -2, static_cast<int>(index + 1));
        }
        return;
    }
    case core::JsonType::Object: {
        lua_createtable(L, 0, static_cast<int>(value.size()));
        if (depth >= 32)
            return;
        for (core::usize index = 0; index < value.size(); ++index) {
            const std::string_view key = value.keyAt(index);
            lua_pushlstring(L, key.data(), key.size());
            pushJson(L, value[key], depth + 1);
            lua_rawset(L, -3);
        }
        return;
    }
    case core::JsonType::Null:
        break;
    }
    lua_pushnil(L);
}

} // namespace

bool setSceneLoadData(lua_State* L, std::string_view json, std::string* diagnostic)
{
    core::JsonDocument document;
    const core::JsonDocument::ParseResult parsed = document.parse(json, "--scene-data");
    if (!parsed) {
        if (diagnostic != nullptr)
            *diagnostic = parsed.diagnostic;
        return false;
    }
    const int top = lua_gettop(L);
    pushJson(L, document.root(), 0);
    std::vector<core::u8> data;
    std::vector<core::InstanceId> refs;
    encodeRemoteArguments(L, top + 1, 1, data, refs);
    lua_settop(L, top);
    world(L).engineState().sceneLoadData = std::move(data);
    return true;
}

} // namespace engine::script
