#pragma once

// **The far ground, kept** (ADR 0150).
//
// A coarse node of the terrain is the level-0 surface under it, gathered (ADR
// 0140), and where its ground is not resident the gathering read every cell
// under the node from disk (ADR 0144): a top-level node over a kilometre of
// metre voxels is 324 cells, a third of a second to decode and gather, done
// again whenever the node was needed again. On a world sixteen kilometres
// across that is what flying over it cost, and what the memory went to.
//
// **What is kept is the gathered surface itself**, per chunk and per level, in
// a file per node: the cells' vertices where the fine surface put them, their
// materials and their paint, and each coarse edge's crossings -- exactly what
// the mesher reads of a chunk at that level, as it was gathered. Never an
// average of voxels: a slab two metres thick and the material on top of the
// ground are in the file because they were in the gather. A node of the far
// ground is then one file of its own level and the eight round it, whatever
// it spans.
//
// **Levels 3, 4 and 5** -- cells of 8, 16 and 32 voxels. The two finer levels
// are four and sixteen times the data, and a node of either reaches so few
// cells that reading them is what it should do.
//
// **A block is a top-level node's footprint**, 32 chunk columns a side, and
// its twenty-one files are brought up to date together (`ensure`): the
// top-level file says what every cell under the block held when the block was
// gathered (`terrainCellSignature`), and a cell that holds something else now
// -- written out, saved, dug away -- has its columns, and the columns beside
// them, gathered again from that cell and the eight round it. Nothing else of
// the block is read.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "engine/asset/field_cells.h"
#include "engine/asset/terrain.h"
#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::asset {

// The first level kept on disk, and the last: the top of the quadtree.
inline constexpr core::u32 PyramidFirstLevel = 3;
inline constexpr core::u32 PyramidTopLevel = ChunkLevels - 1;

// One chunk as a node's file keeps it.
struct PyramidChunk
{
    ChunkKey key;
    // Whether the ground has a chunk here: a surface can sit in a chunk
    // nobody stored, beside one somebody did.
    bool stored = false;
    TerrainChunk::Kept kept;
    // Its surface at the node's level, as it was gathered: with nothing in
    // it where the chunk has none there -- which is kept too, so nobody
    // gathers it again to find that out.
    std::shared_ptr<const SurfaceLevel> surface;
};

struct PyramidCell
{
    core::i32 x = 0;
    core::i32 z = 0;
    core::u64 signature = 0;
    // Its file's stamp when it was read (`TerrainCellSource::stampOf`), zero
    // where there was none.
    core::u64 stamp = 0;
};

// A node's file: the chunks in its footprint, in key order.
struct PyramidNode
{
    core::u32 level = 0;
    core::i32 x = 0;
    core::i32 z = 0;
    FieldSettings settings;
    // **Top level only.** What every cell under the block and in the ring
    // round it held when the block was gathered, by (x, z); and which of the
    // block's finer nodes have a file -- bit `z * 2 + x` of the low four for
    // level 4, bit `4 + z * 4 + x` for level 3. A node of no ground has none.
    std::vector<PyramidCell> cells;
    core::u32 files = 0;
    std::vector<PyramidChunk> chunks;
};

[[nodiscard]] std::vector<std::byte> encodePyramidNode(const PyramidNode& node);
[[nodiscard]] std::optional<core::EngineError> decodePyramidNode(std::span<const std::byte> bytes, PyramidNode& out);

class TerrainPyramid
{
public:
    // Where the files are. `read` answers false for one that is not there.
    struct Store
    {
        std::function<bool(core::u32 level, core::i32 x, core::i32 z, std::vector<std::byte>& out)> read;
        std::function<bool(core::u32 level, core::i32 x, core::i32 z, std::span<const std::byte> bytes)> write;
    };

    TerrainPyramid(FieldSettings settings, core::u32 cellChunks, Store store);

    [[nodiscard]] const FieldSettings& settings() const noexcept { return m_settings; }

    // **Brings one block's files up to date** with the cells as `source` has
    // them now, reading the cells that changed and those round them, and no
    // other. True when the block is current afterwards -- at once, and
    // without a read, when it already was. `wide` gathers on every worker:
    // for whoever builds many blocks from a thread that is not one.
    bool ensure(const TerrainCellSource& source, core::i32 blockX, core::i32 blockZ, bool wide = false);
    // Whether `ensure` would find nothing to do.
    [[nodiscard]] bool current(const TerrainCellSource& source, core::i32 blockX, core::i32 blockZ);

    // **Puts into `field` what the files hold of the chunk columns `[x0, x1]`
    // by `[z0, z1]`**: a summary for every chunk kept there that the field
    // does not hold, in columns `summaryHere` allows -- not where a cell is
    // resident, whose missing chunk was dug away -- and, given to the field
    // (`adoptSurface`), each chunk's surface at `level` and at every level
    // above, in columns `surfaceHere` allows. False when a file a block says
    // it has is not there: the caller reads the cells instead. The blocks it
    // touches must be current (`ensure`).
    using ColumnTest = std::function<bool(core::i32 chunkX, core::i32 chunkZ)>;
    [[nodiscard]] bool seed(TerrainField& field, core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1,
                            core::u32 level, const ColumnTest& summaryHere, const ColumnTest& surfaceHere);

    struct Stats
    {
        // Files decoded, files written, cells read whole, chunks gathered,
        // and blocks `ensure` had to touch.
        core::u64 filesRead = 0;
        core::u64 filesWritten = 0;
        core::u64 cellsRead = 0;
        core::u64 chunksGathered = 0;
        core::u64 blocksBuilt = 0;
    };
    [[nodiscard]] Stats stats() const;

    // A node's file, decoded: kept for the next to ask, a bounded number of
    // them. Null when there is none, or it is of other settings.
    [[nodiscard]] std::shared_ptr<const PyramidNode> node(core::u32 level, core::i32 x, core::i32 z);

    // How many decoded files are kept. Sixty-four megabytes at the size they
    // are on a metre voxel.
    static constexpr core::usize NodesKept = 384;

private:
    using NodeKey = std::tuple<core::u32, core::i32, core::i32>;
    void keep(const NodeKey& key, std::shared_ptr<const PyramidNode> node);
    // The cells that hold something else than the block kept, as columns to
    // gather again; nothing when it is current.
    struct Stale;
    [[nodiscard]] bool staleOf(const TerrainCellSource& source, core::i32 blockX, core::i32 blockZ, bool learn,
                               Stale& out);

    FieldSettings m_settings;
    core::u32 m_cellChunks = 1;
    Store m_store;
    // **A lock a block**: one block is brought up to date by one thread at a
    // time, and two blocks by two. With one lock for the terrain, a node of
    // the far ground waited for whatever block the background was building
    // -- seconds, for ground it did not read.
    std::map<std::pair<core::i32, core::i32>, std::shared_ptr<std::mutex>> m_blockLocks;
    // The decoded files, and the order they were last asked for in.
    mutable std::mutex m_lock;
    struct Kept
    {
        std::shared_ptr<const PyramidNode> node;
        core::u64 used = 0;
    };
    std::map<NodeKey, Kept> m_nodes;
    core::u64 m_clock = 0;
    Stats m_stats;
};

} // namespace engine::asset
