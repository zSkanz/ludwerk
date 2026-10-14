#pragma once
#include <span>
#include <string_view>

#include "engine/script/instance_binding.h"
struct lua_State;
namespace engine::platform {
class GameIntegration;
}
namespace engine::script {
enum class IntegrationResponse
{
    Action,
    Number,
    String,
    Boolean,
    Table,
    Product,
    LeaderboardEntries,
    UserList,
    Presence
};
// Shared native session and scheduler for specific and generic services.
[[nodiscard]] platform::GameIntegration* integrationProvider(lua_State* L, std::string_view id);
int beginIntegrationAction(lua_State* L, std::string_view id, std::string_view operation, std::string_view argument,
                           IntegrationResponse response = IntegrationResponse::Action, bool generic = false);
void warnOptionalIntegration(lua_State* L, std::string_view integration, std::string_view service);
void resumeIntegrationWaiters(lua_State* L);
[[nodiscard]] std::span<const InstanceMethodBinding> optionalServiceMethods();
} // namespace engine::script
