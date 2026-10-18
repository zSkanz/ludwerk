// Vehicles (ADR 0213): a `VehicleController` and its `VehicleWheel`s, as the
// physics seam's vehicle on the chassis' body. Built after every body and joint
// is as the scene says, given what its driver asks each tick, and read back
// after the step -- its speed, its engine, its gear, each wheel -- with each
// wheel's part placed where the wheel is.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/scene/components.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/world.h"

namespace engine::scene {
namespace {

constexpr f32 kDegrees = 3.14159265358979f / 180.0f;

// **What a default asks of a chassis of this mass**, for the torques and
// inertias a property leaves at nought. A part weighs its density times its
// volume, and a car of default parts weighs tens of kilograms, where the
// vehicle's own defaults are a road car's ton and a half: these keep the
// proportions of that car -- its engine, its brakes, its wheels -- at any mass.
// **Under what two driven wheels can grip**: through first gear and the final
// drive (about nine to one) this is some 3.6 m/s^2 at the tyres, where the
// rear wheels hold about half of g. More and the driven wheels spin the whole
// time -- which the gearbox reads as slipping, and never shifts up.
[[nodiscard]] f32 engineTorqueFor(f32 mass, f32 radius) noexcept
{
    return 0.4f * mass * radius;
}

[[nodiscard]] f32 brakeTorqueFor(f32 mass, f32 radius) noexcept
{
    return 3.3f * mass * radius;
}

[[nodiscard]] f32 wheelInertiaFor(f32 mass, f32 radius) noexcept
{
    return 0.5f * (mass / 75.0f) * radius * radius;
}

// The description as one number, so a vehicle is built again only when what
// it was built from changed.
[[nodiscard]] u64 signatureOf(const physics::VehicleDesc& desc) noexcept
{
    u64 hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, usize size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (usize index = 0; index < size; ++index)
            hash = (hash ^ bytes[index]) * 1099511628211ull;
    };
    const auto number = [&](f32 value) { mix(&value, sizeof(value)); };
    mix(&desc.chassis, sizeof(desc.chassis));
    for (const physics::VehicleWheelDesc& wheel : desc.wheels) {
        number(wheel.position.x);
        number(wheel.position.y);
        number(wheel.position.z);
        for (const f32 value :
             {wheel.radius, wheel.width, wheel.suspensionMinLength, wheel.suspensionMaxLength,
              wheel.suspensionFrequency, wheel.suspensionDamping, wheel.maxSteerAngle, wheel.maxBrakeTorque,
              wheel.maxHandBrakeTorque, wheel.longitudinalFriction, wheel.lateralFriction, wheel.inertia})
            number(value);
        const u8 driven = wheel.driven ? 1 : 0;
        mix(&driven, 1);
    }
    for (const f32 value : {desc.maxTorque, desc.minRpm, desc.maxRpm, desc.engineInertia, desc.shiftUpRpm,
                            desc.shiftDownRpm, desc.differentialRatio, desc.maxPitchRollAngle})
        number(value);
    const u8 manual = desc.manual ? 1 : 0;
    mix(&manual, 1);
    return hash;
}

// One intent of `intents`, by its action's name, or none.
[[nodiscard]] const PlayerIntent* intentNamed(std::span<const PlayerIntent> intents, core::NameAtom name) noexcept
{
    if (name.id == 0)
        return nullptr;
    for (const PlayerIntent& intent : intents) {
        if (intent.action.id == name.id)
            return &intent;
    }
    return nullptr;
}

// An intent as an amount: a direction's first axis, a button's press.
[[nodiscard]] f32 amountOf(const PlayerIntent* intent) noexcept
{
    if (intent == nullptr)
        return 0.0f;
    return intent->type == 0 ? (intent->pressed ? 1.0f : 0.0f) : intent->axis.x;
}

} // namespace

std::span<const PlayerIntent> PhysicsSync::driverIntents(u32 driver) const
{
    const PlayerComponent* player = nullptr;
    m_scene.players().forEach([&](core::InstanceId, const PlayerComponent& component) {
        if (component.userId == driver && player == nullptr)
            player = &component;
    });
    if (player == nullptr)
        return {};
    // **In a step taken again, the intents it was first taken with**: the
    // command its character is being stepped by. A vehicle driven by
    // somebody with no character is driven by what they asked last.
    for (const auto& [character, command] : m_replaying) {
        if (character == player->character && command != nullptr)
            return command->intents;
    }
    return player->intents;
}

void PhysicsSync::applyVehicles()
{
    if (m_scene.vehicleControllers().size() == 0 && m_vehicles.empty())
        return;
    for (VehicleRecord& record : m_vehicles)
        record.seen = false;

    m_scene.vehicleControllers().forEach([&](core::InstanceId id, VehicleControllerComponent& controller) {
        const core::InstanceId chassisPart = m_scene.parentOf(id);
        if (!chassisPart.valid() || !inWorld(chassisPart))
            return;
        const PartComponent* part = m_scene.parts().find(chassisPart);
        const physics::BodyHandle body = bodyHandleOf(chassisPart);
        if (part == nullptr || !body.valid())
            return;
        const physics::BodyMassProperties mass = m_backend.bodyMassProperties(m_world, body);
        if (!mass.dynamic)
            return;

        physics::VehicleDesc desc;
        desc.chassis = body;
        std::vector<core::InstanceId> wheels;
        f32 drivenRadius = 0.0f;
        for (core::InstanceId child = m_scene.firstChild(id); child.valid(); child = m_scene.nextSibling(child)) {
            const VehicleWheelComponent* wheel = m_scene.vehicleWheels().find(child);
            if (wheel == nullptr || !wheel->attachment.valid() || m_scene.parentOf(wheel->attachment) != chassisPart)
                continue;
            const AttachmentComponent* attachment = m_scene.attachments().find(wheel->attachment);
            if (attachment == nullptr)
                continue;
            // **In the chassis' own space, as authored**: a part's body is in
            // the part's frame, and the attachment's place is already there.
            // Through the body's moving transform it came out a rounding
            // different every tick, and the vehicle was built again each
            // one, its engine starting from rest each time.
            physics::VehicleWheelDesc given;
            given.position = core::toVec3(attachment->cframe.position);
            given.radius = wheel->radius;
            given.width = wheel->width;
            given.suspensionMinLength = wheel->suspensionMinLength;
            given.suspensionMaxLength = std::max(wheel->suspensionMaxLength, wheel->suspensionMinLength);
            given.suspensionFrequency = wheel->suspensionFrequency;
            given.suspensionDamping = wheel->suspensionDamping;
            given.maxSteerAngle = wheel->maxSteerAngle * kDegrees;
            given.driven = wheel->driven;
            given.maxBrakeTorque =
                wheel->maxBrakeTorque > 0.0f ? wheel->maxBrakeTorque : brakeTorqueFor(mass.mass, wheel->radius);
            given.maxHandBrakeTorque = wheel->maxHandBrakeTorque;
            given.longitudinalFriction = wheel->grip;
            given.lateralFriction = wheel->grip;
            given.inertia = wheelInertiaFor(mass.mass, wheel->radius);
            if (wheel->driven)
                drivenRadius = std::max(drivenRadius, wheel->radius);
            desc.wheels.push_back(given);
            wheels.push_back(child);
        }
        if (desc.wheels.empty())
            return;
        desc.maxTorque = controller.maxTorque > 0.0f
                             ? controller.maxTorque
                             : engineTorqueFor(mass.mass, drivenRadius > 0.0f ? drivenRadius : 0.35f);
        desc.minRpm = controller.minRpm;
        desc.maxRpm = std::max(controller.maxRpm, controller.minRpm + 1.0f);
        desc.engineInertia = std::max(0.5f * mass.mass / 1500.0f, 1.0e-3f);
        desc.manual = controller.transmission == 1;
        desc.shiftUpRpm = controller.shiftUpRpm;
        desc.shiftDownRpm = controller.shiftDownRpm;
        desc.differentialRatio = controller.finalDriveRatio;
        desc.maxPitchRollAngle = std::min(controller.maxLeanAngle, 180.0f) * kDegrees;
        desc.userData = userDataOf(id);

        if (m_vehicles.size() <= id.index)
            m_vehicles.resize(id.index + 1);
        VehicleRecord& record = m_vehicles[id.index];
        const u64 signature = signatureOf(desc);
        if (record.handle.valid() && (record.generation != id.generation || record.signature != signature)) {
            m_backend.destroyVehicle(m_world, record.handle);
            record.handle = {};
        }
        if (!record.handle.valid()) {
            record.handle = m_backend.createVehicle(m_world, desc);
            record.generation = id.generation;
            record.signature = signature;
            if (!record.handle.valid())
                return;
        }
        record.seen = true;
        record.wheels = std::move(wheels);
        // **Driven by a player** (ADR 0213, in a match): their intents write
        // the inputs, here, every tick -- on the authority with what they
        // sent, and on their machine with what they did, and again in a
        // replay with what they did then.
        if (controller.driver != 0) {
            const std::span<const PlayerIntent> intents = driverIntents(controller.driver);
            const PlayerIntent* drive = intentNamed(intents, controller.driveIntent);
            controller.throttle = drive != nullptr ? std::clamp(drive->axis.y, -1.0f, 1.0f) : 0.0f;
            controller.steer = drive != nullptr ? std::clamp(drive->axis.x, -1.0f, 1.0f) : 0.0f;
            controller.brake = std::clamp(amountOf(intentNamed(intents, controller.brakeIntent)), 0.0f, 1.0f);
            controller.handBrake = std::clamp(amountOf(intentNamed(intents, controller.handBrakeIntent)), 0.0f, 1.0f);
        }
        m_backend.setVehicleInput(m_world, record.handle,
                                  physics::VehicleInput{.forward = controller.throttle,
                                                        .right = controller.steer,
                                                        .brake = controller.brake,
                                                        .handBrake = controller.handBrake,
                                                        .gear = controller.gear});
    });

    for (VehicleRecord& record : m_vehicles) {
        if (record.seen || !record.handle.valid())
            continue;
        m_backend.destroyVehicle(m_world, record.handle);
        record = VehicleRecord{};
    }
}

void PhysicsSync::readVehicles()
{
    for (u32 index = 0; index < static_cast<u32>(m_vehicles.size()); ++index) {
        VehicleRecord& record = m_vehicles[index];
        if (!record.handle.valid())
            continue;
        const core::InstanceId id{index, record.generation};
        VehicleControllerComponent* controller = m_scene.vehicleControllers().find(id);
        const core::InstanceId chassisPart = m_scene.parentOf(id);
        const physics::BodyHandle body = bodyHandleOf(chassisPart);
        if (controller == nullptr || !body.valid())
            continue;
        const physics::VehicleState state = m_backend.vehicleState(m_world, record.handle);
        const physics::BodyState chassis = m_backend.bodyState(m_world, body);
        // Along the chassis' forward, its LookVector: negative going back.
        const core::Mat3& turn = chassis.transform.rotation;
        const core::Vec3 forward{-turn.m[2][0], -turn.m[2][1], -turn.m[2][2]};
        controller->speed = core::dot(chassis.linearVelocity, forward);
        controller->rpm = state.rpm;
        controller->currentGear = state.gear;
        controller->clutch = state.clutch;
        controller->gearSwitchTimeLeft = state.gearSwitchTimeLeft;
        controller->clutchReleaseTimeLeft = state.clutchReleaseTimeLeft;
        controller->gearSwitchLatencyTimeLeft = state.gearSwitchLatencyTimeLeft;
        controller->previousDeltaTime = state.previousDeltaTime;
        for (usize at = 0; at < record.wheels.size() && at < state.wheels.size(); ++at) {
            VehicleWheelComponent* wheel = m_scene.vehicleWheels().find(record.wheels[at]);
            if (wheel == nullptr)
                continue;
            const physics::VehicleWheelState& done = state.wheels[at];
            wheel->grounded = done.grounded;
            const f32 travel = wheel->suspensionMaxLength - wheel->suspensionMinLength;
            wheel->compression =
                travel > 0.0f ? std::clamp((wheel->suspensionMaxLength - done.suspensionLength) / travel, 0.0f, 1.0f)
                              : 0.0f;
            wheel->slip = done.lateralSlip / kDegrees;
            wheel->angularVelocity = done.angularVelocity;
            wheel->rotation = done.rotation;
            wheel->steerAngle = done.steerAngle;
            wheel->suspensionLength = done.suspensionLength;
            wheel->suspensionLambda = done.suspensionLambda;
            wheel->suspensionMaxUpLambda = done.suspensionMaxUpLambda;
            wheel->longitudinalLambda = done.longitudinalLambda;
            wheel->lateralLambda = done.lateralLambda;
            // Written as the writeback writes a body's part: quietly, into
            // the component.
            if (PartComponent* drawn = m_scene.parts().find(wheel->wheelPart); drawn != nullptr)
                drawn->cframe = done.transform;
        }
    }
}

void PhysicsSync::putVehicle(const CharacterReplayStart::Vehicle& vehicle)
{
    if (vehicle.controller.index >= m_vehicles.size())
        return;
    const VehicleRecord& record = m_vehicles[vehicle.controller.index];
    if (!record.handle.valid() || record.generation != vehicle.controller.generation)
        return;
    // **Over what the island had**, and nothing of it left that the
    // authority said: a wheel the answer did not carry keeps what the island
    // had.
    const physics::VehicleState had = m_backend.vehicleState(m_world, record.handle);
    physics::VehicleState state = had;
    state.rpm = vehicle.rpm;
    state.gear = vehicle.gear;
    state.clutch = vehicle.clutch;
    state.gearSwitchTimeLeft = vehicle.gearSwitchTimeLeft;
    state.clutchReleaseTimeLeft = vehicle.clutchReleaseTimeLeft;
    state.gearSwitchLatencyTimeLeft = vehicle.gearSwitchLatencyTimeLeft;
    state.previousDeltaTime = vehicle.previousDeltaTime;
    // In the order the vehicle was built with.
    state.wheels.clear();
    for (usize at = 0; at < record.wheels.size(); ++at) {
        physics::VehicleWheelState put = at < had.wheels.size() ? had.wheels[at] : physics::VehicleWheelState{};
        for (const CharacterReplayStart::Vehicle::Wheel& wheel : vehicle.wheels) {
            if (wheel.id == record.wheels[at]) {
                put.angularVelocity = wheel.angularVelocity;
                put.rotation = wheel.rotation;
                put.steerAngle = wheel.steerAngle;
                put.suspensionLength = wheel.suspensionLength;
                put.suspensionLambda = wheel.suspensionLambda;
                put.suspensionMaxUpLambda = wheel.suspensionMaxUpLambda;
                put.longitudinalLambda = wheel.longitudinalLambda;
                put.lateralLambda = wheel.lateralLambda;
            }
        }
        state.wheels.push_back(put);
    }
    m_backend.setVehicleState(m_world, record.handle, state);
}

} // namespace engine::scene
