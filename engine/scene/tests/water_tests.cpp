// Water (ADR 0118): one surface, and what it does to a part in it.
//
// The floating itself -- a box of half the water's density riding half under,
// a hull that rolls -- needs a real physics step and is held in
// `world_host_tests.cpp`; here are the surface's maths and the forces the
// mirror is handed.
#include <cmath>
#include <doctest/doctest.h>

#include "engine/scene/components.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"
#include "scene_fixture.h"

using namespace engine;
using engine::scene::testing::Fixture;

namespace {

struct Pool
{
    Fixture fixture;
    core::InstanceId workspace;
    core::InstanceId water;

    Pool()
    {
        workspace = fixture.folder("Workspace");
        scene::WorkspaceComponent space;
        space.gravity = core::Vec3{0.0f, -10.0f, 0.0f};
        (void)fixture.world.workspaces().add(workspace, space);
        water = fixture.folder("Water");
        (void)fixture.world.waters().add(water, scene::WaterComponent{});
        REQUIRE_FALSE(fixture.world.setParent(water, workspace).has_value());
    }

    core::InstanceId wave(double wavelength, double amplitude, double direction, double steepness = 0.0)
    {
        const core::InstanceId id = fixture.folder("Wave");
        scene::WaterWaveComponent wave;
        wave.wavelength = wavelength;
        wave.amplitude = amplitude;
        wave.direction = direction;
        wave.steepness = steepness;
        (void)fixture.world.waterWaves().add(id, wave);
        REQUIRE_FALSE(fixture.world.setParent(id, water).has_value());
        return id;
    }

    core::InstanceId box(core::DVec3 at, core::Vec3 size, float density)
    {
        const core::InstanceId id = fixture.part("Box");
        REQUIRE_FALSE(fixture.world.setParent(id, workspace).has_value());
        scene::PartComponent* part = fixture.world.parts().find(id);
        REQUIRE(part != nullptr);
        part->cframe.position = at;
        part->size = size;
        (void)fixture.world.rigidBodies().add(id, scene::RigidBodyComponent{});
        scene::RigidBodyComponent* body = fixture.world.rigidBodies().find(id);
        REQUIRE(body != nullptr);
        body->anchored = false;
        body->density = density;
        return id;
    }
};

} // namespace

TEST_CASE("the surface's slope is its height's, and the still level is its mean (ADR 0118)")
{
    Pool pool;
    (void)pool.wave(40.0, 0.8, 30.0, 0.6);
    (void)pool.wave(17.0, 0.3, 200.0);
    const scene::WaterSurface surface = scene::surfaceOf(pool.fixture.world, pool.water);
    REQUIRE(surface.count == 2);

    // The slopes are the height's own derivatives.
    const double step = 1e-4;
    for (const auto& [x, z] : {std::pair{3.0, -7.0}, std::pair{-20.5, 11.25}, std::pair{0.0, 0.0}}) {
        const scene::WaterSample at = surface.sample(x, z, 2.5);
        // The height alone is the sample's, by a shorter road.
        CHECK(surface.heightAt(x, z, 2.5) == doctest::Approx(surface.level + at.height).epsilon(1e-12));
        const double dx =
            (surface.sample(x + step, z, 2.5).height - surface.sample(x - step, z, 2.5).height) / (2 * step);
        const double dz =
            (surface.sample(x, z + step, 2.5).height - surface.sample(x, z - step, 2.5).height) / (2 * step);
        CHECK(at.slopeX == doctest::Approx(dx).epsilon(1e-4));
        CHECK(at.slopeZ == doctest::Approx(dz).epsilon(1e-4));

        // The water rises as fast as its surface does, and moves less deeper
        // down.
        const double dt =
            (surface.sample(x, z, 2.5 + step).height - surface.sample(x, z, 2.5 - step).height) / (2 * step);
        CHECK(surface.motion(x, z, 0.0, 2.5).y == doctest::Approx(dt).epsilon(1e-4));
        CHECK(core::length(core::toVec3(surface.motion(x, z, 10.0, 2.5))) <
              core::length(core::toVec3(surface.motion(x, z, 0.0, 2.5))));
    }

    // Averaged along a wave's travel over its length, it is the still level.
    double sum = 0.0;
    constexpr int Samples = 4000;
    for (int at = 0; at < Samples; ++at) {
        const double along = 40.0 * at / Samples;
        sum += surface.sample(along * std::cos(0.5235987755982988), along * std::sin(0.5235987755982988), 0.0).height;
    }
    CHECK(std::abs(sum / Samples) < 0.35);
}

TEST_CASE("a part in the water is pushed up by what it displaces, and one above it is not (ADR 0118)")
{
    Pool pool;
    const core::InstanceId under = pool.box(core::DVec3{0.0, -5.0, 0.0}, core::Vec3{2.0f, 2.0f, 2.0f}, 0.5f);
    const core::InstanceId over = pool.box(core::DVec3{10.0, 5.0, 0.0}, core::Vec3{2.0f, 2.0f, 2.0f}, 0.5f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);

    // Eight cubic metres wholly under water of density one, in gravity of ten,
    // for a tenth of a second.
    CHECK(pool.fixture.world.rigidBodies().find(under)->pendingImpulse.y == doctest::Approx(8.0).epsilon(1e-4));
    CHECK(pool.fixture.world.rigidBodies().find(over)->pendingImpulse.y == doctest::Approx(0.0));

    // Half in, tilted: pushed up by less, and turned.
    const core::InstanceId half = pool.box(core::DVec3{20.0, 0.0, 0.0}, core::Vec3{4.0f, 2.0f, 2.0f}, 0.5f);
    pool.fixture.world.parts().find(half)->cframe.rotation = core::fromAxisAngle(core::Vec3{0.0f, 0.0f, 1.0f}, 0.3f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    const scene::RigidBodyComponent& body = *pool.fixture.world.rigidBodies().find(half);
    CHECK(body.pendingImpulse.y > 0.0f);
    CHECK(body.pendingImpulse.y < 16.0f * 0.1f * 10.0f * 0.75f);
    CHECK(std::abs(body.pendingAngularImpulse.z) > 1e-3f);

    // Not buoyant, anchored: left alone.
    const core::InstanceId stone = pool.box(core::DVec3{30.0, -5.0, 0.0}, core::Vec3{2.0f, 2.0f, 2.0f}, 3.0f);
    pool.fixture.world.rigidBodies().find(stone)->buoyant = false;
    const core::InstanceId pier = pool.box(core::DVec3{40.0, -5.0, 0.0}, core::Vec3{2.0f, 2.0f, 2.0f}, 1.0f);
    pool.fixture.world.rigidBodies().find(pier)->anchored = true;
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(stone)->pendingImpulse.y == doctest::Approx(0.0));
    CHECK(pool.fixture.world.rigidBodies().find(pier)->pendingImpulse.y == doctest::Approx(0.0));

    // Welded to another: placed by the weld, so not floated -- the part it
    // hangs off is.
    const core::InstanceId hull = pool.box(core::DVec3{50.0, -5.0, 0.0}, core::Vec3{2.0f, 2.0f, 2.0f}, 0.5f);
    const core::InstanceId mast = pool.box(core::DVec3{50.0, -3.0, 0.0}, core::Vec3{1.0f, 2.0f, 1.0f}, 0.5f);
    const core::InstanceId weld = pool.fixture.folder("Weld");
    scene::WeldComponent joint;
    joint.part0 = hull;
    joint.part1 = mast;
    (void)pool.fixture.world.welds().add(weld, joint);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(hull)->pendingImpulse.y > 0.0f);
    CHECK(pool.fixture.world.rigidBodies().find(mast)->pendingImpulse.y == doctest::Approx(0.0));
}

TEST_CASE("a lake is where its box is, and a river along its points (ADR 0118)")
{
    Pool pool;
    scene::WaterComponent& lake = *pool.fixture.world.waters().find(pool.water);
    lake.shape = 1;
    lake.position = core::Vec3{10.0f, 0.0f, 0.0f};
    lake.size = core::Vec3{8.0f, 4.0f, 6.0f};
    CHECK(scene::waterCovers(pool.fixture.world, pool.water, 13.0, 2.0));
    CHECK_FALSE(scene::waterCovers(pool.fixture.world, pool.water, 15.0, 0.0));

    lake.shape = 2;
    lake.size = core::Vec3{4.0f, 2.0f, 1.0f};
    for (const core::Vec3 at :
         {core::Vec3{0.0f, 0.0f, 0.0f}, core::Vec3{20.0f, 0.0f, 0.0f}, core::Vec3{20.0f, 0.0f, 20.0f}}) {
        const core::InstanceId id = pool.fixture.folder("Point");
        (void)pool.fixture.world.waterPoints().add(id, scene::WaterPointComponent{at});
        REQUIRE_FALSE(pool.fixture.world.setParent(id, pool.water).has_value());
    }
    core::Vec3 flow{};
    CHECK(scene::waterCovers(pool.fixture.world, pool.water, 10.0, 1.5, &flow));
    CHECK(flow.x == doctest::Approx(1.0));
    CHECK(scene::waterCovers(pool.fixture.world, pool.water, 21.0, 10.0, &flow));
    CHECK(flow.z == doctest::Approx(1.0));
    CHECK_FALSE(scene::waterCovers(pool.fixture.world, pool.water, 10.0, 5.0));
}

TEST_CASE("where a river runs out into a lake at its level, the river carries what is in both (ADR 0118)")
{
    Pool pool;
    scene::WaterComponent& lake = *pool.fixture.world.waters().find(pool.water);
    lake.shape = 1;
    lake.position = core::Vec3{20.0f, 0.0f, 0.0f};
    lake.size = core::Vec3{20.0f, 4.0f, 20.0f};

    // Created after the lake, so it is not the first found.
    const core::InstanceId river = pool.fixture.folder("River");
    scene::WaterComponent course;
    course.shape = 2;
    course.size = core::Vec3{4.0f, 4.0f, 0.0f};
    course.flowSpeed = 3.0;
    (void)pool.fixture.world.waters().add(river, course);
    REQUIRE_FALSE(pool.fixture.world.setParent(river, pool.workspace).has_value());
    for (const core::Vec3 at : {core::Vec3{-10.0f, 0.0f, 0.0f}, core::Vec3{15.0f, 0.0f, 0.0f}}) {
        const core::InstanceId id = pool.fixture.folder("Point");
        (void)pool.fixture.world.waterPoints().add(id, scene::WaterPointComponent{at});
        REQUIRE_FALSE(pool.fixture.world.setParent(id, river).has_value());
    }

    // At rest in the mouth, where both cover it: dragged downstream.
    const core::InstanceId log = pool.box(core::DVec3{12.0, 0.0, 0.0}, core::Vec3{1.0f, 1.0f, 1.0f}, 0.5f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(log)->pendingImpulse.x > 0.0f);

    // Out in the lake, where the river does not reach: nothing carries it.
    const core::InstanceId drift = pool.box(core::DVec3{25.0, 0.0, 5.0}, core::Vec3{1.0f, 1.0f, 1.0f}, 0.5f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(drift)->pendingImpulse.x == doctest::Approx(0.0));
}
