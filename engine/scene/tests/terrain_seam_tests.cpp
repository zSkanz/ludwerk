// The seam between two terrain chunks' colliders, measured (terrain audit T5;
// the owner's ruling of 2026-09-30: fix it, and choose how by the numbers).
//
// The terrain collides as one static mesh per chunk (ADR 0066/0067), and where
// two chunks meet, each mesh's border edge is an edge with one triangle: Jolt
// keeps a flat edge inside a mesh out of the contacts, but not one between two
// bodies. This drives Jolt directly -- a wheeled vehicle, balls, a character, a
// resting box -- over the same surface joined nine ways, and reports what each
// feels at the seam against the same drive over one mesh with no seam at all.
//
// Driving Jolt directly, not through `IPhysics3D`, because the engine has no
// vehicle yet (F2's movers and constraints): when it does, this harness is its
// regression test on terrain colliders.

// Jolt requires Jolt.h before any other Jolt header, so this block is exempt
// from include sorting.
// clang-format off
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/UnorderedSet.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <doctest/doctest.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/asset/terrain_mesher.h"

using namespace engine;

namespace {

constexpr float Dt = 1.0f / 60.0f;
constexpr double TickSeconds = 1.0 / 60.0;

[[nodiscard]] JPH::RVec3 rvec(double x, double y, double z)
{
    return JPH::RVec3(static_cast<JPH::Real>(x), static_cast<JPH::Real>(y), static_cast<JPH::Real>(z));
}
constexpr double ChunkMetres = 32.0;

// --- Jolt, once ----------------------------------------------------------------

struct Runtime
{
    Runtime()
    {
        if (JPH::Factory::sInstance == nullptr) {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }
};

namespace Layers {
constexpr JPH::ObjectLayer Still = 0;
constexpr JPH::ObjectLayer Moving = 1;
} // namespace Layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "layer"; }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override
    {
        return layer == Layers::Moving || broad.GetValue() == Layers::Moving;
    }
};

class PairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        return a == Layers::Moving || b == Layers::Moving;
    }
};

// --- The ground, and the ways of joining it -------------------------------------

enum class Join
{
    // One mesh body per chunk, as the engine collides today.
    Chunks,
    // The same, and the moving bodies remove internal edges
    // (`mEnhancedInternalEdgeRemoval`), which Jolt does per pair of bodies.
    ChunksEdges,
    // One mesh over the whole ground: the surface with no seam, which every
    // other way is measured against.
    Whole,
    // The same, removing internal edges.
    WholeEdges,
    // (a) The chunks' meshes in one static compound body; the moving bodies
    // remove internal edges across its parts.
    Compound,
    // (a) and (b): the compound's parts each with the ring of (b).
    CompoundRing,
    // (b) One body per chunk, each mesh with a ring of one cell of its
    // neighbours' triangles, so its border edges are interior.
    Ring,
    // (c) One body per chunk, and a contact listener that drops a contact on a
    // chunk's border edge whose normal is not the surface's.
    Listener,
    // (b) and (c): one body per chunk whose mesh carries the band of its
    // neighbours' triangles that touch its own, marked, so its border edges
    // are interior to Jolt's active-edge pass -- and a listener that drops
    // every contact with a marked triangle, so the band never collides.
    RingFiltered,
    // The same, and the moving bodies remove internal edges.
    RingFilteredEdges,
};

[[nodiscard]] const char* nameOf(Join join)
{
    switch (join) {
    case Join::Chunks:
        return "chunks";
    case Join::ChunksEdges:
        return "chunks+e";
    case Join::Whole:
        return "whole";
    case Join::WholeEdges:
        return "whole+e";
    case Join::Compound:
        return "compound";
    case Join::CompoundRing:
        return "comp+ring";
    case Join::Ring:
        return "ring";
    case Join::Listener:
        return "listener";
    case Join::RingFiltered:
        return "ring+f";
    case Join::RingFilteredEdges:
        return "ring+f+e";
    }
    return "?";
}

constexpr std::array<Join, 10> Joins{Join::Chunks,       Join::ChunksEdges,      Join::Whole, Join::WholeEdges,
                                     Join::Compound,     Join::CompoundRing,     Join::Ring,  Join::Listener,
                                     Join::RingFiltered, Join::RingFilteredEdges};

[[nodiscard]] bool banded(Join join)
{
    return join == Join::RingFiltered || join == Join::RingFilteredEdges;
}

// Whether the moving bodies remove internal edges under this join.
[[nodiscard]] bool removesEdges(Join join)
{
    return join == Join::ChunksEdges || join == Join::WholeEdges || join == Join::Compound ||
           join == Join::CompoundRing || join == Join::RingFilteredEdges;
}

[[nodiscard]] JPH::ShapeRefC meshOf(const asset::TerrainMesh& meshed)
{
    JPH::VertexList vertices;
    vertices.reserve(meshed.colliderPoints.size());
    for (const core::Vec3& p : meshed.colliderPoints)
        vertices.push_back(JPH::Float3(p.x, p.y, p.z));
    JPH::IndexedTriangleList triangles;
    for (std::size_t at = 0; at + 2 < meshed.colliderIndices.size(); at += 3)
        triangles.push_back(JPH::IndexedTriangle(meshed.colliderIndices[at], meshed.colliderIndices[at + 1],
                                                 meshed.colliderIndices[at + 2], 0));
    if (triangles.empty())
        return nullptr;
    // As the engine's backend makes a terrain collider: the defaults.
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.SetEmbedded();
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

// **A chunk's collider with its band, as the engine makes it**
// (`asset::meshCollider`, ADR 0143), in Jolt: the band marked 1 in the
// triangle's user data, as the engine's backend marks it.
[[nodiscard]] JPH::ShapeRefC bandedMeshOf(const asset::TerrainField& field, asset::ChunkKey key)
{
    const asset::TerrainCollider made = asset::meshCollider(field, key);
    if (made.indices.size() < 3)
        return nullptr;
    JPH::VertexList vertices;
    vertices.reserve(made.points.size());
    for (const core::Vec3& p : made.points)
        vertices.push_back(JPH::Float3(p.x, p.y, p.z));
    JPH::IndexedTriangleList triangles;
    for (std::size_t at = 0; at + 2 < made.indices.size(); at += 3)
        triangles.push_back(JPH::IndexedTriangle(made.indices[at], made.indices[at + 1], made.indices[at + 2], 0,
                                                 at / 3 >= made.bandFirst ? 1u : 0u));
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.mPerTriangleUserData = true;
    settings.SetEmbedded();
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

// Whether a contact is with a band triangle, which only lends its edges.
[[nodiscard]] bool onBand(const JPH::Shape* shape, const JPH::SubShapeID& subShape)
{
    if (shape == nullptr || shape->GetSubType() != JPH::EShapeSubType::Mesh)
        return false;
    return static_cast<const JPH::MeshShape*>(shape)->GetTriangleUserData(subShape) != 0;
}

[[nodiscard]] asset::MeshRegion chunkRegion(asset::ChunkKey key, int ring)
{
    asset::MeshRegion region;
    region.minX = key.x * static_cast<core::i32>(asset::ChunkEdge) - ring;
    region.minY = key.y * static_cast<core::i32>(asset::ChunkEdge) - ring;
    region.minZ = key.z * static_cast<core::i32>(asset::ChunkEdge) - ring;
    region.cellsX = asset::ChunkEdge + static_cast<core::u32>(2 * ring);
    region.cellsY = asset::ChunkEdge + static_cast<core::u32>(2 * ring);
    region.cellsZ = asset::ChunkEdge + static_cast<core::u32>(2 * ring);
    region.collider = true;
    return region;
}

// The surface's own normal at a point, from the field's occupancy: what a
// contact on a border edge is held against (c).
[[nodiscard]] JPH::Vec3 fieldNormal(const asset::TerrainField& field, JPH::RVec3 at)
{
    const auto distanceAt = [&](double x, double y, double z) {
        // Trilinear over the lattice, whose points are voxel centres.
        const double gx = x - 0.5;
        const double gy = y - 0.5;
        const double gz = z - 0.5;
        const auto x0 = static_cast<core::i32>(std::floor(gx));
        const auto y0 = static_cast<core::i32>(std::floor(gy));
        const auto z0 = static_cast<core::i32>(std::floor(gz));
        const double tx = gx - x0;
        const double ty = gy - y0;
        const double tz = gz - z0;
        double sum = 0.0;
        for (int corner = 0; corner < 8; ++corner) {
            const int dx = corner & 1;
            const int dy = (corner >> 1) & 1;
            const int dz = (corner >> 2) & 1;
            const double w = (dx != 0 ? tx : 1.0 - tx) * (dy != 0 ? ty : 1.0 - ty) * (dz != 0 ? tz : 1.0 - tz);
            sum += w * static_cast<double>(field.sample(x0 + dx, y0 + dy, z0 + dz).distance);
        }
        return sum;
    };
    const double h = 0.5;
    const double x = static_cast<double>(at.GetX());
    const double y = static_cast<double>(at.GetY());
    const double z = static_cast<double>(at.GetZ());
    const JPH::Vec3 gradient(static_cast<float>(distanceAt(x + h, y, z) - distanceAt(x - h, y, z)),
                             static_cast<float>(distanceAt(x, y + h, z) - distanceAt(x, y - h, z)),
                             static_cast<float>(distanceAt(x, y, z + h) - distanceAt(x, y, z - h)));
    return gradient.NormalizedOr(JPH::Vec3::sAxisY());
}

// (c): a contact on a chunk's border edge, against a terrain body, whose
// normal is not the surface's -- the edge's own normal, which a surface that
// goes on past the border does not have -- is dropped; the neighbour's mesh
// holds the body there with its face.
class SeamListener final : public JPH::ContactListener
{
public:
    const asset::TerrainField* field = nullptr;
    JPH::BodyID firstTerrain;
    JPH::uint32 terrainBodies = 0;
    int dropped = 0;

    bool band = false;

    JPH::ValidateResult OnContactValidate(const JPH::Body& one, const JPH::Body& two, JPH::RVec3Arg base,
                                          const JPH::CollideShapeResult& hit) override
    {
        if (band) {
            if ((two.IsStatic() && onBand(two.GetShape(), hit.mSubShapeID2)) ||
                (one.IsStatic() && onBand(one.GetShape(), hit.mSubShapeID1))) {
                ++dropped;
                return JPH::ValidateResult::RejectContact;
            }
            return JPH::ValidateResult::AcceptContact;
        }
        const JPH::Body& terrain = two.IsStatic() ? two : one;
        if (!terrain.IsStatic() || field == nullptr)
            return JPH::ValidateResult::AcceptContact;
        const JPH::RVec3 point = base + (two.IsStatic() ? hit.mContactPointOn2 : hit.mContactPointOn1);
        const auto onBorder = [](double v) {
            const double r = v - ChunkMetres * std::round(v / ChunkMetres);
            return std::abs(r) < 0.02;
        };
        if (!onBorder(static_cast<double>(point.GetX())) && !onBorder(static_cast<double>(point.GetY())) &&
            !onBorder(static_cast<double>(point.GetZ())))
            return JPH::ValidateResult::AcceptContact;
        // The axis moves shape 2 out of shape 1. With the terrain as shape 2
        // that is into the ground, and its outward normal the other way; with
        // it as shape 1, the axis is its outward normal.
        JPH::Vec3 outward = hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY());
        if (two.IsStatic())
            outward = -outward;
        const JPH::Vec3 surface = fieldNormal(*field, point);
        if (outward.Dot(surface) > 0.996f)
            return JPH::ValidateResult::AcceptContact;
        ++dropped;
        return JPH::ValidateResult::RejectContact;
    }
};

class CharacterSeamListener final : public JPH::CharacterContactListener
{
public:
    const asset::TerrainField* field = nullptr;
    const JPH::PhysicsSystem* system = nullptr;
    bool band = false;

    bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact) override
    {
        if (!band || contact.mBodyB.IsInvalid())
            return true;
        const JPH::Body* body = system->GetBodyLockInterfaceNoLock().TryGetBody(contact.mBodyB);
        return body == nullptr || !body->IsStatic() || !onBand(body->GetShape(), contact.mSubShapeIDB);
    }

    void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact,
                        JPH::CharacterContactSettings& settings) override
    {
        if (band)
            return;
        const JPH::RVec3 point = contact.mPosition;
        const JPH::Vec3 normal = contact.mContactNormal;
        if (field == nullptr)
            return;
        const auto onBorder = [](double v) {
            const double r = v - ChunkMetres * std::round(v / ChunkMetres);
            return std::abs(r) < 0.02;
        };
        if (!onBorder(static_cast<double>(point.GetX())) && !onBorder(static_cast<double>(point.GetY())) &&
            !onBorder(static_cast<double>(point.GetZ())))
            return;
        if (normal.Dot(fieldNormal(*field, point)) > 0.996f)
            return;
        settings.mCanPushCharacter = false;
        settings.mCanReceiveImpulses = false;
    }
};

// A world of one ground, joined one way.
struct World
{
    Runtime runtime;
    BroadPhaseLayers broadLayers;
    ObjectVsBroadPhase objectVsBroad;
    PairFilter pairs;
    JPH::TempAllocatorImpl temp{64 * 1024 * 1024};
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::PhysicsSystem system;
    SeamListener seams;
    CharacterSeamListener characterSeams;
    Join join;
    std::vector<JPH::BodyID> ground;
    std::size_t groundBytes = 0;

    World(const asset::TerrainField& field, Join how, int threads = 2) : join(how)
    {
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
        system.Init(65536, 0, 65536, 65536, broadLayers, objectVsBroad, pairs);
        system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        if (how == Join::Listener || banded(how)) {
            seams.field = &field;
            seams.band = banded(how);
            characterSeams.field = &field;
            characterSeams.system = &system;
            characterSeams.band = banded(how);
            system.SetContactListener(&seams);
        }
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        const auto add = [&](const JPH::ShapeRefC& shape) {
            if (shape == nullptr)
                return;
            groundBytes += shape->GetStats().mSizeBytes;
            JPH::BodyCreationSettings settings(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                               JPH::EMotionType::Static, Layers::Still);
            settings.mFriction = 0.6f;
            ground.push_back(bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate));
        };
        const std::vector<asset::ChunkKey> keys = field.chunkKeys();
        if (how == Join::Whole || how == Join::WholeEdges) {
            asset::ChunkKey low = keys.front();
            asset::ChunkKey high = keys.front();
            for (const asset::ChunkKey& key : keys) {
                low = asset::ChunkKey{std::min(low.x, key.x), std::min(low.y, key.y), std::min(low.z, key.z)};
                high = asset::ChunkKey{std::max(high.x, key.x), std::max(high.y, key.y), std::max(high.z, key.z)};
            }
            asset::MeshRegion region;
            region.minX = low.x * static_cast<core::i32>(asset::ChunkEdge);
            region.minY = low.y * static_cast<core::i32>(asset::ChunkEdge);
            region.minZ = low.z * static_cast<core::i32>(asset::ChunkEdge);
            region.cellsX = static_cast<core::u32>(high.x - low.x + 1) * asset::ChunkEdge;
            region.cellsY = static_cast<core::u32>(high.y - low.y + 1) * asset::ChunkEdge;
            region.cellsZ = static_cast<core::u32>(high.z - low.z + 1) * asset::ChunkEdge;
            region.collider = true;
            asset::prepareRegion(field, region);
            add(meshOf(asset::meshField(field, region)));
        }
        else if (how == Join::Compound || how == Join::CompoundRing) {
            JPH::StaticCompoundShapeSettings compound;
            int parts = 0;
            for (const asset::ChunkKey& key : keys) {
                const asset::MeshRegion region = chunkRegion(key, how == Join::CompoundRing ? 1 : 0);
                asset::prepareRegion(field, region);
                const JPH::ShapeRefC part = meshOf(asset::meshField(field, region));
                if (part == nullptr)
                    continue;
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), part);
                ++parts;
            }
            if (parts > 0) {
                const JPH::ShapeSettings::ShapeResult made = compound.Create();
                REQUIRE_FALSE(made.HasError());
                add(made.Get());
            }
        }
        else if (banded(how)) {
            for (const asset::ChunkKey& key : keys)
                add(bandedMeshOf(field, key));
        }
        else {
            const int ring = how == Join::Ring ? 1 : 0;
            for (const asset::ChunkKey& key : keys) {
                const asset::MeshRegion region = chunkRegion(key, ring);
                asset::prepareRegion(field, region);
                add(meshOf(asset::meshField(field, region)));
            }
        }
        system.OptimizeBroadPhase();
    }

    // How a moving body is made for this join: the compound's moving bodies
    // remove internal edges across its parts.
    void moving(JPH::BodyCreationSettings& settings) const
    {
        settings.mEnhancedInternalEdgeRemoval = removesEdges(join);
    }

    void step() { system.Update(Dt, 1, &temp, jobs.get()); }
};

// --- The grounds -----------------------------------------------------------------

// Flat at y = 1, 320 m square: seams on x and z every 32 m.
[[nodiscard]] asset::TerrainField flatGround()
{
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 320.0f, 1.0f, 1);
    return field;
}

// A slope rising 1 in 10 along x, 320 m square, through the chunk seams at
// y = 0 and y = -32 as well as those on x and z.
[[nodiscard]] asset::TerrainField slopeGround()
{
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    constexpr core::u32 Columns = 321;
    std::vector<float> heights(Columns * Columns);
    for (core::u32 z = 0; z < Columns; ++z)
        for (core::u32 x = 0; x < Columns; ++x)
            heights[z * Columns + x] = 0.1f * (static_cast<float>(x) - 160.0f) - 3.0f;
    (void)asset::writeHeights(field, -160, -160, Columns, heights, 1);
    return field;
}

// --- What each crossing measures --------------------------------------------------

struct Crossing
{
    // Across the seam, and across the middle of a chunk, the same drive.
    double seamJolt = 0.0;
    double middleJolt = 0.0;
    double seamLift = 0.0;
    double middleLift = 0.0;
    double seamSpeedLoss = 0.0;
    double middleSpeedLoss = 0.0;
};

// The largest change in a series from one tick to the next, over the ticks
// where `near` holds.
struct Watch
{
    double worstStep = 0.0;
    double highest = -1e9;
    double lowest = 1e9;
    double firstSpeed = -1.0;
    double lastSpeed = 0.0;
    bool have = false;
    double last = 0.0;

    void see(double value, double height, double speed)
    {
        if (have)
            worstStep = std::max(worstStep, std::abs(value - last));
        last = value;
        have = true;
        highest = std::max(highest, height);
        lowest = std::min(lowest, height);
        if (firstSpeed < 0.0)
            firstSpeed = speed;
        lastSpeed = speed;
    }
};

// --- A ball ---------------------------------------------------------------------

// A ball at `speed` along `heading` (radians from +x), crossing x = `crossAt`
// near tick 60. Rolling with friction, or sliding with none. Answers the jolt
// in its vertical velocity, the lift and the speed lost across the metres
// round the crossing.
Watch rollBall(World& world, const asset::TerrainField& field, double crossAt, double speed, double heading,
               bool friction, bool middle)
{
    JPH::BodyInterface& bodies = world.system.GetBodyInterface();
    const double target = middle ? crossAt + 16.0 : crossAt;
    const double dx = std::cos(heading);
    const double dz = std::sin(heading);
    const double startX = target - dx * speed * 1.0;
    const double startZ = 3.3 - dz * speed * 1.0;
    const std::optional<asset::TerrainHit> ground =
        asset::raycastField(field, core::DVec3{startX, 60.0, startZ}, core::Vec3{0.0f, -1.0f, 0.0f}, 200.0);
    REQUIRE(ground.has_value());
    JPH::BodyCreationSettings settings(new JPH::SphereShape(0.5f), rvec(startX, ground->position.y + 0.5, startZ),
                                       JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::Moving);
    settings.mFriction = friction ? 0.8f : 0.0f;
    settings.mLinearDamping = 0.0f;
    settings.mAngularDamping = 0.0f;
    world.moving(settings);
    const JPH::BodyID ball = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    const JPH::Vec3 velocity(static_cast<float>(dx * speed), 0.0f, static_cast<float>(dz * speed));
    bodies.SetLinearVelocity(ball, velocity);
    if (friction)
        bodies.SetAngularVelocity(
            ball, JPH::Vec3(static_cast<float>(dz * speed / 0.5), 0.0f, static_cast<float>(-dx * speed / 0.5)));
    // Settle a tick on the ground, then watch the metres round the crossing.
    Watch watch;
    for (int tick = 0; tick < 120; ++tick) {
        world.step();
        const JPH::RVec3 at = bodies.GetPosition(ball);
        const JPH::Vec3 v = bodies.GetLinearVelocity(ball);
        const double along =
            (static_cast<double>(at.GetX()) - target) * dx + (static_cast<double>(at.GetZ()) - 3.3) * dz;
        if (std::abs(along) < 2.0) {
            // Height above the ground under it, so a slope is not a lift.
            const std::optional<asset::TerrainHit> under = asset::raycastField(
                field, core::DVec3{static_cast<double>(at.GetX()), 60.0, static_cast<double>(at.GetZ())},
                core::Vec3{0.0f, -1.0f, 0.0f}, 200.0);
            const double lift = under.has_value() ? static_cast<double>(at.GetY()) - 0.5 - under->position.y : 0.0;
            // The velocity off the surface: across the slope's own normal.
            const JPH::Vec3 n =
                fieldNormal(field, rvec(static_cast<double>(at.GetX()), under.has_value() ? under->position.y : 0.0,
                                        static_cast<double>(at.GetZ())));
            watch.see(static_cast<double>(v.Dot(n)), lift, static_cast<double>(v.Length()));
        }
    }
    bodies.RemoveBody(ball);
    bodies.DestroyBody(ball);
    return watch;
}

// --- A wheeled vehicle ---------------------------------------------------------------

struct Vehicle
{
    JPH::BodyID body;
    JPH::Ref<JPH::VehicleConstraint> constraint;
};

[[nodiscard]] Vehicle makeVehicle(World& world, JPH::RVec3 at, float yaw, bool cylinders)
{
    JPH::BodyInterface& bodies = world.system.GetBodyInterface();
    const float halfWidth = 0.9f;
    const float halfLength = 2.0f;
    const float halfHeight = 0.4f;
    JPH::RefConst<JPH::Shape> chassis =
        JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0.0f, -halfHeight, 0.0f),
                                             new JPH::BoxShape(JPH::Vec3(halfWidth, halfHeight, halfLength)))
            .Create()
            .Get();
    JPH::BodyCreationSettings settings(chassis, at, JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw),
                                       JPH::EMotionType::Dynamic, Layers::Moving);
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = 1500.0f;
    world.moving(settings);
    Vehicle vehicle;
    vehicle.body = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);

    JPH::VehicleConstraintSettings vehicleSettings;
    const float wheelRadius = 0.35f;
    const float wheelWidth = 0.25f;
    const std::array<JPH::Vec3, 4> positions{
        JPH::Vec3(halfWidth, -0.1f, halfLength - 0.4f), JPH::Vec3(-halfWidth, -0.1f, halfLength - 0.4f),
        JPH::Vec3(halfWidth, -0.1f, -halfLength + 0.4f), JPH::Vec3(-halfWidth, -0.1f, -halfLength + 0.4f)};
    for (const JPH::Vec3& position : positions) {
        JPH::WheelSettingsWV* wheel = new JPH::WheelSettingsWV;
        wheel->mPosition = position;
        wheel->mRadius = wheelRadius;
        wheel->mWidth = wheelWidth;
        wheel->mSuspensionMinLength = 0.3f;
        wheel->mSuspensionMaxLength = 0.5f;
        wheel->mSuspensionSpring.mFrequency = 1.5f;
        wheel->mSuspensionSpring.mDamping = 0.5f;
        vehicleSettings.mWheels.push_back(wheel);
    }
    JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings;
    vehicleSettings.mController = controller;
    controller->mDifferentials.resize(1);
    controller->mDifferentials[0].mLeftWheel = 0;
    controller->mDifferentials[0].mRightWheel = 1;
    JPH::Body* body = world.system.GetBodyLockInterfaceNoLock().TryGetBody(vehicle.body);
    REQUIRE(body != nullptr);
    vehicle.constraint = new JPH::VehicleConstraint(*body, vehicleSettings);
    if (cylinders)
        vehicle.constraint->SetVehicleCollisionTester(
            new JPH::VehicleCollisionTesterCastCylinder(Layers::Moving, 0.05f));
    else
        vehicle.constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(Layers::Moving));
    world.system.AddConstraint(vehicle.constraint);
    world.system.AddStepListener(vehicle.constraint);
    return vehicle;
}

void removeVehicle(World& world, Vehicle& vehicle)
{
    world.system.RemoveStepListener(vehicle.constraint);
    world.system.RemoveConstraint(vehicle.constraint);
    JPH::BodyInterface& bodies = world.system.GetBodyInterface();
    bodies.RemoveBody(vehicle.body);
    bodies.DestroyBody(vehicle.body);
}

// A vehicle coasting at `speed` along `heading`, crossing x = `target`. The
// jolt is the largest change in total suspension force from one tick to the
// next while the car's wheels are within two metres of the target.
struct Drive
{
    double forceJolt = 0.0;
    double accelerationJolt = 0.0;
    double speedLoss = 0.0;
};

Drive drive(World& world, const asset::TerrainField& field, double target, double speed, double heading, bool cylinders)
{
    JPH::BodyInterface& bodies = world.system.GetBodyInterface();
    const double dx = std::cos(heading);
    const double dz = std::sin(heading);
    // Settled for a second and a half before the target, at speed.
    const double lead = speed * 3.0 + 3.0;
    const double startX = target - dx * lead;
    const double startZ = 3.3 - dz * lead;
    const std::optional<asset::TerrainHit> ground =
        asset::raycastField(field, core::DVec3{startX, 60.0, startZ}, core::Vec3{0.0f, -1.0f, 0.0f}, 200.0);
    REQUIRE(ground.has_value());
    // Facing along the heading: the car's +z forward.
    const auto yaw = static_cast<float>(std::atan2(dx, dz));
    Vehicle car = makeVehicle(world, rvec(startX, ground->position.y + 1.2, startZ), yaw, cylinders);
    bodies.SetLinearVelocity(car.body, JPH::Vec3(static_cast<float>(dx * speed), 0.0f, static_cast<float>(dz * speed)));
    auto* controller = static_cast<JPH::WheeledVehicleController*>(car.constraint->GetController());
    controller->SetDriverInput(0.0f, 0.0f, 0.0f, 0.0f);

    Drive out;
    std::vector<double> forces;
    std::vector<double> accelerations;
    double lastVy = 0.0;
    double firstSpeed = -1.0;
    double lastSpeed = 0.0;
    const int ticks = static_cast<int>(std::ceil((lead + 8.0) / speed / TickSeconds)) + 10;
    for (int tick = 0; tick < ticks; ++tick) {
        // Keep it at speed: a coasting car on a slope would not be.
        const JPH::Vec3 v = bodies.GetLinearVelocity(car.body);
        world.step();
        const JPH::RVec3 at = bodies.GetPosition(car.body);
        const JPH::Vec3 now = bodies.GetLinearVelocity(car.body);
        double force = 0.0;
        for (JPH::uint w = 0; w < 4; ++w)
            force += static_cast<double>(car.constraint->GetWheel(w)->GetSuspensionLambda()) / TickSeconds;
        const double accel = (static_cast<double>(now.GetY()) - lastVy) / TickSeconds;
        lastVy = static_cast<double>(now.GetY());
        const double along =
            (static_cast<double>(at.GetX()) - target) * dx + (static_cast<double>(at.GetZ()) - 3.3) * dz;
        if (tick > 90 && std::abs(along) < 4.0) {
            forces.push_back(force);
            accelerations.push_back(accel);
            if (firstSpeed < 0.0)
                firstSpeed = static_cast<double>(v.Length());
            lastSpeed = static_cast<double>(now.Length());
        }
    }
    out.speedLoss = firstSpeed > 0.0 ? (firstSpeed - lastSpeed) : 0.0;
    // **A jolt is a break in the force, not its trend**: the largest second
    // difference of the suspension's total force over the metres round the
    // target -- a car speeding up downhill changes its force smoothly, and a
    // seam kicks it for a tick.
    for (std::size_t at = 1; at + 1 < forces.size(); ++at)
        out.forceJolt = std::max(out.forceJolt, std::abs(forces[at] - 0.5 * (forces[at - 1] + forces[at + 1])));
    for (std::size_t at = 1; at + 1 < accelerations.size(); ++at)
        out.accelerationJolt = std::max(
            out.accelerationJolt, std::abs(accelerations[at] - 0.5 * (accelerations[at - 1] + accelerations[at + 1])));
    removeVehicle(world, car);
    return out;
}

// --- A character ---------------------------------------------------------------

Watch walk(World& world, const asset::TerrainField& field, double target, double speed, double heading)
{
    const double dx = std::cos(heading);
    const double dz = std::sin(heading);
    const double startX = target - dx * (speed * 1.0 + 2.0);
    const double startZ = 3.3 - dz * (speed * 1.0 + 2.0);
    const std::optional<asset::TerrainHit> ground =
        asset::raycastField(field, core::DVec3{startX, 60.0, startZ}, core::Vec3{0.0f, -1.0f, 0.0f}, 200.0);
    REQUIRE(ground.has_value());
    JPH::CharacterVirtualSettings settings;
    settings.mShape = new JPH::CapsuleShape(1.5f, 0.5f);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(46.0f);
    // Under the engine's own join a character is as the engine makes one:
    // without the flag, which stood it still on open ground (D574) and which
    // the band and its filter do not need.
    settings.mEnhancedInternalEdgeRemoval = removesEdges(world.join) && world.join != Join::RingFilteredEdges;
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        &settings, rvec(startX, ground->position.y + 2.0, startZ), JPH::Quat::sIdentity(), &world.system);
    if (world.join == Join::Listener || banded(world.join))
        character->SetListener(&world.characterSeams);
    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    Watch watch;
    double lastX = startX;
    double lastZ = startZ;
    for (int tick = 0; tick < 150; ++tick) {
        JPH::Vec3 velocity(static_cast<float>(dx * speed), 0.0f, static_cast<float>(dz * speed));
        if (character->GetGroundState() != JPH::CharacterVirtual::EGroundState::OnGround)
            velocity += JPH::Vec3(0.0f, character->GetLinearVelocity().GetY() - 9.81f * Dt, 0.0f);
        character->SetLinearVelocity(velocity);
        character->ExtendedUpdate(Dt, JPH::Vec3(0.0f, -9.81f, 0.0f), update,
                                  world.system.GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                  world.system.GetDefaultLayerFilter(Layers::Moving), {}, {}, world.temp);
        const JPH::RVec3 at = character->GetPosition();
        const double moved =
            std::hypot(static_cast<double>(at.GetX()) - lastX, static_cast<double>(at.GetZ()) - lastZ) / TickSeconds;
        lastX = static_cast<double>(at.GetX());
        lastZ = static_cast<double>(at.GetZ());
        const double along =
            (static_cast<double>(at.GetX()) - target) * dx + (static_cast<double>(at.GetZ()) - 3.3) * dz;
        if (tick > 10 && std::abs(along) < 2.0) {
            const std::optional<asset::TerrainHit> under = asset::raycastField(
                field, core::DVec3{static_cast<double>(at.GetX()), 60.0, static_cast<double>(at.GetZ())},
                core::Vec3{0.0f, -1.0f, 0.0f}, 200.0);
            const double lift = under.has_value() ? static_cast<double>(at.GetY()) - 2.0 - under->position.y : 0.0;
            watch.see(moved, lift, moved);
        }
    }
    return watch;
}

// --- A box at rest across a seam -------------------------------------------------

double creep(World& world, double seconds)
{
    JPH::BodyInterface& bodies = world.system.GetBodyInterface();
    JPH::BodyCreationSettings settings(new JPH::BoxShape(JPH::Vec3(1.0f, 0.5f, 1.0f)), rvec(0.0, 1.5, 0.0),
                                       JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::Moving);
    settings.mAllowSleeping = false;
    world.moving(settings);
    const JPH::BodyID box = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    for (int tick = 0; tick < 30; ++tick)
        world.step();
    const JPH::RVec3 from = bodies.GetPosition(box);
    const int ticks = static_cast<int>(seconds / TickSeconds);
    for (int tick = 0; tick < ticks; ++tick)
        world.step();
    const JPH::RVec3 to = bodies.GetPosition(box);
    bodies.RemoveBody(box);
    bodies.DestroyBody(box);
    return static_cast<double>((to - from).Length());
}

} // namespace

TEST_CASE("the seam between two chunks' colliders, felt nine ways" * doctest::skip())
{
    const asset::TerrainField flat = flatGround();
    const asset::TerrainField slope = slopeGround();
    std::printf("\n%-9s %-44s %10s %10s %10s\n", "join", "what", "seam", "middle", "unit");
    for (const Join join : Joins) {
        for (const bool sloped : {false, true}) {
            const asset::TerrainField& field = sloped ? slope : flat;
            World world(field, join);
            const std::string where = sloped ? "slope" : "flat";
            const double down = sloped ? 3.14159265358979 : 0.0;
            for (const double turn : {0.0, 0.5}) {
                const double heading = down + turn;
                const std::string how = where + (turn == 0.0 ? " straight" : " diagonal");
                for (const bool friction : {false, true}) {
                    const Watch seam = rollBall(world, field, 0.0, 8.0, heading, friction, false);
                    const Watch middle = rollBall(world, field, 0.0, 8.0, heading, friction, true);
                    std::printf("%-9s %-44s %10.4f %10.4f %10s\n", nameOf(join),
                                (how + (friction ? " rolling ball: v.n jolt" : " sliding ball: v.n jolt")).c_str(),
                                seam.worstStep, middle.worstStep, "m/s");
                    std::printf("%-9s %-44s %10.4f %10.4f %10s\n", nameOf(join),
                                (how + (friction ? " rolling ball: lift" : " sliding ball: lift")).c_str(),
                                seam.highest, middle.highest, "m");
                    std::printf("%-9s %-44s %10.4f %10.4f %10s\n", nameOf(join),
                                (how + (friction ? " rolling ball: speed lost" : " sliding ball: speed lost")).c_str(),
                                seam.firstSpeed - seam.lastSpeed, middle.firstSpeed - middle.lastSpeed, "m/s");
                }
                for (const double speed : {5.0, 20.0, 40.0}) {
                    for (const bool cylinders : {false, true}) {
                        const Drive seam = drive(world, field, 0.0, speed, heading, cylinders);
                        const Drive middle = drive(world, field, 16.0, speed, heading, cylinders);
                        char label[96];
                        std::snprintf(label, sizeof(label), "%s car %2.0f m/s %s: force break", how.c_str(), speed,
                                      cylinders ? "cyl" : "ray");
                        std::printf("%-9s %-44s %10.1f %10.1f %10s\n", nameOf(join), label, seam.forceJolt,
                                    middle.forceJolt, "N");
                        std::snprintf(label, sizeof(label), "%s car %2.0f m/s %s: accel break", how.c_str(), speed,
                                      cylinders ? "cyl" : "ray");
                        std::printf("%-9s %-44s %10.3f %10.3f %10s\n", nameOf(join), label, seam.accelerationJolt,
                                    middle.accelerationJolt, "m/s2/tick");
                    }
                }
                for (const double speed : {4.0, 8.0}) {
                    const Watch seam = walk(world, field, 0.0, speed, heading);
                    const Watch middle = walk(world, field, 16.0, speed, heading);
                    char label[96];
                    std::snprintf(label, sizeof(label), "%s character %1.0f m/s: speed jolt", how.c_str(), speed);
                    std::printf("%-9s %-44s %10.4f %10.4f %10s\n", nameOf(join), label, seam.worstStep,
                                middle.worstStep, "m/s");
                    std::snprintf(label, sizeof(label), "%s character %1.0f m/s: lift", how.c_str(), speed);
                    std::printf("%-9s %-44s %10.4f %10.4f %10s\n", nameOf(join), label, seam.highest - seam.lowest,
                                middle.highest - middle.lowest, "m");
                }
            }
            if (!sloped)
                std::printf("%-9s %-44s %10.5f %10s %10s\n", nameOf(join), "flat box resting across: creep in 10 s",
                            creep(world, 10.0), "", "m");
            std::fflush(stdout);
        }
    }
}

namespace {

void measureCosts(int patches)
{
    // A 4 km terrain collides only near what moves (`TerrainCollisionReach`):
    // what exists at once is the chunks round the movers. Here, 128 patches of
    // 4 by 4 chunk columns of rolling ground spread over the 4 km -- a couple
    // of hundred movers' worth -- and what each join costs to build, to hold,
    // to query, and to change one chunk as a dig does.
    Runtime runtime;
    asset::TerrainField field(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    for (int patch = 0; patch < patches; ++patch) {
        // Spread over a 4 km square: 128 chunk columns a side, patches of 4.
        const int px = ((patch * 37) % 32) * 4 - 64;
        const int pz = ((patch * 61 + patch / 32) % 32) * 4 - 64;
        constexpr core::u32 Columns = 128;
        std::vector<float> heights(Columns * Columns);
        for (core::u32 z = 0; z < Columns; ++z)
            for (core::u32 x = 0; x < Columns; ++x)
                heights[z * Columns + x] = 4.0f * std::sin(0.1f * static_cast<float>(x)) +
                                           3.0f * std::cos(0.13f * static_cast<float>(z)) + 10.0f;
        (void)asset::writeHeights(field, px * 32, pz * 32, Columns, heights, 1);
    }
    const std::vector<asset::ChunkKey> all = field.chunkKeys();
    std::printf("\nfield chunks %zu\n", all.size());
    // Meshed once; each join builds its bodies from the same meshes.
    std::vector<asset::TerrainMesh> meshes(all.size());
    std::vector<asset::TerrainMesh> ringed(all.size());
    for (std::size_t at = 0; at < all.size(); ++at) {
        const asset::MeshRegion region = chunkRegion(all[at], 0);
        asset::prepareRegion(field, region);
        meshes[at] = asset::meshField(field, region);
        const asset::MeshRegion wide = chunkRegion(all[at], 1);
        asset::prepareRegion(field, wide);
        ringed[at] = asset::meshField(field, wide);
    }
    {
        std::size_t plain = 0;
        std::size_t wide = 0;
        std::size_t plainPoints = 0;
        std::size_t widePoints = 0;
        for (std::size_t at = 0; at < all.size(); ++at) {
            plain += meshes[at].colliderIndices.size() / 3;
            wide += ringed[at].colliderIndices.size() / 3;
            plainPoints += meshes[at].colliderPoints.size();
            widePoints += ringed[at].colliderPoints.size();
        }
        std::printf("triangles %zu, ringed %zu; points %zu, ringed %zu\n", plain, wide, plainPoints, widePoints);
    }
    std::printf("%-10s %10s %10s %10s %12s %10s\n", "join", "build ms", "MiB", "ray us", "overlap us", "dig ms");
    for (const int kind : {0, 1, 2, 3, 4}) {
        // 0 chunks, 1 a mutable compound, 2 a static compound rebuilt on a
        // change, 3 the static compound of ringed meshes.
        const Join join = kind == 0   ? Join::Chunks
                          : kind == 4 ? Join::RingFiltered
                          : kind == 3 ? Join::CompoundRing
                                      : Join::Compound;
        const bool rebuilt = kind == 2 || kind == 3;
        BroadPhaseLayers broadLayers;
        ObjectVsBroadPhase objectVsBroad;
        PairFilter pairs;
        JPH::PhysicsSystem system;
        system.Init(65536, 0, 65536, 65536, broadLayers, objectVsBroad, pairs);
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        std::size_t bytes = 0;
        const auto begin = std::chrono::steady_clock::now();
        std::vector<JPH::BodyID> made;
        std::vector<std::size_t> partOf(all.size(), 0);
        JPH::Ref<JPH::MutableCompoundShape> compound;
        JPH::BodyID compoundBody;
        std::vector<JPH::ShapeRefC> parts;
        const auto staticOf = [&]() {
            JPH::StaticCompoundShapeSettings settings;
            for (const JPH::ShapeRefC& part : parts)
                settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), part);
            const JPH::ShapeSettings::ShapeResult shape = settings.Create();
            REQUIRE_FALSE(shape.HasError());
            return shape.Get();
        };
        if (rebuilt) {
            for (std::size_t at = 0; at < meshes.size(); ++at) {
                const JPH::ShapeRefC part = meshOf(join == Join::CompoundRing ? ringed[at] : meshes[at]);
                if (part == nullptr)
                    continue;
                partOf[at] = parts.size();
                bytes += part->GetStats().mSizeBytes;
                parts.push_back(part);
            }
            JPH::BodyCreationSettings body(staticOf(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static, Layers::Still);
            compoundBody = bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate);
        }
        else if (join == Join::Compound || join == Join::CompoundRing) {
            JPH::MutableCompoundShapeSettings settings;
            JPH::uint count = 0;
            for (std::size_t at = 0; at < meshes.size(); ++at) {
                const JPH::ShapeRefC part = meshOf(join == Join::CompoundRing ? ringed[at] : meshes[at]);
                if (part == nullptr)
                    continue;
                partOf[at] = count++;
                settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), part);
            }
            const JPH::ShapeSettings::ShapeResult shape = settings.Create();
            REQUIRE_FALSE(shape.HasError());
            compound = static_cast<JPH::MutableCompoundShape*>(const_cast<JPH::Shape*>(shape.Get().GetPtr()));
            JPH::Shape::VisitedShapes visited;
            bytes += compound->GetStatsRecursive(visited).mSizeBytes;
            JPH::BodyCreationSettings body(JPH::ShapeRefC(compound), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static, Layers::Still);
            compoundBody = bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate);
        }
        else {
            const std::vector<asset::TerrainMesh>& source = join == Join::Ring || banded(join) ? ringed : meshes;
            for (std::size_t at = 0; at < source.size(); ++at) {
                const JPH::ShapeRefC shape = banded(join) ? bandedMeshOf(field, all[at]) : meshOf(source[at]);
                if (shape == nullptr)
                    continue;
                partOf[at] = made.size();
                bytes += shape->GetStats().mSizeBytes;
                JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                               JPH::EMotionType::Static, Layers::Still);
                made.push_back(bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate));
            }
        }
        system.OptimizeBroadPhase();
        const double buildMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

        const JPH::NarrowPhaseQuery& query = system.GetNarrowPhaseQuery();
        constexpr int Rays = 20000;
        int hits = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < Rays; ++i) {
            const asset::ChunkKey key = all[static_cast<std::size_t>(i * 7919) % all.size()];
            const double x = key.x * 32.0 + (i % 31) + 0.5;
            const double z = key.z * 32.0 + (i % 29) + 0.5;
            const JPH::RRayCast ray(rvec(x, 60.0, z), JPH::Vec3(0.0f, -120.0f, 0.0f));
            JPH::RayCastResult result;
            if (query.CastRay(ray, result))
                ++hits;
        }
        const double rayUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / Rays;
        const JPH::SphereShape sphere(1.0f);
        constexpr int Overlaps = 5000;
        std::size_t touched = 0;
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < Overlaps; ++i) {
            const asset::ChunkKey key = all[static_cast<std::size_t>(i * 104729) % all.size()];
            const double x = key.x * 32.0 + (i % 31) + 0.5;
            const double z = key.z * 32.0 + (i % 29) + 0.5;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            query.CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(rvec(x, 10.0, z)),
                               JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);
            touched += collector.mHits.size();
        }
        const double overlapUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / Overlaps;

        // A dig: one chunk's mesh replaced, sixty times over.
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < 60; ++i) {
            const std::size_t at = static_cast<std::size_t>(i * 131) % all.size();
            const JPH::ShapeRefC shape =
                banded(join) ? bandedMeshOf(field, all[at])
                             : meshOf(join == Join::Ring || join == Join::CompoundRing ? ringed[at] : meshes[at]);
            if (shape == nullptr)
                continue;
            if (rebuilt) {
                parts[partOf[at]] = shape;
                bodies.SetShape(compoundBody, staticOf(), false, JPH::EActivation::DontActivate);
            }
            else if (join == Join::Compound || join == Join::CompoundRing) {
                compound->ModifyShape(static_cast<JPH::uint>(partOf[at]), JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                                      shape);
                bodies.NotifyShapeChanged(compoundBody, JPH::Vec3::sZero(), false, JPH::EActivation::DontActivate);
            }
            else {
                const JPH::BodyID old = made[partOf[at]];
                bodies.RemoveBody(old);
                bodies.DestroyBody(old);
                JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                               JPH::EMotionType::Static, Layers::Still);
                made[partOf[at]] = bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate);
            }
        }
        const double digMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 60.0;
        const char* names[] = {"chunks", "mutable", "static", "static+r", "ring+f"};
        std::printf("%-10s %10.1f %10.1f %10.3f %12.3f %10.3f   (hits %d, touches %zu)\n", names[kind], buildMs,
                    static_cast<double>(bytes) / (1024.0 * 1024.0), rayUs, overlapUs, digMs, hits, touched);
        std::fflush(stdout);
    }
}

} // namespace

TEST_CASE("what each join costs on a large world: queries, memory and a dig" * doctest::skip())
{
    measureCosts(128);
}

TEST_CASE("every kind of body crosses a seam between chunks' colliders as it crosses one surface (ADR 0143)")
{
    // The owner's ruling of 2026-09-30: the ground behaves as one continuous
    // surface for every kind of body. The engine's join -- each chunk with its
    // band, the band refused in contacts, moving bodies removing internal
    // edges -- held against one mesh with no seam at all, the same drive over
    // the same place. The tolerances are what one surface's own run-to-run
    // and place-to-place noise allows.
    for (const bool sloped : {false, true}) {
        CAPTURE(sloped);
        const asset::TerrainField field = sloped ? slopeGround() : flatGround();
        World banded(field, Join::RingFilteredEdges);
        World whole(field, Join::WholeEdges);
        const double down = sloped ? 3.14159265358979 : 0.0;
        for (const double turn : {0.0, 0.5}) {
            CAPTURE(turn);
            const double heading = down + turn;
            for (const bool friction : {false, true}) {
                CAPTURE(friction);
                const Watch seam = rollBall(banded, field, 0.0, 8.0, heading, friction, false);
                const Watch reference = rollBall(whole, field, 0.0, 8.0, heading, friction, false);
                // No hop and no speed change a ball on one surface does not
                // have: within a centimetre a second and a millimetre.
                CHECK(seam.worstStep < reference.worstStep + 0.01);
                CHECK(seam.highest < reference.highest + 0.001);
                CHECK(std::abs((seam.firstSpeed - seam.lastSpeed) - (reference.firstSpeed - reference.lastSpeed)) <
                      0.01);
            }
            for (const double speed : {5.0, 20.0, 40.0}) {
                CAPTURE(speed);
                const Drive seam = drive(banded, field, 0.0, speed, heading, true);
                const Drive reference = drive(whole, field, 0.0, speed, heading, true);
                // The suspension's force breaks no more than on one surface:
                // a car on these 1 m voxels feels their facets, seam or none.
                CHECK(seam.forceJolt < reference.forceJolt * 1.3 + 30.0);
            }
            for (const double speed : {4.0, 8.0}) {
                CAPTURE(speed);
                const Watch seam = walk(banded, field, 0.0, speed, heading);
                const Watch reference = walk(whole, field, 0.0, speed, heading);
                CHECK(seam.worstStep < reference.worstStep + 0.005);
                CHECK(seam.highest - seam.lowest < reference.highest - reference.lowest + 0.001);
            }
        }
        if (!sloped)
            CHECK(creep(banded, 10.0) < 0.001);
    }
}

TEST_CASE("the seam's answer is the same on one worker as on four")
{
    // R10: the band's refusals are a function of the triangle, and Jolt's
    // answer is its own for a given thread count; the engine fixes the count
    // (ADR 0064), and this holds the band to the same across counts.
    const asset::TerrainField field = flatGround();
    const auto run = [&](int threads) {
        World world(field, Join::RingFilteredEdges, threads);
        JPH::BodyInterface& bodies = world.system.GetBodyInterface();
        std::vector<JPH::BodyID> balls;
        for (int i = 0; i < 12; ++i) {
            JPH::BodyCreationSettings settings(new JPH::SphereShape(0.5f), rvec(-6.0 + i * 0.9, 1.5 + i * 0.1, 0.4 * i),
                                               JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::Moving);
            world.moving(settings);
            balls.push_back(bodies.CreateAndAddBody(settings, JPH::EActivation::Activate));
            bodies.SetLinearVelocity(balls.back(), JPH::Vec3(6.0f, 0.0f, 1.0f));
        }
        for (int tick = 0; tick < 180; ++tick)
            world.step();
        std::vector<float> state;
        for (const JPH::BodyID& ball : balls) {
            const JPH::RVec3 at = bodies.GetPosition(ball);
            state.insert(state.end(), {static_cast<float>(static_cast<double>(at.GetX())),
                                       static_cast<float>(static_cast<double>(at.GetY())),
                                       static_cast<float>(static_cast<double>(at.GetZ()))});
        }
        return state;
    };
    const std::vector<float> one = run(1);
    const std::vector<float> four = run(4);
    REQUIRE(one.size() == four.size());
    for (std::size_t at = 0; at < one.size(); ++at) {
        CAPTURE(at);
        CHECK(one[at] == four[at]);
    }
}
