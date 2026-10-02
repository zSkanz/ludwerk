// Movers and constraints that move a part without code (ADR 0127): their
// properties, and the hooks that say which class a component belongs to.
//
// Written from one table (the stage's ledger says where it is kept), so a
// property's range, its default and its page say the same thing. Every write is
// checked here, where a refusal becomes a keyed error, rather than in the
// solver, where it would be a body that leaves the world.
#include <cmath>
#include <variant>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/core/finite.h"
#include "engine/physics/types.h"
#include "engine/scene/components.h"
#include "engine/scene/physics_sync.h"
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

// A `BasePart`, or nothing: what a `NoCollisionConstraint`'s two ends are.
[[nodiscard]] bool takePart(const World& world, const Value& value, core::InstanceId& out)
{
    if (std::holds_alternative<std::monostate>(value)) {
        out = core::InstanceId{};
        return true;
    }
    const auto* part = std::get_if<core::InstanceId>(&value);
    if (part == nullptr || !world.alive(*part) || world.parts().find(*part) == nullptr)
        return false;
    out = *part;
    return true;
}

} // namespace

// --- Hooks ---------------------------------------------------------------------
//
// As the three joints before them: the base added the `ConstraintComponent`,
// and each class stamps what it is onto it. A mover adds its own numbers
// beside it, and takes them away again.

void attachPrismaticConstraintComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr) {
        constraint->kind = static_cast<i32>(physics::ConstraintType::Slider);
        // A rail five metres long, once its limits are switched on.
        constraint->limitLow = 0.0f;
        constraint->limitHigh = 5.0f;
    }
}

void detachPrismaticConstraintComponents(World&, core::InstanceId)
{}

void attachRopeConstraintComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr) {
        constraint->kind = static_cast<i32>(physics::ConstraintType::Distance);
        constraint->flavor = 0;
    }
}

void detachRopeConstraintComponents(World&, core::InstanceId)
{}

void attachRodConstraintComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr) {
        constraint->kind = static_cast<i32>(physics::ConstraintType::Distance);
        constraint->flavor = 1;
    }
}

void detachRodConstraintComponents(World&, core::InstanceId)
{}

void attachSpringConstraintComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr) {
        constraint->kind = static_cast<i32>(physics::ConstraintType::Distance);
        constraint->flavor = 2;
        constraint->stiffness = 100.0f;
        constraint->damping = 1.0f;
    }
}

void detachSpringConstraintComponents(World&, core::InstanceId)
{}

void attachLinearVelocityComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::LinearVelocity;
    MoverComponent mover;
    mover.mode = 2;
    mover.relativeTo = 2;
    world.movers().add(id, mover);
}

void detachLinearVelocityComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachAngularVelocityComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::AngularVelocity;
    MoverComponent mover;
    mover.relativeTo = 2;
    world.movers().add(id, mover);
}

void detachAngularVelocityComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachAlignPositionComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::AlignPosition;
    MoverComponent mover;
    mover.mode = 0;
    world.movers().add(id, mover);
}

void detachAlignPositionComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachAlignOrientationComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::AlignOrientation;
    MoverComponent mover;
    mover.mode = 0;
    world.movers().add(id, mover);
}

void detachAlignOrientationComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachVectorForceComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::VectorForce;
    MoverComponent mover;
    mover.relativeTo = 0;
    world.movers().add(id, mover);
}

void detachVectorForceComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachTorqueComponents(World& world, core::InstanceId id)
{
    if (ConstraintComponent* constraint = world.constraints().find(id); constraint != nullptr)
        constraint->kind = MoverKind::Torque;
    MoverComponent mover;
    mover.relativeTo = 0;
    world.movers().add(id, mover);
}

void detachTorqueComponents(World& world, core::InstanceId id)
{
    world.movers().remove(id);
}

void attachNoCollisionConstraintComponents(World& world, core::InstanceId id)
{
    world.noCollisions().add(id, NoCollisionComponent{});
}

void detachNoCollisionConstraintComponents(World& world, core::InstanceId id)
{
    world.noCollisions().remove(id);
}

// --- Constraint ----------------------------------------------------------------

// Constraint.Visible
Value getConstraintVisible(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->visible};
}

bool setConstraintVisible(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->visible = *flag;
    return true;
}

// Constraint.BreakForce
Value getConstraintBreakForce(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->breakForce)};
}

bool setConstraintBreakForce(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->breakForce = next;
    return true;
}

// Constraint.BreakTorque
Value getConstraintBreakTorque(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->breakTorque)};
}

bool setConstraintBreakTorque(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->breakTorque = next;
    return true;
}

// --- HingeConstraint -----------------------------------------------------------

// HingeConstraint.ActuatorType
Value getHingeConstraintActuatorType(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorTypeEnumId, c->actuatorType}};
}

bool setHingeConstraintActuatorType(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorTypeEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->actuatorType = item->value;
    return true;
}

// HingeConstraint.AngularVelocity
Value getHingeConstraintAngularVelocity(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorVelocity)};
}

bool setHingeConstraintAngularVelocity(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->motorVelocity = next;
    return true;
}

// HingeConstraint.MotorMaxTorque
Value getHingeConstraintMotorMaxTorque(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorMaxForce)};
}

bool setHingeConstraintMotorMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->motorMaxForce = next;
    return true;
}

// HingeConstraint.MotorMaxAcceleration
Value getHingeConstraintMotorMaxAcceleration(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorMaxAcceleration)};
}

bool setHingeConstraintMotorMaxAcceleration(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->motorMaxAcceleration = next;
    return true;
}

// HingeConstraint.TargetAngle
Value getHingeConstraintTargetAngle(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoTarget)};
}

bool setHingeConstraintTargetAngle(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->servoTarget = next;
    return true;
}

// HingeConstraint.AngularSpeed
Value getHingeConstraintAngularSpeed(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoSpeed)};
}

bool setHingeConstraintAngularSpeed(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->servoSpeed = next;
    return true;
}

// HingeConstraint.ServoMaxTorque
Value getHingeConstraintServoMaxTorque(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoMaxForce)};
}

bool setHingeConstraintServoMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->servoMaxForce = next;
    return true;
}

// HingeConstraint.AngularResponsiveness
Value getHingeConstraintAngularResponsiveness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->responsiveness)};
}

bool setHingeConstraintAngularResponsiveness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next > 0.0f))
        return false;
    c->responsiveness = next;
    return true;
}

// HingeConstraint.Stiffness
Value getHingeConstraintStiffness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setHingeConstraintStiffness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// HingeConstraint.Damping
Value getHingeConstraintDamping(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setHingeConstraintDamping(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// --- BallSocketConstraint ------------------------------------------------------

// BallSocketConstraint.ActuatorType
Value getBallSocketConstraintActuatorType(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorTypeEnumId, c->actuatorType}};
}

bool setBallSocketConstraintActuatorType(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorTypeEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->actuatorType = item->value;
    return true;
}

// BallSocketConstraint.TargetOrientation
Value getBallSocketConstraintTargetOrientation(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{core::CFrameD{core::DVec3{}, c->targetOrientation}};
}

bool setBallSocketConstraintTargetOrientation(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* next = std::get_if<core::CFrameD>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->targetOrientation = next->rotation;
    return true;
}

// BallSocketConstraint.ServoMaxTorque
Value getBallSocketConstraintServoMaxTorque(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoMaxForce)};
}

bool setBallSocketConstraintServoMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->servoMaxForce = next;
    return true;
}

// BallSocketConstraint.AngularResponsiveness
Value getBallSocketConstraintAngularResponsiveness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->responsiveness)};
}

bool setBallSocketConstraintAngularResponsiveness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next > 0.0f))
        return false;
    c->responsiveness = next;
    return true;
}

// BallSocketConstraint.Stiffness
Value getBallSocketConstraintStiffness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setBallSocketConstraintStiffness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// BallSocketConstraint.Damping
Value getBallSocketConstraintDamping(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setBallSocketConstraintDamping(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// --- PrismaticConstraint -------------------------------------------------------

// PrismaticConstraint.LimitsEnabled
Value getPrismaticConstraintLimitsEnabled(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->limitsEnabled};
}

bool setPrismaticConstraintLimitsEnabled(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->limitsEnabled = *flag;
    return true;
}

// PrismaticConstraint.LowerLimit
Value getPrismaticConstraintLowerLimit(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->limitLow)};
}

bool setPrismaticConstraintLowerLimit(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->limitLow = next;
    return true;
}

// PrismaticConstraint.UpperLimit
Value getPrismaticConstraintUpperLimit(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->limitHigh)};
}

bool setPrismaticConstraintUpperLimit(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->limitHigh = next;
    return true;
}

// PrismaticConstraint.ActuatorType
Value getPrismaticConstraintActuatorType(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorTypeEnumId, c->actuatorType}};
}

bool setPrismaticConstraintActuatorType(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorTypeEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->actuatorType = item->value;
    return true;
}

// PrismaticConstraint.Velocity
Value getPrismaticConstraintVelocity(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorVelocity)};
}

bool setPrismaticConstraintVelocity(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->motorVelocity = next;
    return true;
}

// PrismaticConstraint.MotorMaxForce
Value getPrismaticConstraintMotorMaxForce(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorMaxForce)};
}

bool setPrismaticConstraintMotorMaxForce(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->motorMaxForce = next;
    return true;
}

// PrismaticConstraint.MotorMaxAcceleration
Value getPrismaticConstraintMotorMaxAcceleration(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->motorMaxAcceleration)};
}

bool setPrismaticConstraintMotorMaxAcceleration(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->motorMaxAcceleration = next;
    return true;
}

// PrismaticConstraint.TargetPosition
Value getPrismaticConstraintTargetPosition(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoTarget)};
}

bool setPrismaticConstraintTargetPosition(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->servoTarget = next;
    return true;
}

// PrismaticConstraint.Speed
Value getPrismaticConstraintSpeed(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoSpeed)};
}

bool setPrismaticConstraintSpeed(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->servoSpeed = next;
    return true;
}

// PrismaticConstraint.ServoMaxForce
Value getPrismaticConstraintServoMaxForce(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->servoMaxForce)};
}

bool setPrismaticConstraintServoMaxForce(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->servoMaxForce = next;
    return true;
}

// PrismaticConstraint.LinearResponsiveness
Value getPrismaticConstraintLinearResponsiveness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->responsiveness)};
}

bool setPrismaticConstraintLinearResponsiveness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next > 0.0f))
        return false;
    c->responsiveness = next;
    return true;
}

// PrismaticConstraint.Stiffness
Value getPrismaticConstraintStiffness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setPrismaticConstraintStiffness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// PrismaticConstraint.Damping
Value getPrismaticConstraintDamping(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setPrismaticConstraintDamping(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// --- RopeConstraint ------------------------------------------------------------

// RopeConstraint.Length
Value getRopeConstraintLength(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->length)};
}

bool setRopeConstraintLength(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->length = next;
    return true;
}

// RopeConstraint.WinchEnabled
Value getRopeConstraintWinchEnabled(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->winchEnabled};
}

bool setRopeConstraintWinchEnabled(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->winchEnabled = *flag;
    return true;
}

// RopeConstraint.WinchTarget
Value getRopeConstraintWinchTarget(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->winchTarget)};
}

bool setRopeConstraintWinchTarget(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->winchTarget = next;
    return true;
}

// RopeConstraint.WinchSpeed
Value getRopeConstraintWinchSpeed(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->winchSpeed)};
}

bool setRopeConstraintWinchSpeed(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->winchSpeed = next;
    return true;
}

// RopeConstraint.WinchForce
Value getRopeConstraintWinchForce(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->winchForce)};
}

bool setRopeConstraintWinchForce(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->winchForce = next;
    return true;
}

// RopeConstraint.Color
Value getRopeConstraintColor(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->color};
}

bool setRopeConstraintColor(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* next = std::get_if<core::Color3>(&value);
    if (c == nullptr || next == nullptr)
        return false;
    c->color = *next;
    return true;
}

// RopeConstraint.Thickness
Value getRopeConstraintThickness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->thickness)};
}

bool setRopeConstraintThickness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->thickness = next;
    return true;
}

// --- RodConstraint -------------------------------------------------------------

// RodConstraint.Length
Value getRodConstraintLength(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->length)};
}

bool setRodConstraintLength(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->length = next;
    return true;
}

// RodConstraint.Color
Value getRodConstraintColor(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->color};
}

bool setRodConstraintColor(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* next = std::get_if<core::Color3>(&value);
    if (c == nullptr || next == nullptr)
        return false;
    c->color = *next;
    return true;
}

// RodConstraint.Thickness
Value getRodConstraintThickness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->thickness)};
}

bool setRodConstraintThickness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->thickness = next;
    return true;
}

// --- SpringConstraint ----------------------------------------------------------

// SpringConstraint.FreeLength
Value getSpringConstraintFreeLength(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->length)};
}

bool setSpringConstraintFreeLength(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->length = next;
    return true;
}

// SpringConstraint.Stiffness
Value getSpringConstraintStiffness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setSpringConstraintStiffness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// SpringConstraint.Damping
Value getSpringConstraintDamping(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setSpringConstraintDamping(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// SpringConstraint.LimitsEnabled
Value getSpringConstraintLimitsEnabled(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->limitsEnabled};
}

bool setSpringConstraintLimitsEnabled(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->limitsEnabled = *flag;
    return true;
}

// SpringConstraint.MinLength
Value getSpringConstraintMinLength(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->minLength)};
}

bool setSpringConstraintMinLength(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->minLength = next;
    return true;
}

// SpringConstraint.MaxLength
Value getSpringConstraintMaxLength(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxLength)};
}

bool setSpringConstraintMaxLength(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxLength = next;
    return true;
}

// SpringConstraint.Color
Value getSpringConstraintColor(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{c->color};
}

bool setSpringConstraintColor(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    const auto* next = std::get_if<core::Color3>(&value);
    if (c == nullptr || next == nullptr)
        return false;
    c->color = *next;
    return true;
}

// SpringConstraint.Thickness
Value getSpringConstraintThickness(const World& world, core::InstanceId id)
{
    const ConstraintComponent* c = world.constraints().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->thickness)};
}

bool setSpringConstraintThickness(World& world, core::InstanceId id, const Value& value)
{
    ConstraintComponent* c = world.constraints().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->thickness = next;
    return true;
}

// --- LinearVelocity ------------------------------------------------------------

// LinearVelocity.VelocityConstraintMode
Value getLinearVelocityVelocityConstraintMode(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::VelocityConstraintModeEnumId, c->mode}};
}

bool setLinearVelocityVelocityConstraintMode(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::VelocityConstraintModeEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->mode = item->value;
    return true;
}

// LinearVelocity.VectorVelocity
Value getLinearVelocityVectorVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->vector};
}

bool setLinearVelocityVectorVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->vector = *next;
    return true;
}

// LinearVelocity.LineDirection
Value getLinearVelocityLineDirection(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->lineDirection};
}

bool setLinearVelocityLineDirection(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->lineDirection = *next;
    return true;
}

// LinearVelocity.LineVelocity
Value getLinearVelocityLineVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->lineVelocity)};
}

bool setLinearVelocityLineVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(true))
        return false;
    c->lineVelocity = next;
    return true;
}

// LinearVelocity.PrimaryTangentAxis
Value getLinearVelocityPrimaryTangentAxis(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->primaryTangentAxis};
}

bool setLinearVelocityPrimaryTangentAxis(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->primaryTangentAxis = *next;
    return true;
}

// LinearVelocity.SecondaryTangentAxis
Value getLinearVelocitySecondaryTangentAxis(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->secondaryTangentAxis};
}

bool setLinearVelocitySecondaryTangentAxis(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->secondaryTangentAxis = *next;
    return true;
}

// LinearVelocity.PlaneVelocity
Value getLinearVelocityPlaneVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->planeVelocity};
}

bool setLinearVelocityPlaneVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec2>(&value);
    if (c == nullptr || next == nullptr || !std::isfinite(next->x) || !std::isfinite(next->y))
        return false;
    c->planeVelocity = *next;
    return true;
}

// LinearVelocity.MaxForce
Value getLinearVelocityMaxForce(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxForce)};
}

bool setLinearVelocityMaxForce(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxForce = next;
    return true;
}

// LinearVelocity.RelativeTo
Value getLinearVelocityRelativeTo(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorRelativeToEnumId, c->relativeTo}};
}

bool setLinearVelocityRelativeTo(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorRelativeToEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->relativeTo = item->value;
    return true;
}

// --- AngularVelocity -----------------------------------------------------------

// AngularVelocity.AngularVelocity
Value getAngularVelocityAngularVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->vector};
}

bool setAngularVelocityAngularVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->vector = *next;
    return true;
}

// AngularVelocity.MaxTorque
Value getAngularVelocityMaxTorque(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxTorque)};
}

bool setAngularVelocityMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxTorque = next;
    return true;
}

// AngularVelocity.RelativeTo
Value getAngularVelocityRelativeTo(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorRelativeToEnumId, c->relativeTo}};
}

bool setAngularVelocityRelativeTo(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorRelativeToEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->relativeTo = item->value;
    return true;
}

// AngularVelocity.ReactionTorqueEnabled
Value getAngularVelocityReactionTorqueEnabled(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->reactionEnabled};
}

bool setAngularVelocityReactionTorqueEnabled(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->reactionEnabled = *flag;
    return true;
}

// --- AlignPosition -------------------------------------------------------------

// AlignPosition.Mode
Value getAlignPositionMode(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::PositionAlignmentModeEnumId, c->mode}};
}

bool setAlignPositionMode(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::PositionAlignmentModeEnumId || item->value < 0 ||
        item->value > 1)
        return false;
    c->mode = item->value;
    return true;
}

// AlignPosition.Position
Value getAlignPositionPosition(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{core::toVec3(c->position)};
}

bool setAlignPositionPosition(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->position = core::toDVec3(*next);
    return true;
}

// AlignPosition.MaxForce
Value getAlignPositionMaxForce(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxForce)};
}

bool setAlignPositionMaxForce(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxForce = next;
    return true;
}

// AlignPosition.MaxVelocity
Value getAlignPositionMaxVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxVelocity)};
}

bool setAlignPositionMaxVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxVelocity = next;
    return true;
}

// AlignPosition.Responsiveness
Value getAlignPositionResponsiveness(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->responsiveness)};
}

bool setAlignPositionResponsiveness(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next > 0.0f))
        return false;
    c->responsiveness = next;
    return true;
}

// AlignPosition.RigidityEnabled
Value getAlignPositionRigidityEnabled(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->rigidityEnabled};
}

bool setAlignPositionRigidityEnabled(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->rigidityEnabled = *flag;
    return true;
}

// AlignPosition.ApplyAtCenterOfMass
Value getAlignPositionApplyAtCenterOfMass(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->applyAtCenterOfMass};
}

bool setAlignPositionApplyAtCenterOfMass(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->applyAtCenterOfMass = *flag;
    return true;
}

// AlignPosition.ReactionForceEnabled
Value getAlignPositionReactionForceEnabled(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->reactionEnabled};
}

bool setAlignPositionReactionForceEnabled(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->reactionEnabled = *flag;
    return true;
}

// AlignPosition.Stiffness
Value getAlignPositionStiffness(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setAlignPositionStiffness(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// AlignPosition.Damping
Value getAlignPositionDamping(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setAlignPositionDamping(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// --- AlignOrientation ----------------------------------------------------------

// AlignOrientation.Mode
Value getAlignOrientationMode(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::OrientationAlignmentModeEnumId, c->mode}};
}

bool setAlignOrientationMode(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::OrientationAlignmentModeEnumId ||
        item->value < 0 || item->value > 1)
        return false;
    c->mode = item->value;
    return true;
}

// AlignOrientation.CFrame
Value getAlignOrientationCFrame(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{core::CFrameD{core::DVec3{}, c->orientation}};
}

bool setAlignOrientationCFrame(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::CFrameD>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->orientation = next->rotation;
    return true;
}

// AlignOrientation.MaxTorque
Value getAlignOrientationMaxTorque(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxTorque)};
}

bool setAlignOrientationMaxTorque(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxTorque = next;
    return true;
}

// AlignOrientation.MaxAngularVelocity
Value getAlignOrientationMaxAngularVelocity(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->maxAngularVelocity)};
}

bool setAlignOrientationMaxAngularVelocity(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->maxAngularVelocity = next;
    return true;
}

// AlignOrientation.Responsiveness
Value getAlignOrientationResponsiveness(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->responsiveness)};
}

bool setAlignOrientationResponsiveness(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next > 0.0f))
        return false;
    c->responsiveness = next;
    return true;
}

// AlignOrientation.RigidityEnabled
Value getAlignOrientationRigidityEnabled(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->rigidityEnabled};
}

bool setAlignOrientationRigidityEnabled(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->rigidityEnabled = *flag;
    return true;
}

// AlignOrientation.ReactionTorqueEnabled
Value getAlignOrientationReactionTorqueEnabled(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->reactionEnabled};
}

bool setAlignOrientationReactionTorqueEnabled(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->reactionEnabled = *flag;
    return true;
}

// AlignOrientation.Stiffness
Value getAlignOrientationStiffness(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->stiffness)};
}

bool setAlignOrientationStiffness(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->stiffness = next;
    return true;
}

// AlignOrientation.Damping
Value getAlignOrientationDamping(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->damping)};
}

bool setAlignOrientationDamping(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->damping = next;
    return true;
}

// --- VectorForce ---------------------------------------------------------------

// VectorForce.Force
Value getVectorForceForce(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->vector};
}

bool setVectorForceForce(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->vector = *next;
    return true;
}

// VectorForce.RelativeTo
Value getVectorForceRelativeTo(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorRelativeToEnumId, c->relativeTo}};
}

bool setVectorForceRelativeTo(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorRelativeToEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->relativeTo = item->value;
    return true;
}

// VectorForce.ApplyAtCenterOfMass
Value getVectorForceApplyAtCenterOfMass(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->applyAtCenterOfMass};
}

bool setVectorForceApplyAtCenterOfMass(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->applyAtCenterOfMass = *flag;
    return true;
}

// --- Torque --------------------------------------------------------------------

// Torque.Torque
Value getTorqueTorque(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{c->vector};
}

bool setTorqueTorque(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (c == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    c->vector = *next;
    return true;
}

// Torque.RelativeTo
Value getTorqueRelativeTo(const World& world, core::InstanceId id)
{
    const MoverComponent* c = world.movers().find(id);
    return c == nullptr ? Value{} : Value{EnumValue{generated::ActuatorRelativeToEnumId, c->relativeTo}};
}

bool setTorqueRelativeTo(World& world, core::InstanceId id, const Value& value)
{
    MoverComponent* c = world.movers().find(id);
    const auto* item = std::get_if<EnumValue>(&value);
    if (c == nullptr || item == nullptr || item->enumId != generated::ActuatorRelativeToEnumId || item->value < 0 ||
        item->value > 2)
        return false;
    c->relativeTo = item->value;
    return true;
}

// --- NoCollisionConstraint -----------------------------------------------------

Value getNoCollisionConstraintPart0(const World& world, core::InstanceId id)
{
    const NoCollisionComponent* c = world.noCollisions().find(id);
    return c == nullptr || !c->part0.valid() ? Value{} : Value{c->part0};
}

bool setNoCollisionConstraintPart0(World& world, core::InstanceId id, const Value& value)
{
    NoCollisionComponent* c = world.noCollisions().find(id);
    return c != nullptr && takePart(world, value, c->part0);
}

Value getNoCollisionConstraintPart1(const World& world, core::InstanceId id)
{
    const NoCollisionComponent* c = world.noCollisions().find(id);
    return c == nullptr || !c->part1.valid() ? Value{} : Value{c->part1};
}

bool setNoCollisionConstraintPart1(World& world, core::InstanceId id, const Value& value)
{
    NoCollisionComponent* c = world.noCollisions().find(id);
    return c != nullptr && takePart(world, value, c->part1);
}

Value getNoCollisionConstraintEnabled(const World& world, core::InstanceId id)
{
    const NoCollisionComponent* c = world.noCollisions().find(id);
    return c == nullptr ? Value{} : Value{c->enabled};
}

bool setNoCollisionConstraintEnabled(World& world, core::InstanceId id, const Value& value)
{
    NoCollisionComponent* c = world.noCollisions().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->enabled = *flag;
    return true;
}

// --- CharacterBody (D466) ------------------------------------------------------

Value getCharacterBodyGravityScale(const World& world, core::InstanceId id)
{
    const CharacterBodyComponent* c = world.characterBodies().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->gravityScale)};
}

bool setCharacterBodyGravityScale(World& world, core::InstanceId id, const Value& value)
{
    CharacterBodyComponent* c = world.characterBodies().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next))
        return false;
    c->gravityScale = next;
    return true;
}

Value getCharacterBodySwimSpeed(const World& world, core::InstanceId id)
{
    const CharacterBodyComponent* c = world.characterBodies().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->swimSpeed)};
}

bool setCharacterBodySwimSpeed(World& world, core::InstanceId id, const Value& value)
{
    CharacterBodyComponent* c = world.characterBodies().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->swimSpeed = next;
    return true;
}

Value getCharacterBodyFlySpeed(const World& world, core::InstanceId id)
{
    const CharacterBodyComponent* c = world.characterBodies().find(id);
    return c == nullptr ? Value{} : Value{static_cast<f64>(c->flySpeed)};
}

bool setCharacterBodyFlySpeed(World& world, core::InstanceId id, const Value& value)
{
    CharacterBodyComponent* c = world.characterBodies().find(id);
    f32 next = 0.0f;
    if (c == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    c->flySpeed = next;
    return true;
}

Value getCharacterBodyFlying(const World& world, core::InstanceId id)
{
    const CharacterBodyComponent* c = world.characterBodies().find(id);
    return c == nullptr ? Value{} : Value{c->flying};
}

bool setCharacterBodyFlying(World& world, core::InstanceId id, const Value& value)
{
    CharacterBodyComponent* c = world.characterBodies().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (c == nullptr || flag == nullptr)
        return false;
    c->flying = *flag;
    return true;
}

Value getCharacterBodyFloorMaterial(const World& world, core::InstanceId id)
{
    const MaterialRef floor = floorMaterial(world, id);
    return floor.source.empty() ? Value{} : Value{floor};
}

// --- BasePart ------------------------------------------------------------------

Value getBasePartMass(const World& world, core::InstanceId id)
{
    // From what the part is, not from what the simulation last measured: a
    // part made this instant weighs what it will weigh, and one just resized
    // weighs what it does now (D472).
    return world.rigidBodies().find(id) == nullptr ? Value{} : Value{static_cast<f64>(partMass(world, id))};
}

// **Everything rigidly joined, added up**: a walk over the welds and the fixed
// joints from this part outwards. A weld drives one part from another and a
// fixed joint has the solver hold them, and either way what moves one moves
// all of them.
Value getBasePartAssemblyMass(const World& world, core::InstanceId id)
{
    if (world.rigidBodies().find(id) == nullptr)
        return Value{};
    std::vector<core::InstanceId> joined{id};
    const auto reach = [&](core::InstanceId from, core::InstanceId to) {
        if (!from.valid() || !to.valid())
            return false;
        bool hasFrom = false;
        bool hasTo = false;
        for (const core::InstanceId seen : joined) {
            hasFrom = hasFrom || seen == from;
            hasTo = hasTo || seen == to;
        }
        if (hasFrom == hasTo)
            return false;
        joined.push_back(hasFrom ? to : from);
        return true;
    };
    // Until a pass adds nothing: an assembly is a handful of parts, and the
    // pools are walked in their own order.
    for (bool grew = true; grew;) {
        grew = false;
        world.welds().forEach(
            [&](core::InstanceId, const WeldComponent& weld) { grew = reach(weld.part0, weld.part1) || grew; });
        world.constraints().forEach([&](core::InstanceId, const ConstraintComponent& constraint) {
            if (constraint.kind != static_cast<i32>(physics::ConstraintType::Fixed) || !constraint.enabled)
                return;
            grew = reach(world.parentOf(constraint.attachment0), world.parentOf(constraint.attachment1)) || grew;
        });
    }
    f64 total = 0.0;
    for (const core::InstanceId part : joined) {
        total += static_cast<f64>(partMass(world, part));
    }
    return Value{total};
}

Value getBasePartLinearDamping(const World& world, core::InstanceId id)
{
    const RigidBodyComponent* body = world.rigidBodies().find(id);
    return body == nullptr ? Value{} : Value{static_cast<f64>(body->linearDamping)};
}

bool setBasePartLinearDamping(World& world, core::InstanceId id, const Value& value)
{
    RigidBodyComponent* body = world.rigidBodies().find(id);
    f32 next = 0.0f;
    if (body == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    body->linearDamping = next;
    return true;
}

Value getBasePartAngularDamping(const World& world, core::InstanceId id)
{
    const RigidBodyComponent* body = world.rigidBodies().find(id);
    return body == nullptr ? Value{} : Value{static_cast<f64>(body->angularDamping)};
}

bool setBasePartAngularDamping(World& world, core::InstanceId id, const Value& value)
{
    RigidBodyComponent* body = world.rigidBodies().find(id);
    f32 next = 0.0f;
    if (body == nullptr || !takeF32(value, next) || !(next >= 0.0f))
        return false;
    body->angularDamping = next;
    return true;
}

Value getBasePartContactDetails(const World& world, core::InstanceId id)
{
    const RigidBodyComponent* body = world.rigidBodies().find(id);
    return body == nullptr ? Value{} : Value{body->contactDetails};
}

bool setBasePartContactDetails(World& world, core::InstanceId id, const Value& value)
{
    RigidBodyComponent* body = world.rigidBodies().find(id);
    const auto* flag = std::get_if<bool>(&value);
    if (body == nullptr || flag == nullptr)
        return false;
    body->contactDetails = *flag;
    return true;
}

// **A velocity a script wrote is the speed the next tick starts from** (N5).
// Kept in the component at once, so reading it back says what was written, and
// flagged for the mirror, which hands it to the solver.
bool setBasePartLinearVelocity(World& world, core::InstanceId id, const Value& value)
{
    RigidBodyComponent* body = world.rigidBodies().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (body == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    body->linearVelocity = *next;
    body->velocityWritten = true;
    return true;
}

bool setBasePartAngularVelocity(World& world, core::InstanceId id, const Value& value)
{
    RigidBodyComponent* body = world.rigidBodies().find(id);
    const auto* next = std::get_if<core::Vec3>(&value);
    if (body == nullptr || next == nullptr || !core::isFinite(*next))
        return false;
    body->angularVelocity = *next;
    body->velocityWritten = true;
    return true;
}

} // namespace engine::scene::native
