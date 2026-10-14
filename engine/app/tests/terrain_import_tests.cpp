// A world laid a tile at a time (ADR 0149 §2).
//
// What is proved here is that the tiles are not visible: ground laid a tile
// at a time is the ground one table of the whole would have laid, whatever
// the source -- hills, one image, an image in pieces, or a function.
#include <lua.h>
#include <lualib.h>

#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "engine/app/terrain_import.h"
#include "engine/asset/image.h"
#include "engine/asset/terrain.h"
#include "engine/platform/file.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "inspector_fixture.h"

using namespace engine;

namespace {

constexpr asset::FieldSettings Settings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 192.0f};

struct Ground
{
    app::testing::Fixture fixture;
    scene::World world{fixture.classes, fixture.enums, fixture.atoms, 7u};
    core::InstanceId id;

    Ground()
    {
        const core::InstanceId workspace = world.create(fixture.workspaceClass);
        world.workspaces().add(workspace, scene::WorkspaceComponent{});
        id = world.create(fixture.folderClass);
        scene::TerrainComponent terrain;
        terrain.field = asset::TerrainField(Settings);
        world.terrains().add(id, std::move(terrain));
        (void)world.setParent(id, workspace);
    }

    [[nodiscard]] scene::TerrainComponent& terrain() { return *world.terrains().find(id); }
};

// Lays every tile.
[[nodiscard]] app::TerrainImport::Step layAll(app::TerrainImport& import, Ground& ground, std::string& error)
{
    app::TerrainImport::Step step = app::TerrainImport::Step::More;
    while (step == app::TerrainImport::Step::More)
        step = import.step(ground.world, ground.terrain(), error);
    return step;
}

// Whether two fields hold the same voxels in every column of a rectangle, in
// the rows round the surface: the top, and the ramp either side of it.
[[nodiscard]] core::u32 differingColumns(const asset::TerrainField& a, const asset::TerrainField& b, core::i32 firstX,
                                         core::i32 firstZ, core::u32 columns, core::u32 rows)
{
    core::u32 differing = 0;
    for (core::u32 row = 0; row < rows; ++row) {
        for (core::u32 column = 0; column < columns; ++column) {
            const core::i32 x = firstX + static_cast<core::i32>(column);
            const core::i32 z = firstZ + static_cast<core::i32>(row);
            const std::optional<float> top = a.columnTop(x, z);
            const std::optional<float> other = b.columnTop(x, z);
            bool same = top.has_value() == other.has_value();
            if (same && top.has_value()) {
                same = *top == *other;
                const core::i32 at = a.voxelIndex(static_cast<double>(*top));
                for (core::i32 y = at - 6; same && y <= at + 6; ++y) {
                    const asset::Voxel left = a.voxel(x, y, z);
                    const asset::Voxel right = b.voxel(x, y, z);
                    same = left.occupancy == right.occupancy && left.material == right.material;
                }
            }
            differing += same ? 0u : 1u;
        }
    }
    return differing;
}

} // namespace

TEST_CASE("ground laid a tile at a time is the ground one table lays")
{
    // Hills steep enough that the slope matters, over a rectangle that starts
    // and ends inside a tile -- 300 by 200 columns across tiles of 64.
    const asset::HillSettings hills{.seed = 5, .octaves = 4, .scale = 48.0f, .low = 0.0f, .high = 90.0f};
    constexpr core::i32 FirstX = -150;
    constexpr core::i32 FirstZ = -70;
    constexpr core::u32 Columns = 300;
    constexpr core::u32 Rows = 200;

    asset::TerrainField whole(Settings);
    const std::vector<float> table = asset::hillHeights(whole, FirstX, FirstZ, Columns, Rows, hills);
    REQUIRE_FALSE(asset::writeHeights(whole, FirstX, FirstZ, Columns, table, 2).refused);

    Ground ground;
    app::TerrainImportPlan plan{FirstX, FirstZ, Columns, Rows, 2, 64};
    app::TerrainImport import(app::hillSource(Settings, hills), plan);
    CHECK(import.tiles() == 6u * 5u);
    std::string error;
    REQUIRE(layAll(import, ground, error) == app::TerrainImport::Step::Done);
    CHECK(import.progress() == 1.0f);
    CHECK(differingColumns(whole, ground.terrain().field, FirstX, FirstZ, Columns, Rows) == 0u);

    // **And without its neighbours' column a tile is not**: the slope at a
    // seam is one-sided, and the ramp there is another ramp. This is what the
    // window of a table is for (`asset::HeightWindow`).
    asset::TerrainField bare(Settings);
    for (core::i32 tileZ = -2; tileZ <= 2; ++tileZ) {
        for (core::i32 tileX = -3; tileX <= 2; ++tileX) {
            const core::i32 x0 = std::max(FirstX, tileX * 64);
            const core::i32 z0 = std::max(FirstZ, tileZ * 64);
            const core::i32 x1 = std::min(FirstX + static_cast<core::i32>(Columns) - 1, tileX * 64 + 63);
            const core::i32 z1 = std::min(FirstZ + static_cast<core::i32>(Rows) - 1, tileZ * 64 + 63);
            if (x1 < x0 || z1 < z0)
                continue;
            const auto width = static_cast<core::u32>(x1 - x0 + 1);
            const std::vector<float> tile =
                asset::hillHeights(bare, x0, z0, width, static_cast<core::u32>(z1 - z0 + 1), hills);
            (void)asset::writeHeights(bare, x0, z0, width, tile, 2);
        }
    }
    CHECK(differingColumns(whole, bare, FirstX, FirstZ, Columns, Rows) > 0u);
}

TEST_CASE("a heightmap in pieces is the heightmap whole")
{
    // Sixty-four pixels a side, a bowl: whole, and as four pieces of
    // thirty-two named the way terrain tools name a world's tiles.
    asset::HeightImage image;
    image.width = 64;
    image.height = 64;
    image.samples.resize(64u * 64u);
    for (core::u32 y = 0; y < 64; ++y) {
        for (core::u32 x = 0; x < 64; ++x) {
            const float dx = (static_cast<float>(x) - 31.5f) / 31.5f;
            const float dy = (static_cast<float>(y) - 31.5f) / 31.5f;
            image.samples[static_cast<std::size_t>(y) * 64u + x] = std::min(1.0f, 0.5f * (dx * dx + dy * dy));
        }
    }
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "engine-terrain-import-tests";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    REQUIRE(platform::createDirectories(folder / "pieces"));
    const auto write = [&](const asset::HeightImage& piece, const std::filesystem::path& file) {
        std::vector<std::byte> bytes;
        REQUIRE_FALSE(asset::encodeHeightmap(piece, asset::HeightmapFormat::Png16, bytes).has_value());
        REQUIRE(platform::writeFile(file, bytes));
    };
    write(image, folder / "bowl.png");
    for (core::u32 pieceY = 0; pieceY < 2; ++pieceY) {
        for (core::u32 pieceX = 0; pieceX < 2; ++pieceX) {
            asset::HeightImage piece;
            piece.width = 32;
            piece.height = 32;
            piece.samples.resize(32u * 32u);
            for (core::u32 y = 0; y < 32; ++y) {
                for (core::u32 x = 0; x < 32; ++x)
                    piece.samples[static_cast<std::size_t>(y) * 32u + x] =
                        image.samples[static_cast<std::size_t>(pieceY * 32u + y) * 64u + pieceX * 32u + x];
            }
            write(piece,
                  folder / "pieces" / ("bowl_x" + std::to_string(pieceX) + "_y" + std::to_string(pieceY) + ".png"));
        }
    }

    // Stretched over 200 columns, so every column is between pixels.
    const app::TerrainImportPlan plan{-100, -100, 200, 200, 1, 64};
    std::string error;
    std::unique_ptr<app::HeightmapSource> whole = app::openHeightmap(folder / "bowl.png", error);
    std::unique_ptr<app::HeightmapSource> pieces = app::openHeightmap(folder / "pieces", error);
    REQUIRE(whole != nullptr);
    REQUIRE(pieces != nullptr);
    CHECK(whole->size().width == 64u);
    CHECK(pieces->size().width == 64u);
    CHECK(pieces->size().height == 64u);
    whole->map(plan, 0.0f, 40.0f);
    pieces->map(plan, 0.0f, 40.0f);

    // The whole image through `resampleHeights`, which is what the one-table
    // import lays: the tiled source answers the same heights.
    const std::vector<float> expected = asset::resampleHeights(image, 200, 200, 0.0f, 40.0f);
    std::vector<float> fromWhole(200u * 200u);
    std::vector<float> fromPieces(200u * 200u);
    REQUIRE(whole->read(-100, -100, 200, 200, fromWhole, error));
    REQUIRE(pieces->read(-100, -100, 200, 200, fromPieces, error));
    float worst = 0.0f;
    for (std::size_t at = 0; at < expected.size(); ++at) {
        worst = std::max(worst, std::abs(fromWhole[at] - expected[at]));
        worst = std::max(worst, std::abs(fromPieces[at] - fromWhole[at]));
    }
    // Sixteen bits of forty metres is under a millimetre.
    CHECK(static_cast<double>(worst) < 0.002);

    // A piece that is missing is ground that is not laid.
    std::filesystem::remove(folder / "pieces" / "bowl_x1_y1.png", ignored);
    std::unique_ptr<app::HeightmapSource> holed = app::openHeightmap(folder / "pieces", error);
    REQUIRE(holed != nullptr);
    holed->map(plan, 0.0f, 40.0f);
    std::vector<float> fromHoled(4);
    REQUIRE(holed->read(-100, -100, 2, 1, std::span<float>(fromHoled).first(2), error));
    CHECK_FALSE(std::isnan(fromHoled[0]));
    REQUIRE(holed->read(98, 98, 2, 1, std::span<float>(fromHoled).first(2), error));
    CHECK(std::isnan(fromHoled[0]));
    Ground ground;
    app::TerrainImport import(std::move(holed), plan);
    REQUIRE(layAll(import, ground, error) == app::TerrainImport::Step::Done);
    CHECK(ground.terrain().field.columnTop(-60, -60).has_value());
    CHECK_FALSE(ground.terrain().field.columnTop(60, 60).has_value());

    // And a folder with no pieces in it is refused by name.
    REQUIRE(platform::createDirectories(folder / "empty"));
    CHECK(app::openHeightmap(folder / "empty", error) == nullptr);
    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("ground from a Luau function is the function's heights, and a function that fails says so")
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    std::string error;
    {
        // A slope: a metre up every two along x, and the terrain a hundred
        // metres up, so the field's height is the world's less that.
        const core::DVec3 origin{1000.0, 100.0, -500.0};
        std::unique_ptr<app::HeightSource> slope = app::functionSource(
            L, "return function(x: number, z: number): number\n    return 120 + (x - 1000) * 0.5\nend", "=slope",
            origin, 1.0f, error);
        REQUIRE(slope != nullptr);
        std::vector<float> heights(4);
        REQUIRE(slope->read(10, 0, 2, 2, heights, error));
        // Column 10's middle is at x = 1010.5.
        CHECK(static_cast<double>(heights[0]) == doctest::Approx(20.0 + 10.5 * 0.5));
        CHECK(static_cast<double>(heights[1]) == doctest::Approx(20.0 + 11.5 * 0.5));
        CHECK(static_cast<double>(heights[2]) == doctest::Approx(20.0 + 10.5 * 0.5));

        Ground ground;
        ground.terrain().origin = origin;
        app::TerrainImport import(std::move(slope), app::TerrainImportPlan{0, 0, 96, 40, 1, 64});
        REQUIRE(layAll(import, ground, error) == app::TerrainImport::Step::Done);
        const std::optional<float> top = ground.terrain().field.columnTop(40, 20);
        REQUIRE(top.has_value());
        CHECK(static_cast<double>(*top) == doctest::Approx(20.0 + 40.5 * 0.5).epsilon(0.02));
    }
    {
        // Not a function, a chunk that does not compile, and a function that
        // answers nothing: each refused, with something to say.
        error.clear();
        CHECK(app::functionSource(L, "return 3", "=three", core::DVec3{}, 1.0f, error) == nullptr);
        CHECK_FALSE(error.empty());
        error.clear();
        CHECK(app::functionSource(L, "return function(", "=broken", core::DVec3{}, 1.0f, error) == nullptr);
        CHECK_FALSE(error.empty());
        std::unique_ptr<app::HeightSource> silent =
            app::functionSource(L, "return function(x, z) end", "=silent", core::DVec3{}, 1.0f, error);
        REQUIRE(silent != nullptr);
        Ground ground;
        app::TerrainImport import(std::move(silent), app::TerrainImportPlan{0, 0, 8, 8, 1, 64});
        CHECK(layAll(import, ground, error) == app::TerrainImport::Step::Failed);
        CHECK(ground.terrain().field.empty());
        std::unique_ptr<app::HeightSource> raises = app::functionSource(
            L, "return function(x, z) error('no ground here') end", "=raises", core::DVec3{}, 1.0f, error);
        REQUIRE(raises != nullptr);
        std::vector<float> heights(1);
        error.clear();
        CHECK_FALSE(raises->read(0, 0, 1, 1, heights, error));
        CHECK(error.find("no ground here") != std::string::npos);
    }
    lua_close(L);
}

TEST_CASE("a table's window is laid and its apron is not")
{
    // Sixteen columns of a slope, the middle eight laid: nothing outside them
    // is ground, and the edge of what is laid has the slope the whole table
    // gives it.
    asset::TerrainField whole(Settings);
    asset::TerrainField windowed(Settings);
    std::vector<float> heights(16u * 16u);
    for (core::u32 row = 0; row < 16; ++row) {
        for (core::u32 column = 0; column < 16; ++column)
            heights[static_cast<std::size_t>(row) * 16u + column] = 10.0f + 1.5f * static_cast<float>(column);
    }
    (void)asset::writeHeights(whole, 0, 0, 16, heights, 1);
    const asset::EditReport report =
        asset::writeHeights(windowed, 0, 0, 16, heights, 1, asset::HeightWindow{4, 4, 8, 8});
    CHECK(report.touched > 0u);
    CHECK_FALSE(windowed.columnTop(3, 8).has_value());
    CHECK_FALSE(windowed.columnTop(12, 8).has_value());
    CHECK_FALSE(windowed.columnTop(8, 3).has_value());
    CHECK(differingColumns(whole, windowed, 4, 4, 8, 8) == 0u);
    // A window past its table lays nothing.
    asset::TerrainField none(Settings);
    CHECK(asset::writeHeights(none, 0, 0, 16, heights, 1, asset::HeightWindow{12, 0, 8, 8}).touched == 0u);
    CHECK(none.empty());
}
