#include "engine/script/travel.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/core/i18n.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
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

// **A player still here, or nothing**: one who left while the script
// waited -- a dungeon being made, a backend asked -- is gone, and so is their
// `Player`. That is an ordinary race, not the script's mistake, so it is not
// an error: they are not sent, and what is returned says who was. What is not
// a player at all still raises.
[[nodiscard]] std::optional<core::u32> presentUserId(lua_State* L, int index)
{
    const core::InstanceId* held = toInstance(L, index);
    if (held == nullptr)
        (void)checkInstance(L, index);
    scene::World& w = world(L);
    if (held == nullptr || !w.alive(*held) || w.destroyed(*held))
        return std::nullopt;
    const scene::PlayerComponent* component = w.players().find(*held);
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
    // Who is sent, for `TravelGroup` to return: the players still here, in
    // the order given.
    std::vector<int> sentAt;
    if (group) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const int count = lua_objlen(L, 2);
        for (int at = 1; at <= count; ++at) {
            lua_rawgeti(L, 2, at);
            if (const std::optional<core::u32> userId = presentUserId(L, lua_gettop(L)); userId.has_value()) {
                players.push_back(*userId);
                sentAt.push_back(at);
            }
            lua_pop(L, 1);
        }
    }
    else if (const std::optional<core::u32> userId = presentUserId(L, 2); userId.has_value()) {
        players.push_back(*userId);
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
    const bool any = !players.empty();
    if (any)
        state.travelRequests.push_back(
            scene::EngineState::TravelRequest{state.nextTravel++, std::move(players), where, std::move(data), 0, {}});
    if (!group) {
        lua_pushboolean(L, any ? 1 : 0);
        return 1;
    }
    lua_createtable(L, static_cast<int>(sentAt.size()), 0);
    for (std::size_t at = 0; at < sentAt.size(); ++at) {
        lua_rawgeti(L, 2, sentAt[at]);
        lua_rawseti(L, -2, static_cast<int>(at + 1));
    }
    return 1;
}

// The `[[world]]` this one is: `town` of `town:2`.
[[nodiscard]] std::string baseOf(const std::string& world)
{
    const std::size_t colon = world.rfind(':');
    return colon == std::string::npos ? world : world.substr(0, colon);
}

// A travel only a server under a manager may ask for.
void requireManagedServer(lua_State* L)
{
    const scene::EngineState& state = world(L).engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.travel_client"));
    if (state.managedWorld.empty())
        raise(L, ENG_TR("script.err.travel_no_manager"));
}

} // namespace

int networkServiceSwitchChannel(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireManagedServer(L);
    const std::optional<core::u32> userId = presentUserId(L, 2);
    const double channel = luaL_checknumber(L, 3);
    if (!(channel >= 1.0 && channel <= 64.0) || channel != std::floor(channel))
        raise(L, ENG_TR("script.err.channel_number"));
    if (!userId.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    scene::EngineState& state = world(L).engineState();
    scene::EngineState::TravelRequest request{state.nextTravel++, {*userId}, baseOf(state.managedWorld), {}, 0, {}};
    request.channel = static_cast<core::u32>(channel);
    state.travelRequests.push_back(std::move(request));
    lua_pushboolean(L, 1);
    return 1;
}

int networkServiceTravelToPlayer(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireManagedServer(L);
    const std::optional<core::u32> userId = presentUserId(L, 2);
    size_t length = 0;
    const char* key = luaL_checklstring(L, 3, &length);
    if (length == 0 || length > 128)
        raise(L, ENG_TR("script.err.travel_to_player_key"));
    if (!userId.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    scene::EngineState& state = world(L).engineState();
    scene::EngineState::TravelRequest request{state.nextTravel++, {*userId}, {}, {}, 0, {}};
    request.toPlayer.assign(key, length);
    state.travelRequests.push_back(std::move(request));
    lua_pushboolean(L, 1);
    return 1;
}

int networkServiceGetChannelsAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireManagedServer(L);
    requireYieldable(L, "GetChannelsAsync");
    scene::EngineState& state = world(L).engineState();
    const core::u32 id = state.nextChannelRequest++;
    state.channelRequests.push_back(id);
    lua_pushthread(L);
    services(L).channelWaiters.push_back(ServiceState::InstanceWaiter{id, lua_ref(L, -1)});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

namespace {

int signIn(lua_State* L, bool registering)
{
    (void)checkInstance(L, 1);
    size_t addressLength = 0;
    size_t nameLength = 0;
    size_t passwordLength = 0;
    const char* address = luaL_checklstring(L, 2, &addressLength);
    const char* name = luaL_checklstring(L, 3, &nameLength);
    const char* password = luaL_checklstring(L, 4, &passwordLength);
    requireYieldable(L, registering ? "RegisterAsync" : "SignInAsync");
    scene::EngineState& state = world(L).engineState();
    scene::EngineState::SignInAsk ask;
    ask.id = state.nextSignIn++;
    ask.address.assign(address, addressLength);
    ask.name.assign(name, nameLength);
    ask.password.assign(password, passwordLength);
    ask.registering = registering;
    lua_pushthread(L);
    services(L).signInWaiters.push_back(ServiceState::InstanceWaiter{ask.id, lua_ref(L, -1)});
    lua_pop(L, 1);
    state.signInAsks.push_back(std::move(ask));
    return lua_yield(L, 0);
}

} // namespace

int networkServiceSignInAsync(lua_State* L)
{
    return signIn(L, false);
}

int networkServiceRegisterAsync(lua_State* L)
{
    return signIn(L, true);
}

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
    // **Channels** (ADR 0230): the list a script waited for, as a table of
    // `{ Channel, Players, MaxPlayers }`; and `ChannelMerging`.
    for (const scene::EngineState::ChannelAnswer& answer : state.channelAnswers) {
        ServiceState& kept = services(L);
        const auto waiter =
            std::find_if(kept.channelWaiters.begin(), kept.channelWaiters.end(),
                         [&](const ServiceState::InstanceWaiter& waiting) { return waiting.id == answer.id; });
        if (waiter == kept.channelWaiters.end())
            continue;
        const int threadRef = waiter->threadRef;
        kept.channelWaiters.erase(waiter);
        lua_getref(L, threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            lua_createtable(co, static_cast<int>(answer.channels.size()), 0);
            for (std::size_t at = 0; at < answer.channels.size(); ++at) {
                lua_createtable(co, 0, 3);
                lua_pushnumber(co, answer.channels[at].channel);
                lua_setfield(co, -2, "Channel");
                lua_pushnumber(co, answer.channels[at].players);
                lua_setfield(co, -2, "Players");
                lua_pushnumber(co, answer.channels[at].maxPlayers);
                lua_setfield(co, -2, "MaxPlayers");
                lua_rawseti(co, -2, static_cast<int>(at + 1));
            }
            (void)resumeScheduled(L, co, 1);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
    }
    state.channelAnswers.clear();
    // **Signed in, or why not** (ADR 0237): `{ Ok, Ticket, Reason,
    // RetryAfter }`, the ticket nil and the reason an `Enum.SignInFailure`.
    for (const scene::EngineState::SignInAnswer& answer : state.signInAnswers) {
        ServiceState& kept = services(L);
        const auto waiter =
            std::find_if(kept.signInWaiters.begin(), kept.signInWaiters.end(),
                         [&](const ServiceState::InstanceWaiter& waiting) { return waiting.id == answer.id; });
        if (waiter == kept.signInWaiters.end())
            continue;
        const int threadRef = waiter->threadRef;
        kept.signInWaiters.erase(waiter);
        lua_getref(L, threadRef);
        if (lua_State* co = lua_tothread(L, -1); co != nullptr) {
            lua_createtable(co, 0, 4);
            lua_pushboolean(co, answer.failure == 0 ? 1 : 0);
            lua_setfield(co, -2, "Ok");
            if (answer.failure == 0) {
                lua_pushlstring(co, answer.ticket.data(), answer.ticket.size());
                lua_setfield(co, -2, "Ticket");
            }
            else {
                pushEnumItem(co, scene::EnumValue{static_cast<core::u16>(scene::generated::SignInFailureEnumId),
                                                  static_cast<core::i32>(answer.failure)});
                lua_setfield(co, -2, "Reason");
            }
            lua_pushnumber(co, static_cast<double>(answer.retryAfter));
            lua_setfield(co, -2, "RetryAfter");
            (void)resumeScheduled(L, co, 1);
        }
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
    }
    state.signInAnswers.clear();
    // **In line at the front door** (ADR 0232): `Queued(position,
    // estimateSeconds)`, the estimate nil until the line has moved.
    if (!state.queueUpdates.empty()) {
        const scene::ClassId networkClass = w.classes().findId(w.atoms().lookup("NetworkService"));
        const core::InstanceId service = scene::networkServiceOf(w, services(L).dataModel);
        const scene::EventDesc* queued =
            service.valid() ? w.classes().findEvent(networkClass, w.atoms().intern("Queued")) : nullptr;
        for (const scene::EngineState::QueueUpdate& update : state.queueUpdates) {
            if (queued == nullptr)
                break;
            const int top = lua_gettop(L);
            lua_pushnumber(L, update.position);
            if (update.estimate < 0)
                lua_pushnil(L);
            else
                lua_pushnumber(L, static_cast<double>(update.estimate));
            fireInstanceEvent(L, service, queued->slot, top + 1, 2);
            lua_settop(L, top);
        }
        state.queueUpdates.clear();
    }
    if (!state.channelMerging.empty()) {
        const scene::ClassId networkClass = w.classes().findId(w.atoms().lookup("NetworkService"));
        const core::InstanceId service = scene::networkServiceOf(w, services(L).dataModel);
        const scene::EventDesc* merging =
            service.valid() ? w.classes().findEvent(networkClass, w.atoms().intern("ChannelMerging")) : nullptr;
        for (const core::f64 seconds : state.channelMerging) {
            if (merging == nullptr)
                break;
            const int top = lua_gettop(L);
            lua_pushnumber(L, seconds);
            fireInstanceEvent(L, service, merging->slot, top + 1, 1);
            lua_settop(L, top);
        }
        state.channelMerging.clear();
    }
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
