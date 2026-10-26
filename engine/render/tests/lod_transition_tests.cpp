// **A mesh changes its level of detail without being seen to** (ADR 0261):
// the band a level keeps, the fade between two, and what is too small to draw.

#include <doctest/doctest.h>

#include "engine/asset/model.h"
#include "engine/core/math.h"
#include "engine/render/lod_transition.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/rhi/backends.h"

using engine::core::f32;
using engine::core::Vec3;
using namespace engine::render;

namespace {

// A level-one error of five centimetres at a thousand pixels a metre: the
// coarse level does from fifty metres.
constexpr f32 PixelsPerUnit = 1000.0f;

struct Fixture
{
    engine::rhi::DeviceResult device = engine::rhi::createNullDevice({.backend = engine::rhi::BackendId::Null});
    MeshCache cache;
    MeshHandle mesh;

    Fixture()
    {
        REQUIRE(device != nullptr);
        REQUIRE_FALSE(cache.create(*device).has_value());
        engine::asset::Mesh model;
        model.vertices.resize(6);
        model.indices = {0, 1, 2, 0, 2, 3, 0, 1, 2};
        model.submeshes.push_back(
            engine::asset::Submesh{.firstIndex = 0, .indexCount = 6, .material = 0, .bounds = {}});
        model.submeshes.push_back(
            engine::asset::Submesh{.firstIndex = 6, .indexCount = 3, .material = 0, .bounds = {}});
        const MeshLodRange ranges[] = {
            {.firstSection = 0, .sectionCount = 1, .error = 0.0f},
            {.firstSection = 1, .sectionCount = 1, .error = 0.05f},
        };
        auto* cmd = device->beginFrame();
        REQUIRE(cmd != nullptr);
        mesh = cache.create(*device, *cmd, model, MeshUsage::Static, nullptr, ranges);
        REQUIRE(mesh.valid());
    }

    ~Fixture() { cache.destroy(*device); }

    // A frame of one thing `distance` down the camera's view, `radius` round.
    [[nodiscard]] RenderWorld frameAt(f32 distance, f32 radius = 1.0f, bool finest = false) const
    {
        RenderWorld world;
        world.materials.push_back(RenderMaterial{});
        DrawItem draw;
        draw.transform = engine::core::translation(Vec3{0.0f, 0.0f, -distance});
        draw.mesh = mesh;
        draw.material = 0;
        draw.boundsCenter = Vec3{0.0f, 0.0f, -distance};
        draw.boundsRadius = radius;
        draw.inCameraFrustum = true;
        draw.motionKey = 7;
        draw.finestLod = finest;
        world.draws.push_back(draw);
        return world;
    }
};

[[nodiscard]] unsigned levelOf(const RenderWorld& world)
{
    REQUIRE(!world.draws.empty());
    return world.draws.front().lodFixed;
}

} // namespace

TEST_CASE("a level holds within a band either side of where it changes")
{
    Fixture fixture;
    LodTransitions lods;
    GraphicsSettings settings;
    const auto step = [&](f32 distance) {
        RenderWorld world = fixture.frameAt(distance);
        (void)lods.apply(world, fixture.cache, PixelsPerUnit, settings, false, 0.0);
        return levelOf(world);
    };
    // Fine near; past fifty the coarse level would do, but not seen a sixth
    // nearer until past 58: it holds.
    CHECK(step(40.0f) == 0);
    CHECK(step(52.0f) == 0);
    CHECK(step(56.0f) == 0);
    CHECK(step(60.0f) == 1);
    // And back: the fine level is wanted inside fifty, but the coarse still
    // does seen a sixth further until inside 43.
    CHECK(step(48.0f) == 1);
    CHECK(step(45.0f) == 1);
    CHECK(step(40.0f) == 0);
}

TEST_CASE("a change of level is a quarter of a second of both, each through the other's holes")
{
    Fixture fixture;
    LodTransitions lods;
    GraphicsSettings settings;
    RenderWorld first = fixture.frameAt(40.0f);
    (void)lods.apply(first, fixture.cache, PixelsPerUnit, settings, true, 0.0);

    RenderWorld changing = fixture.frameAt(70.0f);
    const LodTransitionStats stats = lods.apply(changing, fixture.cache, PixelsPerUnit, settings, true, 1.0);
    CHECK(stats.changed == 1);
    // The moment it changes: all of the old level, none of the new.
    REQUIRE(changing.draws.size() == 2);
    CHECK(static_cast<double>(changing.draws[0].lodFade) == doctest::Approx(2.0));
    CHECK(static_cast<double>(changing.draws[1].lodFade) == doctest::Approx(4.999).epsilon(0.0001));
    // A tenth of a second on.
    RenderWorld fading = fixture.frameAt(70.0f);
    (void)lods.apply(fading, fixture.cache, PixelsPerUnit, settings, true, 1.1);
    REQUIRE(fading.draws.size() == 2);
    const DrawItem& incoming = fading.draws[0];
    const DrawItem& outgoing = fading.draws[1];
    CHECK(incoming.lodFixed == 1);
    CHECK(outgoing.lodFixed == 0);
    // Shares that add to one: the new keeps 0.4 of the dither, the old 0.6.
    CHECK(static_cast<double>(incoming.lodFade) == doctest::Approx(2.4).epsilon(0.001));
    CHECK(static_cast<double>(outgoing.lodFade) == doctest::Approx(4.6).epsilon(0.001));
    // The old level is the camera's alone: the shadow is the new one's.
    CHECK(incoming.castShadow);
    CHECK_FALSE(outgoing.castShadow);

    // Done: one draw, no fade.
    RenderWorld done = fixture.frameAt(70.0f);
    (void)lods.apply(done, fixture.cache, PixelsPerUnit, settings, true, 1.3);
    REQUIRE(done.draws.size() == 1);
    CHECK(done.draws[0].lodFixed == 1);
    CHECK(done.draws[0].lodFade == 0.0f);
}

TEST_CASE("what is smaller than two pixels is not drawn, and the view distance moves where that is")
{
    Fixture fixture;
    GraphicsSettings settings;
    {
        // A centimetre at forty metres: half a pixel.
        LodTransitions lods;
        RenderWorld world = fixture.frameAt(40.0f, 0.01f);
        const LodTransitionStats stats = lods.apply(world, fixture.cache, PixelsPerUnit, settings, true, 0.0);
        CHECK(stats.tooSmall == 1);
        CHECK_FALSE(world.draws[0].inCameraFrustum);
    }
    {
        // Four times the view distance: half a pixel is drawn, and as itself
        // -- no fade, so it stays in its instanced run.
        settings.viewDistance = 4.0f;
        LodTransitions lods;
        RenderWorld world = fixture.frameAt(40.0f, 0.01f);
        (void)lods.apply(world, fixture.cache, PixelsPerUnit, settings, true, 0.0);
        CHECK(world.draws[0].inCameraFrustum);
        CHECK(world.draws[0].lodFade == 0.0f);
    }
}

TEST_CASE("a mesh part asked to be drawn at its finest is, at any distance")
{
    Fixture fixture;
    LodTransitions lods;
    GraphicsSettings settings;
    RenderWorld world = fixture.frameAt(500.0f, 1.0f, true);
    (void)lods.apply(world, fixture.cache, PixelsPerUnit, settings, true, 0.0);
    CHECK(levelOf(world) == 0);
}
