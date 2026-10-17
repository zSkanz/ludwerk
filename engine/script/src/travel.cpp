#include "engine/script/travel.h"

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
#include "engine/script/save_service.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// What a travel may carry, written (ADR 0222 §1).
constexpr core::usize MaxTravelData = 16 * 1024;
// How many ticks an arrival waits for its player to be in the world.
constexpr core::u32 ArrivalTicks = 600;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

[[nodiscard]] core::u32 userIdOf(lua_State* L, int index)
{
    const core::InstanceId player = checkInstance(L, index);
    const scene::PlayerComponent* component = world(L).players().find(player);
    if (component == nullptr)
        raise(L, ENG_TR("script.err.travel_not_a_player"));
    return component->userId;
}

int travel(lua_State* L, bool group)
{
    (void)checkInstance(L, 1);
    scene::EngineState& state = world(L).engineState();
    // **The server's, and under a manager**: a world on its own has nowhere
    // to send anybody (ADR 0222 §1).
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.travel_client"));
    if (state.managedWorld.empty())
        raise(L, ENG_TR("script.err.travel_no_manager"));
    std::vector<core::u32> players;
    if (group) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const int count = lua_objlen(L, 2);
        for (int at = 1; at <= count; ++at) {
            lua_rawgeti(L, 2, at);
            players.push_back(userIdOf(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
    }
    else {
        players.push_back(userIdOf(L, 2));
    }
    size_t length = 0;
    const char* target = luaL_checklstring(L, 3, &length);
    const std::string where(target, length);
    // A world's name the manager has, or an instance's id (they begin "i-").
    if (!where.starts_with("i-") &&
        std::find(state.managerWorlds.begin(), state.managerWorlds.end(), where) == state.managerWorlds.end()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"target", std::string_view{where}}};
        raise(L, ENG_TR("script.err.travel_unknown_world"), args);
    }
    std::string data;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4))
        data = encodeSavedValue(L, 4, MaxTravelData);
    if (players.empty())
        return 0;
    state.travelRequests.push_back(
        scene::EngineState::TravelRequest{state.nextTravel++, std::move(players), where, std::move(data)});
    return 0;
}

} // namespace

int networkServiceTravel(lua_State* L)
{
    return travel(L, false);
}

int networkServiceTravelGroup(lua_State* L)
{
    return travel(L, true);
}

void fireTravelEvents(lua_State* L)
{
    scene::World& w = world(L);
    scene::EngineState& state = w.engineState();
    // A client: sent elsewhere.
    std::vector<std::string> started;
    started.swap(state.travelStarted);
    for (const std::string& address : started)
        fireNetworkEvent(L, "TravelStarted", std::string_view{address});
    if (state.travelArrivals.empty())
        return;
    // A server: who arrived, once their player is in the world.
    const scene::ClassId networkClass = w.classes().findId(w.atoms().lookup("NetworkService"));
    const core::InstanceId network = scene::networkServiceOf(w, services(L).dataModel);
    const scene::EventDesc* arrived =
        network.valid() ? w.classes().findEvent(networkClass, w.atoms().intern("TravelArrived")) : nullptr;
    std::vector<scene::EngineState::TravelArrival> waiting;
    for (scene::EngineState::TravelArrival& arrival : state.travelArrivals) {
        core::InstanceId player;
        for (core::InstanceId child = network.valid() ? w.firstChild(network) : core::InstanceId{}; child.valid();
             child = w.nextSibling(child)) {
            const scene::PlayerComponent* component = w.players().find(child);
            if (component != nullptr && component->publicKey == arrival.publicKey && !w.destroyed(child))
                player = child;
        }
        if (!player.valid()) {
            if (++arrival.waited < ArrivalTicks)
                waiting.push_back(std::move(arrival));
            continue;
        }
        if (arrived == nullptr)
            continue;
        const int top = lua_gettop(L);
        pushInstance(L, player);
        if (arrival.data.empty())
            lua_pushnil(L);
        else
            pushSavedValue(L, arrival.data);
        lua_pushlstring(L, arrival.origin.data(), arrival.origin.size());
        lua_pushlstring(L, arrival.reason.data(), arrival.reason.size());
        fireInstanceEvent(L, network, arrived->slot, top + 1, 4);
        lua_settop(L, top);
    }
    state.travelArrivals = std::move(waiting);
}

} // namespace engine::script
