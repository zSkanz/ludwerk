#include "engine/scene/physics_sync.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/asset/terrain_mesher.h"
#include "engine/asset/voxel_mesher.h"
#include "engine/jobs/jobs.h"
#include "engine/scene/players.h"
#include "engine/scene/voxel_fluid.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"

namespace engine::scene {
namespace {

// The instance id IS the body's user data. Eight bytes each way, no table, and
// a query hit resolves back to an instance by decoding rather than by looking
// up -- with the liveness check the world already knows how to do.
[[nodiscard]] u64 packInstance(core::InstanceId id) noexcept
{
    return (static_cast<u64>(id.generation) << 32) | id.index;
}

[[nodiscard]] core::InstanceId unpackInstance(u64 packed) noexcept
{
    return core::InstanceId{static_cast<u32>(packed & 0xffffffffu), static_cast<u32>(packed >> 32)};
}

// Whether two descriptions ask for the same body.
//
// **`pointsRevision` is here because the geometry can change underneath a hull
// that is otherwise identical** -- a mesh finishing its load, or a hot reload
// replacing one. Without it, a `MeshPart` that came up as a hull and then had
// its points replaced kept the first hull for ever, and the only reason nobody
// hit it is that meshes used to load inside the frame that created the part.
//
// The spans themselves are deliberately not compared. Comparing them would mean
// keeping one, and `ShapeDesc::points` may not outlive the create call.
[[nodiscard]] bool sameShape(const physics::ShapeDesc& a, const physics::ShapeDesc& b) noexcept
{
    return a.type == b.type && a.size == b.size && a.pointScale == b.pointScale &&
           a.pointsRevision == b.pointsRevision && a.geometryRevision == b.geometryRevision &&
           a.heightSampleCount == b.heightSampleCount && a.heightBlockSize == b.heightBlockSize &&
           a.heightMin == b.heightMin && a.heightMax == b.heightMax;
}

// The description as a RECORD may keep it: everything except the spans, whose
// documented lifetime is the call they were handed to.
//
// **Every span, not just `points`** (ADR 0066). `indices` and `heights` arrived
// with the two static shapes under the same rule, and a record that kept either
// would be holding a view into a buffer the next brush stroke replaces -- a
// dangling read rather than a stale one. `geometryRevision` is what lets
// `sameShape` above answer "are these the same triangles" without keeping any
// of them, exactly as `pointsRevision` does for a hull.
[[nodiscard]] physics::ShapeDesc withoutPoints(physics::ShapeDesc shape) noexcept
{
    shape.points = {};
    shape.indices = {};
    shape.heights = {};
    return shape;
}

} // namespace

PhysicsSync::PhysicsSync(World& world, physics::IPhysics3D& backend) : m_scene(world), m_backend(backend)
{
    physics::WorldDesc desc;
    m_world = m_backend.createWorld(desc);
}

PhysicsSync::~PhysicsSync()
{
    if (m_world.valid())
        m_backend.destroyWorld(m_world);
}

core::InstanceId PhysicsSync::instanceOf(u64 userData) const noexcept
{
    const core::InstanceId id = unpackInstance(userData);
    // **Destroyed is gone, from the moment of `Destroy`** (D458): the instance
    // is still in the pools until the tick retires it and its body is still
    // in the simulation until the next step, and neither is something a query
    // may answer with -- a ray handed back the part that had just been
    // destroyed.
    return m_scene.alive(id) && !m_scene.destroyed(id) ? id : core::InstanceId{};
}

u64 PhysicsSync::userDataOf(core::InstanceId id) const noexcept
{
    return packInstance(id);
}

bool PhysicsSync::inWorld(core::InstanceId id) const
{
    // Under `Workspace`, which is what "in the world" means (api-design.md
    // §2.1). A part parented to a Folder under Workspace is in; one parented to
    // nil or to a service that is not Workspace is not.
    if (!m_workspace.valid())
        return false;

    // Answered for the PARENT and remembered for one tick, because the walk is
    // O(depth) and ten thousand parts in one folder ask the identical question
    // ten thousand times. A single-entry memo rather than a map: the parts of a
    // scene arrive in pool order, which is creation order, which groups them by
    // parent for free.
    // **And for as long as the world is unchanged** (audit E15): a script
    // between two passes of the same tick could move the parent out of the
    // world, and the memo answered for it until the next tick began.
    const core::InstanceId parent = m_scene.parentOf(id);
    if (parent == m_lastParent && m_scene.mutations() == m_lastParentMutations)
        return m_lastParentInWorld;

    m_lastParent = parent;
    m_lastParentMutations = m_scene.mutations();
    m_lastParentInWorld = parent == m_workspace || m_scene.isAncestorOf(m_workspace, parent);
    return m_lastParentInWorld;
}

// How many ticks an anchored part stays `Kinematic` after the last write to its
// `CFrame` (D031).
//
// **Hysteresis, and the number is chosen rather than found.** Without it a part
// written every other tick would flip between the two broadphase layers every
// other tick, and each flip is a body rebuild. Twelve ticks covers any driver
// writing at five hertz or faster -- a tween writes every tick, a script nudging
// a platform on a timer is the slow end -- and costs at most a fifth of a second
// of a stopped body sitting in the moving layer, where the only price is that it
// is re-fitted for those twelve ticks.
//
// TICKS and not seconds. A body's broadphase layer must not depend on how fast
// the machine was running (R10).
constexpr u64 kAnchoredMovingTicks = 12;

// `Enum.CollisionFidelity`'s items, by name rather than by the numbers they
// happen to have. The component stores the raw value -- for the reason every
// enum-valued component does, that a generated accessor needs no per-enum C++
// type -- and reading it back against a literal is how D146 happened.
enum class CollisionFidelity : i32
{
    Default = 0,
    Hull = 1,
    Box = 2,
    Precise = 3,
};

physics::ShapeType shapeForPartShape(i32 shape) noexcept
{
    // `Enum.PartShape`: Block, Ball, Cylinder, Capsule, Wedge. A wedge collides
    // as its bounding box in this release -- the renderer has no wedge either,
    // so nothing yet disagrees about what one looks like.
    switch (shape) {
    case 1:
        return physics::ShapeType::Sphere;
    case 2:
        return physics::ShapeType::Cylinder;
    case 3:
        return physics::ShapeType::Capsule;
    default:
        break;
    }
    return physics::ShapeType::Box;
}

f32 partMass(const World& world, core::InstanceId id) noexcept
{
    const RigidBodyComponent* body = world.rigidBodies().find(id);
    const PartComponent* part = world.parts().find(id);
    if (body == nullptr || part == nullptr)
        return 0.0f;
    if (world.characterBodies().find(id) != nullptr)
        return body->mass > 0.0f ? body->mass : physics::CharacterDesc{}.mass;
    if (const MeshPartComponent* mesh = world.meshParts().find(id);
        mesh != nullptr && mesh->collisionFidelity != static_cast<i32>(CollisionFidelity::Box)) {
        // A hull's volume is the solver's to say; until it has, the box.
        return body->mass > 0.0f ? body->mass : physics::solidMass(physics::ShapeType::Box, part->size, body->density);
    }
    const physics::ShapeType shape =
        world.meshParts().find(id) != nullptr ? physics::ShapeType::Box : shapeForPartShape(part->shape);
    return physics::solidMass(shape, part->size, body->density);
}

std::string_view terrainLayerAt(const TerrainComponent& terrain, const asset::TerrainHit& hit) noexcept
{
    const core::Vec3 ground{static_cast<f32>(hit.position.x), static_cast<f32>(hit.position.y),
                            static_cast<f32>(hit.position.z)};
    const u8 layer = asset::drawnMaterial(terrain.rules, asset::Voxel{255, hit.material, hit.top, hit.cover},
                                          hit.normal, ground, static_cast<f32>(hit.position.y + terrain.origin.y));
    if (layer == 0 || layer > terrain.layers.size())
        return {};
    return terrain.layers[layer - 1];
}

MaterialRef floorMaterial(const World& world, core::InstanceId character)
{
    const CharacterBodyComponent* body = world.characterBodies().find(character);
    const PartComponent* self = world.parts().find(character);
    if (body == nullptr || self == nullptr || !body->grounded)
        return {};
    if (const PartComponent* ground = world.parts().find(body->groundPart); ground != nullptr) {
        if (!ground->material.valid())
            return {};
        return MaterialRef{std::string(world.atoms().text(ground->material)), ground->materialClone};
    }
    // **Standing on no part is standing on ground**: a terrain's colliders
    // name no instance, so the ground is asked directly -- straight down from
    // its middle, a little past its feet, in whichever terrain is nearest
    // under it. What it is drawn as there is what it is.
    const f64 reach = static_cast<f64>(self->size.y) * 0.5 + 1.0;
    MaterialRef found;
    f64 nearest = reach;
    world.terrains().forEach([&](core::InstanceId, const TerrainComponent& terrain) {
        if (terrain.field.empty())
            return;
        const core::DVec3 from{self->cframe.position.x - terrain.origin.x, self->cframe.position.y - terrain.origin.y,
                               self->cframe.position.z - terrain.origin.z};
        const std::optional<asset::TerrainHit> hit =
            asset::raycastField(terrain.field, from, core::Vec3{0.0f, -static_cast<f32>(reach), 0.0f}, reach);
        if (!hit.has_value() || hit->distance > nearest)
            return;
        nearest = hit->distance;
        const std::string_view layer = terrainLayerAt(terrain, *hit);
        found = layer.empty() ? MaterialRef{} : MaterialRef{std::string(layer), 0};
    });
    return found;
}

physics::ShapeDesc PhysicsSync::shapeOf(core::InstanceId id, const PartComponent& part) const
{
    physics::ShapeDesc shape;
    shape.size = part.size;

    // A `MeshPart` collides as the box or hull its `CollisionFidelity` asks for.
    //
    // `Box` is the fidelity that says "the bounding box" and it is honoured
    // exactly. Every other value asks for the geometry, and the geometry that
    // exists is a CONVEX HULL: a hull is what a rigid-body solver can use
    // directly, and a concave triangle mesh is a different shape class with
    // different rules (it cannot be dynamic) that nobody has asked for.
    //
    // A mesh whose points have not arrived falls back to the box rather than to
    // nothing. That is the state of a `MeshPart` in the frame before its file
    // finishes loading, and a body with no shape for one frame is a body that
    // falls through the floor.
    if (const MeshPartComponent* mesh = m_scene.meshParts().find(id); mesh != nullptr) {
        shape.type = physics::ShapeType::Box;
        // **`Box` is item 2, and this asked for item 0** (D146). Zero is
        // `Default` -- the item whose whole meaning is "the engine chooses", and
        // whose documented choice is a hull -- so every `MeshPart` nobody
        // touched collided as its bounding box while the enum promised the
        // geometry, and the one person who explicitly asked for a box got a
        // hull. Both wrong, in opposite directions, from one constant; and the
        // comment above has described the intended behaviour correctly the whole
        // time.
        if (mesh->collisionFidelity == static_cast<i32>(CollisionFidelity::Box)) {
            return shape;
        }
        const core::NameAtom content = mesh->meshContent;
        const auto at = std::lower_bound(m_collisionPoints.begin(), m_collisionPoints.end(), content,
                                         [](const auto& entry, core::NameAtom key) { return entry.first.id < key.id; });
        if (at != m_collisionPoints.end() && at->first == content && at->second.points.size() >= 4) {
            shape.type = physics::ShapeType::ConvexHull;
            shape.points = at->second.points;
            shape.pointsRevision = at->second.revision;
            // The same `Size / MeshSize` the renderer draws with, so the hull is
            // the shape on screen and not the shape in the file. The box branch
            // above needs no equivalent: it is already `part.size`.
            shape.pointScale = core::Vec3{part.size.x / mesh->meshSize.x, part.size.y / mesh->meshSize.y,
                                          part.size.z / mesh->meshSize.z};
        }
        return shape;
    }
    shape.type = shapeForPartShape(part.shape);
    return shape;
}

physics::BodyDesc PhysicsSync::descOf(core::InstanceId id, const PartComponent& part, const RigidBodyComponent& body,
                                      bool movingAnchored) const
{
    physics::BodyDesc desc;
    desc.shape = shapeOf(id, part);
    desc.transform = part.cframe;
    // A part an active weld drives is Kinematic: it still collides and still
    // pushes what it runs into, and the solver does not move it -- which is what
    // "driven, not simulated" means (roadmap M5).
    //
    // **And so is an anchored part something is currently WRITING** (D031). A
    // moving platform is `Anchored` with a tween on its `CFrame`, and it was
    // classified `Static` -- which is the layer Jolt keeps precisely so that it
    // is never re-fitted. A static body does not move and derives no velocity,
    // so D027's `MoveKinematic` was handed a target and nothing happened: the
    // fix was right and unreachable.
    //
    // Only what is written, and only while it is written. The alternative --
    // `Anchored` meaning kinematic always -- was rejected by the human on cost
    // rather than taste: `jolt_physics.cpp` splits the broadphase into
    // `NonMoving` and `Moving`, and making every floor and wall kinematic would
    // put a world of never-moving bodies into the layer that is updated every
    // tick, to solve a problem two platforms have.
    // **On a replica every loose body is driven** (ADR 0069, decision 5): the
    // authority simulates it and the snapshots say where it is, so the local
    // solver must neither fight the incoming transforms nor overwrite them.
    // Kinematic does both -- it follows a written target, and `writeBack` skips
    // it -- with no branch in game script and no second authority over where a
    // thing is.
    //
    // **Except what this replica owns** (ADR 0099): it simulates that itself,
    // and the authority is the one that follows. A replica holds an owner only
    // for its own parts, so any owner here is this machine. **And except what
    // the authority never sent** (NA17): a part this machine's own scripts
    // made hung in the air, driven by snapshots that would never mention it.
    const NetworkTopology topology = m_scene.engineState().networkTopology;
    const bool replicated = topology == NetworkTopology::Replica && body.fromAuthority && !body.anchored &&
                            body.networkOwner == 0 && !body.predicted;
    const bool ownedElsewhere = topology != NetworkTopology::Replica && body.networkOwner != 0;
    const bool driven = isDriven(id) || replicated || ownedElsewhere || (body.anchored && movingAnchored);
    desc.motion = driven          ? physics::MotionType::Kinematic
                  : body.anchored ? physics::MotionType::Static
                                  : physics::MotionType::Dynamic;
    desc.friction = body.friction;
    desc.restitution = body.restitution;
    desc.density = body.density;
    desc.linearDamping = body.linearDamping;
    desc.angularDamping = body.angularDamping;
    desc.collidable = body.canCollide;
    desc.queryable = body.canQuery;
    // What this replica only follows (the multiplayer smoothness brief): its
    // own character pushes through it as the authority's does, instead of
    // stopping against where it was four ticks ago.
    desc.passableForCharacters = replicated;
    const u16 group = m_scene.collisionGroups().find(body.collisionGroup);
    desc.group = group == CollisionGroups::kInvalid ? CollisionGroups::kDefault : group;
    desc.userData = packInstance(id);
    return desc;
}

void PhysicsSync::syncCollisionGroups()
{
    const CollisionGroups& groups = m_scene.collisionGroups();
    if (groups.revision() == m_groupRevision)
        return;
    m_groupRevision = groups.revision();

    // Pushed wholesale rather than diffed. The table changes when a script
    // registers a group or flips a pair -- twice in a game's life -- and a
    // hundred-entry rewrite then is cheaper to be right about than a diff that
    // runs every tick.
    for (u32 index = 0; index < groups.count(); ++index) {
        const std::string_view name = m_scene.atoms().text(groups.nameAt(static_cast<u16>(index)));
        [[maybe_unused]] const physics::CollisionGroup registered = m_backend.registerCollisionGroup(m_world, name);
    }
    for (u32 a = 0; a < groups.count(); ++a) {
        for (u32 b = a; b < groups.count(); ++b) {
            m_backend.setGroupsCollidable(m_world, static_cast<physics::CollisionGroup>(a),
                                          static_cast<physics::CollisionGroup>(b),
                                          groups.collidable(static_cast<u16>(a), static_cast<u16>(b)));
        }
    }
}

void PhysicsSync::applyBody(core::InstanceId id, PartComponent& part, RigidBodyComponent& body)
{
    if (id.index >= m_bodies.size())
        m_bodies.resize(static_cast<usize>(id.index) + 1);

    BodyRecord& record = m_bodies[id.index];

    // A script's write, told apart from the mirror's own the same way the
    // transform sync below does it: the component differs from what this mirror
    // last put there. An anchored part that is being written is a platform in
    // motion, and it holds that state for `kAnchoredMovingTicks` afterwards
    // (D031).
    const u64 tick = m_scene.engineState().tick;
    const bool written = record.generation == id.generation && !(part.cframe == record.written);
    if (written)
        record.movingUntilTick = tick + kAnchoredMovingTicks;

    physics::BodyDesc desc = descOf(id, part, body, record.movingUntilTick > tick);

    // **A part collides with what it wears** (ADR 0117): its material's
    // friction and bounce, where the part's own are the defaults -- a part
    // that says a number of its own keeps it. A part wearing nothing is not
    // asked at all.
    if (part.material.valid()) {
        const RigidBodyComponent fresh;
        if (body.friction == fresh.friction || body.restitution == fresh.restitution) {
            if (part.materialClone != 0) {
                // A runtime copy is a script's to change at any moment.
                const asset::ResolvedMaterial worn = m_scene.resolveMaterial(part.material, part.materialClone);
                record.wornFriction = worn.properties.friction;
                record.wornRestitution = worn.properties.restitution;
                record.wornMaterial = core::NameAtom{};
            }
            else {
                const asset::MaterialLibrary* library = m_scene.materialLibrary();
                const u64 revision = library != nullptr ? library->revision() : 0;
                if (!(record.wornMaterial == part.material) || record.wornLibrary != revision) {
                    const asset::MaterialProperties& worn = m_scene.resolveMaterialAsset(part.material).properties;
                    record.wornFriction = worn.friction;
                    record.wornRestitution = worn.restitution;
                    record.wornMaterial = part.material;
                    record.wornLibrary = revision;
                }
            }
            if (body.friction == fresh.friction)
                desc.friction = record.wornFriction;
            if (body.restitution == fresh.restitution)
                desc.restitution = record.wornRestitution;
        }
    }

    // Everything the record keeps about a description, minus the span it may not
    // keep. One place, because a field written in one branch and forgotten in
    // the other is how the two paths below used to disagree.
    // What it weighs, asked once when the body is made or reshaped: a mass is
    // the shape's volume times the density, and only the solver has the volume.
    const auto weigh = [this, &record, &body] {
        body.mass = m_backend.bodyMassProperties(m_world, record.handle).mass;
    };
    const auto remember = [&record, &desc, &part] {
        record.shape = withoutPoints(desc.shape);
        record.motion = desc.motion;
        record.density = desc.density;
        record.linearDamping = desc.linearDamping;
        record.angularDamping = desc.angularDamping;
        record.friction = desc.friction;
        record.restitution = desc.restitution;
        record.collidable = desc.collidable;
        record.queryable = desc.queryable;
        record.group = desc.group;
        record.written = part.cframe;
    };

    if (record.generation != id.generation) {
        // The slot is empty, or it holds a body that belonged to a different
        // instance in the same slot. Either way this instance has no body yet.
        if (record.generation != 0 && record.live) {
            m_backend.destroyBody(m_world, record.handle);
            --m_bodyCount;
        }

        record = BodyRecord{};
        record.generation = id.generation;
        record.seen = true;
        remember();

        // **Remembered whether or not it worked**, which is the fix: a refusal
        // used to zero the record, so the next tick saw an empty slot and asked
        // again -- for ever, once per tick, burning a body generation each time
        // and saying nothing. Now the attempt is recorded, and only a
        // description that actually DIFFERS is worth another one.
        const physics::BodyHandle handle = m_backend.createBody(m_world, desc);
        if (!handle.valid())
            return;

        record.handle = handle;
        record.live = true;
        record.backendMotion = desc.motion;
        ++m_bodyCount;
        weigh();
        return;
    }

    record.seen = true;

    // A shape, motion type or density change is a rebuild on the backend's
    // side, so it is one call rather than four setters -- and the handle
    // survives it, because everything above holds one.
    const bool describedDifferently =
        !sameShape(record.shape, desc.shape) || record.motion != desc.motion || record.density != desc.density;

    if (!record.live) {
        // There is no body to set anything on. Asking again only when the
        // description has changed is what keeps a refusal costing one attempt:
        // a hull whose points have since arrived, or a size that moved, is a
        // different question and gets asked.
        if (!describedDifferently)
            return;
        remember();
        const physics::BodyHandle handle = m_backend.createBody(m_world, desc);
        if (!handle.valid())
            return;
        record.handle = handle;
        record.live = true;
        record.backendMotion = desc.motion;
        ++m_bodyCount;
        weigh();
        return;
    }

    if (describedDifferently) {
        const bool applied = m_backend.updateBody(m_world, record.handle, desc);
        if (applied) {
            remember();
            record.backendMotion = desc.motion;
            weigh();
            // **A body that becomes simulated starts moving as it was**: a
            // rebuilt body is at rest, and a part a replica starts predicting
            // mid-flight would otherwise drop out of the air (ADR 0133).
            if (desc.motion == physics::MotionType::Dynamic && !(body.linearVelocity == core::Vec3{0.0f, 0.0f, 0.0f} &&
                                                                 body.angularVelocity == core::Vec3{0.0f, 0.0f, 0.0f}))
                m_backend.setBodyVelocity(m_world, record.handle, body.linearVelocity, body.angularVelocity);
        }
        else {
            // **A refusal leaves the body as it was, so only the retry gate is
            // recorded** (D135). Committing the whole description would be the
            // mirror claiming the backend took a motion type and a friction it
            // refused -- and because `describedDifferently` then goes false for
            // ever, an `Anchored` part would keep falling with its friction
            // change permanently lost.
            //
            // These three ARE recorded, and that is what keeps "a refused
            // rebuild costs one attempt" true: they are the comparison the
            // retry is gated on. Everything else stays as the backend has it,
            // so the incremental branch below pushes it through
            // `setBodyMaterial`, `setBodyFlags` and `setBodyGroup` next tick --
            // and `record.written` stays as it was, so a `CFrame` written in the
            // same tick as a refused rebuild is delivered next tick rather than
            // recorded as delivered and swallowed.
            record.shape = withoutPoints(desc.shape);
            record.motion = desc.motion;
            record.density = desc.density;
        }
    }
    else {
        if (record.friction != desc.friction || record.restitution != desc.restitution) {
            m_backend.setBodyMaterial(m_world, record.handle, desc.friction, desc.restitution);
            record.friction = desc.friction;
            record.restitution = desc.restitution;
        }
        if (record.collidable != desc.collidable || record.queryable != desc.queryable) {
            m_backend.setBodyFlags(m_world, record.handle, desc.collidable, desc.queryable);
            record.collidable = desc.collidable;
            record.queryable = desc.queryable;
        }
        if (record.group != desc.group) {
            m_backend.setBodyGroup(m_world, record.handle, desc.group);
            record.group = desc.group;
        }
        if (record.linearDamping != desc.linearDamping || record.angularDamping != desc.angularDamping) {
            m_backend.setBodyDamping(m_world, record.handle, desc.linearDamping, desc.angularDamping);
            record.linearDamping = desc.linearDamping;
            record.angularDamping = desc.angularDamping;
        }

        // The one place a script's write is told apart from the mirror's own:
        // the component differs from what this mirror last wrote into it, so
        // something else did. No dirty flag on either side.
        if (!(part.cframe == record.written)) {
            m_backend.setBodyTransform(m_world, record.handle, part.cframe);
            record.written = part.cframe;
        }
        else if (record.backendMotion == physics::MotionType::Kinematic) {
            // **A kinematic body that was not written this tick is told to stay
            // where it is**, and that is not a no-op: `MoveKinematic` sets a
            // velocity, and a body nobody re-targets keeps the last one and
            // COASTS. A platform whose tween finished sailed on past its
            // destination, and a welded part whose anchor stopped moving flew
            // away -- both found by two conformance cases the moment D027's fix
            // landed.
            //
            // Handing over the same target computes a velocity of zero, which is
            // the honest way to say "it is not moving" to a system whose whole
            // vocabulary is velocity.
            m_backend.setBodyTransform(m_world, record.handle, part.cframe);
        }
    }

    // **A velocity a script wrote is where this tick starts from** (ADR 0127,
    // N5), before the pushes queued beside it.
    if (body.velocityWritten) {
        m_backend.setBodyVelocity(m_world, record.handle, body.linearVelocity, body.angularVelocity);
        body.velocityWritten = false;
    }
    if (!(body.pendingImpulse == core::Vec3{0.0f, 0.0f, 0.0f})) {
        m_backend.applyImpulse(m_world, record.handle, body.pendingImpulse);
        body.pendingImpulse = core::Vec3{0.0f, 0.0f, 0.0f};
    }
    if (!(body.pendingAngularImpulse == core::Vec3{0.0f, 0.0f, 0.0f})) {
        m_backend.applyAngularImpulse(m_world, record.handle, body.pendingAngularImpulse);
        body.pendingAngularImpulse = core::Vec3{0.0f, 0.0f, 0.0f};
    }
}

void PhysicsSync::applyCharacter(core::InstanceId id, PartComponent& part, RigidBodyComponent& body,
                                 CharacterBodyComponent& character, f32 fixedDt)
{
    const u64 key = packInstance(id);
    const u16 sceneGroup = m_scene.collisionGroups().find(body.collisionGroup);
    const physics::CollisionGroup group = sceneGroup == CollisionGroups::kInvalid
                                              ? CollisionGroups::kDefault
                                              : static_cast<physics::CollisionGroup>(sceneGroup);

    auto found = m_characters.find(key);
    if (found == m_characters.end() || found->second.height != part.size.y ||
        found->second.diameter != std::max(part.size.x, part.size.z) ||
        found->second.maxSlopeAngle != character.maxSlopeAngle ||
        found->second.stepHeight != character.autoStepHeight || found->second.group != group) {
        // Rebuilt rather than adjusted: a controller's capsule, slope limit and
        // step height are settings it is constructed with, and a character that
        // changes size mid-stride is a rare enough event to pay for.
        if (found != m_characters.end()) {
            m_backend.destroyCharacter(m_world, found->second.handle);
            m_characters.erase(found);
        }

        physics::CharacterDesc desc;
        desc.transform = part.cframe;
        desc.height = part.size.y;
        desc.diameter = std::max(part.size.x, part.size.z);
        desc.maxSlopeAngle = character.maxSlopeAngle;
        desc.stepHeight = character.autoStepHeight;
        desc.group = group;
        desc.userData = key;

        // What a controller weighs is the controller's: an impulse is this
        // times a change of speed, and `Mass` says it.
        body.mass = desc.mass;

        CharacterRecord record;
        record.handle = m_backend.createCharacter(m_world, desc);
        record.height = desc.height;
        record.diameter = desc.diameter;
        record.maxSlopeAngle = desc.maxSlopeAngle;
        record.stepHeight = desc.stepHeight;
        record.group = group;
        record.written = part.cframe;
        record.seen = true;
        if (!record.handle.valid())
            return;
        found = m_characters.emplace(key, record).first;
    }

    CharacterRecord& record = found->second;
    record.seen = true;

    if (!(part.cframe == record.written)) {
        m_backend.setCharacterTransform(m_world, record.handle, part.cframe);
        record.written = part.cframe;
    }

    // **On a replica, only its own player's character is simulated** (ADR
    // 0076). Every other one is where the authority's snapshots put it: moving
    // it here as well was two answers to one question, and the snapshot and
    // the local gravity fought over it every tick. The own character is
    // simulated -- that is the prediction -- and the snapshots correct it.
    record.follower = false;
    if (m_scene.engineState().networkTopology == NetworkTopology::Replica) {
        const core::InstanceId local = localPlayerOf(m_scene);
        const PlayerComponent* player = local.valid() ? m_scene.players().find(local) : nullptr;
        if (player == nullptr || !(player->character == id)) {
            record.follower = true;
            character.moveDirection = core::Vec3{};
            character.jumpRequested = false;
            return;
        }
    }

    const physics::CharacterState state = m_backend.characterState(m_world, record.handle);
    const bool grounded = state.ground == physics::CharacterGround::Grounded;

    // **`ApplyImpulse` does what it says** (D466): a change of speed of the
    // impulse over the controller's mass. Up and down it joins what gravity
    // integrates; across, it is a push that fades. In a fluid or in flight all
    // three axes are the push's, because nothing there is falling.
    if (!(body.pendingImpulse == core::Vec3{0.0f, 0.0f, 0.0f})) {
        const core::Vec3 change = body.pendingImpulse * (1.0f / std::max(body.mass, 1.0f));
        if (character.mode >= 2) {
            character.push = character.push + change;
        }
        else {
            character.push.x += change.x;
            character.push.z += change.z;
            character.verticalVelocity += change.y;
        }
        body.pendingImpulse = core::Vec3{0.0f, 0.0f, 0.0f};
    }
    // A controller does not spin: a twist has nothing to turn.
    body.pendingAngularImpulse = core::Vec3{0.0f, 0.0f, 0.0f};
    // **A velocity written is the character's own momentum from here**: what
    // it is thrown at. Its walk is still added on top by `Move`.
    if (body.velocityWritten) {
        const core::Vec3 written = body.linearVelocity;
        character.push = core::Vec3{written.x, character.mode >= 2 ? written.y : 0.0f, written.z};
        if (character.mode < 2)
            character.verticalVelocity = written.y;
        body.velocityWritten = false;
    }

    CharacterCommand command;
    command.moveDirection = character.moveDirection;
    command.jump = character.jumpRequested;
    command.walkSpeed = character.walkSpeed;
    command.jumpSpeed = character.jumpSpeed;
    command.dt = fixedDt;
    command.gravityScale = character.gravityScale;
    command.swimSpeed = character.swimSpeed;
    command.flySpeed = character.flySpeed;
    command.flying = character.flying;
    record.last = command;
    const CharacterMotion motion =
        stepController(record, command, CharacterMotion{character.verticalVelocity, character.push, character.mode},
                       grounded, state.groundNormal, state.transform.position);
    character.verticalVelocity = motion.vertical;
    character.push = motion.push;
    character.mode = motion.mode;
    character.jumpRequested = false;

    // Cleared once consumed: a character told nothing stops, which is what
    // `Move`'s Doc promises.
    character.moveDirection = core::Vec3{0.0f, 0.0f, 0.0f};
}

bool PhysicsSync::inFluid(core::DVec3 at) const
{
    bool wet = false;
    m_scene.waters().forEach([&](core::InstanceId id, const WaterComponent&) {
        if (wet || !inWorld(id))
            return;
        const WaterHere here = waterHere(m_scene, id, at.x, at.z);
        wet = here.covered && at.y < here.level;
    });
    // The block world is the service's, not something under the workspace: the
    // one the mirror builds colliders for.
    m_scene.voxels().forEach([&](core::InstanceId, const VoxelComponent& voxels) {
        if (wet || !(voxels.blockSize > 0.0f) || voxels.grid.chunkCount() == 0)
            return;
        const auto block = [&](f64 along) {
            return static_cast<core::i32>(std::floor(along / static_cast<f64>(voxels.blockSize)));
        };
        const asset::BlockId found = voxels.grid.get(block(at.x), block(at.y), block(at.z));
        wet = found != asset::AirBlock && isFluidType(voxels, asset::blockTypeOf(found));
    });
    return wet;
}

PhysicsSync::CharacterMotion PhysicsSync::stepController(const CharacterRecord& record, const CharacterCommand& command,
                                                         CharacterMotion motion, bool grounded, core::Vec3 groundNormal,
                                                         core::DVec3 position)
{
    // The movement model is the caller's and the sweeping is the backend's.
    // Gravity integrates here rather than in the solver because a character
    // controller is not a body the solver knows about.
    const core::Vec3 gravity = m_scene.workspaces().find(m_workspace) != nullptr
                                   ? m_scene.workspaces().find(m_workspace)->gravity
                                   : core::Vec3{0.0f, -9.81f, 0.0f};

    // **How it moves is where it is** (D466): flying when the game says so,
    // swimming by itself with its middle under a `Water`'s surface or inside a
    // fluid block, on foot or falling otherwise. Asked of the world at the
    // start of the step, so a replay asks the same world the same question.
    const bool swimming = !command.flying && inFluid(position);
    motion.mode = command.flying ? 3 : swimming ? 2 : 0;

    // A push fades: on the ground as friction would take it, in a fluid as
    // drag would, and in the air hardly at all.
    const f32 fade = motion.mode == 2 ? 3.0f : motion.mode == 3 ? 2.0f : grounded ? 10.0f : 0.5f;
    const core::Vec3 push = motion.push;
    motion.push = motion.push * std::max(0.0f, 1.0f - fade * command.dt);

    if (motion.mode != 0) {
        // **No weight, and `Move` in three dimensions**: up is up. The same
        // direction-and-throttle rule as the walk -- never more than all of it.
        core::Vec3 move = command.moveDirection;
        if (const f32 length = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z); length > 1.0f)
            move = move * (1.0f / length);
        // A jump in a fluid is a kick upwards, and it carries like any other
        // push: what takes a swimmer up through the surface and onto a bank.
        // In flight there is nothing to jump from.
        core::Vec3 carried = push;
        if (command.jump && motion.mode == 2 && carried.y < command.jumpSpeed) {
            carried.y = command.jumpSpeed;
            motion.push.y = command.jumpSpeed * std::max(0.0f, 1.0f - fade * command.dt);
        }
        core::Vec3 velocity = move * (motion.mode == 3 ? command.flySpeed : command.swimSpeed) + carried;
        // Left as its vertical speed, so the step it leaves the fluid in
        // carries on upwards and then falls.
        motion.vertical = velocity.y;
        m_backend.moveCharacter(m_world, record.handle, velocity, command.dt);
        return motion;
    }
    // On foot the push is across the ground; what there was of it up or down
    // is the fall's.
    motion.push.y = 0.0f;

    if (grounded && motion.vertical <= 0.0f)
        motion.vertical = 0.0f;
    motion.vertical += gravity.y * command.gravityScale * command.dt;

    if (command.jump) {
        // Applied WHEREVER the character is, grounded or not (human decision,
        // 2026-08-21). The recorded reasoning had argued *ignored* against
        // *queued* -- "a jump that fires the moment you land is a jump you did
        // not ask for" -- and that argument is about queuing, which nothing
        // here proposes. Letting the caller decide was never among the
        // alternatives it ruled out.
        //
        // `Grounded` is already exposed, so `if character.Grounded then
        // character:Jump() end` reproduces the old behaviour in one line, in
        // the game, where a jump policy belongs. What it unlocks costs nothing
        // more: double jump, wall jump, coyote time and jump buffering all
        // become counters in Luau.
        //
        // What stays engine-side is the TICK. The velocity is set here, at the
        // next simulation step, and never inside the call -- or a replay
        // diverges (R10).
        motion.vertical = command.jumpSpeed;
    }

    // Horizontal only -- vertical movement is gravity's and Jump's -- and
    // scaled rather than normalised, so a shorter direction walks slower.
    //
    // **A direction and a throttle, never more than all of it** (D440): the
    // vector's length is clamped to one. `Move(1, 0, 1)` walked at 1.41 times
    // `WalkSpeed` and `Move(5, 0, 0)` at five -- the diagonal-is-faster bug
    // handed to every game, and a speed hack to any server that forwards a
    // client's vector. Shorter than one still walks slower, which is what an
    // analog stick wants.
    core::Vec3 move{command.moveDirection.x, 0.0f, command.moveDirection.z};
    if (const f32 length = std::sqrt(move.x * move.x + move.z * move.z); length > 1.0f)
        move = move * (1.0f / length);
    const core::Vec3 horizontal = move * command.walkSpeed + core::Vec3{push.x, 0.0f, push.z};
    core::Vec3 velocity{horizontal.x, motion.vertical, horizontal.z};

    // **On ground it can walk, `WalkSpeed` is the HORIZONTAL speed, up a slope
    // and down it alike** (D439). The walk was handed over flat and the sweep
    // laid it along the slope: uphill the horizontal speed came out as the
    // speed times the square of the slope's cosine -- 4.4 of 6 at thirty
    // degrees -- and downhill it was kept whole with the fall added, so a
    // player ran down hills and crawled up them. So the walk is given ALONG
    // the ground, the rise or the fall that keeps its horizontal part whole;
    // gravity has nothing to add to a foot that is on the ground, and a jump
    // or a fall is as it was.
    //
    // Not while it is on its way UP: an impulse that lifts a standing
    // character is a launch, and following the ground would cancel it.
    constexpr f32 Level = 1.0e-3f;
    if (grounded && !command.jump && motion.vertical <= 0.0f && groundNormal.y > Level)
        velocity.y = -(horizontal.x * groundNormal.x + horizontal.z * groundNormal.z) / groundNormal.y;
    m_backend.moveCharacter(m_world, record.handle, velocity, command.dt);
    return motion;
}

std::optional<CharacterCommand> PhysicsSync::lastCommand(core::InstanceId character) const
{
    const auto found = m_characters.find(packInstance(character));
    if (found == m_characters.end() || found->second.follower)
        return std::nullopt;
    return found->second.last;
}

void PhysicsSync::remember(u64 tick)
{
    if (!m_world.valid() || tick == 0)
        return;
    Island island;
    std::vector<physics::BodyHandle> bodies;
    for (usize index = 0; index < m_bodies.size(); ++index) {
        const BodyRecord& record = m_bodies[index];
        if (record.generation == 0 || !record.live || record.backendMotion != physics::MotionType::Dynamic)
            continue;
        const core::InstanceId id{static_cast<u32>(index), record.generation};
        const PartComponent* part = m_scene.parts().find(id);
        const RigidBodyComponent* body = m_scene.rigidBodies().find(id);
        if (part == nullptr || body == nullptr)
            continue;
        bodies.push_back(record.handle);
        island.entities.push_back(IslandEntity{.id = id,
                                               .cframe = part->cframe,
                                               .body = true,
                                               .linear = body->linearVelocity,
                                               .angular = body->angularVelocity});
    }
    // In id order, whatever order the map holds them in (R10).
    std::vector<u64> keys;
    for (const auto& [key, record] : m_characters) {
        if (!record.follower)
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    std::vector<physics::CharacterHandle> characters;
    for (const u64 key : keys) {
        const core::InstanceId id = unpackInstance(key);
        const PartComponent* part = m_scene.parts().find(id);
        const CharacterBodyComponent* body = m_scene.characterBodies().find(id);
        if (part == nullptr || body == nullptr)
            continue;
        characters.push_back(m_characters.at(key).handle);
        island.entities.push_back(IslandEntity{.id = id,
                                               .cframe = part->cframe,
                                               .character = true,
                                               .grounded = body->grounded,
                                               .state = body->state,
                                               .groundPart = body->groundPart,
                                               .move = body->moveDirection,
                                               .jump = body->jumpRequested,
                                               .vertical = body->verticalVelocity,
                                               .push = body->push,
                                               .mode = body->mode});
    }
    if (!m_backend.saveIsland(m_world, bodies, characters, island.solver))
        return;
    m_islands.insert_or_assign(tick, std::move(island));
    while (m_islands.size() > IslandMemory)
        m_islands.erase(m_islands.begin());
}

std::optional<core::CFrameD> PhysicsSync::remembered(u64 tick, core::InstanceId id) const
{
    const auto island = m_islands.find(tick);
    if (island == m_islands.end())
        return std::nullopt;
    for (const IslandEntity& entity : island->second.entities) {
        if (entity.id == id)
            return entity.cframe;
    }
    return std::nullopt;
}

bool PhysicsSync::restoreIsland(const Island& island)
{
    // Every one of them still here, as it was, or nothing is touched.
    for (const IslandEntity& entity : island.entities) {
        if (m_scene.parts().find(entity.id) == nullptr)
            return false;
    }
    if (!m_backend.restoreIsland(m_world, island.solver))
        return false;
    for (const IslandEntity& entity : island.entities) {
        PartComponent* part = m_scene.parts().find(entity.id);
        part->cframe = entity.cframe;
        if (entity.id.index < m_bodies.size() && m_bodies[entity.id.index].generation == entity.id.generation)
            m_bodies[entity.id.index].written = entity.cframe;
        if (const auto found = m_characters.find(packInstance(entity.id)); found != m_characters.end())
            found->second.written = entity.cframe;
        if (RigidBodyComponent* body = m_scene.rigidBodies().find(entity.id); body != nullptr && entity.body) {
            body->linearVelocity = entity.linear;
            body->angularVelocity = entity.angular;
        }
        if (CharacterBodyComponent* body = m_scene.characterBodies().find(entity.id);
            body != nullptr && entity.character) {
            body->grounded = entity.grounded;
            body->state = entity.state;
            body->groundPart = entity.groundPart;
            body->moveDirection = entity.move;
            body->jumpRequested = entity.jump;
            body->verticalVelocity = entity.vertical;
            body->push = entity.push;
            body->mode = entity.mode;
        }
    }
    return true;
}

std::vector<core::CFrameD> PhysicsSync::replay(core::InstanceId character, const CharacterReplayStart& start,
                                               std::span<const CharacterCommand> commands)
{
    std::vector<core::CFrameD> frames;
    const auto found = m_characters.find(packInstance(character));
    PartComponent* part = m_scene.parts().find(character);
    CharacterBodyComponent* body = m_scene.characterBodies().find(character);
    if (found == m_characters.end() || part == nullptr || body == nullptr)
        return frames;
    CharacterRecord& record = found->second;

    // **The live step, again** (ADR 0133): the island as it was after the
    // answered tick, the authority's word on the character put in, and every
    // unanswered command stepped through the same step the tick takes --
    // gravity, the walk, the jump, the sweep, the bodies it pushes. Its
    // shorter path below replayed the character alone, from less state, and
    // at a corner the two came apart.
    if (const auto island = m_islands.find(start.tick); start.tick != 0 && island != m_islands.end()) {
        const Island saved = island->second;
        // **What is waiting for the next step stays waiting**: a script's
        // `Move` and `Jump` this tick are the next step's, and the steps
        // taken again below consume and clear the same fields. Consumed here,
        // the next live step moved nothing -- a step behind at every
        // correction.
        const core::Vec3 pendingMove = body->moveDirection;
        const bool pendingJump = body->jumpRequested;
        const f32 pendingWalk = body->walkSpeed;
        const f32 pendingJumpSpeed = body->jumpSpeed;
        if (restoreIsland(saved)) {
            // The authority's word on the character, when it differs. Put
            // somewhere new, a controller finds its contacts again; nudged by
            // the little a prediction is off, it keeps the ones of its own
            // last step, which are the authority's too, near enough -- found
            // afresh at a crate's edge, they stepped a replay 8 cm from the
            // authority's in two ticks.
            const auto mine = std::find_if(saved.entities.begin(), saved.entities.end(),
                                           [&](const IslandEntity& entity) { return entity.id == character; });
            if (mine == saved.entities.end() || !mine->character) {
                m_backend.setCharacterTransform(m_world, record.handle, start.transform);
            }
            else if (!(mine->cframe == start.transform)) {
                const core::DVec3 off = start.transform.position - mine->cframe.position;
                if (off.x * off.x + off.y * off.y + off.z * off.z < NudgeMetres * NudgeMetres)
                    m_backend.nudgeCharacter(m_world, record.handle, start.transform);
                else
                    m_backend.setCharacterTransform(m_world, record.handle, start.transform);
            }
            part->cframe = start.transform;
            record.written = start.transform;
            body->verticalVelocity = start.verticalVelocity;
            body->push = start.push;
            body->grounded = start.grounded;
            // And what it pushes, where the authority had it (ADR 0133) --
            // but only what the authority disagrees about. A body put back
            // where it already is is not left as it was: it is woken, and its
            // rotation goes through a matrix and back. A crate asleep in the
            // step it answers, woken here, was stepped when it had not been.
            for (const CharacterReplayStart::Body& pushed : start.bodies) {
                const auto same = std::find_if(saved.entities.begin(), saved.entities.end(),
                                               [&](const IslandEntity& entity) { return entity.id == pushed.id; });
                if (same != saved.entities.end() && same->body && same->cframe == pushed.cframe &&
                    same->linear == pushed.linear && same->angular == pushed.angular)
                    continue;
                PartComponent* place = m_scene.parts().find(pushed.id);
                RigidBodyComponent* motion = m_scene.rigidBodies().find(pushed.id);
                if (place == nullptr || motion == nullptr || pushed.id.index >= m_bodies.size())
                    continue;
                BodyRecord& held = m_bodies[pushed.id.index];
                if (held.generation != pushed.id.generation || !held.live)
                    continue;
                place->cframe = pushed.cframe;
                held.written = pushed.cframe;
                motion->linearVelocity = pushed.linear;
                motion->angularVelocity = pushed.angular;
                m_backend.setBodyTransform(m_world, held.handle, pushed.cframe);
                m_backend.setBodyVelocity(m_world, held.handle, pushed.linear, pushed.angular);
            }
            frames.reserve(commands.size());
            for (usize at = 0; at < commands.size(); ++at) {
                const CharacterCommand& command = commands[at];
                body->moveDirection = command.moveDirection;
                body->jumpRequested = command.jump;
                body->walkSpeed = command.walkSpeed;
                body->jumpSpeed = command.jumpSpeed;
                stepQuietly(static_cast<f64>(command.dt));
                frames.push_back(part->cframe);
                remember(start.tick + 1 + at);
            }
            body->moveDirection = pendingMove;
            body->jumpRequested = pendingJump;
            body->walkSpeed = pendingWalk;
            body->jumpSpeed = pendingJumpSpeed;
            return frames;
        }
    }

    // **The first step's ground is the authority's**: a controller put
    // somewhere new still holds the contacts of where it was, and the
    // authority said what it was standing on. Every later step's is the one
    // the step before found, exactly as the simulation's own.
    m_backend.setCharacterTransform(m_world, record.handle, start.transform);
    CharacterMotion motion{start.verticalVelocity, start.push, body->mode};
    bool grounded = start.grounded;
    // What it stands on now, as the simulation's own first step asks it.
    core::Vec3 groundNormal = m_backend.characterState(m_world, record.handle).groundNormal;
    core::DVec3 position = start.transform.position;
    frames.reserve(commands.size());
    for (const CharacterCommand& command : commands) {
        motion = stepController(record, command, motion, grounded, groundNormal, position);
        const physics::CharacterState state = m_backend.characterState(m_world, record.handle);
        grounded = state.ground == physics::CharacterGround::Grounded;
        groundNormal = state.groundNormal;
        position = state.transform.position;
        frames.push_back(state.transform);
    }
    part->cframe = frames.empty() ? start.transform : frames.back();
    record.written = part->cframe;
    body->verticalVelocity = motion.vertical;
    body->push = motion.push;
    body->mode = motion.mode;
    return frames;
}

// --- Terrain colliders (ADR 0082) --------------------------------------------
//
// **One static `TriangleMesh` body per chunk, and only near things that move.**
// A chunk's collider is its surface meshed at full detail by the mesher the
// renderer uses, so what a body stands on is what is drawn, to within the
// renderer's level of detail. Meshing a chunk and handing Jolt a few thousand
// triangles is a millisecond or two, so a chunk nothing can reach has none --
// the bargain the reference platform strikes by building terrain collision
// lazily around bodies, and the one this engine's caves already struck.
//
// It replaced a `HeightField` per height tile plus a mesh per bricked column
// (ADR 0066, 0067), whose seam was one more place the two encodings met.
namespace {

// **What a mover can cross before its ground could be made again** (D417).
//
// Ground has a collider only near what moves, a few chunks a tick -- which is
// a bargain struck for things that walk. A log thrown at 100 m/s from thirty
// metres up crossed the reach in a quarter of a second, arrived before its
// ground had been made, and fell through the world: the report was "fast
// bodies pass through terrain", and half of it was that the terrain was not
// there yet. So the chunks inside this box are made in the tick that finds
// them missing, whatever the tick's count allows: its own size, a tenth of a
// second of its travel, and a metre.
struct Sweep
{
    core::InstanceId id;
    core::DVec3 low;
    core::DVec3 high;
    // A body the solver moves, and not a character: what new ground can bury.
    bool loose = false;
    // A character: buried by the same ground, and put on top of it the same.
    bool character = false;
};

inline constexpr f64 SweepSeconds = 0.1;
inline constexpr f64 SweepMargin = 1.0;

[[nodiscard]] Sweep sweepOf(core::InstanceId id, const PartComponent& part, const RigidBodyComponent& body, bool loose,
                            bool character = false) noexcept
{
    const f64 reach = static_cast<f64>(core::length(part.size)) * 0.5 +
                      static_cast<f64>(core::length(body.linearVelocity)) * SweepSeconds + SweepMargin;
    const core::DVec3 at = part.cframe.position;
    return Sweep{id, core::DVec3{at.x - reach, at.y - reach, at.z - reach},
                 core::DVec3{at.x + reach, at.y + reach, at.z + reach}, loose, character};
}

// How far up new ground is looked through for the air over a buried body, in
// metres. Past it the body is under a mountain somebody put there, and stays.
inline constexpr f64 UnburyReach = 64.0;

// Order-sensitive, which is what a key built from an ordered walk wants.
[[nodiscard]] u64 combine(u64 seed, u64 value) noexcept
{
    u64 z = seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

[[nodiscard]] u64 packKey(asset::ChunkKey key) noexcept
{
    return combine(combine(static_cast<u64>(static_cast<u32>(key.x)), static_cast<u64>(static_cast<u32>(key.y))),
                   static_cast<u64>(static_cast<u32>(key.z)));
}

// Whether a chunk can hold none of the surface its mesh would own: it and all
// twenty-six round it are one solid value, or it is air and so is everything
// its mesh reads above and beside it.
[[nodiscard]] bool buried(const asset::TerrainField& field, asset::ChunkKey key) noexcept
{
    const asset::TerrainChunk* own = field.findChunk(key);
    const bool solid = own != nullptr && own->uniform() && own->value().occupancy >= 128;
    if (own != nullptr && !solid)
        return false;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const asset::TerrainChunk* near = field.findChunk(asset::ChunkKey{key.x + dx, key.y + dy, key.z + dz});
                const bool nearSolid = near != nullptr && near->uniform() && near->value().occupancy >= 128;
                if (solid ? !nearSolid : near != nullptr)
                    return false;
            }
        }
    }
    return true;
}

// **Everything a chunk's mesh reads**: itself whole, and of each of the
// twenty-six round it only the layers against it -- the mesher reaches two
// voxels past a region's side. Keyed on whole neighbours, a dig in the middle of
// one chunk rebuilt the colliders of all twenty-six round it. A missing
// neighbour counts, because one appearing changes the mesh.
[[nodiscard]] u64 chunkContent(const asset::TerrainField& field, asset::ChunkKey key) noexcept
{
    u64 content = packKey(key);
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const asset::TerrainChunk* near = field.findChunk(asset::ChunkKey{key.x + dx, key.y + dy, key.z + dz});
                if (near == nullptr) {
                    content = combine(content, 0x6E6F6E65ull);
                    continue;
                }
                // What of the neighbour this chunk's collider reads: its four
                // layers that face back this way (a face, an edge or a corner)
                // -- the mesh's two and the band's cell (ADR 0143).
                content = combine(content, near->borderDigest(-dx, -dy, -dz, 4));
            }
        }
    }
    return content;
}

} // namespace

const PhysicsSync::TerrainSurfaces& PhysicsSync::terrainSurfacesOf(core::InstanceId id, const TerrainComponent& terrain)
{
    auto found = std::find_if(m_terrainSurfaces.begin(), m_terrainSurfaces.end(),
                              [&](const TerrainSurfaces& entry) { return entry.terrain == id; });
    if (found == m_terrainSurfaces.end()) {
        m_terrainSurfaces.push_back(TerrainSurfaces{});
        found = m_terrainSurfaces.end() - 1;
        found->terrain = id;
    }

    // The rules decide which layer the ground is drawn as, so they are part
    // of what a collider was built from. A handful of numbers, folded.
    u64 rules = terrain.rules.size();
    for (const asset::TerrainRule& rule : terrain.rules) {
        rules = combine(rules, (rule.enabled ? 1u : 0u) | (static_cast<u64>(rule.material) << 8));
        for (const f32 value : {rule.slopeMin, rule.slopeMax, rule.heightMin, rule.heightMax, rule.blend, rule.noise})
            rules = combine(rules, std::bit_cast<u32>(value));
        for (const u8 layer : rule.appliesTo)
            rules = combine(rules, layer);
    }

    const asset::MaterialLibrary* library = m_scene.materialLibrary();
    const u64 libraryRevision = library != nullptr ? library->revision() : 0;
    if (found->layersRevision == terrain.layersRevision && found->libraryRevision == libraryRevision &&
        found->rules == rules)
        return *found;
    found->layersRevision = terrain.layersRevision;
    found->libraryRevision = libraryRevision;
    found->rules = rules;

    // By layer id: voxel byte `n` is `layers[n - 1]`, and 0 is nothing.
    const physics::SurfaceMaterial standard;
    found->table.assign(terrain.layers.size() + 1, standard);
    bool any = false;
    u64 key = combine(rules, terrain.layers.size());
    for (usize index = 0; index < terrain.layers.size(); ++index) {
        const asset::MaterialProperties& layer =
            m_scene.resolveMaterialAsset(m_scene.atoms().intern(terrain.layers[index])).properties;
        found->table[index + 1] = physics::SurfaceMaterial{layer.friction, layer.restitution};
        any = any || layer.friction != standard.friction || layer.restitution != standard.restitution;
        key = combine(combine(key, std::bit_cast<u32>(layer.friction)), std::bit_cast<u32>(layer.restitution));
    }
    // Zero says "every layer is the default": nothing is handed to the solver
    // and nothing about a collider's key changes.
    found->key = any ? (key == 0 ? 1 : key) : 0;
    return *found;
}

void PhysicsSync::applyTerrain()
{
    for (auto& collider : m_terrainColliders)
        collider.seen = false;

    // **A count, never a clock.** How many colliders get rebuilt in a tick is
    // part of the operation sequence: a collider is part of the world, so it
    // cannot depend on how fast the machine was (R10).
    u32 rebuilt = 0;

    // Sorted by (terrain, key): the order the walk below meets them in, which
    // is the order bodies are created in, which decides the ids the backend
    // hands out.
    const auto locate = [this](core::InstanceId id, asset::ChunkKey key) {
        const auto at = std::lower_bound(m_terrainColliders.begin(), m_terrainColliders.end(), key,
                                         [&](const TerrainCollider& entry, const asset::ChunkKey& probe) {
                                             if (entry.terrain.index != id.index)
                                                 return entry.terrain.index < id.index;
                                             return entry.key < probe;
                                         });
        const bool exists = at != m_terrainColliders.end() && at->terrain == id && at->key == key;
        return std::pair{at, exists};
    };

    // Where collision is wanted: near bodies that are not anchored, and
    // characters. Gathered once per tick.
    std::vector<core::DVec3> movers;
    std::vector<Sweep> sweeps;
    // The terrains a collider of which was made again this tick.
    std::vector<core::InstanceId> remade;
    bool moversGathered = false;
    const auto gatherMovers = [&] {
        if (moversGathered)
            return;
        moversGathered = true;
        m_scene.rigidBodies().forEach([&](core::InstanceId id, const RigidBodyComponent& body) {
            const bool character = m_scene.characterBodies().find(id) != nullptr;
            if (body.anchored && !character)
                return;
            const PartComponent* part = m_scene.parts().find(id);
            if (part != nullptr && inWorld(id)) {
                movers.push_back(part->cframe.position);
                sweeps.push_back(sweepOf(id, *part, body, !body.anchored && !character, character));
            }
        });
    };

    m_scene.terrains().forEach([&](core::InstanceId id, TerrainComponent& terrain) {
        // **A terrain passed over this tick has no colliders after it**: the
        // retirement below takes every one not seen. So its last pass is not
        // settled any more, or the next one with the same boxes would mark
        // colliders seen that are gone -- and a character respawned where it
        // died stood on nothing (terrain audit P1).
        const auto unsettle = [&] {
            for (TerrainWant& entry : m_terrainWants) {
                if (entry.terrain == id)
                    entry.settled = false;
            }
        };
        if (!inWorld(id) || terrain.field.empty()) {
            unsettle();
            return;
        }
        gatherMovers();
        if (movers.empty()) {
            unsettle();
            return;
        }

        const asset::TerrainField& field = terrain.field;
        const f64 chunkMetres = static_cast<f64>(asset::ChunkEdge) * static_cast<f64>(field.settings().voxelSize);
        // What each layer is to touch (ADR 0117), and a key that is zero when
        // every one of them is the default surface.
        const TerrainSurfaces& surfaces = terrainSurfacesOf(id, terrain);
        const u64 placed = combine(combine(std::bit_cast<u64>(terrain.origin.x), std::bit_cast<u64>(terrain.origin.y)),
                                   std::bit_cast<u64>(terrain.origin.z));
        // Folded into both keys, so a layer's friction changing remakes the
        // colliders it is in; with nothing to say, both are what they were.
        const u64 placement = surfaces.key == 0 ? placed : combine(placed, surfaces.key);

        // The chunks within reach of any mover, sorted and unique: a box of
        // `TerrainCollisionReach` round each, in the field's own space. **The
        // boxes first, one per distinct box** (audit E9): five thousand bodies
        // on one field are a few boxes, and each box was expanded and sorted
        // once per body.
        std::vector<std::array<i32, 6>> boxes;
        boxes.reserve(movers.size());
        for (const core::DVec3& mover : movers) {
            const core::DVec3 local{mover.x - terrain.origin.x, mover.y - terrain.origin.y, mover.z - terrain.origin.z};
            const auto low = [&](f64 value) {
                return static_cast<i32>(std::floor((value - TerrainCollisionReach) / chunkMetres));
            };
            const auto high = [&](f64 value) {
                return static_cast<i32>(std::floor((value + TerrainCollisionReach) / chunkMetres));
            };
            boxes.push_back({low(local.x), low(local.y), low(local.z), high(local.x), high(local.y), high(local.z)});
        }
        std::sort(boxes.begin(), boxes.end());
        boxes.erase(std::unique(boxes.begin(), boxes.end()), boxes.end());

        // The same boxes over the same field in the same place, after a pass
        // that finished: every collider stands as it is.
        auto want = std::find_if(m_terrainWants.begin(), m_terrainWants.end(),
                                 [&](const TerrainWant& entry) { return entry.terrain == id; });
        if (want == m_terrainWants.end()) {
            m_terrainWants.push_back(TerrainWant{id, {}, 0, 0, false});
            want = m_terrainWants.end() - 1;
        }
        if (want->settled && want->revision == terrain.fieldRevision && want->placement == placement &&
            want->boxes == boxes) {
            for (TerrainCollider& collider : m_terrainColliders) {
                if (collider.terrain == id)
                    collider.seen = true;
            }
            return;
        }

        std::vector<asset::ChunkKey> wanted;
        for (const std::array<i32, 6>& box : boxes) {
            for (i32 z = box[2]; z <= box[5]; ++z) {
                for (i32 y = box[1]; y <= box[4]; ++y) {
                    for (i32 x = box[0]; x <= box[3]; ++x)
                        wanted.push_back(asset::ChunkKey{x, y, z});
                }
            }
        }
        // **And the chunks a mover is about to cross** (`Sweep`): wanted
        // whatever the reach says, and made this tick.
        std::vector<asset::ChunkKey> urgent;
        for (const Sweep& sweep : sweeps) {
            const auto index = [&](f64 value, f64 origin) {
                return static_cast<i32>(std::floor((value - origin) / chunkMetres));
            };
            for (i32 z = index(sweep.low.z, terrain.origin.z); z <= index(sweep.high.z, terrain.origin.z); ++z) {
                for (i32 y = index(sweep.low.y, terrain.origin.y); y <= index(sweep.high.y, terrain.origin.y); ++y) {
                    for (i32 x = index(sweep.low.x, terrain.origin.x); x <= index(sweep.high.x, terrain.origin.x); ++x)
                        urgent.push_back(asset::ChunkKey{x, y, z});
                }
            }
        }
        std::sort(urgent.begin(), urgent.end());
        urgent.erase(std::unique(urgent.begin(), urgent.end()), urgent.end());
        wanted.insert(wanted.end(), urgent.begin(), urgent.end());
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

        struct Pending
        {
            asset::ChunkKey key;
            u64 content = 0;
            f64 distance = 0.0;
            // The revision its collider was last current at; 0 for none.
            u64 current = 0;
            // In a mover's way: made this tick, count or no count.
            bool urgent = false;
        };
        std::vector<Pending> pending;
        for (const asset::ChunkKey key : wanted) {
            if (buried(field, key))
                continue;
            auto [at, exists] = locate(id, key);
            // Nothing written and nothing moved: the common case, answered
            // before any digest is read.
            if (exists && at->revision == terrain.fieldRevision && at->placement == placement) {
                at->seen = true;
                continue;
            }
            const u64 content =
                surfaces.key == 0 ? chunkContent(field, key) : combine(chunkContent(field, key), surfaces.key);
            if (exists) {
                // Kept, and seen, until its rebuild comes round: retiring a
                // stale collider would drop somebody standing on it.
                at->seen = true;
                if (at->content == content) {
                    // **Moved, not remade** (terrain audit T5): the same ground
                    // somewhere else is the same triangles, and a terrain moved
                    // by its `Position` remeshed every collider it had.
                    if (at->placement != placement) {
                        if (at->body.valid()) {
                            core::CFrameD moved;
                            moved.position = terrain.origin;
                            m_backend.setBodyTransform(m_world, at->body, moved);
                        }
                        at->placement = placement;
                    }
                    at->revision = terrain.fieldRevision;
                    continue;
                }
            }
            // Nearest to a mover first, ties by key: a function of the world.
            f64 nearest = std::numeric_limits<f64>::max();
            for (const core::DVec3& mover : movers) {
                const f64 cx = terrain.origin.x + (static_cast<f64>(key.x) + 0.5) * chunkMetres - mover.x;
                const f64 cy = terrain.origin.y + (static_cast<f64>(key.y) + 0.5) * chunkMetres - mover.y;
                const f64 cz = terrain.origin.z + (static_cast<f64>(key.z) + 0.5) * chunkMetres - mover.z;
                nearest = std::min(nearest, cx * cx + cy * cy + cz * cz);
            }
            pending.push_back(Pending{key, content, nearest, exists ? at->revision : 0,
                                      std::binary_search(urgent.begin(), urgent.end(), key)});
        }
        // **Longest waiting first, then nearest, ties by key** -- a function
        // of the world (R10). Nearest alone rebuilt the same four every tick
        // of a dig across more, and the rest stayed solid where they had been
        // dug for as long as the digging went on (terrain audit P5). A chunk
        // with no collider yet has waited longest of all.
        // **What is in a mover's way before all of it** (D417): those are not
        // a matter of whose turn it is.
        std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
            if (a.urgent != b.urgent)
                return a.urgent;
            if (a.current != b.current)
                return a.current < b.current;
            return a.distance != b.distance ? a.distance < b.distance : a.key < b.key;
        });
        const usize inTheWay = static_cast<usize>(
            std::count_if(pending.begin(), pending.end(), [](const Pending& entry) { return entry.urgent; }));

        // Settled when every rebuild this pass wanted fits in it; one left for
        // the next tick is a pass that must run again.
        want->boxes = std::move(boxes);
        want->revision = terrain.fieldRevision;
        want->placement = placement;
        // **This tick's rebuilds, meshed at once on the pool** (terrain audit
        // TA14's other half): each was meshed in turn on the main thread, a
        // dig's four a tick one after another. What is chosen, and the order
        // the bodies are made in below, are the same as before -- a function
        // of the world; only the meshing runs side by side, and a mesh is a
        // function of the field alone, so which worker made it is not
        // observable (R10). **Collider meshes** (`asset::meshCollider`):
        // without the sky term and the geomorph, which only drawing reads, so
        // they gather no surfaces; and with their bands (ADR 0143).
        // The tick's count of them, and every one in a mover's way beyond it:
        // still a function of the world and of nothing else (R10).
        const usize chosen =
            std::min<usize>(pending.size(), std::max<usize>(inTheWay, TerrainRebuildsPerTick -
                                                                          std::min(rebuilt, TerrainRebuildsPerTick)));
        want->settled = chosen == pending.size();
        std::vector<asset::TerrainCollider> meshes(chosen);
        jobs::parallelFor("terrain.collider.meshes", jobs::Domain::SimVisible, 0, chosen, 1,
                          [&](usize begin, usize end, u32) noexcept {
                              for (usize at = begin; at < end; ++at)
                                  meshes[at] = asset::meshCollider(field, pending[at].key);
                          });

        for (usize index = 0; index < chosen; ++index) {
            const Pending& next = pending[index];
            const asset::TerrainCollider& meshed = meshes[index];

            // **What the ground is, triangle by triangle** (ADR 0117): the
            // layer each is DRAWN as -- its paint and the rules included, as
            // a raycast reports it -- so ice painted over rock is ice to
            // stand on. Only for a chunk that has a triangle of a surface
            // that is not the default: the rest are handed nothing, and
            // collide exactly as they did.
            std::vector<u8> drawn;
            if (surfaces.key != 0 && meshed.surfaces.size() == meshed.indices.size()) {
                const usize triangles = meshed.indices.size() / 3;
                drawn.resize(triangles, 0);
                bool any = false;
                const physics::SurfaceMaterial standard;
                for (usize triangle = 0; triangle < triangles; ++triangle) {
                    const core::Vec3& a = meshed.points[meshed.indices[triangle * 3]];
                    const core::Vec3& b = meshed.points[meshed.indices[triangle * 3 + 1]];
                    const core::Vec3& c = meshed.points[meshed.indices[triangle * 3 + 2]];
                    const core::Vec3 face = core::cross(b - a, c - a);
                    const f32 area = std::sqrt(core::dot(face, face));
                    const core::Vec3 normal = area > 1.0e-12f ? face * (1.0f / area) : core::Vec3{0.0f, 1.0f, 0.0f};
                    const core::Vec3 middle = (a + b + c) * (1.0f / 3.0f);
                    const u8 layer = asset::drawnMaterial(
                        terrain.rules,
                        asset::Voxel{255, meshed.surfaces[triangle * 3], meshed.surfaces[triangle * 3 + 1],
                                     meshed.surfaces[triangle * 3 + 2]},
                        normal, middle, middle.y + static_cast<f32>(terrain.origin.y));
                    if (layer >= surfaces.table.size())
                        continue;
                    drawn[triangle] = layer;
                    any = any || surfaces.table[layer].friction != standard.friction ||
                          surfaces.table[layer].restitution != standard.restitution;
                }
                if (!any)
                    drawn.clear();
            }

            physics::BodyHandle handle{};
            if (meshed.indices.size() >= 3) {
                physics::BodyDesc desc;
                desc.shape.type = physics::ShapeType::TriangleMesh;
                desc.shape.points = meshed.points;
                desc.shape.indices = meshed.indices;
                desc.shape.triangleSurfaces = drawn;
                if (!drawn.empty())
                    desc.surfaces = surfaces.table;
                // The band of its neighbours' triangles, which only lends its
                // edges: the seam is inside the mesh (ADR 0143).
                desc.shape.bandFirst = meshed.bandFirst;
                desc.shape.pointsRevision = next.content;
                desc.shape.geometryRevision = next.content;
                desc.motion = physics::MotionType::Static;
                // **Offset by the terrain's own origin**, which is what makes a
                // terrain a thing you can move: the field is untouched.
                desc.transform.position = terrain.origin;
                handle = m_backend.createBody(m_world, desc);
            }

            // Kept even with no body -- a chunk dug to nothing is a chunk whose
            // collider is current -- so it is not remeshed every tick.
            auto [at, exists] = locate(id, next.key);
            if (exists) {
                if (at->body.valid())
                    m_backend.destroyBody(m_world, at->body);
                at->body = handle;
                at->revision = terrain.fieldRevision;
                at->content = next.content;
                at->placement = placement;
                at->seen = true;
            }
            else {
                m_terrainColliders.insert(
                    at, TerrainCollider{id, next.key, handle, terrain.fieldRevision, next.content, placement, true});
            }
            rebuilt += 1;
        }

        if (chosen > 0)
            remade.push_back(id);
    });

    // **The ground is solid, not a skin** (D417). A terrain's collider is a
    // shell of triangles, and what gets behind it has nothing to push it out:
    // a log lying where the ground was raised a metre and a half; a log that
    // struck end first, came round on that end and put its other one through
    // -- a sweep is of where a body goes, not of how it turns. Both fell out
    // of the bottom of the world. So a loose body whose own middle is in the
    // ground is put on top of it, as a height field's ground does in the
    // engines that have one: the field is asked, which is what the ground is.
    //
    // Asked of a body that is moving -- one asleep cannot sink -- and of every
    // loose body on a tick that remade a collider, which is the tick ground
    // changed near something. A body a raise merely touches has its middle in
    // the air, and is the solver's.
    if (moversGathered) {
        m_scene.terrains().forEach([&](core::InstanceId id, TerrainComponent& terrain) {
            if (!inWorld(id) || terrain.field.empty())
                return;
            const asset::TerrainField& field = terrain.field;
            const bool everyBody = std::find(remade.begin(), remade.end(), id) != remade.end();
            const f64 rise = static_cast<f64>(field.settings().voxelSize) * 0.5;
            for (const Sweep& sweep : sweeps) {
                if (!sweep.loose && !sweep.character)
                    continue;
                RigidBodyComponent* body = m_scene.rigidBodies().find(sweep.id);
                PartComponent* part = m_scene.parts().find(sweep.id);
                if (body == nullptr || part == nullptr)
                    continue;
                // **A character is asked every tick**: it stands still while
                // the ground is raised round it, and there are few of them.
                const bool moving = !(body->linearVelocity == core::Vec3{0.0f, 0.0f, 0.0f}) ||
                                    !(body->angularVelocity == core::Vec3{0.0f, 0.0f, 0.0f});
                if (!everyBody && !moving && !sweep.character)
                    continue;
                const core::DVec3 at = part->cframe.position;
                const core::DVec3 local{at.x - terrain.origin.x, at.y - terrain.origin.y, at.z - terrain.origin.z};
                if (!(asset::sampleField(field, local).distance < 0.0f))
                    continue;
                // The nearest air over it.
                f64 lifted = 0.0;
                bool open = false;
                for (f64 up = rise; up <= UnburyReach; up += rise) {
                    if (!(asset::sampleField(field, core::DVec3{local.x, local.y + up, local.z}).distance < 0.0f)) {
                        lifted = up;
                        open = true;
                        break;
                    }
                }
                if (!open)
                    continue;
                // Resting on it as it lies: half its height as it is turned.
                const core::Vec3 half{part->size.x * 0.5f, part->size.y * 0.5f, part->size.z * 0.5f};
                const core::Mat3& turn = part->cframe.rotation;
                const f64 standing = static_cast<f64>(std::abs((turn * core::Vec3{1.0f, 0.0f, 0.0f}).y) * half.x +
                                                      std::abs((turn * core::Vec3{0.0f, 1.0f, 0.0f}).y) * half.y +
                                                      std::abs((turn * core::Vec3{0.0f, 0.0f, 1.0f}).y) * half.z);
                // Written to the part: the walk after this hands a part
                // somebody moved to the backend, and this is somebody.
                part->cframe.position.y = at.y + lifted + standing;
                // It was falling, or it was not moving: either way it is not
                // falling now.
                body->linearVelocity.y = std::max(body->linearVelocity.y, 0.0f);
                if (sweep.character) {
                    // A controller owns its own fall.
                    if (CharacterBodyComponent* walker = m_scene.characterBodies().find(sweep.id); walker != nullptr)
                        walker->verticalVelocity = std::max(walker->verticalVelocity, 0.0f);
                }
                else if (sweep.id.index < m_bodies.size()) {
                    const BodyRecord& record = m_bodies[sweep.id.index];
                    if (record.live && record.generation == sweep.id.generation)
                        m_backend.setBodyVelocity(m_world, record.handle, body->linearVelocity, body->angularVelocity);
                }
            }
        });
    }

    retireUnseenTerrain();
}

void PhysicsSync::applyVoxels()
{
    for (VoxelCollider& collider : m_voxelColliders)
        collider.seen = false;

    const VoxelComponent* voxels = nullptr;
    m_scene.voxels().forEach([&voxels](core::InstanceId, const VoxelComponent& found) {
        if (voxels == nullptr)
            voxels = &found;
    });

    if (voxels != nullptr && voxels->grid.chunkCount() > 0) {
        // Where things that move are: bodies not anchored, and characters --
        // and what each can cross before its blocks could be made again
        // (`Sweep`).
        std::vector<core::DVec3> movers;
        std::vector<Sweep> sweeps;
        m_scene.rigidBodies().forEach([&](core::InstanceId id, const RigidBodyComponent& body) {
            const bool character = m_scene.characterBodies().find(id) != nullptr;
            if (body.anchored && !character)
                return;
            const PartComponent* part = m_scene.parts().find(id);
            if (part != nullptr && inWorld(id)) {
                movers.push_back(part->cframe.position);
                sweeps.push_back(sweepOf(id, *part, body, !body.anchored && !character, character));
            }
        });

        const f64 chunkMetres = static_cast<f64>(asset::VoxelChunkEdge) * static_cast<f64>(voxels->blockSize);
        u32 rebuilt = 0;
        // **Only which types are fluids**: every other block collides as the
        // opaque solid it always did, glass and leaves included, and a fluid
        // leaves the collider -- a lake is waded into, not stood on.
        std::vector<asset::BlockLook> looks(voxels->types.size() + 1);
        u64 fluidDigest = 0x666C7569ull;
        for (usize at = 0; at < voxels->types.size(); ++at) {
            if (voxels->types[at].fluidReach == 0)
                continue;
            looks[at + 1].fluid = true;
            looks[at + 1].reach = voxels->types[at].fluidReach;
            fluidDigest = combine(fluidDigest, static_cast<u64>(at + 1));
        }
        for (const asset::VoxelChunkKey key : voxels->grid.chunkKeys()) {
            bool near = false;
            for (const core::DVec3& mover : movers) {
                const auto axis = [&](i32 index, f64 at) {
                    const f64 low = static_cast<f64>(index) * chunkMetres;
                    return std::max({low - at, 0.0, at - (low + chunkMetres)});
                };
                const f64 dx = axis(key.x, mover.x);
                const f64 dy = axis(key.y, mover.y);
                const f64 dz = axis(key.z, mover.z);
                if (dx * dx + dy * dy + dz * dz <= VoxelCollisionReach * VoxelCollisionReach) {
                    near = true;
                    break;
                }
            }
            if (!near)
                continue;

            u64 content = combine(static_cast<u64>(std::bit_cast<u32>(voxels->blockSize)), fluidDigest);
            for (const asset::VoxelChunkKey neighbour :
                 {key, asset::VoxelChunkKey{key.x - 1, key.y, key.z}, asset::VoxelChunkKey{key.x + 1, key.y, key.z},
                  asset::VoxelChunkKey{key.x, key.y - 1, key.z}, asset::VoxelChunkKey{key.x, key.y + 1, key.z},
                  asset::VoxelChunkKey{key.x, key.y, key.z - 1}, asset::VoxelChunkKey{key.x, key.y, key.z + 1}}) {
                const asset::VoxelChunk* chunk = voxels->grid.findChunk(neighbour);
                content = combine(content, chunk == nullptr ? 0x6E6F6E65ull : asset::digestOf(*chunk));
            }

            auto at = std::lower_bound(
                m_voxelColliders.begin(), m_voxelColliders.end(), key,
                [](const VoxelCollider& entry, asset::VoxelChunkKey probe) { return entry.key < probe; });
            const bool exists = at != m_voxelColliders.end() && at->key == key;
            if (exists) {
                at->seen = true;
                if (at->content == content)
                    continue;
            }
            // What the collider's mesh actually read, asked only when the
            // coarse key moved: a block mined inside a neighbour rebuilt this
            // chunk's collider for nothing (audit P1, 14-voxels).
            const u64 exact = combine(combine(asset::shellDigestOf(voxels->grid, key), fluidDigest),
                                      static_cast<u64>(std::bit_cast<u32>(voxels->blockSize)));
            if (exists && at->exact == exact) {
                at->content = content;
                continue;
            }
            if (rebuilt >= VoxelRebuildsPerTick) {
                // The old collider, if any, stands until its turn -- unless
                // the chunk is in a mover's way (D417).
                const f64 low[3] = {static_cast<f64>(key.x) * chunkMetres, static_cast<f64>(key.y) * chunkMetres,
                                    static_cast<f64>(key.z) * chunkMetres};
                const bool inTheWay = std::any_of(sweeps.begin(), sweeps.end(), [&](const Sweep& sweep) {
                    return sweep.high.x >= low[0] && sweep.low.x <= low[0] + chunkMetres && sweep.high.y >= low[1] &&
                           sweep.low.y <= low[1] + chunkMetres && sweep.high.z >= low[2] &&
                           sweep.low.z <= low[2] + chunkMetres;
                });
                if (!inTheWay)
                    continue;
            }

            const asset::VoxelMesh meshed = asset::meshVoxelChunk(voxels->grid, key, looks, voxels->blockSize);
            physics::BodyHandle handle{};
            if (meshed.colliderIndices.size() >= 3) {
                physics::BodyDesc desc;
                desc.shape.type = physics::ShapeType::TriangleMesh;
                desc.shape.points = meshed.colliderPoints;
                desc.shape.indices = meshed.colliderIndices;
                desc.shape.pointsRevision = content;
                desc.shape.geometryRevision = content;
                desc.motion = physics::MotionType::Static;
                handle = m_backend.createBody(m_world, desc);
            }
            if (exists) {
                if (at->body.valid())
                    m_backend.destroyBody(m_world, at->body);
                at->body = handle;
                at->content = content;
                at->exact = exact;
            }
            else {
                m_voxelColliders.insert(at, VoxelCollider{key, handle, content, exact, true});
            }
            rebuilt += 1;
        }
    }

    // Chunks gone, or out of reach, give their bodies back.
    for (usize at = m_voxelColliders.size(); at > 0; --at) {
        VoxelCollider& collider = m_voxelColliders[at - 1];
        if (collider.seen)
            continue;
        if (collider.body.valid())
            m_backend.destroyBody(m_world, collider.body);
        m_voxelColliders.erase(m_voxelColliders.begin() + static_cast<std::ptrdiff_t>(at - 1));
    }
}

void PhysicsSync::retireUnseenTerrain()
{
    // A tile the field no longer holds -- cleared, or its terrain destroyed --
    // takes its collider with it. Walked back to front so an erase cannot move
    // an entry this loop has not reached.
    for (usize at = m_terrainColliders.size(); at > 0; --at) {
        TerrainCollider& collider = m_terrainColliders[at - 1];
        if (collider.seen)
            continue;
        if (collider.body.valid())
            m_backend.destroyBody(m_world, collider.body);
        m_terrainColliders.erase(m_terrainColliders.begin() + static_cast<std::ptrdiff_t>(at - 1));
    }
}

void PhysicsSync::applyScene()
{
    syncCollisionGroups();

    // **Terrain first, and the order is load-bearing.** Body slots are assigned
    // in creation order, so an interleave that depended on which pool happened
    // to be walked first would make the ids a fact about the walk rather than
    // about the world (R10).
    applyTerrain();
    applyVoxels();

    // Cleared every tick: a part reparented between two ticks must not be
    // answered from the previous one's memo.
    m_lastParent = core::InstanceId{};
    m_lastParentInWorld = false;

    retireGone();
    for (BodyRecord& record : m_bodies)
        record.seen = false;
    for (auto& entry : m_characters)
        entry.second.seen = false;

    const f32 fixedDt = static_cast<f32>(m_scene.engineState().fixedTimestep);

    // The pool walk is dense and in slot order, which is a pure function of the
    // operation sequence -- so the order bodies are created in, and therefore
    // the order the backend assigns its own ids in, is deterministic (R10).
    m_scene.rigidBodies().forEach([&](core::InstanceId id, RigidBodyComponent& body) {
        PartComponent* part = m_scene.parts().find(id);
        if (part == nullptr || !inWorld(id))
            return;

        if (CharacterBodyComponent* character = m_scene.characterBodies().find(id); character != nullptr) {
            applyCharacter(id, *part, body, *character, fixedDt);
            return;
        }
        applyBody(id, *part, body);
    });

    // **A SECOND walk, after every body exists**, and in the constraint pool's
    // own order -- which is creation order, and which the backend solves in.
    // Folding this into the body walk would mean creating a joint before the
    // body at its other end had been made; creating that body from here instead
    // would put body creation order in the CONSTRAINT pool's order, and two
    // scenes that differ only in the order somebody added joints would then
    // simulate differently (R10).
    for (ConstraintRecord& record : m_constraints)
        record.seen = false;
    m_anySpring = false;

    m_scene.constraints().forEach(
        [&](core::InstanceId id, ConstraintComponent& constraint) { applyConstraint(id, constraint); });

    retireUnseen();
    // After the sweep, so a pair is only ever excluded between bodies that
    // exist.
    applyNoCollisions();
}

void PhysicsSync::syncForQuery()
{
    if (!m_workspace.valid())
        return;
    const u64 written = m_scene.mutations();
    if (written == m_querySynced)
        return;
    m_querySynced = written;

    // The memo `applyScene` clears, for its reason.
    m_lastParent = core::InstanceId{};
    m_lastParentInWorld = false;
    // In the pool's own order, as the tick makes them: the bodies come to be
    // in the order they would have, only sooner.
    m_scene.rigidBodies().forEach([&](core::InstanceId id, RigidBodyComponent& body) {
        // Already a body, or already refused one: the tick's.
        if (id.index < m_bodies.size() && m_bodies[id.index].generation == id.generation)
            return;
        PartComponent* part = m_scene.parts().find(id);
        if (part == nullptr || m_scene.destroyed(id) || !inWorld(id) || m_scene.characterBodies().find(id) != nullptr)
            return;
        applyBody(id, *part, body);
    });
}

bool PhysicsSync::isDriven(core::InstanceId id) const
{
    return marked(m_drivenMarks, id);
}

// Welds resolve AFTER the step and the writeback, which is the defined point in
// the tick the roadmap asks for. Before it, a driven part would follow where its
// anchor was last tick and lag by a frame; after it, it follows where the anchor
// ended up this one.
//
// The order within the pass is dependency order, not pool order: `resolveWeld`
// resolves whatever its anchor hangs from before it resolves itself, so a chain
// lands correctly however the pool holds it. Cycles cannot occur -- the property
// setters refuse the write that would create one -- and the recursion is bounded
// by the weld count regardless.
void PhysicsSync::resolveWelds()
{
    ++m_resolveRound;

    // Slot order, which is a pure function of the operation sequence. It decides
    // nothing about the result, because dependency order does -- what it decides
    // is that two runs walk the same list.
    m_scene.welds().forEach([&](core::InstanceId weldId, WeldComponent& weld) { resolveWeld(weldId, weld); });
}

// The part an id is or sits on: itself for a part, its parent for an attachment,
// invalid for anything else.
core::InstanceId PhysicsSync::rigAbove(core::InstanceId id) const
{
    // The nearest `MeshPart` with a skeleton, walking upwards. A handful of
    // steps: a bone sits on a character or on one of its limbs.
    for (core::InstanceId cursor = m_scene.parentOf(id); cursor.valid(); cursor = m_scene.parentOf(cursor)) {
        if (m_scene.meshParts().find(cursor) == nullptr)
            continue;
        if (m_skeleton != nullptr && m_skeleton->jointCount(cursor) > 0)
            return cursor;
    }
    return {};
}

core::InstanceId PhysicsSync::ownerOf(core::InstanceId id) const
{
    if (m_scene.parts().find(id) != nullptr)
        return id;
    if (m_scene.attachments().find(id) != nullptr)
        return m_scene.parentOf(id);
    return {};
}

bool PhysicsSync::anchorFrame(core::InstanceId id, core::CFrameD& out)
{
    if (const PartComponent* part = m_scene.parts().find(id); part != nullptr) {
        out = part->cframe;
        return true;
    }
    if (const AttachmentComponent* attachment = m_scene.attachments().find(id); attachment != nullptr) {
        // Resolved first, so an attachment used as an anchor is where it ends up
        // this tick rather than where it was last one -- the same rule the weld
        // chain follows.
        resolveAttachment(id);
        out = attachment->worldCFrame;
        return true;
    }
    return false;
}

void PhysicsSync::resolveAttachment(core::InstanceId id)
{
    if (marked(m_attachmentMarks, id))
        return;
    mark(m_attachmentMarks, id);

    AttachmentComponent* attachment = m_scene.attachments().find(id);
    if (attachment == nullptr)
        return;

    const core::InstanceId owner = m_scene.parentOf(id);
    const PartComponent* part = m_scene.parts().find(owner);
    if (part == nullptr) {
        // Parented to something that is not a part -- while a script is still
        // assigning, or by mistake. Its own frame is the honest answer: it is
        // where it says it is, relative to nothing.
        attachment->worldCFrame = attachment->cframe;
        return;
    }

    // No weld settle here, and that is checked rather than assumed. `step` runs
    // `resolveWelds()` before `resolveAttachments()`, so every weld is already
    // done by the time this walk starts -- and the one path that reaches an
    // attachment EARLY, a weld anchored to one, settles the owner itself
    // through `ownerOf` before it asks. A settle in this function changed no
    // observable behaviour, so it is not here.
    core::CFrameD base = part->cframe;
    // **The rig a bone belongs to is the nearest skinned `MeshPart` above it**,
    // which is the owner itself for a bone on a character and the CHARACTER for
    // a bone on a ragdoll's limb. A limb is a plain `Part` with no rig of its
    // own; asking it about a joint would make the answer depend on how defensive
    // the host happens to be, and relying on that is not a contract worth having.
    if (attachment->jointName.id != 0 && m_skeleton != nullptr) {
        const core::InstanceId rig = rigAbove(id);
        if (rig.valid()) {
            if (attachment->jointIndex < 0)
                attachment->jointIndex = m_skeleton->findJoint(rig, m_scene.atoms().text(attachment->jointName));

            // **Resolved against the rig, but FOLLOWED only when the rig is the
            // owner.** A bone on a character IS the joint and moves with the
            // animation; a bone on a ragdoll's limb is a LABEL saying which
            // joint that limb stands for, and it has to stay where the
            // simulation put the limb -- following the joint would make it chase
            // the pose it is itself about to write.
            //
            // The `>= 0` guard is for the CAST rather than for the host:
            // `jointModel` refuses an index it does not have, so no test can
            // tell this apart from asking with `static_cast<u32>(-1)`.
            if (rig == owner && attachment->jointIndex >= 0) {
                core::CFrameD joint;
                if (m_skeleton->jointModel(owner, static_cast<u32>(attachment->jointIndex), joint))
                    base = part->cframe * joint * attachment->transform;
            }
        }
    }

    attachment->worldCFrame = base * attachment->cframe;
}

void PhysicsSync::driveRagdolls()
{
    if (m_skeleton == nullptr)
        return;

    // Pool order, which is creation order. It decides nothing about the result
    // -- a ragdoll's joints are independent of each other's overrides -- but it
    // decides that two runs walk the same list (R10).
    m_scene.ragdolls().forEach([&](core::InstanceId id, RagdollComponent& ragdoll) {
        if (!ragdoll.enabled)
            return;

        // The mesh whose pose this drives is the thing the ragdoll is parented
        // to, exactly as an `AnimationPlayer`'s is.
        const core::InstanceId meshPart = m_scene.parentOf(id);
        const PartComponent* mesh = m_scene.parts().find(meshPart);
        if (mesh == nullptr || m_skeleton->jointCount(meshPart) == 0)
            return;

        // Every `Bone` under the ragdoll that resolved to a joint. The part it
        // sits on is where the simulation put that limb; the bone says which
        // joint it is. Nothing else is declared -- which is also why a PARTIAL
        // ragdoll works: the joints nobody drives keep their own place relative
        // to their parent when `commitOverrides` re-runs the forward pass.
        std::vector<core::InstanceId> descendants;
        m_scene.collectDescendants(id, descendants);
        for (const core::InstanceId child : descendants) {
            const AttachmentComponent* bone = m_scene.attachments().find(child);
            if (bone == nullptr || bone->jointIndex < 0)
                continue;

            // Into the MESH's own space, because that is the space a pose is in.
            // The bone's world frame is where the limb ended up; dividing out
            // the mesh part's own transform is what carries it there.
            const core::CFrameD simulated = core::inverse(mesh->cframe) * bone->worldCFrame;

            // **`Blend` interpolates the POSE, not the solver.** At 0.5 the limb
            // has fallen exactly as far as it would at 1 and the drawn joint is
            // carried halfway there, which is what a stumble is: the clip keeps
            // running and the character sags. Ramping it is what makes going
            // down something other than a one-frame snap.
            //
            // The animated end comes from `jointModel`, which answers for a mesh
            // in bind pose as well as for one mid-clip -- so a character with
            // nothing playing blends towards where it is standing rather than
            // towards nothing.
            core::CFrameD model = simulated;
            if (ragdoll.blend < 1.0f) {
                core::CFrameD animated;
                if (!m_skeleton->jointModel(meshPart, static_cast<u32>(bone->jointIndex), animated))
                    continue;
                model = core::lerp(animated, simulated, static_cast<f64>(ragdoll.blend));
            }
            m_skeleton->setJointOverride(meshPart, static_cast<u32>(bone->jointIndex), model);
        }
    });
}

void PhysicsSync::resolveAttachments()
{
    // Pool order, which is creation order. It decides nothing about the RESULT
    // -- the recursion above settles dependencies whatever order they are
    // reached in -- but it decides that two runs walk the same list (R10).
    m_scene.attachments().forEach([&](core::InstanceId id, AttachmentComponent&) { resolveAttachment(id); });
}

void PhysicsSync::resolveWeld(core::InstanceId weldId, WeldComponent& weld)
{
    if (marked(m_weldMarks, weldId))
        return;
    mark(m_weldMarks, weldId);

    if (!weld.enabled || !m_scene.alive(weld.part0) || !m_scene.alive(weld.part1))
        return;

    // **The DRIVEN end must be a part.** A weld moves something, and an
    // attachment has nothing of its own to move -- it is a place on a part.
    PartComponent* driven = m_scene.parts().find(weld.part1);
    if (driven == nullptr)
        return;

    // The anchor may itself be driven by another weld. Resolve that one first,
    // so a chain settles in one pass instead of lagging one link per tick.
    //
    // An anchor that is an ATTACHMENT resolves through the part it is on, which
    // is what makes "weld a sword to the hand of a character that is itself
    // welded to a platform" settle in the same single pass.
    const core::InstanceId anchorPart = ownerOf(weld.part0);
    m_scene.welds().forEach([&](core::InstanceId otherId, WeldComponent& other) {
        if (otherId != weldId && other.enabled && other.part1 == anchorPart)
            resolveWeld(otherId, other);
    });

    core::CFrameD anchorFrameValue;
    if (!anchorFrame(weld.part0, anchorFrameValue))
        return;

    // A constraint reads the relationship off the world the first time it holds;
    // a weld was told it. `C1` is what carries it either way, so the resolver
    // has one formula.
    if (weld.captures && !weld.captured) {
        weld.c1 = core::inverse(driven->cframe) * (anchorFrameValue * weld.c0);
        weld.captured = true;
    }

    driven->cframe = (anchorFrameValue * weld.c0) * core::inverse(weld.c1);
    mark(m_drivenMarks, weld.part1);
}

// **What was destroyed leaves before anything is made** (D467).
//
// The sweep below runs after the walk that creates this tick's bodies, so a
// thing made in the tick another was destroyed in was made beside the dead
// one's body. A rigid body is gone again before the step and nothing came of
// it -- but a character controller keeps the contacts it finds when it is
// made, and its first step was resolved against a body that no longer existed:
// a character respawned where the last one stood was shoved a metre and a half
// sideways, or stood on nothing for a tick and lost the jump it was given.
//
// Only what is DESTROYED goes here, which is one flag an instance. What left
// the world some other way -- reparented out, its class changed -- is still
// the sweep's.
void PhysicsSync::retireGone()
{
    const auto gone = [this](core::InstanceId id) { return !m_scene.alive(id) || m_scene.destroyed(id); };

    // Constraints first, for the sweep's reason.
    for (usize index = 0; index < m_constraints.size(); ++index) {
        ConstraintRecord& record = m_constraints[index];
        if (record.generation == 0 || !gone(core::InstanceId{static_cast<u32>(index), record.generation}))
            continue;
        m_backend.destroyConstraint(m_world, record.handle);
        record = ConstraintRecord{};
    }
    for (usize index = 0; index < m_bodies.size(); ++index) {
        BodyRecord& record = m_bodies[index];
        if (record.generation == 0 || !gone(core::InstanceId{static_cast<u32>(index), record.generation}))
            continue;
        if (record.live) {
            m_backend.destroyBody(m_world, record.handle);
            --m_bodyCount;
        }
        record = BodyRecord{};
    }
    for (auto it = m_characters.begin(); it != m_characters.end();) {
        if (!gone(unpackInstance(it->first))) {
            ++it;
            continue;
        }
        m_backend.destroyCharacter(m_world, it->second.handle);
        it = m_characters.erase(it);
    }
}

void PhysicsSync::retireUnseen()
{
    // **Constraints first, and this is a contract rather than a tidiness.** A
    // joint holding a body that is gone is a dangling pointer inside the solver
    // and it is silent -- so the backend drops a body's joints itself, and a
    // sweep that retired bodies first would then destroy those joints a second
    // time from here.
    for (ConstraintRecord& record : m_constraints) {
        if (record.generation == 0 || record.seen)
            continue;
        m_backend.destroyConstraint(m_world, record.handle);
        record = ConstraintRecord{};
    }

    for (BodyRecord& record : m_bodies) {
        if (record.generation == 0 || record.seen)
            continue;
        // **`live` decides whether there is anything to destroy.** A record that
        // remembers a REFUSAL carries this instance's generation and no body, so
        // destroying it would hand the backend a handle that names nothing and
        // take the count down for a body that was never made.
        if (record.live) {
            m_backend.destroyBody(m_world, record.handle);
            --m_bodyCount;
        }
        record = BodyRecord{};
    }
    for (auto it = m_characters.begin(); it != m_characters.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        m_backend.destroyCharacter(m_world, it->second.handle);
        it = m_characters.erase(it);
    }
}

// The body an instance has, or an invalid handle. A part outside the world, or
// one whose body has not been made yet, has none.
physics::BodyHandle PhysicsSync::bodyHandleOf(core::InstanceId id) const
{
    if (!id.valid() || id.index >= m_bodies.size())
        return {};
    const BodyRecord& record = m_bodies[id.index];
    // **`seen` and not just `generation`.** The body walk runs before the
    // constraint walk, so `seen` is already this tick's answer -- and a body
    // that left the world still HAS a record until `retireUnseen` runs, so
    // asking only about the generation would hand a joint a body that is about
    // to be destroyed under it. The joint would then be marked seen, survive the
    // sweep, and hold a dangling pointer inside the solver.
    //
    // **And `live`**, for the reason `retireUnseen` reads it: a record that
    // remembers a refusal has this instance's generation and no body behind it.
    return record.generation == id.generation && record.seen && record.live ? record.handle : physics::BodyHandle{};
}

void PhysicsSync::applyConstraint(core::InstanceId id, ConstraintComponent& constraint)
{
    // **A mover that holds something is a motor in the solver** (ADR 0127,
    // D471): a place, a facing, a speed, a spin -- solved with the joints the
    // body is in, or it fights them. One that only pushes -- a force, a
    // torque, a spring without its stops -- is an impulse before the step, and
    // the solver is never handed anything for it.
    const auto distance = static_cast<i32>(physics::ConstraintType::Distance);
    if (constraint.kind == distance && constraint.flavor == 2)
        m_anySpring = true;
    if (constraint.kind >= MoverKind::LinearVelocity && constraint.kind <= MoverKind::AlignOrientation) {
        applyDrive(id, constraint);
        return;
    }
    if (constraint.kind >= MoverKind::LinearVelocity ||
        (constraint.kind == distance && constraint.flavor == 2 && !constraint.limitsEnabled))
        return;

    // The two BODIES, through the parts the attachments sit on. A constraint
    // joins bodies; the attachments are where on them.
    const core::InstanceId body0 = m_scene.parentOf(constraint.attachment0);
    const core::InstanceId body1 = m_scene.parentOf(constraint.attachment1);

    const AttachmentComponent* end0 = m_scene.attachments().find(constraint.attachment0);
    const AttachmentComponent* end1 = m_scene.attachments().find(constraint.attachment1);
    const bool usable = end0 != nullptr && end1 != nullptr && body0.valid() && body1.valid() && body0 != body1 &&
                        inWorld(id) && bodyHandleOf(body0).valid() && bodyHandleOf(body1).valid() &&
                        // **A `CharacterBody` is swept, not solved** (the M5
                        // finding): `CharacterVirtual` is not a solver body, so
                        // there is nothing for a joint to hold.
                        m_scene.characterBodies().find(body0) == nullptr &&
                        m_scene.characterBodies().find(body1) == nullptr;

    if (id.index >= m_constraints.size())
        m_constraints.resize(id.index + 1);
    ConstraintRecord& record = m_constraints[id.index];

    if (!usable) {
        // Left unseen, so `retireUnseen` destroys it. A joint whose ends a
        // script is still assigning is not an error -- it is what every script
        // that sets two properties on two lines briefly produces.
        return;
    }

    // **A ball joint that holds a pose is a swing-twist** (amendment A1): the
    // free ball has no motor, so asking for one changes which solver joint is
    // built, as switching its limits on does.
    const auto point = static_cast<i32>(physics::ConstraintType::Point);
    const i32 kind = constraint.kind == point && constraint.actuatorType == 2
                         ? static_cast<i32>(physics::ConstraintType::SwingTwist)
                         : constraint.kind;

    // A rebuild is needed when what the joint IS changed. Everything else --
    // a limit, the collide flag -- is an update the backend can take without
    // moving the constraint's place in the solve order.
    const bool rebuild =
        record.generation != id.generation || record.body0 != body0 || record.body1 != body1 || record.kind != kind;

    const f32 fixedDt = static_cast<f32>(m_scene.engineState().fixedTimestep);
    // **A winch takes in rope by shortening the rope** (ADR 0127): `Length`
    // itself moves towards the target, and stalls when the last tick's tension
    // was more than the winch can pull against.
    if (constraint.kind == distance && constraint.flavor == 0 && constraint.winchEnabled && constraint.enabled) {
        const f32 most = constraint.winchSpeed * fixedDt;
        if (constraint.length > constraint.winchTarget) {
            if (constraint.lastForce <= constraint.winchForce)
                constraint.length = std::max(constraint.winchTarget, constraint.length - most);
        }
        else if (constraint.length < constraint.winchTarget) {
            constraint.length = std::min(constraint.winchTarget, constraint.length + most);
        }
    }

    physics::ConstraintDesc desc;
    desc.type = static_cast<physics::ConstraintType>(kind);
    desc.first = bodyHandleOf(body0);
    desc.second = bodyHandleOf(body1);
    // In each body's OWN space, which is what the seam asks for and what makes a
    // floating-origin rebase cost nothing.
    desc.firstFrame = end0->cframe;
    desc.secondFrame = end1->cframe;
    desc.collideConnected = constraint.collideConnected;
    if (constraint.limitsEnabled) {
        desc.limitLow = constraint.limitLow;
        desc.limitHigh = constraint.limitHigh;
        desc.swingLimit = constraint.swingLimit;
        desc.twistLimit = constraint.twistLimit;
    }
    // The distance family: a rope may be as short as it likes and no longer
    // than its length, a rod is exactly its length, and a spring's stops are
    // its two lengths.
    if (kind == distance) {
        desc.minDistance = constraint.flavor == 1   ? constraint.length
                           : constraint.flavor == 2 ? constraint.minLength
                                                    : 0.0f;
        desc.maxDistance =
            constraint.flavor == 2 ? std::max(constraint.maxLength, constraint.minLength) : constraint.length;
    }
    motorOf(constraint, record, fixedDt, desc);
    desc.userData = packInstance(id);

    if (rebuild) {
        if (record.generation != 0)
            m_backend.destroyConstraint(m_world, record.handle);
        record = ConstraintRecord{};
        record.handle = m_backend.createConstraint(m_world, desc);
        if (!record.handle.valid())
            return;
        record.generation = id.generation;
    }
    else if (record.collideConnected != constraint.collideConnected || record.limitLow != constraint.limitLow ||
             record.limitHigh != constraint.limitHigh || record.swingLimit != constraint.swingLimit ||
             record.twistLimit != constraint.twistLimit || record.limitsEnabled != constraint.limitsEnabled) {
        m_backend.updateConstraint(m_world, record.handle, desc);
    }
    else {
        // What changes every tick, in place: the motor and the range. The
        // backend does nothing when neither moved.
        m_backend.driveConstraint(m_world, record.handle, desc);
    }

    if (record.enabled != constraint.enabled) {
        m_backend.setConstraintEnabled(m_world, record.handle, constraint.enabled);
        record.enabled = constraint.enabled;
    }

    record.seen = true;
    record.body0 = body0;
    record.body1 = body1;
    record.kind = kind;
    record.collideConnected = constraint.collideConnected;
    record.limitLow = constraint.limitLow;
    record.limitHigh = constraint.limitHigh;
    record.swingLimit = constraint.swingLimit;
    record.twistLimit = constraint.twistLimit;
    record.limitsEnabled = constraint.limitsEnabled;
}

void PhysicsSync::writeBack()
{
    m_active.clear();
    m_backend.collectActiveBodies(m_world, m_active);
    ++m_writeBackStamp;

    for (const physics::ActiveBody& active : m_active) {
        const core::InstanceId id = unpackInstance(active.userData);
        if (!m_scene.alive(id))
            continue;

        if (id.index >= m_bodies.size() || m_bodies[id.index].generation != id.generation)
            continue;
        BodyRecord& record = m_bodies[id.index];
        record.activeStamp = m_writeBackStamp;

        // **A kinematic body is not written back** (D031). Its transform is the
        // script's -- a tween wrote it and the mirror moved the body to match --
        // so copying the solver's answer back into the component says nothing
        // and costs a pool lookup and a write per body per tick.
        //
        // Jolt reports every kinematic body as active, always, so before this
        // line `churn10k`'s writeback went from 0.03 ms to 9.9 ms the moment
        // moving anchored parts became kinematic. The measurement is what found
        // it, which is the whole argument for `perf-baselines.md`.
        if (record.backendMotion == physics::MotionType::Kinematic)
            continue;

        // The QUIET write: straight into the component, with the changed set
        // reaching a listener only if one exists (architecture.md §4). Ten
        // thousand moving parts enqueue nothing while nobody is watching.
        if (PartComponent* part = m_scene.parts().find(id); part != nullptr) {
            part->cframe = active.state.transform;
            record.written = active.state.transform;
        }
        if (RigidBodyComponent* body = m_scene.rigidBodies().find(id); body != nullptr) {
            body->linearVelocity = active.state.linearVelocity;
            body->angularVelocity = active.state.angularVelocity;
            body->active = active.state.active;
        }
    }

    // A body that just went to sleep is no longer in the active list, and its
    // component still says it is moving. One pass over the records fixes that
    // and costs nothing for a world where nothing is asleep.
    // Over what the SCENE says is still moving rather than over every record: a
    // body that just went to sleep has left the active list while its component
    // still says it is moving, and a world where nothing is asleep pays for
    // nothing.
    //
    // **By the record's stamp, not by searching the active list** (audit E4):
    // the search, once per moving body, was five thousand bodies times five
    // thousand compares a tick.
    m_scene.rigidBodies().forEach([&](core::InstanceId id, RigidBodyComponent& body) {
        if (!body.active)
            return;
        const bool stillActive = id.index < m_bodies.size() && m_bodies[id.index].generation == id.generation &&
                                 m_bodies[id.index].activeStamp == m_writeBackStamp;
        if (!stillActive) {
            body.active = false;
            body.linearVelocity = core::Vec3{0.0f, 0.0f, 0.0f};
            body.angularVelocity = core::Vec3{0.0f, 0.0f, 0.0f};
        }
    });

    writeCharacters();
}

void PhysicsSync::writeCharacters()
{
    for (auto& entry : m_characters) {
        const core::InstanceId id = unpackInstance(entry.first);
        CharacterBodyComponent* character = m_scene.characterBodies().find(id);
        PartComponent* part = m_scene.parts().find(id);
        if (character == nullptr || part == nullptr || entry.second.follower)
            continue;

        const physics::CharacterState state = m_backend.characterState(m_world, entry.second.handle);
        part->cframe = state.transform;
        entry.second.written = state.transform;

        const bool grounded = state.ground == physics::CharacterGround::Grounded;
        const core::InstanceId ground =
            state.groundUserData == 0 ? core::InstanceId{} : instanceOf(state.groundUserData);

        // Landing is a transition, not a state: airborne last tick and grounded
        // now. Reading the flag alone would fire it every tick a character
        // stands still.
        if (grounded && !character->grounded) {
            Change change;
            change.kind = ChangeKind::InstanceEvent;
            change.subject = id;
            change.other = ground;
            change.name = m_scene.atoms().intern("Landed");
            m_scene.changes().push(change);
        }

        // **`Touched` is the backend's, all of it** (D028).
        //
        // M6 diffed the surface under the character's feet here, because the
        // rigid-body contact listener could not see a `CharacterVirtual` at all
        // and a checkpoint pad you walk onto is what an obby is made of. That
        // covered the ground and nothing else -- a wall walked into fired
        // nothing -- and it was a second diff for a signal that already had one.
        //
        // Both halves come through `publishContacts` now, from the character's
        // own active contacts. `groundPart` stays because `Landed` above is
        // about ground STATE rather than about contact, and because riding a
        // platform needs to know which one.

        character->grounded = grounded;
        // Swimming and flying are what it IS doing; on foot, whether it is
        // supported.
        character->state = character->mode >= 2 ? character->mode : grounded ? 0 : 1;
        character->groundPart = ground;
        if (RigidBodyComponent* body = m_scene.rigidBodies().find(id); body != nullptr) {
            body->linearVelocity = state.linearVelocity;
            body->active = true;
        }
    }
}

void PhysicsSync::publishContacts()
{
    if (m_quiet) {
        (void)m_backend.drainContacts(m_world);
        return;
    }
    const core::NameAtom touched = m_scene.atoms().intern("Touched");
    const core::NameAtom touchEnded = m_scene.atoms().intern("TouchEnded");
    const core::NameAtom collided = m_scene.atoms().intern("Collided");

    // Both directions, because `Touched` is a fact about each part and a script
    // connects to one of them without knowing which side of the pair it is.
    for (const physics::ContactEvent& event : m_backend.drainContacts(m_world)) {
        const core::InstanceId first = unpackInstance(event.firstUserData);
        const core::InstanceId second = unpackInstance(event.secondUserData);
        if (!m_scene.alive(first) || !m_scene.alive(second))
            continue;

        // **`CanTouch` gates the PAIR**, which is why it is asked here rather
        // than per side below: a signal naming a part that said not to report
        // touches would be that part reporting one on somebody else's handler.
        // The contact was still solved -- what stops is the queueing, which is
        // the cost a world full of scenery is paying for listeners it does not
        // have.
        const RigidBodyComponent* firstBody = m_scene.rigidBodies().find(first);
        const RigidBodyComponent* secondBody = m_scene.rigidBodies().find(second);
        if ((firstBody != nullptr && !firstBody->canTouch) || (secondBody != nullptr && !secondBody->canTouch))
            continue;

        const core::NameAtom name = event.phase == physics::ContactPhase::Began ? touched : touchEnded;

        Change change;
        change.kind = ChangeKind::InstanceEvent;
        change.name = name;

        change.subject = first;
        change.other = second;
        m_scene.changes().push(change);

        change.subject = second;
        change.other = first;
        m_scene.changes().push(change);

        // **What the contact was, for a part that asked** (ADR 0127, N2):
        // after `Touched`, with the normal pointing from the part that hears
        // it into the other one.
        if (event.phase != physics::ContactPhase::Began || !event.detailed)
            continue;
        if (firstBody != nullptr && firstBody->contactDetails)
            m_scene.changes().pushContact(first, collided, ContactNote{second, event.point, event.normal, event.speed});
        if (secondBody != nullptr && secondBody->contactDetails)
            m_scene.changes().pushContact(second, collided,
                                          ContactNote{first, event.point, event.normal * -1.0f, event.speed});
    }
}

bool PhysicsSync::shouldRebase(core::DVec3 focus, f64 threshold) const noexcept
{
    const core::DVec3 offset = focus - m_origin;
    // Squared, so the common answer -- "no" -- costs no square root, and
    // compared per axis so the tolerance is a BOX rather than a sphere: a focus
    // travelling along one axis should rebase at the same distance whichever
    // axis it is.
    return std::abs(offset.x) > threshold || std::abs(offset.y) > threshold || std::abs(offset.z) > threshold;
}

void PhysicsSync::setOrigin(core::DVec3 origin)
{
    if (origin == m_origin) {
        return;
    }
    m_origin = origin;
    m_rebaseCount += 1;
    m_backend.setWorldOrigin(m_world, origin);

    // The mirror's own record of what it last pushed is in ABSOLUTE
    // coordinates, so it survives a rebase untouched -- which is the reason the
    // record was stored that way rather than as whatever the solver held. If it
    // had been local, every part in the world would look like a script write on
    // the next tick and the mirror would push six thousand transforms back down
    // for nothing.
}

void PhysicsSync::mirror()
{
    if (!m_world.valid() || !m_workspace.valid())
        return;
    applyScene();
}

// --- Rollback (ADR 0101) -------------------------------------------------------

namespace {

// "LGSS": a saved simulation, and the version of its layout.
constexpr u32 SimulationMagic = 0x5353474Cu;
constexpr u32 SimulationVersion = 3;

template <class T>
void put(std::vector<u8>& out, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <class T>
[[nodiscard]] bool take(std::span<const u8> bytes, usize& at, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (at > bytes.size() || bytes.size() - at < sizeof(T))
        return false;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

} // namespace

std::vector<core::InstanceId> PhysicsSync::simulatedIds() const
{
    // Every instance with a live body or a character, in id order -- the same
    // order on every machine, because it is the ids' and not a map's.
    std::vector<core::InstanceId> ids;
    for (usize index = 0; index < m_bodies.size(); ++index) {
        const BodyRecord& record = m_bodies[index];
        const core::InstanceId id{static_cast<u32>(index), record.generation};
        if (record.live && m_scene.alive(id))
            ids.push_back(id);
    }
    for (const auto& [key, record] : m_characters) {
        const core::InstanceId id = unpackInstance(key);
        if (record.handle.valid() && m_scene.alive(id))
            ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), [](core::InstanceId a, core::InstanceId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool PhysicsSync::saveSimulation(std::vector<u8>& out) const
{
    out.clear();
    if (!m_world.valid())
        return false;
    std::vector<u8> solver;
    if (!m_backend.saveState(m_world, solver))
        return false;
    put(out, SimulationMagic);
    put(out, SimulationVersion);
    put(out, static_cast<u64>(solver.size()));
    out.insert(out.end(), solver.begin(), solver.end());
    const std::vector<core::InstanceId> ids = simulatedIds();
    put(out, static_cast<u32>(ids.size()));
    for (const core::InstanceId id : ids) {
        put(out, id.index);
        put(out, id.generation);
        const PartComponent* part = m_scene.parts().find(id);
        const RigidBodyComponent* body = m_scene.rigidBodies().find(id);
        const CharacterBodyComponent* character = m_scene.characterBodies().find(id);
        // **A pending twist only where there is one** (ADR 0118): bit 8, so a
        // snapshot of a world with none is the bytes it always was, and one
        // written before twists existed still reads.
        const bool twisted = body != nullptr && !(body->pendingAngularImpulse == core::Vec3{0.0f, 0.0f, 0.0f});
        // **A velocity a script wrote and the mirror has not yet handed over**
        // (ADR 0127, N5): bit 16, and only the bit -- the two velocities are
        // saved either way, and this says which of them the solver is told.
        const bool written = body != nullptr && body->velocityWritten;
        const u8 has = static_cast<u8>((part != nullptr ? 1 : 0) | (body != nullptr ? 2 : 0) |
                                       (character != nullptr ? 4 : 0) | (twisted ? 8 : 0) | (written ? 16 : 0));
        put(out, has);
        if (part != nullptr)
            put(out, part->cframe);
        if (body != nullptr) {
            put(out, body->linearVelocity);
            put(out, body->angularVelocity);
            put(out, body->pendingImpulse);
            put(out, static_cast<u8>(body->active ? 1 : 0));
            // **Until when an anchored part stays kinematic** (audit E7): the
            // tick a script's write moved it, part of what the next ticks do
            // with it, and a restore that kept the later value re-simulated a
            // platform as moving before anything had moved it.
            const bool recorded = id.index < m_bodies.size() && m_bodies[id.index].generation == id.generation;
            put(out, recorded ? m_bodies[id.index].movingUntilTick : u64{0});
            if (twisted)
                put(out, body->pendingAngularImpulse);
        }
        if (character != nullptr) {
            put(out, static_cast<u8>(character->grounded ? 1 : 0));
            put(out, character->state);
            put(out, character->groundPart);
            put(out, character->moveDirection);
            put(out, static_cast<u8>(character->jumpRequested ? 1 : 0));
            put(out, character->verticalVelocity);
            put(out, character->push);
            put(out, character->mode);
        }
    }
    return true;
}

bool PhysicsSync::restoreSimulation(std::span<const u8> bytes)
{
    if (!m_world.valid())
        return false;
    usize at = 0;
    u32 magic = 0;
    u32 version = 0;
    u64 solverBytes = 0;
    if (!take(bytes, at, magic) || !take(bytes, at, version) || !take(bytes, at, solverBytes) ||
        magic != SimulationMagic || version != SimulationVersion || solverBytes > bytes.size() - at)
        return false;
    const std::span<const u8> solver = bytes.subspan(at, static_cast<usize>(solverBytes));
    at += static_cast<usize>(solverBytes);

    // **Read everything before writing anything**, so a refusal leaves the
    // world exactly as it was.
    struct Entry
    {
        core::InstanceId id;
        u8 has = 0;
        core::CFrameD cframe;
        core::Vec3 linear{};
        core::Vec3 angular{};
        core::Vec3 impulse{};
        core::Vec3 twist{};
        u8 active = 0;
        u64 movingUntil = 0;
        u8 grounded = 0;
        i32 state = 0;
        core::InstanceId groundPart;
        core::Vec3 move{};
        u8 jump = 0;
        f32 vertical = 0.0f;
        core::Vec3 push{};
        i32 mode = 0;
    };
    u32 count = 0;
    if (!take(bytes, at, count))
        return false;
    std::vector<Entry> entries;
    for (u32 index = 0; index < count; ++index) {
        Entry entry;
        if (!take(bytes, at, entry.id.index) || !take(bytes, at, entry.id.generation) || !take(bytes, at, entry.has))
            return false;
        if ((entry.has & 1) != 0 && !take(bytes, at, entry.cframe))
            return false;
        if ((entry.has & 2) != 0 &&
            (!take(bytes, at, entry.linear) || !take(bytes, at, entry.angular) || !take(bytes, at, entry.impulse) ||
             !take(bytes, at, entry.active) || !take(bytes, at, entry.movingUntil)))
            return false;
        if ((entry.has & 8) != 0 && ((entry.has & 2) == 0 || !take(bytes, at, entry.twist)))
            return false;
        if ((entry.has & 4) != 0 &&
            (!take(bytes, at, entry.grounded) || !take(bytes, at, entry.state) || !take(bytes, at, entry.groundPart) ||
             !take(bytes, at, entry.move) || !take(bytes, at, entry.jump) || !take(bytes, at, entry.vertical) ||
             !take(bytes, at, entry.push) || !take(bytes, at, entry.mode)))
            return false;
        entries.push_back(entry);
    }
    if (at != bytes.size())
        return false;

    // The same bodies, or nothing: the solver's half would refuse too, but
    // this half is ours to check.
    const std::vector<core::InstanceId> ids = simulatedIds();
    if (ids.size() != entries.size())
        return false;
    for (usize index = 0; index < ids.size(); ++index) {
        if (!(ids[index] == entries[index].id))
            return false;
        const u8 has = static_cast<u8>((m_scene.parts().find(ids[index]) != nullptr ? 1 : 0) |
                                       (m_scene.rigidBodies().find(ids[index]) != nullptr ? 2 : 0) |
                                       (m_scene.characterBodies().find(ids[index]) != nullptr ? 4 : 0));
        if (has != (entries[index].has & 7))
            return false;
    }
    if (!m_backend.restoreState(m_world, solver))
        return false;

    for (const Entry& entry : entries) {
        if (PartComponent* part = m_scene.parts().find(entry.id); part != nullptr) {
            part->cframe = entry.cframe;
            // What the mirror last wrote is what the solver now holds, so the
            // next apply does not teleport what the restore just put back.
            if (entry.id.index < m_bodies.size() && m_bodies[entry.id.index].generation == entry.id.generation)
                m_bodies[entry.id.index].written = entry.cframe;
            if (const auto found = m_characters.find(packInstance(entry.id)); found != m_characters.end())
                found->second.written = entry.cframe;
        }
        if (RigidBodyComponent* body = m_scene.rigidBodies().find(entry.id); body != nullptr) {
            body->linearVelocity = entry.linear;
            body->angularVelocity = entry.angular;
            body->pendingImpulse = entry.impulse;
            body->pendingAngularImpulse = entry.twist;
            body->velocityWritten = (entry.has & 16) != 0;
            body->active = entry.active != 0;
            if (entry.id.index < m_bodies.size() && m_bodies[entry.id.index].generation == entry.id.generation)
                m_bodies[entry.id.index].movingUntilTick = entry.movingUntil;
        }
        if (CharacterBodyComponent* character = m_scene.characterBodies().find(entry.id); character != nullptr) {
            character->grounded = entry.grounded != 0;
            character->state = entry.state;
            character->groundPart = entry.groundPart;
            character->moveDirection = entry.move;
            character->jumpRequested = entry.jump != 0;
            character->verticalVelocity = entry.vertical;
            character->push = entry.push;
            character->mode = entry.mode;
        }
    }
    return true;
}

void PhysicsSync::stepQuietly(f64 fixedDt)
{
    m_quiet = true;
    step(fixedDt);
    m_quiet = false;
}

void PhysicsSync::step(f64 fixedDt)
{
    if (!m_world.valid())
        return;

    // `Workspace` is handed in by the host rather than looked up here: `scene`
    // has no notion of the DataModel root, and the host already resolves it for
    // the renderer. An invalid id means no world root, which means nothing has
    // a body -- and that is exactly the M4.5 defect shape, so the host has a
    // test asserting it resolved (`world_host_tests.cpp`).
    if (!m_workspace.valid())
        return;

    const auto begin = std::chrono::steady_clock::now();
    // **The water's push and drag first** (ADR 0118), onto the pending
    // impulses the mirror is about to apply -- here, so every step takes it:
    // a tick, a rollback's re-simulation, a replica's prediction.
    applyWaterForces(m_scene, m_workspace, fixedDt);
    applyScene();
    const auto applied = std::chrono::steady_clock::now();

    if (const WorkspaceComponent* workspace = m_scene.workspaces().find(m_workspace); workspace != nullptr)
        m_backend.setGravity(m_world, workspace->gravity);

    // **The movers and the springs, as forces** (ADR 0127): after every body
    // and joint is as the scene says, and before the solver takes the step.
    applyMovers(static_cast<f32>(fixedDt));

    m_backend.step(m_world, static_cast<f32>(fixedDt));
    const auto stepped = std::chrono::steady_clock::now();

    writeBack();
    readConstraints(static_cast<f32>(fixedDt));
    // After the writeback, so a driven part follows where its anchor ENDED UP
    // this tick rather than where it was at the start of it.
    resolveWelds();
    resolveAttachments();
    driveRagdolls();
    // And the pose is committed after everything that could have moved a joint,
    // because `commitOverrides` re-runs the forward pass and anything written
    // afterwards would be a frame behind. Nothing sets an override yet; this is
    // where a ragdoll's writes land when one exists.
    if (m_skeleton != nullptr)
        m_skeleton->commitOverrides();
    publishContacts();
    const auto end = std::chrono::steady_clock::now();

    m_timings.apply = std::chrono::duration<f64>(applied - begin).count();
    m_timings.step = std::chrono::duration<f64>(stepped - applied).count();
    m_timings.writeback = std::chrono::duration<f64>(end - stepped).count();
}

void PhysicsSync::setCollisionPoints(core::NameAtom content, std::vector<core::Vec3> points)
{
    const auto at = std::lower_bound(m_collisionPoints.begin(), m_collisionPoints.end(), content,
                                     [](const auto& entry, core::NameAtom key) { return entry.first.id < key.id; });
    // **Counted up on every replacement**, and never reset. It is what tells a
    // body whose hull came from this cloud that the geometry underneath it is
    // not the geometry it was built with -- a question two `ShapeDesc`s could
    // not answer from type and size alone, and could only answer by comparing
    // spans nobody is allowed to keep.
    ++m_collisionRevision;
    if (at != m_collisionPoints.end() && at->first == content) {
        at->second.points = std::move(points);
        at->second.revision = m_collisionRevision;
        return;
    }
    m_collisionPoints.insert(at, {content, CollisionMesh{std::move(points), m_collisionRevision}});
}

} // namespace engine::scene
