// The far ground, kept on disk (ADR 0150).
//
// What must hold is that a coarse node built from the far ground's files is
// the node the whole ground gives -- vertex for vertex -- and so that the
// files hold what the gather holds (ADR 0140): the slab thinner than a coarse
// cell, the ball, flat ground where it is, the material and the paint on top.
// And that keeping them is what it is for: a node built again reads no cell,
// and a cell that changed costs the nine cells round it and nothing more.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "engine/asset/field_cells.h"
#include "engine/asset/terrain.h"
#include "engine/asset/terrain_cell.h"
#include "engine/asset/terrain_mesher.h"
#include "engine/asset/terrain_pyramid.h"
#include "engine/render/terrain_loader.h"

using namespace engine;
using namespace engine::render;

namespace {

constexpr asset::FieldSettings Settings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f};

// A terrain's ground as cells in memory, its far ground's files in memory
// beside them, and how often either was read.
struct FarGround
{
    std::vector<asset::TerrainCell> cells;
    std::map<std::pair<core::i32, core::i32>, asset::TerrainCell> byId;
    std::map<std::tuple<core::u32, core::i32, core::i32>, std::vector<std::byte>> files;
    int cellReads = 0;
    int fileReads = 0;
    int fileWrites = 0;
    std::shared_ptr<asset::TerrainPyramid> pyramid;
    std::unique_ptr<asset::TerrainCellSource> source;
    core::u32 cellChunks = asset::terrainCellChunks(Settings.voxelSize);

    explicit FarGround(const asset::TerrainField& whole, bool withFiles = true)
    {
        cells = asset::splitTerrain(whole);
        std::vector<asset::ChunkId> ids;
        std::vector<core::u64> signatures;
        for (const asset::TerrainCell& cell : cells) {
            ids.push_back(asset::ChunkId{cell.x, cell.z, asset::FieldLayerTerrain});
            signatures.push_back(asset::terrainCellSignature(cell.field, cell.x, cell.z, cellChunks));
            byId[{cell.x, cell.z}] = cell;
        }
        source = std::make_unique<asset::TerrainCellSource>(
            cellChunks, ids, signatures, [this](asset::ChunkId id) -> std::optional<asset::TerrainCell> {
                ++cellReads;
                const auto found = byId.find({id.x, id.z});
                return found == byId.end() ? std::nullopt : std::optional<asset::TerrainCell>(found->second);
            });
        if (!withFiles)
            return;
        asset::TerrainPyramid::Store store;
        store.read = [this](core::u32 level, core::i32 x, core::i32 z, std::vector<std::byte>& out) {
            const auto found = files.find({level, x, z});
            if (found == files.end())
                return false;
            ++fileReads;
            out = found->second;
            return true;
        };
        store.write = [this](core::u32 level, core::i32 x, core::i32 z, std::span<const std::byte> bytes) {
            ++fileWrites;
            files[{level, x, z}] = std::vector<std::byte>(bytes.begin(), bytes.end());
            return true;
        };
        pyramid = std::make_shared<asset::TerrainPyramid>(Settings, cellChunks, std::move(store));
        source->setPyramid(pyramid);
    }

    // The same ground with one cell's replaced, as a save or the session cache
    // writes it: the cell, and what it now holds.
    void rewrite(const asset::TerrainField& whole, core::i32 cellX, core::i32 cellZ)
    {
        for (const asset::TerrainCell& cell : asset::splitTerrain(whole)) {
            if (cell.x != cellX || cell.z != cellZ)
                continue;
            byId[{cellX, cellZ}] = cell;
            const asset::ChunkId id{cellX, cellZ, asset::FieldLayerTerrain};
            const core::u64 signature = asset::terrainCellSignature(cell.field, cellX, cellZ, cellChunks);
            source->update(std::span<const asset::ChunkId>(&id, 1), {}, std::span<const core::u64>(&signature, 1));
        }
    }
};

[[nodiscard]] bool sameMesh(const asset::TerrainMesh& a, const asset::TerrainMesh& b)
{
    if (a.mesh.vertices.size() != b.mesh.vertices.size() || a.mesh.indices != b.mesh.indices ||
        !(a.morphs == b.morphs) || a.morphTags != b.morphTags || a.sectionMaterials != b.sectionMaterials)
        return false;
    for (std::size_t at = 0; at < a.mesh.vertices.size(); ++at) {
        const asset::Vertex& left = a.mesh.vertices[at];
        const asset::Vertex& right = b.mesh.vertices[at];
        if (left.position.x != right.position.x || left.position.y != right.position.y ||
            left.position.z != right.position.z || left.normal.x != right.normal.x || left.normal.y != right.normal.y ||
            left.normal.z != right.normal.z || left.uv[0] != right.uv[0] || left.uv[1] != right.uv[1] ||
            left.tangent[0] != right.tangent[0] || left.tangent[1] != right.tangent[1] ||
            left.tangent[2] != right.tangent[2] || left.tangent[3] != right.tangent[3])
            return false;
    }
    return a.error == b.error;
}

// Every node of the levels kept on disk that the ground reaches.
template <typename Visit>
void eachKeptNode(core::i32 reachChunks, Visit visit)
{
    for (core::u32 level = asset::PyramidFirstLevel; level <= TerrainTopLevel; ++level) {
        const core::i32 n = 1 << level;
        for (core::i32 z = -reachChunks / n - 1; z <= reachChunks / n; ++z) {
            for (core::i32 x = -reachChunks / n - 1; x <= reachChunks / n; ++x)
                visit(TerrainNodeKey{level, x, z});
        }
    }
}

// The audit's gallery, as T2's own tests build it (`terrain_levels_tests`).
struct Shape
{
    const char* name;
    std::function<void(asset::TerrainField&)> build;
};

[[nodiscard]] std::vector<Shape> gallery()
{
    return {
        {"a 2 m slab",
         [](asset::TerrainField& f) { (void)asset::fillBlock(f, {0.0, 1.0, 0.0}, {96.0f, 2.0f, 96.0f}, 1); }},
        {"a 4 m slab",
         [](asset::TerrainField& f) { (void)asset::fillBlock(f, {0.0, 2.0, 0.0}, {96.0f, 4.0f, 96.0f}, 1); }},
        {"a ball of radius 2", [](asset::TerrainField& f) { (void)asset::fillBall(f, {0.5, 8.5, 0.5}, 2.0, 1); }},
        {"a ball of radius 4", [](asset::TerrainField& f) { (void)asset::fillBall(f, {0.5, 8.5, 0.5}, 4.0, 1); }},
        {"a ball of radius 8", [](asset::TerrainField& f) { (void)asset::fillBall(f, {0.5, 16.5, 0.5}, 8.0, 1); }},
        {"two merged balls",
         [](asset::TerrainField& f) {
             (void)asset::fillBall(f, {-3.0, 8.5, 0.5}, 4.0, 1);
             (void)asset::fillBall(f, {3.0, 8.5, 0.5}, 4.0, 1);
         }},
    };
}

// The meshes of the four nodes of `level` round the origin, built from the far
// ground's files with nothing resident, as one.
[[nodiscard]] asset::TerrainMesh farMeshAt(FarGround& ground, core::u32 level)
{
    const asset::TerrainField nothing(Settings);
    asset::TerrainMesh out;
    for (core::i32 z = -1; z <= 0; ++z) {
        for (core::i32 x = -1; x <= 0; ++x)
            asset::appendMesh(out,
                              buildTerrainNodeFromCells(nothing, *ground.source, TerrainNodeKey{level, x, z}).mesh);
    }
    return out;
}

template <typename Visit>
void eachTopVertex(const asset::TerrainMesh& meshed, float inner, Visit visit)
{
    for (const asset::Vertex& vertex : meshed.mesh.vertices) {
        if (vertex.normal.y < 0.9f || std::abs(vertex.position.x) > inner || std::abs(vertex.position.z) > inner)
            continue;
        visit(vertex);
    }
}

} // namespace

TEST_CASE("a coarse node from the far ground's files is the node the whole ground gives, and reads no cell again")
{
    // The ground `terrain_loader_tests` holds the cells' path to: a floating
    // 2 m slab, ground filled from the floor, a slab of another material and
    // a ball across the cells' borders.
    asset::TerrainField whole(Settings);
    (void)asset::fillBlock(whole, core::DVec3{0.0, 1.0, 0.0}, core::Vec3{768.0f, 2.0f, 512.0f}, 1);
    (void)asset::fillFlat(whole, core::DVec3{0.0, 0.0, 512.0}, 512.0f, 0.0f, 1);
    (void)asset::fillBlock(whole, core::DVec3{-40.0, 3.0, 20.0}, core::Vec3{160.0f, 2.0f, 70.0f}, 2);
    (void)asset::fillBall(whole, core::DVec3{64.0, 6.0, -64.0}, 24.0, 3);
    FarGround ground(whole);
    REQUIRE(ground.cells.size() > 100);

    const auto residentWhere = [&](const std::function<bool(const asset::TerrainCell&)>& kept) {
        asset::TerrainField resident(Settings);
        for (const asset::TerrainCell& cell : ground.cells) {
            if (kept(cell)) {
                for (const asset::TerrainField::Entry& entry : cell.field.chunks())
                    resident.setChunk(entry.first, entry.second);
            }
        }
        return resident;
    };
    const std::vector<asset::TerrainField> residents{
        residentWhere([](const asset::TerrainCell&) { return false; }),
        residentWhere([](const asset::TerrainCell& cell) { return cell.x < 0; }),
        residentWhere(
            [](const asset::TerrainCell& cell) { return std::abs(cell.x - 2) <= 1 && std::abs(cell.z + 1) <= 1; }),
    };
    std::map<std::tuple<core::u32, core::i32, core::i32>, asset::TerrainMesh> expectedOf;
    const auto expectedFor = [&](const TerrainNodeKey& key) -> const asset::TerrainMesh& {
        const auto at = std::tuple{key.level, key.x, key.z};
        auto found = expectedOf.find(at);
        if (found == expectedOf.end())
            found = expectedOf.emplace(at, meshTerrainNode(whole, key)).first;
        return found->second;
    };

    int compared = 0;
    int drawn = 0;
    for (const asset::TerrainField& resident : residents) {
        eachKeptNode(12, [&](TerrainNodeKey key) {
            if (!terrainNodeReadsCells(resident, ground.source.get(), key))
                return;
            CAPTURE(key.level);
            CAPTURE(key.x);
            CAPTURE(key.z);
            const TerrainNodeBuild built = buildTerrainNodeFromCells(resident, *ground.source, key);
            const asset::TerrainMesh& expected = expectedFor(key);
            CHECK(sameMesh(built.mesh, expected));
            // Named as the whole ground, resident, names it; and as the
            // ground on disk does.
            CHECK(built.content == terrainNodeContent(whole, ground.source.get(), key));
            CHECK(terrainNodeContent(resident, ground.source.get(), key) == built.content);
            ++compared;
            drawn += expected.mesh.indices.empty() ? 0 : 1;
        });
    }
    CHECK(compared > 30);
    CHECK(drawn > 10);
    // The files were made, by reading the cells -- once.
    CHECK(ground.fileWrites > 0);
    CHECK(ground.pyramid->stats().blocksBuilt > 0);
    const int readsToBuild = ground.cellReads;
    CHECK(readsToBuild > 0);

    // **And again, nothing is read**: not a cell, whatever the node spans.
    for (const asset::TerrainField& resident : residents) {
        eachKeptNode(12, [&](TerrainNodeKey key) {
            if (terrainNodeReadsCells(resident, ground.source.get(), key))
                CHECK(sameMesh(buildTerrainNodeFromCells(resident, *ground.source, key).mesh, expectedFor(key)));
        });
    }
    CHECK(ground.cellReads == readsToBuild);

    // A second terrain of the same ground and the same files -- another run --
    // reads no cell at all.
    FarGround again(whole);
    again.files = ground.files;
    const asset::TerrainField nothing(Settings);
    eachKeptNode(12, [&](TerrainNodeKey key) {
        CHECK(sameMesh(buildTerrainNodeFromCells(nothing, *again.source, key).mesh, expectedFor(key)));
    });
    CHECK(again.cellReads == 0);
    CHECK(again.fileWrites == 0);
}

TEST_CASE(
    "the far ground's files hold what the gather holds: thin slabs, balls, flat ground, the material and the paint")
{
    // The terrain audit's T2 (ADR 0140), held against what is kept on disk:
    // nothing here is resident, so every vertex is from a file.
    SUBCASE("every shape of the gallery still has triangles at every level kept (TA1)")
    {
        for (const Shape& shape : gallery()) {
            asset::TerrainField field(Settings);
            shape.build(field);
            FarGround ground(field);
            for (core::u32 level = asset::PyramidFirstLevel; level <= TerrainTopLevel; ++level) {
                CAPTURE(shape.name);
                CAPTURE(level);
                CHECK_FALSE(farMeshAt(ground, level).mesh.indices.empty());
            }
            CHECK(ground.fileWrites > 0);
        }
    }
    SUBCASE("flat ground at a quarter-metre height is within 2 cm (TA2)")
    {
        for (const float height : {3.0f, 3.25f, 3.5f, 3.75f}) {
            asset::TerrainField field(Settings);
            (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, height, 1);
            FarGround ground(field);
            for (core::u32 level = asset::PyramidFirstLevel; level <= TerrainTopLevel; ++level) {
                CAPTURE(height);
                CAPTURE(level);
                const asset::TerrainMesh meshed = farMeshAt(ground, level);
                float worst = 0.0f;
                int seen = 0;
                eachTopVertex(meshed, 64.0f, [&](const asset::Vertex& vertex) {
                    worst = std::max(worst, std::abs(vertex.position.y - height));
                    ++seen;
                });
                REQUIRE(seen > 0);
                CHECK(static_cast<double>(worst) < 0.02);
            }
        }
    }
    SUBCASE("a coarse cell takes the material on top, and paint survives every level (TA7)")
    {
        asset::TerrainField field(Settings);
        (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
        (void)asset::fillBlock(field, core::DVec3{0.0, 8.0, 0.0}, core::Vec3{48.0f, 16.0f, 48.0f}, 2);
        asset::PaintOptions over;
        over.mode = asset::PaintMode::Blend;
        over.strength = 0.5f;
        (void)asset::paintBall(field, core::DVec3{80.0, 0.0, 80.0}, 40.0, 3, over);
        FarGround ground(field);
        {
            const asset::TerrainMesh meshed = farMeshAt(ground, asset::PyramidFirstLevel);
            int wrong = 0;
            int seen = 0;
            eachTopVertex(meshed, 16.0f, [&](const asset::Vertex& vertex) {
                if (vertex.position.y < 12.0f)
                    return;
                ++seen;
                wrong += static_cast<int>(vertex.tangent[0] + 0.5f) != 2 ? 1 : 0;
            });
            REQUIRE(seen > 0);
            CHECK(wrong == 0);
        }
        for (core::u32 level = asset::PyramidFirstLevel; level <= TerrainTopLevel; ++level) {
            CAPTURE(level);
            const asset::TerrainMesh meshed = farMeshAt(ground, level);
            int painted = 0;
            int seen = 0;
            for (const asset::Vertex& vertex : meshed.mesh.vertices) {
                // Seventeen metres round the paint's middle: a level-5 cell is
                // 32 across, its vertex at the middle of what it gathers.
                if (vertex.normal.y < 0.9f || std::abs(vertex.position.x - 80.0f) > 17.0f ||
                    std::abs(vertex.position.z - 80.0f) > 17.0f)
                    continue;
                ++seen;
                const auto tops = static_cast<core::u32>(vertex.tangent[2] + 0.5f);
                const auto corner = static_cast<core::u32>(vertex.uv[1] + 0.5f);
                painted += ((tops >> (8u * corner)) & 0xFFu) == 3u ? 1 : 0;
            }
            REQUIRE(seen > 0);
            CHECK(painted == seen);
        }
    }
}

TEST_CASE("a cell that changes is gathered again with the cells round it, and nothing else of its block")
{
    // Hills over three kilometres -- nine blocks' worth and more -- and one
    // crater, written out as the session cache or a save writes it.
    asset::TerrainField whole(Settings);
    const asset::HillSettings hills{.seed = 3, .octaves = 4, .scale = 160.0f, .low = 0.0f, .high = 40.0f};
    constexpr core::i32 First = -640;
    constexpr core::u32 Columns = 1280;
    const std::vector<float> heights = asset::hillHeights(whole, First, First, Columns, Columns, hills);
    REQUIRE_FALSE(asset::writeHeights(whole, First, First, Columns, heights, 1).refused);
    FarGround ground(whole);
    const asset::TerrainField nothing(Settings);
    eachKeptNode(20, [&](TerrainNodeKey key) { (void)buildTerrainNodeFromCells(nothing, *ground.source, key); });
    const asset::TerrainPyramid::Stats before = ground.pyramid->stats();
    const int cellReads = ground.cellReads;
    const int fileWrites = ground.fileWrites;
    REQUIRE(before.blocksBuilt >= 4);

    // A crater in the middle of one cell -- the one from 64 to 128 m each way
    // -- well inside a block and inside one node of each level.
    const std::optional<float> top = asset::heightAt(whole, 96.5, 96.5);
    REQUIRE(top.has_value());
    REQUIRE(asset::fillBall(whole, core::DVec3{96.0, static_cast<double>(*top), 96.0}, 12.0, 0).touched > 0);
    ground.rewrite(whole, 1, 1);

    // Every node again: what the crater touched is the crater's now, and the
    // rest is as it was.
    eachKeptNode(20, [&](TerrainNodeKey key) {
        CAPTURE(key.level);
        CAPTURE(key.x);
        CAPTURE(key.z);
        CHECK(sameMesh(buildTerrainNodeFromCells(nothing, *ground.source, key).mesh, meshTerrainNode(whole, key)));
    });
    const asset::TerrainPyramid::Stats after = ground.pyramid->stats();
    // The cell and the eight round it, read once; one block touched; one file
    // a level written again.
    CHECK(ground.cellReads - cellReads <= 9);
    CHECK(after.blocksBuilt - before.blocksBuilt == 1);
    CHECK(ground.fileWrites - fileWrites == 3);
}

TEST_CASE("ground changed and not written out is meshed as it is now, and its files are left as the disk is")
{
    asset::TerrainField whole(Settings);
    (void)asset::fillFlat(whole, core::DVec3{0.0, 0.0, 0.0}, 640.0f, 4.0f, 1);
    FarGround ground(whole);
    const asset::TerrainField nothing(Settings);
    eachKeptNode(10, [&](TerrainNodeKey key) { (void)buildTerrainNodeFromCells(nothing, *ground.source, key); });
    const int fileWrites = ground.fileWrites;

    // The cells round the origin resident, and a hill raised on them by a
    // brush: in the field, not in the cells.
    asset::TerrainField resident(Settings);
    for (const asset::TerrainCell& cell : ground.cells) {
        if (std::abs(cell.x) <= 2 && std::abs(cell.z) <= 2) {
            for (const asset::TerrainField::Entry& entry : cell.field.chunks())
                resident.setChunk(entry.first, entry.second);
        }
    }
    (void)asset::fillBall(resident, core::DVec3{10.0, 6.0, 10.0}, 14.0, 2);
    asset::TerrainField edited = whole;
    (void)asset::fillBall(edited, core::DVec3{10.0, 6.0, 10.0}, 14.0, 2);

    int compared = 0;
    eachKeptNode(10, [&](TerrainNodeKey key) {
        if (!terrainNodeReadsCells(resident, ground.source.get(), key))
            return;
        CAPTURE(key.level);
        CAPTURE(key.x);
        CAPTURE(key.z);
        const TerrainNodeBuild built = buildTerrainNodeFromCells(resident, *ground.source, key);
        CHECK(sameMesh(built.mesh, meshTerrainNode(edited, key)));
        // Another node than the one the ground on disk gives.
        CHECK(terrainNodeContent(resident, ground.source.get(), key) == built.content);
        ++compared;
    });
    CHECK(compared > 3);
    // Nothing of the edit went to disk: it is the field's until it is saved.
    CHECK(ground.fileWrites == fileWrites);
}

TEST_CASE("a file of the far ground reads back as it was written, and a broken one is refused")
{
    asset::TerrainField whole(Settings);
    (void)asset::fillFlat(whole, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 3.25f, 1);
    (void)asset::fillBall(whole, core::DVec3{20.0, 8.0, 20.0}, 9.0, 2);
    FarGround ground(whole);
    const asset::TerrainField nothing(Settings);
    eachKeptNode(4, [&](TerrainNodeKey key) { (void)buildTerrainNodeFromCells(nothing, *ground.source, key); });
    REQUIRE_FALSE(ground.files.empty());
    for (const auto& [at, bytes] : ground.files) {
        asset::PyramidNode node;
        REQUIRE_FALSE(asset::decodePyramidNode(bytes, node).has_value());
        CHECK(node.level == std::get<0>(at));
        CHECK(asset::encodePyramidNode(node) == bytes);
        // Cut short, or with a count that points outside it: refused, never
        // read past its end.
        asset::PyramidNode broken;
        CHECK(asset::decodePyramidNode(std::span<const std::byte>(bytes).first(bytes.size() - 3), broken).has_value());
        std::vector<std::byte> lying = bytes;
        // The count of cells, right after the nine words of the header.
        lying[36] = std::byte{0xFF};
        lying[37] = std::byte{0xFF};
        lying[38] = std::byte{0xFF};
        lying[39] = std::byte{0x7F};
        CHECK(asset::decodePyramidNode(lying, broken).has_value());
    }
}

TEST_CASE("a terrain saved before its rows said what a cell holds reads each cell once, and never again while its file "
          "stands")
{
    // An index from before ADR 0150 has no signatures. The first time a block
    // is brought up to date its cells are read to learn them -- and the block's
    // own file remembers each with its file's stamp, so the next run learns
    // them from there.
    asset::TerrainField whole(Settings);
    (void)asset::fillFlat(whole, core::DVec3{0.0, 0.0, 0.0}, 640.0f, 3.5f, 1);
    (void)asset::fillBall(whole, core::DVec3{40.0, 9.0, -30.0}, 11.0, 2);
    const std::vector<asset::TerrainCell> cells = asset::splitTerrain(whole);
    std::map<std::pair<core::i32, core::i32>, const asset::TerrainCell*> byId;
    std::vector<asset::ChunkId> ids;
    for (const asset::TerrainCell& cell : cells) {
        ids.push_back(asset::ChunkId{cell.x, cell.z, asset::FieldLayerTerrain});
        byId[{cell.x, cell.z}] = &cell;
    }
    std::map<std::tuple<core::u32, core::i32, core::i32>, std::vector<std::byte>> files;
    // What a cell's file is stamped with: other when the file is.
    std::map<std::pair<core::i32, core::i32>, core::u64> stamps;
    for (const asset::ChunkId id : ids)
        stamps[{id.x, id.z}] = 1000u + static_cast<core::u64>(id.x * 31 + id.z);
    const auto run = [&](int& cellReads, const std::function<void(asset::TerrainCellSource&)>& with) {
        const core::u32 cellChunks = asset::terrainCellChunks(Settings.voxelSize);
        asset::TerrainCellSource source(cellChunks, ids, [&](asset::ChunkId id) -> std::optional<asset::TerrainCell> {
            ++cellReads;
            const auto found = byId.find({id.x, id.z});
            return found == byId.end() ? std::nullopt : std::optional<asset::TerrainCell>(*found->second);
        });
        asset::TerrainPyramid::Store store;
        store.read = [&](core::u32 level, core::i32 x, core::i32 z, std::vector<std::byte>& out) {
            const auto found = files.find({level, x, z});
            if (found == files.end())
                return false;
            out = found->second;
            return true;
        };
        store.write = [&](core::u32 level, core::i32 x, core::i32 z, std::span<const std::byte> bytes) {
            files[{level, x, z}] = std::vector<std::byte>(bytes.begin(), bytes.end());
            return true;
        };
        source.setPyramid(std::make_shared<asset::TerrainPyramid>(Settings, cellChunks, std::move(store)));
        source.setStamper([&](asset::ChunkId id) { return stamps[{id.x, id.z}]; });
        with(source);
    };
    const asset::TerrainField nothing(Settings);
    const auto everyNode = [&](asset::TerrainCellSource& source) {
        eachKeptNode(10, [&](TerrainNodeKey key) {
            CHECK(sameMesh(buildTerrainNodeFromCells(nothing, source, key).mesh, meshTerrainNode(whole, key)));
        });
    };

    int first = 0;
    run(first, everyNode);
    CHECK(first > 0);
    // Another run: the rows still say nothing, the files are as they were.
    int second = 0;
    run(second, everyNode);
    CHECK(second == 0);
    // One cell's file is another file now -- written by something else -- and
    // that cell is read again to learn what it holds; no other is.
    stamps[{0, 0}] += 1;
    int third = 0;
    run(third, everyNode);
    CHECK(third >= 1);
    CHECK(third <= 2);
}
