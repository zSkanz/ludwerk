// Water (ADR 0118): one surface, and what it does to a part in it.
//
// The floating itself -- a box of half the water's density riding half under,
// a hull that rolls -- needs a real physics step and is held in
// `world_host_tests.cpp`; here are the surface's maths and the forces the
// mirror is handed.
#include <cmath>
#include <doctest/doctest.h>
#include <optional>

#include "engine/asset/terrain.h"
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

// --- Curves, rivers that descend, lakes of any outline (ADR 0146) ------------

namespace {

// The pool's water made a shape along these points.
void along(Pool& pool, core::i32 shape, std::initializer_list<scene::WaterPointComponent> points)
{
    pool.fixture.world.waters().find(pool.water)->shape = shape;
    for (const scene::WaterPointComponent& point : points) {
        const core::InstanceId id = pool.fixture.folder("Point");
        (void)pool.fixture.world.waterPoints().add(id, point);
        REQUIRE_FALSE(pool.fixture.world.setParent(id, pool.water).has_value());
    }
}

} // namespace

TEST_CASE("a river's curve passes through its points and bends smoothly between them (ADR 0146)")
{
    Pool pool;
    pool.fixture.world.waters().find(pool.water)->size = core::Vec3{4.0f, 2.0f, 1.0f};
    along(pool, scene::water_shape::River,
          {{core::Vec3{0.0f, 10.0f, 0.0f}},
           {core::Vec3{20.0f, 8.0f, 0.0f}},
           {core::Vec3{20.0f, 6.0f, 20.0f}},
           {core::Vec3{40.0f, 4.0f, 20.0f}}});
    const scene::WaterCourse course = scene::courseOf(pool.fixture.world, pool.water);
    REQUIRE(course.points.size() == 4);
    const core::Vec3 points[4] = {core::Vec3{0.0f, 10.0f, 0.0f}, core::Vec3{20.0f, 8.0f, 0.0f},
                                  core::Vec3{20.0f, 6.0f, 20.0f}, core::Vec3{40.0f, 4.0f, 20.0f}};
    for (int at = 0; at < 4; ++at) {
        const scene::WaterCourseSample& sample = course.samples[course.points[static_cast<core::usize>(at)]];
        CHECK(sample.position.x == doctest::Approx(static_cast<double>(points[at].x)));
        CHECK(sample.position.y == doctest::Approx(static_cast<double>(points[at].y)));
        CHECK(sample.position.z == doctest::Approx(static_cast<double>(points[at].z)));
    }
    // No corner anywhere: from one stretch of samples to the next it turns by
    // well under what the two right angles of its points are.
    double sharpest = 1.0;
    for (core::usize at = 0; at + 2 < course.samples.size(); ++at) {
        const core::DVec3 a = course.samples[at + 1].position - course.samples[at].position;
        const core::DVec3 b = course.samples[at + 2].position - course.samples[at + 1].position;
        const double la = std::sqrt(a.x * a.x + a.z * a.z);
        const double lb = std::sqrt(b.x * b.x + b.z * b.z);
        REQUIRE(la > 1e-6);
        REQUIRE(lb > 1e-6);
        sharpest = std::min(sharpest, (a.x * b.x + a.z * b.z) / (la * lb));
    }
    // The cosine of forty degrees.
    CHECK(sharpest > 0.76);
    // And it never climbs between two points that descend.
    for (core::usize at = 0; at + 1 < course.samples.size(); ++at)
        CHECK(course.samples[at + 1].position.y <= course.samples[at].position.y + 1e-9);
}

TEST_CASE("a river's surface is as high as its course is, and it runs faster where it drops (ADR 0146)")
{
    Pool pool;
    scene::WaterComponent& water = *pool.fixture.world.waters().find(pool.water);
    water.size = core::Vec3{6.0f, 3.0f, 1.0f};
    // Level for forty metres, then down ten in forty.
    along(pool, scene::water_shape::River,
          {{core::Vec3{0.0f, 20.0f, 0.0f}},
           {core::Vec3{40.0f, 20.0f, 0.0f}, 0.0, 0.0, true},
           {core::Vec3{80.0f, 10.0f, 0.0f}}});

    const scene::WaterHere level = scene::waterHere(pool.fixture.world, pool.water, 20.0, 1.0);
    REQUIRE(level.covered);
    CHECK(level.level == doctest::Approx(20.0));
    CHECK(level.depth == doctest::Approx(3.0));
    CHECK(static_cast<double>(level.flow.x) == doctest::Approx(1.0));

    const scene::WaterHere dropping = scene::waterHere(pool.fixture.world, pool.water, 60.0, -1.0);
    REQUIRE(dropping.covered);
    CHECK(dropping.level == doctest::Approx(15.0).epsilon(0.01));
    // A slope of one in four: twice as fast.
    CHECK(static_cast<double>(dropping.flow.x) == doctest::Approx(2.0).epsilon(0.01));

    // Past its width, and past its end: no river.
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 20.0, 4.0).covered);
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 90.0, 0.0).covered);

    // **What floats is held up at the river's height there**, not at the
    // water's `SurfaceLevel`: a box under the surface at twenty metres up.
    const core::InstanceId log = pool.box(core::DVec3{20.0, 19.0, 0.0}, core::Vec3{1.0f, 1.0f, 1.0f}, 0.5f);
    const core::InstanceId dry = pool.box(core::DVec3{60.0, 19.0, 0.0}, core::Vec3{1.0f, 1.0f, 1.0f}, 0.5f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(log)->pendingImpulse.y > 0.5f);
    // Over where the river has dropped to fifteen: in the air.
    CHECK(pool.fixture.world.rigidBodies().find(dry)->pendingImpulse.y == doctest::Approx(0.0));
}

TEST_CASE("a river is as wide and as deep as its points say (ADR 0146)")
{
    Pool pool;
    pool.fixture.world.waters().find(pool.water)->size = core::Vec3{4.0f, 2.0f, 1.0f};
    along(pool, scene::water_shape::River,
          {{core::Vec3{0.0f, 0.0f, 0.0f}}, {core::Vec3{40.0f, 0.0f, 0.0f}, 12.0, 6.0, false}});
    // The water's own width at the first point, the point's at the second.
    CHECK(scene::waterHere(pool.fixture.world, pool.water, 2.0, 1.5).covered);
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 2.0, 3.0).covered);
    const scene::WaterHere wide = scene::waterHere(pool.fixture.world, pool.water, 38.0, 5.0);
    CHECK(wide.covered);
    CHECK(wide.depth > 5.0);
}

TEST_CASE("a lake is the inside of the curve through its points, level at its surface (ADR 0146)")
{
    Pool pool;
    scene::WaterComponent& water = *pool.fixture.world.waters().find(pool.water);
    water.surfaceLevel = 7.0;
    water.size = core::Vec3{1.0f, 5.0f, 1.0f};
    // Two points: not an outline yet.
    along(pool, scene::water_shape::Lake, {{core::Vec3{-10.0f, 0.0f, -10.0f}}, {core::Vec3{10.0f, 0.0f, -10.0f}}});
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 0.0, -10.0).covered);
    along(pool, scene::water_shape::Lake, {{core::Vec3{10.0f, 99.0f, 10.0f}}, {core::Vec3{-10.0f, 0.0f, 10.0f}}});

    const scene::WaterHere middle = scene::waterHere(pool.fixture.world, pool.water, 0.0, 0.0);
    REQUIRE(middle.covered);
    // Level, whatever its points' own heights.
    CHECK(middle.level == doctest::Approx(7.0));
    CHECK(middle.depth == doctest::Approx(5.0));
    // Rounded: the curve bows out past the straight side, and cuts no corner
    // sharper than its points.
    CHECK(scene::waterHere(pool.fixture.world, pool.water, 0.0, 11.0).covered);
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 0.0, 16.0).covered);
    CHECK_FALSE(scene::waterHere(pool.fixture.world, pool.water, 30.0, 0.0).covered);

    const core::InstanceId log = pool.box(core::DVec3{0.0, 6.5, 0.0}, core::Vec3{1.0f, 1.0f, 1.0f}, 0.5f);
    scene::applyWaterForces(pool.fixture.world, pool.workspace, 0.1);
    CHECK(pool.fixture.world.rigidBodies().find(log)->pendingImpulse.y > 0.5f);
}

TEST_CASE("a water's bed is cut into the ground under it, sloping from its edge to its depth (ADR 0146)")
{
    Pool pool;
    // Flat ground at ten metres.
    const core::InstanceId ground = pool.fixture.folder("Terrain");
    scene::TerrainComponent terrain;
    terrain.field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)asset::fillFlat(terrain.field, core::DVec3{0.0, 0.0, 0.0}, 128.0f, 10.0f, 1);
    terrain.fieldRevision = 1;
    (void)pool.fixture.world.terrains().add(ground, terrain);
    REQUIRE_FALSE(pool.fixture.world.setParent(ground, pool.workspace).has_value());

    // A river along x on that ground: six wide, three deep, a bank of two.
    scene::WaterComponent& water = *pool.fixture.world.waters().find(pool.water);
    water.size = core::Vec3{6.0f, 3.0f, 1.0f};
    water.bankWidth = 2.0;
    along(pool, scene::water_shape::River, {{core::Vec3{-20.0f, 10.0f, 0.5f}}, {core::Vec3{20.0f, 10.0f, 0.5f}}});

    CHECK(scene::carveWaterBed(pool.fixture.world, pool.water, ground) > 100);
    const scene::TerrainComponent& carved = *pool.fixture.world.terrains().find(ground);
    CHECK(carved.fieldRevision == 2);
    const auto top = [&](core::i32 x, core::i32 z) {
        const std::optional<float> height = carved.field.columnTop(x, z);
        REQUIRE(height.has_value());
        return static_cast<double>(*height);
    };
    // Down the middle, its whole depth; half way up the bank, half of it; at
    // the water's edge and past it, the ground as it was.
    CHECK(top(0, 0) == doctest::Approx(7.0).epsilon(0.03));
    CHECK(top(0, 2) == doctest::Approx(8.5).epsilon(0.03));
    CHECK(top(0, 3) == doctest::Approx(10.0).epsilon(0.03));
    CHECK(top(0, 8) == doctest::Approx(10.0).epsilon(0.03));
    // Past its end too.
    CHECK(top(30, 0) == doctest::Approx(10.0).epsilon(0.03));

    // It only ever digs: again, and there is nothing to cut.
    CHECK(scene::carveWaterBed(pool.fixture.world, pool.water, ground) == 0);
    CHECK(carved.fieldRevision == 2);

    // A sea carves nothing.
    water.shape = scene::water_shape::Ocean;
    CHECK(scene::carveWaterBed(pool.fixture.world, pool.water, ground) == 0);
}
