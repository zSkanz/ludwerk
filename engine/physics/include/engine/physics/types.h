// The vocabulary of the physics seam (architecture.md §2, ADR 0007, ADR 0023).
//
// Everything here is POD that `core` can express. No backend type appears --
// not in a field, not in an enum value, not behind a pointer (R17) -- and
// nothing above L2 has to know that Jolt exists to talk about a body.
//
// The one deliberate omission is any notion of an *Instance*. A body carries an
// opaque `userData` the caller chooses and the physics module never interprets;
// the glue above stores an `InstanceId` in it. That is what lets `scene` own
// the tree and `physics` own the simulation without either learning the other's
// vocabulary -- and it is why a contact event reports two `userData` values
// rather than two things this module would have to name.
#pragma once

#include <optional>
#include <span>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::physics {

using core::f32;
using core::f64;
using core::u16;
using core::u32;
using core::u64;
using core::u8;

// Handles are index + generation for the same reason `core::InstanceId` is: a
// slot that is freed and refilled must hand out a handle that compares unequal
// to the one that pointed at the old occupant, so a stale reference is
// detectable rather than silently aliasing whatever moved in.
struct WorldHandle
{
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const WorldHandle&) const noexcept = default;
};

struct BodyHandle
{
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const BodyHandle&) const noexcept = default;
};

struct CharacterHandle
{
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const CharacterHandle&) const noexcept = default;
};

// An `Anchored` part is Static, and that is not the same thing as a dynamic
// body with an enormous mass: a static body is not in the solver's island graph
// at all. Kinematic exists for the M6 case a moving platform needs -- driven by
// a script, pushing dynamic bodies, unmoved by them.
enum class MotionType : u8
{
    Static,
    Kinematic,
    Dynamic,
};

// The shapes `Enum.PartShape` names, the convex hull a `MeshPart` collapses to,
// and the two STATIC shapes terrain needs (ADR 0066).
//
// A shape this enum cannot express is a shape the caller must approximate
// knowingly rather than one this module silently substitutes. That rule is why
// `HeightField` and `TriangleMesh` are here at all: a sculpted surface is
// concave by construction -- which is what a cave IS -- so no convex hull
// describes it and no primitive comes close.
enum class ShapeType : u8
{
    Box,
    Sphere,
    Capsule,
    Cylinder,
    ConvexHull,

    // **Both of these can only ever be static, and the backend enforces it
    // rather than trusting the description** (ADR 0066). Jolt's own classes
    // report `MustBeStatic()`, and both report a volume of zero -- so a body
    // created dynamic would take its mass from `volume x density`, clamp to the
    // one-gram floor, be activated, and leave. Asking the shape is better than
    // listing the kinds here, because a third static-only shape added later is
    // handled without this enum's readers knowing about it.

    // A regular grid of heights over the shape's `size` footprint: cheap,
    // sub-rectangle updatable in place, and single-valued, so it cannot express
    // an overhang. `heights` and `heightSampleCount` describe it.
    HeightField,
    // The triangles themselves. `points` is the vertex list and `indices` is
    // triples into it. What `Enum.CollisionFidelity.Precise` has been waiting
    // for -- though F1 does not wire `MeshPart` to it, and that enum item's
    // documentation stays as honest as it is until something does.
    TriangleMesh,
};

// Sized in the same units `BasePart.Size` is: `size` is the FULL extent, never
// a half-extent. Getting that wrong is a factor of two that looks plausible in
// every screenshot, which is why the field is not called `extents`.
struct ShapeDesc
{
    ShapeType type = ShapeType::Box;

    // Box and Wedge: the full box. Sphere: `size.x` is the diameter. Capsule
    // and Cylinder: `size.x` is the diameter and `size.y` the full height,
    // caps included, which is how `BasePart.Size` describes one.
    core::Vec3 size{1.0f, 1.0f, 1.0f};

    // ConvexHull only. Points are in the part's local space and the span must
    // outlive the `createBody` call and no longer.
    //
    // **Nobody may keep it.** A mirror that stored this desc across ticks would
    // be holding a span into a vector the next mesh load replaces, which is a
    // dangling read rather than a stale one. `pointsRevision` exists so a caller
    // can answer "are these the same points" without keeping them.
    std::span<const core::Vec3> points;

    // Which VERSION of that point cloud, counted up every time the cloud behind
    // a content name is replaced.
    //
    // A mirror decides whether to rebuild a body by comparing two `ShapeDesc`s,
    // and two hulls with the same type and the same size are not the same hull
    // if the geometry underneath them changed -- which is exactly what a mesh
    // arriving late, or a hot reload replacing one, does. Comparing the spans
    // would mean keeping one; comparing a counter costs eight bytes and keeps
    // nothing.
    core::u64 pointsRevision = 0;

    // ConvexHull only: what to multiply each point by on the way in.
    //
    // A factor rather than pre-scaled points, because the points are shared --
    // every `MeshPart` naming one file collides against one cached point cloud,
    // and scaling in `scene` would mean a copy per body per resize. The backend
    // already copies each point into its own array to build the hull, so this
    // rides along for nothing.
    core::Vec3 pointScale{1.0f, 1.0f, 1.0f};

    // --- The two static shapes (ADR 0066) ---------------------------------
    //
    // **Every span below lives under the same rule as `points`**: it must
    // outlive the call it is handed to and no longer, and nobody may keep it. A
    // mirror comparing two descriptions across ticks uses `geometryRevision`
    // for exactly the reason it uses `pointsRevision` -- comparing the spans
    // would mean keeping one.

    // `TriangleMesh` only: triples indexing into `points`, which is the vertex
    // list. Reused rather than given a second vertex span, because `points` is
    // already "positions in the part's local space" with the lifetime rule this
    // needs.
    std::span<const core::u32> indices;

    // `TriangleMesh` only: **the triangles from this one on only lend their
    // edges** (ADR 0143). A terrain chunk's collider carries a band of its
    // neighbours' triangles so the edge along its border is inside the mesh to
    // the active-edge pass, and a body crossing into the next chunk meets no
    // edge there; the band itself is never collided with. Past the last
    // triangle -- the default -- is no band.
    core::u32 bandFirst = ~core::u32{0};
    // **What each triangle is made of** (ADR 0117): one byte a triangle, an
    // index into the body's `BodyDesc::surfaces`. Empty for a mesh that is one
    // thing all over, which collides by the body's own friction as it always
    // did. Like every span here, it outlives the call and no longer.
    std::span<const core::u8> triangleSurfaces{};

    // `HeightField` only: `heightSampleCount * heightSampleCount` samples in row
    // order, each the height in the part's local space.
    //
    // The footprint comes from `size`, so a sample is a height and nothing else:
    // the surface is `(x, heights[z * n + x], z)` scaled to fit `size.x` by
    // `size.z` and centred, which makes a height field describe the same box a
    // `Box` of the same `size` would occupy.
    std::span<const float> heights;

    // The grid's edge, in samples. Jolt requires `heightSampleCount /
    // heightBlockSize` to be at least 2, and a power of two is the cheapest.
    core::u32 heightSampleCount = 0;

    // The block a height field is culled and updated in. **This is also the
    // alignment an in-place update must respect**: `updateHeightField` asserts
    // that its rectangle starts on a multiple of it, so a caller grows a brush's
    // affected area outward to a block boundary before handing it over.
    //
    // Two is Jolt's own default and the finest available, which is what a
    // sculpting tool wants: a bigger block makes an edit rewrite more than it
    // touched.
    core::u32 heightBlockSize = 2;

    // **The range a height field may ever hold, which is decided ONCE, when the
    // shape is built, and cannot be widened afterwards.**
    //
    // This is the single least obvious thing about a height field and it is
    // silent when got wrong. A field's samples are quantised into a fixed number
    // of bits spread across `[min, max]`, and that mapping is baked at
    // construction -- so `updateHeightField` CLAMPS every sample it is given
    // into the range the shape was born with. Build a flat field from all-zero
    // samples and its range is zero wide; every edit afterwards then quantises
    // back to the height it already had, the call reports success, and nothing
    // moves.
    //
    // So these are the heights the terrain is ALLOWED to reach, not the heights
    // it currently has: a cell that will be dug 40 m down and raised 200 m up
    // says so here on the day it is created. Left equal (the default), the range
    // is whatever the initial samples span, which is right for a field nobody
    // will edit and wrong for all terrain.
    float heightMin = 0.0f;
    float heightMax = 0.0f;

    // Which VERSION of `indices` or `heights`, for the reason `pointsRevision`
    // exists. Counted up whenever the geometry behind an otherwise identical
    // description is replaced -- and terrain replaces it on every brush stroke,
    // so this is the field that stops a mirror concluding nothing happened.
    core::u64 geometryRevision = 0;
};

// --- Constraints -------------------------------------------------------------
//
// What holds two bodies together, and how much freedom is left between them.
//
// **Ragdoll is what this exists for.** A character that falls is a dozen bodies
// and a dozen joints, and the joint is where the character stops being a pile of
// capsules: a shoulder that swings but does not bend backwards is a
// `SwingTwist`, and it is one constraint rather than a per-frame correction.
// Doors, wheels and lids come out of the same seam for free.

struct ConstraintHandle
{
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const ConstraintHandle&) const noexcept = default;
};

enum class ConstraintType : u8
{
    // No freedom at all. Two bodies that move as one, which is what a weld is
    // when the solver rather than the transform hierarchy has to hold it.
    Fixed,
    // A ball joint: three rotational degrees, no limits.
    Point,
    // One rotational degree about the joint's X axis, with an optional angular
    // range. A door, a lid, an elbow.
    Hinge,
    // **The ragdoll workhorse.** A cone of swing about X plus a twist along it,
    // which is exactly what a shoulder or a hip is -- and it is cheaper than a
    // six-degree-of-freedom joint configured to imitate one, which is why that
    // more general shape stays unexposed.
    SwingTwist,
    // One translational degree along the joint's X axis, with an optional range.
    Slider,
    // The two frames' origins held at a distance, or within a range of them.
    Distance,
    // **Nothing held, everything driven** (ADR 0127, D471): six free axes, and
    // a motor on any of them -- what "go to that place", "face that way" and
    // "hold this speed" are when the solver is the one doing them, so they
    // are solved WITH the joints a body is in rather than before them. See
    // `DriveDesc`. The one type whose first body may be nothing at all, which
    // is the world.
    Drive,
};

// The motor a constraint may drive itself with. Off is the default and is what
// a passive ragdoll uses; a POWERED ragdoll turns it on and blends towards the
// animated pose in the SOLVER, which is a different thing from blending the
// palette afterwards and produces a different result.
enum class MotorMode : u8
{
    Off,
    // Drive towards a target velocity.
    Velocity,
    // Drive towards a target position or angle.
    Position,
};

// What a `ConstraintType::Drive` pulls with: a motor along each of the first
// frame's three axes, and one about all three.
//
// **The first body may be invalid, and then it is the world**: the first frame
// is at the world's origin with the rotation it was given, a position target
// is a place in the world and a velocity is against the ground. That is how a
// floating-origin rebase costs nothing here either -- the target is said in
// the world's own numbers every time, and the backend puts it where its own
// space has the world.
struct DriveDesc
{
    // Along the first frame's X, Y and Z. `Position` pulls the second frame's
    // origin to `linearTarget` as a spring; `Velocity` holds `linearVelocity`.
    MotorMode linear[3] = {MotorMode::Off, MotorMode::Off, MotorMode::Off};
    // In the first frame; in the world when the first body is.
    core::DVec3 linearTarget;
    // In the first frame's axes: the speed to hold, or -- under `Position` --
    // the speed the target is itself moving at, so a spring following
    // something does not trail it by its own damping.
    core::Vec3 linearVelocity{0.0f, 0.0f, 0.0f};
    // The most force the motor may use, **along each axis**.
    f32 linearMaxForce = 3.4e38f;
    // The spring a position motor is, in the terms `ConstraintDesc` says them
    // in: a stiffness above zero with its damping, or a frequency and a ratio.
    f32 linearStiffness = 0.0f;
    f32 linearDamping = 0.0f;
    f32 linearFrequency = 2.0f;
    f32 linearDampingRatio = 1.0f;

    // About all three axes at once: a turn is not three independent numbers.
    MotorMode angular = MotorMode::Off;
    // `Position`: the second frame's orientation in the first.
    core::Mat3 angularTarget;
    // `Velocity`: the second body's spin against the first's, in WORLD axes.
    // Under `Position`, the spin the target itself has.
    core::Vec3 angularVelocity{0.0f, 0.0f, 0.0f};
    f32 angularMaxTorque = 3.4e38f;
    f32 angularStiffness = 0.0f;
    f32 angularDamping = 0.0f;
    f32 angularFrequency = 2.0f;
    f32 angularDampingRatio = 1.0f;

    // The second frame's origin is wherever the second body balances, not
    // where the frame says: a push there moves the body without turning it,
    // and it stays there when the body's shape changes.
    bool atCenterOfMass = false;

    [[nodiscard]] bool operator==(const DriveDesc&) const noexcept = default;
};

// Where a joint sits, what it may still do, and what drives it.
struct ConstraintDesc
{
    ConstraintType type = ConstraintType::Fixed;

    BodyHandle first;
    BodyHandle second;

    // The joint frame **in each body's own local space**, so a floating-origin
    // rebase costs nothing: the bodies move and the frames do not. Expressing
    // it once in world space would mean re-deriving both every time the origin
    // shifted, and getting one of them wrong is a joint that drifts.
    //
    // The two frames are the same physical place when the constraint is built,
    // which is what "this is where they are attached" means. A backend that
    // needs one relative to a centre of mass computes it; that is a backend
    // detail and never the caller's.
    core::CFrameD firstFrame;
    core::CFrameD secondFrame;

    // Hinge and Slider: the range about or along the joint's X axis. Radians
    // for a hinge, metres for a slider. `low > high` means unlimited.
    f32 limitLow = 1.0f;
    f32 limitHigh = -1.0f;

    // SwingTwist: the half-angle of the swing cone and the twist range, in
    // radians. A swing of pi and a twist of pi is unlimited.
    f32 swingLimit = 3.14159265f;
    f32 twistLimit = 3.14159265f;

    // Distance: the range the two origins are kept within, in metres. Equal
    // values are a rigid rod.
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f;

    MotorMode motor = MotorMode::Off;
    // What the motor drives towards, in the units its mode implies, and the
    // most force or torque it may use to get there.
    f32 motorTarget = 0.0f;
    f32 motorMaxForce = 0.0f;

    // SwingTwist under `MotorMode::Position` (ADR 0127, amendment A1): where
    // the second frame is pulled to, as its orientation in the first's. A
    // shoulder held up, a spine held straight -- the joint doing work.
    core::Mat3 motorOrientation;

    // **A position motor is a spring**, and one number cannot say both "soft
    // and damped" and "stiff and bouncy" (amendment A3). A stiffness above zero
    // is read with its damping, in the motor's own units -- newton-metres a
    // radian and newton-metre-seconds a radian for a turn, newtons a metre and
    // newton-seconds a metre along a line. At zero the frequency and the ratio
    // are read instead, which do not depend on what is being moved.
    f32 motorStiffness = 0.0f;
    f32 motorDamping = 0.0f;
    f32 motorFrequency = 2.0f;
    f32 motorDampingRatio = 1.0f;

    // `ConstraintType::Drive` only.
    DriveDesc drive;

    // Whether the two bodies still collide with each other.
    //
    // **False is the case a ragdoll needs and the one that is not free.** An
    // upper arm and a lower arm overlap at the elbow by construction, and left
    // colliding they push each other apart every step -- a character that
    // vibrates rather than falls. The exclusion is per PAIR and the object
    // layer matrix is per LAYER, so it cannot be expressed as a group: the
    // backend keeps a sorted pair list and refuses the contact.
    bool collideConnected = true;

    // Opaque to this module, exactly as `BodyDesc::userData` is.
    u64 userData = 0;
};

// What a live constraint is doing, for a caller that wants to break one under
// load or show it in an inspector.
struct ConstraintState
{
    bool enabled = false;
    // The impulse the solver applied last step to hold the joint together, in
    // newton-seconds. A joint that is being pulled apart reports a large one,
    // which is what a breakable joint is a threshold on.
    f32 appliedImpulse = 0.0f;
    // The same for what keeps it from turning, in newton-metre-seconds: zero
    // for a joint with nothing to resist a turn with (a ball, a rope).
    f32 appliedAngularImpulse = 0.0f;
    // Hinge: the angle about the joint's X, in radians. Slider: how far along
    // it, in metres. Distance: how far apart the two frames are. Zero for the
    // rest.
    f32 position = 0.0f;
    // How fast that is changing: the second body against the first.
    f32 velocity = 0.0f;
    // What the joint's own MOTOR did last step, as a magnitude: newton-seconds
    // along a slider, newton-metre-seconds about a hinge or a ball. Apart from
    // the two above on purpose -- those are what the joint bore, and a
    // breakable joint is a threshold on what it bore, not on how hard its own
    // motor worked. A `Drive` is all motor: its linear part is here and its
    // angular part in `motorAngularImpulse`.
    f32 motorImpulse = 0.0f;
    f32 motorAngularImpulse = 0.0f;
};

// **What a body of one of the solid shapes weighs**, from its description
// alone: the volume the backend builds for it, times the density, with the
// backend's own floors. Zero for a shape whose volume only the solver has -- a
// hull -- and for the two that are never moved.
//
// Here so that a part can say what it weighs before it has a body (D472): the
// first thing a script does with a part it just made is size a force for it,
// and "zero until the simulation has stepped" sized every one of them wrong.
// `physics_tests.cpp` holds this to what the solver says, shape by shape.
[[nodiscard]] inline f32 solidMass(ShapeType type, core::Vec3 size, f32 density) noexcept
{
    constexpr f32 pi = 3.14159265358979f;
    const auto half = [](f32 full) {
        const f32 value = full * 0.5f;
        return value == value && value - value == 0.0f ? (value < 0.005f   ? 0.005f
                                                          : value > 5.0e5f ? 5.0e5f
                                                                           : value)
                                                       : 0.005f;
    };
    const f32 hx = half(size.x);
    const f32 hy = half(size.y);
    const f32 hz = half(size.z);
    f32 volume = 0.0f;
    switch (type) {
    case ShapeType::Box:
        volume = 8.0f * hx * hy * hz;
        break;
    case ShapeType::Sphere: {
        const f32 radius = hx > hy ? (hx > hz ? hx : hz) : (hy > hz ? hy : hz);
        volume = 4.0f / 3.0f * pi * radius * radius * radius;
        break;
    }
    case ShapeType::Capsule: {
        const f32 radius = hx > hz ? hx : hz;
        const f32 halfCylinder = hy - radius > 0.005f ? hy - radius : 0.005f;
        volume = 4.0f / 3.0f * pi * radius * radius * radius + 2.0f * pi * halfCylinder * radius * radius;
        break;
    }
    case ShapeType::Cylinder: {
        const f32 radius = hx > hz ? hx : hz;
        volume = 2.0f * pi * hy * radius * radius;
        break;
    }
    default:
        return 0.0f;
    }
    const f32 mass = volume * (density > 0.0001f ? density : 0.0001f);
    return mass > 0.001f ? mass : 0.001f;
}

// What a mover needs to turn "this much faster" into an impulse (ADR 0127):
// a body's mass, how hard it is to turn about each axis as it stands now, and
// where it balances.
struct BodyMassProperties
{
    // The shape's volume times the density, whatever moves the body: an
    // anchored part weighs what it would if it were let go.
    f32 mass = 0.0f;
    // Whether the solver moves it. The inertia below is zero when it does not.
    bool dynamic = false;
    // World space: an angular impulse through this is a change of angular
    // velocity.
    core::Mat3 inverseInertia{{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}};
    core::DVec3 centerOfMass;
};

// A collision group is an index into the world's collidability matrix.
// Group 0 is "Default" and always exists.
using CollisionGroup = u16;

inline constexpr CollisionGroup kDefaultCollisionGroup = 0;

// The most groups a world can hold. Ten bits of the object layer, which is what
// leaves room for the moving/non-moving split the broad phase needs.
inline constexpr u32 kMaxCollisionGroups = 1024;

// What a surface is to touch: what a `TriangleMesh`'s triangle slides and
// bounces by, in place of its body's.
struct SurfaceMaterial
{
    f32 friction = 0.3f;
    f32 restitution = 0.0f;
};

struct BodyDesc
{
    ShapeDesc shape;
    core::CFrameD transform;
    MotionType motion = MotionType::Dynamic;

    // `BasePart.Friction`, `.Restitution`, `.Density`. Density is what gives a
    // body its mass, together with the shape's volume -- there is no `Mass`
    // property, because a mass that disagreed with the size and density would
    // be a third source of truth.
    f32 friction = 0.3f;
    f32 restitution = 0.0f;
    f32 density = 1.0f;

    // **The surfaces a mesh's triangles name** (`ShapeDesc::triangleSurfaces`):
    // copied by the backend, read on its worker threads while it steps. Entry
    // 0 is not looked up: a triangle of surface 0 is the body's own.
    std::span<const SurfaceMaterial> surfaces{};

    // `BasePart.LinearDamping` and `.AngularDamping`: the share of a speed
    // lost in a second with nothing touching it. The solver's own defaults.
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;

    // `CanCollide`. A non-colliding body still reports contacts -- that is what
    // makes a trigger volume work, and it matches what a `Touched` on a
    // pass-through part means.
    bool collidable = true;

    // `CanQuery`. Excluded from raycasts, shapecasts and overlaps, and from
    // nothing else.
    bool queryable = true;

    // **Walked through from the side by a character, stood on from above**
    // (the multiplayer smoothness brief): a loose part a replica only follows.
    // The authority simulates it, and a character there pushes it out of the
    // way; the replica has it where the authority did four ticks ago, where
    // it did not give way -- so a predicting character stopped against it,
    // and was corrected every time it touched one. A contact from below still
    // holds a character up.
    bool passableForCharacters = false;

    CollisionGroup group = kDefaultCollisionGroup;

    // Opaque to this module. The glue stores an instance identity here.
    u64 userData = 0;
};

// Everything the writeback loop reads, in one call: reading a transform, then a
// velocity, then an awake flag would take three virtual calls and three
// lookups per body per tick, and there are a thousand of them.
struct BodyState
{
    core::CFrameD transform;
    core::Vec3 linearVelocity{0.0f, 0.0f, 0.0f};
    core::Vec3 angularVelocity{0.0f, 0.0f, 0.0f};
    // False for a body the solver has put to sleep, and for every static body.
    bool active = false;
};

// A body whose state changed this step, reported in a stable order the caller
// may rely on (R10). Jolt's own active-body list is explicitly documented as
// unordered under a multi-threaded job system (Docs/Architecture.md:807), so
// this is sorted before it is handed over and the sort is part of the contract
// rather than an implementation detail.
struct ActiveBody
{
    BodyHandle body;
    u64 userData = 0;
    BodyState state;
};

enum class ContactPhase : u8
{
    Began,
    Ended,
};

// One contact between two bodies, deduplicated to one event per pair per step.
// `first` and `second` are ordered by the backend so that the same pair always
// reports in the same order, which is what lets the glue diff two ticks' worth
// of contacts without sorting them again.
struct ContactEvent
{
    ContactPhase phase = ContactPhase::Began;
    BodyHandle first;
    BodyHandle second;
    u64 firstUserData = 0;
    u64 secondUserData = 0;

    // **What the contact was, for a pair that began** (ADR 0127, N2): where,
    // which way -- from the first body towards the second -- and how fast the
    // two were closing along that at the moment they met, before the solver
    // answered. Enough to say how hard a blow was. `detailed` is false for an
    // end, and for a beginning the backend had no manifold for.
    bool detailed = false;
    core::DVec3 point;
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 speed = 0.0f;
};

// f64 origin because a ray is cast from a world position; f32 direction because
// a direction is an extent and f32 is still exact at that scale (ADR 0014).
// `direction` is NOT normalised: its length is the ray's length, which is how
// `Workspace:Raycast(origin, direction)` reads in api-design.md §2.1.
struct RayD
{
    core::DVec3 origin;
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
};

struct RayHit
{
    BodyHandle body;
    u64 userData = 0;
    core::DVec3 position;
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    // Along the ray, in metres.
    f32 distance = 0.0f;
};

// `RaycastParams` in the API, and the same filter serves every query. Exclude
// and include are the two modes `Enum.RaycastFilterType` names; the list holds
// `userData` values because that is the only identity this module has.
struct QueryFilter
{
    enum class Mode : u8
    {
        Exclude,
        Include,
    };

    Mode mode = Mode::Exclude;
    std::span<const u64> userData;

    // Empty means every group. A named group filters to bodies in it.
    bool filterGroup = false;
    CollisionGroup group = kDefaultCollisionGroup;
};

// A capsule that walks. Not a rigid body: Jolt's character sweeps its own shape
// and resolves its own contacts, which is what lets it climb a step without the
// solver deciding the capsule should tip over instead.
struct CharacterDesc
{
    core::CFrameD transform;
    // Full height including the caps, and the diameter -- authored the way
    // `BasePart.Size` is, so a 2 x 5 x 2 part and a character of the same size
    // occupy the same volume.
    f32 height = 5.0f;
    f32 diameter = 2.0f;

    // Degrees. A slope steeper than this is a wall.
    f32 maxSlopeAngle = 46.0f;
    // The tallest ledge the character walks over rather than into.
    f32 stepHeight = 0.5f;

    f32 mass = 80.0f;
    CollisionGroup group = kDefaultCollisionGroup;
    u64 userData = 0;
};

// `Enum.CharacterState` has exactly these two items (api-design.md §2.3), so
// this enum is that enum rather than a superset of it.
enum class CharacterGround : u8
{
    Grounded,
    Airborne,
};

struct CharacterState
{
    core::CFrameD transform;
    core::Vec3 linearVelocity{0.0f, 0.0f, 0.0f};
    CharacterGround ground = CharacterGround::Airborne;
    // The surface under the character's feet, or an invalid handle when
    // airborne. This is what a moving platform needs at M6 and what a
    // `Landed` signal names.
    BodyHandle groundBody;
    u64 groundUserData = 0;
    core::Vec3 groundNormal{0.0f, 1.0f, 0.0f};
};

// Seconds spent inside the last `step`, and nothing else.
//
// The roadmap asks the M5 budget to be broken down as broadphase / narrowphase
// / solver, "because one number says a budget was missed and three say which
// stage missed it". Jolt does not expose those three: the split exists only
// inside its own profiler, which is a compile-time feature (JPH_PROFILE_ENABLED
// or an external-measurement class it calls from EVERY scope in the library)
// that dumps to a file rather than answering a query. Turning it on would tax
// every configuration, shipping included, for a number the gate reads once.
//
// So the breakdown the budget records is the one that exists at this seam and
// measures stages that are really separable: applying the scene's writes,
// stepping, and writing the result back. The bench reports those three; this
// struct is the middle one, and it is the backend's to fill because it is the
// only party that knows where its own update begins and ends.
struct StepTimings
{
    f64 step = 0.0;
};

struct WorldDesc
{
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    // The number of bodies the world is sized for. Jolt allocates up front; a
    // world that grows past this refuses to create rather than reallocating
    // under a running simulation.
    u32 maxBodies = 16384;
};

// The wireframe Jolt can draw of what it thinks the world looks like -- which
// is the only picture that can disagree with the rendered one, and therefore
// the only one worth having (roadmap M5, "Jolt debug-draw bridge").
//
// A sink rather than a returned buffer: the drawing is a walk over shapes the
// backend already holds, and copying it into a vector so the caller can walk it
// again would double the cost of a debug view.
class IDebugDrawSink
{
public:
    virtual ~IDebugDrawSink() = default;

    // World space, f64 -- the sink rebases. Colour is 0xRRGGBB.
    virtual void line(core::DVec3 from, core::DVec3 to, u32 color) = 0;

    // **Where the body whose `userData` this is should be DRAWN**, when that
    // is not where the simulation holds it: the pose the renderer shows
    // between two ticks. A wireframe drawn at the body's own pose lags the
    // mesh it outlines by up to a tick -- more for an anchored part a script
    // moves, whose body catches up only at the next step (the owner: the
    // collision box turned behind the spinning cube). Nothing draws it where
    // the simulation has it.
    [[nodiscard]] virtual std::optional<core::CFrameD> drawnPose([[maybe_unused]] u64 userData) { return std::nullopt; }
};

} // namespace engine::physics
