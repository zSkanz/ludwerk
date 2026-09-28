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
