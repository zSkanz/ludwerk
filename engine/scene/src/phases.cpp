#include "engine/scene/phases.h"

#include <algorithm>

#include "engine/scene/world.h"

namespace engine::scene {

std::optional<core::u32> phaseBitOf(const World& world, core::NameAtom phase) noexcept
{
    const std::vector<core::NameAtom>& names = world.engineState().phaseNames;
    const auto found = std::find(names.begin(), names.end(), phase);
    if (found == names.end())
        return std::nullopt;
    return static_cast<core::u32>(found - names.begin());
}

std::optional<core::u32> phaseBit(World& world, core::NameAtom phase)
{
    if (const std::optional<core::u32> bit = phaseBitOf(world, phase); bit.has_value())
        return bit;
    std::vector<core::NameAtom>& names = world.engineState().phaseNames;
    if (names.size() >= MostPhases)
        return std::nullopt;
    names.push_back(phase);
    return static_cast<core::u32>(names.size() - 1);
}

bool anyPhased(const World& world) noexcept
{
    return world.phases().size() > 0;
}

core::u64 phaseMask(const World& world, const PlayerComponent& player) noexcept
{
    core::u64 mask = 0;
    for (const core::NameAtom phase : player.phases) {
        if (const std::optional<core::u32> bit = phaseBitOf(world, phase); bit.has_value())
            mask |= core::u64{1} << *bit;
    }
    return mask;
}

bool playerSees(const World& world, const PlayerComponent& player, core::InstanceId id) noexcept
{
    // Up the tree: a phase anywhere above it that the player is not in hides
    // it with what holds it.
    for (core::InstanceId at = id; at.valid(); at = world.parentOf(at)) {
        const PhaseComponent* phased = world.phases().find(at);
        if (phased == nullptr || !phased->phase.valid())
            continue;
        if (std::find(player.phases.begin(), player.phases.end(), phased->phase) == player.phases.end())
            return false;
    }
    return true;
}

core::NameAtom collidingPhase(const World& world, core::InstanceId id) noexcept
{
    for (core::InstanceId at = id; at.valid(); at = world.parentOf(at)) {
        const PhaseComponent* phased = world.phases().find(at);
        if (phased == nullptr || !phased->phase.valid())
            continue;
        // `PhaseCollision` is the part's own word, whichever phase it has.
        const PhaseComponent* own = world.phases().find(id);
        if (own != nullptr && !own->collision)
            return core::NameAtom{};
        return phased->phase;
    }
    return core::NameAtom{};
}

} // namespace engine::scene
