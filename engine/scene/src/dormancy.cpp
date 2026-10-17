#include "engine/scene/dormancy.h"

#include <cmath>
#include <limits>

#include "engine/scene/world.h"

namespace engine::scene {

using core::f64;

namespace {

// Where a player looks from: the part named as their focus, else their
// character -- the same rule the authority sends them the world by.
[[nodiscard]] const PartComponent* focusPart(const World& world, const PlayerComponent& player) noexcept
{
    if (player.replicationFocus.valid() && world.alive(player.replicationFocus)) {
        if (const PartComponent* part = world.parts().find(player.replicationFocus); part != nullptr)
            return part;
    }
    if (player.character.valid() && world.alive(player.character))
        return world.parts().find(player.character);
    return nullptr;
}

} // namespace

void takePlayerFoci(World& world)
{
    EngineState& state = world.engineState();
    state.playerFoci.clear();
    state.focusless = false;
    // In the pool's own order, which is the operation sequence's (R10).
    world.players().forEach([&](core::InstanceId id, const PlayerComponent& player) {
        if (world.destroyed(id))
            return;
        if (const PartComponent* part = focusPart(world, player); part != nullptr)
            state.playerFoci.push_back(part->cframe.position);
        else
            state.focusless = true;
    });
}

f64 nearestPlayerDistance(const World& world, const core::DVec3& at) noexcept
{
    f64 nearest = std::numeric_limits<f64>::infinity();
    for (const core::DVec3& focus : world.engineState().playerFoci) {
        const f64 dx = focus.x - at.x;
        const f64 dy = focus.y - at.y;
        const f64 dz = focus.z - at.z;
        nearest = std::min(nearest, dx * dx + dy * dy + dz * dz);
    }
    return std::isinf(nearest) ? nearest : std::sqrt(nearest);
}

f64 dormantRadiusOf(const World& world) noexcept
{
    const EngineState& state = world.engineState();
    return state.dormantRadius < 0.0 ? 2.0 * state.streamingLoadRadius : state.dormantRadius;
}

bool dormancyOn(const World& world) noexcept
{
    const EngineState& state = world.engineState();
    return state.networkTopology == NetworkTopology::Dedicated && dormantRadiusOf(world) > 0.0 && !state.focusless;
}

bool dormantAt(const World& world, const core::DVec3& at) noexcept
{
    if (!dormancyOn(world))
        return false;
    const f64 radius = dormantRadiusOf(world);
    for (const core::DVec3& focus : world.engineState().playerFoci) {
        const f64 dx = focus.x - at.x;
        const f64 dy = focus.y - at.y;
        const f64 dz = focus.z - at.z;
        if (dx * dx + dy * dy + dz * dz <= radius * radius)
            return false;
    }
    return true;
}

} // namespace engine::scene
