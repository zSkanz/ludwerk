// Foliage (ADR 0116): which tiles grow, what regrows after an edit, and what
// the frame is handed.
//
// **Against the null device**, judged by the system's own counts: the cull and
// the draw are the renderer's, and a real device is what the meadow example
// is for.
#include <doctest/doctest.h>

#include "engine/asset/terrain.h"
#include "engine/render/foliage.h"
#include "engine/render/render_world.h"
#include "engine/rhi/backends.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::render;

namespace {

struct FoliageFixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId workspaceClass;
    scene::ClassId terrainClass;
    scene::ClassId layerClass;
    scene::ClassId meshClass;
    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    rhi::ICmdList* cmd = nullptr;
    scene::World world;
    core::InstanceId root;
    core::InstanceId terrain;
    core::InstanceId layer;
    FoliageSystem foliage;

    FoliageFixture()
        : workspaceClass(
              classes.registerClass({.name = atoms.intern("Workspace"), .defaultName = atoms.intern("Workspace")})),
          terrainClass(
              classes.registerClass({.name = atoms.intern("Terrain"), .defaultName = atoms.intern("Terrain")})),
          layerClass(classes.registerClass(
              {.name = atoms.intern("FoliageLayer"), .defaultName = atoms.intern("FoliageLayer")})),
          meshClass(
              classes.registerClass({.name = atoms.intern("FoliageMesh"), .defaultName = atoms.intern("FoliageMesh")})),
          world(classes, enums, atoms, 1234u)
    {
        REQUIRE(device != nullptr);
        cmd = device->beginFrame();
        root = world.create(workspaceClass);
        terrain = world.create(terrainClass);
        scene::TerrainComponent ground;
        ground.field =
            asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 64.0f});
        (void)asset::fillFlat(ground.field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
        world.terrains().add(terrain, std::move(ground));
        REQUIRE(world.setParent(terrain, root) == std::nullopt);

        layer = world.create(layerClass);
        scene::FoliageLayerComponent rules;
        rules.density = 1.0f;
        rules.minSpacing = 0.0f;
        rules.drawDistance = 60.0f;
        world.foliageLayers().add(layer, rules);
        REQUIRE(world.setParent(layer, terrain) == std::nullopt);

        const core::InstanceId mesh = world.create(meshClass);
        scene::FoliageMeshComponent grass;
        grass.mesh = atoms.intern("asset://models/grass.gltf");
        world.foliageMeshes().add(mesh, grass);
        REQUIRE(world.setParent(mesh, layer) == std::nullopt);

        foliage.setSettings({.density = 1.0f, .shadowDistance = 30.0f, .growthsPerSync = 1000});
        foliage.setFocus(core::DVec3{0.0, 2.0, 0.0});
    }

    ~FoliageFixture() { foliage.destroy(*device); }

    FoliageFixture(const FoliageFixture&) = delete;
    FoliageFixture& operator=(const FoliageFixture&) = delete;

    void sync() { foliage.sync(*device, *cmd, world); }
};

} // namespace

TEST_CASE("the tiles within the draw distance grow, once")
{
    FoliageFixture fixture;
    fixture.sync();
    const FoliageStats grown = fixture.foliage.stats();
    // A 60 m reach over 32 m tiles, centred on a tile corner: a disc of them.
    CHECK(grown.tilesResident >= 12);
    CHECK(grown.tilesResident <= 25);
    CHECK(grown.tilesGrownLastSync == grown.tilesResident);
    // About a square metre each, over the tiles' ground.
    CHECK(grown.instancesResident > grown.tilesResident * 1024u * 8u / 10u);

    // Nothing changed: nothing grows again.
    fixture.sync();
    CHECK(fixture.foliage.stats().tilesGrownLastSync == 0);
    CHECK(fixture.foliage.stats().instancesResident == grown.instancesResident);
}

TEST_CASE("an edit regrows the tiles it touched and no others")
{
    FoliageFixture fixture;
    fixture.sync();
    const core::u32 resident = fixture.foliage.stats().tilesResident;

    // A dent in the middle of one tile: that tile and, since the mesher reads
    // two voxels past a tile's side, at most its ring of neighbours.
    (void)asset::fillBall(fixture.world.terrains().find(fixture.terrain)->field, core::DVec3{16.0, 0.0, 16.0}, 3.0, 0);
    fixture.sync();
    const core::u32 regrown = fixture.foliage.stats().tilesGrownLastSync;
    CHECK(regrown >= 1);
    CHECK(regrown <= 9);
    CHECK(regrown < resident);
}

TEST_CASE("a change to the layer's rules regrows every tile of it")
{
    FoliageFixture fixture;
    fixture.sync();
    const core::u32 resident = fixture.foliage.stats().tilesResident;
    fixture.world.foliageLayers().find(fixture.layer)->density = 2.0f;
    fixture.sync();
    CHECK(fixture.foliage.stats().tilesGrownLastSync == resident);
}

TEST_CASE("tiles the camera left let their instances go, and a disabled layer grows nothing")
{
    FoliageFixture fixture;
    fixture.sync();
    fixture.foliage.setFocus(core::DVec3{5000.0, 2.0, 5000.0});
    fixture.sync();
    CHECK(fixture.foliage.stats().tilesResident == 0);

    fixture.foliage.setFocus(core::DVec3{0.0, 2.0, 0.0});
    fixture.world.foliageLayers().find(fixture.layer)->enabled = false;
    fixture.sync();
    CHECK(fixture.foliage.stats().tilesResident == 0);
}

TEST_CASE("a mesh that has not loaded contributes no bucket, and the frame carries the settings")
{
    FoliageFixture fixture;
    fixture.foliage.setSettings({.density = 0.5f, .shadowDistance = 12.0f, .growthsPerSync = 1000});
    fixture.sync();
    MeshLibrary empty;
    RenderWorld frame;
    fixture.foliage.append(fixture.world, empty, frame);
    CHECK(frame.foliageBuckets.empty());
    CHECK(frame.foliageRuns.empty());
    CHECK(frame.foliageDensity == 0.5f);
    CHECK(frame.foliageShadowDistance == 12.0f);
}

TEST_CASE("tiles are grown off the calling thread, and an edit is grown in the frame that finds it")
{
    // **D542**: every tile that came within reach was grown by the `sync` that
    // found it, on every worker with the frame waiting -- eight a frame, 14 to
    // 17 ms of each, for as long as there were tiles to grow: a game's first
    // second in its map, and any step into a meadow after it.
    FoliageFixture fixture;
    fixture.foliage.setSettings({.density = 1.0f, .shadowDistance = 30.0f, .growthsPerSync = 8});
    fixture.foliage.setAsync(true);
    core::u32 inFrame = 0;
    core::u32 grown = 0;
    int frames = 0;
    for (; frames < 4000 && (frames == 0 || fixture.foliage.pending()); ++frames) {
        fixture.sync();
        inFrame += fixture.foliage.stats().tilesGrownInFrame;
        grown += fixture.foliage.stats().tilesGrownLastSync;
    }
    REQUIRE_FALSE(fixture.foliage.pending());
    CHECK(inFrame == 0);
    // All of them, each once, a batch at a time.
    const FoliageStats settled = fixture.foliage.stats();
    CHECK(settled.tilesResident >= 12);
    CHECK(grown == settled.tilesResident);
    CHECK(frames > 2);
    CHECK(settled.instancesResident > settled.tilesResident * 1024u * 8u / 10u);

    // What it grew is what a `sync` that waits grows.
    FoliageFixture waited;
    waited.sync();
    CHECK(waited.foliage.stats().instancesResident == settled.instancesResident);

    // A dent in one tile: the tiles it touched, in this `sync`.
    (void)asset::fillBall(fixture.world.terrains().find(fixture.terrain)->field, core::DVec3{16.0, 0.0, 16.0}, 3.0, 0);
    fixture.sync();
    CHECK(fixture.foliage.stats().tilesGrownInFrame >= 1);
    CHECK(fixture.foliage.stats().tilesGrownInFrame <= 9);
    CHECK_FALSE(fixture.foliage.pending());

    // A rule of the layer: every tile, and none of them in the frame.
    fixture.world.foliageLayers().find(fixture.layer)->density = 2.0f;
    inFrame = 0;
    grown = 0;
    for (frames = 0; frames < 4000 && (frames == 0 || fixture.foliage.pending()); ++frames) {
        fixture.sync();
        inFrame += fixture.foliage.stats().tilesGrownInFrame;
        grown += fixture.foliage.stats().tilesGrownLastSync;
    }
    CHECK(inFrame == 0);
    CHECK(grown == settled.tilesResident);
}

TEST_CASE("a tile let go while it grows is not put up, and a system destroyed while one grows waits for it")
{
    FoliageFixture fixture;
    fixture.foliage.setSettings({.density = 1.0f, .shadowDistance = 30.0f, .growthsPerSync = 8});
    fixture.foliage.setAsync(true);
    fixture.sync();
    REQUIRE(fixture.foliage.pending());
    // The camera is gone before the first batch is back.
    fixture.foliage.setFocus(core::DVec3{5000.0, 2.0, 5000.0});
    for (int frames = 0; frames < 4000 && (frames == 0 || fixture.foliage.pending()); ++frames)
        fixture.sync();
    CHECK(fixture.foliage.stats().tilesResident == 0);
    CHECK(fixture.foliage.stats().instancesResident == 0);

    // And back, and destroyed with a batch in flight: the fixture's own end.
    fixture.foliage.setFocus(core::DVec3{0.0, 2.0, 0.0});
    fixture.sync();
    CHECK(fixture.foliage.pending());
}

TEST_CASE("ADR 0185: a bucket takes decals as its layer says, and saying so regrows nothing")
{
    FoliageFixture fixture;
    fixture.sync();
    MeshLibrary meshes;
    MeshLibrary::Entry grass;
    grass.mesh = MeshHandle{0, 1};
    grass.bounds = core::AABB::fromCenterSize(core::Vec3{}, core::Vec3{1.0f, 1.0f, 1.0f});
    grass.sectionCount = 1;
    meshes.set(fixture.atoms.intern("asset://models/grass.gltf"), grass);

    // A layer receives unless it says otherwise: a mark on the ground is on
    // the grass that stands in it.
    {
        RenderWorld frame;
        fixture.foliage.append(fixture.world, meshes, frame);
        REQUIRE(frame.foliageBuckets.size() == 1);
        CHECK(frame.foliageBuckets[0].receivesDecals);
    }

    fixture.world.foliageLayers().find(fixture.layer)->receivesDecals = false;
    fixture.sync();
    // How it is drawn, not where it grows: no tile is grown again for it.
    CHECK(fixture.foliage.stats().tilesGrownLastSync == 0);
    RenderWorld frame;
    fixture.foliage.append(fixture.world, meshes, frame);
    REQUIRE(frame.foliageBuckets.size() == 1);
    CHECK_FALSE(frame.foliageBuckets[0].receivesDecals);
}
