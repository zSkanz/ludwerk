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
class TerrainCellSource
{
public:
    // Reads one cell from wherever it is kept; nothing when it cannot, or when
    // it is of other settings than the field's. Called from any thread.
    using Reader = std::function<std::optional<TerrainCell>(ChunkId id)>;
    // A cell's chunks as summaries (`TerrainChunk::summary`), by key.
    using Summaries = std::vector<TerrainField::Entry>;

    TerrainCellSource(core::u32 cellChunks, std::vector<ChunkId> cells, Reader read);

    // How many chunk columns a cell is on a side.
    [[nodiscard]] core::u32 cellChunks() const noexcept { return m_cellChunks; }
    // Every cell, by (x, z).
    [[nodiscard]] std::span<const ChunkId> cells() const noexcept { return m_cells; }
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
    core::u32 m_cellChunks = 1;
    std::vector<ChunkId> m_cells;
    Reader m_read;
    mutable std::mutex m_lock;
    mutable std::map<std::pair<core::i32, core::i32>, std::shared_ptr<const Summaries>> m_summaries;
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
