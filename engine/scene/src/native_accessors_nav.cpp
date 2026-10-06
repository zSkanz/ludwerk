// `NavigationService`'s agent properties (ADR 0089). Each write refuses what
// no agent can be, here, where a refusal is a keyed error -- rather than in
// the mesh builder, where it would be a mesh of nothing.
#include <cmath>
#include <variant>

#include "class_descriptors.gen.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::scene::native {
namespace {

[[nodiscard]] bool takeF32(const Value& value, f32& out) noexcept
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !std::isfinite(*number))
        return false;
    out = static_cast<f32>(*number);
    return true;
}

// One getter and one setter per field, with the setter's own rule.
template <f32 NavigationComponent::*Field>
[[nodiscard]] Value getField(const World& world, core::InstanceId id)
{
    const NavigationComponent* c = world.navigation().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->*Field)};
}

template <f32 NavigationComponent::*Field, class Rule>
[[nodiscard]] bool setField(World& world, core::InstanceId id, const Value& value, Rule rule)
{
    NavigationComponent* c = world.navigation().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !rule(next))
        return false;
    c->*Field = next;
    return true;
}

} // namespace

void attachNavigationComponents(World& world, core::InstanceId id)
{
    world.navigation().add(id, NavigationComponent{});
}

void detachNavigationComponents(World& world, core::InstanceId id)
{
    world.navigation().remove(id);
}

Value getNavigationServiceAgentRadius(const World& world, core::InstanceId id)
{
    return getField<&NavigationComponent::agentRadius>(world, id);
}

bool setNavigationServiceAgentRadius(World& world, core::InstanceId id, const Value& value)
{
    return setField<&NavigationComponent::agentRadius>(world, id, value, [](f32 v) { return v > 0.0f; });
}

Value getNavigationServiceAgentHeight(const World& world, core::InstanceId id)
{
    return getField<&NavigationComponent::agentHeight>(world, id);
}

bool setNavigationServiceAgentHeight(World& world, core::InstanceId id, const Value& value)
{
    return setField<&NavigationComponent::agentHeight>(world, id, value, [](f32 v) { return v > 0.0f; });
}

Value getNavigationServiceAgentMaxClimb(const World& world, core::InstanceId id)
{
    return getField<&NavigationComponent::agentMaxClimb>(world, id);
}

bool setNavigationServiceAgentMaxClimb(World& world, core::InstanceId id, const Value& value)
{
    return setField<&NavigationComponent::agentMaxClimb>(world, id, value, [](f32 v) { return v >= 0.0f; });
}

Value getNavigationServiceAgentMaxSlope(const World& world, core::InstanceId id)
{
    return getField<&NavigationComponent::agentMaxSlope>(world, id);
}

bool setNavigationServiceAgentMaxSlope(World& world, core::InstanceId id, const Value& value)
{
    return setField<&NavigationComponent::agentMaxSlope>(world, id, value,
                                                         [](f32 v) { return v >= 0.0f && v <= 89.0f; });
}

// --- ADR 0098: areas, links and agents ------------------------------------------

namespace {

[[nodiscard]] Value vectorOf(const core::DVec3& at) noexcept
{
    return Value{core::Vec3{static_cast<f32>(at.x), static_cast<f32>(at.y), static_cast<f32>(at.z)}};
}

[[nodiscard]] bool takeVector(const Value& value, core::DVec3& out) noexcept
{
    const auto* vector = std::get_if<core::Vec3>(&value);
    if (vector == nullptr || !std::isfinite(vector->x) || !std::isfinite(vector->y) || !std::isfinite(vector->z))
        return false;
    out = core::toDVec3(*vector);
    return true;
}

template <class Component, class Field>
[[nodiscard]] Value readString(const World& world, const ComponentPool<Component>& pool, core::InstanceId id,
                               Field field)
{
    (void)world;
    const Component* c = pool.find(id);
    return c == nullptr ? Value{} : Value{c->*field};
}

} // namespace

void attachNavigationAreaComponents(World& world, core::InstanceId id)
{
    world.navigationAreas().add(id, NavigationAreaComponent{});
}

void detachNavigationAreaComponents(World& world, core::InstanceId id)
{
    world.navigationAreas().remove(id);
}

Value getNavigationAreaLabel(const World& world, core::InstanceId id)
{
    return readString(world, world.navigationAreas(), id, &NavigationAreaComponent::label);
}

bool setNavigationAreaLabel(World& world, core::InstanceId id, const Value& value)
{
    NavigationAreaComponent* c = world.navigationAreas().find(id);
    const auto* text = std::get_if<std::string>(&value);
    if (c == nullptr || text == nullptr)
        return false;
    c->label = *text;
    return true;
}

void attachNavigationLinkComponents(World& world, core::InstanceId id)
{
    world.navigationLinks().add(id, NavigationLinkComponent{});
}

void detachNavigationLinkComponents(World& world, core::InstanceId id)
{
    world.navigationLinks().remove(id);
}

Value getNavigationLinkFrom(const World& world, core::InstanceId id)
{
    const NavigationLinkComponent* c = world.navigationLinks().find(id);
    return c == nullptr ? Value{} : vectorOf(c->from);
}

bool setNavigationLinkFrom(World& world, core::InstanceId id, const Value& value)
{
    NavigationLinkComponent* c = world.navigationLinks().find(id);
    return c != nullptr && takeVector(value, c->from);
}

Value getNavigationLinkTo(const World& world, core::InstanceId id)
{
    const NavigationLinkComponent* c = world.navigationLinks().find(id);
    return c == nullptr ? Value{} : vectorOf(c->to);
}

bool setNavigationLinkTo(World& world, core::InstanceId id, const Value& value)
{
    NavigationLinkComponent* c = world.navigationLinks().find(id);
    return c != nullptr && takeVector(value, c->to);
}

Value getNavigationLinkBidirectional(const World& world, core::InstanceId id)
{
    const NavigationLinkComponent* c = world.navigationLinks().find(id);
    return c == nullptr ? Value{} : Value{c->bidirectional};
}

bool setNavigationLinkBidirectional(World& world, core::InstanceId id, const Value& value)
{
    NavigationLinkComponent* c = world.navigationLinks().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->bidirectional = *flag;
    return true;
}

Value getNavigationLinkLabel(const World& world, core::InstanceId id)
{
    return readString(world, world.navigationLinks(), id, &NavigationLinkComponent::label);
}

bool setNavigationLinkLabel(World& world, core::InstanceId id, const Value& value)
{
    NavigationLinkComponent* c = world.navigationLinks().find(id);
    const auto* text = std::get_if<std::string>(&value);
    if (c == nullptr || text == nullptr)
        return false;
    c->label = *text;
    return true;
}

void attachNavigationAgentComponents(World& world, core::InstanceId id)
{
    world.navigationAgents().add(id, NavigationAgentComponent{});
}

void detachNavigationAgentComponents(World& world, core::InstanceId id)
{
    world.navigationAgents().remove(id);
}

Value getNavigationAgentTarget(const World& world, core::InstanceId id)
{
    const NavigationAgentComponent* c = world.navigationAgents().find(id);
    return c == nullptr ? Value{} : vectorOf(c->target);
}

// **Writing the target starts the walk**, which is what somebody writing it
// means; `Active` is how to stop without choosing a new one.
bool setNavigationAgentTarget(World& world, core::InstanceId id, const Value& value)
{
    NavigationAgentComponent* c = world.navigationAgents().find(id);
    if (c == nullptr || !takeVector(value, c->target))
        return false;
    c->active = true;
    return true;
}

Value getNavigationAgentActive(const World& world, core::InstanceId id)
{
    const NavigationAgentComponent* c = world.navigationAgents().find(id);
    return c == nullptr ? Value{} : Value{c->active};
}

bool setNavigationAgentActive(World& world, core::InstanceId id, const Value& value)
{
    NavigationAgentComponent* c = world.navigationAgents().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->active = *flag;
    return true;
}

Value getNavigationAgentMaxSpeed(const World& world, core::InstanceId id)
{
    const NavigationAgentComponent* c = world.navigationAgents().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxSpeed)};
}

bool setNavigationAgentMaxSpeed(World& world, core::InstanceId id, const Value& value)
{
    NavigationAgentComponent* c = world.navigationAgents().find(id);
    f32 speed = 0.0f;
    if (c == nullptr || !takeF32(value, speed) || speed <= 0.0f)
        return false;
    c->maxSpeed = speed;
    return true;
}

Value getNavigationAgentAgentType(const World& world, core::InstanceId id)
{
    return readString(world, world.navigationAgents(), id, &NavigationAgentComponent::agentType);
}

bool setNavigationAgentAgentType(World& world, core::InstanceId id, const Value& value)
{
    NavigationAgentComponent* c = world.navigationAgents().find(id);
    const auto* text = std::get_if<std::string>(&value);
    if (c == nullptr || text == nullptr)
        return false;
    c->agentType = *text;
    return true;
}

// --- Swarm (ADR 0156) -----------------------------------------------------------

void attachSwarmComponents(World& world, core::InstanceId id)
{
    world.swarms().add(id, SwarmComponent{});
}

void detachSwarmComponents(World& world, core::InstanceId id)
{
    world.swarms().remove(id);
}

Value getSwarmTarget(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : vectorOf(c->target);
}

bool setSwarmTarget(World& world, core::InstanceId id, const Value& value)
{
    SwarmComponent* c = world.swarms().find(id);
    return c != nullptr && takeVector(value, c->target);
}

Value getSwarmReplicates(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : Value{c->replicates};
}

bool setSwarmReplicates(World& world, core::InstanceId id, const Value& value)
{
    SwarmComponent* c = world.swarms().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    // The authority's (ADR 0162): on a replica it is what arrived.
    if (!c->mirrored)
        c->replicates = *flag;
    return true;
}

Value getSwarmReplicationRadius(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->replicationRadius)};
}

bool setSwarmReplicationRadius(World& world, core::InstanceId id, const Value& value)
{
    SwarmComponent* c = world.swarms().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (c == nullptr || number == nullptr || !std::isfinite(*number) || !(*number > 0.0))
        return false;
    if (!c->mirrored)
        c->replicationRadius = static_cast<f32>(*number);
    return true;
}

Value getSwarmEnabled(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : Value{c->enabled};
}

bool setSwarmEnabled(World& world, core::InstanceId id, const Value& value)
{
    SwarmComponent* c = world.swarms().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->enabled = *flag;
    return true;
}

namespace {

// A number field of a swarm, and the rule its setter keeps.
template <f32 SwarmComponent::*Field>
[[nodiscard]] Value getSwarmNumber(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->*Field)};
}

template <f32 SwarmComponent::*Field>
[[nodiscard]] bool setSwarmNumber(World& world, core::InstanceId id, const Value& value, bool aboveZero)
{
    SwarmComponent* c = world.swarms().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || next < 0.0f || (aboveZero && next <= 0.0f))
        return false;
    c->*Field = next;
    return true;
}

} // namespace

Value getSwarmCellSize(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::cellSize>(world, id);
}

bool setSwarmCellSize(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::cellSize>(world, id, value, true);
}

Value getSwarmMaxNeighbours(const World& world, core::InstanceId id)
{
    const SwarmComponent* c = world.swarms().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxNeighbours)};
}

bool setSwarmMaxNeighbours(World& world, core::InstanceId id, const Value& value)
{
    SwarmComponent* c = world.swarms().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || next < 0.0f || next > 1024.0f)
        return false;
    c->maxNeighbours = static_cast<u32>(next);
    return true;
}

Value getSwarmGravity(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::gravity>(world, id);
}

bool setSwarmGravity(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::gravity>(world, id, value, false);
}

Value getSwarmClimbSpeed(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::climbSpeed>(world, id);
}

bool setSwarmClimbSpeed(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::climbSpeed>(world, id, value, false);
}

Value getSwarmPileHeight(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::pileHeight>(world, id);
}

bool setSwarmPileHeight(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::pileHeight>(world, id, value, false);
}

Value getSwarmStopDistance(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::stopDistance>(world, id);
}

bool setSwarmStopDistance(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::stopDistance>(world, id, value, false);
}

Value getSwarmNearDistance(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::nearDistance>(world, id);
}

bool setSwarmNearDistance(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::nearDistance>(world, id, value, false);
}

Value getSwarmFarDistance(const World& world, core::InstanceId id)
{
    return getSwarmNumber<&SwarmComponent::farDistance>(world, id);
}

bool setSwarmFarDistance(World& world, core::InstanceId id, const Value& value)
{
    return setSwarmNumber<&SwarmComponent::farDistance>(world, id, value, false);
}

} // namespace engine::scene::native
