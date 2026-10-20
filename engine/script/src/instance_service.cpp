#include "engine/script/instance_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/remote.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// **Under a manager, on the server** (ADR 0221 §1).
void requireManaged(lua_State* L)
{
    const scene::EngineState& state = world(L).engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.instance_client"));
    if (state.managedWorld.empty())
        raise(L, ENG_TR("script.err.instance_no_manager"));
}

void requireInstance(lua_State* L)
{
    requireManaged(L);
    if (!world(L).engineState().managedWorld.starts_with("i-"))
        raise(L, ENG_TR("script.err.instance_not_one"));
}

[[nodiscard]] std::string checkKey(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return std::string(text, length);
}

void control(lua_State* L, scene::EngineState::InstanceControl::Kind kind, std::string key, core::f64 seconds)
{
    world(L).engineState().instanceControls.push_back(
        scene::EngineState::InstanceControl{kind, std::move(key), seconds});
}

} // namespace

int instanceServiceCreateInstanceAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireManaged(L);
    const std::string templateName = checkKey(L, 2);
    requireYieldable(L, "CreateInstanceAsync");
    scene::World& w = world(L);
    scene::EngineState::InstanceRequest request;
    request.templateName = templateName;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "Players");
        if (lua_istable(L, -1)) {
            const int count = lua_objlen(L, -1);
            for (int at = 1; at <= count; ++at) {
                lua_rawgeti(L, -1, at);
                const scene::PlayerComponent* player = w.players().find(checkInstance(L, lua_gettop(L)));
                if (player == nullptr || player->publicKey.empty())
                    raise(L, ENG_TR("script.err.instance_player"));
                request.keys.push_back(player->publicKey);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "Party");
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t length = 0;
            const char* party = lua_tolstring(L, -1, &length);
            request.party.assign(party, length);
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "Data");
        if (!lua_isnil(L, -1)) {
            std::vector<core::InstanceId> refs;
            encodeRemoteArguments(L, lua_gettop(L), 1, request.data, refs);
            // Another process cannot be handed this one's instances.
            if (!refs.empty())
                raise(L, ENG_TR("script.err.instance_data_refs"));
        }
        lua_pop(L, 1);
    }
    scene::EngineState& state = w.engineState();
    request.id = state.nextInstanceRequest++;
    const core::u32 id = request.id;
    state.instanceRequests.push_back(std::move(request));
    lua_pushthread(L);
    services(L).instanceWaiters.push_back(ServiceState::InstanceWaiter{id, lua_ref(L, -1)});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

int instanceServiceAllowPlayer(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireInstance(L);
    control(L, scene::EngineState::InstanceControl::Kind::Allow, checkKey(L, 2), 0.0);
    return 0;
}

int instanceServiceDisallowPlayer(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireInstance(L);
    control(L, scene::EngineState::InstanceControl::Kind::Disallow, checkKey(L, 2), 0.0);
    return 0;
}

int instanceServiceClose(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireInstance(L);
    control(L, scene::EngineState::InstanceControl::Kind::Close, {}, 0.0);
    return 0;
}

int instanceServiceKeepOpen(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireInstance(L);
    const core::f64 seconds = luaL_checknumber(L, 2);
    if (!(seconds >= 0.0) || seconds > 86400.0 * 7.0)
        raise(L, ENG_TR("script.err.instance_keep_open"));
    control(L, scene::EngineState::InstanceControl::Kind::KeepOpen, {}, seconds);
    return 0;
}

void runInstanceEvents(lua_State* L)
{
    ServiceState& state = services(L);
    scene::World& w = world(L);
    scene::EngineState& engine = w.engineState();
    // Answers to `CreateInstanceAsync`.
    for (auto answer = engine.instanceAnswers.begin(); answer != engine.instanceAnswers.end();) {
        const auto waiter = std::find_if(state.instanceWaiters.begin(), state.instanceWaiters.end(),
                                         [&](const ServiceState::InstanceWaiter& w) { return w.id == answer->id; });
        if (waiter == state.instanceWaiters.end()) {
            answer = engine.instanceAnswers.erase(answer);
            continue;
        }
        const int threadRef = waiter->threadRef;
        state.instanceWaiters.erase(waiter);
        lua_getref(L, threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            if (answer->error.empty()) {
                lua_pushlstring(co, answer->instanceId.data(), answer->instanceId.size());
                (void)resumeScheduled(L, co, 1);
            }
            else {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"why", std::string_view{answer->error}}};
                const std::string said = core::formatKeyPrefixed(ENG_TR("script.err.instance_failed"), args);
                lua_pushlstring(co, said.data(), said.size());
                (void)resumeScheduledWithError(L, co);
            }
        }
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
        answer = engine.instanceAnswers.erase(answer);
    }
    // **An instance this world made, gone**: for rewards and cleanup.
    if (!engine.instancesClosed.empty()) {
        std::vector<scene::EngineState::InstanceClosed> closed;
        closed.swap(engine.instancesClosed);
        const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("InstanceService"));
        const core::InstanceId service = w.findFirstChildOfClass(state.dataModel, serviceClass);
        const scene::EventDesc* event =
            service.valid() ? w.classes().findEvent(serviceClass, w.atoms().intern("InstanceClosed")) : nullptr;
        for (const scene::EngineState::InstanceClosed& gone : closed) {
            if (event == nullptr)
                break;
            const int top = lua_gettop(L);
            lua_pushlstring(L, gone.id.data(), gone.id.size());
            lua_pushlstring(L, gone.reason.data(), gone.reason.size());
            fireInstanceEvent(L, service, event->slot, top + 1, 2);
            lua_settop(L, top);
        }
    }
    // **`Closing`, once**, for the instance's script to save what it keeps.
    if (engine.instanceClosing && !engine.instanceClosingFired) {
        engine.instanceClosingFired = true;
        const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("InstanceService"));
        const core::InstanceId service = w.findFirstChildOfClass(state.dataModel, serviceClass);
        const scene::EventDesc* closing =
            service.valid() ? w.classes().findEvent(serviceClass, w.atoms().intern("Closing")) : nullptr;
        if (closing != nullptr)
            fireInstanceEvent(L, service, closing->slot, lua_gettop(L) + 1, 0);
    }
}

} // namespace engine::script
