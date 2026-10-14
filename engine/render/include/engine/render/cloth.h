// **The cloth of a world, a frame at a time** (ADR 0214): every `Cloth`'s mesh
// a soft body of its vertices, carried by the pose its mesh is drawn with,
// stepped in a physics world of its own, and handed back to the renderer as
// how far each vertex moved from the mesh as it was modelled.
//
// Picture only, as the spring chains are (ADR 0194). It reads the drawn pose
// and the drawn place of each mesh, and the anchored parts near a cloth as a
// host reports them; it writes nothing the simulation reads, and its physics
// world is never the game's -- a cape cannot push a crate. A world with no
// window never steps one.
#pragma once

#include <functional>
#include <map>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/physics/physics.h"
#include "engine/render/morph.h"
#include "engine/scene/wind.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class AnimationSystem;
class DrawPoses;
class MeshLibrary;

// A mesh with more vertices is drawn as its animation has it: a cape is a few
// hundred, a skirt a couple of thousand, and a whole body is not a cloth.
inline constexpr core::u32 kClothMaxVertices = 8192;
// And what a frame draws of cloth at most, in vertices: about sixteen capes
// of the largest, and two hundred of an ordinary one. A cloth past it is drawn
// as its animation has it.
inline constexpr core::u32 kMaxClothRows = 131072;

// An anchored part near a cloth, as it collides.
struct ClothObstacle
{
    core::InstanceId part{};
    physics::ShapeDesc shape{};
    core::CFrameD place{};
};

// What one frame tells the cloths.
struct ClothFrame
{
    // Seconds since the frame before.
    core::f32 seconds = 0.0f;
    // Where the picture is taken from, and how far from it a cloth is still
    // stepped. Past it a cloth is drawn as its animation has it.
    core::DVec3 camera{};
    core::f32 maxDistance = 60.0f;
    // Off at a quality level, or on a machine, where cloth does not fit.
    bool enabled = true;
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    scene::WindSettings wind{};
    // Seconds, for the wind's gusts.
    core::f32 time = 0.0f;
    // **The anchored parts inside a box**, for a cloth that collides with the
    // world: the host asks its own physics world, which this never touches.
    // Unset, a cloth collides with its colliders alone.
    std::function<void(const core::CFrameD& box, core::Vec3 size, std::vector<ClothObstacle>& out)> obstacles;
};

class ClothSystem
{
public:
    ClothSystem() = default;
    ~ClothSystem();
    ClothSystem(const ClothSystem&) = delete;
    ClothSystem& operator=(const ClothSystem&) = delete;

    // A frame: every cloth that is on, seen and near is stepped and presented
    // to `animation`; every other one is forgotten, so it starts from where its
    // animation has it when it comes back. `physics` makes the cloths' own
    // world; null, there is no cloth.
    void update(const scene::World& world, AnimationSystem& animation, const DrawPoses& poses,
                const MeshLibrary& meshes, physics::IPhysics3D* physics, const ClothFrame& frame);

    // Every cloth forgotten, and the cloths' world with them: a scene changed,
    // or the backend that made it is going away.
    void clear() noexcept;

    // How many cloths the last `update` stepped, the vertices of their meshes,
    // and the seconds it took.
    [[nodiscard]] core::u32 clothsStepped() const noexcept { return clothsStepped_; }
    [[nodiscard]] core::u32 verticesStepped() const noexcept { return verticesStepped_; }
    [[nodiscard]] core::f64 lastSeconds() const noexcept { return lastSeconds_; }

private:
    struct Cloth
    {
        core::InstanceId meshPart{};
        core::NameAtom content{};
        // What it was built from: its settings and its stretch. Another is a
        // new soft body.
        core::u64 signature = 0;
        physics::SoftBodyHandle body{};
        // The mesh's vertex for each particle, and the particle for each vertex:
        // a vertex split for its normals or its UVs is one particle.
        std::vector<core::u32> vertexOf;
        std::vector<core::u32> particleOf;
        // The mesh's own triangles, over its vertices, and each vertex's normal
        // as modelled: what a vertex's normal now is worked out from, and from.
        std::vector<core::u32> triangles;
        std::vector<core::Vec3> restNormals;
        // How many joints the skin names: what a skinning gives.
        core::u32 jointCount = 1;
        // Its reach, for what counts as a jump.
        core::f32 size = 1.0f;
        core::DVec3 carried{};
        core::u64 frame = 0;
        // The world near it, asked again when it has moved this far from where
        // it was last asked, or a second has passed.
        core::DVec3 askedAt{};
        core::u64 askedFrame = 0;
    };

    struct Body
    {
        physics::BodyHandle body{};
        core::u64 signature = 0;
        core::u64 frame = 0;
        // An anchored part's: where it stands, followed each frame, since a
        // door a script swings is anchored too.
        core::InstanceId part{};
        core::CFrameD place{};
    };

    void build(Cloth& cloth, const scene::World& world, AnimationSystem& animation, const MeshLibrary& meshes,
               core::InstanceId id, core::Vec3 stretch);
    void release(Cloth& cloth) noexcept;
    // A collider or an anchored part, kept in the cloths' world while some
    // cloth is near it: placed where it is now, made again when its shape
    // changes.
    Body& keep(std::map<core::u64, Body>& bodies, core::u64 key, const physics::ShapeDesc& shape,
               const core::CFrameD& place, bool moving);

    physics::IPhysics3D* physics_ = nullptr;
    physics::WorldHandle world_{};
    std::map<core::u64, Cloth> cloths_;
    std::map<core::u64, Body> colliders_;
    std::map<core::u64, Body> obstacles_;
    core::u64 frame_ = 0;
    core::u32 clothsStepped_ = 0;
    core::u32 verticesStepped_ = 0;
    core::f64 lastSeconds_ = 0.0;
    // Scratch, kept so a frame does not allocate.
    std::vector<core::Mat4> joints_;
    std::vector<core::Vec3> particles_;
    std::vector<core::Vec3> normals_;
    std::vector<ClothObstacle> near_;
};

} // namespace engine::render
