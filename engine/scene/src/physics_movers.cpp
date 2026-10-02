// Movers and what a joint does of itself (ADR 0127): the half of the physics
// mirror that turns "hold this speed", "go to that place" and "a spring this
// stiff" into impulses, before each step.
//
// **Everything here is a function of the world as the step finds it**: the
// bodies' positions and velocities from the solver, the instances' properties,
// and the tick's length. Nothing is remembered between ticks -- a servo's error
// is measured, a winch's rope is the `Length` property itself -- so a rollback
// that restores the world restores every one of these with it, and a replay
// makes the same impulses.
//
// Evaluated in the constraint pool's own order, which is the order the
// instances were made in (R10).
#include <algorithm>
#include <cmath>

#include "engine/scene/physics_sync.h"
#include "engine/scene/world.h"

namespace engine::scene {
namespace {

using core::Mat3;
using core::Vec3;

static_assert(MoverKind::DistanceJoint == static_cast<i32>(physics::ConstraintType::Distance),
              "a rope is drawn by the kind the solver builds it as");

constexpr f32 kDegreesToRadians = 0.017453292519943295f;
constexpr f32 kPi = 3.14159265358979f;
// Below this an impulse is not applied at all: a mover resting at its target
// would otherwise wake its body every tick, for ever.
constexpr f32 kQuietImpulse = 1.0e-6f;

[[nodiscard]] f32 lengthOf(Vec3 v) noexcept
{
    return std::sqrt(core::dot(v, v));
}

[[nodiscard]] Vec3 normalizedOr(Vec3 v, Vec3 fallback) noexcept
{
    const f32 length = lengthOf(v);
    return length > 1.0e-6f ? v * (1.0f / length) : fallback;
}

[[nodiscard]] Vec3 clamped(Vec3 v, f32 most) noexcept
{
    const f32 length = lengthOf(v);
    return length > most && length > 0.0f ? v * (most / length) : v;
}

// `m[column][row]`, as `core::Mat3` stores it.
[[nodiscard]] Mat3 inverseOf(const Mat3& a, bool& ok) noexcept
{
    const f32 a00 = a.m[0][0], a01 = a.m[1][0], a02 = a.m[2][0];
    const f32 a10 = a.m[0][1], a11 = a.m[1][1], a12 = a.m[2][1];
    const f32 a20 = a.m[0][2], a21 = a.m[1][2], a22 = a.m[2][2];
    const f32 c00 = a11 * a22 - a12 * a21;
    const f32 c01 = a12 * a20 - a10 * a22;
    const f32 c02 = a10 * a21 - a11 * a20;
    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    Mat3 out;
    ok = std::fabs(det) > 1.0e-20f;
    if (!ok)
        return out;
    const f32 inv = 1.0f / det;
    out.m[0][0] = c00 * inv;
    out.m[1][0] = (a02 * a21 - a01 * a22) * inv;
    out.m[2][0] = (a01 * a12 - a02 * a11) * inv;
    out.m[0][1] = c01 * inv;
    out.m[1][1] = (a00 * a22 - a02 * a20) * inv;
    out.m[2][1] = (a02 * a10 - a00 * a12) * inv;
    out.m[0][2] = c02 * inv;
    out.m[1][2] = (a01 * a20 - a00 * a21) * inv;
    out.m[2][2] = (a00 * a11 - a01 * a10) * inv;
    return out;
}

// One end of a mover: the body, as the solver has it now.
struct End
{
    physics::BodyHandle handle;
    physics::BodyState state;
    physics::BodyMassProperties mass;
    // The attachment, in the world.
    core::DVec3 point;
    Mat3 rotation;
    bool present = false;
    bool dynamic = false;

    [[nodiscard]] Vec3 velocityAt(core::DVec3 where) const noexcept
    {
        if (!present)
            return Vec3{0.0f, 0.0f, 0.0f};
        return state.linearVelocity + core::cross(state.angularVelocity, core::toVec3(where - mass.centerOfMass));
    }

    // What an impulse along `axis` at `where` does to the speed there, for
    // each unit of it: one over the mass the point seems to have that way.
    [[nodiscard]] f32 give(core::DVec3 where, Vec3 axis) const noexcept
    {
        if (!dynamic || mass.mass <= 0.0f)
            return 0.0f;
        const Vec3 arm = core::toVec3(where - mass.centerOfMass);
        const Vec3 turn = core::cross(arm, axis);
        return 1.0f / mass.mass + core::dot(turn, mass.inverseInertia * turn);
    }
};

// The impulse at `where` that changes the speed THERE by `change`: the mass a
// point seems to have is not the body's -- a push at the end of a plank also
// turns it -- and a mover that used the body's would overshoot every tick.
[[nodiscard]] Vec3 impulseFor(const End& end, core::DVec3 where, Vec3 change) noexcept
{
    const Vec3 arm = core::toVec3(where - end.mass.centerOfMass);
    // K = E / m - [arm]x * Iinv * [arm]x, built a column at a time.
    Mat3 k;
    for (u32 column = 0; column < 3; ++column) {
        Vec3 unit{0.0f, 0.0f, 0.0f};
        (column == 0 ? unit.x : column == 1 ? unit.y : unit.z) = 1.0f;
        const Vec3 turned = core::cross(end.mass.inverseInertia * core::cross(arm, unit), arm);
        const Vec3 result = unit * (1.0f / end.mass.mass) + turned;
        k.m[column][0] = result.x;
        k.m[column][1] = result.y;
        k.m[column][2] = result.z;
    }
    bool ok = false;
    const Mat3 inverse = inverseOf(k, ok);
    return ok ? inverse * change : change * end.mass.mass;
}

// The angular impulse that changes the spin by `change`.
[[nodiscard]] Vec3 twistFor(const End& end, Vec3 change) noexcept
{
    bool ok = false;
    const Mat3 inertia = inverseOf(end.mass.inverseInertia, ok);
    return ok ? inertia * change : Vec3{0.0f, 0.0f, 0.0f};
}

// How a spring changes a speed over one tick, solved for the speed at the END
// of the tick rather than the start: a stiff spring stepped from where it is
// now overshoots and gains energy, and this cannot.
//   m (v' - v) = dt k (x - v' dt) - dt c v'
[[nodiscard]] f32 springGain(f32 mass, f32 stiffness, f32 damping, f32 dt) noexcept
{
    return 1.0f / (mass + dt * damping + dt * dt * stiffness);
}

} // namespace

void PhysicsSync::applyMovers(f32 fixedDt)
{
    if (fixedDt <= 0.0f || (m_scene.movers().size() == 0 && !m_anySpring))
        return;
    // What the solver is about to add to every moving body's velocity by
    // itself. A mover that HOLDS a velocity answers it in the same tick, or it
    // would always be one tick of falling behind.
    Vec3 falling{0.0f, 0.0f, 0.0f};
    if (const WorkspaceComponent* workspace = m_scene.workspaces().find(m_workspace); workspace != nullptr)
        falling = workspace->gravity * fixedDt;

    const auto endOf = [this](core::InstanceId attachment) {
        End end;
        const AttachmentComponent* frame = m_scene.attachments().find(attachment);
        const core::InstanceId part = m_scene.parentOf(attachment);
        if (frame == nullptr || !part.valid() || m_scene.characterBodies().find(part) != nullptr)
            return end;
        end.handle = bodyHandleOf(part);
        if (!end.handle.valid())
            return end;
        end.state = m_backend.bodyState(m_world, end.handle);
        end.mass = m_backend.bodyMassProperties(m_world, end.handle);
        const core::CFrameD world = end.state.transform * frame->cframe;
        end.point = world.position;
        end.rotation = world.rotation;
        end.present = true;
        end.dynamic = end.mass.dynamic;
        return end;
    };
    const auto push = [this](const End& end, Vec3 impulse, core::DVec3 where) {
        if (end.dynamic && lengthOf(impulse) > kQuietImpulse)
            m_backend.applyImpulseAt(m_world, end.handle, impulse, where);
    };
    const auto pushCentre = [this](const End& end, Vec3 impulse) {
        if (end.dynamic && lengthOf(impulse) > kQuietImpulse)
            m_backend.applyImpulse(m_world, end.handle, impulse);
    };
    const auto twist = [this](const End& end, Vec3 impulse) {
        if (end.dynamic && lengthOf(impulse) > kQuietImpulse)
            m_backend.applyAngularImpulse(m_world, end.handle, impulse);
    };

    m_scene.constraints().forEach([&](core::InstanceId id, const ConstraintComponent& constraint) {
        const bool spring =
            constraint.kind == static_cast<i32>(physics::ConstraintType::Distance) && constraint.flavor == 2;
        if ((!spring && constraint.kind < MoverKind::LinearVelocity) || !constraint.enabled || !inWorld(id))
            return;

        const End first = endOf(constraint.attachment0);
        if (!first.present)
            return;

        // --- A spring: the two ends, and a force along the line between them.
        if (spring) {
            const End second = endOf(constraint.attachment1);
            if (!second.present || first.handle == second.handle)
                return;
            const Vec3 apart = core::toVec3(second.point - first.point);
            const f32 length = lengthOf(apart);
            if (length < 1.0e-5f)
                return;
            const Vec3 along = apart * (1.0f / length);
            const f32 give = first.give(first.point, along) + second.give(second.point, along);
            if (give <= 0.0f)
                return;
            const f32 mass = 1.0f / give;
            const f32 stretch = length - constraint.length;
            const f32 opening = core::dot(second.velocityAt(second.point) - first.velocityAt(first.point), along);
            const f32 gain = springGain(mass, constraint.stiffness, constraint.damping, fixedDt);
            const f32 change = -(fixedDt * constraint.stiffness * stretch +
                                 (fixedDt * constraint.damping + fixedDt * fixedDt * constraint.stiffness) * opening) *
                               gain;
            const Vec3 impulse = along * (mass * change);
            push(second, impulse, second.point);
            push(first, impulse * -1.0f, first.point);
            return;
        }

        const MoverComponent* mover = m_scene.movers().find(id);
        if (mover == nullptr || !first.dynamic)
            return;
        const End second = endOf(constraint.attachment1);

        // The frame a mover's vector is written in.
        const auto inFrame = [&](Vec3 v) {
            if (mover->relativeTo == 0)
                return first.rotation * v;
            if (mover->relativeTo == 1 && second.present)
                return second.rotation * v;
            return v;
        };

        switch (constraint.kind) {
        case MoverKind::LinearVelocity: {
            const Vec3 velocity = first.state.linearVelocity + falling;
            Vec3 change{0.0f, 0.0f, 0.0f};
            if (mover->mode == 0) {
                const Vec3 line = normalizedOr(inFrame(mover->lineDirection), Vec3{1.0f, 0.0f, 0.0f});
                change = line * (mover->lineVelocity - core::dot(velocity, line));
            }
            else if (mover->mode == 1) {
                const Vec3 a = normalizedOr(inFrame(mover->primaryTangentAxis), Vec3{1.0f, 0.0f, 0.0f});
                const Vec3 raw = inFrame(mover->secondaryTangentAxis);
                const Vec3 b = normalizedOr(raw - a * core::dot(raw, a), Vec3{0.0f, 0.0f, 1.0f});
                change = a * (mover->planeVelocity.x - core::dot(velocity, a)) +
                         b * (mover->planeVelocity.y - core::dot(velocity, b));
            }
            else {
                change = inFrame(mover->vector) - velocity;
            }
            pushCentre(first, clamped(change * first.mass.mass, mover->maxForce * fixedDt));
            break;
        }
        case MoverKind::AngularVelocity: {
            const Vec3 change = inFrame(mover->vector) - first.state.angularVelocity;
            const Vec3 impulse = clamped(twistFor(first, change), mover->maxTorque * fixedDt);
            twist(first, impulse);
            if (mover->reactionEnabled && second.present)
                twist(second, impulse * -1.0f);
            break;
        }
        case MoverKind::AlignPosition: {
            const bool two = mover->mode == 1;
            if (two && !second.present)
                break;
            const core::DVec3 from = mover->applyAtCenterOfMass ? first.mass.centerOfMass : first.point;
            const core::DVec3 target = two ? second.point : mover->position;
            const Vec3 error = core::toVec3(target - from);
            Vec3 closing = first.velocityAt(from) - (two ? second.velocityAt(target) : Vec3{0.0f, 0.0f, 0.0f});
            // A spring is left to find its own balance against the weight; a
            // pull that holds a place answers the weight as it comes.
            if (mover->rigidityEnabled || !(mover->stiffness > 0.0f)) {
                closing = closing + falling;
                if (two && second.dynamic)
                    closing = closing - falling;
            }
            Vec3 change{0.0f, 0.0f, 0.0f};
            if (mover->rigidityEnabled) {
                change = error * (1.0f / fixedDt) - closing;
            }
            else if (mover->stiffness > 0.0f) {
                const f32 distance = lengthOf(error);
                const Vec3 along = distance > 1.0e-6f ? error * (1.0f / distance) : Vec3{0.0f, 1.0f, 0.0f};
                const f32 give = first.give(from, along);
                const f32 mass = give > 0.0f ? 1.0f / give : first.mass.mass;
                const f32 gain = springGain(mass, mover->stiffness, mover->damping, fixedDt);
                change = (error * (fixedDt * mover->stiffness) -
                          closing * (fixedDt * mover->damping + fixedDt * fixedDt * mover->stiffness)) *
                         gain;
            }
            else {
                const f32 eager = std::min(mover->responsiveness, 1.0f / fixedDt);
                change = clamped(error * eager, mover->maxVelocity) - closing;
            }
            Vec3 impulse = impulseFor(first, from, change);
            if (!mover->rigidityEnabled)
                impulse = clamped(impulse, mover->maxForce * fixedDt);
            push(first, impulse, from);
            if (two && mover->reactionEnabled)
                push(second, impulse * -1.0f, target);
            break;
        }
        case MoverKind::AlignOrientation: {
            const bool two = mover->mode == 1;
            if (two && !second.present)
                break;
            const Mat3 target = two ? second.rotation : mover->orientation;
            // From where it is to where it should be, as one turn: its axis
            // times its angle.
            Vec3 axis{0.0f, 1.0f, 0.0f};
            f32 angle = 0.0f;
            core::toAxisAngle(target * core::transpose(first.rotation), axis, angle);
            if (angle > kPi)
                angle -= 2.0f * kPi;
            const Vec3 error = axis * angle;
            const Vec3 spin =
                first.state.angularVelocity - (two ? second.state.angularVelocity : Vec3{0.0f, 0.0f, 0.0f});
            Vec3 change{0.0f, 0.0f, 0.0f};
            if (mover->rigidityEnabled) {
                change = error * (1.0f / fixedDt) - spin;
            }
            else if (mover->stiffness > 0.0f) {
                const f32 give = core::dot(axis, first.mass.inverseInertia * axis);
                const f32 inertia = give > 0.0f ? 1.0f / give : 1.0f;
                const f32 gain = springGain(inertia, mover->stiffness, mover->damping, fixedDt);
                change = (error * (fixedDt * mover->stiffness) -
                          spin * (fixedDt * mover->damping + fixedDt * fixedDt * mover->stiffness)) *
                         gain;
            }
            else {
                const f32 eager = std::min(mover->responsiveness, 1.0f / fixedDt);
                change = clamped(error * eager, mover->maxAngularVelocity) - spin;
            }
            Vec3 impulse = twistFor(first, change);
            if (!mover->rigidityEnabled)
                impulse = clamped(impulse, mover->maxTorque * fixedDt);
            twist(first, impulse);
            if (two && mover->reactionEnabled)
                twist(second, impulse * -1.0f);
            break;
        }
        case MoverKind::VectorForce: {
            const Vec3 impulse = inFrame(mover->vector) * fixedDt;
            if (mover->applyAtCenterOfMass)
                pushCentre(first, impulse);
            else
                push(first, impulse, first.point);
            break;
        }
        case MoverKind::Torque:
            twist(first, inFrame(mover->vector) * fixedDt);
            break;
        default:
            break;
        }
    });
}

// What a joint's actuator asks of the solver this tick (ADR 0127 section 2,
// amendments A1 and A3), written into the description the joint is built or
// driven with.
void PhysicsSync::motorOf(const ConstraintComponent& constraint, const ConstraintRecord& record, f32 fixedDt,
                          physics::ConstraintDesc& desc) const
{
    desc.motor = physics::MotorMode::Off;
    if (!constraint.enabled || constraint.actuatorType == 0)
        return;

    const auto point = static_cast<i32>(physics::ConstraintType::Point);
    const auto swingTwist = static_cast<i32>(physics::ConstraintType::SwingTwist);
    const auto hinge = static_cast<i32>(physics::ConstraintType::Hinge);
    const auto slider = static_cast<i32>(physics::ConstraintType::Slider);

    if (constraint.kind == point || constraint.kind == swingTwist) {
        // A ball has no axis to spin about: only the pose.
        if (constraint.actuatorType != 2)
            return;
        desc.motor = physics::MotorMode::Position;
        desc.motorOrientation = constraint.targetOrientation;
        desc.motorMaxForce = constraint.servoMaxForce;
        if (constraint.stiffness > 0.0f) {
            desc.motorStiffness = constraint.stiffness;
            desc.motorDamping = constraint.damping;
        }
        else {
            // Settles without ringing; how soon is the responsiveness.
            desc.motorFrequency = std::max(constraint.responsiveness * 0.1f, 0.1f);
            desc.motorDampingRatio = 1.0f;
        }
        return;
    }
    if (constraint.kind != hinge && constraint.kind != slider)
        return;

    const bool live = record.generation != 0 && record.handle.valid();
    if (constraint.actuatorType == 1) {
        f32 target = constraint.motorVelocity;
        // Wound up, not switched on: no further from the speed it has than the
        // acceleration allows in one tick. Measured, so there is nothing to
        // remember.
        if (constraint.motorMaxAcceleration < 100000.0f) {
            // A joint not built yet is at rest: the first tick is wound up too.
            const f32 now = live ? m_backend.constraintState(m_world, record.handle).velocity : 0.0f;
            const f32 most = constraint.motorMaxAcceleration * fixedDt;
            target = std::clamp(target, now - most, now + most);
        }
        desc.motor = physics::MotorMode::Velocity;
        desc.motorTarget = target;
        desc.motorMaxForce = constraint.motorMaxForce;
        return;
    }

    const f32 goal = constraint.kind == hinge ? constraint.servoTarget * kDegreesToRadians : constraint.servoTarget;
    desc.motorMaxForce = constraint.servoMaxForce;
    if (constraint.stiffness > 0.0f) {
        // A spring to the target: the solver's own position motor.
        desc.motor = physics::MotorMode::Position;
        desc.motorTarget = goal;
        desc.motorStiffness = constraint.stiffness;
        desc.motorDamping = constraint.damping;
        return;
    }
    // Otherwise a speed towards it, no faster than the servo's own, that falls
    // to nothing as it arrives -- and then holds, because a motor told to stay
    // still is a brake.
    f32 error = goal - (live ? m_backend.constraintState(m_world, record.handle).position : 0.0f);
    if (constraint.kind == hinge) {
        while (error > kPi)
            error -= 2.0f * kPi;
        while (error < -kPi)
            error += 2.0f * kPi;
    }
    const f32 eager = std::min(constraint.responsiveness, 1.0f / fixedDt);
    f32 speed = std::clamp(error * eager, -constraint.servoSpeed, constraint.servoSpeed);
    if (std::fabs(error) < 1.0e-4f)
        speed = 0.0f;
    desc.motor = physics::MotorMode::Velocity;
    desc.motorTarget = speed;
}

void PhysicsSync::applyNoCollisions()
{
    if (m_scene.noCollisions().size() == 0 && m_noCollisions.empty())
        return;
    for (NoCollisionRecord& record : m_noCollisions)
        record.seen = false;

    m_scene.noCollisions().forEach([&](core::InstanceId id, const NoCollisionComponent& pair) {
        if (id.index >= m_noCollisions.size())
            m_noCollisions.resize(id.index + 1);
        NoCollisionRecord& record = m_noCollisions[id.index];

        const physics::BodyHandle first = bodyHandleOf(pair.part0);
        const physics::BodyHandle second = bodyHandleOf(pair.part1);
        const bool wanted = pair.enabled && inWorld(id) && first.valid() && second.valid() && pair.part0 != pair.part1;

        // What is excluded is a pair of BODIES: a part that was reshaped keeps
        // its body, and one that was replaced is another pair.
        const bool same =
            record.applied && record.generation == id.generation && record.first == first && record.second == second;
        if (record.applied && (!wanted || !same)) {
            m_backend.setPairCollidable(m_world, record.first, record.second, true);
            record.applied = false;
        }
        if (wanted && !record.applied) {
            m_backend.setPairCollidable(m_world, first, second, false);
            record.applied = true;
            record.first = first;
            record.second = second;
        }
        record.generation = id.generation;
        record.seen = true;
    });

    for (NoCollisionRecord& record : m_noCollisions) {
        if (record.seen || !record.applied)
            continue;
        m_backend.setPairCollidable(m_world, record.first, record.second, true);
        record = NoCollisionRecord{};
    }
}

// After the step: what each joint carried, and whether that was more than it
// can (N3).
void PhysicsSync::readConstraints(f32 fixedDt)
{
    if (fixedDt <= 0.0f)
        return;
    const core::NameAtom broken = m_scene.atoms().intern("Broken");
    m_scene.constraints().forEach([&](core::InstanceId id, ConstraintComponent& constraint) {
        if (id.index >= m_constraints.size())
            return;
        const ConstraintRecord& record = m_constraints[id.index];
        if (record.generation != id.generation || !record.handle.valid() || !constraint.enabled) {
            constraint.lastForce = 0.0f;
            constraint.lastTorque = 0.0f;
            return;
        }
        const physics::ConstraintState state = m_backend.constraintState(m_world, record.handle);
        constraint.lastForce = state.appliedImpulse / fixedDt;
        constraint.lastTorque = state.appliedAngularImpulse / fixedDt;
        const bool gave = (constraint.breakForce > 0.0f && constraint.lastForce > constraint.breakForce) ||
                          (constraint.breakTorque > 0.0f && constraint.lastTorque > constraint.breakTorque);
        if (!gave)
            return;
        // Disabled, not destroyed: it keeps its place in the solve order, and
        // enabling it again mends it.
        constraint.enabled = false;
        if (m_quiet)
            return;
        Change change;
        change.kind = ChangeKind::InstanceEventNoArgs;
        change.name = broken;
        change.subject = id;
        m_scene.changes().push(change);
    });
}

} // namespace engine::scene
