// Terrain and block worlds cut into streaming cells (ADR 0075).
//
// **A cell is a vertical column of the world about 64 metres square**, the size
// `Terrain.CellSize` has always named. The ground and the blocks inside it are
// one file each, keyed by an `asset::ChunkId` on a grid of their own -- not the
// parts' 256 m one -- so a cell of ground is something a streaming manager can
// score, load, materialise and evict with the same policy a cell of parts gets.
//
// Everything here is a pure function of its input, so a partition run twice
// writes the same bytes, and in `ChunkId` order, so what it writes is a fact
// about the world rather than about a walk (R10).
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/asset/chunk.h"
#include "engine/asset/terrain.h"
#include "engine/asset/terrain_cell.h"
#include "engine/asset/voxel.h"
#include "engine/core/error.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::asset {

// The side of a field cell, in metres, before it is rounded to whole tiles or
// whole chunks.
inline constexpr core::f64 FieldCellMetres = 64.0;

// The `ChunkId::layer` each kind of field cell is filed under, in the field
// index. Two kinds on one grid, told apart the way the parts' size classes are.
inline constexpr core::i32 FieldLayerTerrain = 0;
inline constexpr core::i32 FieldLayerVoxels = 1;

// --- Terrain ---------------------------------------------------------------

// How many chunk columns on a side a cell holds at this voxel size: the whole
// count nearest `cellMetres`, never fewer than one. A 64 m cell is four columns
// of chunks at half a metre and two at a metre; at two metres a chunk is
// already 64 m, and the cell is the chunk column.
[[nodiscard]] core::u32 terrainCellChunks(core::f32 voxelSize, core::f64 cellMetres = FieldCellMetres) noexcept;

// Which cell a chunk belongs to: every chunk of its column, whatever its `y`,
// so a cave is never split from the ground it is dug into.
[[nodiscard]] ChunkId terrainCellOf(ChunkKey key, core::u32 cellChunks) noexcept;

// The whole field cut into cells, in `ChunkId` order. The field inside each
// cell holds absolute keys and SHARES the chunks: this runs once, at a
// partition, and what it produces is written to disk and dropped.
[[nodiscard]] std::vector<TerrainCell> splitTerrain(const TerrainField& field, core::f64 cellMetres = FieldCellMetres);

// The world-space box a cell covers: its square, and the field's whole height
// range, both offset by where the terrain sits.
[[nodiscard]] core::DAABB terrainCellBounds(const TerrainCell& cell, core::u32 cellChunks, core::DVec3 origin) noexcept;

// **Whether `field` still holds exactly what `cell` put into it**, within the
// cell's footprint: every chunk the same one `cell` shares, and nothing
// there that `cell` did not bring. False is a cell somebody edited, which an
// evicting streamer must keep -- dropping it would drop the edit.
[[nodiscard]] bool terrainCellUntouched(const TerrainField& field, const TerrainCell& cell,
                                        core::u32 cellChunks) noexcept;

// Takes a cell's chunks back out of `field`.
void removeTerrainCell(TerrainField& field, const TerrainCell& cell);

// **What a cell holds, as one number** (ADR 0150): the keys and digests of the
// chunks in its columns, in key order -- never zero, which stands for a cell
// nobody has read. The same from a cell's own field and from a field that
// holds the cell's ground with the ground round it, so what a node of the far
// ground is built from can be named by its cells whether they are resident or
// not: the same number while a cell comes in and goes out untouched, and
// another the moment somebody edits it.
[[nodiscard]] core::u64 terrainCellSignature(const TerrainField& field, core::i32 cellX, core::i32 cellZ,
                                             core::u32 cellChunks) noexcept;

// --- A streamed terrain's cells, for drawing (ADR 0144) -----------------------

// **The cells a streamed terrain is made of, readable from any thread**: what
// the renderer draws the ground from where its cells are not resident. The
// streamer makes one when it adopts a terrain's index; nothing but drawing
// reads it.
//
// **It remembers the digest of every chunk it has read**, so what a node of the
// ground is built from does not change as its cells come in and go out: a
// resident chunk and the one read from its cell are the same chunk, and the
// node keyed on either is the same node.
class TerrainPyramid;

class TerrainCellSource
{
public:
    // Reads one cell from wherever it is kept; nothing when it cannot, or when
    // it is of other settings than the field's. Called from any thread.
    using Reader = std::function<std::optional<TerrainCell>(ChunkId id)>;
    // A cell's chunks as summaries (`TerrainChunk::summary`), by key.
    using Summaries = std::vector<TerrainField::Entry>;

    TerrainCellSource(core::u32 cellChunks, std::vector<ChunkId> cells, Reader read);
    // The same, with what each cell holds (`terrainCellSignature`) where it
    // is known: parallel to `cells`, zero for a cell nobody has read.
    TerrainCellSource(core::u32 cellChunks, std::vector<ChunkId> cells, std::vector<core::u64> signatures, Reader read);

    // **Cells whose file is another now, and cells that are gone** (ADR 0149):
    // a cell written to the session cache, a cell a save wrote, a cell dug to
    // nothing. What was kept of each is forgotten -- it is read again when it
    // is next asked for -- and a cell not known before is one now. Every
    // other cell keeps its summaries: a source made again for one cell's sake
    // read the whole far ground again, and with ground written out every
    // frame that was every frame. From the thread that owns the terrain;
    // every reader below may run beside it.
    // `signatures`, parallel to `changed`, is what each now holds where
    // whoever changed it knows -- it wrote the cell -- and may be shorter:
    // a cell with none is read to learn it.
    void update(std::span<const ChunkId> changed, std::span<const ChunkId> gone,
                std::span<const core::u64> signatures = {});

    // **What each cell over the chunk columns `[x0, x1] x [z0, z1]` holds**
    // (`terrainCellSignature`), by (x, z); zero for one nobody has read. What
    // a node's content is made of (ADR 0150): asked every frame of every node
    // drawn, so it is a walk of the cells and nothing else.
    void signaturesIn(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1,
                      std::vector<std::pair<ChunkId, core::u64>>& out) const;
    // What one cell holds, or zero.
    [[nodiscard]] core::u64 signature(ChunkId id) const;
    // **Moves whenever a cell changes, comes, goes or is first read**: while
    // it stands, every signature is what it was, and whoever named something
    // by them need not ask again.
    [[nodiscard]] core::u64 revision() const noexcept { return m_revision.load(std::memory_order_acquire); }
    // Learnt by whoever read the cell. Kept unless the cell changed meanwhile:
    // a signature is never put over a newer one's.
    void learnSignature(ChunkId id, core::u64 signature) const;

    // **The far ground's files** (ADR 0150), where the terrain has them: what
    // a coarse node is built from in place of the cells under it. Set once,
    // by whoever made the source, before anything draws from it.
    void setPyramid(std::shared_ptr<TerrainPyramid> pyramid) noexcept { m_pyramid = std::move(pyramid); }
    [[nodiscard]] TerrainPyramid* pyramid() const noexcept { return m_pyramid.get(); }

    // **A cell's file as one number that changes when the file does** -- its
    // size and when it was written -- or zero where that is not known. For a
    // cell whose row does not say what it holds, an index from before ADR
    // 0150: the far ground's files remember what such a cell held and this,
    // and while this is the same the cell need not be read to learn it again.
    using Stamper = std::function<core::u64(ChunkId)>;
    void setStamper(Stamper stamper) { m_stamper = std::move(stamper); }
    [[nodiscard]] core::u64 stampOf(ChunkId id) const { return m_stamper ? m_stamper(id) : 0; }

    // How many cells' summaries are kept at once: the least lately asked for
    // go. A summary is read again in a millisecond; kept for every cell ever
    // seen, they were what a flight over a large world held on to.
    static constexpr core::usize SummariesKept = 1024;

    // How many chunk columns a cell is on a side.
    [[nodiscard]] core::u32 cellChunks() const noexcept { return m_cellChunks; }
    // Every cell, by (x, z).
    [[nodiscard]] std::vector<ChunkId> cells() const;
    // The chunk columns the cells cover, inclusive: x low, x high, z low, z
    // high. Nothing when there are no cells.
    [[nodiscard]] std::optional<std::array<core::i32, 4>> extent() const noexcept;
    // The cells over the chunk columns `[x0, x1] x [z0, z1]`, by (x, z).
    void cellsIn(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1, std::vector<ChunkId>& out) const;
    [[nodiscard]] bool covers(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1) const noexcept;

    // One cell, read now, voxels and all: nothing of it is kept here.
    [[nodiscard]] std::optional<TerrainCell> read(ChunkId id) const;
    // **A cell's chunks as summaries**, read the first time they are asked for
    // and kept: a few hundred bytes a chunk, where its voxels are tens of
    // kilobytes. Null for a cell that cannot be read.
    [[nodiscard]] std::shared_ptr<const Summaries> summaries(ChunkId id) const;
    // The digests of the chunks in the columns `[x0, x1] x [z0, z1]` of every
    // cell summarised so far, in key order.
    void digestsIn(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1,
                   std::vector<std::pair<ChunkKey, core::u64>>& out) const;
    // Whether every cell over those columns has been summarised.
    [[nodiscard]] bool summarised(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1) const;

private:
    // `cellsIn`, by whoever holds the lock.
    void cellsWithin(core::i32 x0, core::i32 x1, core::i32 z0, core::i32 z1, std::vector<ChunkId>& out) const;

    core::u32 m_cellChunks = 1;
    Reader m_read;
    // Guards the cells and what is kept of them.
    mutable std::mutex m_lock;
    std::vector<ChunkId> m_cells;
    // Parallel to `m_cells`: what each holds, zero where nobody has read it.
    mutable std::vector<core::u64> m_signatures;
    // The lowest and highest z of any cell, kept as the cells change: asked
    // every frame, and a walk of every cell when it was worked out each time.
    // Never narrowed when a cell goes -- a box a little wide costs nothing.
    core::i32 m_lowZ = 0;
    core::i32 m_highZ = -1;
    mutable std::atomic<core::u64> m_revision{1};
    struct KeptSummaries
    {
        std::shared_ptr<const Summaries> summaries;
        core::u64 used = 0;
    };
    mutable std::map<std::pair<core::i32, core::i32>, KeptSummaries> m_summaries;
    mutable core::u64 m_summaryClock = 0;
    std::shared_ptr<TerrainPyramid> m_pyramid;
    Stamper m_stamper;
};

// --- Block worlds ------------------------------------------------------------

// How many 16-block chunks on a side a cell holds at this block size, on
// `terrainCellChunks`' terms.
[[nodiscard]] core::u32 voxelCellChunks(core::f32 blockSize, core::f64 cellMetres = FieldCellMetres) noexcept;

// Which cell a chunk belongs to: every chunk of the column, whatever its `y`.
[[nodiscard]] ChunkId voxelCellOf(VoxelChunkKey key, core::u32 cellChunks) noexcept;

// One cell's worth of blocks.
struct VoxelCell
{
    core::i32 x = 0;
    core::i32 z = 0;
    core::f32 blockSize = 1.0f;
    VoxelGrid grid;
};

// The whole grid cut into cells, in `ChunkId` order, on `splitTerrain`'s terms.
[[nodiscard]] std::vector<VoxelCell> splitVoxels(const VoxelGrid& grid, core::f32 blockSize,
                                                 core::f64 cellMetres = FieldCellMetres);

// Its square, and the height its chunks actually span.
[[nodiscard]] core::DAABB voxelCellBounds(const VoxelCell& cell, core::u32 cellChunks) noexcept;

// On `terrainCellUntouched`'s terms.
[[nodiscard]] bool voxelCellUntouched(const VoxelGrid& grid, const VoxelCell& cell, core::u32 cellChunks) noexcept;
void removeVoxelCell(VoxelGrid& grid, const VoxelCell& cell);

// **The `.lvoxel` cell file.** Little-endian words: magic `LGVC`, version,
// x, z, the block size's bits and a chunk count; then per chunk its key's three
// words, a byte count and `encodeVoxelChunk`'s run-coded bytes. Keys in sorted
// order, which the decoder requires, so a file has one reading.
inline constexpr core::u32 VoxelCellFormatVersion = 1;
// A corrupt count must not be able to ask for a gigabyte: a 64 m cell of
// one-metre blocks is sixteen columns of chunks, and 65,536 is room for every
// one of them to be 4,096 chunks tall.
inline constexpr core::u32 MaxVoxelCellChunks = 65536;

[[nodiscard]] std::vector<std::byte> encodeVoxelCell(const VoxelCell& cell);
[[nodiscard]] std::optional<core::EngineError> decodeVoxelCell(std::span<const std::byte> bytes, VoxelCell& out);

} // namespace engine::asset
