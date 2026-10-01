#pragma once

// **A world larger than memory, laid a tile at a time** (ADR 0149 §2).
//
// `Terrain:WriteHeights`, a heightmap import and Generate Hills each lay one
// table, and a table is a thing in memory: 4 096 columns a side was where the
// editor stopped, and a world the size of a city is four times that each way.
// This lays the same ground from the same sources a tile at a time -- each
// tile with a column of its neighbours round it, so the slope at a seam is the
// one a single table would have had (`asset::HeightWindow`) -- and never holds
// more than the tile. What it lays behind it is the session cache's to write
// out (`FieldStreamer`), which is what bounds the memory; this only bounds the
// table.
//
// **Not undoable, and cancellable**: the history is the world as it was, and a
// world this size is not in it. Whoever runs one takes a snapshot first and
// opens a layer of the cache (`FieldStreamer::beginSessionLayer`); cancelling
// is putting both back.

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

struct lua_State;

namespace engine::scene {
class World;
struct TerrainComponent;
} // namespace engine::scene

namespace engine::app {

// Heights for any rectangle of a field's voxel columns.
class HeightSource
{
public:
    virtual ~HeightSource() = default;
    // Fills `out` -- `width` by `depth`, row after row along +z -- with the
    // height in FIELD metres of each column from (`x`, `z`). NaN is a column
    // with no ground, which is not laid. False, with why, when it cannot.
    [[nodiscard]] virtual bool read(core::i32 x, core::i32 z, core::u32 width, core::u32 depth, std::span<float> out,
                                    std::string& error) = 0;
};

// The rectangle of columns an import lays, in the field's voxel columns.
struct TerrainImportPlan
{
    core::i32 firstX = 0;
    core::i32 firstZ = 0;
    core::u32 columns = 0;
    core::u32 rows = 0;
    core::u8 material = 1;
    // Columns a side of one tile. On the chunk grid, so a chunk column is
    // laid by one tile and the covered-chunk shortcut sees all of it.
    core::u32 tile = 256;
};

// **A heightmap: one image, or a folder of tiles.** `columns` by `rows`
// columns from (`firstX`, `firstZ`) span the image corner to corner, as
// `asset::resampleHeights` maps them; black is `low` and white is `high`, in
// field metres.
//
// A folder is a set of images named `<anything>_x<column>_y<row>.<ext>`, all
// one size, which is how terrain tools export a world in pieces: each is
// decoded when a tile first reads it and a few are kept, so the set is never
// in memory whole. A piece that is missing is ground that is not laid. One
// image is decoded whole -- four bytes a pixel.
struct HeightmapSize
{
    core::u32 width = 0;
    core::u32 height = 0;
};
class HeightmapSource : public HeightSource
{
public:
    // The pixels it has across and down, which decide an import's
    // proportions: known once it is open, before any plan.
    [[nodiscard]] virtual HeightmapSize size() const noexcept = 0;
    // The columns it spans, and what black and white are.
    virtual void map(const TerrainImportPlan& plan, float low, float high) noexcept = 0;
};
[[nodiscard]] std::unique_ptr<HeightmapSource> openHeightmap(const std::filesystem::path& fileOrFolder,
                                                             std::string& error);
// Whether a file is one piece of a tiled heightmap, by its name. Choosing one
// brings the set: every file beside it with its name before `_x`, and its
// extension -- which is how a picker that picks files picks a set of them.
[[nodiscard]] bool isHeightmapPiece(const std::filesystem::path& file);

// **Hills from noise** (`asset::hillHeights`), which is a function of the
// column and the seed and so the same hills whatever the tiles are.
[[nodiscard]] std::unique_ptr<HeightSource> hillSource(const asset::FieldSettings& settings,
                                                       const asset::HillSettings& hills);

// **A Luau function of a place**: `source` is a chunk that returns
// `function(x: number, z: number): number`, world metres in and a world height
// out, called once a column on a sandboxed thread of `L`. `origin` and
// `voxel` are the terrain's. A chunk that does not compile, or returns
// something else, is refused here; a call that fails or answers no number
// fails the tile it was for.
[[nodiscard]] std::unique_ptr<HeightSource> functionSource(lua_State* L, std::string_view source,
                                                           std::string_view chunkName, core::DVec3 origin, float voxel,
                                                           std::string& error);

class TerrainImport
{
public:
    TerrainImport(std::unique_ptr<HeightSource> source, const TerrainImportPlan& plan);

    enum class Step : core::u8
    {
        More,
        Done,
        // The ground refused the tile: steeper than a table may be, or over
        // more streamed ground than loads at once. `error` is empty.
        Refused,
        // The source could not give the tile's heights. `error` says why.
        Failed,
    };
    // Lays the next tile, reading the ground under it first where it streams.
    [[nodiscard]] Step step(scene::World& world, scene::TerrainComponent& terrain, std::string& error);

    [[nodiscard]] core::u32 tiles() const noexcept { return m_tilesX * m_tilesZ; }
    [[nodiscard]] core::u32 laid() const noexcept { return m_next; }
    [[nodiscard]] float progress() const noexcept
    {
        return tiles() == 0 ? 1.0f : static_cast<float>(m_next) / static_cast<float>(tiles());
    }
    [[nodiscard]] bool done() const noexcept { return m_next >= tiles(); }
    // The middle of the tile `step` lays next, in the world: where the ground
    // is kept while the rest of it is written out.
    [[nodiscard]] core::DVec3 cursor(const scene::TerrainComponent& terrain) const noexcept;
    [[nodiscard]] const TerrainImportPlan& plan() const noexcept { return m_plan; }

private:
    std::unique_ptr<HeightSource> m_source;
    TerrainImportPlan m_plan;
    core::i32 m_tileX0 = 0;
    core::i32 m_tileZ0 = 0;
    core::u32 m_tilesX = 0;
    core::u32 m_tilesZ = 0;
    core::u32 m_next = 0;
    std::vector<float> m_table;
};

} // namespace engine::app
