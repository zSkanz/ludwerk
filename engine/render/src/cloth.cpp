#include "engine/render/cloth.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include "engine/core/profile.h"
#include "engine/render/animation.h"
#include "engine/render/draw_poses.h"
#include "engine/render/render_world.h"
#include "engine/render/spring_bones.h"
#include "engine/scene/world.h"

namespace engine::render {

namespace {

using core::CFrameD;
using core::DVec3;
using core::Vec3;

[[nodiscard]] core::u64 keyOf(core::InstanceId id) noexcept
{
    return static_cast<core::u64>(id.index) | (static_cast<core::u64>(id.generation) << 32);
}

// **A cloak weighs about what heavy wool does**: four tenths of a kilogram a
// square metre. What a wind's push is set against; gravity pulls everything
// alike.
constexpr f32 ArealDensity = 0.4f;
// How hard air pushes a square metre of cloth that crosses it at a metre a
// second, in newtons -- still air, for a cloth carried through it, as much as
// a wind. **A ten metre wind straight through a cape pushes it as hard as
// gravity pulls it**, which is what a spring chain's wind does (ADR 0194): a
// cape changed from one to the other is blown the same way.
constexpr f32 AirDrag = ArealDensity * 9.81f / 10.0f;
// A frame this long is a hitch, not motion: the cloth is put where its
// animation has it.
constexpr f32 Hitch = 0.25f;
// The longest step: a slower frame is stepped as this, and its cloth falls
// behind by what is left rather than being flung by a step too long to solve.
constexpr f32 LongestStep = 1.0f / 30.0f;
// The parts near a cloth are asked again this often, or when it has moved
// this far; one no cloth asked for in twice that is let go of.
constexpr core::u64 AskEvery = 30;
constexpr f32 AskBeyond = 0.5f;

[[nodiscard]] core::u64 mix(core::u64 hash, core::u64 value) noexcept
{
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

[[nodiscard]] core::u64 bitsOf(f32 value) noexcept
{
    core::u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] core::u64 signatureOf(const physics::ShapeDesc& shape) noexcept
{
    core::u64 hash = static_cast<core::u64>(shape.type);
    hash = mix(hash, bitsOf(shape.size.x));
    hash = mix(hash, bitsOf(shape.size.y));
    hash = mix(hash, bitsOf(shape.size.z));
    hash = mix(hash, shape.points.size());
    return mix(hash, shape.pointsRevision);
}

// **Stiffness as the solver takes it**: how much an edge gives, in metres a
// newton. At 1 none; lower, a knitted stretch.
[[nodiscard]] f32 stretchComplianceOf(f32 stiffness) noexcept
{
    const f32 give = 1.0f - std::clamp(stiffness, 0.0f, 1.0f);
    return give * give * 1.0e-2f;
}

// And a fold: at nought, no resistance; leather near 1.
[[nodiscard]] f32 bendComplianceOf(f32 bend) noexcept
{
    const f32 held = std::clamp(bend, 0.0f, 1.0f);
    return held <= 0.0f ? 1.0e3f : 2.0e-3f / (held * held);
}

// "How much of its motion it loses each second", as a rate.
[[nodiscard]] f32 dampingOf(f32 damping) noexcept
{
    return -std::log(1.0f - std::clamp(damping, 0.0f, 0.99f));
}

// Where a carrier's part puts its mesh, and how much the mesh is stretched:
// a mesh that wears another's pose is drawn in its leader's place (ADR 0201).
[[nodiscard]] Vec3 stretchOf(const scene::World& world, core::InstanceId carrier) noexcept
{
    const scene::PartComponent* part = world.parts().find(carrier);
    const scene::MeshPartComponent* mesh = world.meshParts().find(carrier);
    if (part == nullptr || mesh == nullptr || !(mesh->meshSize.x > 0.0f) || !(mesh->meshSize.y > 0.0f) ||
        !(mesh->meshSize.z > 0.0f))
        return Vec3{1.0f, 1.0f, 1.0f};
    return Vec3{part->size.x / mesh->meshSize.x, part->size.y / mesh->meshSize.y, part->size.z / mesh->meshSize.z};
}

// A turn whose up is `along`.
[[nodiscard]] core::Mat3 upAlong(Vec3 along) noexcept
{
    const Vec3 up = core::normalize(along);
    const Vec3 hint = std::abs(up.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    const Vec3 back = core::normalize(core::cross(hint, up));
    const Vec3 right = core::cross(up, back);
    core::Mat3 turn;
    turn.m[0][0] = right.x;
    turn.m[0][1] = right.y;
    turn.m[0][2] = right.z;
    turn.m[1][0] = up.x;
    turn.m[1][1] = up.y;
    turn.m[1][2] = up.z;
    turn.m[2][0] = back.x;
    turn.m[2][1] = back.y;
    turn.m[2][2] = back.z;
    return turn;
}

// Each vertex's normal from the faces it is a corner of, weighted by their
// areas: as modelled, and as the cloth now is.
void normalsOf(std::span<const Vec3> positions, std::span<const core::u32> triangles, std::vector<Vec3>& out)
{
    out.assign(positions.size(), Vec3{});
    for (core::usize at = 0; at + 2 < triangles.size(); at += 3) {
        const core::u32 a = triangles[at];
        const core::u32 b = triangles[at + 1];
        const core::u32 c = triangles[at + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size())
            continue;
        const Vec3 face = core::cross(positions[b] - positions[a], positions[c] - positions[a]);
        out[a] = out[a] + face;
        out[b] = out[b] + face;
        out[c] = out[c] + face;
    }
    for (Vec3& normal : out)
        normal = core::normalize(normal);
}

} // namespace

ClothSystem::~ClothSystem()
{
    clear();
}

void ClothSystem::clear() noexcept
{
    if (physics_ != nullptr && world_.valid()) {
        // The world takes its bodies with it.
        physics_->destroyWorld(world_);
    }
    world_ = physics::WorldHandle{};
    cloths_.clear();
    colliders_.clear();
    obstacles_.clear();
}

void ClothSystem::release(Cloth& cloth) noexcept
{
    if (physics_ != nullptr && world_.valid() && cloth.body.valid())
        physics_->destroySoftBody(world_, cloth.body);
    cloth.body = physics::SoftBodyHandle{};
}

ClothSystem::Body& ClothSystem::keep(std::map<core::u64, Body>& bodies, core::u64 key, const physics::ShapeDesc& shape,
                                     const CFrameD& place, bool moving)
{
    const core::u64 signature = mix(signatureOf(shape), moving ? 1u : 0u);
    Body& kept = bodies[key];
    if (kept.body.valid() && kept.signature != signature) {
        physics_->destroyBody(world_, kept.body);
        kept.body = physics::BodyHandle{};
    }
    if (!kept.body.valid()) {
        physics::BodyDesc desc;
        desc.shape = shape;
        desc.transform = place;
        desc.motion = moving ? physics::MotionType::Kinematic : physics::MotionType::Static;
        kept.body = physics_->createBody(world_, desc);
        kept.signature = signature;
    }
    else if (!(kept.place == place)) {
        physics_->setBodyTransform(world_, kept.body, place);
    }
    kept.place = place;
    kept.frame = frame_;
    return kept;
}

void ClothSystem::build(Cloth& cloth, const scene::World& world, AnimationSystem& animation, const MeshLibrary& meshes,
                        core::InstanceId id, Vec3 stretch)
{
    const scene::ClothComponent& settings = *world.cloths().find(id);
    const MeshLibrary::Entry& entry = *meshes.find(cloth.content);
    const auto count = static_cast<core::u32>(entry.positions.size());

    // **One particle where several vertices are one point**: a mesh splits a
    // vertex for each normal and each UV seam it sits on, and a cloth torn
    // along its seams is not a cloth. Numbered in the order the mesh first
    // names them.
    std::vector<core::u32> order(count);
    for (core::u32 vertex = 0; vertex < count; ++vertex)
        order[vertex] = vertex;
    const auto before = [&entry](core::u32 a, core::u32 b) {
        const Vec3 p = entry.positions[a];
        const Vec3 q = entry.positions[b];
        if (p.x != q.x)
            return p.x < q.x;
        if (p.y != q.y)
            return p.y < q.y;
        if (p.z != q.z)
            return p.z < q.z;
        return a < b;
    };
    std::sort(order.begin(), order.end(), before);
    std::vector<core::u32> firstOf(count);
    for (core::u32 at = 0; at < count; ++at) {
        const bool same = at > 0 && entry.positions[order[at]].x == entry.positions[order[at - 1]].x &&
                          entry.positions[order[at]].y == entry.positions[order[at - 1]].y &&
                          entry.positions[order[at]].z == entry.positions[order[at - 1]].z;
        firstOf[order[at]] = same ? firstOf[order[at - 1]] : order[at];
    }
    cloth.particleOf.assign(count, 0);
    cloth.vertexOf.clear();
    std::vector<core::u32> particleAt(count, 0xFFFFFFFFu);
    for (core::u32 vertex = 0; vertex < count; ++vertex) {
        core::u32& particle = particleAt[firstOf[vertex]];
        if (particle == 0xFFFFFFFFu) {
            particle = static_cast<core::u32>(cloth.vertexOf.size());
            cloth.vertexOf.push_back(firstOf[vertex]);
        }
        cloth.particleOf[vertex] = particle;
    }

    // The joints it hangs from.
    const core::u32 rigJoints = animation.jointCount(cloth.meshPart);
    std::vector<core::u8> pins(rigJoints, 0);
    if (const std::string_view pattern = world.atoms().text(settings.pinJoints); !pattern.empty())
        for (core::u32 joint = 0; joint < rigJoints; ++joint)
            pins[joint] = matchesJointPattern(pattern, animation.jointName(cloth.meshPart, joint)) ? 1 : 0;

    physics::SoftBodyDesc desc;
    desc.position = cloth.carried;
    cloth.jointCount = 1;
    const auto particles = static_cast<core::u32>(cloth.vertexOf.size());
    desc.vertices.reserve(particles);
    desc.skin.reserve(particles);
    desc.maxDistance.reserve(particles);
    for (core::u32 particle = 0; particle < particles; ++particle) {
        const core::u32 vertex = cloth.vertexOf[particle];
        const Vec3 at = entry.positions[vertex];
        desc.vertices.push_back(Vec3{at.x * stretch.x, at.y * stretch.y, at.z * stretch.z});
        const asset::SkinVertex& skin = entry.skin[vertex];
        physics::SoftBodySkin joints;
        f32 pinned = 0.0f;
        for (core::u32 slot = 0; slot < 4; ++slot) {
            const auto joint = static_cast<core::u32>(std::max(skin.joints[slot], 0.0f));
            joints.joints[slot] = joint;
            joints.weights[slot] = skin.weights[slot];
            if (skin.weights[slot] > 0.0f) {
                cloth.jointCount = std::max(cloth.jointCount, joint + 1);
                if (joint < rigJoints && pins[joint] != 0)
                    pinned += skin.weights[slot];
            }
        }
        desc.skin.push_back(joints);
        desc.maxDistance.push_back(pinned >= 0.5f ? 0.0f : std::max(settings.maxDistance, 0.0f));
    }

    // Its faces over its particles, each once: a cloth modelled with a back
    // as well as a front has every face twice.
    std::vector<std::array<core::u32, 3>> seen;
    seen.reserve(entry.triangles.size() / 3);
    f32 area = 0.0f;
    for (core::usize at = 0; at + 2 < entry.triangles.size(); at += 3) {
        if (entry.triangles[at] >= count || entry.triangles[at + 1] >= count || entry.triangles[at + 2] >= count)
            continue;
        const std::array<core::u32, 3> face{cloth.particleOf[entry.triangles[at]],
                                            cloth.particleOf[entry.triangles[at + 1]],
                                            cloth.particleOf[entry.triangles[at + 2]]};
        if (face[0] == face[1] || face[1] == face[2] || face[0] == face[2])
            continue;
        std::array<core::u32, 3> sorted = face;
        std::sort(sorted.begin(), sorted.end());
        seen.push_back(sorted);
        desc.triangles.insert(desc.triangles.end(), face.begin(), face.end());
    }
    {
        // Keep the first of each, by where it was in the mesh.
        std::vector<core::u32> faceOrder(seen.size());
        for (core::u32 at = 0; at < faceOrder.size(); ++at)
            faceOrder[at] = at;
        std::stable_sort(faceOrder.begin(), faceOrder.end(),
                         [&seen](core::u32 a, core::u32 b) { return seen[a] < seen[b]; });
        std::vector<core::u8> keepFace(seen.size(), 1);
        for (core::usize at = 1; at < faceOrder.size(); ++at)
            if (seen[faceOrder[at]] == seen[faceOrder[at - 1]])
                keepFace[faceOrder[at]] = 0;
        std::vector<core::u32> unique;
        unique.reserve(desc.triangles.size());
        for (core::usize face = 0; face < keepFace.size(); ++face) {
            if (keepFace[face] == 0)
                continue;
            const core::u32 a = desc.triangles[face * 3];
            const core::u32 b = desc.triangles[face * 3 + 1];
            const core::u32 c = desc.triangles[face * 3 + 2];
            unique.insert(unique.end(), {a, b, c});
            area += 0.5f *
                    core::length(core::cross(desc.vertices[b] - desc.vertices[a], desc.vertices[c] - desc.vertices[a]));
        }
        desc.triangles = std::move(unique);
    }

    desc.stretchCompliance = stretchComplianceOf(settings.stiffness);
    desc.bendCompliance = bendComplianceOf(settings.bendStiffness);
    desc.linearDamping = dampingOf(settings.damping);
    desc.gravityFactor = settings.gravityScale;
    desc.vertexRadius = std::max(settings.thickness, 0.0f);
    desc.mass = std::max(area * ArealDensity, 0.01f);
    cloth.body = physics_->createSoftBody(world_, desc);

    cloth.triangles = entry.triangles;
    normalsOf(entry.positions, cloth.triangles, cloth.restNormals);
    const Vec3 extent = entry.bounds.max - entry.bounds.min;
    cloth.size = std::max(core::length(Vec3{extent.x * stretch.x, extent.y * stretch.y, extent.z * stretch.z}), 0.1f);
}

void ClothSystem::update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses,
                         const MeshLibrary& meshes, physics::IPhysics3D* physics, const ClothFrame& frame)
{
    ENG_PROFILE_SCOPE("animation.cloth");
    const auto began = std::chrono::steady_clock::now();
    ++frame_;
    clothsStepped_ = 0;
    verticesStepped_ = 0;
    animation.clearPresentedCloth();
    if (physics != physics_) {
        clear();
        physics_ = physics;
    }
    if (physics_ == nullptr || !frame.enabled || !(frame.seconds > 0.0f) || world.cloths().size() == 0) {
        if (world_.valid())
            clear();
        lastSeconds_ = 0.0;
        return;
    }
    if (!world_.valid()) {
        physics::WorldDesc desc;
        desc.maxBodies = 1024;
        world_ = physics_->createWorld(desc);
        if (!world_.valid())
            return;
    }
    physics_->setGravity(world_, frame.gravity);
    const bool hitch = frame.seconds >= Hitch;

    struct Placed
    {
        core::u64 key = 0;
        core::InstanceId meshPart{};
        CFrameD place{};
        Vec3 stretch{};
    };
    std::vector<Placed> placed;

    world.cloths().forEach([&](core::InstanceId id, const scene::ClothComponent& settings) {
        if (!settings.enabled || !world.alive(id))
            return;
        const core::InstanceId meshPart = world.parentOf(id);
        const scene::MeshPartComponent* mesh = meshPart.valid() ? world.meshParts().find(meshPart) : nullptr;
        if (mesh == nullptr || !animation.seenLately(meshPart))
            return;
        const MeshLibrary::Entry* entry = meshes.find(mesh->meshContent);
        if (entry == nullptr || entry->skin.empty() || entry->skin.size() != entry->positions.size() ||
            entry->positions.size() > kClothMaxVertices || entry->triangles.empty())
            return;
        const core::InstanceId carrier = animation.drawnWith(meshPart);
        const CFrameD place = poses.part(carrier);
        if (core::length(core::toVec3(place.position - frame.camera)) > frame.maxDistance)
            return;
        const Vec3 stretch = stretchOf(world, carrier);

        core::u64 signature = mix(keyOf(meshPart), static_cast<core::u64>(mesh->meshContent.id));
        signature = mix(signature, entry->positions.size());
        for (const f32 value : {settings.maxDistance, settings.stiffness, settings.bendStiffness, settings.damping,
                                settings.gravityScale, settings.thickness, stretch.x, stretch.y, stretch.z})
            signature = mix(signature, bitsOf(value));
        signature = mix(signature, settings.pinJoints.id);
        signature = mix(signature, animation.jointCount(meshPart));

        Cloth& cloth = cloths_[keyOf(id)];
        bool snap = hitch;
        if (!cloth.body.valid() || cloth.signature != signature) {
            release(cloth);
            cloth = Cloth{};
            cloth.meshPart = meshPart;
            cloth.content = mesh->meshContent;
            cloth.signature = signature;
            cloth.carried = place.position;
            build(cloth, world, animation, meshes, id, stretch);
            snap = true;
        }
        if (!cloth.body.valid())
            return;
        // Further in one frame than twice its own size is not a run: it was
        // put somewhere else, and is put there whole.
        if (core::length(core::toVec3(place.position - cloth.carried)) > std::max(cloth.size * 2.0f, 1.0f))
            snap = true;
        cloth.carried = place.position;
        cloth.frame = frame_;

        // Its joints as the frame draws them, from the carrier's place, with
        // the stretch taken out of what the vertices were given in.
        const Pose* pose = animation.drawnPose(meshPart);
        const core::Mat4 carry = core::toRenderMatrixScaled(place, place.position, stretch);
        const core::Mat4 unstretch = core::scaling(Vec3{1.0f / stretch.x, 1.0f / stretch.y, 1.0f / stretch.z});
        joints_.resize(cloth.jointCount);
        for (core::u32 joint = 0; joint < cloth.jointCount; ++joint) {
            const core::Mat4 palette =
                pose != nullptr && joint < pose->palette.size() ? pose->palette[joint] : core::Mat4{};
            joints_[joint] = carry * palette * unstretch;
        }
        physics_->skinSoftBody(world_, cloth.body, place.position, joints_, snap);

        physics::SoftBodyWind air;
        air.velocity = scene::windAt(frame.wind, core::toVec3(place.position), frame.time) *
                       std::max(settings.windInfluence, 0.0f);
        air.drag = AirDrag;
        physics_->blowSoftBody(world_, cloth.body, air);

        // **The colliders it is kept out of**: its own mesh's, and, on a mesh
        // that wears another's pose, those of the mesh it follows.
        const f32 scale = (stretch.x + stretch.y + stretch.z) / 3.0f;
        const std::array<core::InstanceId, 2> owners{meshPart, carrier};
        for (core::usize which = 0; which < (carrier == meshPart ? 1u : 2u); ++which) {
            const core::InstanceId owner = owners[which];
            for (core::InstanceId child = world.firstChild(owner); child.valid(); child = world.nextSibling(child)) {
                const scene::SpringColliderComponent* collider = world.springColliders().find(child);
                if (collider == nullptr)
                    continue;
                CFrameD model;
                const core::i32 joint = animation.findJoint(owner, world.atoms().text(collider->jointName));
                if (joint >= 0)
                    (void)animation.drawnJointModel(owner, static_cast<core::u32>(joint), model);
                const Vec3 lower = model.rotation * collider->offset + core::toVec3(model.position);
                const Vec3 upper = model.rotation * (collider->offset + Vec3{0.0f, collider->length, 0.0f}) +
                                   core::toVec3(model.position);
                const auto inWorld = [&](Vec3 at) {
                    return place.position +
                           core::toDVec3(place.rotation * Vec3{at.x * stretch.x, at.y * stretch.y, at.z * stretch.z});
                };
                const DVec3 from = inWorld(lower);
                const DVec3 to = inWorld(upper);
                const f32 radius = std::max(collider->radius * scale, 1.0e-3f);
                const Vec3 along = core::toVec3(to - from);
                const f32 length = core::length(along);
                physics::ShapeDesc shape;
                CFrameD where;
                where.position = from + core::toDVec3(along * 0.5f);
                if (length < 1.0e-4f) {
                    shape.type = physics::ShapeType::Sphere;
                    shape.size = Vec3{radius * 2.0f, radius * 2.0f, radius * 2.0f};
                }
                else {
                    shape.type = physics::ShapeType::Capsule;
                    shape.size = Vec3{radius * 2.0f, length + radius * 2.0f, radius * 2.0f};
                    where.rotation = upAlong(along);
                }
                keep(colliders_, keyOf(child), shape, where, true);
            }
        }

        // **And the anchored parts near it**, asked of the host's world now and
        // then: a wall it is pressed against, a floor it drags on. **Not its
        // own body**: the model it is part of is what its colliders describe,
        // and a body's hull -- a block, rounded by the solver's margin --
        // stands a few centimetres into a cape modelled a few centimetres off
        // the back, which threw the cape up over the shoulders.
        if (settings.collidesWithWorld && frame.obstacles) {
            core::InstanceId own = meshPart;
            for (core::InstanceId up = world.parentOf(meshPart); up.valid(); up = world.parentOf(up))
                if (world.models().find(up) != nullptr)
                    own = up;
            const auto ofItsOwn = [&](core::InstanceId part) {
                if (part == meshPart || part == carrier)
                    return true;
                for (core::InstanceId up = world.parentOf(part); up.valid(); up = world.parentOf(up))
                    if (up == own)
                        return true;
                return false;
            };
            if (cloth.askedFrame == 0 || frame_ - cloth.askedFrame >= AskEvery ||
                core::length(core::toVec3(place.position - cloth.askedAt)) > AskBeyond) {
                near_.clear();
                CFrameD box;
                box.position = place.position;
                const f32 reach = cloth.size * 2.0f + 1.0f;
                frame.obstacles(box, Vec3{reach, reach, reach}, near_);
                for (const ClothObstacle& obstacle : near_)
                    if (!ofItsOwn(obstacle.part))
                        keep(obstacles_, keyOf(obstacle.part), obstacle.shape, obstacle.place, false).part =
                            obstacle.part;
                cloth.askedAt = place.position;
                cloth.askedFrame = frame_;
            }
        }

        placed.push_back(Placed{keyOf(id), meshPart, place, stretch});
        ++clothsStepped_;
        verticesStepped_ += static_cast<core::u32>(entry->positions.size());
    });

    // What was not reached this frame is forgotten, so it comes back from
    // where its animation has it.
    for (auto it = cloths_.begin(); it != cloths_.end();) {
        if (it->second.frame != frame_) {
            release(it->second);
            it = cloths_.erase(it);
        }
        else {
            ++it;
        }
    }
    const auto letGo = [this](std::map<core::u64, Body>& bodies, core::u64 age) {
        for (auto it = bodies.begin(); it != bodies.end();) {
            if (frame_ - it->second.frame > age) {
                if (it->second.body.valid())
                    physics_->destroyBody(world_, it->second.body);
                it = bodies.erase(it);
            }
            else {
                ++it;
            }
        }
    };
    letGo(colliders_, 0);
    letGo(obstacles_, AskEvery * 2);
    for (auto& [key, obstacle] : obstacles_) {
        const scene::PartComponent* part = world.parts().find(obstacle.part);
        if (part != nullptr && !(part->cframe == obstacle.place)) {
            physics_->setBodyTransform(world_, obstacle.body, part->cframe);
            obstacle.place = part->cframe;
        }
    }

    if (placed.empty()) {
        lastSeconds_ = std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
        return;
    }
    if (!hitch) {
        ENG_PROFILE_SCOPE("cloth.step");
        physics_->step(world_, std::min(frame.seconds, LongestStep));
    }

    // **Back into each mesh's own space**, as how far each vertex moved from
    // where it was modelled: a row a vertex, which the vertex stage adds.
    ENG_PROFILE_SCOPE("cloth.present");
    std::vector<GpuMorphDelta> rows;
    std::vector<Vec3> moved;
    for (const Placed& one : placed) {
        Cloth& cloth = cloths_[one.key];
        const scene::MeshPartComponent* mesh = world.meshParts().find(one.meshPart);
        const MeshLibrary::Entry* entry = mesh != nullptr ? meshes.find(mesh->meshContent) : nullptr;
        if (entry == nullptr || entry->positions.size() != cloth.particleOf.size())
            continue;
        physics_->softBodyVertices(world_, cloth.body, one.place.position, particles_);
        if (particles_.size() != cloth.vertexOf.size())
            continue;
        const core::Mat3 back = core::transpose(one.place.rotation);
        const Vec3 unstretch{1.0f / one.stretch.x, 1.0f / one.stretch.y, 1.0f / one.stretch.z};
        moved.resize(entry->positions.size());
        core::AABB bounds;
        for (core::usize vertex = 0; vertex < moved.size(); ++vertex) {
            const Vec3 local = back * particles_[cloth.particleOf[vertex]];
            moved[vertex] = Vec3{local.x * unstretch.x, local.y * unstretch.y, local.z * unstretch.z};
            core::expand(bounds, moved[vertex]);
        }
        normalsOf(moved, cloth.triangles, normals_);
        rows.resize(moved.size());
        for (core::usize vertex = 0; vertex < moved.size(); ++vertex) {
            const Vec3 delta = moved[vertex] - entry->positions[vertex];
            rows[vertex].position[0] = delta.x;
            rows[vertex].position[1] = delta.y;
            rows[vertex].position[2] = delta.z;
            rows[vertex].normal = packMorphNormal(normals_[vertex] - cloth.restNormals[vertex]);
        }
        animation.presentCloth(one.meshPart, rows, bounds);
    }
    lastSeconds_ = std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
}

} // namespace engine::render
