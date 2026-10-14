// The native side of `VehicleController` and `VehicleWheel` (ADR 0213): what
// the instances say, read and written within the ranges their errors name. What
// the vehicle did is written here by the physics, after each step, and read.

#include <cmath>

#include "class_descriptors.gen.h"
#include "engine/scene/world.h"

namespace engine::scene::native {
namespace {

template <class Component>
[[nodiscard]] Value numberOf(const Component* component, f32 Component::*field)
{
    return component == nullptr ? Value{} : Value{static_cast<f64>(component->*field)};
}

template <class Component>
[[nodiscard]] bool putNumber(Component* component, f32 Component::*field, const Value& value, f64 least, f64 most)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || component == nullptr || !std::isfinite(*number) || *number < least || *number > most)
        return false;
    component->*field = static_cast<f32>(*number);
    return true;
}

// An instance of the kind `kind` asks for, or nil.
template <class Kind>
[[nodiscard]] bool putReference(const World& world, const Value& value, Kind kind, core::InstanceId& field)
{
    if (const auto* reference = std::get_if<core::InstanceId>(&value); reference != nullptr) {
        if (!world.alive(*reference) || !kind(*reference))
            return false;
        field = *reference;
        return true;
    }
    if (valueType(value) != ValueType::Nil)
        return false;
    field = core::InstanceId{};
    return true;
}

[[nodiscard]] Value referenceOf(const World& world, core::InstanceId field)
{
    return field.valid() && world.alive(field) ? Value{field} : Value{};
}

} // namespace

void attachVehicleControllerComponents(World& world, core::InstanceId id)
{
    VehicleControllerComponent made;
    made.driveIntent = world.atoms().intern("Drive");
    made.brakeIntent = world.atoms().intern("Brake");
    made.handBrakeIntent = world.atoms().intern("HandBrake");
    world.vehicleControllers().add(id, made);
}

void detachVehicleControllerComponents(World& world, core::InstanceId id)
{
    world.vehicleControllers().remove(id);
}

void attachVehicleWheelComponents(World& world, core::InstanceId id)
{
    world.vehicleWheels().add(id, VehicleWheelComponent{});
}

void detachVehicleWheelComponents(World& world, core::InstanceId id)
{
    world.vehicleWheels().remove(id);
}

Value getVehicleControllerMaxTorque(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::maxTorque);
}

bool setVehicleControllerMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::maxTorque, value, 0.0, 1.0e9);
}

Value getVehicleControllerMinRPM(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::minRpm);
}

bool setVehicleControllerMinRPM(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::minRpm, value, 0.0, 1.0e9);
}

Value getVehicleControllerMaxRPM(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::maxRpm);
}

bool setVehicleControllerMaxRPM(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::maxRpm, value, 1.0e-6, 1.0e9);
}

Value getVehicleControllerShiftUpRPM(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::shiftUpRpm);
}

bool setVehicleControllerShiftUpRPM(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::shiftUpRpm, value, 1.0e-6,
                     1.0e9);
}

Value getVehicleControllerShiftDownRPM(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::shiftDownRpm);
}

bool setVehicleControllerShiftDownRPM(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::shiftDownRpm, value, 1.0e-6,
                     1.0e9);
}

Value getVehicleControllerFinalDriveRatio(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::finalDriveRatio);
}

bool setVehicleControllerFinalDriveRatio(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::finalDriveRatio, value, 1.0e-6,
                     1.0e9);
}

Value getVehicleControllerMaxLeanAngle(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::maxLeanAngle);
}

bool setVehicleControllerMaxLeanAngle(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::maxLeanAngle, value, 0.0, 1.0e9);
}

Value getVehicleControllerThrottle(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::throttle);
}

bool setVehicleControllerThrottle(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::throttle, value, -1.0, 1.0);
}

Value getVehicleControllerSteer(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::steer);
}

bool setVehicleControllerSteer(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::steer, value, -1.0, 1.0);
}

Value getVehicleControllerBrake(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::brake);
}

bool setVehicleControllerBrake(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::brake, value, 0.0, 1.0);
}

Value getVehicleControllerHandBrake(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::handBrake);
}

bool setVehicleControllerHandBrake(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleControllers().find(id), &VehicleControllerComponent::handBrake, value, 0.0, 1.0);
}

Value getVehicleControllerTransmission(const World& world, core::InstanceId id)
{
    const VehicleControllerComponent* c = world.vehicleControllers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::VehicleTransmissionEnumId, c->transmission}};
}

bool setVehicleControllerTransmission(World& world, core::InstanceId id, const Value& value)
{
    VehicleControllerComponent* c = world.vehicleControllers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::VehicleTransmissionEnumId || item->value < 0 ||
        item->value > 1)
        return false;
    c->transmission = item->value;
    return true;
}

// A whole gear: -1 reverse, 0 neutral, then the forward gears.
Value getVehicleControllerGear(const World& world, core::InstanceId id)
{
    const VehicleControllerComponent* c = world.vehicleControllers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->gear)};
}

bool setVehicleControllerGear(World& world, core::InstanceId id, const Value& value)
{
    VehicleControllerComponent* c = world.vehicleControllers().find(id);
    const auto* number = std::get_if<f64>(&value);
    if (c == nullptr || number == nullptr || !std::isfinite(*number) || *number != std::floor(*number) ||
        *number < -1.0 || *number > 16.0)
        return false;
    c->gear = static_cast<i32>(*number);
    return true;
}

Value getVehicleControllerSpeed(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::speed);
}

Value getVehicleControllerRPM(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleControllers().find(id), &VehicleControllerComponent::rpm);
}

Value getVehicleControllerCurrentGear(const World& world, core::InstanceId id)
{
    const VehicleControllerComponent* c = world.vehicleControllers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->currentGear)};
}

Value getVehicleWheelRadius(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::radius);
}

bool setVehicleWheelRadius(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::radius, value, 1.0e-6, 1.0e9);
}

Value getVehicleWheelWidth(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::width);
}

bool setVehicleWheelWidth(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::width, value, 1.0e-6, 1.0e9);
}

Value getVehicleWheelSuspensionMinLength(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionMinLength);
}

bool setVehicleWheelSuspensionMinLength(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionMinLength, value, 0.0, 1.0e9);
}

Value getVehicleWheelSuspensionMaxLength(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionMaxLength);
}

bool setVehicleWheelSuspensionMaxLength(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionMaxLength, value, 0.0, 1.0e9);
}

Value getVehicleWheelSuspensionFrequency(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionFrequency);
}

bool setVehicleWheelSuspensionFrequency(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionFrequency, value, 1.0e-6, 1.0e9);
}

Value getVehicleWheelSuspensionDamping(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionDamping);
}

bool setVehicleWheelSuspensionDamping(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::suspensionDamping, value, 0.0, 1.0);
}

Value getVehicleWheelMaxSteerAngle(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::maxSteerAngle);
}

bool setVehicleWheelMaxSteerAngle(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::maxSteerAngle, value, 0.0, 1.0e9);
}

Value getVehicleWheelMaxBrakeTorque(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::maxBrakeTorque);
}

bool setVehicleWheelMaxBrakeTorque(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::maxBrakeTorque, value, 0.0, 1.0e9);
}

Value getVehicleWheelMaxHandBrakeTorque(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::maxHandBrakeTorque);
}

bool setVehicleWheelMaxHandBrakeTorque(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::maxHandBrakeTorque, value, 0.0, 1.0e9);
}

Value getVehicleWheelGrip(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::grip);
}

bool setVehicleWheelGrip(World& world, core::InstanceId id, const Value& value)
{
    return putNumber(world.vehicleWheels().find(id), &VehicleWheelComponent::grip, value, 0.0, 1.0e9);
}

Value getVehicleWheelAttachment(const World& world, core::InstanceId id)
{
    const VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w == nullptr ? Value{} : referenceOf(world, w->attachment);
}

bool setVehicleWheelAttachment(World& world, core::InstanceId id, const Value& value)
{
    VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w != nullptr &&
           putReference(
               world, value, [&](core::InstanceId other) { return world.attachments().find(other) != nullptr; },
               w->attachment);
}

Value getVehicleWheelWheelPart(const World& world, core::InstanceId id)
{
    const VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w == nullptr ? Value{} : referenceOf(world, w->wheelPart);
}

bool setVehicleWheelWheelPart(World& world, core::InstanceId id, const Value& value)
{
    VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w != nullptr &&
           putReference(
               world, value, [&](core::InstanceId other) { return world.parts().find(other) != nullptr; },
               w->wheelPart);
}

Value getVehicleWheelDriven(const World& world, core::InstanceId id)
{
    const VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w == nullptr ? Value{} : Value{w->driven};
}

bool setVehicleWheelDriven(World& world, core::InstanceId id, const Value& value)
{
    VehicleWheelComponent* w = world.vehicleWheels().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (w == nullptr || flag == nullptr)
        return false;
    w->driven = *flag;
    return true;
}

Value getVehicleWheelGrounded(const World& world, core::InstanceId id)
{
    const VehicleWheelComponent* w = world.vehicleWheels().find(id);
    return w == nullptr ? Value{} : Value{w->grounded};
}

Value getVehicleWheelCompression(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::compression);
}

Value getVehicleWheelSlip(const World& world, core::InstanceId id)
{
    return numberOf(world.vehicleWheels().find(id), &VehicleWheelComponent::slip);
}

// The player of that user id on this machine, or none.
[[nodiscard]] core::InstanceId playerOfUser(const World& world, u32 user)
{
    core::InstanceId found;
    if (user == 0)
        return found;
    world.players().forEach([&](core::InstanceId player, const PlayerComponent& component) {
        if (component.userId == user && !found.valid())
            found = player;
    });
    return found;
}

Value getVehicleControllerDriver(const World& world, core::InstanceId id)
{
    const VehicleControllerComponent* c = world.vehicleControllers().find(id);
    return c == nullptr ? Value{} : referenceOf(world, playerOfUser(world, c->driver));
}

bool setVehicleControllerDriver(World& world, core::InstanceId id, const Value& value)
{
    VehicleControllerComponent* c = world.vehicleControllers().find(id);
    core::InstanceId player;
    if (c == nullptr ||
        !putReference(
            world, value, [&](core::InstanceId other) { return world.players().find(other) != nullptr; }, player))
        return false;
    c->driver = player.valid() ? world.players().find(player)->userId : 0;
    return true;
}

namespace {

[[nodiscard]] Value nameOf(const World& world, const VehicleControllerComponent* c,
                           core::NameAtom VehicleControllerComponent::*field)
{
    return c == nullptr ? Value{} : Value{std::string(world.atoms().text(c->*field))};
}

[[nodiscard]] bool putName(World& world, VehicleControllerComponent* c,
                           core::NameAtom VehicleControllerComponent::*field, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    if (c == nullptr || text == nullptr)
        return false;
    c->*field = world.atoms().intern(*text);
    return true;
}

} // namespace

Value getVehicleControllerDriveIntent(const World& world, core::InstanceId id)
{
    return nameOf(world, world.vehicleControllers().find(id), &VehicleControllerComponent::driveIntent);
}

bool setVehicleControllerDriveIntent(World& world, core::InstanceId id, const Value& value)
{
    return putName(world, world.vehicleControllers().find(id), &VehicleControllerComponent::driveIntent, value);
}

Value getVehicleControllerBrakeIntent(const World& world, core::InstanceId id)
{
    return nameOf(world, world.vehicleControllers().find(id), &VehicleControllerComponent::brakeIntent);
}

bool setVehicleControllerBrakeIntent(World& world, core::InstanceId id, const Value& value)
{
    return putName(world, world.vehicleControllers().find(id), &VehicleControllerComponent::brakeIntent, value);
}

Value getVehicleControllerHandBrakeIntent(const World& world, core::InstanceId id)
{
    return nameOf(world, world.vehicleControllers().find(id), &VehicleControllerComponent::handBrakeIntent);
}

bool setVehicleControllerHandBrakeIntent(World& world, core::InstanceId id, const Value& value)
{
    return putName(world, world.vehicleControllers().find(id), &VehicleControllerComponent::handBrakeIntent, value);
}

} // namespace engine::scene::native
