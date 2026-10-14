// Foliage in the editor (ADR 0116): the density a brush paints, and what the
// scene keeps of it.
#include <doctest/doctest.h>
#include <string>

#include "engine/app/editor.h"
#include "engine/app/world_host.h"
#include "engine/asset/terrain.h"
#include "engine/scene/components.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;

namespace {

// A world with a terrain under the workspace and a foliage layer under it.
struct Meadow
{
    app::testing::Captured log;
    app::testing::Project project;
    app::WorldHost host;
    core::InstanceId terrain;
    core::InstanceId layer;

    Meadow()
    {
        REQUIRE_FALSE(host.boot(app::testing::bootOptions(project.root)).has_value());
        scene::World& world = host.world();
        terrain = world.create(world.classes().findId(world.atoms().intern("Terrain")));
        REQUIRE(terrain.valid());
        REQUIRE_FALSE(world.setParent(terrain, host.workspace()).has_value());
        layer = world.create(world.classes().findId(world.atoms().intern("FoliageLayer")));
        REQUIRE(layer.valid());
        world.setName(layer, world.atoms().intern("Meadow"));
        REQUIRE_FALSE(world.setParent(layer, terrain).has_value());
    }

    [[nodiscard]] scene::FoliageLayerComponent& component() { return *host.world().foliageLayers().find(layer); }
};

} // namespace

TEST_CASE("thinning paints a mask over the columns it touches, and painting back removes it")
{
    Meadow meadow;
    scene::World& world = meadow.host.world();
    CHECK(meadow.component().mask.empty());

    // Restoring what was never thinned changes nothing and makes no entry.
    CHECK_FALSE(app::Editor::paintFoliage(world, meadow.layer, core::DVec3{16.0, 0.0, 16.0}, 4.0, 1.0f, 1.0f));
    CHECK(meadow.component().mask.empty());

    // Thinned, in one tile: the centre most, the rim not at all.
    REQUIRE(app::Editor::paintFoliage(world, meadow.layer, core::DVec3{16.0, 0.0, 16.0}, 4.0, -1.0f, 1.0f));
    REQUIRE(meadow.component().mask.size() == 1);
    const scene::FoliageMaskColumn& column = meadow.component().mask.front();
    CHECK(column.x == 0);
    CHECK(column.z == 0);
    REQUIRE(column.density.size() == asset::ChunkEdge * asset::ChunkEdge);
    CHECK(column.density[16 * asset::ChunkEdge + 16] < 64);
    CHECK(column.density[0] == 255);

    // A stroke across a tile edge touches both tiles.
    REQUIRE(app::Editor::paintFoliage(world, meadow.layer, core::DVec3{32.0, 0.0, 16.0}, 3.0, -0.5f, 1.0f));
    CHECK(meadow.component().mask.size() == 2);

    // Painted all the way back, the entries go.
    for (int pass = 0; pass < 8; ++pass) {
        (void)app::Editor::paintFoliage(world, meadow.layer, core::DVec3{16.0, 0.0, 16.0}, 6.0, 1.0f, 1.0f);
        (void)app::Editor::paintFoliage(world, meadow.layer, core::DVec3{32.0, 0.0, 16.0}, 6.0, 1.0f, 1.0f);
    }
    CHECK(meadow.component().mask.empty());
}

TEST_CASE("a painted mask and a layer's materials are saved with the scene and read back")
{
    Meadow meadow;
    scene::World& world = meadow.host.world();
    REQUIRE(app::Editor::paintFoliage(world, meadow.layer, core::DVec3{-10.0, 0.0, 40.0}, 5.0, -0.75f, 1.0f));
    meadow.component().materials = {scene::FoliageMaterial{1, 1.0f}, scene::FoliageMaterial{3, 0.25f}};
    const std::vector<scene::FoliageMaskColumn> painted = meadow.component().mask;

    const std::string text = scene::writeScene(world);
    CHECK(text.find("foliageMask") != std::string::npos);
    CHECK(text.find("foliageMaterials") != std::string::npos);

    Meadow reloaded;
    scene::World& other = reloaded.host.world();
    other.destroy(reloaded.terrain);
    other.retireDestroyed();
    REQUIRE_FALSE(scene::readScene(other, text).has_value());
    core::InstanceId layer;
    other.foliageLayers().forEach([&](core::InstanceId id, const scene::FoliageLayerComponent&) {
        if (other.name(id) == other.atoms().intern("Meadow"))
            layer = id;
    });
    REQUIRE(layer.valid());
    const scene::FoliageLayerComponent& read = *other.foliageLayers().find(layer);
    CHECK(read.mask == painted);
    REQUIRE(read.materials.size() == 2);
    CHECK(read.materials[1].material == 3);
    CHECK(read.materials[1].density == 0.25f);
}
