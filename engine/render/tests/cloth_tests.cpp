// A cape that is cloth (ADR 0214): everything between a `Cloth` in the tree and
// the soft body under it, and the rows the vertex stage reads back. What is
// asked is what a cape is judged by -- it hangs from what pins it at the
// length it was made, it rests on the body's colliders and does not pass
// through them, it looks the same at any frame rate, a teleport leaves it at
// rest -- and what it must not do: run where it is switched off.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/physics/backends.h"
#include "engine/render/animation.h"
#include "engine/render/cloth.h"
#include "engine/render/draw_poses.h"
#include "engine/render/render_world.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using core::f32;
using core::u32;
using core::Vec3;

namespace {

// The cape: COLUMNS by ROWS corners from the shoulders down, behind the
// body, drawn on both sides; its top row on `CapePin`, the rest on `Body`.
constexpr u32 Columns = 9;
constexpr u32 Rows = 14;
constexpr f32 Width = 0.62f;
constexpr f32 Top = 1.62f;
constexpr f32 Bottom = 0.32f;
constexpr f32 Back = 0.2f;

render::SkeletonLibrary::Entry rig()
{
    render::SkeletonLibrary::Entry entry;
    asset::Joint body;
    body.name = "Body";
    body.parent = asset::Joint::NoParent;
    entry.joints.push_back(body);
    asset::Joint pin;
    pin.name = "CapePin";
    pin.parent = 0;
    pin.localBind.position = core::DVec3{0.0, static_cast<core::f64>(Top), static_cast<core::f64>(Back)};
    entry.joints.push_back(pin);
    return entry;
}

render::MeshLibrary::Entry cape()
{
    render::MeshLibrary::Entry entry;
    for (const f32 facing : {1.0f, -1.0f}) {
        const auto first = static_cast<u32>(entry.positions.size());
        for (u32 row = 0; row < Rows; ++row) {
            for (u32 column = 0; column < Columns; ++column) {
                entry.positions.push_back(Vec3{Width * (static_cast<f32>(column) / (Columns - 1) - 0.5f),
                                               Top - (Top - Bottom) * static_cast<f32>(row) / (Rows - 1), Back});
                asset::SkinVertex skin;
                skin.joints[0] = row == 0 ? 1.0f : 0.0f;
                skin.weights[0] = 1.0f;
                entry.skin.push_back(skin);
            }
        }
        for (u32 row = 0; row + 1 < Rows; ++row) {
            for (u32 column = 0; column + 1 < Columns; ++column) {
                const u32 a = first + row * Columns + column;
                const u32 b = a + 1;
                const u32 c = a + Columns + 1;
                const u32 d = a + Columns;
                if (facing > 0.0f)
                    entry.triangles.insert(entry.triangles.end(), {a, d, c, a, c, b});
                else
                    entry.triangles.insert(entry.triangles.end(), {a, b, c, a, c, d});
            }
        }
    }
    for (const Vec3& at : entry.positions)
        core::expand(entry.bounds, at);
    return entry;
}

// Where the hem's middle is, in the mesh's own space, as the frame draws it.
[[nodiscard]] Vec3 drawnVertex(const render::MeshLibrary::Entry& entry,
                               const render::AnimationSystem::PresentedCloth& cloth, u32 vertex)
{
    const render::GpuMorphDelta& row = cloth.rows[vertex];
    return entry.positions[vertex] + Vec3{row.position[0], row.position[1], row.position[2]};
}

constexpr u32 HemMiddle = (Rows - 1) * Columns + Columns / 2;

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId instanceClass = scene::InvalidClass;
    scene::ClassId meshPartClass = scene::InvalidClass;

    Fixture()
    {
        scene::ClassDescriptor instance;
        instance.name = atoms.intern("Instance");
        instance.defaultName = instance.name;
        instanceClass = classes.registerClass(instance);

        scene::ClassDescriptor meshPart;
        meshPart.name = atoms.intern("MeshPart");
        meshPart.super = instanceClass;
        meshPart.defaultName = meshPart.name;
        meshPart.attachComponents = [](scene::World& w, core::InstanceId id) {
            w.parts().add(id, scene::PartComponent{});
            w.meshParts().add(id, scene::MeshPartComponent{});
        };
        meshPart.detachComponents = [](scene::World& w, core::InstanceId id) {
            w.meshParts().remove(id);
            w.parts().remove(id);
        };
        meshPartClass = classes.registerClass(meshPart);

        physics = physics::createJoltPhysics();
        REQUIRE(physics != nullptr);
        skeletons.set(content, rig());
        meshes.set(content, cape());
        entry = meshes.find(content);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    scene::World world{classes, enums, atoms, 1u};
    render::SkeletonLibrary skeletons;
    render::MeshLibrary meshes;
    const render::MeshLibrary::Entry* entry = nullptr;
    core::NameAtom content = atoms.intern("asset://models/cape.gltf");
    std::unique_ptr<physics::IPhysics3D> physics;

    // A caped figure at `at`, the cloth's settings as `says`.
    core::InstanceId figure(core::DVec3 at, const scene::ClothComponent& says)
    {
        const core::InstanceId mesh = world.create(meshPartClass);
        world.meshParts().find(mesh)->meshContent = content;
        world.parts().find(mesh)->cframe.position = at;
        const core::InstanceId cloth = world.create(instanceClass);
        (void)world.setParent(cloth, mesh);
        world.cloths().add(cloth, says);
        return mesh;
    }

    // A capsule on the body's joint, as a `SpringCollider` would be.
    void collider(core::InstanceId mesh, Vec3 offset, f32 length, f32 radius)
    {
        const core::InstanceId id = world.create(instanceClass);
        (void)world.setParent(id, mesh);
        scene::SpringColliderComponent says;
        says.jointName = atoms.intern("Body");
        says.offset = offset;
        says.length = length;
        says.radius = radius;
        world.springColliders().add(id, says);
    }

    [[nodiscard]] static scene::ClothComponent pinned(core::AtomTable& atoms)
    {
        scene::ClothComponent says;
        says.pinJoints = atoms.intern("CapePin");
        says.maxDistance = 1.4f;
        says.stiffness = 0.95f;
        says.bendStiffness = 0.15f;
        says.damping = 0.15f;
        return says;
    }

    Vec3 wind{};
    // What the host's world has near a cloth, if anything.
    std::function<void(const core::CFrameD&, Vec3, std::vector<render::ClothObstacle>&)> obstacles;

    // Frames of `seconds` each, `count` of them.
    void frames(render::ClothSystem& cloth, render::AnimationSystem& animation, int count, f32 seconds = 1.0f / 60.0f,
                bool enabled = true)
    {
        render::DrawPoses poses;
        for (int at = 0; at < count; ++at) {
            poses.begin(world, nullptr, 1.0f);
            render::ClothFrame frame;
            frame.seconds = seconds;
            frame.wind.global = wind;
            frame.time = static_cast<f32>(at) * seconds;
            frame.enabled = enabled;
            frame.obstacles = obstacles;
            cloth.update(world, animation, poses, meshes, physics.get(), frame);
        }
    }
};

// The three capsules down a block of a body's back, as the example has them.
void backOf(Fixture& fixture, core::InstanceId mesh)
{
    for (const f32 x : {-0.2f, 0.0f, 0.2f})
        fixture.collider(mesh, Vec3{x, 0.17f, 0.0f}, 1.38f, 0.17f);
}

// How far `point` is inside the capsule from `from` up `length`, of `radius`:
// positive inside.
[[nodiscard]] f32 inside(Vec3 point, Vec3 from, f32 length, f32 radius)
{
    const f32 y = std::clamp(point.y, from.y, from.y + length);
    return radius - core::length(point - Vec3{from.x, y, from.z});
}

} // namespace

TEST_CASE("a cloth cape hangs from its pin at the length it was made, in the frame's rows")
{
    Fixture fixture;
    const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::ClothSystem cloth;
    fixture.frames(cloth, animation, 180);

    CHECK(cloth.clothsStepped() == 1);
    CHECK(cloth.verticesStepped() == Columns * Rows * 2);
    const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
    REQUIRE(drawn != nullptr);
    REQUIRE(drawn->rows.size() == fixture.entry->positions.size());
    // The pinned row is where it was modelled.
    for (u32 column = 0; column < Columns; ++column)
        CHECK(core::length(drawnVertex(*fixture.entry, *drawn, column) - fixture.entry->positions[column]) < 1.0e-3f);
    // Modelled hanging straight down, with nothing to push it, it hangs as it
    // was: the hem under the pin, no longer and not crumpled.
    const Vec3 hem = drawnVertex(*fixture.entry, *drawn, HemMiddle);
    CHECK(std::abs(hem.x) < 0.02f);
    CHECK(std::abs(hem.z - Back) < 0.05f);
    CHECK(static_cast<double>(hem.y) == doctest::Approx(static_cast<double>(Bottom)).epsilon(0.05));
    // Both sides of it are the same cloth: a back vertex is where its front is.
    const u32 back = Columns * Rows + HemMiddle;
    CHECK(core::length(drawnVertex(*fixture.entry, *drawn, back) - hem) < 1.0e-4f);
}

TEST_CASE("a cloth cape rests on the body's colliders and does not pass through them")
{
    Fixture fixture;
    const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
    backOf(fixture, mesh);
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::ClothSystem cloth;
    // A gale from behind, pressing the cape onto the body's back.
    fixture.wind = Vec3{0.0f, 0.0f, -20.0f};
    fixture.frames(cloth, animation, 240);
    const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
    REQUIRE(drawn != nullptr);
    // Every vertex, and ten points along every edge of every face, out of
    // every capsule -- what is drawn is the cloth between the vertices.
    f32 deepest = -1.0f;
    f32 nearest = 1.0f;
    const auto& triangles = fixture.entry->triangles;
    for (core::usize at = 0; at + 2 < triangles.size(); at += 3) {
        for (u32 edge = 0; edge < 3; ++edge) {
            const Vec3 a = drawnVertex(*fixture.entry, *drawn, triangles[at + edge]);
            const Vec3 b = drawnVertex(*fixture.entry, *drawn, triangles[at + (edge + 1) % 3]);
            for (u32 step = 0; step <= 10; ++step) {
                const Vec3 point = a + (b - a) * (static_cast<f32>(step) / 10.0f);
                for (const f32 x : {-0.2f, 0.0f, 0.2f}) {
                    const f32 depth = inside(point, Vec3{x, 0.17f, 0.0f}, 1.38f, 0.17f);
                    deepest = std::max(deepest, depth);
                    nearest = std::min(nearest, -depth);
                }
            }
        }
    }
    MESSAGE("deepest into a collider: ", deepest * 1000.0f, " mm; nearest outside: ", nearest * 1000.0f, " mm");
    // A centimetre is the cloth's own thickness and the capsules' facets
    // between vertices: through is more than that.
    CHECK(deepest < 0.01f);
    // And pressed onto it: the gale has laid it on the back, not left it
    // hanging where it was modelled, two centimetres off.
    CHECK(nearest < 0.03f);
}

TEST_CASE("a cloth cape settles in the same place at 30, 60 and 144 frames a second")
{
    std::vector<Vec3> hems;
    for (const f32 rate : {30.0f, 60.0f, 144.0f}) {
        Fixture fixture;
        const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
        backOf(fixture, mesh);
        render::AnimationSystem animation{fixture.world, fixture.skeletons};
        render::ClothSystem cloth;
        // A steady wind from in front, through the body to the cape, three
        // seconds of it: what lifts a cape off the back. One along the cape
        // would barely stir it, as it barely stirs cloth.
        fixture.wind = Vec3{0.0f, 0.0f, 6.0f};
        fixture.frames(cloth, animation, static_cast<int>(rate * 3.0f), 1.0f / rate);
        const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
        REQUIRE(drawn != nullptr);
        hems.push_back(drawnVertex(*fixture.entry, *drawn, HemMiddle));
        MESSAGE(rate, " frames a second: hem at ", hems.back().x, ", ", hems.back().y, ", ", hems.back().z);
    }
    CHECK(hems[0].z > Back + 0.1f);
    for (const Vec3& hem : hems)
        CHECK(core::length(hem - hems[1]) < 0.03f);
}

TEST_CASE("a cloth cape put somewhere else is there at rest, and one far away or switched off is not stepped")
{
    Fixture fixture;
    const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::ClothSystem cloth;
    fixture.frames(cloth, animation, 60);
    fixture.world.parts().find(mesh)->cframe.position = core::DVec3{40.0, 0.0, 0.0};
    fixture.frames(cloth, animation, 1);
    const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
    REQUIRE(drawn != nullptr);
    // In the mesh's own space it hangs as it did: not flung forty metres.
    const Vec3 hem = drawnVertex(*fixture.entry, *drawn, HemMiddle);
    CHECK(std::abs(hem.x) < 0.02f);
    CHECK(std::abs(hem.z - Back) < 0.05f);

    // Off at the frame's level, nothing is presented: the mesh is drawn as
    // its animation has it.
    fixture.frames(cloth, animation, 1, 1.0f / 60.0f, false);
    CHECK(animation.drawnCloth(mesh) == nullptr);
    CHECK(cloth.clothsStepped() == 0);
    // And switched off on the instance.
    fixture.world.cloths().forEach([](core::InstanceId, scene::ClothComponent& says) { says.enabled = false; });
    fixture.frames(cloth, animation, 1);
    CHECK(animation.drawnCloth(mesh) == nullptr);
}

TEST_CASE("what a frame of cloth costs: one hero, and twenty")
{
    for (const u32 heroes : {1u, 20u}) {
        Fixture fixture;
        std::vector<core::InstanceId> figures;
        for (u32 hero = 0; hero < heroes; ++hero) {
            figures.push_back(fixture.figure(
                core::DVec3{static_cast<core::f64>(hero % 5) * 2.0, 0.0, static_cast<core::f64>(hero / 5) * 2.0},
                Fixture::pinned(fixture.atoms)));
            backOf(fixture, figures.back());
        }
        render::AnimationSystem animation{fixture.world, fixture.skeletons};
        render::ClothSystem cloth;
        fixture.wind = Vec3{6.0f, 0.0f, 0.0f};
        fixture.frames(cloth, animation, 60);
        core::f64 total = 0.0;
        const int measured = 120;
        for (int frame = 0; frame < measured; ++frame) {
            fixture.frames(cloth, animation, 1);
            total += cloth.lastSeconds();
        }
        CHECK(cloth.clothsStepped() == heroes);
        MESSAGE(heroes, " caped heroes (", heroes * Columns * Rows * 2, " vertices): ", total / measured * 1000.0,
                " ms a frame");
    }
}

TEST_CASE("a cloth cape pressed against a wall lies on it, out of it, and still")
{
    Fixture fixture;
    const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
    backOf(fixture, mesh);
    // A wall behind the figure, its face twenty centimetres behind the cape
    // as modelled, and a wind blowing the cape onto it.
    const core::InstanceId wall = fixture.world.create(fixture.instanceClass);
    constexpr f32 Face = Back + 0.2f;
    fixture.obstacles = [wall](const core::CFrameD&, Vec3, std::vector<render::ClothObstacle>& out) {
        render::ClothObstacle obstacle;
        obstacle.part = wall;
        obstacle.shape.type = physics::ShapeType::Box;
        obstacle.shape.size = Vec3{3.0f, 3.0f, 0.3f};
        obstacle.place.position = core::DVec3{0.0, 1.5, static_cast<core::f64>(Face) + 0.15};
        out.push_back(obstacle);
    };
    fixture.wind = Vec3{0.0f, 0.0f, 8.0f};
    render::AnimationSystem animation{fixture.world, fixture.skeletons};
    render::ClothSystem cloth;
    fixture.frames(cloth, animation, 180);

    // Then two seconds watched: every vertex out of the wall, the hem on it,
    // and nothing that moves back and forth from one frame to the next by
    // more than a millimetre -- what would show as a cloth that flickers.
    std::vector<Vec3> before;
    std::vector<Vec3> now;
    std::vector<Vec3> after;
    f32 deepest = -1.0f;
    f32 shake = 0.0f;
    for (int frame = 0; frame < 120; ++frame) {
        fixture.frames(cloth, animation, 1);
        const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
        REQUIRE(drawn != nullptr);
        before = std::move(now);
        now = std::move(after);
        after.clear();
        for (u32 vertex = 0; vertex < Columns * Rows; ++vertex) {
            after.push_back(drawnVertex(*fixture.entry, *drawn, vertex));
            deepest = std::max(deepest, after.back().z - Face);
        }
        if (!before.empty())
            for (u32 vertex = 0; vertex < after.size(); ++vertex)
                shake = std::max(shake, core::length(after[vertex] - now[vertex] * 2.0f + before[vertex]));
    }
    MESSAGE("into the wall: ", deepest * 1000.0f, " mm; the most a vertex shook: ", shake * 1000.0f, " mm");
    CHECK(deepest < 0.0f);
    CHECK(after[HemMiddle].z > Face - 0.05f);
    CHECK(shake < 0.001f);
}

TEST_CASE("a cloth cape in the wind moves smoothly from one frame to the next")
{
    // Still air, a steady wind and a gusting one: the second difference of
    // every vertex over three seconds -- how far it is from where its last
    // two frames say it would be. A cloth that flickers is one whose vertices
    // go back and forth from frame to frame; a cloth that flutters moves
    // further, and smoothly.
    for (const f32 gusts : {-1.0f, 0.0f, 0.6f}) {
        Fixture fixture;
        const core::InstanceId mesh = fixture.figure(core::DVec3{}, Fixture::pinned(fixture.atoms));
        backOf(fixture, mesh);
        render::AnimationSystem animation{fixture.world, fixture.skeletons};
        render::ClothSystem cloth;
        render::DrawPoses poses;
        std::vector<Vec3> before;
        std::vector<Vec3> now;
        std::vector<Vec3> after;
        f32 shake = 0.0f;
        for (int at = 0; at < 300; ++at) {
            poses.begin(fixture.world, nullptr, 1.0f);
            render::ClothFrame frame;
            frame.seconds = 1.0f / 60.0f;
            frame.wind.global = gusts < 0.0f ? Vec3{} : Vec3{0.0f, 0.0f, 5.0f};
            frame.wind.gusts = std::max(gusts, 0.0f);
            frame.time = static_cast<f32>(at) / 60.0f;
            cloth.update(fixture.world, animation, poses, fixture.meshes, fixture.physics.get(), frame);
            const render::AnimationSystem::PresentedCloth* drawn = animation.drawnCloth(mesh);
            REQUIRE(drawn != nullptr);
            before = std::move(now);
            now = std::move(after);
            after.clear();
            for (u32 vertex = 0; vertex < Columns * Rows; ++vertex)
                after.push_back(drawnVertex(*fixture.entry, *drawn, vertex));
            if (at >= 120)
                for (u32 vertex = 0; vertex < after.size(); ++vertex)
                    shake = std::max(shake, core::length(after[vertex] - now[vertex] * 2.0f + before[vertex]));
        }
        MESSAGE("gusts ", gusts, ": the most a vertex shook from one frame to the next: ", shake * 1000.0f, " mm");
        CHECK(shake < 0.002f);
    }
}
