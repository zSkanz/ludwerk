// The Jolt backend (ADR 0007, ADR 0023).
//
// The whole backend is one translation unit and it has no public header, which
// is what keeps R17 mechanical rather than aspirational: no module above L2 can
// include a JPH type because there is no file of ours to include that names
// one.
//
// Three things in here exist because of R10 rather than because Jolt needs
// them, and upstream's own documentation is why (Docs/Architecture.md:804-807):
// contact callbacks arrive in a non-deterministic order, the active-body list
// is in a non-deterministic order, and a query's hits arrive in a
// non-deterministic order. All three are true today only in the sense that they
// WILL be true when M7 wires a multi-threaded job system -- with the
// single-threaded one they happen to be stable. Writing the sorts now is what
// stops M7 from being the milestone that discovers a thousand recorded traces
// are worthless.
//
// Jolt requires Jolt.h before any other Jolt header -- its own headers say so
// and none of them include it -- so this block is exempt from include sorting.
// clang-format off
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>
#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#endif
// clang-format on

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/physics/backends.h"

namespace engine::physics {

// The fastest a body spins, in radians a second (K1).
constexpr float MaxAngularVelocity = 500.0f;
namespace {

// **What a terrain chunk's band triangles are marked with** (ADR 0143): 1 in
// the triangle's user data. The band only lends its edges.
constexpr JPH::uint32 kBandTriangle = 1;

using core::f32;
using core::f64;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// --- Layer encoding ---------------------------------------------------------
//
// One object layer per (collision group, moving) pair: `layer = group * 2 +
// moving`. Ten bits of group and one of motion fit inside the 16-bit object
// layer Jolt is built with, which is what caps a world at
// `kMaxCollisionGroups`.
//
// The split by motion is not ours; it is what lets the broad phase keep static
// geometry in a tree it never rebuilds. Everything else about which pairs
// collide is a runtime matrix, because `PhysicsService:RegisterCollisionGroup`
// is a call a script makes after the world exists.

constexpr JPH::BroadPhaseLayer kBroadPhaseNonMoving(0);
constexpr JPH::BroadPhaseLayer kBroadPhaseMoving(1);
constexpr JPH::uint kBroadPhaseLayerCount = 2;

[[nodiscard]] JPH::ObjectLayer encodeLayer(CollisionGroup group, bool moving) noexcept
{
    return static_cast<JPH::ObjectLayer>((static_cast<u32>(group) << 1) | (moving ? 1u : 0u));
}

[[nodiscard]] CollisionGroup decodeGroup(JPH::ObjectLayer layer) noexcept
{
    return static_cast<CollisionGroup>(static_cast<u32>(layer) >> 1);
}

[[nodiscard]] bool decodeMoving(JPH::ObjectLayer layer) noexcept
{
    return (static_cast<u32>(layer) & 1u) != 0u;
}

// The collidability matrix, dense and square, one byte per pair. A world with
// the four groups a game actually registers costs sixteen bytes; the bound is
// what stops a script from asking for a megabyte by looping.
class CollisionMatrix
{
public:
    CollisionMatrix()
    {
        m_names.emplace_back("Default");
        m_collidable.assign(1, 1);
    }

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_names.size()); }

    [[nodiscard]] CollisionGroup find(std::string_view name) const noexcept
    {
        for (usize i = 0; i < m_names.size(); ++i) {
            if (m_names[i] == name) {
                return static_cast<CollisionGroup>(i);
            }
        }
        return kInvalidGroup;
    }

    [[nodiscard]] CollisionGroup add(std::string_view name)
    {
        const CollisionGroup existing = find(name);
        if (existing != kInvalidGroup) {
            return existing;
        }
        if (m_names.size() >= kMaxCollisionGroups) {
            return kInvalidGroup;
        }

        const u32 previous = count();
        const u32 next = previous + 1;
        std::vector<u8> grown(static_cast<usize>(next) * next, 1);
        for (u32 row = 0; row < previous; ++row) {
            for (u32 column = 0; column < previous; ++column) {
                grown[static_cast<usize>(row) * next + column] =
                    m_collidable[static_cast<usize>(row) * previous + column];
            }
        }
        m_collidable.swap(grown);
        m_names.emplace_back(name);
        return static_cast<CollisionGroup>(previous);
    }

    void setCollidable(CollisionGroup a, CollisionGroup b, bool collidable) noexcept
    {
        if (a >= count() || b >= count()) {
            return;
        }
        m_collidable[static_cast<usize>(a) * count() + b] = collidable ? 1u : 0u;
        m_collidable[static_cast<usize>(b) * count() + a] = collidable ? 1u : 0u;
    }

    [[nodiscard]] bool collidable(CollisionGroup a, CollisionGroup b) const noexcept
    {
        if (a >= count() || b >= count()) {
            return false;
        }
        return m_collidable[static_cast<usize>(a) * count() + b] != 0u;
    }

    void collectNames(std::vector<std::string_view>& out) const
    {
        for (const std::string& name : m_names) {
            out.emplace_back(name);
        }
    }

    static constexpr CollisionGroup kInvalidGroup = 0xffffu;

private:
    std::vector<std::string> m_names;
    std::vector<u8> m_collidable;
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return kBroadPhaseLayerCount; }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return decodeMoving(layer) ? kBroadPhaseMoving : kBroadPhaseNonMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == kBroadPhaseMoving ? "moving" : "static";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
    {
        // Static against static is the pair the broad-phase split exists to
        // skip: two things that never move cannot begin to overlap.
        return decodeMoving(layer) || broadPhase == kBroadPhaseMoving;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    explicit ObjectPairFilter(const CollisionMatrix& matrix) : m_matrix(matrix) {}

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (!decodeMoving(a) && !decodeMoving(b)) {
            return false;
        }
        return m_matrix.collidable(decodeGroup(a), decodeGroup(b));
    }

private:
    const CollisionMatrix& m_matrix;
};

// --- Records ----------------------------------------------------------------

struct BodyRecord
{
    JPH::BodyID id;
    u32 generation = 0;
    bool alive = false;
    bool collidable = true;
    bool queryable = true;
    bool passableForCharacters = false;
    MotionType motion = MotionType::Dynamic;
    CollisionGroup group = kDefaultCollisionGroup;
    u64 userData = 0;
    // The surfaces a mesh's triangles name (ADR 0117). Written when the body
    // is made, between steps; read by the contact listener on worker threads
    // during one, when nothing writes.
    std::vector<SurfaceMaterial> surfaces;
    // Volume times density, kept because a body the solver does not move has
    // no mass of its own to be asked for.
    f32 mass = 0.0f;
};

// A live joint, plus everything needed to rebuild it: `updateBody` destroys and
// recreates the Jolt body underneath, which would dangle every constraint on it.
// **The motor of a joint that turns about a point** (D470): a hinge's, about
// its axis, and a ball socket's, about all three.
//
// The solver's own joint motors are a row about each body's centre of mass,
// beside the rows that hold the pivot together -- and a solver that goes round
// its rows in turn settles two rows quickly only when they have little to do
// with each other. These have everything to do with each other the moment a
// body swings from a pivot away from where it balances: the motor turns the
// body, the pivot rows take most of that back, the motor turns it again, and
// what a tick's ten rounds arrive at is a fraction of what was asked. A ball a
// metre out on an arm got a spring a seventh as stiff as it was told and none
// of its damping, and rang for ever; that was the servo that never settled.
//
// So the motor is a row of its own, and the row is **the turn with the pivot
// already held**: a twist about the axis on both bodies, plus the push at the
// pivot that keeps the two points together while they take it. Written that
// way it asks nothing of the pivot's rows -- the two are at right angles in
// the measure that matters, the bodies' own masses -- and one round solves it.
// What the row feels as its mass is then what the pair really is to turn about
// that pivot, parallel axes and all, which is also what makes "critically
// damped at this many radians a second" mean the same on a finger and on a
// crane.
//
// The joint itself -- the point, the axis, the limits -- stays the solver's.
class PivotMotorSettings final : public JPH::TwoBodyConstraintSettings
{
public:
    JPH::TwoBodyConstraint* Create(JPH::Body& first, JPH::Body& second) const override;

    // World space: the pivot on each body, and each body's joint frame.
    JPH::RVec3 point1 = JPH::RVec3::sZero();
    JPH::RVec3 point2 = JPH::RVec3::sZero();
    JPH::Quat frame1 = JPH::Quat::sIdentity();
    JPH::Quat frame2 = JPH::Quat::sIdentity();
    // One axis, the frame's X, or all three.
    bool ball = false;
};

class PivotMotor final : public JPH::TwoBodyConstraint
{
public:
    PivotMotor(JPH::Body& first, JPH::Body& second, const PivotMotorSettings& settings)
        : JPH::TwoBodyConstraint(first, second, settings), m_rows(settings.ball ? 3 : 1)
    {
        m_point1 = JPH::Vec3(first.GetInverseCenterOfMassTransform() * settings.point1);
        m_point2 = JPH::Vec3(second.GetInverseCenterOfMassTransform() * settings.point2);
        m_frame1 = first.GetRotation().Conjugated() * settings.frame1;
        m_frame2 = second.GetRotation().Conjugated() * settings.frame2;
    }

    // What it is told, every tick it is driven in.
    MotorMode mode = MotorMode::Off;
    // A hinge: radians, and radians a second.
    float targetAngle = 0.0f;
    float targetVelocity = 0.0f;
    // A ball: the second frame's orientation in the first.
    JPH::Quat targetOrientation = JPH::Quat::sIdentity();
    float maxTorque = 0.0f;
    // The spring a position is held with: a stiffness above zero with its
    // damping, or a frequency in radians a second and a ratio.
    float stiffness = 0.0f;
    float damping = 0.0f;
    float natural = 12.0f;
    float ratio = 1.0f;

    // What the motor did over the last step, in newton-metre-seconds.
    [[nodiscard]] float totalImpulse() const
    {
        float squared = 0.0f;
        for (int index = 0; index < m_rows; ++index) {
            squared += m_row[index].total * m_row[index].total;
        }
        return std::sqrt(squared);
    }

    JPH::EConstraintSubType GetSubType() const override { return JPH::EConstraintSubType::User1; }

    void NotifyShapeChanged(const JPH::BodyID& body, JPH::Vec3Arg deltaCentre) override
    {
        if (mBody1->GetID() == body) {
            m_point1 -= deltaCentre;
        }
        else if (mBody2->GetID() == body) {
            m_point2 -= deltaCentre;
        }
    }

    void SetupVelocityConstraint(float deltaTime) override
    {
        for (Row& row : m_row) {
            row.active = false;
        }
        if (mode == MotorMode::Off || deltaTime <= 0.0f) {
            ResetWarmStart();
            return;
        }
        const bool moves1 = mBody1->IsDynamic();
        const bool moves2 = mBody2->IsDynamic();
        const float give1 = moves1 ? mBody1->GetMotionProperties()->GetInverseMass() : 0.0f;
        const float give2 = moves2 ? mBody2->GetMotionProperties()->GetInverseMass() : 0.0f;
        if (give1 + give2 <= 0.0f) {
            ResetWarmStart();
            return;
        }
        const JPH::Mat44 turn1 = moves1 ? mBody1->GetInverseInertia() : JPH::Mat44::sZero();
        const JPH::Mat44 turn2 = moves2 ? mBody2->GetInverseInertia() : JPH::Mat44::sZero();
        const JPH::Vec3 arm1 = mBody1->GetRotation() * m_point1;
        const JPH::Vec3 arm2 = mBody2->GetRotation() * m_point2;

        // What holding the pivot costs: the two bodies' give at that point.
        const JPH::Mat44 cross1 = JPH::Mat44::sCrossProduct(arm1);
        const JPH::Mat44 cross2 = JPH::Mat44::sCrossProduct(arm2);
        const JPH::Mat44 held = JPH::Mat44::sScale(give1 + give2) - cross1.Multiply3x3(turn1).Multiply3x3(cross1) -
                                cross2.Multiply3x3(turn2).Multiply3x3(cross2);
        JPH::Mat44 holds;
        if (!holds.SetInversed3x3(held)) {
            ResetWarmStart();
            return;
        }

        const JPH::Quat world1 = mBody1->GetRotation() * m_frame1;
        const JPH::Quat world2 = mBody2->GetRotation() * m_frame2;
        // How far past where it should be, along the first frame's axes.
        JPH::Vec3 past = JPH::Vec3::sZero();
        if (mode == MotorMode::Position) {
            const JPH::Quat turned = world1.Conjugated() * world2;
            if (m_rows == 1) {
                const JPH::Quat twist = turned.GetTwist(JPH::Vec3::sAxisX());
                float error = 2.0f * std::atan2(twist.GetX(), twist.GetW()) - targetAngle;
                error = std::remainder(error, 2.0f * JPH::JPH_PI);
                past = JPH::Vec3(error, 0.0f, 0.0f);
            }
            else {
                JPH::Quat over = turned * targetOrientation.Conjugated();
                if (over.GetW() < 0.0f) {
                    over = -over;
                }
                JPH::Vec3 axis;
                float angle = 0.0f;
                over.GetAxisAngle(axis, angle);
                past = axis * angle;
            }
        }

        const JPH::Mat44 axes = JPH::Mat44::sRotation(world1);
        for (int index = 0; index < m_rows; ++index) {
            Row& row = m_row[index];
            const JPH::Vec3 axis = axes.GetColumn3(static_cast<JPH::uint>(index));
            const JPH::Vec3 spin1 = turn1.Multiply3x3(axis);
            const JPH::Vec3 spin2 = turn2.Multiply3x3(axis);
            // How fast the pivot would come apart for a unit of twist, and the
            // push there that stops it.
            const JPH::Vec3 apart = spin2.Cross(arm2) + spin1.Cross(arm1);
            const JPH::Vec3 push = -holds.Multiply3x3(apart);
            const float yields = axis.Dot(spin1 + spin2) + apart.Dot(push);
            if (yields <= 1.0e-12f) {
                row.total = 0.0f;
                continue;
            }
            row.active = true;
            row.linear = push;
            row.angular1 = axis + arm1.Cross(push);
            row.angular2 = axis + arm2.Cross(push);
            row.linearStep1 = push * -give1;
            row.angularStep1 = -turn1.Multiply3x3(row.angular1);
            row.linearStep2 = push * give2;
            row.angularStep2 = turn2.Multiply3x3(row.angular2);
            row.most = maxTorque * deltaTime;

            if (mode == MotorMode::Velocity) {
                row.softness = 0.0f;
                row.bias = index == 0 ? -targetVelocity : 0.0f;
                row.effective = 1.0f / yields;
                continue;
            }
            // A spring, stepped for where it will be at the end of the tick:
            // any stiffness is stable, and none gains energy.
            const float inertia = 1.0f / yields;
            const float firm = stiffness > 0.0f ? stiffness : inertia * natural * natural;
            const float damp = stiffness > 0.0f ? std::max(damping, 0.0f) : 2.0f * ratio * inertia * natural;
            const float both = deltaTime * (damp + deltaTime * firm);
            if (both <= 0.0f) {
                row.active = false;
                row.total = 0.0f;
                continue;
            }
            row.softness = 1.0f / both;
            row.bias = deltaTime * firm * row.softness * past[static_cast<JPH::uint>(index)];
            row.effective = 1.0f / (yields + row.softness);
        }
    }

    void ResetWarmStart() override
    {
        for (Row& row : m_row) {
            row.total = 0.0f;
        }
    }

    void WarmStartVelocityConstraint(float warmStartRatio) override
    {
        for (int index = 0; index < m_rows; ++index) {
            Row& row = m_row[index];
            if (!row.active) {
                continue;
            }
            row.total *= warmStartRatio;
            apply(row, row.total);
        }
    }

    bool SolveVelocityConstraint(float) override
    {
        bool any = false;
        for (int index = 0; index < m_rows; ++index) {
            Row& row = m_row[index];
            if (!row.active) {
                continue;
            }
            const float speed = row.linear.Dot(mBody2->GetLinearVelocity() - mBody1->GetLinearVelocity()) +
                                row.angular2.Dot(mBody2->GetAngularVelocity()) -
                                row.angular1.Dot(mBody1->GetAngularVelocity());
            const float wanted = -row.effective * (speed + row.bias + row.softness * row.total);
            const float next = std::clamp(row.total + wanted, -row.most, row.most);
            const float change = next - row.total;
            row.total = next;
            if (change != 0.0f) {
                apply(row, change);
                any = true;
            }
        }
        return any;
    }

    bool SolvePositionConstraint(float, float) override { return false; }

#ifdef JPH_DEBUG_RENDERER
    void DrawConstraint(JPH::DebugRenderer*) const override {}
#endif

    void SaveState(JPH::StateRecorder& stream) const override
    {
        JPH::TwoBodyConstraint::SaveState(stream);
        for (const Row& row : m_row) {
            stream.Write(row.total);
        }
    }

    void RestoreState(JPH::StateRecorder& stream) override
    {
        JPH::TwoBodyConstraint::RestoreState(stream);
        for (Row& row : m_row) {
            stream.Read(row.total);
        }
    }

    JPH::Ref<JPH::ConstraintSettings> GetConstraintSettings() const override
    {
        JPH::Ref<PivotMotorSettings> settings = new PivotMotorSettings;
        settings->ball = m_rows == 3;
        return settings.GetPtr();
    }

    JPH::Mat44 GetConstraintToBody1Matrix() const override
    {
        return JPH::Mat44::sRotationTranslation(m_frame1, m_point1);
    }

    JPH::Mat44 GetConstraintToBody2Matrix() const override
    {
        return JPH::Mat44::sRotationTranslation(m_frame2, m_point2);
    }

private:
    struct Row
    {
        bool active = false;
        // The row: a push at the pivot (on the second body, reversed on the
        // first) and a twist on each.
        JPH::Vec3 linear = JPH::Vec3::sZero();
        JPH::Vec3 angular1 = JPH::Vec3::sZero();
        JPH::Vec3 angular2 = JPH::Vec3::sZero();
        // What a unit of it does to each body's velocities.
        JPH::Vec3 linearStep1 = JPH::Vec3::sZero();
        JPH::Vec3 angularStep1 = JPH::Vec3::sZero();
        JPH::Vec3 linearStep2 = JPH::Vec3::sZero();
        JPH::Vec3 angularStep2 = JPH::Vec3::sZero();
        float effective = 0.0f;
        float softness = 0.0f;
        float bias = 0.0f;
        float most = 0.0f;
        float total = 0.0f;
    };

    void apply(const Row& row, float impulse)
    {
        if (mBody1->IsDynamic()) {
            JPH::MotionProperties* motion = mBody1->GetMotionProperties();
            motion->AddLinearVelocityStep(row.linearStep1 * impulse);
            motion->AddAngularVelocityStep(row.angularStep1 * impulse);
        }
        if (mBody2->IsDynamic()) {
            JPH::MotionProperties* motion = mBody2->GetMotionProperties();
            motion->AddLinearVelocityStep(row.linearStep2 * impulse);
            motion->AddAngularVelocityStep(row.angularStep2 * impulse);
        }
    }

    // In each body's own space, about where it balances.
    JPH::Vec3 m_point1;
    JPH::Vec3 m_point2;
    JPH::Quat m_frame1;
    JPH::Quat m_frame2;
    int m_rows = 1;
    Row m_row[3];
};

JPH::TwoBodyConstraint* PivotMotorSettings::Create(JPH::Body& first, JPH::Body& second) const
{
    return new PivotMotor(first, second, *this);
}

struct ConstraintRecord
{
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    // A hinge's and a ball socket's motor, once one has been asked for: a
    // constraint of its own beside the joint, added after it and gone with it.
    JPH::Ref<PivotMotor> motor;
    u32 generation = 0;
    bool alive = false;
    // Kept because a rebuild needs them and because retiring a body has to find
    // the joints that name it.
    BodyHandle first;
    BodyHandle second;
    ConstraintDesc desc;
};

struct CharacterRecord
{
    JPH::Ref<JPH::CharacterVirtual> character;
    u32 generation = 0;
    bool alive = false;
    f32 stepHeight = 0.5f;
    u64 userData = 0;
    // The layer the character sweeps the world as -- its own group, moving.
    // Kept because `ExtendedUpdate` needs it every tick and the settings that
    // carried it are gone by then.
    JPH::ObjectLayer layer = 0;
    // **What the last move did**, in metres a second (D441): where it ended
    // less where it began, over the step. The controller's own velocity is
    // what it was ASKED for, and a character walking into a wall for twenty
    // seconds reported six metres a second the whole time.
    core::Vec3 moved{};
    // **Its stand-in** (`setCharacterStandIn`, ADR 0163): a kinematic copy of
    // its capsule where other characters are to meet it. While there is one,
    // the character's own inner body does not stand in a character's way.
    JPH::BodyID standIn;
};

// What a stand-in's body carries where a body carries its handle: no record,
// so no query sees it, and the contact listener knows it by this.
inline constexpr u64 kStandInUserData = ~u64{0};

// A kinematic body's target for this tick, waiting for the delta that turns it
// into a velocity. See `setBodyTransform`.
struct PendingMove
{
    JPH::BodyID id;
    JPH::RVec3 position;
    JPH::Quat rotation;
};

// A contacting pair, keyed by our own handles rather than by Jolt's body ids.
// Packed so a pair is one comparison and one sort key -- and so the ordering is
// ours, which is what makes it survive a job system that reports the same pair
// in either order (Docs/Architecture.md:806).
struct ContactPair
{
    u64 first = 0;
    u64 second = 0;

    [[nodiscard]] constexpr bool operator==(const ContactPair&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const ContactPair& other) const noexcept
    {
        return first != other.first ? first < other.first : second < other.second;
    }
};

// What a pair's contact was when it began (ADR 0127, N2), in the simulation's
// own space. Kept beside the pairs rather than in them: a pair is compared and
// sorted every step, and none of this is part of what a pair IS.
struct ContactDetail
{
    ContactPair pair;
    JPH::RVec3 point;
    JPH::Vec3 normal;
    f32 speed = 0.0f;
};

// One contact between a CHARACTER and something else, for the tick.
//
// Held apart from `ContactPair` rather than folded into it, and the reason is
// not tidiness: a `CharacterVirtual` is not a body, so the two sides are handles
// from different spaces, and the sleep exception the rigid diff makes must not
// apply here -- a character is never put to sleep by the solver and its inner
// body is created with `mAllowSleeping = false` (`CharacterVirtual.cpp:146`), so
// a contact that stops being reported really has ended.
//
// The character is always the FIRST side, which is what makes the pair
// canonical without a swap.
struct CharacterPair
{
    u64 character = 0;
    u64 other = 0;
    // The other side is another character's inner body rather than an ordinary
    // one, so its handle is a `CharacterHandle` and resolves through a different
    // table. Part of the key, because the two spaces can collide numerically.
    bool otherIsCharacter = false;

    [[nodiscard]] constexpr bool operator==(const CharacterPair&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const CharacterPair& rhs) const noexcept
    {
        if (character != rhs.character)
            return character < rhs.character;
        if (other != rhs.other)
            return other < rhs.other;
        return static_cast<int>(otherIsCharacter) < static_cast<int>(rhs.otherIsCharacter);
    }
};

[[nodiscard]] constexpr u64 packHandle(BodyHandle handle) noexcept
{
    return (static_cast<u64>(handle.generation) << 32) | handle.index;
}

[[nodiscard]] constexpr u64 packHandle(CharacterHandle handle) noexcept
{
    return (static_cast<u64>(handle.generation) << 32) | handle.index;
}

[[nodiscard]] constexpr CharacterHandle unpackCharacter(u64 packed) noexcept
{
    return CharacterHandle{static_cast<u32>(packed & 0xffffffffu), static_cast<u32>(packed >> 32)};
}

[[nodiscard]] constexpr BodyHandle unpackHandle(u64 packed) noexcept
{
    return BodyHandle{static_cast<u32>(packed & 0xffffffffu), static_cast<u32>(packed >> 32)};
}

// --- Conversions ------------------------------------------------------------
//
// f64 in, f32 out. This is architecture.md §10's split and not a shortcut:
// world precision is f64 in `scene` plus a floating origin, and physics runs in
// the rebased f32 space.
//
// **M7 made the origin real, and the two functions below are now the WRONG ones
// to call from inside a world.** They narrow against an origin of zero, which is
// only correct for a world that has never rebased; `JoltWorld::toLocal` and
// `::toWorld` are the pair that subtracts and adds the world's own origin, and
// every call site inside the world uses those. These stay because the debug
// bridge -- which is handed a sink and not a world -- needs a raw pair, and
// because a raycast's DIRECTION is a displacement rather than a position and
// must never be shifted.

[[nodiscard]] JPH::Vec3 toJolt(core::Vec3 v) noexcept
{
    return JPH::Vec3(v.x, v.y, v.z);
}

[[nodiscard]] JPH::RVec3 toJoltPosition(core::DVec3 v) noexcept
{
    return JPH::RVec3(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z));
}

[[nodiscard]] core::Vec3 fromJolt(JPH::Vec3Arg v) noexcept
{
    return core::Vec3{v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] core::DVec3 fromJoltPosition(JPH::RVec3Arg v) noexcept
{
    return core::DVec3{static_cast<f64>(v.GetX()), static_cast<f64>(v.GetY()), static_cast<f64>(v.GetZ())};
}

[[nodiscard]] JPH::Quat toJolt(const core::Mat3& rotation) noexcept
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;
    core::toQuaternion(rotation, x, y, z, w);
    return JPH::Quat(x, y, z, w).Normalized();
}

[[nodiscard]] core::Mat3 fromJolt(JPH::QuatArg q) noexcept
{
    return core::fromQuaternion(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
}

// --- Shapes -----------------------------------------------------------------

[[nodiscard]] JPH::ShapeRefC buildShape(const ShapeDesc& desc)
{
    // Half-extents, because `size` is the full extent everywhere in this engine
    // and Jolt takes halves. Clamped away from zero: a zero-sized shape is a
    // degenerate hull Jolt refuses, and a script writing `Size = Vector3.zero`
    // must produce a very small part rather than an error the frame cannot
    // recover from.
    //
    // **And NaN-safe** (audit E3): `std::max(NaN, k)` is NaN, and a NaN extent
    // is a shape Jolt builds wrong. What is not finite is the smallest part;
    // what is past a million metres is held there.
    constexpr f32 kMinHalfExtent = 0.005f;
    constexpr f32 kMaxHalfExtent = 5.0e5f;
    const auto half = [&](f32 full) {
        const f32 value = full * 0.5f;
        return std::isfinite(value) ? std::clamp(value, kMinHalfExtent, kMaxHalfExtent) : kMinHalfExtent;
    };
    const f32 hx = half(desc.size.x);
    const f32 hy = half(desc.size.y);
    const f32 hz = half(desc.size.z);

    switch (desc.type) {
    case ShapeType::Box: {
        // Jolt's box carries a convex radius that must fit inside the box; the
        // default 0.05 makes a part thinner than 10 cm fail to build.
        const f32 convexRadius = std::min({hx, hy, hz, JPH::cDefaultConvexRadius});
        return JPH::ShapeRefC(new JPH::BoxShape(JPH::Vec3(hx, hy, hz), convexRadius));
    }
    case ShapeType::Sphere:
        return JPH::ShapeRefC(new JPH::SphereShape(std::max({hx, hy, hz})));
    case ShapeType::Capsule: {
        const f32 radius = std::max(hx, hz);
        // The cylindrical part only: Jolt's half-height excludes the caps,
        // while `Size.y` includes them.
        const f32 halfCylinder = std::max(hy - radius, kMinHalfExtent);
        return JPH::ShapeRefC(new JPH::CapsuleShape(halfCylinder, radius));
    }
    case ShapeType::Cylinder: {
        const f32 radius = std::max(hx, hz);
        const f32 convexRadius = std::min({radius, hy, JPH::cDefaultConvexRadius});
        return JPH::ShapeRefC(new JPH::CylinderShape(hy, radius, convexRadius));
    }
    case ShapeType::ConvexHull: {
        if (desc.points.size() < 4) {
            return {};
        }
        JPH::Array<JPH::Vec3> points;
        points.reserve(desc.points.size());
        for (const core::Vec3& point : desc.points) {
            points.push_back(toJolt(
                core::Vec3{point.x * desc.pointScale.x, point.y * desc.pointScale.y, point.z * desc.pointScale.z}));
        }
        JPH::ConvexHullShapeSettings settings(points);
        settings.SetEmbedded();
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            return {};
        }
        return result.Get();
    }
    case ShapeType::HeightField: {
        // **The grid has to be big enough to have blocks in it.** Jolt requires
        // `sampleCount / blockSize >= 2`, and a smaller one asserts inside the
        // builder rather than returning an error -- so it is refused here, where
        // a refusal is a body that does not appear rather than a crash.
        const core::u32 samples = desc.heightSampleCount;
        const core::u32 block = std::max(desc.heightBlockSize, 1u);
        if (samples < block * 2 || desc.heights.size() < static_cast<core::usize>(samples) * samples) {
            return {};
        }

        // The surface Jolt builds is `offset + scale * (x, sample, z)` for
        // integer x and z. So the scale spreads `samples - 1` intervals across
        // the footprint `size` describes, the offset centres it, and **the y
        // scale is one** -- which is what makes a sample a height in local
        // metres rather than a number needing a second document to interpret.
        const f32 span = static_cast<f32>(samples - 1);
        const JPH::Vec3 scale(desc.size.x / span, 1.0f, desc.size.z / span);
        const JPH::Vec3 offset(-desc.size.x * 0.5f, 0.0f, -desc.size.z * 0.5f);

        // **Padded to a power-of-two count of blocks, with no-collision samples
        // on the +x and +z sides.** Jolt 5.6.0 builds its range-block hierarchy
        // over the block count rounded UP to a power of two, and `SetHeights`
        // walks it with the count halved and rounded DOWN -- so an in-place edit
        // of a grid of, say, 17 blocks writes its ranges into the wrong cells
        // and the ground stops colliding. Measured, not read: a 33-sample grid
        // edited in place let a resting cube fall through, a 32-sample one did
        // not. The padding sits beyond the last real sample, so the scale and
        // offset above -- and so every real sample's position -- are unchanged.
        const u32 blocks = (samples + block - 1) / block;
        u32 blocksPow2 = 1;
        while (blocksPow2 < blocks)
            blocksPow2 <<= 1;
        const u32 padded = blocksPow2 * block;
        std::vector<float> paddedHeights;
        const float* heightData = desc.heights.data();
        if (padded != samples) {
            paddedHeights.assign(static_cast<core::usize>(padded) * padded,
                                 JPH::HeightFieldShapeConstants::cNoCollisionValue);
            for (u32 row = 0; row < samples; ++row) {
                std::copy_n(desc.heights.data() + static_cast<core::usize>(row) * samples, samples,
                            paddedHeights.data() + static_cast<core::usize>(row) * padded);
            }
            heightData = paddedHeights.data();
        }

        JPH::HeightFieldShapeSettings settings(heightData, offset, scale, padded);
        settings.mBlockSize = block;
        // **The range the field may ever hold, reserved now because it cannot be
        // widened later.** Jolt spreads its sample bits across
        // `[min(mMinHeightValue, samples...), max(mMaxHeightValue, samples...)]`
        // and bakes that mapping into the shape, so `SetHeights` clamps
        // everything it is later given back into it. A flat field built from
        // all-zero samples gets a zero-wide range, and then every edit succeeds
        // and changes nothing.
        //
        // Only when the caller asked for a wider range than its samples span.
        // Left at the default the two are equal, Jolt's own defaults stand, and
        // the range is the samples' -- which is right for a field nobody edits.
        if (desc.heightMax > desc.heightMin) {
            settings.mMinHeightValue = desc.heightMin;
            settings.mMaxHeightValue = desc.heightMax;
        }
        settings.SetEmbedded();
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            return {};
        }
        return result.Get();
    }
    case ShapeType::TriangleMesh: {
        // Triples, and a partial one is a description this cannot honour. Three
        // vertices is the fewest a surface can have.
        if (desc.points.size() < 3 || desc.indices.size() < 3 || desc.indices.size() % 3 != 0) {
            return {};
        }

        JPH::VertexList vertices;
        vertices.reserve(desc.points.size());
        for (const core::Vec3& point : desc.points) {
            vertices.push_back(
                JPH::Float3(point.x * desc.pointScale.x, point.y * desc.pointScale.y, point.z * desc.pointScale.z));
        }

        JPH::IndexedTriangleList triangles;
        triangles.reserve(desc.indices.size() / 3);
        const auto vertexCount = static_cast<core::u32>(desc.points.size());
        for (core::usize at = 0; at + 2 < desc.indices.size(); at += 3) {
            const core::u32 a = desc.indices[at];
            const core::u32 b = desc.indices[at + 1];
            const core::u32 c = desc.indices[at + 2];
            // **An index past the end is refused rather than clamped.** Clamping
            // would build a degenerate triangle out of somebody's bug and hide
            // it inside a collider that behaves almost right.
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
                return {};
            }
            // **The band is marked** (ADR 0143): 1 in the triangle's user
            // data, which the contact listeners refuse.
            const bool band = at / 3 >= desc.bandFirst;
            // And what it is made of (ADR 0117), above the band's bit: the
            // surface a contact on this triangle slides and bounces by.
            const u32 surface = at / 3 < desc.triangleSurfaces.size() ? desc.triangleSurfaces[at / 3] : 0u;
            triangles.push_back(JPH::IndexedTriangle(a, b, c, 0, (band ? kBandTriangle : 0u) | (surface << 8)));
        }

        // `Sanitize` runs inside this constructor -- duplicate and degenerate
        // triangles are removed for us, which a mesher's output at a cell
        // boundary produces routinely.
        JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
        settings.mPerTriangleUserData = desc.bandFirst < desc.indices.size() / 3 || !desc.triangleSurfaces.empty();
        settings.SetEmbedded();
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            return {};
        }
        return result.Get();
    }
    }
    return {};
}

// --- Deterministic collectors -----------------------------------------------
//
// Jolt's own closest-hit collector keeps the FIRST of two hits at an equal
// fraction, and which one arrives first is traversal order. These keep the one
// with the lower body id instead, so a tie -- two coincident surfaces, a ray
// down a seam -- answers the same way on every run (R10).

class ClosestRayCollector final : public JPH::CastRayCollector
{
public:
    void AddHit(const JPH::RayCastResult& result) override
    {
        if (!has || result.mFraction < hit.mFraction ||
            (result.mFraction == hit.mFraction &&
             result.mBodyID.GetIndexAndSequenceNumber() < hit.mBodyID.GetIndexAndSequenceNumber())) {
            hit = result;
            has = true;
            UpdateEarlyOutFraction(result.mFraction);
        }
    }

    JPH::RayCastResult hit;
    bool has = false;
};

class ClosestShapeCollector final : public JPH::CastShapeCollector
{
public:
    void AddHit(const JPH::ShapeCastResult& result) override
    {
        if (!has || result.mFraction < hit.mFraction ||
            (result.mFraction == hit.mFraction &&
             result.mBodyID2.GetIndexAndSequenceNumber() < hit.mBodyID2.GetIndexAndSequenceNumber())) {
            hit = result;
            has = true;
            UpdateEarlyOutFraction(result.mFraction);
        }
    }

    JPH::ShapeCastResult hit;
    bool has = false;
};

class OverlapCollector final : public JPH::CollideShapeCollector
{
public:
    void AddHit(const JPH::CollideShapeResult& result) override { bodies.push_back(result.mBodyID2); }

    JPH::Array<JPH::BodyID> bodies;
};

// --- The world --------------------------------------------------------------

class JoltWorld;

// Whether a contact is with a band triangle (ADR 0143): a static mesh's
// triangle whose user data says so. A mesh with no user data answers 0 for
// every triangle.
[[nodiscard]] bool onBand(const JPH::Body& body, const JPH::SubShapeID& subShape)
{
    if (!body.IsStatic())
        return false;
    const JPH::Shape* shape = body.GetShape();
    return shape->GetSubType() == JPH::EShapeSubType::Mesh &&
           (static_cast<const JPH::MeshShape*>(shape)->GetTriangleUserData(subShape) & kBandTriangle) != 0;
}

// Appends to the world's per-step pair buffer and does nothing else. Runs
// inside `PhysicsSystem::Update`, and from M7 on a worker thread -- so it may
// not touch the scene, allocate a script value, or decide an order.
// How the listener asks what a triangle is made of: the world's records are
// the world's, and the listener is told how to read them.
class SurfaceSource
{
public:
    virtual ~SurfaceSource() = default;
    // False for a body with no surfaces of its own, or a triangle of none.
    [[nodiscard]] virtual bool surfaceOf(const JPH::Body& body, const JPH::SubShapeID& subShape,
                                         SurfaceMaterial& out) const = 0;
};

class ContactRecorder final : public JPH::ContactListener
{
public:
    void setSurfaces(const SurfaceSource* source) noexcept { m_surfaces = source; }

    // **A triangle's own friction and bounce, in place of its body's** (ADR
    // 0117): combined with the other body's the way the solver combines two
    // bodies' -- the geometric mean, and the larger bounce -- so a triangle of
    // the default surface gives exactly the numbers it always did.
    void applySurface(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                      JPH::ContactSettings& settings) const
    {
        if (m_surfaces == nullptr)
            return;
        SurfaceMaterial one{first.GetFriction(), first.GetRestitution()};
        SurfaceMaterial two{second.GetFriction(), second.GetRestitution()};
        const bool firstHas = m_surfaces->surfaceOf(first, manifold.mSubShapeID1, one);
        const bool secondHas = m_surfaces->surfaceOf(second, manifold.mSubShapeID2, two);
        if (!firstHas && !secondHas)
            return;
        settings.mCombinedFriction = std::sqrt(one.friction * two.friction);
        settings.mCombinedRestitution = std::max(one.restitution, two.restitution);
    }

    void OnContactAdded(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override
    {
        record(first, second);
        applySurface(first, second, manifold, settings);
        // **How the two met**, read here because here is the only place it
        // exists: the bodies' velocities are still the ones they arrived with.
        // The first contact point, the normal from the first body to the
        // second, and how fast they were closing along it.
        if (manifold.mRelativeContactPointsOn1.empty()) {
            return;
        }
        ContactDetail detail;
        detail.pair = ContactPair{first.GetUserData(), second.GetUserData()};
        detail.point = manifold.GetWorldSpaceContactPointOn1(0);
        detail.normal = manifold.mWorldSpaceNormal;
        const JPH::Vec3 closing = first.GetPointVelocity(detail.point) - second.GetPointVelocity(detail.point);
        detail.speed = std::max(closing.Dot(detail.normal), 0.0f);
        if (detail.pair.second < detail.pair.first) {
            std::swap(detail.pair.first, detail.pair.second);
            detail.normal = -detail.normal;
        }
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_details.push_back(detail);
    }

    void OnContactPersisted(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override
    {
        record(first, second);
        applySurface(first, second, manifold, settings);
    }

    // **The one place `collideConnected = false` can be implemented.**
    //
    // An upper arm and a lower arm overlap at the elbow by construction, and
    // left colliding they shove each other apart every step -- a ragdoll that
    // vibrates instead of falling. The exclusion is per PAIR and Jolt's object
    // layer matrix is per LAYER, so a collision group cannot express it: two
    // limbs of one character must ignore each other and still collide with the
    // two limbs of the next.
    //
    // Called from Jolt's collision jobs, so it takes the same lock the recorder
    // does -- and returns early on the common case, which is a world with no
    // constraints at all.
    JPH::ValidateResult OnContactValidate(const JPH::Body& first, const JPH::Body& second, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult& hit) override
    {
        // **A character's stand-in is for characters alone** (ADR 0163): the
        // character it stands in for is already in the world, where it is
        // drawn, and a crate must not be pushed by both.
        if (first.GetUserData() == kStandInUserData || second.GetUserData() == kStandInUserData)
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        {
            const std::lock_guard<std::mutex> guard(m_mutex);
            if (!m_excluded.empty()) {
                ContactPair pair{first.GetUserData(), second.GetUserData()};
                if (pair.second < pair.first) {
                    std::swap(pair.first, pair.second);
                }
                if (std::binary_search(m_excluded.begin(), m_excluded.end(), pair)) {
                    return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
                }
            }
        }
        // **A terrain chunk's band only lends its edges** (ADR 0143): a contact
        // with one of its triangles is refused, and the pair is asked about
        // each contact -- the neighbour's own triangle holds the body there.
        const bool firstMesh = first.IsStatic() && first.GetShape()->GetSubType() == JPH::EShapeSubType::Mesh;
        const bool secondMesh = second.IsStatic() && second.GetShape()->GetSubType() == JPH::EShapeSubType::Mesh;
        if (firstMesh || secondMesh) {
            if ((firstMesh && onBand(first, hit.mSubShapeID1)) || (secondMesh && onBand(second, hit.mSubShapeID2)))
                return JPH::ValidateResult::RejectContact;
            return JPH::ValidateResult::AcceptContact;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    // Sorted on insert, so the validate hook is a binary search rather than a
    // scan -- a ragdoll is a dozen pairs and a crowd of them is hundreds, and
    // this runs once per candidate pair per step.
    void exclude(u64 first, u64 second)
    {
        ContactPair pair{first, second};
        if (pair.second < pair.first) {
            std::swap(pair.first, pair.second);
        }
        const std::lock_guard<std::mutex> guard(m_mutex);
        const auto at = std::lower_bound(m_excluded.begin(), m_excluded.end(), pair);
        // Not deduplicated: two constraints between one pair are two exclusions,
        // and removing one must leave the other standing.
        m_excluded.insert(at, pair);
    }

    void unexclude(u64 first, u64 second)
    {
        ContactPair pair{first, second};
        if (pair.second < pair.first) {
            std::swap(pair.first, pair.second);
        }
        const std::lock_guard<std::mutex> guard(m_mutex);
        const auto at = std::lower_bound(m_excluded.begin(), m_excluded.end(), pair);
        if (at != m_excluded.end() && *at == pair) {
            m_excluded.erase(at);
        }
    }

    void clear()
    {
        // No lock: called between steps, from the simulation thread.
        m_pairs.clear();
        m_details.clear();
    }

    [[nodiscard]] std::vector<ContactPair>& pairs() noexcept { return m_pairs; }
    [[nodiscard]] std::vector<ContactDetail>& details() noexcept { return m_details; }

private:
    void record(const JPH::Body& first, const JPH::Body& second)
    {
        ContactPair pair{first.GetUserData(), second.GetUserData()};
        if (pair.second < pair.first) {
            std::swap(pair.first, pair.second);
        }
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_pairs.push_back(pair);
    }

    const SurfaceSource* m_surfaces = nullptr;
    std::mutex m_mutex;
    std::vector<ContactPair> m_pairs;
    std::vector<ContactDetail> m_details;
    // Sorted, and read under the same lock: the validate hook runs on Jolt's
    // worker threads while nothing may be writing here, but a constraint created
    // between steps writes from the simulation thread and the lock is what makes
    // the two safe against each other.
    std::vector<ContactPair> m_excluded;
};

#ifdef JPH_DEBUG_RENDERER
// Jolt's simple debug renderer draws everything as triangles and lines; we take
// the lines and turn the triangles into their three edges, because the engine's
// debug draw is a wireframe and a filled physics shape would hide the render
// mesh it is there to be compared against.
class DebugBridge final : public JPH::DebugRendererSimple
{
public:
    // The origin is passed in rather than read from a world, because the bridge
    // is handed a sink and never a world -- and a wireframe drawn in local space
    // while everything else is drawn in world space is D011 again, one rebase
    // later.
    DebugBridge(IDebugDrawSink& sink, core::DVec3 origin) : m_sink(sink), m_origin(origin) {}

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override
    {
        m_sink.line(toWorld(from), toWorld(to), toRgb(color));
    }

    void DrawTriangle(JPH::RVec3Arg v1, JPH::RVec3Arg v2, JPH::RVec3Arg v3, JPH::ColorArg color, ECastShadow) override
    {
        const u32 rgb = toRgb(color);
        m_sink.line(toWorld(v1), toWorld(v2), rgb);
        m_sink.line(toWorld(v2), toWorld(v3), rgb);
        m_sink.line(toWorld(v3), toWorld(v1), rgb);
    }

    void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override {}

private:
    [[nodiscard]] core::DVec3 toWorld(JPH::RVec3Arg v) const noexcept { return fromJoltPosition(v) + m_origin; }

    [[nodiscard]] static u32 toRgb(JPH::ColorArg color) noexcept
    {
        return (static_cast<u32>(color.r) << 16) | (static_cast<u32>(color.g) << 8) | static_cast<u32>(color.b);
    }

    IDebugDrawSink& m_sink;
    core::DVec3 m_origin;
};
#endif

// The three budgets a world is sized by, all derived from the body count so
// that one number in `WorldDesc` decides them together.
//
// A stack of a thousand crates produces far more contacts than bodies, and a
// buffer that overflows drops contacts -- which reads as parts sinking through
// each other rather than as an error. The temp allocator is derived from the
// same numbers because Jolt allocates its per-step working set from it in ONE
// request: sized independently, the first integration here asked for 30 MB from
// a 16 MB allocator and the process aborted with no message.
[[nodiscard]] JPH::uint bodyBudget(const WorldDesc& desc) noexcept
{
    return std::max<JPH::uint>(desc.maxBodies, 1024);
}

[[nodiscard]] JPH::uint contactBudget(const WorldDesc& desc) noexcept
{
    return bodyBudget(desc) * 2;
}

// **How many threads Jolt solves on, and why it is a constant** (S6.10).
//
// Jolt is deterministic across runs provided the thread count is the same, so
// this number is part of the world hash in the way a physics constant is: change
// it and every recorded trace in `tests/determinism` has to be re-recorded.
// Deriving it from the machine -- `hardware_concurrency`, or the engine job
// pool's worker count -- would make the SAME platform's trace differ between two
// machines, which is the one thing a committed trace cannot survive.
//
// Four rather than one, measured on `win-msvc-dev` at 1,000 and 10,000 bodies:
//
//   physics1k step   1.76 ms -> 0.65 ms
//   churn10k  step   3.73 ms -> 1.85 ms
//   churn10k  worst  174 ms  -> 40 ms
//
// Four rather than eight because the gain is in the solver's own parallelism and
// the tail flattens, and because a fixed count that oversubscribes a small
// machine costs more than the threads it adds. It is a number this project can
// revisit with a measurement and a trace re-record, which is what makes it a
// constant with a name rather than a literal at the call site.
inline constexpr int kPhysicsThreads = 4;

[[nodiscard]] JPH::uint tempBytes(const WorldDesc& desc) noexcept
{
    return 8u * 1024u * 1024u + contactBudget(desc) * 64u + contactBudget(desc) * 512u;
}

class JoltWorld final : public SurfaceSource
{
public:
    // What a triangle of a mesh is made of (ADR 0117), for the contact
    // listener: the body's own table, by the triangle's user data. Read on the
    // solver's worker threads during a step, when no record is written.
    [[nodiscard]] bool surfaceOf(const JPH::Body& body, const JPH::SubShapeID& subShape,
                                 SurfaceMaterial& out) const override
    {
        if (!body.IsStatic())
            return false;
        const JPH::Shape* shape = body.GetShape();
        if (shape->GetSubType() != JPH::EShapeSubType::Mesh)
            return false;
        const u32 surface = static_cast<const JPH::MeshShape*>(shape)->GetTriangleUserData(subShape) >> 8;
        if (surface == 0)
            return false;
        const BodyRecord* record = resolve(unpackHandle(body.GetUserData()));
        if (record == nullptr || surface >= record->surfaces.size())
            return false;
        out = record->surfaces[surface];
        return true;
    }

    explicit JoltWorld(const WorldDesc& desc)
        : m_pairFilter(m_matrix), m_temp(tempBytes(desc)),
          m_jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, kPhysicsThreads), m_gravity(desc.gravity),
          m_contactBudget(contactBudget(desc))
    {
        m_system.Init(bodyBudget(desc), 0, contactBudget(desc), contactBudget(desc), m_broadPhaseLayers,
                      m_objectVsBroadPhase, m_pairFilter);
        m_system.SetGravity(toJolt(desc.gravity));
        m_system.SetContactListener(&m_contacts);
        m_contacts.setSurfaces(this);
    }

    ~JoltWorld()
    {
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        for (const BodyRecord& record : m_bodies) {
            if (record.alive) {
                bodies.RemoveBody(record.id);
                bodies.DestroyBody(record.id);
            }
        }
    }

    JoltWorld(const JoltWorld&) = delete;
    JoltWorld& operator=(const JoltWorld&) = delete;

    // --- The floating origin (ADR 0014, architecture.md §10) ------------------
    //
    // Every position crossing this seam is ABSOLUTE f64 -- that is what `scene`
    // stores and what a script reads. Everything inside Jolt is f32 relative to
    // `m_origin`. These two are the whole translation, and they are members
    // rather than free functions precisely so that a call site cannot forget
    // which space it is in: there is no way to reach a body's position without
    // going through one of them.
    //
    // A DIRECTION never passes through here. A ray's direction is a
    // displacement, and shifting it would turn a hundred-metre ray into a
    // hundred-metre ray pointing at the old origin.
    [[nodiscard]] JPH::RVec3 toLocal(core::DVec3 v) const noexcept { return toJoltPosition(v - m_origin); }

    [[nodiscard]] core::DVec3 toWorld(JPH::RVec3Arg v) const noexcept { return fromJoltPosition(v) + m_origin; }

    [[nodiscard]] core::DVec3 origin() const noexcept { return m_origin; }

    // Moves the world under the simulation. Every resident body and character
    // shifts by the negative of the delta, so **nothing moves in absolute
    // terms** -- which is the entire point, and is why the test for this is a
    // hash rather than a picture.
    //
    // Velocities are untouched, and that is not an omission: a teleport that
    // reset them would stop a falling body dead every time the origin moved,
    // which is architecture.md's "velocity-preserving teleport" spelled out.
    // Bodies are NOT activated, because a sleeping body that wakes on a rebase
    // is a world that behaves differently depending on where the camera is.
    void setOrigin(core::DVec3 origin)
    {
        if (origin == m_origin) {
            return;
        }

        const JPH::Vec3 delta = toJolt(core::toVec3(origin - m_origin));
        m_origin = origin;

        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        for (const BodyRecord& record : m_bodies) {
            if (!record.alive) {
                continue;
            }
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodies.GetPositionAndRotation(record.id, position, rotation);
            bodies.SetPositionAndRotation(record.id, position - delta, rotation, JPH::EActivation::DontActivate);
        }

        // A kinematic body's pending target is a POSITION and shifts with
        // everything else. Missing this would send every moving platform back
        // to where it was before the rebase, once, on the frame it happened.
        for (PendingMove& move : m_kinematicMoves) {
            move.position = move.position - delta;
        }

        for (const CharacterRecord& record : m_characters) {
            if (record.character != nullptr) {
                record.character->SetPosition(record.character->GetPosition() - delta);
            }
        }
    }

    void setGravity(core::Vec3 gravity)
    {
        m_gravity = gravity;
        m_system.SetGravity(toJolt(gravity));
    }

    [[nodiscard]] core::Vec3 gravity() const noexcept { return m_gravity; }

    // --- Bodies ---------------------------------------------------------------

    // **A hull built once per cloud and scale** (H8). Every body of a mesh
    // built its convex hull from the mesh's points again -- a quarter of a
    // millisecond each -- so a game making a projectile a shot paid for the
    // hull a shot. The key is the cloud's revision, which names one set of
    // points for as long as it lives (`ShapeDesc::pointsRevision`), and the
    // scale it is drawn at. A shape is immutable once built and Jolt counts
    // its references, so sharing one between bodies is what it is for.
    [[nodiscard]] core::u64 shapesBuilt() const noexcept { return m_shapesBuilt; }

    [[nodiscard]] JPH::ShapeRefC shapeFor(const ShapeDesc& desc)
    {
        if (desc.type != ShapeType::ConvexHull || desc.pointsRevision == 0) {
            ++m_shapesBuilt;
            return buildShape(desc);
        }
        const HullKey key{desc.pointsRevision, std::bit_cast<u32>(desc.pointScale.x),
                          std::bit_cast<u32>(desc.pointScale.y), std::bit_cast<u32>(desc.pointScale.z)};
        if (const auto found = m_hulls.find(key); found != m_hulls.end())
            return found->second;
        ++m_shapesBuilt;
        JPH::ShapeRefC shape = buildShape(desc);
        if (shape == nullptr)
            return shape;
        // Bounded: a world that resizes one mesh to every size there is
        // starts again rather than growing for ever.
        constexpr core::usize MostHulls = 4096;
        if (m_hulls.size() >= MostHulls)
            m_hulls.clear();
        m_hulls.emplace(key, shape);
        return shape;
    }

    [[nodiscard]] BodyHandle createBody(const BodyDesc& desc)
    {
        // A shape built ahead (ADR 0181) is taken as it is, and counted as
        // one built: it was, for this world.
        JPH::ShapeRefC shape;
        if (desc.prepared.valid()) {
            shape = *static_cast<const JPH::ShapeRefC*>(desc.prepared.shape.get());
            ++m_shapesBuilt;
        }
        else {
            shape = shapeFor(desc.shape);
        }
        if (shape == nullptr) {
            return {};
        }

        u32 slot = 0;
        if (!m_freeBodies.empty()) {
            slot = m_freeBodies.back();
            m_freeBodies.pop_back();
        }
        else {
            slot = static_cast<u32>(m_bodies.size());
            m_bodies.emplace_back();
        }

        BodyRecord& record = m_bodies[slot];
        // Generations start at one so a default-constructed handle is invalid,
        // and never wrap to zero for the same reason.
        record.generation = record.generation + 1 == 0 ? 1 : record.generation + 1;

        const BodyHandle handle{slot, record.generation};
        if (!instantiate(record, handle, desc, shape)) {
            record.alive = false;
            m_freeBodies.push_back(slot);
            return {};
        }
        return handle;
    }

    // A shape or motion-type change is a recreate on Jolt's side, and the
    // handle survives it: everything above holds one, and the caller asked for
    // the body to change rather than to be replaced. Velocity is carried across
    // so that resizing a falling part does not stop it in mid-air.
    [[nodiscard]] bool updateBody(BodyHandle handle, const BodyDesc& desc)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return false;
        }
        const JPH::ShapeRefC shape = shapeFor(desc.shape);
        if (shape == nullptr) {
            // **The body stays what it was**, which is the useful answer:
            // replacing a working box with nothing would drop the part through
            // the floor. Reported so the caller can say so once rather than
            // retrying it every tick.
            //
            // What actually reaches here is a shape Jolt's builder rejects or a
            // description it cannot make -- and NOT, as this comment used to
            // claim, a coplanar quad: `ConvexHullBuilder` accepts one and gives
            // it a small thickness. Naming a case that does not happen is worse
            // than naming none, because somebody writes a test around it.
            return false;
        }

        const BodyState previous = bodyState(handle);

        // **The pairs are deliberately KEPT** (D151). This called `forgetPairs`,
        // copied from `destroyBody` below, and it was wrong here for the exact
        // reason that makes it right there: a destroyed body's pair "can never
        // appear again", so dropping it is the only way the diff stops referring
        // to something gone. A body being RESHAPED persists -- the record and
        // the handle are the same, only `record->id` changes -- so its pairs are
        // still about a body that exists.
        //
        // Dropping them made `Began`/`Ended` lie in one direction only: the
        // contact vanished from the previous set, reappeared in the next, and
        // was announced as new, with no `Ended` to pair it. A script writing
        // `part.Size` on something a character was standing on got a `Touched`
        // for a contact that never broke, and a script pairing `Touched` with
        // `TouchEnded` leaked a count every time.
        //
        // Keeping them costs nothing and is self-correcting: a reshape that
        // genuinely ends a contact leaves the pair in the previous set and
        // absent from the next, which is exactly the `Ended` the diff exists to
        // emit.
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        bodies.RemoveBody(record->id);
        bodies.DestroyBody(record->id);
        record->id = JPH::BodyID();

        if (!instantiate(*record, handle, desc, shape)) {
            record->alive = false;
            m_freeBodies.push_back(handle.index);
            return false;
        }
        if (desc.motion != MotionType::Static) {
            bodies.SetLinearAndAngularVelocity(record->id, toJolt(previous.linearVelocity),
                                               toJolt(previous.angularVelocity));
        }

        // The Jolt body underneath is a NEW one, so every constraint on it now
        // holds a pointer to the destroyed one. Rebuilt in creation order, so a
        // resized limb stays attached and the solve sequence does not move.
        rebuildConstraintsOn(handle);
        return true;
    }

    // The in-place height edit (ADR 0066). Everything `updateBody` above does --
    // destroy, recreate, re-insert into the broadphase, rebuild constraints --
    // is skipped, because the shape object itself is edited.
    [[nodiscard]] bool updateHeightField(BodyHandle handle, u32 x, u32 z, u32 sizeX, u32 sizeZ,
                                         std::span<const float> heights)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->id.IsInvalid() || sizeX == 0 || sizeZ == 0) {
            return false;
        }
        if (heights.size() < static_cast<core::usize>(sizeX) * sizeZ) {
            return false;
        }

        // **The lock is SCOPED, and that is load-bearing rather than tidy.**
        // `NotifyShapeChanged` below takes a body lock of its own, and taking
        // one while holding another of the same priority is a lock-order
        // violation Jolt detects and reports -- "this can create a deadlock" --
        // and then deadlocks on. It cost an hour of a hung test process to
        // learn, and it was invisible until the physics tests were given the
        // message catalogue that turns `assertFailedImpl`'s report from a hash
        // into a file and a line.
        {
            const JPH::BodyLockWrite lock(m_system.GetBodyLockInterface(), record->id);
            if (!lock.Succeeded()) {
                return false;
            }
            // A `const_cast` because `GetShape` answers const and the edit is
            // the one mutation Jolt sanctions on a live shape. Checked rather
            // than assumed: a body of any other kind returns null from the cast
            // and is refused, so a caller that got its handles crossed gets
            // false instead of memory reinterpreted as a height field.
            const auto* const shape = dynamic_cast<const JPH::HeightFieldShape*>(lock.GetBody().GetShape());
            if (shape == nullptr) {
                return false;
            }

            // **Jolt ASSERTS on a misaligned or out-of-range rectangle**, so
            // every one of its preconditions is a refusal here. An assert is a
            // crash in a debug build and undefined behaviour in a shipping one,
            // and a brush dragged to the edge of a cell reaches all of these.
            const u32 samples = shape->GetSampleCount();
            const u32 block = shape->GetBlockSize();
            if (block == 0 || x % block != 0 || z % block != 0) {
                return false;
            }
            if (x >= samples || z >= samples || x + sizeX > samples || z + sizeZ > samples) {
                return false;
            }

            // **A rectangle is widened to whole blocks.** `SetHeights` reads the
            // samples around the rectangle it is given with `GetHeights`, which
            // asserts on an unaligned start -- so an odd-sized rectangle, the 33
            // samples a terrain tile hands over among them, asserts on the
            // column just past its end. The widened part is filled from the
            // shape's own current samples, so nothing the caller did not name
            // changes. The grid is always a whole number of blocks (the build
            // pads it), so the widened rectangle always fits.
            const u32 fullX = std::min((sizeX + block - 1) / block * block, samples - x);
            const u32 fullZ = std::min((sizeZ + block - 1) / block * block, samples - z);
            auto* const editable = const_cast<JPH::HeightFieldShape*>(shape);
            if (fullX == sizeX && fullZ == sizeZ) {
                editable->SetHeights(x, z, sizeX, sizeZ, heights.data(), static_cast<intptr_t>(sizeX), m_temp);
            }
            else {
                std::vector<float> widened(static_cast<core::usize>(fullX) * fullZ);
                editable->GetHeights(x, z, fullX, fullZ, widened.data(), static_cast<intptr_t>(fullX));
                for (u32 row = 0; row < sizeZ; ++row) {
                    std::copy_n(heights.data() + static_cast<core::usize>(row) * sizeX, sizeX,
                                widened.data() + static_cast<core::usize>(row) * fullX);
                }
                editable->SetHeights(x, z, fullX, fullZ, widened.data(), static_cast<intptr_t>(fullX), m_temp);
            }
        }

        // **The broadphase still holds the old bounds.** `SetHeights` changes
        // the shape's local bounding box, and nothing recomputes the body's
        // world bounds on its own -- so a valley dug below the old minimum is
        // outside the box the broadphase culls against and stops being hit by
        // anything, which is a collider that silently has a hole in it.
        m_system.GetBodyInterface().NotifyShapeChanged(record->id, JPH::Vec3::sZero(), false,
                                                       JPH::EActivation::DontActivate);
        return true;
    }

    void destroyBody(BodyHandle handle)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }

        // BEFORE the body goes. A joint holding a body that is gone is a
        // dangling pointer inside the solver, and nothing about it is loud --
        // the interface states this as a contract because a caller sweeping
        // both must not be the only thing that remembers.
        retireConstraintsOn(handle);

        // A destroyed part does not fire TouchEnded -- the instance is gone,
        // and a signal on an instance nobody can reach is a signal nobody can
        // handle. Dropping its pairs here is what makes that true rather than
        // leaving an event referring to a body that no longer exists.
        forgetPairs(packHandle(handle));

        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        bodies.RemoveBody(record->id);
        bodies.DestroyBody(record->id);
        record->alive = false;
        record->id = JPH::BodyID();
        m_freeBodies.push_back(handle.index);
    }

    void setBodyTransform(BodyHandle handle, const core::CFrameD& transform)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }

        // **A kinematic body MOVES; it does not teleport** (D027).
        //
        // `SetPositionAndRotation` puts a body somewhere and derives no velocity
        // from having done so, so every script-moved kinematic body in this
        // engine had a velocity of zero: a closing door did not push, a piston
        // did not launch, a conveyor did not carry, and a character standing on
        // a moving platform stayed where it was while the platform left.
        //
        // `MoveKinematic` is the call that takes a target and a delta and
        // computes the velocity that gets there. It needs the delta, and the
        // only honest delta is the tick the simulation is ABOUT to take -- so
        // the target is recorded here and spent in `step`, where that number is.
        // Reading a wall clock for it would put a wall clock inside the
        // simulation, which is exactly what R10 forbids.
        if (record->motion == MotionType::Kinematic) {
            // Appended, never searched. Two writes to one body in one tick both
            // land and `step` applies them in order, so the last one wins --
            // which is what two property writes in one tick mean anyway.
            //
            // The first version deduplicated here by scanning the list, and that
            // was quadratic: `churn10k` moves six thousand anchored parts a tick
            // and each write walked every pending move before it, which cost
            // four milliseconds a tick that the benchmark found immediately.
            m_kinematicMoves.push_back(
                PendingMove{record->id, toLocal(transform.position), toJolt(transform.rotation)});
            return;
        }

        m_system.GetBodyInterface().SetPositionAndRotation(
            record->id, toLocal(transform.position), toJolt(transform.rotation),
            record->motion == MotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    }

    void setBodyVelocity(BodyHandle handle, core::Vec3 linear, core::Vec3 angular)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->motion == MotionType::Static) {
            return;
        }
        m_system.GetBodyInterface().SetLinearAndAngularVelocity(record->id, toJolt(linear), toJolt(angular));
    }

    void applyImpulse(BodyHandle handle, core::Vec3 impulse)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->motion != MotionType::Dynamic) {
            return;
        }
        m_system.GetBodyInterface().AddImpulse(record->id, toJolt(impulse));
    }

    void applyAngularImpulse(BodyHandle handle, core::Vec3 impulse)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->motion != MotionType::Dynamic) {
            return;
        }
        m_system.GetBodyInterface().AddAngularImpulse(record->id, toJolt(impulse));
    }

    void applyImpulseAt(BodyHandle handle, core::Vec3 impulse, core::DVec3 point)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->motion != MotionType::Dynamic) {
            return;
        }
        m_system.GetBodyInterface().AddImpulse(record->id, toJolt(impulse), toLocal(point));
    }

    [[nodiscard]] BodyMassProperties bodyMassProperties(BodyHandle handle) const
    {
        BodyMassProperties out;
        const BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return out;
        }
        out.mass = record->mass;
        const JPH::BodyLockRead lock(m_system.GetBodyLockInterface(), record->id);
        if (!lock.Succeeded()) {
            return out;
        }
        const JPH::Body& body = lock.GetBody();
        out.centerOfMass = toWorld(body.GetCenterOfMassPosition());
        if (record->motion != MotionType::Dynamic) {
            return out;
        }
        out.dynamic = true;
        const f32 inverseMass = body.GetMotionProperties()->GetInverseMass();
        out.mass = inverseMass > 0.0f ? 1.0f / inverseMass : 0.0f;
        const JPH::Mat44 inertia = body.GetInverseInertia();
        for (u32 column = 0; column < 3; ++column) {
            for (u32 row = 0; row < 3; ++row) {
                out.inverseInertia.m[column][row] = inertia(row, column);
            }
        }
        return out;
    }

    void setBodyDamping(BodyHandle handle, f32 linear, f32 angular)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->motion == MotionType::Static) {
            return;
        }
        const JPH::BodyLockWrite lock(m_system.GetBodyLockInterface(), record->id);
        if (!lock.Succeeded()) {
            return;
        }
        if (JPH::MotionProperties* motion = lock.GetBody().GetMotionPropertiesUnchecked(); motion != nullptr) {
            motion->SetLinearDamping(std::max(linear, 0.0f));
            motion->SetAngularDamping(std::max(angular, 0.0f));
        }
    }

    void setPairCollidable(BodyHandle first, BodyHandle second, bool collidable)
    {
        if (collidable) {
            m_contacts.unexclude(packHandle(first), packHandle(second));
        }
        else {
            m_contacts.exclude(packHandle(first), packHandle(second));
        }
        // A pair asleep against each other would otherwise stay as it was.
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        for (const BodyHandle handle : {first, second}) {
            if (const BodyRecord* record = resolve(handle); record != nullptr && record->motion != MotionType::Static) {
                bodies.ActivateBody(record->id);
            }
        }
    }

    void setBodyMaterial(BodyHandle handle, f32 friction, f32 restitution)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        bodies.SetFriction(record->id, friction);
        bodies.SetRestitution(record->id, restitution);
    }

    void setBodyFlags(BodyHandle handle, bool collidable, bool queryable)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        record->queryable = queryable;
        if (record->collidable != collidable) {
            record->collidable = collidable;
            m_system.GetBodyInterface().SetIsSensor(record->id, !collidable);
        }
    }

    void setBodyGroup(BodyHandle handle, CollisionGroup group)
    {
        BodyRecord* record = resolve(handle);
        if (record == nullptr || record->group == group) {
            return;
        }
        record->group = group;
        m_system.GetBodyInterface().SetObjectLayer(record->id,
                                                   encodeLayer(group, record->motion != MotionType::Static));
    }

    [[nodiscard]] BodyState bodyState(BodyHandle handle) const
    {
        const BodyRecord* record = resolve(handle);
        BodyState state;
        if (record == nullptr) {
            return state;
        }
        const JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies.GetPositionAndRotation(record->id, position, rotation);
        state.transform.position = toWorld(position);
        state.transform.rotation = fromJolt(rotation);
        JPH::Vec3 linear;
        JPH::Vec3 angular;
        bodies.GetLinearAndAngularVelocity(record->id, linear, angular);
        state.linearVelocity = fromJolt(linear);
        state.angularVelocity = fromJolt(angular);
        state.active = bodies.IsActive(record->id);
        return state;
    }

    void collectActiveBodies(std::vector<ActiveBody>& out) const
    {
        JPH::BodyIDVector active;
        m_system.GetActiveBodies(JPH::EBodyType::RigidBody, active);

        const usize first = out.size();
        for (const JPH::BodyID& id : active) {
            const u64 packed = m_system.GetBodyInterface().GetUserData(id);
            const BodyHandle handle = unpackHandle(packed);
            const BodyRecord* record = resolve(handle);
            if (record == nullptr) {
                continue;
            }
            out.push_back(ActiveBody{handle, record->userData, bodyState(handle)});
        }

        // Jolt documents this list as unordered under a multi-threaded job
        // system (Docs/Architecture.md:807). Sorting by our own handle makes
        // the order a property of when the caller created the body, which is
        // the scene's deterministic walk.
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                  [](const ActiveBody& a, const ActiveBody& b) { return packHandle(a.body) < packHandle(b.body); });
    }

    // --- Simulation -----------------------------------------------------------

    // --- Rollback (ADR 0101) ---------------------------------------------------
    //
    // **The solver's whole state, and ours beside it**: every body's position,
    // velocity and sleep, the contact cache and the constraints' warm start
    // (`PhysicsSystem::SaveState` with everything), each character's own state,
    // which Jolt keeps outside the system, and the contact pairs the next step's
    // begin/end diff compares against. Without the contact cache a restored
    // world steps differently from the one saved; without the pairs, it reports
    // touches that already began.
    //
    // **A restore puts state back into the SAME bodies**, and refuses anything
    // else. The blob opens with which slots are alive at which generation, and
    // the origin; a world that has gained or lost a body since, or moved its
    // origin, is a different world, and restoring into it would be Jolt reading
    // one body's state into another.
    void saveState(std::vector<u8>& out) const
    {
        JPH::StateRecorderImpl recorder;
        writeLayout(recorder);
        m_system.SaveState(recorder, JPH::EStateRecorderState::All);
        for (const CharacterRecord& record : m_characters) {
            if (record.alive && record.character != nullptr)
                record.character->SaveState(recorder);
        }
        writePairs(recorder, m_previousPairs);
        const u64 characterPairs = m_previousCharacterPairs.size();
        recorder.Write(characterPairs);
        for (const CharacterPair& pair : m_previousCharacterPairs) {
            recorder.Write(pair.character);
            recorder.Write(pair.other);
            recorder.Write(pair.otherIsCharacter);
        }
        const std::string data = recorder.GetData();
        out.assign(data.begin(), data.end());
    }

    // A replica's island (ADR 0133): each body's own state and each
    // character's, by handle, and nothing between them.
    [[nodiscard]] bool saveIsland(std::span<const BodyHandle> bodies, std::span<const CharacterHandle> characters,
                                  std::vector<u8>& out) const
    {
        JPH::StateRecorderImpl recorder;
        const u64 bodyCount = bodies.size();
        recorder.Write(bodyCount);
        for (const BodyHandle handle : bodies) {
            const BodyRecord* record = resolve(handle);
            if (record == nullptr)
                return false;
            recorder.Write(packHandle(handle));
            const JPH::BodyLockRead lock(m_system.GetBodyLockInterface(), record->id);
            if (!lock.Succeeded())
                return false;
            m_system.SaveBodyState(lock.GetBody(), recorder);
            // Awake or asleep: a body's own state does not say, and a crate
            // restored awake that had been asleep is stepped differently.
            recorder.Write(lock.GetBody().IsActive());
        }
        const u64 characterCount = characters.size();
        recorder.Write(characterCount);
        for (const CharacterHandle handle : characters) {
            const CharacterRecord* record = resolve(handle);
            if (record == nullptr || record->character == nullptr)
                return false;
            recorder.Write(packHandle(handle));
            record->character->SaveState(recorder);
            // **And what it was touching** (G37): stepped again from here, a
            // touch it began after is begun again, and one it was already in
            // is not.
            u64 touching = 0;
            for (const CharacterPair& pair : m_previousCharacterPairs)
                touching += pair.character == packHandle(handle) ? 1 : 0;
            recorder.Write(touching);
            for (const CharacterPair& pair : m_previousCharacterPairs) {
                if (pair.character != packHandle(handle))
                    continue;
                recorder.Write(pair.other);
                recorder.Write(pair.otherIsCharacter);
            }
        }
        // **And the contacts the solver warm-starts from.** Without them a
        // step taken again started from the impulses of the newest step, not
        // of the one restored, and even a crate at rest came out a few
        // micrometres elsewhere -- which a crate pushed into another made
        // centimetres. Every contact the cache holds involves a body that
        // moves, and on a replica those are the island's.
        m_system.SaveState(recorder, JPH::EStateRecorderState::Contacts);
        const std::string data = recorder.GetData();
        out.assign(data.begin(), data.end());
        return true;
    }

    [[nodiscard]] bool restoreIsland(std::span<const u8> blob)
    {
        JPH::StateRecorderImpl recorder;
        recorder.WriteBytes(blob.data(), blob.size());
        recorder.Rewind();
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        u64 bodyCount = 0;
        recorder.Read(bodyCount);
        for (u64 at = 0; at < bodyCount && !recorder.IsFailed(); ++at) {
            u64 packed = 0;
            recorder.Read(packed);
            const BodyRecord* record = resolve(unpackHandle(packed));
            if (record == nullptr)
                return false;
            {
                const JPH::BodyLockWrite lock(m_system.GetBodyLockInterface(), record->id);
                if (!lock.Succeeded())
                    return false;
                m_system.RestoreBodyState(lock.GetBody(), recorder);
            }
            bool active = false;
            recorder.Read(active);
            // The broad phase learns where it is now, and the body is awake
            // or asleep as it was.
            bodies.SetPositionAndRotation(record->id, bodies.GetPosition(record->id), bodies.GetRotation(record->id),
                                          JPH::EActivation::DontActivate);
            if (active)
                bodies.ActivateBody(record->id);
            else
                bodies.DeactivateBody(record->id);
        }
        u64 characterCount = 0;
        recorder.Read(characterCount);
        for (u64 at = 0; at < characterCount && !recorder.IsFailed(); ++at) {
            u64 packed = 0;
            recorder.Read(packed);
            CharacterRecord* record = resolve(unpackCharacter(packed));
            if (record == nullptr || record->character == nullptr)
                return false;
            record->character->RestoreState(recorder);
            u64 touching = 0;
            recorder.Read(touching);
            std::erase_if(m_previousCharacterPairs,
                          [&](const CharacterPair& pair) { return pair.character == packed; });
            for (u64 pair = 0; pair < touching && !recorder.IsFailed(); ++pair) {
                CharacterPair restored;
                restored.character = packed;
                recorder.Read(restored.other);
                recorder.Read(restored.otherIsCharacter);
                m_previousCharacterPairs.push_back(restored);
            }
            std::sort(m_previousCharacterPairs.begin(), m_previousCharacterPairs.end());
        }
        if (recorder.IsFailed() || !m_system.RestoreState(recorder))
            return false;
        return !recorder.IsFailed();
    }

    [[nodiscard]] bool restoreState(std::span<const u8> blob)
    {
        JPH::StateRecorderImpl recorder;
        recorder.WriteBytes(blob.data(), blob.size());
        recorder.Rewind();
        JPH::StateRecorderImpl expected;
        writeLayout(expected);
        const std::string layout = expected.GetData();
        std::string found(layout.size(), '\0');
        if (blob.size() < layout.size())
            return false;
        recorder.ReadBytes(found.data(), found.size());
        if (recorder.IsFailed() || found != layout)
            return false;
        if (!m_system.RestoreState(recorder))
            return false;
        for (CharacterRecord& record : m_characters) {
            if (record.alive && record.character != nullptr)
                record.character->RestoreState(recorder);
        }
        std::vector<ContactPair> pairs;
        if (!readPairs(recorder, pairs))
            return false;
        u64 characterPairs = 0;
        recorder.Read(characterPairs);
        std::vector<CharacterPair> restored;
        for (u64 at = 0; at < characterPairs && !recorder.IsFailed(); ++at) {
            CharacterPair pair;
            recorder.Read(pair.character);
            recorder.Read(pair.other);
            recorder.Read(pair.otherIsCharacter);
            restored.push_back(pair);
        }
        if (recorder.IsFailed())
            return false;
        m_previousPairs = std::move(pairs);
        m_previousCharacterPairs = std::move(restored);
        // A move queued before the restore targets a world that no longer is.
        m_kinematicMoves.clear();
        return true;
    }

    void step(f32 fixedDt)
    {
        m_contacts.clear();

        // The kinematic targets a script set this tick, spent against the tick
        // the simulation is about to take (D027). In the order they were
        // written, which is `applyScene`'s pool order and therefore a pure
        // function of the operation sequence (R10).
        if (!m_kinematicMoves.empty()) {
            JPH::BodyInterface& bodies = m_system.GetBodyInterface();
            for (const PendingMove& pending : m_kinematicMoves)
                bodies.MoveKinematic(pending.id, pending.position, pending.rotation, fixedDt);
            m_kinematicMoves.clear();
        }

        const auto begin = std::chrono::steady_clock::now();
        // One collision step per tick. Jolt allows several sub-steps per call;
        // the sim tick already IS the substep grid, and a second, hidden one
        // would make `FixedTimestep` mean two different things.
        const JPH::EPhysicsUpdateError updateError = m_system.Update(fixedDt, 1, &m_temp, &m_jobs);
        reportUpdateError(updateError);
        const auto end = std::chrono::steady_clock::now();
        m_timings.step = std::chrono::duration<f64>(end - begin).count();

        buildContactEvents();
        collectCharacterContacts();
        buildCharacterContactEvents();
    }

    // **What Jolt returned, said out loud, once.**
    //
    // The return value used to be discarded, and Jolt's own assert then fired
    // with "an error occurred during the physics update, see
    // EPhysicsUpdateError for more information" -- a message that tells you to
    // go and look at something the log does not contain. Worse, in a build with
    // asserts off there is no message at all, and every one of these means the
    // same thing: **contacts were silently dropped**, which reads as parts
    // sinking through each other rather than as an error.
    //
    // Once per distinct kind rather than per tick: a full buffer is full on
    // every tick that follows, and a line a frame turns a log into a wall.
    void reportUpdateError(JPH::EPhysicsUpdateError error) noexcept
    {
        if (error == JPH::EPhysicsUpdateError::None)
            return;

        const auto fresh = static_cast<core::u32>(error) & ~m_reportedUpdateErrors;
        if (fresh == 0)
            return;
        m_reportedUpdateErrors |= fresh;

        // Named rather than numbered, and each name says which budget to raise.
        // All three come out of `PhysicsSystem::Init`'s contact arguments, which
        // `contactBudget` derives from `WorldDesc::maxBodies`.
        struct Named
        {
            JPH::EPhysicsUpdateError bit;
            std::string_view text;
        };
        static constexpr std::array<Named, 3> kNames{
            Named{JPH::EPhysicsUpdateError::ManifoldCacheFull, "the manifold cache is full"},
            Named{JPH::EPhysicsUpdateError::BodyPairCacheFull, "the body-pair cache is full"},
            Named{JPH::EPhysicsUpdateError::ContactConstraintsFull, "the contact constraint buffer is full"},
        };
        for (const Named& named : kNames) {
            if ((fresh & static_cast<core::u32>(named.bit)) == 0)
                continue;
            const std::array<core::I18nArg, 2> args{
                core::I18nArg{"reason", named.text},
                core::I18nArg{"budget", static_cast<core::i64>(m_contactBudget)},
            };
            core::log(core::LogLevel::Warn, ENG_TR("physics.jolt.warn.update_error"), args);
        }
    }

    [[nodiscard]] std::span<const ContactEvent> contacts() const noexcept { return m_events; }
    [[nodiscard]] StepTimings timings() const noexcept { return m_timings; }

    // --- Queries --------------------------------------------------------------

    [[nodiscard]] bool raycast(const RayD& ray, const QueryFilter& filter, RayHit& outHit) const
    {
        const JPH::RRayCast cast{toLocal(ray.origin), toJolt(ray.direction)};
        const BodyFilterAdapter bodyFilter(*this, filter);
        const LayerFilterAdapter layerFilter(filter);

        ClosestRayCollector collector;
        m_system.GetNarrowPhaseQuery().CastRay(cast, JPH::RayCastSettings{}, collector, JPH::BroadPhaseLayerFilter{},
                                               layerFilter, bodyFilter);
        if (!collector.has) {
            return false;
        }

        const BodyHandle handle = unpackHandle(m_system.GetBodyInterface().GetUserData(collector.hit.mBodyID));
        const BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return false;
        }

        const JPH::RVec3 point = cast.GetPointOnRay(collector.hit.mFraction);
        outHit.body = handle;
        outHit.userData = record->userData;
        outHit.position = toWorld(point);
        // The fraction is along the ray as given, and the ray's length is the
        // direction's magnitude -- `Workspace:Raycast(origin, direction)` takes
        // an unnormalised direction whose length IS the range.
        outHit.distance = collector.hit.mFraction * core::length(ray.direction);
        outHit.normal = surfaceNormal(collector.hit.mBodyID, collector.hit.mSubShapeID2, point);
        return true;
    }

    [[nodiscard]] bool spherecast(const RayD& ray, f32 radius, const QueryFilter& filter, RayHit& outHit) const
    {
        const JPH::SphereShape sphere(std::max(radius, 0.005f));
        const JPH::RShapeCast cast(&sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(toLocal(ray.origin)),
                                   toJolt(ray.direction));
        const BodyFilterAdapter bodyFilter(*this, filter);
        const LayerFilterAdapter layerFilter(filter);

        ClosestShapeCollector collector;
        m_system.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{}, JPH::RVec3::sZero(), collector,
                                                 JPH::BroadPhaseLayerFilter{}, layerFilter, bodyFilter);
        if (!collector.has) {
            return false;
        }

        const BodyHandle handle = unpackHandle(m_system.GetBodyInterface().GetUserData(collector.hit.mBodyID2));
        const BodyRecord* record = resolve(handle);
        if (record == nullptr) {
            return false;
        }

        outHit.body = handle;
        outHit.userData = record->userData;
        outHit.position = toWorld(JPH::RVec3(collector.hit.mContactPointOn2));
        outHit.distance = collector.hit.mFraction * core::length(ray.direction);
        const JPH::Vec3 axis = collector.hit.mPenetrationAxis;
        outHit.normal = axis.IsNearZero() ? core::Vec3{0.0f, 1.0f, 0.0f} : fromJolt(-axis.Normalized());
        return true;
    }

    void overlapBox(const core::CFrameD& transform, core::Vec3 size, const QueryFilter& filter,
                    std::vector<u64>& out) const
    {
        const JPH::BoxShape box(JPH::Vec3(std::max(size.x * 0.5f, 0.005f), std::max(size.y * 0.5f, 0.005f),
                                          std::max(size.z * 0.5f, 0.005f)),
                                0.0f);
        const JPH::RMat44 centerOfMass =
            JPH::RMat44::sRotationTranslation(toJolt(transform.rotation), toLocal(transform.position));
        const BodyFilterAdapter bodyFilter(*this, filter);
        const LayerFilterAdapter layerFilter(filter);

        OverlapCollector collector;
        m_system.GetNarrowPhaseQuery().CollideShape(&box, JPH::Vec3::sOne(), centerOfMass, JPH::CollideShapeSettings{},
                                                    JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter{},
                                                    layerFilter, bodyFilter);

        const usize first = out.size();
        for (const JPH::BodyID& id : collector.bodies) {
            const BodyHandle handle = unpackHandle(m_system.GetBodyInterface().GetUserData(id));
            const BodyRecord* record = resolve(handle);
            if (record != nullptr) {
                out.push_back(record->userData);
            }
        }
        // One entry per body, in an order the caller can rely on: a collide
        // query reports one hit per sub-shape, and its traversal order is
        // explicitly not deterministic (Docs/Architecture.md:805).
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
        out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(first), out.end()), out.end());
    }

    void overlapSphere(core::DVec3 center, f32 radius, const QueryFilter& filter, std::vector<u64>& out) const
    {
        const JPH::SphereShape ball(std::max(radius, 0.005f));
        const JPH::RMat44 centerOfMass = JPH::RMat44::sTranslation(toLocal(center));
        const BodyFilterAdapter bodyFilter(*this, filter);
        const LayerFilterAdapter layerFilter(filter);

        OverlapCollector collector;
        m_system.GetNarrowPhaseQuery().CollideShape(&ball, JPH::Vec3::sOne(), centerOfMass, JPH::CollideShapeSettings{},
                                                    JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter{},
                                                    layerFilter, bodyFilter);

        const usize first = out.size();
        for (const JPH::BodyID& id : collector.bodies) {
            const BodyHandle handle = unpackHandle(m_system.GetBodyInterface().GetUserData(id));
            const BodyRecord* record = resolve(handle);
            if (record != nullptr) {
                out.push_back(record->userData);
            }
        }
        // As `overlapBox`: one entry a body, in an order the caller can rely on.
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
        out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(first), out.end()), out.end());
    }

    // --- Characters -----------------------------------------------------------

    [[nodiscard]] CharacterHandle createCharacter(const CharacterDesc& desc)
    {
        const f32 radius = std::max(desc.diameter * 0.5f, 0.01f);
        const f32 halfCylinder = std::max(desc.height * 0.5f - radius, 0.01f);

        JPH::CharacterVirtualSettings settings;
        settings.mShape = JPH::ShapeRefC(new JPH::CapsuleShape(halfCylinder, radius));
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(desc.maxSlopeAngle);
        settings.mMass = desc.mass;
        // **Not `mEnhancedInternalEdgeRemoval`, which a moving body sets** (ADR
        // 0143, amended by D574). With it a character standing by the edge two
        // facets of the ground share keeps one contact for the two, and its
        // step is solved along the one plane; the sweep that checks the step
        // then meets the other facet a little way on, is taken back by the
        // controller's padding to where the character already stands, and
        // the whole step is thrown away -- on the ground, asked to move, not
        // moving, every tick, wherever a step is long enough to reach the
        // next facet. Without it the solver has both planes and slides over
        // the crease. At a seam between chunks it bought a character nothing:
        // the band and its filter measure the same with and without (ADR
        // 0143's table, `ring+f` against `ring+f+e`).
        settings.mEnhancedInternalEdgeRemoval = false;
        // A character with no inner body is invisible to the simulation: other
        // bodies pass through it, which is not what "a capsule standing on a
        // seesaw" means. The inner body is what makes the character push and be
        // pushed against, and it is why a crate the capsule walks into moves.
        //
        // It is also what makes TWO characters collide, and that is worth
        // stating because the obvious reading of Jolt says they cannot: a
        // `CharacterVirtual` is not a `Body`, and `mCharacterVsCharacterCollision`
        // is null unless somebody sets it (`CharacterVirtual.h:696`). Both true
        // -- and beside the point here, because the thing another character
        // sweeps into is this inner body, which IS a `Body` and is in the
        // broad phase like any other.
        //
        // So `CharacterVsCharacterCollisionSimple` (`CharacterVirtual.h:246`) is
        // deliberately not used. It would be a second, redundant source of the
        // same contact; it is brute force over every registered character where
        // the inner bodies are already indexed by the broad phase; and its
        // `mCharacters` walk has no filter, so it would make character-against-
        // character the one pair in the world that ignores `CollisionGroup`.
        // The tests that hold this down are "two characters cannot walk through
        // each other" and "two characters whose groups do not collide walk
        // through each other" -- both go red if this line is removed.
        settings.mInnerBodyShape = settings.mShape;
        settings.mInnerBodyLayer = encodeLayer(desc.group, true);
        // No shape offset: `transform` is the character's CENTRE, like every
        // other `BasePart`'s, so Jolt's position and the capsule's centre are
        // the same point.
        //
        // The first version put the origin at the feet, on the reasoning that a
        // character stands somewhere. It made `CharacterBody` the one BasePart
        // whose `Position` did not mean the middle of its `Size`, and the
        // debug-draw bridge showed it the first frame it drew: the collider
        // capsule floated a half-height above the part's own box.

        u32 slot = 0;
        if (!m_freeCharacters.empty()) {
            slot = m_freeCharacters.back();
            m_freeCharacters.pop_back();
        }
        else {
            slot = static_cast<u32>(m_characters.size());
            m_characters.emplace_back();
        }

        CharacterRecord& record = m_characters[slot];
        record.generation = record.generation + 1 == 0 ? 1 : record.generation + 1;
        record.alive = true;
        record.stepHeight = desc.stepHeight;
        record.userData = desc.userData;
        record.layer = settings.mInnerBodyLayer;
        record.character = new JPH::CharacterVirtual(&settings, toLocal(desc.transform.position),
                                                     toJolt(desc.transform.rotation), desc.userData, &m_system);
        record.character->SetListener(&m_characterContacts);

        return CharacterHandle{slot, record.generation};
    }

    void destroyCharacter(CharacterHandle handle)
    {
        CharacterRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        forgetCharacterPairs(packHandle(handle));
        removeStandIn(*record);
        record->character = nullptr;
        record->alive = false;
        m_freeCharacters.push_back(handle.index);
    }

    void setCharacterStandIn(CharacterHandle handle, const core::CFrameD* where)
    {
        CharacterRecord* record = resolve(handle);
        if (record == nullptr || record->character == nullptr)
            return;
        if (where == nullptr) {
            removeStandIn(*record);
            return;
        }
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        if (record->standIn.IsInvalid()) {
            // The character's own capsule, on the layer its inner body is on,
            // so the groups that decide who meets the character decide who
            // meets this. Kinematic and never moved by velocity: it is put.
            JPH::BodyCreationSettings settings(record->character->GetShape(), toLocal(where->position),
                                               toJolt(where->rotation), JPH::EMotionType::Kinematic, record->layer);
            settings.mUserData = kStandInUserData;
            settings.mAllowSleeping = false;
            record->standIn = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            return;
        }
        bodies.SetPositionAndRotation(record->standIn, toLocal(where->position), toJolt(where->rotation),
                                      JPH::EActivation::DontActivate);
    }

    void removeStandIn(CharacterRecord& record)
    {
        if (record.standIn.IsInvalid())
            return;
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        bodies.RemoveBody(record.standIn);
        bodies.DestroyBody(record.standIn);
        record.standIn = JPH::BodyID();
    }

    void moveCharacter(CharacterHandle handle, core::Vec3 velocity, f32 fixedDt, bool walking)
    {
        CharacterRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }

        // **The character inherits its ground's motion** (D027). Without this a
        // platform slides out from under a player who stays exactly where they
        // were -- the first thing anybody notices about a moving platform, and
        // the second half of the same defect: the velocity below reads zero
        // unless kinematic bodies are MOVED rather than teleported.
        //
        // Only while grounded. A character in mid-air is not standing on
        // anything, and carrying the last platform's velocity through a jump
        // would launch it.
        JPH::Vec3 inherited = JPH::Vec3::sZero();
        if (record->character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround)
            inherited = record->character->GetGroundVelocity();

        record->character->SetLinearVelocity(toJolt(velocity) + inherited);

        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = JPH::Vec3(0.0f, record->stepHeight, 0.0f);
        settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -record->stepHeight, 0.0f);

        // The filters are the world's, not the defaults. A default-constructed
        // `ObjectLayerFilter` accepts every layer, which made the character the
        // one thing in the world that ignored `CollisionGroup`: a wall in a
        // group the character's group is set never to collide with still
        // stopped it, and nothing said so. `GetDefaultLayerFilter` asks the same
        // `ObjectPairFilter` every body pair goes through, against the
        // character's own layer.
        const JPH::RVec3 before = record->character->GetPosition();
        const bool wasSupported = record->character->IsSupported();
        record->character->ExtendedUpdate(
            fixedDt, toJolt(m_gravity), settings, m_system.GetDefaultBroadPhaseLayerFilter(record->layer),
            m_system.GetDefaultLayerFilter(record->layer), JPH::BodyFilter{}, JPH::ShapeFilter{}, m_temp);
        // **A walk up a slope stays on it** (D532). The controller sticks to
        // the floor only when the step did not rise -- and a walk up a slope
        // rises, following the ground. Over the crease where a slope of
        // facets turns less steep, the step carried it up off the ground, and
        // it was in the air for a tick, then landed: `Grounded` flickered and
        // `Landed` fired, every few ticks up every hill. Walking -- not
        // jumping, not thrown -- it is put back on what is within a step
        // below, as it would have been walking down.
        if (walking && wasSupported && !record->character->IsSupported() && record->stepHeight > 0.0f) {
            (void)record->character->StickToFloor(
                JPH::Vec3(0.0f, -record->stepHeight, 0.0f), m_system.GetDefaultBroadPhaseLayerFilter(record->layer),
                m_system.GetDefaultLayerFilter(record->layer), JPH::BodyFilter{}, JPH::ShapeFilter{}, m_temp);
        }
        if (fixedDt > 0.0f) {
            const JPH::Vec3 travelled = JPH::Vec3(record->character->GetPosition() - before);
            record->moved = fromJolt(travelled / fixedDt);
        }
    }

    void setCharacterTransform(CharacterHandle handle, const core::CFrameD& transform)
    {
        CharacterRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        record->character->SetPosition(toLocal(transform.position));
        record->character->SetRotation(toJolt(transform.rotation));
        // **The contacts of where it is now, not where it was.** A controller
        // keeps the contacts of its last update, and the next update is
        // resolved against them: a character put somewhere new still leaned on
        // the wall it had been standing against, and its first step away from
        // it was blocked. Found replaying a replica's prediction (ADR 0076),
        // which puts a character back where the authority said every time it
        // corrects one -- and a script teleporting a character hit it too.
        record->character->RefreshContacts(m_system.GetDefaultBroadPhaseLayerFilter(record->layer),
                                           m_system.GetDefaultLayerFilter(record->layer), JPH::BodyFilter{},
                                           JPH::ShapeFilter{}, m_temp);
    }

    void nudgeCharacter(CharacterHandle handle, const core::CFrameD& transform)
    {
        if (CharacterRecord* record = resolve(handle); record != nullptr) {
            record->character->SetPosition(toLocal(transform.position));
            record->character->SetRotation(toJolt(transform.rotation));
        }
    }

    [[nodiscard]] CharacterState characterState(CharacterHandle handle) const
    {
        CharacterState state;
        const CharacterRecord* record = resolve(handle);
        if (record == nullptr) {
            return state;
        }

        state.transform.position = toWorld(record->character->GetPosition());
        state.transform.rotation = fromJolt(record->character->GetRotation());
        // What happened, not what was asked for (`CharacterRecord::moved`).
        state.linearVelocity = record->moved;

        // `Enum.CharacterState` has two items, and Jolt has three: standing on
        // ground too steep to walk on is `OnSteepGround`, which is airborne as
        // far as a jump is concerned and grounded as far as a fall is. It reads
        // as Airborne here, because the property a script branches on is "may I
        // jump".
        const JPH::CharacterBase::EGroundState ground = record->character->GetGroundState();
        state.ground = ground == JPH::CharacterBase::EGroundState::OnGround ? CharacterGround::Grounded
                                                                            : CharacterGround::Airborne;
        state.groundNormal = fromJolt(record->character->GetGroundNormal());

        const JPH::BodyID groundId = record->character->GetGroundBodyID();
        if (!groundId.IsInvalid()) {
            const BodyHandle body = unpackHandle(m_system.GetBodyInterface().GetUserData(groundId));
            const BodyRecord* groundRecord = resolve(body);
            if (groundRecord != nullptr) {
                state.groundBody = body;
                state.groundUserData = groundRecord->userData;
            }
        }
        return state;
    }

    // --- Collision groups -----------------------------------------------------

    [[nodiscard]] CollisionMatrix& matrix() noexcept { return m_matrix; }
    [[nodiscard]] const CollisionMatrix& matrix() const noexcept { return m_matrix; }

    void debugDraw([[maybe_unused]] IDebugDrawSink& sink)
    {
#ifdef JPH_DEBUG_RENDERER
        DebugBridge bridge(sink, m_origin);
        // **Body by body rather than `DrawBodies`**, so each is drawn where the
        // sink says it is on screen (`IDebugDrawSink::drawnPose`). Coloured by
        // how it moves -- still, moved by a script, simulated -- which is the
        // question a person looking at colliders is usually asking.
        const JPH::BodyLockInterfaceNoLock& locks = m_system.GetBodyLockInterfaceNoLock();
        for (const BodyRecord& record : m_bodies) {
            if (!record.alive)
                continue;
            const JPH::BodyLockRead lock(locks, record.id);
            if (!lock.Succeeded())
                continue;
            const JPH::Body& body = lock.GetBody();
            const JPH::Shape* shape = body.GetShape();
            JPH::RMat44 transform = body.GetCenterOfMassTransform();
            if (const std::optional<core::CFrameD> pose = sink.drawnPose(record.userData); pose.has_value()) {
                transform =
                    JPH::RMat44::sRotationTranslation(toJolt(pose->rotation), toJoltPosition(pose->position - m_origin))
                        .PreTranslated(shape->GetCenterOfMass());
            }
            const JPH::Color color = record.motion == MotionType::Static      ? JPH::Color(154, 154, 154)
                                     : record.motion == MotionType::Kinematic ? JPH::Color(64, 255, 64)
                                                                              : JPH::Color(255, 208, 64);
            shape->Draw(&bridge, transform, JPH::Vec3::sOne(), color, false, true);
        }
#endif
    }

    [[nodiscard]] const BodyRecord* resolve(BodyHandle handle) const noexcept
    {
        if (handle.index >= m_bodies.size()) {
            return nullptr;
        }
        const BodyRecord& record = m_bodies[handle.index];
        return record.alive && record.generation == handle.generation ? &record : nullptr;
    }

    // --- Constraints ----------------------------------------------------------

    [[nodiscard]] ConstraintHandle createConstraint(const ConstraintDesc& desc)
    {
        // Two bodies, and not the same one twice: Jolt asserts on a self-joint
        // in a debug build and solves nonsense in a release one. A drive is
        // the one type whose first body may be none, which is the world.
        const bool againstWorld = desc.type == ConstraintType::Drive && !desc.first.valid();
        if ((!againstWorld && resolve(desc.first) == nullptr) || resolve(desc.second) == nullptr ||
            desc.first == desc.second) {
            return {};
        }

        JPH::Ref<JPH::TwoBodyConstraint> built = buildConstraint(desc);
        if (built == nullptr) {
            return {};
        }

        u32 slot = 0;
        if (!m_freeConstraints.empty()) {
            slot = m_freeConstraints.back();
            m_freeConstraints.pop_back();
        }
        else {
            slot = static_cast<u32>(m_constraints.size());
            m_constraints.emplace_back();
        }

        ConstraintRecord& record = m_constraints[slot];
        record.generation = record.generation + 1 == 0 ? 1 : record.generation + 1;
        record.alive = true;
        record.first = desc.first;
        record.second = desc.second;
        record.desc = desc;
        record.constraint = built;
        m_system.AddConstraint(built);
        attachMotor(record);
        applyExclusion(desc);
        return ConstraintHandle{slot, record.generation};
    }

    void destroyConstraint(ConstraintHandle handle)
    {
        ConstraintRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        retireConstraint(*record);
        record->alive = false;
        m_freeConstraints.push_back(handle.index);
    }

    // A body the solver has put to sleep does not notice that the joint holding
    // it changed. It stays exactly where it was, for ever, and the symptom is a
    // constraint that "did not apply" -- which is indistinguishable from a bug
    // in the rebuild until you look at the activation state.
    void wakeBoth(const ConstraintRecord& record)
    {
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        if (const BodyRecord* first = resolve(record.first); first != nullptr) {
            bodies.ActivateBody(first->id);
        }
        if (const BodyRecord* second = resolve(record.second); second != nullptr) {
            bodies.ActivateBody(second->id);
        }
    }

    void setConstraintEnabled(ConstraintHandle handle, bool enabled)
    {
        if (ConstraintRecord* record = resolve(handle); record != nullptr) {
            // The constraint stays IN the world. Removing and re-adding it would
            // move it to the end of the solve order, and a ragdoll that toggled
            // itself off and on would simulate differently afterwards.
            record->constraint->SetEnabled(enabled);
            if (record->motor != nullptr) {
                record->motor->SetEnabled(enabled);
            }
            wakeBoth(*record);
        }
    }

    void updateConstraint(ConstraintHandle handle, const ConstraintDesc& desc)
    {
        ConstraintRecord* record = resolve(handle);
        if (record == nullptr) {
            return;
        }
        // The two bodies and the type are what a constraint IS. Changing them is
        // a different joint and the caller rebuilds; swapping them here would
        // change what the solver holds without anything saying so.
        ConstraintDesc next = desc;
        next.type = record->desc.type;
        next.first = record->first;
        next.second = record->second;

        JPH::Ref<JPH::TwoBodyConstraint> built = buildConstraint(next);
        if (built == nullptr) {
            return;
        }
        const bool wasEnabled = record->constraint->GetEnabled();
        m_system.RemoveConstraint(record->constraint);
        detachMotor(*record);
        dropExclusion(record->desc);
        record->constraint = built;
        record->desc = next;
        built->SetEnabled(wasEnabled);
        m_system.AddConstraint(built);
        attachMotor(*record);
        applyExclusion(next);
        wakeBoth(*record);
    }

    // The motor and the distance, set on the joint that is there (see the
    // interface). What it set is kept in the record, so a rebuild -- a resized
    // limb -- builds the joint as it was last driven.
    void driveConstraint(ConstraintHandle handle, const ConstraintDesc& desc)
    {
        ConstraintRecord* record = resolve(handle);
        if (record == nullptr || record->constraint == nullptr) {
            return;
        }
        ConstraintDesc& kept = record->desc;
        if (kept.type == ConstraintType::Drive) {
            // **Set every time, and woken only by a change.** A position
            // target is said in the world and kept in the solver's own space,
            // so a rebase moves it; and a restored snapshot brings back the
            // targets of the tick it was taken in. Neither is a change of
            // what was asked, so neither may be skipped as one.
            const bool changed = !(kept.drive == desc.drive);
            kept.drive = desc.drive;
            driveSixAxis(static_cast<JPH::SixDOFConstraint*>(record->constraint.GetPtr()), kept);
            if (changed) {
                wakeBoth(*record);
            }
            return;
        }
        const bool moved = kept.motor != desc.motor || kept.motorTarget != desc.motorTarget ||
                           kept.motorMaxForce != desc.motorMaxForce || kept.motorStiffness != desc.motorStiffness ||
                           kept.motorDamping != desc.motorDamping || kept.motorFrequency != desc.motorFrequency ||
                           kept.motorDampingRatio != desc.motorDampingRatio ||
                           !(kept.motorOrientation == desc.motorOrientation) || kept.minDistance != desc.minDistance ||
                           kept.maxDistance != desc.maxDistance;
        if (!moved) {
            return;
        }
        kept.motor = desc.motor;
        kept.motorTarget = desc.motorTarget;
        kept.motorMaxForce = desc.motorMaxForce;
        kept.motorStiffness = desc.motorStiffness;
        kept.motorDamping = desc.motorDamping;
        kept.motorFrequency = desc.motorFrequency;
        kept.motorDampingRatio = desc.motorDampingRatio;
        kept.motorOrientation = desc.motorOrientation;
        kept.minDistance = desc.minDistance;
        kept.maxDistance = desc.maxDistance;

        JPH::TwoBodyConstraint* constraint = record->constraint.GetPtr();
        switch (kept.type) {
        case ConstraintType::Hinge:
        case ConstraintType::SwingTwist:
            attachMotor(*record);
            break;
        case ConstraintType::Slider: {
            auto* slider = static_cast<JPH::SliderConstraint*>(constraint);
            applyMotor(slider->GetMotorSettings(), kept, massOf(slider));
            driveMotor(slider, kept);
            break;
        }
        case ConstraintType::Distance:
            static_cast<JPH::DistanceConstraint*>(constraint)
                ->SetDistance(std::min(kept.minDistance, kept.maxDistance),
                              std::max(kept.minDistance, kept.maxDistance));
            break;
        case ConstraintType::Fixed:
        case ConstraintType::Point:
        case ConstraintType::Drive:
            break;
        }
        wakeBoth(*record);
    }

    [[nodiscard]] ConstraintState constraintState(ConstraintHandle handle) const
    {
        ConstraintState state;
        const ConstraintRecord* record = resolve(handle);
        if (record == nullptr || record->constraint == nullptr) {
            return state;
        }
        state.enabled = record->constraint->GetEnabled();
        state.appliedImpulse = appliedImpulseOf(*record);
        state.appliedAngularImpulse = appliedAngularImpulseOf(*record);
        motorImpulseOf(*record, state);
        measure(*record, state);
        return state;
    }

    // Every constraint that names this body, destroyed. Called BEFORE the body
    // is: a joint holding a body that is gone is a dangling pointer inside the
    // solver, and it is silent.
    void retireConstraintsOn(BodyHandle body)
    {
        for (u32 slot = 0; slot < static_cast<u32>(m_constraints.size()); ++slot) {
            ConstraintRecord& record = m_constraints[slot];
            if (!record.alive || (!(record.first == body) && !(record.second == body))) {
                continue;
            }
            retireConstraint(record);
            record.alive = false;
            m_freeConstraints.push_back(slot);
        }
    }

    // Every constraint on this body, rebuilt against the body that replaced it.
    //
    // In SLOT order, which is creation order, so a rebuilt limb solves in the
    // sequence it always did -- a resize that reordered the solve would change
    // the simulation, which is the kind of silent divergence R10 is about.
    void rebuildConstraintsOn(BodyHandle body)
    {
        for (u32 slot = 0; slot < static_cast<u32>(m_constraints.size()); ++slot) {
            ConstraintRecord& record = m_constraints[slot];
            if (!record.alive || (!(record.first == body) && !(record.second == body))) {
                continue;
            }
            m_system.RemoveConstraint(record.constraint);
            detachMotor(record);
            JPH::Ref<JPH::TwoBodyConstraint> built = buildConstraint(record.desc);
            if (built == nullptr) {
                // The body could not be re-joined. Dropped rather than left
                // pointing at the one that was destroyed.
                dropExclusion(record.desc);
                record.constraint = nullptr;
                record.alive = false;
                m_freeConstraints.push_back(slot);
                continue;
            }
            record.constraint = built;
            m_system.AddConstraint(built);
            attachMotor(record);
        }
    }

private:
    // **A hinge's or a ball socket's motor, made the first time one is asked
    // for** and told what it is asked every time after (see `PivotMotor`). A
    // joint nobody drives has none, so a world of passive joints is solved
    // exactly as it was.
    void attachMotor(ConstraintRecord& record)
    {
        const ConstraintDesc& desc = record.desc;
        if (desc.type != ConstraintType::Hinge && desc.type != ConstraintType::SwingTwist) {
            return;
        }
        if (record.motor == nullptr) {
            if (desc.motor == MotorMode::Off) {
                return;
            }
            const BodyRecord* firstRecord = resolve(desc.first);
            const BodyRecord* secondRecord = resolve(desc.second);
            if (firstRecord == nullptr || secondRecord == nullptr) {
                return;
            }
            // Frames before locks, for `buildConstraint`'s reason.
            const JPH::RMat44 frameOne = jointFrame(desc.first, desc.firstFrame);
            const JPH::RMat44 frameTwo = jointFrame(desc.second, desc.secondFrame);
            PivotMotorSettings settings;
            settings.point1 = frameOne.GetTranslation();
            settings.point2 = frameTwo.GetTranslation();
            settings.frame1 = frameOne.GetQuaternion();
            settings.frame2 = frameTwo.GetQuaternion();
            settings.ball = desc.type == ConstraintType::SwingTwist;
            const JPH::BodyID ids[2] = {firstRecord->id, secondRecord->id};
            const JPH::BodyLockMultiWrite lock(m_system.GetBodyLockInterface(), ids, 2);
            JPH::Body* first = lock.GetBody(0);
            JPH::Body* second = lock.GetBody(1);
            if (first == nullptr || second == nullptr) {
                return;
            }
            record.motor = new PivotMotor(*first, *second, settings);
            record.motor->SetEnabled(record.constraint->GetEnabled());
            m_system.AddConstraint(record.motor);
        }
        PivotMotor& motor = *record.motor;
        motor.mode = desc.motor;
        motor.maxTorque = desc.motorMaxForce;
        motor.targetAngle = desc.motorTarget;
        motor.targetVelocity = desc.motorTarget;
        motor.targetOrientation = toJolt(desc.motorOrientation).Normalized();
        motor.stiffness = desc.motorStiffness;
        motor.damping = desc.motorDamping;
        motor.natural = 2.0f * JPH::JPH_PI * std::max(desc.motorFrequency, 0.01f);
        motor.ratio = std::max(desc.motorDampingRatio, 0.0f);
    }

    void detachMotor(ConstraintRecord& record)
    {
        if (record.motor != nullptr) {
            m_system.RemoveConstraint(record.motor);
            record.motor = nullptr;
        }
    }

    // The joint frame in WORLD space, from the body's current transform and the
    // frame the caller gave in that body's own space.
    //
    // **This is what sidesteps the centre-of-mass trap.** Jolt's
    // `LocalToBodyCOM` space is relative to the centre of mass and not to the
    // body origin, and a hull MeshPart's two are not the same point -- a joint
    // authored at a shoulder would end up wherever the arm's mass happened to
    // balance. Handing Jolt world space lets IT do that conversion, which it is
    // guaranteed to do consistently with its own solver.
    [[nodiscard]] JPH::RMat44 jointFrame(BodyHandle body, const core::CFrameD& local) const
    {
        const core::CFrameD world = bodyState(body).transform * local;
        return JPH::RMat44::sRotationTranslation(toJolt(world.rotation), toLocal(world.position));
    }

    // **A drive**: six free axes and their motors, between two bodies or
    // between one and the world (see `DriveDesc`).
    [[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> buildDrive(const ConstraintDesc& desc)
    {
        const BodyRecord* firstRecord = resolve(desc.first);
        const BodyRecord* secondRecord = resolve(desc.second);
        if (secondRecord == nullptr || (desc.first.valid() && firstRecord == nullptr)) {
            return nullptr;
        }
        // Both frames before either lock, for `buildConstraint`'s reason.
        JPH::RMat44 frameTwo = jointFrame(desc.second, desc.secondFrame);
        if (desc.drive.atCenterOfMass) {
            frameTwo.SetTranslation(m_system.GetBodyInterface().GetCenterOfMassPosition(secondRecord->id));
        }
        // The world's frame sits at the solver's own origin: every target is
        // handed over relative to it, so where it is does not matter and a
        // rebase has nothing to move.
        const JPH::RMat44 frameOne =
            firstRecord != nullptr
                ? jointFrame(desc.first, desc.firstFrame)
                : JPH::RMat44::sRotationTranslation(toJolt(desc.firstFrame.rotation), JPH::RVec3::sZero());

        JPH::SixDOFConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mPosition1 = frameOne.GetTranslation();
        settings.mAxisX1 = frameOne.GetAxisX();
        settings.mAxisY1 = frameOne.GetAxisY();
        settings.mPosition2 = frameTwo.GetTranslation();
        settings.mAxisX2 = frameTwo.GetAxisX();
        settings.mAxisY2 = frameTwo.GetAxisY();
        for (int axis = 0; axis < JPH::SixDOFConstraintSettings::EAxis::Num; ++axis) {
            settings.MakeFreeAxis(static_cast<JPH::SixDOFConstraintSettings::EAxis>(axis));
        }

        JPH::Ref<JPH::TwoBodyConstraint> made;
        if (firstRecord != nullptr) {
            const JPH::BodyID ids[2] = {firstRecord->id, secondRecord->id};
            const JPH::BodyLockMultiWrite lock(m_system.GetBodyLockInterface(), ids, 2);
            JPH::Body* first = lock.GetBody(0);
            JPH::Body* second = lock.GetBody(1);
            if (first == nullptr || second == nullptr) {
                return nullptr;
            }
            made = settings.Create(*first, *second);
        }
        else {
            const JPH::BodyLockWrite lock(m_system.GetBodyLockInterface(), secondRecord->id);
            if (!lock.Succeeded()) {
                return nullptr;
            }
            made = settings.Create(JPH::Body::sFixedToWorld, lock.GetBody());
        }
        driveSixAxis(static_cast<JPH::SixDOFConstraint*>(made.GetPtr()), desc);
        return made;
    }

    static void springOf(JPH::SpringSettings& spring, f32 stiffness, f32 damping, f32 frequency, f32 ratio)
    {
        if (stiffness > 0.0f) {
            spring.mMode = JPH::ESpringMode::StiffnessAndDamping;
            spring.mStiffness = stiffness;
            spring.mDamping = std::max(damping, 0.0f);
        }
        else {
            spring.mMode = JPH::ESpringMode::FrequencyAndDamping;
            spring.mFrequency = std::max(frequency, 0.01f);
            spring.mDamping = std::max(ratio, 0.0f);
        }
    }

    // What a drive's motors are told, every tick it is driven in.
    void driveSixAxis(JPH::SixDOFConstraint* constraint, const ConstraintDesc& desc) const
    {
        if (constraint == nullptr) {
            return;
        }
        using Axis = JPH::SixDOFConstraintSettings::EAxis;
        const DriveDesc& drive = desc.drive;

        // A spring whose target is itself moving follows it at that speed.
        const bool following = !(drive.linearVelocity == core::Vec3{0.0f, 0.0f, 0.0f});
        for (int index = 0; index < 3; ++index) {
            const Axis axis = static_cast<Axis>(Axis::TranslationX + index);
            JPH::MotorSettings& motor = constraint->GetMotorSettings(axis);
            motor.SetForceLimit(drive.linearMaxForce);
            springOf(motor.mSpringSettings, drive.linearStiffness, drive.linearDamping, drive.linearFrequency,
                     drive.linearDampingRatio);
            const MotorMode mode = drive.linear[index];
            constraint->SetMotorState(axis, mode == MotorMode::Off        ? JPH::EMotorState::Off
                                            : mode == MotorMode::Velocity ? JPH::EMotorState::Velocity
                                            : following                   ? JPH::EMotorState::PositionAndVelocity
                                                                          : JPH::EMotorState::Position);
        }
        constraint->SetTargetVelocityCS(toJolt(drive.linearVelocity));
        // Against the world the first frame is at the solver's origin, so the
        // target is where the world's place is in the solver's space, read
        // along that frame's axes.
        const core::Vec3 place = desc.first.valid() ? core::toVec3(drive.linearTarget)
                                                    : core::transpose(desc.firstFrame.rotation) *
                                                          core::toVec3(drive.linearTarget - m_origin);
        constraint->SetTargetPositionCS(toJolt(place));

        const bool turning = !(drive.angularVelocity == core::Vec3{0.0f, 0.0f, 0.0f});
        for (int index = 0; index < 3; ++index) {
            const Axis axis = static_cast<Axis>(Axis::RotationX + index);
            JPH::MotorSettings& motor = constraint->GetMotorSettings(axis);
            motor.SetTorqueLimit(drive.angularMaxTorque);
            springOf(motor.mSpringSettings, drive.angularStiffness, drive.angularDamping, drive.angularFrequency,
                     drive.angularDampingRatio);
            constraint->SetMotorState(axis, drive.angular == MotorMode::Off        ? JPH::EMotorState::Off
                                            : drive.angular == MotorMode::Velocity ? JPH::EMotorState::Velocity
                                            : turning ? JPH::EMotorState::PositionAndVelocity
                                                      : JPH::EMotorState::Position);
        }
        if (drive.angular != MotorMode::Off) {
            // The solver reads a spin along the SECOND body's frame as it
            // stands now; what was asked is said in the world's axes.
            const JPH::Quat frame =
                constraint->GetBody2()->GetRotation() * constraint->GetConstraintToBody2Matrix().GetQuaternion();
            constraint->SetTargetAngularVelocityCS(frame.Conjugated() * toJolt(drive.angularVelocity));
        }
        if (drive.angular == MotorMode::Position) {
            JPH::Quat target = toJolt(drive.angularTarget).Normalized();
            // **The solver reads how far there is to turn as twice the sine of
            // half of it**, which is the angle only while the angle is small:
            // a spring asked to turn a right angle would pull as if it were
            // eighty degrees, and one said in newton-metres a radian would not
            // be. So the target handed over is the one whose reading IS the
            // angle -- exact up to two radians, and all the way round past it.
            const JPH::Quat now = constraint->GetRotationInConstraintSpace();
            JPH::Quat turn = now.Conjugated() * target;
            if (turn.GetW() < 0.0f) {
                turn = -turn;
            }
            JPH::Vec3 axis;
            float angle = 0.0f;
            turn.GetAxisAngle(axis, angle);
            if (angle > 1.0e-3f) {
                const float asked = angle < 2.0f ? 2.0f * std::asin(angle * 0.5f) : JPH::JPH_PI;
                target = (now * JPH::Quat::sRotation(axis, asked)).Normalized();
            }
            constraint->SetTargetOrientationCS(target);
        }
    }

    [[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> buildConstraint(const ConstraintDesc& desc)
    {
        if (desc.type == ConstraintType::Drive) {
            return buildDrive(desc);
        }
        const BodyRecord* firstRecord = resolve(desc.first);
        const BodyRecord* secondRecord = resolve(desc.second);
        if (firstRecord == nullptr || secondRecord == nullptr) {
            return nullptr;
        }
        // **Both frames BEFORE either lock, and that order is load-bearing.**
        // `jointFrame` reads the body's transform through the LOCKING body
        // interface, so computing one inside the write locks below is a
        // recursive lock on a body this thread already holds -- which is not an
        // error, an assert or a slowdown: the process simply stops, at zero CPU,
        // with no output. It cost one wedged test run to find.
        const JPH::RMat44 frameOne = jointFrame(desc.first, desc.firstFrame);
        const JPH::RMat44 frameTwo = jointFrame(desc.second, desc.secondFrame);

        // **One multi-lock, not two single ones.** Taking two body write locks
        // in a row is a lock-ordering bug: Jolt's own assertion says so
        // ("a lock of same or higher priority was already taken, this can create
        // a deadlock"), and it fired fifty-six times before this was written
        // that way. `BodyLockMultiWrite` sorts the ids and takes them in the one
        // order every caller agrees on.
        const JPH::BodyID ids[2] = {firstRecord->id, secondRecord->id};
        const JPH::BodyLockMultiWrite lock(m_system.GetBodyLockInterface(), ids, 2);
        JPH::Body* first = lock.GetBody(0);
        JPH::Body* second = lock.GetBody(1);
        if (first == nullptr || second == nullptr) {
            return nullptr;
        }

        // X is the joint axis and Y the reference direction a limit is measured
        // from, in every type below. One frame, read one way, whatever the joint
        // then does with it.
        switch (desc.type) {
        case ConstraintType::Fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            // The frames as given rather than "wherever they are now": a weld
            // authored to hold two parts a metre apart must hold them a metre
            // apart, and auto-detection would silently substitute their current
            // relative pose for the one that was asked for.
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = frameOne.GetTranslation();
            settings.mAxisX1 = frameOne.GetAxisX();
            settings.mAxisY1 = frameOne.GetAxisY();
            settings.mPoint2 = frameTwo.GetTranslation();
            settings.mAxisX2 = frameTwo.GetAxisX();
            settings.mAxisY2 = frameTwo.GetAxisY();
            return settings.Create(*first, *second);
        }
        case ConstraintType::Point: {
            JPH::PointConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = frameOne.GetTranslation();
            settings.mPoint2 = frameTwo.GetTranslation();
            return settings.Create(*first, *second);
        }
        case ConstraintType::Hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = frameOne.GetTranslation();
            settings.mHingeAxis1 = frameOne.GetAxisX();
            settings.mNormalAxis1 = frameOne.GetAxisY();
            settings.mPoint2 = frameTwo.GetTranslation();
            settings.mHingeAxis2 = frameTwo.GetAxisX();
            settings.mNormalAxis2 = frameTwo.GetAxisY();
            // **A range of nothing is still a range** (D473): the solver holds
            // a joint whose two limits are one value exactly there, and is
            // happy to -- but making one that way trips an assumption of its
            // own, reported as an error on every joint a script locks by
            // giving both limits the same number. So it is made a hair wide
            // and then told the range it was asked for.
            const bool locked = desc.limitLow == desc.limitHigh;
            if (desc.limitLow <= desc.limitHigh) {
                settings.mLimitsMin = desc.limitLow - (locked ? 1.0e-4f : 0.0f);
                settings.mLimitsMax = desc.limitHigh + (locked ? 1.0e-4f : 0.0f);
            }
            // Its motor is a constraint of its own (see `PivotMotor`).
            JPH::Ref<JPH::TwoBodyConstraint> made = settings.Create(*first, *second);
            if (locked) {
                static_cast<JPH::HingeConstraint*>(made.GetPtr())->SetLimits(desc.limitLow, desc.limitHigh);
            }
            return made;
        }
        case ConstraintType::SwingTwist: {
            JPH::SwingTwistConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPosition1 = frameOne.GetTranslation();
            settings.mTwistAxis1 = frameOne.GetAxisX();
            settings.mPlaneAxis1 = frameOne.GetAxisY();
            settings.mPosition2 = frameTwo.GetTranslation();
            settings.mTwistAxis2 = frameTwo.GetAxisX();
            settings.mPlaneAxis2 = frameTwo.GetAxisY();
            // One cone rather than an ellipse: a shoulder's two half-angles are
            // rarely different enough to be worth authoring separately, and the
            // ellipse is there in Jolt if a profile ever asks for it.
            settings.mNormalHalfConeAngle = desc.swingLimit;
            settings.mPlaneHalfConeAngle = desc.swingLimit;
            settings.mTwistMinAngle = -desc.twistLimit;
            settings.mTwistMaxAngle = desc.twistLimit;
            return settings.Create(*first, *second);
        }
        case ConstraintType::Slider: {
            JPH::SliderConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = frameOne.GetTranslation();
            settings.mSliderAxis1 = frameOne.GetAxisX();
            settings.mNormalAxis1 = frameOne.GetAxisY();
            settings.mPoint2 = frameTwo.GetTranslation();
            settings.mSliderAxis2 = frameTwo.GetAxisX();
            settings.mNormalAxis2 = frameTwo.GetAxisY();
            // **A range of nothing is still a range** (D473): the solver holds
            // a joint whose two limits are one value exactly there, and is
            // happy to -- but making one that way trips an assumption of its
            // own, reported as an error on every joint a script locks by
            // giving both limits the same number. So it is made a hair wide
            // and then told the range it was asked for.
            const bool locked = desc.limitLow == desc.limitHigh;
            if (desc.limitLow <= desc.limitHigh) {
                settings.mLimitsMin = desc.limitLow - (locked ? 1.0e-4f : 0.0f);
                settings.mLimitsMax = desc.limitHigh + (locked ? 1.0e-4f : 0.0f);
            }
            JPH::Ref<JPH::TwoBodyConstraint> made = settings.Create(*first, *second);
            auto* slider = static_cast<JPH::SliderConstraint*>(made.GetPtr());
            if (locked) {
                slider->SetLimits(desc.limitLow, desc.limitHigh);
            }
            applyMotor(slider->GetMotorSettings(), desc, massOf(slider));
            driveMotor(slider, desc);
            return made;
        }
        case ConstraintType::Distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = frameOne.GetTranslation();
            settings.mPoint2 = frameTwo.GetTranslation();
            settings.mMinDistance = desc.minDistance;
            settings.mMaxDistance = desc.maxDistance;
            return settings.Create(*first, *second);
        }
        case ConstraintType::Drive:
            break;
        }
        return nullptr;
    }

    // Along a rail neither body turns, so what moves is the two masses against
    // each other.
    [[nodiscard]] static f32 massOf(const JPH::SliderConstraint* slider)
    {
        const JPH::Body& first = *slider->GetBody1();
        const JPH::Body& second = *slider->GetBody2();
        const f32 give = (first.IsDynamic() ? first.GetMotionProperties()->GetInverseMass() : 0.0f) +
                         (second.IsDynamic() ? second.GetMotionProperties()->GetInverseMass() : 0.0f);
        return give > 0.0f ? 1.0f / give : 0.0f;
    }

    // A rail's motor, which is the solver's own: along a line there is no arm
    // for a row to be wrong by.
    //
    // `inertia` is what the motor moves, in kilograms, and is what makes the
    // frequency form mean what it says: zero leaves the solver to its own
    // reading.
    static void applyMotor(JPH::MotorSettings& settings, const ConstraintDesc& desc, f32 inertia)
    {
        if (desc.motor == MotorMode::Off) {
            return;
        }
        settings.SetForceLimit(desc.motorMaxForce);
        settings.SetTorqueLimit(desc.motorMaxForce);
        // The spring a position motor pulls with (see `ConstraintDesc`).
        if (desc.motorStiffness > 0.0f) {
            settings.mSpringSettings.mMode = JPH::ESpringMode::StiffnessAndDamping;
            settings.mSpringSettings.mStiffness = desc.motorStiffness;
            settings.mSpringSettings.mDamping = std::max(desc.motorDamping, 0.0f);
        }
        else if (inertia > 0.0f) {
            const f32 natural = 2.0f * JPH::JPH_PI * std::max(desc.motorFrequency, 0.01f);
            settings.mSpringSettings.mMode = JPH::ESpringMode::StiffnessAndDamping;
            settings.mSpringSettings.mStiffness = inertia * natural * natural;
            settings.mSpringSettings.mDamping = 2.0f * std::max(desc.motorDampingRatio, 0.0f) * inertia * natural;
        }
        else {
            settings.mSpringSettings.mMode = JPH::ESpringMode::FrequencyAndDamping;
            settings.mSpringSettings.mFrequency = std::max(desc.motorFrequency, 0.01f);
            settings.mSpringSettings.mDamping = std::max(desc.motorDampingRatio, 0.0f);
        }
    }

    static void setMotorVelocity(JPH::SliderConstraint* c, f32 target) { c->SetTargetVelocity(target); }
    static void setMotorPosition(JPH::SliderConstraint* c, f32 target) { c->SetTargetPosition(target); }

    template <typename T>
    static void driveMotor(T* constraint, const ConstraintDesc& desc)
    {
        if (constraint == nullptr) {
            return;
        }
        if (desc.motor == MotorMode::Off) {
            constraint->SetMotorState(JPH::EMotorState::Off);
            return;
        }
        if (desc.motor == MotorMode::Velocity) {
            constraint->SetMotorState(JPH::EMotorState::Velocity);
            setMotorVelocity(constraint, desc.motorTarget);
            return;
        }
        constraint->SetMotorState(JPH::EMotorState::Position);
        setMotorPosition(constraint, desc.motorTarget);
    }

    // In newton-seconds, and a magnitude rather than a vector: this number is
    // what a breakable joint is a threshold on, and its direction says nothing a
    // caller at this seam can use.
    [[nodiscard]] static f32 appliedImpulseOf(const ConstraintRecord& record)
    {
        const JPH::TwoBodyConstraint* constraint = record.constraint.GetPtr();
        if (constraint == nullptr) {
            return 0.0f;
        }
        switch (record.desc.type) {
        case ConstraintType::Fixed:
            return static_cast<const JPH::FixedConstraint*>(constraint)->GetTotalLambdaPosition().Length();
        case ConstraintType::Point:
            return static_cast<const JPH::PointConstraint*>(constraint)->GetTotalLambdaPosition().Length();
        case ConstraintType::Hinge:
            return static_cast<const JPH::HingeConstraint*>(constraint)->GetTotalLambdaPosition().Length();
        case ConstraintType::SwingTwist:
            return static_cast<const JPH::SwingTwistConstraint*>(constraint)->GetTotalLambdaPosition().Length();
        case ConstraintType::Slider:
            return static_cast<const JPH::SliderConstraint*>(constraint)->GetTotalLambdaPosition().Length();
        case ConstraintType::Distance:
            return std::fabs(static_cast<const JPH::DistanceConstraint*>(constraint)->GetTotalLambdaPosition());
        case ConstraintType::Drive:
            // Holds nothing: all of it is motor.
            return 0.0f;
        }
        return 0.0f;
    }

    // What the joint's own motor did last step (see `ConstraintState`).
    static void motorImpulseOf(const ConstraintRecord& record, ConstraintState& state)
    {
        const JPH::TwoBodyConstraint* constraint = record.constraint.GetPtr();
        if (constraint == nullptr) {
            return;
        }
        switch (record.desc.type) {
        case ConstraintType::Hinge:
        case ConstraintType::SwingTwist:
            state.motorAngularImpulse = record.motor != nullptr ? record.motor->totalImpulse() : 0.0f;
            break;
        case ConstraintType::Slider:
            state.motorImpulse =
                std::fabs(static_cast<const JPH::SliderConstraint*>(constraint)->GetTotalLambdaMotor());
            break;
        case ConstraintType::Drive: {
            const auto* drive = static_cast<const JPH::SixDOFConstraint*>(constraint);
            state.motorImpulse = drive->GetTotalLambdaMotorTranslation().Length();
            state.motorAngularImpulse = drive->GetTotalLambdaMotorRotation().Length();
            break;
        }
        case ConstraintType::Fixed:
        case ConstraintType::Point:
        case ConstraintType::Distance:
            break;
        }
    }

    // What resists a TURN, as a magnitude in newton-metre-seconds: the parts of
    // each joint that hold an axis, and nothing for a joint that holds none.
    [[nodiscard]] static f32 appliedAngularImpulseOf(const ConstraintRecord& record)
    {
        const JPH::TwoBodyConstraint* constraint = record.constraint.GetPtr();
        if (constraint == nullptr) {
            return 0.0f;
        }
        switch (record.desc.type) {
        case ConstraintType::Fixed:
            return static_cast<const JPH::FixedConstraint*>(constraint)->GetTotalLambdaRotation().Length();
        case ConstraintType::Hinge: {
            const auto* hinge = static_cast<const JPH::HingeConstraint*>(constraint);
            const JPH::Vector<2> held = hinge->GetTotalLambdaRotation();
            return std::sqrt(held[0] * held[0] + held[1] * held[1]) + std::fabs(hinge->GetTotalLambdaRotationLimits());
        }
        case ConstraintType::SwingTwist: {
            const auto* joint = static_cast<const JPH::SwingTwistConstraint*>(constraint);
            const f32 twist = joint->GetTotalLambdaTwist();
            const f32 swingY = joint->GetTotalLambdaSwingY();
            const f32 swingZ = joint->GetTotalLambdaSwingZ();
            return std::sqrt(twist * twist + swingY * swingY + swingZ * swingZ);
        }
        case ConstraintType::Slider:
            return static_cast<const JPH::SliderConstraint*>(constraint)->GetTotalLambdaRotation().Length();
        case ConstraintType::Point:
        case ConstraintType::Distance:
        case ConstraintType::Drive:
            return 0.0f;
        }
        return 0.0f;
    }

    // Where a joint is along what it leaves free, and how fast it is going.
    void measure(const ConstraintRecord& record, ConstraintState& state) const
    {
        const JPH::TwoBodyConstraint* constraint = record.constraint.GetPtr();
        const JPH::Body* first = constraint->GetBody1();
        const JPH::Body* second = constraint->GetBody2();
        switch (record.desc.type) {
        case ConstraintType::Hinge: {
            state.position = static_cast<const JPH::HingeConstraint*>(constraint)->GetCurrentAngle();
            // About the hinge's axis as the first body carries it.
            const JPH::Vec3 axis =
                first->GetRotation() * toJolt(record.desc.firstFrame.rotation * core::Vec3{1.0f, 0.0f, 0.0f});
            state.velocity = (second->GetAngularVelocity() - first->GetAngularVelocity()).Dot(axis);
            break;
        }
        case ConstraintType::Slider: {
            state.position = static_cast<const JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
            const JPH::Vec3 axis =
                first->GetRotation() * toJolt(record.desc.firstFrame.rotation * core::Vec3{1.0f, 0.0f, 0.0f});
            state.velocity = (second->GetLinearVelocity() - first->GetLinearVelocity()).Dot(axis);
            break;
        }
        case ConstraintType::Distance: {
            const JPH::RVec3 one = first->GetWorldTransform() * toJoltPosition(record.desc.firstFrame.position);
            const JPH::RVec3 two = second->GetWorldTransform() * toJoltPosition(record.desc.secondFrame.position);
            const JPH::Vec3 apart = JPH::Vec3(two - one);
            state.position = apart.Length();
            if (state.position > 1.0e-5f) {
                state.velocity =
                    (second->GetPointVelocity(two) - first->GetPointVelocity(one)).Dot(apart / state.position);
            }
            break;
        }
        case ConstraintType::Fixed:
        case ConstraintType::Point:
        case ConstraintType::SwingTwist:
        case ConstraintType::Drive:
            break;
        }
    }

    // Removed from the world, exclusion dropped, reference released. Separate
    // from `destroyConstraint` because retiring a BODY does the same to every
    // joint that names it, and doing it twice is a use-after-free.
    void retireConstraint(ConstraintRecord& record)
    {
        if (record.constraint != nullptr) {
            m_system.RemoveConstraint(record.constraint);
        }
        detachMotor(record);
        dropExclusion(record.desc);
        record.constraint = nullptr;
    }

    [[nodiscard]] ConstraintRecord* resolve(ConstraintHandle handle) noexcept
    {
        return const_cast<ConstraintRecord*>(static_cast<const JoltWorld*>(this)->resolve(handle));
    }

    [[nodiscard]] const ConstraintRecord* resolve(ConstraintHandle handle) const noexcept
    {
        if (handle.index >= m_constraints.size()) {
            return nullptr;
        }
        const ConstraintRecord& record = m_constraints[handle.index];
        return record.alive && record.generation == handle.generation ? &record : nullptr;
    }

    void applyExclusion(const ConstraintDesc& desc)
    {
        if (desc.collideConnected) {
            return;
        }
        m_contacts.exclude(packHandle(desc.first), packHandle(desc.second));
    }

    void dropExclusion(const ConstraintDesc& desc)
    {
        if (desc.collideConnected) {
            return;
        }
        m_contacts.unexclude(packHandle(desc.first), packHandle(desc.second));
    }

    [[nodiscard]] BodyRecord* resolve(BodyHandle handle) noexcept
    {
        return const_cast<BodyRecord*>(static_cast<const JoltWorld*>(this)->resolve(handle));
    }

    [[nodiscard]] const CharacterRecord* resolve(CharacterHandle handle) const noexcept
    {
        if (handle.index >= m_characters.size()) {
            return nullptr;
        }
        const CharacterRecord& record = m_characters[handle.index];
        return record.alive && record.generation == handle.generation ? &record : nullptr;
    }

    [[nodiscard]] CharacterRecord* resolve(CharacterHandle handle) noexcept
    {
        return const_cast<CharacterRecord*>(static_cast<const JoltWorld*>(this)->resolve(handle));
    }

    // The half of body creation that both `createBody` and `updateBody` need:
    // everything from the desc onto the record and into Jolt, with the handle
    // already decided.
    [[nodiscard]] bool instantiate(BodyRecord& record, BodyHandle handle, const BodyDesc& desc,
                                   const JPH::ShapeRefC& shape)
    {
        record.alive = true;
        record.collidable = desc.collidable;
        record.queryable = desc.queryable;
        record.passableForCharacters = desc.passableForCharacters;
        record.group = desc.group;
        record.userData = desc.userData;
        record.surfaces.assign(desc.surfaces.begin(), desc.surfaces.end());

        // **A shape that can only be static IS static, whatever was asked for**
        // (ADR 0066). Both `HeightFieldShape` and `MeshShape` report
        // `MustBeStatic()`, and both report a volume of zero -- so a dynamic
        // one would take its mass from the clamp below, weigh a gram, be
        // activated, and leave, taking a kilometre of terrain with it.
        //
        // The shape is asked rather than the kinds being listed here, so a third
        // static-only shape added later is handled without this line knowing
        // about it. It is corrected rather than refused, because refusing leaves
        // a world with a hole where the ground should be, and silently-static
        // ground is a far smaller surprise than absent ground.
        const MotionType motion = shape->MustBeStatic() ? MotionType::Static : desc.motion;
        record.motion = motion;

        JPH::BodyCreationSettings settings(shape, toLocal(desc.transform.position), toJolt(desc.transform.rotation),
                                           toJoltMotion(motion), encodeLayer(desc.group, motion != MotionType::Static));
        settings.mFriction = desc.friction;
        settings.mRestitution = desc.restitution;
        settings.mLinearDamping = std::max(desc.linearDamping, 0.0f);
        settings.mAngularDamping = std::max(desc.angularDamping, 0.0f);
        settings.mIsSensor = !desc.collidable;
        // **A moving body removes the ghost edges it meets** (ADR 0143): a ball
        // rolled over any triangulated ground -- one mesh, no seam -- struck the
        // edges between its triangles and hopped. Jolt does it per pair of
        // bodies, which is why the chunks' seams need their bands as well.
        settings.mEnhancedInternalEdgeRemoval = motion == MotionType::Dynamic;
        // **A moving body is swept to where it is going, not put there and
        // asked what it overlaps** (D417). A log 0.7 m thick at 56 m/s moves
        // 0.93 m a tick: put there, it was past a terrain's shell of triangles
        // with nothing behind it, and fell through the world. Jolt sweeps a
        // body only on a step that moves it more than three quarters of its
        // own inner radius (`mLinearCastThreshold`), so a body at rest or at a
        // walk costs what it did; the sweep is against everything, moving or
        // not, and it is as deterministic as the step it is part of.
        settings.mMotionQuality =
            motion == MotionType::Dynamic ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
        // **A round body may spin as fast as a wheel does** (K1). Jolt's own
        // ceiling is a quarter turn a step -- 47 rad/s at sixty -- so a kart's
        // 0.27 m wheel could not pass 46 km/h whatever its motor asked. A ball,
        // and a cylinder no longer than it is wide -- a wheel, a disc, a fan --
        // sweep little new as they turn, so they may turn at highway speed;
        // anything else keeps the ceiling, which is what stops a tumbling log
        // tunnelling through a floor (D417's logs fell through it at five
        // hundred).
        const bool wheel = desc.shape.type == ShapeType::Sphere ||
                           (desc.shape.type == ShapeType::Cylinder && desc.shape.size.y <= desc.shape.size.x);
        if (wheel)
            settings.mMaxAngularVelocity = MaxAngularVelocity;
        settings.mUserData = packHandle(handle);
        // Mass is volume times `BasePart.Density`, and there is no `Mass`
        // property precisely so that the two cannot disagree. `CalculateInertia`
        // takes the mass from here and derives the inertia tensor from the
        // shape, which is the only combination that keeps a dense small part
        // and a light large one behaving differently under a torque.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = std::max(shape->GetVolume() * std::max(desc.density, 0.0001f), 0.001f);
        record.mass = settings.mMassPropertiesOverride.mMass;

        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        record.id = bodies.CreateAndAddBody(settings, motion == MotionType::Static ? JPH::EActivation::DontActivate
                                                                                   : JPH::EActivation::Activate);
        return !record.id.IsInvalid();
    }

    [[nodiscard]] static JPH::EMotionType toJoltMotion(MotionType motion) noexcept
    {
        switch (motion) {
        case MotionType::Static:
            return JPH::EMotionType::Static;
        case MotionType::Kinematic:
            return JPH::EMotionType::Kinematic;
        case MotionType::Dynamic:
            break;
        }
        return JPH::EMotionType::Dynamic;
    }

    [[nodiscard]] core::Vec3 surfaceNormal(const JPH::BodyID& id, const JPH::SubShapeID& subShape,
                                           JPH::RVec3Arg point) const
    {
        const JPH::BodyLockRead lock(m_system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) {
            return core::Vec3{0.0f, 1.0f, 0.0f};
        }
        return fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(subShape, point));
    }

    void forgetPairs(u64 packed)
    {
        const auto drop = [packed](const ContactPair& pair) { return pair.first == packed || pair.second == packed; };
        m_previousPairs.erase(std::remove_if(m_previousPairs.begin(), m_previousPairs.end(), drop),
                              m_previousPairs.end());

        // A character standing on the body being destroyed would otherwise carry
        // the contact forever: the pair can never appear again, so the diff can
        // never fire its `TouchEnded`, and `emitCharacter` would refuse it
        // anyway once the record is gone.
        const auto dropCharacter = [packed](const CharacterPair& pair) {
            return !pair.otherIsCharacter && pair.other == packed;
        };
        m_previousCharacterPairs.erase(
            std::remove_if(m_previousCharacterPairs.begin(), m_previousCharacterPairs.end(), dropCharacter),
            m_previousCharacterPairs.end());
    }

    void forgetCharacterPairs(u64 packed)
    {
        const auto drop = [packed](const CharacterPair& pair) {
            return pair.character == packed || (pair.otherIsCharacter && pair.other == packed);
        };
        m_previousCharacterPairs.erase(
            std::remove_if(m_previousCharacterPairs.begin(), m_previousCharacterPairs.end(), drop),
            m_previousCharacterPairs.end());
    }

    // The whole of `Touched`/`TouchEnded`: this tick's contacting pairs against
    // last tick's. A pair that appears fires Began, one that disappears fires
    // Ended, one that stays fires nothing. Built from a diff rather than from
    // Jolt's OnContactRemoved because that callback can arrive for a body the
    // caller has already destroyed, and because the diff is what makes the
    // event order ours.
    void buildContactEvents()
    {
        m_events.clear();

        std::vector<ContactPair>& current = m_contacts.pairs();
        std::sort(current.begin(), current.end());
        current.erase(std::unique(current.begin(), current.end()), current.end());

        // One detail a pair, the hardest: the listener's order is the job
        // system's, and two manifolds of one pair may arrive either way round.
        std::vector<ContactDetail>& details = m_contacts.details();
        std::sort(details.begin(), details.end(), [](const ContactDetail& a, const ContactDetail& b) {
            return a.pair == b.pair ? a.speed > b.speed : a.pair < b.pair;
        });
        details.erase(std::unique(details.begin(), details.end(),
                                  [](const ContactDetail& a, const ContactDetail& b) { return a.pair == b.pair; }),
                      details.end());

        m_carried.clear();
        usize i = 0;
        usize j = 0;
        while (i < current.size() || j < m_previousPairs.size()) {
            if (j == m_previousPairs.size() || (i < current.size() && current[i] < m_previousPairs[j])) {
                m_carried.push_back(current[i]);
                emit(ContactPhase::Began, current[i]);
                ++i;
            }
            else if (i == current.size() || m_previousPairs[j] < current[i]) {
                // A pair that stops being reported has not necessarily
                // separated: Jolt stops calling the listener for an island it
                // has put to sleep, so a crate that settles on the floor and
                // dozes off would otherwise fire TouchEnded while still visibly
                // resting on it -- and Touched again the moment anything nudged
                // it. Both bodies asleep means the contact is still there and
                // nobody is looking at it.
                if (asleep(m_previousPairs[j])) {
                    m_carried.push_back(m_previousPairs[j]);
                }
                else {
                    emit(ContactPhase::Ended, m_previousPairs[j]);
                }
                ++j;
            }
            else {
                m_carried.push_back(current[i]);
                ++i;
                ++j;
            }
        }

        m_previousPairs = m_carried;
    }

    // True when neither body of the pair is being simulated -- a static body is
    // never active, and a dynamic one stops being active when the solver puts
    // its island to sleep.
    [[nodiscard]] bool asleep(const ContactPair& pair) const
    {
        const BodyRecord* first = resolve(unpackHandle(pair.first));
        const BodyRecord* second = resolve(unpackHandle(pair.second));
        if (first == nullptr || second == nullptr) {
            return false;
        }
        const JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        return !bodies.IsActive(first->id) && !bodies.IsActive(second->id);
    }

    // **`Touched` for a character's contacts, all of them** (D028).
    //
    // A `CharacterBody` is a `BasePart`, so a script reasonably expects
    // `Touched` from one -- and the rigid-body contact listener cannot give it,
    // because a `CharacterVirtual` is not a body in the broad phase. M6 answered
    // the half an obby needs by diffing the surface under the character's feet
    // in the scene glue, and left a wall walked into firing nothing.
    //
    // This is the whole of it instead, and the ground half moved here with it:
    // two mechanisms for one signal is how the two disagree. `GetActiveContacts`
    // is what the character's own update already collected
    // (`CharacterVirtual.h:511`), so the cost is a walk over a handful of
    // contacts and no extra collision work.
    //
    // The order is `m_characters`' own slot order and then the sort in
    // `buildCharacterContactEvents`, so nothing about how Jolt's job system
    // happened to schedule the sweep reaches the event stream (R10).
    void collectCharacterContacts()
    {
        m_characterPairs.clear();
        const JPH::BodyInterface& bodies = m_system.GetBodyInterface();

        for (usize slot = 0; slot < m_characters.size(); ++slot) {
            const CharacterRecord& record = m_characters[slot];
            if (!record.alive || record.character == nullptr)
                continue;

            const u64 self = packHandle(CharacterHandle{static_cast<u32>(slot), record.generation});
            for (const JPH::CharacterContact& contact : record.character->GetActiveContacts()) {
                // A PREDICTIVE contact is one the sweep found ahead of the
                // character and never reached: `mHadCollision` is what separates
                // "touching" from "about to". A discarded one was refused by the
                // validate callback and never happened at all.
                //
                // **A sensor -- a part that does not collide and does touch --
                // is touched by being inside it** (D524): Jolt marks a
                // character's contact with one as collided only when the sweep
                // runs into the contact's plane, so a character that stood in
                // a zone, or crossed a pad its sweep never met, touched
                // nothing. Its contact is real where the shapes overlap.
                const bool touching =
                    contact.mIsSensorB ? contact.mDistance <= 0.0f : contact.mHadCollision && !contact.mWasDiscarded;
                if (!touching)
                    continue;
                // Another `CharacterVirtual` directly, which this engine never
                // produces: `mCharacterVsCharacterCollision` is deliberately
                // unset (see `createCharacter`), so character-against-character
                // arrives as the inner BODY below.
                if (contact.mBodyB.IsInvalid())
                    continue;

                CharacterPair pair;
                pair.character = self;
                if (const CharacterHandle peer = characterOfInnerBody(contact.mBodyB); peer.valid()) {
                    // Skip the pair a character makes with its own inner body,
                    // which is a contact with itself and not an event.
                    if (packHandle(peer) == self)
                        continue;
                    pair.other = packHandle(peer);
                    pair.otherIsCharacter = true;
                }
                else {
                    pair.other = bodies.GetUserData(contact.mBodyB);
                }
                m_characterPairs.push_back(pair);
            }
        }
    }

    // Which character owns this body, when the body is a character's inner one.
    //
    // Linear over the character table, which is a handful of entries and is
    // walked only for contacts a character actually has. A map keyed by `BodyID`
    // would be the answer if that stopped being true.
    [[nodiscard]] CharacterHandle characterOfInnerBody(JPH::BodyID id) const noexcept
    {
        for (usize slot = 0; slot < m_characters.size(); ++slot) {
            const CharacterRecord& record = m_characters[slot];
            if (record.alive && record.character != nullptr &&
                (record.character->GetInnerBodyID() == id || (!record.standIn.IsInvalid() && record.standIn == id)))
                return CharacterHandle{static_cast<u32>(slot), record.generation};
        }
        return CharacterHandle{};
    }

    // The same diff `buildContactEvents` makes, without the sleep exception --
    // see `CharacterPair` for why there is nothing to except.
    void buildCharacterContactEvents()
    {
        std::sort(m_characterPairs.begin(), m_characterPairs.end());
        m_characterPairs.erase(std::unique(m_characterPairs.begin(), m_characterPairs.end()), m_characterPairs.end());

        usize i = 0;
        usize j = 0;
        while (i < m_characterPairs.size() || j < m_previousCharacterPairs.size()) {
            if (j == m_previousCharacterPairs.size() ||
                (i < m_characterPairs.size() && m_characterPairs[i] < m_previousCharacterPairs[j])) {
                emitCharacter(ContactPhase::Began, m_characterPairs[i]);
                ++i;
            }
            else if (i == m_characterPairs.size() || m_previousCharacterPairs[j] < m_characterPairs[i]) {
                emitCharacter(ContactPhase::Ended, m_previousCharacterPairs[j]);
                ++j;
            }
            else {
                ++i;
                ++j;
            }
        }

        m_previousCharacterPairs = m_characterPairs;
    }

    void emitCharacter(ContactPhase phase, const CharacterPair& pair)
    {
        const CharacterHandle character = unpackCharacter(pair.character);
        const CharacterRecord* record = resolve(character);
        if (record == nullptr)
            return;

        // The BODY side of the event is left invalid for a character, because a
        // character does not have one. `firstUserData` and `secondUserData` are
        // filled either way, and they are what the scene glue reads.
        ContactEvent event;
        event.phase = phase;
        event.firstUserData = record->userData;

        if (pair.otherIsCharacter) {
            const CharacterRecord* peer = resolve(unpackCharacter(pair.other));
            if (peer == nullptr)
                return;
            event.secondUserData = peer->userData;
        }
        else {
            const BodyHandle other = unpackHandle(pair.other);
            const BodyRecord* otherRecord = resolve(other);
            if (otherRecord == nullptr)
                return;
            event.second = other;
            event.secondUserData = otherRecord->userData;
        }
        m_events.push_back(event);
    }

    void emit(ContactPhase phase, const ContactPair& pair)
    {
        const BodyHandle first = unpackHandle(pair.first);
        const BodyHandle second = unpackHandle(pair.second);
        const BodyRecord* firstRecord = resolve(first);
        const BodyRecord* secondRecord = resolve(second);
        if (firstRecord == nullptr || secondRecord == nullptr) {
            return;
        }
        ContactEvent event;
        event.phase = phase;
        event.first = first;
        event.second = second;
        event.firstUserData = firstRecord->userData;
        event.secondUserData = secondRecord->userData;
        if (phase == ContactPhase::Began) {
            const std::vector<ContactDetail>& details = m_contacts.details();
            const auto found = std::lower_bound(
                details.begin(), details.end(), pair,
                [](const ContactDetail& detail, const ContactPair& wanted) { return detail.pair < wanted; });
            if (found != details.end() && found->pair == pair) {
                event.detailed = true;
                event.point = toWorld(found->point);
                event.normal = fromJolt(found->normal);
                event.speed = found->speed;
            }
        }
        m_events.push_back(event);
    }

    // Filters translating a `QueryFilter` into the two things Jolt asks for.
    // Both are stack objects living for the duration of one query.
    class BodyFilterAdapter final : public JPH::BodyFilter
    {
    public:
        BodyFilterAdapter(const JoltWorld& world, const QueryFilter& filter) : m_world(world), m_filter(filter) {}

        [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override
        {
            const BodyHandle handle = unpackHandle(body.GetUserData());
            const BodyRecord* record = m_world.resolve(handle);
            if (record == nullptr || !record->queryable) {
                return false;
            }

            const bool listed = std::find(m_filter.userData.begin(), m_filter.userData.end(), record->userData) !=
                                m_filter.userData.end();
            return m_filter.mode == QueryFilter::Mode::Exclude ? !listed : listed;
        }

    private:
        const JoltWorld& m_world;
        const QueryFilter& m_filter;
    };

    class LayerFilterAdapter final : public JPH::ObjectLayerFilter
    {
    public:
        explicit LayerFilterAdapter(const QueryFilter& filter) : m_filter(filter) {}

        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override
        {
            return !m_filter.filterGroup || decodeGroup(layer) == m_filter.group;
        }

    private:
        const QueryFilter& m_filter;
    };

    // **A character's contacts, as the world decides them** (the multiplayer
    // smoothness brief): a body marked passable holds a character up and
    // does not stand in its way -- a contact whose normal is not mostly up is
    // discarded. Everything else is Jolt's own answer.
    class CharacterContacts final : public JPH::CharacterContactListener
    {
    public:
        explicit CharacterContacts(const JoltWorld& world) : m_world(world) {}

        [[nodiscard]] bool OnContactValidate(const JPH::CharacterVirtual*,
                                             const JPH::CharacterContact& contact) override
        {
            // A terrain chunk's band only lends its edges (ADR 0143).
            if (!contact.mBodyB.IsInvalid()) {
                const JPH::Body* touched = m_world.m_system.GetBodyLockInterfaceNoLock().TryGetBody(contact.mBodyB);
                if (touched != nullptr && onBand(*touched, contact.mSubShapeIDB))
                    return false;
            }
            // **A character that has a stand-in is met there, and not where it
            // is** (ADR 0163): its own inner body is no obstacle.
            if (!contact.mBodyB.IsInvalid() && contact.mUserData != kStandInUserData) {
                for (const CharacterRecord& other : m_world.m_characters) {
                    if (other.alive && other.character != nullptr && !other.standIn.IsInvalid() &&
                        other.character->GetInnerBodyID() == contact.mBodyB)
                        return false;
                }
            }
            if (contact.mUserData == kStandInUserData)
                return true;
            const BodyRecord* body = m_world.resolve(unpackHandle(contact.mUserData));
            if (body == nullptr || !body->passableForCharacters)
                return true;
            return contact.mContactNormal.GetY() > kPassableSupportNormal;
        }

    private:
        // About 45 degrees: steeper than that is a side, not a floor.
        static constexpr float kPassableSupportNormal = 0.7f;
        const JoltWorld& m_world;
    };

    CollisionMatrix m_matrix;
    BroadPhaseLayers m_broadPhaseLayers;
    ObjectVsBroadPhaseFilter m_objectVsBroadPhase;
    ObjectPairFilter m_pairFilter;
    JPH::TempAllocatorImpl m_temp;
    // **Jolt's own pool, not the engine's** (S6.10, ADR 0064). The engine job
    // pool sizes itself from the machine, and Jolt's determinism is per thread
    // COUNT -- so running the solver on it would make a trace recorded here
    // unreproducible on a machine with a different core count, which is the one
    // property `tests/determinism` exists to hold. A fixed sub-pool of the
    // engine's would be the same threads with more code between them and the
    // same fixed number.
    JPH::JobSystemThreadPool m_jobs;
    core::DVec3 m_origin;
    JPH::PhysicsSystem m_system;
    CharacterContacts m_characterContacts{*this};

    // Which body and character slots are alive, at which generation, and the
    // origin: what a restore must find unchanged (ADR 0101).
    void writeLayout(JPH::StateRecorder& recorder) const
    {
        recorder.Write(m_origin.x);
        recorder.Write(m_origin.y);
        recorder.Write(m_origin.z);
        const u64 bodies = m_bodies.size();
        recorder.Write(bodies);
        for (const BodyRecord& record : m_bodies) {
            recorder.Write(record.alive);
            recorder.Write(record.alive ? record.generation : 0u);
        }
        const u64 characters = m_characters.size();
        recorder.Write(characters);
        for (const CharacterRecord& record : m_characters) {
            const bool alive = record.alive && record.character != nullptr;
            recorder.Write(alive);
            recorder.Write(alive ? record.generation : 0u);
        }
    }

    static void writePairs(JPH::StateRecorder& recorder, const std::vector<ContactPair>& pairs)
    {
        const u64 count = pairs.size();
        recorder.Write(count);
        for (const ContactPair& pair : pairs) {
            recorder.Write(pair.first);
            recorder.Write(pair.second);
        }
    }

    [[nodiscard]] static bool readPairs(JPH::StateRecorderImpl& recorder, std::vector<ContactPair>& pairs)
    {
        u64 count = 0;
        recorder.Read(count);
        for (u64 at = 0; at < count && !recorder.IsFailed(); ++at) {
            ContactPair pair;
            recorder.Read(pair.first);
            recorder.Read(pair.second);
            pairs.push_back(pair);
        }
        return !recorder.IsFailed();
    }
    ContactRecorder m_contacts;

    std::vector<BodyRecord> m_bodies;
    std::vector<u32> m_freeBodies;
    std::vector<CharacterRecord> m_characters;
    std::vector<u32> m_freeCharacters;
    // Slot-indexed and never compacted, exactly as the bodies are: a handle is
    // an index plus a generation, and Jolt solves in the order constraints were
    // added, so a caller that creates them in a stable order gets a stable
    // solve (R10).
    std::vector<ConstraintRecord> m_constraints;
    std::vector<u32> m_freeConstraints;

    std::vector<ContactPair> m_previousPairs;
    // The character half of the same diff (D028).
    std::vector<CharacterPair> m_characterPairs;
    std::vector<CharacterPair> m_previousCharacterPairs;
    // Cleared every `step`; see `setBodyTransform`.
    std::vector<PendingMove> m_kinematicMoves;
    // Scratch for the diff, kept as a member so a tick with ten thousand
    // contacts does not allocate one.
    std::vector<ContactPair> m_carried;
    std::vector<ContactEvent> m_events;
    StepTimings m_timings;
    // The hulls `shapeFor` built, by cloud and scale.
    struct HullKey
    {
        core::u64 revision = 0;
        u32 scaleX = 0;
        u32 scaleY = 0;
        u32 scaleZ = 0;
        [[nodiscard]] bool operator==(const HullKey&) const noexcept = default;
    };
    struct HullHash
    {
        [[nodiscard]] core::usize operator()(const HullKey& key) const noexcept
        {
            core::u64 hash = key.revision * 0x9E3779B97F4A7C15ull;
            hash ^= (static_cast<core::u64>(key.scaleX) << 32 | key.scaleY) + 0x9E3779B97F4A7C15ull + (hash << 6);
            hash ^= static_cast<core::u64>(key.scaleZ) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
            return static_cast<core::usize>(hash);
        }
    };
    std::unordered_map<HullKey, JPH::ShapeRefC, HullHash> m_hulls;
    core::u64 m_shapesBuilt = 0;
    core::Vec3 m_gravity{0.0f, -9.81f, 0.0f};
    // What `PhysicsSystem::Init` was told, so a report about a full buffer can
    // say how full is full.
    JPH::uint m_contactBudget = 0;
    // Which `EPhysicsUpdateError` bits have already been reported. A full buffer
    // stays full, and a line a tick would bury everything else in the log.
    core::u32 m_reportedUpdateErrors = 0;
};

// --- Process-wide Jolt state ------------------------------------------------
//
// Jolt has three globals that must be set up before any of its types exist and
// torn down after the last of them is gone: the allocator, the factory and the
// type registry. Reference-counted here rather than initialised at static
// construction time, because a static initialiser would run in a test binary
// that never creates a physics world and would leak the factory in a process
// that creates one and destroys it.
// Jolt's own diagnostics, routed through the engine log rather than to a
// console nobody is reading. The text is upstream's and is not translated --
// what carries the i18n key (R3) is the line around it, exactly as the catalog
// reader's own developer diagnostics do.
// The attribute is what lets Clang see that `format` reaches `vsnprintf` from a
// printf-like parameter rather than from a runtime string: without it,
// -Wformat-nonliteral is an error on the Tier-2 build and MSVC says nothing at
// all. The Linux tier found this.
#if defined(__clang__) || defined(__GNUC__)
void traceImpl(const char* format, ...) __attribute__((format(printf, 1, 2)));
#endif

void traceImpl(const char* format, ...)
{
    va_list list;
    va_start(list, format);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, list);
    va_end(list);
    // `logText` rather than a keyed line: this string is upstream's, and R3's
    // rule is that engine-AUTHORED prose goes through the catalog. Wrapping
    // somebody else's diagnostic in a translated sentence would translate the
    // half nobody reads and leave the half that matters in English.
    core::logText(core::LogLevel::Debug, std::string_view(buffer));
}

#ifdef JPH_ENABLE_ASSERTS
// Returning false means "do not break". An assert here is Jolt telling us we
// misused it -- a degenerate shape, a velocity set on a static body -- and the
// engine that reports it and keeps running is more useful than the one that
// dies, because the report names the call site and the crash would not.
bool assertFailedImpl(const char* expression, const char* message, const char* file, JPH::uint line)
{
    const std::string_view text = message != nullptr ? std::string_view(message) : std::string_view{};
    const std::array<core::I18nArg, 4> args{
        core::I18nArg{"file", std::string_view(file)},
        core::I18nArg{"line", static_cast<core::i64>(line)},
        core::I18nArg{"expression", std::string_view(expression)},
        core::I18nArg{"message", text},
    };
    core::log(core::LogLevel::Error, ENG_TR("physics.jolt.err.assert"), args);
    return false;
}
#endif

class JoltRuntime
{
public:
    static void acquire()
    {
        if (s_refs++ == 0) {
            JPH::RegisterDefaultAllocator();
            JPH::Trace = traceImpl;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assertFailedImpl;)
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    static void release()
    {
        if (--s_refs == 0) {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

private:
    static inline int s_refs = 0;
};

class JoltPhysics final : public IPhysics3D
{
public:
    JoltPhysics() { JoltRuntime::acquire(); }
    ~JoltPhysics() override
    {
        m_worlds.clear();
        JoltRuntime::release();
    }

    [[nodiscard]] WorldHandle createWorld(const WorldDesc& desc) override
    {
        u32 slot = 0;
        if (!m_free.empty()) {
            slot = m_free.back();
            m_free.pop_back();
        }
        else {
            slot = static_cast<u32>(m_worlds.size());
            m_worlds.emplace_back();
            m_generations.push_back(0);
        }

        m_generations[slot] = m_generations[slot] + 1 == 0 ? 1 : m_generations[slot] + 1;
        m_worlds[slot] = std::make_unique<JoltWorld>(desc);
        return WorldHandle{slot, m_generations[slot]};
    }

    void destroyWorld(WorldHandle handle) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            m_worlds[handle.index].reset();
            m_free.push_back(handle.index);
        }
    }

    void setGravity(WorldHandle handle, core::Vec3 gravity) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setGravity(gravity);
        }
    }

    void setWorldOrigin(WorldHandle handle, core::DVec3 origin) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setOrigin(origin);
        }
    }

    [[nodiscard]] core::DVec3 worldOrigin(WorldHandle handle) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->origin() : core::DVec3{};
    }

    [[nodiscard]] BodyHandle createBody(WorldHandle handle, const BodyDesc& desc) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr ? world->createBody(desc) : BodyHandle{};
    }

    // No world in it: a shape is a function of its description, and Jolt
    // builds one without touching any world's state -- which is what lets
    // several be built at once on the pool.
    [[nodiscard]] PreparedShape prepareShape(const ShapeDesc& desc) const override
    {
        JPH::ShapeRefC shape = buildShape(desc);
        if (shape == nullptr)
            return {};
        return PreparedShape{std::make_shared<const JPH::ShapeRefC>(std::move(shape))};
    }

    void destroyBody(WorldHandle handle, BodyHandle body) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->destroyBody(body);
        }
    }

    [[nodiscard]] core::u64 shapesBuilt(WorldHandle handle) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->shapesBuilt() : 0u;
    }

    // --- Constraints ---------------------------------------------------------

    [[nodiscard]] ConstraintHandle createConstraint(WorldHandle handle, const ConstraintDesc& desc) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr ? world->createConstraint(desc) : ConstraintHandle{};
    }

    void destroyConstraint(WorldHandle handle, ConstraintHandle constraint) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->destroyConstraint(constraint);
        }
    }

    void setConstraintEnabled(WorldHandle handle, ConstraintHandle constraint, bool enabled) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setConstraintEnabled(constraint, enabled);
        }
    }

    void updateConstraint(WorldHandle handle, ConstraintHandle constraint, const ConstraintDesc& desc) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->updateConstraint(constraint, desc);
        }
    }

    void driveConstraint(WorldHandle handle, ConstraintHandle constraint, const ConstraintDesc& desc) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->driveConstraint(constraint, desc);
        }
    }

    [[nodiscard]] ConstraintState constraintState(WorldHandle handle, ConstraintHandle constraint) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->constraintState(constraint) : ConstraintState{};
    }

    void setBodyTransform(WorldHandle handle, BodyHandle body, const core::CFrameD& transform) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyTransform(body, transform);
        }
    }

    void setBodyVelocity(WorldHandle handle, BodyHandle body, core::Vec3 linear, core::Vec3 angular) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyVelocity(body, linear, angular);
        }
    }

    void applyImpulse(WorldHandle handle, BodyHandle body, core::Vec3 impulse) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->applyImpulse(body, impulse);
        }
    }

    void applyAngularImpulse(WorldHandle handle, BodyHandle body, core::Vec3 impulse) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->applyAngularImpulse(body, impulse);
        }
    }

    void applyImpulseAt(WorldHandle handle, BodyHandle body, core::Vec3 impulse, core::DVec3 point) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->applyImpulseAt(body, impulse, point);
        }
    }

    [[nodiscard]] BodyMassProperties bodyMassProperties(WorldHandle handle, BodyHandle body) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->bodyMassProperties(body) : BodyMassProperties{};
    }

    void setBodyDamping(WorldHandle handle, BodyHandle body, f32 linear, f32 angular) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyDamping(body, linear, angular);
        }
    }

    void setPairCollidable(WorldHandle handle, BodyHandle first, BodyHandle second, bool collidable) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setPairCollidable(first, second, collidable);
        }
    }

    bool updateBody(WorldHandle handle, BodyHandle body, const BodyDesc& desc) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr && world->updateBody(body, desc);
    }

    bool updateHeightField(WorldHandle handle, BodyHandle body, u32 x, u32 z, u32 sizeX, u32 sizeZ,
                           std::span<const float> heights) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr && world->updateHeightField(body, x, z, sizeX, sizeZ, heights);
    }

    void setBodyMaterial(WorldHandle handle, BodyHandle body, f32 friction, f32 restitution) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyMaterial(body, friction, restitution);
        }
    }

    void setBodyFlags(WorldHandle handle, BodyHandle body, bool collidable, bool queryable) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyFlags(body, collidable, queryable);
        }
    }

    void setBodyGroup(WorldHandle handle, BodyHandle body, CollisionGroup group) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setBodyGroup(body, group);
        }
    }

    [[nodiscard]] BodyState bodyState(WorldHandle handle, BodyHandle body) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->bodyState(body) : BodyState{};
    }

    void collectActiveBodies(WorldHandle handle, std::vector<ActiveBody>& out) const override
    {
        if (const JoltWorld* world = resolve(handle); world != nullptr) {
            world->collectActiveBodies(out);
        }
    }

    void step(WorldHandle handle, f32 fixedDt) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->step(fixedDt);
        }
    }

    [[nodiscard]] std::span<const ContactEvent> drainContacts(WorldHandle handle) override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->contacts() : std::span<const ContactEvent>{};
    }

    [[nodiscard]] StepTimings lastStepTimings(WorldHandle handle) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->timings() : StepTimings{};
    }

    [[nodiscard]] bool raycast(WorldHandle handle, const RayD& ray, const QueryFilter& filter,
                               RayHit& outHit) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr && world->raycast(ray, filter, outHit);
    }

    [[nodiscard]] bool spherecast(WorldHandle handle, const RayD& ray, f32 radius, const QueryFilter& filter,
                                  RayHit& outHit) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr && world->spherecast(ray, radius, filter, outHit);
    }

    void overlapBox(WorldHandle handle, const core::CFrameD& transform, core::Vec3 size, const QueryFilter& filter,
                    std::vector<u64>& out) const override
    {
        if (const JoltWorld* world = resolve(handle); world != nullptr) {
            world->overlapBox(transform, size, filter, out);
        }
    }

    void overlapSphere(WorldHandle handle, core::DVec3 center, f32 radius, const QueryFilter& filter,
                       std::vector<u64>& out) const override
    {
        if (const JoltWorld* world = resolve(handle); world != nullptr) {
            world->overlapSphere(center, radius, filter, out);
        }
    }

    [[nodiscard]] CharacterHandle createCharacter(WorldHandle handle, const CharacterDesc& desc) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr ? world->createCharacter(desc) : CharacterHandle{};
    }

    void destroyCharacter(WorldHandle handle, CharacterHandle character) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->destroyCharacter(character);
        }
    }

    void moveCharacter(WorldHandle handle, CharacterHandle character, core::Vec3 velocity, f32 fixedDt,
                       bool walking = false) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->moveCharacter(character, velocity, fixedDt, walking);
        }
    }

    void setCharacterTransform(WorldHandle handle, CharacterHandle character, const core::CFrameD& transform) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setCharacterTransform(character, transform);
        }
    }

    void nudgeCharacter(WorldHandle handle, CharacterHandle character, const core::CFrameD& transform) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->nudgeCharacter(character, transform);
        }
    }

    void setCharacterStandIn(WorldHandle handle, CharacterHandle character, const core::CFrameD* where) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->setCharacterStandIn(character, where);
        }
    }

    [[nodiscard]] CharacterState characterState(WorldHandle handle, CharacterHandle character) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->characterState(character) : CharacterState{};
    }

    [[nodiscard]] CollisionGroup registerCollisionGroup(WorldHandle handle, std::string_view name) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr ? world->matrix().add(name) : CollisionMatrix::kInvalidGroup;
    }

    [[nodiscard]] CollisionGroup findCollisionGroup(WorldHandle handle, std::string_view name) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr ? world->matrix().find(name) : CollisionMatrix::kInvalidGroup;
    }

    void setGroupsCollidable(WorldHandle handle, CollisionGroup a, CollisionGroup b, bool collidable) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->matrix().setCollidable(a, b, collidable);
        }
    }

    [[nodiscard]] bool groupsCollidable(WorldHandle handle, CollisionGroup a, CollisionGroup b) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr && world->matrix().collidable(a, b);
    }

    void collectCollisionGroups(WorldHandle handle, std::vector<std::string_view>& out) const override
    {
        if (const JoltWorld* world = resolve(handle); world != nullptr) {
            world->matrix().collectNames(out);
        }
    }

    [[nodiscard]] bool saveState(WorldHandle handle, std::vector<u8>& out) const override
    {
        const JoltWorld* world = resolve(handle);
        if (world == nullptr)
            return false;
        world->saveState(out);
        return true;
    }

    [[nodiscard]] bool restoreState(WorldHandle handle, std::span<const u8> blob) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr && world->restoreState(blob);
    }

    [[nodiscard]] bool saveIsland(WorldHandle handle, std::span<const BodyHandle> bodies,
                                  std::span<const CharacterHandle> characters, std::vector<u8>& out) const override
    {
        const JoltWorld* world = resolve(handle);
        return world != nullptr && world->saveIsland(bodies, characters, out);
    }

    [[nodiscard]] bool restoreIsland(WorldHandle handle, std::span<const u8> blob) override
    {
        JoltWorld* world = resolve(handle);
        return world != nullptr && world->restoreIsland(blob);
    }

    void debugDraw(WorldHandle handle, IDebugDrawSink& sink) override
    {
        if (JoltWorld* world = resolve(handle); world != nullptr) {
            world->debugDraw(sink);
        }
    }

private:
    [[nodiscard]] JoltWorld* resolve(WorldHandle handle) noexcept
    {
        if (handle.index >= m_worlds.size() || m_generations[handle.index] != handle.generation) {
            return nullptr;
        }
        return m_worlds[handle.index].get();
    }

    [[nodiscard]] const JoltWorld* resolve(WorldHandle handle) const noexcept
    {
        if (handle.index >= m_worlds.size() || m_generations[handle.index] != handle.generation) {
            return nullptr;
        }
        return m_worlds[handle.index].get();
    }

    std::vector<std::unique_ptr<JoltWorld>> m_worlds;
    std::vector<u32> m_generations;
    std::vector<u32> m_free;
};

} // namespace

PhysicsResult createJoltPhysics(core::EngineError*)
{
    return std::make_unique<JoltPhysics>();
}

} // namespace engine::physics
