// Particles (F2): what an emitter makes, and when -- on the frame, from a seed.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/render/particles.h"
#include "engine/render/render_world.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::render;

namespace {

struct ParticleFixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId folderClass = classes.registerClass({.name = atoms.intern("Folder")});
    scene::ClassId partClass = classes.registerClass({.name = atoms.intern("Part")});
    scene::ClassId emitterClass = classes.registerClass({.name = atoms.intern("ParticleEmitter")});
    scene::World world{classes, enums, atoms, 1234u};
    core::InstanceId root;
    core::InstanceId part;
    core::InstanceId emitter;
    ParticleSystem system;

    ParticleFixture()
    {
        root = world.create(folderClass);
        part = world.create(partClass);
        world.parts().add(part, scene::PartComponent{});
        world.parts().find(part)->cframe.position = core::DVec3{0.0, 10.0, 0.0};
        world.parts().find(part)->size = core::Vec3{0.0f, 0.0f, 0.0f};
        REQUIRE_FALSE(world.setParent(part, root).has_value());
        emitter = world.create(emitterClass);
        world.particleEmitters().add(emitter, scene::ParticleEmitterComponent{});
        REQUIRE_FALSE(world.setParent(emitter, part).has_value());
    }

    scene::ParticleEmitterComponent& config() { return *world.particleEmitters().find(emitter); }

    void run(int frames, double dt = 1.0 / 60.0)
    {
        for (int at = 0; at < frames; ++at)
            system.update(world, root, dt);
    }
};

} // namespace

TEST_CASE("a stream is born at its rate, whatever the frame rate")
{
    ParticleFixture fixture;
    fixture.config().rate = 30.0f;
    fixture.config().lifetime = 100.0f;
    fixture.run(60);
    CHECK(fixture.system.liveCount() == 30);

    // The same second cut into twice as many frames: the same count, because
    // the fraction a frame owes is carried rather than dropped.
    ParticleFixture fine;
    fine.config().rate = 30.0f;
    fine.config().lifetime = 100.0f;
    fine.run(120, 1.0 / 120.0);
    CHECK(fine.system.liveCount() == 30);
}

TEST_CASE("particles die at the end of their lives, and a disabled emitter only bursts")
{
    ParticleFixture fixture;
    fixture.config().rate = 60.0f;
    fixture.config().lifetime = 0.5f;
    fixture.run(60);
    // Half a second of stream, give or take the tenth of variation each life has.
    CHECK(fixture.system.liveCount() >= 26);
    CHECK(fixture.system.liveCount() <= 34);

    fixture.config().enabled = false;
    fixture.run(60);
    CHECK(fixture.system.liveCount() == 0);

    fixture.config().emitted += 25;
    fixture.run(1);
    CHECK(fixture.system.liveCount() == 25);
    // Seen once is spawned once.
    fixture.run(1);
    CHECK(fixture.system.liveCount() == 25);
}

TEST_CASE("they fly along the emitter's up, fall with acceleration, and are drawn back to front")
{
    ParticleFixture fixture;
    fixture.config().enabled = false;
    fixture.config().speed = 10.0f;
    fixture.config().spreadAngle = 0.0f;
    fixture.config().lifetime = 5.0f;
    fixture.config().emitted = 3;
    fixture.run(1);
    fixture.run(30);

    RenderWorld drawn;
    drawn.camera.origin = core::DVec3{0.0, 0.0, 0.0};
    fixture.system.append(drawn);
    REQUIRE(drawn.particles.size() == 3);
    // Straight up from the part, a jet with no spread: above where they began.
    for (const RenderParticle& particle : drawn.particles) {
        CHECK(static_cast<double>(particle.position.y) > 10.0);
        CHECK(static_cast<double>(particle.position.x) == doctest::Approx(0.0).epsilon(1e-3));
    }
    for (std::size_t at = 1; at < drawn.particles.size(); ++at)
        CHECK(drawn.particles[at - 1].distance >= drawn.particles[at].distance);

    // Gravity turns them round.
    fixture.config().acceleration = core::Vec3{0.0f, -40.0f, 0.0f};
    fixture.run(60);
    RenderWorld later;
    fixture.system.append(later);
    REQUIRE(later.particles.size() == 3);
    CHECK(static_cast<double>(later.particles[0].position.y) < static_cast<double>(drawn.particles[0].position.y));
}

TEST_CASE("the wind carries the particles set to drift with it, and only those")
{
    // ADR 0115: a still jet goes straight up; with the wind on and drift on,
    // it leans downwind; with drift off, the wind is nothing to it.
    const auto lean = [](bool drift, core::Vec3 wind) {
        ParticleFixture fixture;
        fixture.world.workspaces().add(fixture.root, scene::WorkspaceComponent{});
        fixture.world.workspaces().find(fixture.root)->globalWind = wind;
        fixture.config().enabled = false;
        fixture.config().speed = 5.0f;
        fixture.config().spreadAngle = 0.0f;
        fixture.config().lifetime = 5.0f;
        fixture.config().windAffectsDrift = drift;
        fixture.config().emitted = 1;
        fixture.run(60);
        RenderWorld drawn;
        fixture.system.append(drawn);
        REQUIRE(drawn.particles.size() == 1);
        return static_cast<double>(drawn.particles[0].position.x);
    };
    CHECK(lean(true, core::Vec3{6.0f, 0.0f, 0.0f}) == doctest::Approx(6.0).epsilon(0.05));
    CHECK(lean(false, core::Vec3{6.0f, 0.0f, 0.0f}) == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(lean(true, core::Vec3{}) == doctest::Approx(0.0).epsilon(1e-3));
}

TEST_CASE("the same emitter, the same frames: the same particles")
{
    ParticleFixture first;
    ParticleFixture second;
    for (ParticleFixture* fixture : {&first, &second}) {
        fixture->config().spreadAngle = 90.0f;
        fixture->config().rate = 50.0f;
        fixture->run(45);
    }
    RenderWorld a;
    RenderWorld b;
    first.system.append(a);
    second.system.append(b);
    REQUIRE(a.particles.size() == b.particles.size());
    REQUIRE_FALSE(a.particles.empty());
    for (std::size_t at = 0; at < a.particles.size(); ++at) {
        CHECK(a.particles[at].position.x == b.particles[at].position.x);
        CHECK(a.particles[at].position.z == b.particles[at].position.z);
    }
}

TEST_CASE("an emitter that leaves the world takes its particles, and one under nothing makes none")
{
    ParticleFixture fixture;
    fixture.config().lifetime = 100.0f;
    fixture.run(30);
    REQUIRE(fixture.system.liveCount() > 0);
    REQUIRE_FALSE(fixture.world.setParent(fixture.emitter, core::InstanceId{}).has_value());
    fixture.run(1);
    CHECK(fixture.system.liveCount() == 0);
    CHECK(fixture.system.emitterCount() == 0);
}

TEST_CASE("a stream over its cap is thinned evenly, where it pulsed")
{
    // 3000 a second living two seconds is six thousand at once, and an emitter
    // holds 4096: it filled, stopped, and began again as the first ones died.
    using render::ParticleSystem;
    const core::f32 capped = ParticleSystem::effectiveRate(3000.0f, 2.0f);
    CHECK(capped < 3000.0f);
    // What it sustains: the longest-lived of them (a tenth over) never past the cap.
    CHECK(static_cast<double>(capped) * 2.0 * 1.1 <= static_cast<double>(ParticleSystem::MaxPerEmitter) + 1.0);
    CHECK(static_cast<double>(capped) * 2.0 * 1.1 > static_cast<double>(ParticleSystem::MaxPerEmitter) - 8.0);
    // A stream under the cap is what was asked for.
    CHECK(ParticleSystem::effectiveRate(100.0f, 2.0f) == 100.0f);
    CHECK(ParticleSystem::effectiveRate(1800.0f, 2.0f) == 1800.0f);
    // And nothing is born of no rate or no life.
    CHECK(ParticleSystem::effectiveRate(0.0f, 2.0f) == 0.0f);
    CHECK(ParticleSystem::effectiveRate(100.0f, 0.0f) == 0.0f);
}

TEST_CASE("ADR 0160: a flipbook steps on a loop, once over a life, or holds a frame of its own")
{
    scene::ParticleEmitterComponent config;
    config.flipbookColumns = 4;
    config.flipbookRows = 2;
    config.flipbookFramerate = 10.0f;
    const core::u32 frames = ParticleSystem::flipbookFrames(config);
    CHECK(frames == 8);
    // Loop: ten a second, round and round, from the frame it was born on.
    config.flipbookMode = 0;
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 0.0f, 0.0f) == 0);
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 0.35f, 0.1f) == 3);
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 0.95f, 0.9f) == 1);
    // Over its life: every frame once, the last held at the end.
    config.flipbookMode = 1;
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 5.0f, 0.0f) == 0);
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 5.0f, 0.5f) == 4);
    CHECK(ParticleSystem::flipbookFrame(config, frames, 0, 5.0f, 1.0f) == 7);
    // Random: the frame it was given, whatever its age.
    config.flipbookMode = 2;
    CHECK(ParticleSystem::flipbookFrame(config, frames, 5, 0.0f, 0.0f) == 5);
    CHECK(ParticleSystem::flipbookFrame(config, frames, 5, 3.0f, 0.9f) == 5);
    // Past sixteen a side is sixteen.
    config.flipbookColumns = 99;
    config.flipbookRows = 1;
    CHECK(ParticleSystem::flipbookFrames(config) == 16);
}

TEST_CASE("ADR 0160: curves over life multiply the start and end, and a spin turns each particle")
{
    ParticleFixture fixture;
    fixture.config().rate = 0.0f;
    fixture.config().lifetime = 1.0f;
    fixture.config().speed = 0.0f;
    fixture.config().size = 2.0f;
    fixture.config().sizeEnd = 2.0f;
    fixture.config().transparency = 0.0f;
    fixture.config().transparencyEnd = 0.0f;
    fixture.config().color = core::Color3{1.0f, 0.5f, 0.25f};
    fixture.config().colorEnd = core::Color3{1.0f, 0.5f, 0.25f};
    fixture.config().rotation = 90.0f;
    fixture.config().rotationSpeed = 180.0f;
    fixture.config().sizeOverLife.keypoints = {core::NumberKeypoint{0.0f, 0.0f, 0.0f},
                                               core::NumberKeypoint{1.0f, 1.0f, 0.0f}};
    fixture.config().transparencyOverLife.keypoints = {core::NumberKeypoint{0.0f, 0.5f, 0.0f},
                                                       core::NumberKeypoint{1.0f, 0.5f, 0.0f}};
    fixture.config().colorOverLife.keypoints = {core::ColorKeypoint{0.0f, core::Color3{0.5f, 1.0f, 1.0f}},
                                                core::ColorKeypoint{1.0f, core::Color3{0.5f, 1.0f, 1.0f}}};
    fixture.config().emitted = 1;
    // Born, then half a second old.
    fixture.run(1);
    fixture.run(30);
    RenderWorld out;
    fixture.system.append(out);
    REQUIRE(out.particles.size() == 1);
    const RenderParticle& drawn = out.particles.front();
    // Half its life: half of two metres.
    CHECK(drawn.size == doctest::Approx(1.0).epsilon(0.15));
    // Half see-through, laid over solid.
    CHECK(drawn.color[3] == doctest::Approx(0.5).epsilon(0.01));
    // The colour times the curve's.
    CHECK(drawn.color[0] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(drawn.color[1] == doctest::Approx(0.5).epsilon(0.01));
    // A quarter turn at birth and half a turn a second: about 180 degrees.
    CHECK(drawn.rotation == doctest::Approx(3.14159265).epsilon(0.05));
    // Drawn as its shape: no picture was named.
    CHECK_FALSE(drawn.texture.valid());
}

TEST_CASE("ADR 0160: an emitter that asks for no spread is born as it was before there was any")
{
    // The spreads draw from the emitter's generator only when they are asked
    // for, so every emitter made before them is the same particles in the
    // same places -- the property a golden image needs.
    ParticleFixture plain;
    ParticleFixture turned;
    for (ParticleFixture* fixture : {&plain, &turned}) {
        fixture->config().rate = 40.0f;
        fixture->config().spreadAngle = 30.0f;
    }
    turned.config().rotation = 45.0f;
    turned.config().rotationSpeed = 90.0f;
    plain.run(60);
    turned.run(60);
    RenderWorld a;
    RenderWorld b;
    plain.system.append(a);
    turned.system.append(b);
    REQUIRE(a.particles.size() == b.particles.size());
    REQUIRE_FALSE(a.particles.empty());
    for (std::size_t at = 0; at < a.particles.size(); ++at) {
        CHECK(a.particles[at].position.x == b.particles[at].position.x);
        CHECK(a.particles[at].position.y == b.particles[at].position.y);
        CHECK(a.particles[at].position.z == b.particles[at].position.z);
    }
}

namespace {

// A slope of terrain under a fixture's emitter: the ground rises a metre every
// four along +x, through the origin.
core::InstanceId slopeUnder(ParticleFixture& fixture)
{
    const scene::ClassId terrainClass = fixture.classes.registerClass({.name = fixture.atoms.intern("Terrain")});
    const core::InstanceId terrain = fixture.world.create(terrainClass);
    scene::TerrainComponent ground;
    ground.field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 64.0f});
    constexpr core::u32 Columns = 64;
    std::vector<float> heights(static_cast<std::size_t>(Columns) * Columns);
    for (core::u32 z = 0; z < Columns; ++z) {
        for (core::u32 x = 0; x < Columns; ++x)
            heights[static_cast<std::size_t>(z) * Columns + x] = (static_cast<float>(x) - 32.0f) * 0.25f;
    }
    (void)asset::writeHeights(ground.field, -32, -32, Columns, heights, 1);
    fixture.world.terrains().add(terrain, std::move(ground));
    REQUIRE_FALSE(fixture.world.setParent(terrain, fixture.root).has_value());
    return terrain;
}

// The ground's height at (x, z), as the particles ask it.
double groundAt(ParticleFixture& fixture, core::InstanceId terrain, double x, double z)
{
    const std::optional<float> height = asset::heightAt(fixture.world.terrains().find(terrain)->field, x, z);
    REQUIRE(height.has_value());
    return static_cast<double>(*height);
}

} // namespace

TEST_CASE("ADR 0160: sparks that fall on a slope of terrain bounce, and settle on it")
{
    ParticleFixture fixture;
    const core::InstanceId terrain = slopeUnder(fixture);
    fixture.config().rate = 0.0f;
    fixture.config().lifetime = 30.0f;
    fixture.config().speed = 2.0f;
    fixture.config().spreadAngle = 40.0f;
    fixture.config().acceleration = core::Vec3{0.0f, -9.81f, 0.0f};
    fixture.config().size = 0.2f;
    fixture.config().sizeEnd = 0.2f;
    fixture.config().transparencyEnd = 0.0f;
    fixture.config().collision = 2;
    fixture.config().bounce = 0.5f;
    fixture.config().friction = 0.5f;
    fixture.config().emitted = 40;

    // A second in: they have hit the ground and come back up off it.
    fixture.run(1);
    core::u32 bounced = 0;
    for (int frame = 0; frame < 120; ++frame) {
        fixture.run(1);
        bounced += fixture.system.lastCollisions();
    }
    CHECK(bounced >= 40);
    // Ten seconds in: every one of them rests on the slope, a radius over it,
    // and none has fallen through.
    fixture.run(600);
    RenderWorld drawn;
    fixture.system.append(drawn);
    REQUIRE(drawn.particles.size() == 40);
    for (const RenderParticle& particle : drawn.particles) {
        const double ground = groundAt(fixture, terrain, static_cast<double>(particle.position.x),
                                       static_cast<double>(particle.position.z));
        CAPTURE(particle.position.x);
        CHECK(static_cast<double>(particle.position.y) >= ground + 0.1 - 0.05);
        CHECK(static_cast<double>(particle.position.y) <= ground + 0.1 + 0.6);
    }
    // And they have stopped moving: the same places a second later.
    fixture.run(60);
    RenderWorld later;
    fixture.system.append(later);
    REQUIRE(later.particles.size() == drawn.particles.size());
    double moved = 0.0;
    for (std::size_t at = 0; at < later.particles.size(); ++at)
        moved = std::max(
            moved, static_cast<double>(std::abs(later.particles[at].position.y - drawn.particles[at].position.y)));
    CHECK(moved < 0.25);
}

TEST_CASE("ADR 0160: a particle that kills on a hit is gone when it lands, and one that sticks stays where it did")
{
    ParticleFixture killed;
    (void)slopeUnder(killed);
    killed.config().rate = 0.0f;
    killed.config().lifetime = 30.0f;
    killed.config().speed = 0.0f;
    killed.config().acceleration = core::Vec3{0.0f, -9.81f, 0.0f};
    killed.config().collision = 2;
    killed.config().collisionResponse = 2;
    killed.config().emitted = 10;
    killed.run(2);
    CHECK(killed.system.liveCount() == 10);
    // Ten metres up: on the ground well inside three seconds.
    killed.run(180);
    CHECK(killed.system.liveCount() == 0);

    ParticleFixture stuck;
    const core::InstanceId terrain = slopeUnder(stuck);
    stuck.config().rate = 0.0f;
    stuck.config().lifetime = 30.0f;
    stuck.config().speed = 3.0f;
    stuck.config().spreadAngle = 60.0f;
    stuck.config().acceleration = core::Vec3{0.0f, -9.81f, 0.0f};
    stuck.config().size = 0.2f;
    stuck.config().sizeEnd = 0.2f;
    stuck.config().transparencyEnd = 0.0f;
    stuck.config().collision = 2;
    stuck.config().collisionResponse = 1;
    stuck.config().emitted = 10;
    stuck.run(240);
    RenderWorld landed;
    stuck.system.append(landed);
    REQUIRE(landed.particles.size() == 10);
    stuck.run(60);
    RenderWorld later;
    stuck.system.append(later);
    REQUIRE(later.particles.size() == 10);
    for (std::size_t at = 0; at < 10; ++at) {
        CHECK(later.particles[at].position.x == landed.particles[at].position.x);
        CHECK(later.particles[at].position.y == landed.particles[at].position.y);
        const double ground = groundAt(stuck, terrain, static_cast<double>(landed.particles[at].position.x),
                                       static_cast<double>(landed.particles[at].position.z));
        CHECK(static_cast<double>(landed.particles[at].position.y) == doctest::Approx(ground + 0.1).epsilon(0.05));
    }
}

TEST_CASE(
    "ADR 0160: on the CPU, what is seen is a ray against the parts -- and a particle with no collision asks nothing")
{
    // A floor at y = 4, as the host's ray would find it.
    core::u32 asked = 0;
    const auto floor = [&asked](core::DVec3 from, core::Vec3 delta, core::DVec3& hit, core::Vec3& normal) {
        ++asked;
        const double to = from.y + static_cast<double>(delta.y);
        if (from.y < 4.0 || to > 4.0 || delta.y >= 0.0f)
            return false;
        const double t = (from.y - 4.0) / (from.y - to);
        hit = core::DVec3{from.x + static_cast<double>(delta.x) * t, 4.0, from.z + static_cast<double>(delta.z) * t};
        normal = core::Vec3{0.0f, 1.0f, 0.0f};
        return true;
    };

    ParticleFixture fixture;
    fixture.system.setRaycast(floor);
    fixture.config().rate = 0.0f;
    fixture.config().lifetime = 30.0f;
    fixture.config().speed = 0.0f;
    fixture.config().acceleration = core::Vec3{0.0f, -9.81f, 0.0f};
    fixture.config().size = 0.2f;
    fixture.config().sizeEnd = 0.2f;
    fixture.config().transparencyEnd = 0.0f;
    fixture.config().collision = 1;
    fixture.config().bounce = 0.0f;
    fixture.config().emitted = 5;
    fixture.run(240);
    CHECK(asked > 0);
    RenderWorld drawn;
    fixture.system.append(drawn);
    REQUIRE(drawn.particles.size() == 5);
    for (const RenderParticle& particle : drawn.particles)
        CHECK(static_cast<double>(particle.position.y) == doctest::Approx(4.1).epsilon(0.03));

    // No collision asked for: not one ray.
    ParticleFixture plain;
    core::u32 plainAsked = 0;
    plain.system.setRaycast([&plainAsked](core::DVec3, core::Vec3, core::DVec3&, core::Vec3&) {
        ++plainAsked;
        return false;
    });
    plain.config().rate = 60.0f;
    plain.run(120);
    CHECK(plainAsked == 0);
}

TEST_CASE("ADR 0160: an emitter on the GPU keeps no particles here, and asks its buffer for this frame's births")
{
    ParticleFixture fixture;
    fixture.system.setGpuSimulation(true);
    fixture.config().simulation = 1;
    fixture.config().rate = 6000.0f;
    fixture.config().lifetime = 5.0f;
    fixture.run(60);
    // A second at six thousand a second, a hundred a frame.
    CHECK(fixture.system.liveCount() == 0);
    CHECK(fixture.system.gpuEmitterCount() == 1);
    RenderWorld out;
    out.camera.valid = true;
    fixture.system.append(out);
    CHECK(out.particles.empty());
    REQUIRE(out.gpuEmitters.size() == 1);
    const RenderGpuEmitter& asked = out.gpuEmitters.front();
    CHECK(asked.spawn == 100);
    // What it sustains, a tenth over, to the power of two above: 33 000 -> 65 536.
    CHECK(asked.capacity == 65536);
    CHECK(asked.lifetime == 5.0f);
    CHECK(asked.frame.position.y == doctest::Approx(10.0));
    // A burst is this frame's births too, and the buffer holds it.
    fixture.config().emitted = 500;
    fixture.run(1);
    RenderWorld burst;
    fixture.system.append(burst);
    REQUIRE(burst.gpuEmitters.size() == 1);
    CHECK(burst.gpuEmitters.front().spawn == 600);
    // Each update is stepped once, however many views draw it.
    CHECK(burst.gpuEmitters.front().serial == asked.serial + 1);
    RenderWorld again;
    fixture.system.append(again);
    CHECK(again.gpuEmitters.front().serial == burst.gpuEmitters.front().serial);
}

TEST_CASE("ADR 0160: where the device cannot, an emitter set to the GPU is simulated on the CPU")
{
    ParticleFixture fixture;
    fixture.system.setGpuSimulation(false);
    fixture.config().simulation = 1;
    fixture.config().rate = 60.0f;
    fixture.config().lifetime = 10.0f;
    fixture.run(60);
    CHECK(fixture.system.gpuEmitterCount() == 0);
    // One a frame for sixty frames, give or take the fraction a frame carries.
    CHECK(fixture.system.liveCount() >= 59);
    CHECK(fixture.system.liveCount() <= 61);
    RenderWorld out;
    fixture.system.append(out);
    CHECK(out.gpuEmitters.empty());
    CHECK(out.particles.size() == fixture.system.liveCount());
}

TEST_CASE("ADR 0160: the ground map is the terrain's heights round the camera, and what CPU particles land on")
{
    ParticleFixture fixture;
    const core::InstanceId terrain = slopeUnder(fixture);
    fixture.config().rate = 0.0f;
    fixture.config().lifetime = 60.0f;
    fixture.config().speed = 2.0f;
    fixture.config().spreadAngle = 40.0f;
    fixture.config().acceleration = core::Vec3{0.0f, -9.81f, 0.0f};
    fixture.config().size = 0.2f;
    fixture.config().sizeEnd = 0.2f;
    fixture.config().transparencyEnd = 0.0f;
    fixture.config().collision = 2;
    fixture.config().collisionResponse = 1;

    // A view over the origin, and the updates the map takes to fill.
    RenderWorld view;
    view.camera.valid = true;
    fixture.system.append(view);
    fixture.run(static_cast<int>(ParticleSystem::GroundCells / ParticleSystem::GroundRowsPerUpdate) + 1);
    RenderWorld mapped;
    mapped.camera.valid = true;
    fixture.system.append(mapped);
    REQUIRE(mapped.particleGround != nullptr);
    const RenderParticleGround& ground = *mapped.particleGround;
    REQUIRE(ground.cells == ParticleSystem::GroundCells);
    REQUIRE(ground.heights.size() == static_cast<std::size_t>(ground.cells) * ground.cells);
    // Each height is the terrain's at the middle of its cell.
    for (core::u32 row = 10; row < ground.cells; row += 23) {
        for (core::u32 column = 7; column < ground.cells; column += 19) {
            const double x = ground.corner.x + (column + 0.5) * static_cast<double>(ParticleSystem::GroundCellMetres);
            const double z = ground.corner.z + (row + 0.5) * static_cast<double>(ParticleSystem::GroundCellMetres);
            const std::optional<float> height = asset::heightAt(fixture.world.terrains().find(terrain)->field, x, z);
            const float held = ground.heights[static_cast<std::size_t>(row) * ground.cells + column];
            if (height.has_value())
                CHECK(static_cast<double>(held) == doctest::Approx(static_cast<double>(*height)).epsilon(1e-4));
            else
                CHECK(held < -1.0e8f);
        }
    }

    // And sparks stuck on it lie on the terrain, read from the map.
    fixture.config().emitted = 20;
    fixture.run(300);
    RenderWorld landed;
    landed.camera.valid = true;
    fixture.system.append(landed);
    REQUIRE(landed.particles.size() == 20);
    for (const RenderParticle& particle : landed.particles) {
        const double under = groundAt(fixture, terrain, static_cast<double>(particle.position.x),
                                      static_cast<double>(particle.position.z));
        CAPTURE(particle.position.x);
        // The map is a height a metre, read between: within a hand of the field's.
        CHECK(static_cast<double>(particle.position.y) == doctest::Approx(under + 0.1).epsilon(0.08));
    }
}
