#include "engine/script/cross_world.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/save_service.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// ADR 0223 §1's bounds.
constexpr std::size_t MaxTopicBytes = 80;
constexpr core::usize MaxMessageBytes = 4 * 1024;
constexpr core::u32 MostPerSecond = 200;
constexpr std::size_t MostTopics = 1000;
// A second of ticks, for the rate.
constexpr core::u64 TicksPerSecond = 60;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

void requireServer(lua_State* L)
{
    if (world(L).engineState().networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.cross_world_client"));
}

[[nodiscard]] std::string checkTopic(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    if (length == 0 || length > MaxTopicBytes)
        raise(L, ENG_TR("script.err.cross_world_topic"));
    return std::string(text, length);
}

} // namespace

int crossWorldServicePublish(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServer(L);
    std::string topic = checkTopic(L, 2);
    luaL_checkany(L, 3);
    std::string message = encodeSavedValue(L, 3, MaxMessageBytes);
    ServiceState& state = services(L);
    scene::EngineState& engine = world(L).engineState();
    // **Bounded**: so many a second of the world's own ticks.
    const core::u64 second = engine.tick / TicksPerSecond;
    if (second != state.crossWorldSecond) {
        state.crossWorldSecond = second;
        state.crossWorldThisSecond = 0;
    }
    if (++state.crossWorldThisSecond > MostPerSecond)
        raise(L, ENG_TR("script.err.cross_world_rate"));
    // **A world on its own hears its own**; under a manager, the manager
    // delivers it to every world subscribed, this one included.
    if (engine.managedWorld.empty())
        engine.crossWorldIn.push_back(scene::EngineState::CrossWorldMessage{std::move(topic), std::move(message), {}});
    else
        engine.crossWorldOut.push_back(scene::EngineState::CrossWorldMessage{std::move(topic), std::move(message), {}});
    return 0;
}

int crossWorldServiceSubscribe(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServer(L);
    std::string topic = checkTopic(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    ServiceState& state = services(L);
    const bool known = std::any_of(state.crossWorldHandlers.begin(), state.crossWorldHandlers.end(),
                                   [&](const ServiceState::CrossWorldHandler& h) { return h.topic == topic; });
    if (!known) {
        std::vector<std::string> topics;
        for (const ServiceState::CrossWorldHandler& handler : state.crossWorldHandlers) {
            if (std::find(topics.begin(), topics.end(), handler.topic) == topics.end())
                topics.push_back(handler.topic);
        }
        if (topics.size() >= MostTopics)
            raise(L, ENG_TR("script.err.cross_world_topics"));
        world(L).engineState().crossWorldTopics.emplace_back(topic, true);
    }
    lua_pushvalue(L, 3);
    state.crossWorldHandlers.push_back(ServiceState::CrossWorldHandler{std::move(topic), lua_ref(L, -1)});
    lua_pop(L, 1);
    return 0;
}

int crossWorldServiceUnsubscribe(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireServer(L);
    const std::string topic = checkTopic(L, 2);
    ServiceState& state = services(L);
    bool had = false;
    for (auto at = state.crossWorldHandlers.begin(); at != state.crossWorldHandlers.end();) {
        if (at->topic == topic) {
            (void)lua_unref(L, at->functionRef);
            at = state.crossWorldHandlers.erase(at);
            had = true;
        }
        else {
            ++at;
        }
    }
    if (had)
        world(L).engineState().crossWorldTopics.emplace_back(topic, false);
    return 0;
}

void fireCrossWorldMessages(lua_State* L)
{
    scene::EngineState& engine = world(L).engineState();
    if (engine.crossWorldIn.empty())
        return;
    std::deque<scene::EngineState::CrossWorldMessage> arrived;
    arrived.swap(engine.crossWorldIn);
    ServiceState& state = services(L);
    for (const scene::EngineState::CrossWorldMessage& message : arrived) {
        // Collected first: a callback may subscribe or unsubscribe.
        std::vector<int> refs;
        for (const ServiceState::CrossWorldHandler& handler : state.crossWorldHandlers) {
            if (handler.topic == message.topic)
                refs.push_back(handler.functionRef);
        }
        for (const int ref : refs) {
            lua_State* co = lua_newthread(L);
            const int threadRef = lua_ref(L, -1);
            lua_pop(L, 1);
            lua_getref(L, ref);
            lua_xmove(L, co, 1);
            pushSavedValue(co, message.message);
            lua_pushlstring(co, message.origin.data(), message.origin.size());
            (void)startScheduled(L, co, 2);
            (void)lua_unref(L, threadRef);
        }
    }
}

} // namespace engine::script
