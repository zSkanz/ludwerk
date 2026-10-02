// Movers and what a joint does of itself (ADR 0127): the half of the physics
// mirror that turns "hold this speed" and "go to that place" into a motor the
// solver solves with everything else a body is held by, and "push this hard"
// and "a spring this stiff" into impulses before each step.
//
// **Everything here is a function of the world as the step finds it**: the
// bodies' positions and velocities from the solver, the instances' properties,
// and the tick's length. Nothing is remembered between ticks -- a servo's error
// is measured, a winch's rope is the `Length` property itself -- so a rollback
// that restores the world restores every one of these with it, and a replay
// asks the same of the solver.
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

// Three axes at right angles from one or two directions, as a rotation whose
// columns they are: what a line or a plane a speed is held along looks like
// to a motor that pushes along axes.
[[nodiscard]] Mat3 basisOf(Vec3 first, Vec3 second) noexcept
{
    const Vec3 x = normalizedOr(first, Vec3{1.0f, 0.0f, 0.0f});
    // Whatever is least along X, so the choice does not flip as X turns a
    // little.
    Vec3 hint = second - x * core::dot(second, x);
    if (lengthOf(hint) < 1.0e-5f) {
        const Vec3 other = std::fabs(x.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        hint = other - x * core::dot(other, x);
    }
    const Vec3 y = normalizedOr(hint, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 z = core::cross(x, y);
    Mat3 out;
    out.m[0][0] = x.x, out.m[0][1] = x.y, out.m[0][2] = x.z;
    out.m[1][0] = y.x, out.m[1][1] = y.y, out.m[1][2] = y.z;
    out.m[2][0] = z.x, out.m[2][1] = z.y, out.m[2][2] = z.z;
    return out;
}

// A frequency no tick can show more of: what "rigid" is as a spring. Stepped
// the way the solver steps a spring, it closes most of what is left every
// tick and cannot gain energy however heavy the body.
[[nodiscard]] f32 rigidFrequency(f32 dt) noexcept
{
    return 0.5f / dt;
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
        if ((!spring && constraint.kind < MoverKind::VectorForce) || !constraint.enabled || !inWorld(id))
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

// **A mover that holds something** (D471). It was an impulse worked out before
// the step from the body alone, which is right for a body nothing else holds
// and wrong for every other: a limb pulled to a pose was pushed as if its
// joints were not there, the joints pushed back a tick later, and the two
// never agreed. A motor in the solver is solved WITH the joints, in the same
// iterations, and settles as the spring it says it is.
//
// The moved body is always the drive's second, and its first is the world or
// -- when what it does to one body it does, reversed, to the other -- the
// body at its other end.
void PhysicsSync::applyDrive(core::InstanceId id, const ConstraintComponent& constraint)
{
    const MoverComponent* mover = m_scene.movers().find(id);
    const AttachmentComponent* end0 = m_scene.attachments().find(constraint.attachment0);
    const core::InstanceId moved = m_scene.parentOf(constraint.attachment0);
    if (mover == nullptr || end0 == nullptr || !moved.valid() || !inWorld(id) ||
        m_scene.characterBodies().find(moved) != nullptr)
        return;
    const physics::BodyHandle movedHandle = bodyHandleOf(moved);
    if (!movedHandle.valid())
        return;
    // Only what the solver moves can be driven: an anchored part has a body
    // and nothing to push.
    const physics::BodyMassProperties mass = m_backend.bodyMassProperties(m_world, movedHandle);
    if (!mass.dynamic)
        return;

    // The other end, where there is one: a body of its own, or -- a character,
    // an attachment on something with no body -- only a place.
    const AttachmentComponent* end1 = m_scene.attachments().find(constraint.attachment1);
    const core::InstanceId other = end1 != nullptr ? m_scene.parentOf(constraint.attachment1) : core::InstanceId{};
    const PartComponent* otherPart = other.valid() ? m_scene.parts().find(other) : nullptr;
    const bool hasOther = end1 != nullptr && otherPart != nullptr && other != moved;
    const physics::BodyHandle otherHandle =
        hasOther && m_scene.characterBodies().find(other) == nullptr ? bodyHandleOf(other) : physics::BodyHandle{};

    const physics::BodyState state = m_backend.bodyState(m_world, movedHandle);
    const core::CFrameD here = state.transform * end0->cframe;
    core::CFrameD there;
    Vec3 thereVelocity{0.0f, 0.0f, 0.0f};
    Vec3 thereSpin{0.0f, 0.0f, 0.0f};
    if (otherHandle.valid()) {
        const physics::BodyState otherState = m_backend.bodyState(m_world, otherHandle);
        const physics::BodyMassProperties otherMass = m_backend.bodyMassProperties(m_world, otherHandle);
        there = otherState.transform * end1->cframe;
        thereSpin = otherState.angularVelocity;
        thereVelocity = otherState.linearVelocity +
                        core::cross(otherState.angularVelocity, core::toVec3(there.position - otherMass.centerOfMass));
    }
    else if (hasOther) {
        there = otherPart->cframe * end1->cframe;
    }

    const f32 fixedDt = static_cast<f32>(m_scene.engineState().fixedTimestep);
    // The frame a mover's vector is written in.
    const auto inFrame = [&](Vec3 v) {
        if (mover->relativeTo == 0)
            return here.rotation * v;
        if (mover->relativeTo == 1 && hasOther)
            return there.rotation * v;
        return v;
    };

    physics::ConstraintDesc desc;
    desc.type = physics::ConstraintType::Drive;
    desc.second = movedHandle;
    desc.secondFrame = end0->cframe;
    // The instance, as every body and joint the mirror makes names its own.
    desc.userData = (static_cast<u64>(id.generation) << 32) | static_cast<u64>(id.index);
    physics::DriveDesc& drive = desc.drive;
    core::InstanceId first;

    // The spring a pull is: as rigid as a tick can show, or the two numbers
    // that say a spring in its own terms, or -- neither given -- critically
    // damped at `Responsiveness` radians a second whatever is being moved.
    const f32 natural = std::max(mover->responsiveness, 0.01f);
    const bool plain = !mover->rigidityEnabled && !(mover->stiffness > 0.0f);

    switch (constraint.kind) {
    case MoverKind::AlignPosition: {
        const bool two = mover->mode == 1;
        if (two && !hasOther)
            return;
        drive.atCenterOfMass = mover->applyAtCenterOfMass;
        const core::DVec3 from = mover->applyAtCenterOfMass ? mass.centerOfMass : here.position;
        const core::DVec3 target = two ? there.position : mover->position;
        // **No faster than its speed** -- and AT its speed, until it is near.
        // A critically damped spring let go a distance out at a speed towards
        // its target arrives without crossing it when that distance is two
        // speeds over its frequency, so further out than that the mover
        // travels at its speed and nearer it is the spring.
        const Vec3 gap = core::toVec3(target - from);
        const bool travelling =
            plain && mover->maxVelocity < 100000.0f && lengthOf(gap) > 2.0f * mover->maxVelocity / natural;
        const Vec3 cruise = travelling ? gap * (mover->maxVelocity / lengthOf(gap)) : Vec3{0.0f, 0.0f, 0.0f};

        if (two && mover->reactionEnabled && otherHandle.valid()) {
            // Between the two: what pulls the one to the other pulls the
            // other to the one. The target is the other's own attachment,
            // which in its frame is nowhere at all.
            first = other;
            desc.first = otherHandle;
            desc.firstFrame = end1->cframe;
            drive.linearVelocity = core::transpose(there.rotation) * cruise;
        }
        else {
            drive.linearTarget = target;
            // What it follows may itself be moving, and is followed at that
            // speed rather than trailed.
            drive.linearVelocity = cruise + (two ? thereVelocity : Vec3{0.0f, 0.0f, 0.0f});
        }
        drive.linear[0] = drive.linear[1] = drive.linear[2] =
            travelling ? physics::MotorMode::Velocity : physics::MotorMode::Position;
        if (mover->rigidityEnabled) {
            drive.linearFrequency = rigidFrequency(fixedDt);
        }
        else {
            drive.linearMaxForce = mover->maxForce;
            drive.linearStiffness = std::max(mover->stiffness, 0.0f);
            drive.linearDamping = mover->damping;
            drive.linearFrequency = natural / (2.0f * kPi);
        }
        break;
    }
    case MoverKind::AlignOrientation: {
        const bool two = mover->mode == 1;
        if (two && !hasOther)
            return;
        const Mat3 target = two ? there.rotation : mover->orientation;
        // The same rule as a turn: at its speed until it is near, then the
        // spring.
        Vec3 cruise{0.0f, 0.0f, 0.0f};
        bool travelling = false;
        if (plain && mover->maxAngularVelocity < 100000.0f) {
            Vec3 axis{0.0f, 1.0f, 0.0f};
            f32 angle = 0.0f;
            core::toAxisAngle(target * core::transpose(here.rotation), axis, angle);
            travelling = angle > 2.0f * mover->maxAngularVelocity / natural;
            if (travelling)
                cruise = axis * mover->maxAngularVelocity;
        }
        const bool between = two && mover->reactionEnabled && otherHandle.valid();
        if (between) {
            first = other;
            desc.first = otherHandle;
            desc.firstFrame = end1->cframe;
        }
        else {
            drive.angularTarget = target;
        }
        // A spin is said in the world's axes whatever the drive is against;
        // against the world, what it follows may itself be turning.
        drive.angularVelocity = cruise + (two && !between ? thereSpin : Vec3{0.0f, 0.0f, 0.0f});
        drive.angular = travelling ? physics::MotorMode::Velocity : physics::MotorMode::Position;
        if (mover->rigidityEnabled) {
            drive.angularFrequency = rigidFrequency(fixedDt);
        }
        else {
            drive.angularMaxTorque = mover->maxTorque;
            drive.angularStiffness = std::max(mover->stiffness, 0.0f);
            drive.angularDamping = mover->damping;
            drive.angularFrequency = natural / (2.0f * kPi);
        }
        break;
    }
    case MoverKind::LinearVelocity: {
        // The body's own speed, which is the speed of where it balances: a
        // push anywhere else would turn it as well.
        drive.atCenterOfMass = true;
        drive.linearMaxForce = mover->maxForce;
        if (mover->mode == 0) {
            desc.firstFrame.rotation = basisOf(inFrame(mover->lineDirection), Vec3{0.0f, 0.0f, 0.0f});
            drive.linear[0] = physics::MotorMode::Velocity;
            drive.linearVelocity = Vec3{mover->lineVelocity, 0.0f, 0.0f};
        }
        else if (mover->mode == 1) {
            desc.firstFrame.rotation =
                basisOf(inFrame(mover->primaryTangentAxis), inFrame(mover->secondaryTangentAxis));
            drive.linear[0] = drive.linear[1] = physics::MotorMode::Velocity;
            drive.linearVelocity = Vec3{mover->planeVelocity.x, mover->planeVelocity.y, 0.0f};
        }
        else {
            drive.linear[0] = drive.linear[1] = drive.linear[2] = physics::MotorMode::Velocity;
            drive.linearVelocity = inFrame(mover->vector);
        }
        break;
    }
    case MoverKind::AngularVelocity: {
        if (mover->reactionEnabled && otherHandle.valid()) {
            first = other;
            desc.first = otherHandle;
            desc.firstFrame = end1->cframe;
        }
        drive.angular = physics::MotorMode::Velocity;
        drive.angularVelocity = inFrame(mover->vector);
        drive.angularMaxTorque = mover->maxTorque;
        break;
    }
    default:
        return;
    }

    if (id.index >= m_constraints.size())
        m_constraints.resize(id.index + 1);
    ConstraintRecord& record = m_constraints[id.index];

    // What the drive IS: its bodies, its frames, and where on the body it
    // holds. The rest -- the targets, the spring, the cap -- is driven.
    const bool rebuild = record.generation != id.generation || record.kind != constraint.kind ||
                         record.body0 != first || record.body1 != moved || !(record.handle0 == desc.first) ||
                         !(record.handle1 == desc.second) || !(record.frame0 == desc.firstFrame) ||
                         !(record.frame1 == desc.secondFrame) || record.atCenter != drive.atCenterOfMass;
    if (rebuild) {
        if (record.generation != 0)
            m_backend.destroyConstraint(m_world, record.handle);
        record = ConstraintRecord{};
        record.handle = m_backend.createConstraint(m_world, desc);
        if (!record.handle.valid())
            return;
        record.generation = id.generation;
    }
    else {
        m_backend.driveConstraint(m_world, record.handle, desc);
    }
    if (record.enabled != constraint.enabled) {
        m_backend.setConstraintEnabled(m_world, record.handle, constraint.enabled);
        record.enabled = constraint.enabled;
    }

    record.seen = true;
    record.kind = constraint.kind;
    record.body0 = first;
    record.body1 = moved;
    record.handle0 = desc.first;
    record.handle1 = desc.second;
    record.frame0 = desc.firstFrame;
    record.frame1 = desc.secondFrame;
    record.atCenter = drive.atCenterOfMass;
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
            // Critically damped, as every servo is: the responsiveness is its
            // natural frequency in radians a second.
            desc.motorFrequency = std::max(constraint.responsiveness, 0.01f) / (2.0f * kPi);
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

    // **A servo is a critically damped spring to its target, by construction**
    // (D470): its responsiveness is the spring's natural frequency in radians
    // a second, handed to the solver's motor in the form that does not depend
    // on what is being moved -- so the same number settles the same way on a
    // finger and on a crane, and never rings. It was a speed proportional to
    // the error, which over a real inertia under a finite cap arrived too fast
    // to brake and swung round its target for ever.
    const f32 goal = constraint.kind == hinge ? constraint.servoTarget * kDegreesToRadians : constraint.servoTarget;
    desc.motor = physics::MotorMode::Position;
    desc.motorMaxForce = constraint.servoMaxForce;
    desc.motorTarget = goal;
    if (constraint.stiffness > 0.0f) {
        // The spring said in its own two terms: how firm and how damped.
        desc.motorStiffness = constraint.stiffness;
        desc.motorDamping = constraint.damping;
        return;
    }
    const f32 natural = std::max(constraint.responsiveness, 0.01f);
    desc.motorFrequency = natural / (2.0f * kPi);
    desc.motorDampingRatio = 1.0f;
    // **No faster than its speed, and at its speed until it is near.** A
    // critically damped spring let go a distance out, at a speed towards its
    // target, arrives without crossing it when that distance is two speeds
    // over its frequency: further out than that the servo is a motor at its
    // speed, and nearer it is the spring.
    if (constraint.servoSpeed < 100000.0f) {
        const f32 now = live ? m_backend.constraintState(m_world, record.handle).position : 0.0f;
        f32 error = goal - now;
        if (constraint.kind == hinge) {
            while (error > kPi)
                error -= 2.0f * kPi;
            while (error < -kPi)
                error += 2.0f * kPi;
        }
        if (std::fabs(error) > 2.0f * constraint.servoSpeed / natural) {
            desc.motor = physics::MotorMode::Velocity;
            desc.motorTarget = error > 0.0f ? constraint.servoSpeed : -constraint.servoSpeed;
        }
    }
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
            constraint.lastMotorForce = 0.0f;
            constraint.lastMotorTorque = 0.0f;
            return;
        }
        const physics::ConstraintState state = m_backend.constraintState(m_world, record.handle);
        constraint.lastForce = state.appliedImpulse / fixedDt;
        constraint.lastTorque = state.appliedAngularImpulse / fixedDt;
        constraint.lastMotorForce = state.motorImpulse / fixedDt;
        constraint.lastMotorTorque = state.motorAngularImpulse / fixedDt;
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
