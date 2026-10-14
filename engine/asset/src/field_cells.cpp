#include "engine/asset/field_cells.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#include "engine/core/i18n.h"

namespace engine::asset {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::usize;

[[nodiscard]] i32 floorDivide(i32 value, i32 divisor) noexcept
{
    const i32 quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

[[nodiscard]] u32 cellsAcross(f64 unitMetres, f64 cellMetres) noexcept
{
    if (!(unitMetres > 0.0) || !(cellMetres > 0.0))
        return 1;
    const f64 across = std::round(cellMetres / unitMetres);
    return across < 1.0 ? 1u : static_cast<u32>(across);
}

// --- The `.lvoxel` writer and reader ----------------------------------------

constexpr u32 VoxelCellMagic = 0x4356474Cu; // "LGVC", little-endian

void putWord(std::vector<std::byte>& out, u32 value)
{
    for (u32 shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] u32 word() noexcept
    {
        if (m_at + 4 > m_bytes.size()) {
            m_ok = false;
            return 0;
        }
        u32 value = 0;
        for (u32 index = 0; index < 4; ++index)
            value |= static_cast<u32>(m_bytes[m_at + index]) << (8u * index);
        m_at += 4;
        return value;
    }

    [[nodiscard]] std::span<const std::byte> take(usize count) noexcept
    {
        if (count > m_bytes.size() - std::min(m_at, m_bytes.size())) {
            m_ok = false;
            return {};
        }
        const std::span<const std::byte> taken = m_bytes.subspan(m_at, count);
        m_at += count;
        return taken;
    }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool finished() const noexcept { return m_at == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    usize m_at = 0;
    bool m_ok = true;
};

[[nodiscard]] core::EngineError malformedVoxels()
{
    return core::makeError(ENG_TR("asset.voxel.err.cell_malformed"));
}

} // namespace

// --- Terrain -----------------------------------------------------------------

u32 terrainCellChunks(f32 voxelSize, f64 cellMetres) noexcept
{
    return cellsAcross(static_cast<f64>(ChunkEdge) * static_cast<f64>(voxelSize), cellMetres);
}

ChunkId terrainCellOf(ChunkKey key, u32 cellChunks) noexcept
{
    const auto across = static_cast<i32>(cellChunks);
    return ChunkId{floorDivide(key.x, across), floorDivide(key.z, across), FieldLayerTerrain};
}

std::vector<TerrainCell> splitTerrain(const TerrainField& field, f64 cellMetres)
{
    const u32 cellChunks = terrainCellChunks(field.settings().voxelSize, cellMetres);
    std::map<ChunkId, TerrainField> cells;
    // Chunks are shared into the cells rather than copied: they are immutable
    // to everyone but a field that holds them alone.
    for (const TerrainField::Entry& entry : field.chunks()) {
        const ChunkId id = terrainCellOf(entry.first, cellChunks);
        auto at = cells.find(id);
        if (at == cells.end())
            at = cells.emplace(id, TerrainField(field.settings())).first;
        at->second.setChunk(entry.first, entry.second);
    }

    std::vector<TerrainCell> out;
    out.reserve(cells.size());
    for (auto& entry : cells) {
        TerrainCell cell;
        cell.x = entry.first.x;
        cell.z = entry.first.z;
        cell.settings = field.settings();
        cell.field = std::move(entry.second);
        out.push_back(std::move(cell));
    }
    return out;
}

core::DAABB terrainCellBounds(const TerrainCell& cell, u32 cellChunks, core::DVec3 origin) noexcept
{
    const f64 side =
        static_cast<f64>(cellChunks) * static_cast<f64>(ChunkEdge) * static_cast<f64>(cell.settings.voxelSize);
    core::DAABB bounds;
    bounds.min =
        core::DVec3{origin.x + static_cast<f64>(cell.x) * side, origin.y + static_cast<f64>(cell.settings.minHeight),
                    origin.z + static_cast<f64>(cell.z) * side};
    bounds.max =
        core::DVec3{bounds.min.x + side, origin.y + static_cast<f64>(cell.settings.maxHeight), bounds.min.z + side};
    return bounds;
}

bool terrainCellUntouched(const TerrainField& field, const TerrainCell& cell, u32 cellChunks) noexcept
{
    const auto across = static_cast<i32>(cellChunks);
    // Every chunk column of the cell's square: the field holds exactly the
    // chunks the cell brought -- the very objects it shared, or ones with the
    // same bytes. The second is an undo (ADR 0087): a snapshot hands back the
    // chunks it kept, which are the same ground in other objects, and a cell
    // compared by identity alone would never be let go again.
    for (i32 z = cell.z * across; z < (cell.z + 1) * across; ++z) {
        for (i32 x = cell.x * across; x < (cell.x + 1) * across; ++x) {
            const std::span<const TerrainField::Entry> held = field.column(x, z);
            const std::span<const TerrainField::Entry> brought = cell.field.column(x, z);
            if (held.size() != brought.size())
                return false;
            for (usize at = 0; at < held.size(); ++at) {
                if (!(held[at].first == brought[at].first))
                    return false;
                if (held[at].second != brought[at].second && held[at].second->digest() != brought[at].second->digest())
                    return false;
            }
        }
    }
    return true;
}

core::u64 terrainCellSignature(const TerrainField& field, i32 cellX, i32 cellZ, u32 cellChunks) noexcept
{
    // Order-sensitive, over the chunks in key order: x, then z, then y.
    const auto mix = [](core::u64 seed, core::u64 value) noexcept {
        core::u64 z = seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const auto across = static_cast<i32>(std::max<u32>(cellChunks, 1u));
    core::u64 held = 0x7E88A1C0DE5EED01ull;
    for (i32 x = cellX * across; x < (cellX + 1) * across; ++x) {
        for (i32 z = cellZ * across; z < (cellZ + 1) * across; ++z) {
            for (const TerrainField::Entry& entry : field.column(x, z)) {
                held = mix(held, static_cast<core::u64>(static_cast<u32>(entry.first.x)));
                held = mix(held, static_cast<core::u64>(static_cast<u32>(entry.first.y)));
                held = mix(held, static_cast<core::u64>(static_cast<u32>(entry.first.z)));
                held = mix(held, entry.second->digest());
            }
        }
    }
    return held == 0 ? 1 : held;
}

void removeTerrainCell(TerrainField& field, const TerrainCell& cell)
{
    const std::vector<ChunkKey> keys = cell.field.chunkKeys();
    field.removeAll(keys);
}

// --- Block worlds ------------------------------------------------------------

u32 voxelCellChunks(f32 blockSize, f64 cellMetres) noexcept
{
    return cellsAcross(static_cast<f64>(VoxelChunkEdge) * static_cast<f64>(blockSize), cellMetres);
}

ChunkId voxelCellOf(VoxelChunkKey key, u32 cellChunks) noexcept
{
    const auto across = static_cast<i32>(cellChunks);
    return ChunkId{floorDivide(key.x, across), floorDivide(key.z, across), FieldLayerVoxels};
}

std::vector<VoxelCell> splitVoxels(const VoxelGrid& grid, f32 blockSize, f64 cellMetres)
{
    const u32 cellChunks = voxelCellChunks(blockSize, cellMetres);
    std::map<ChunkId, VoxelGrid> cells;
    for (const VoxelChunkKey key : grid.chunkKeys()) {
        const VoxelChunk* chunk = grid.findChunk(key);
        cells[voxelCellOf(key, cellChunks)].setChunk(key, std::span<const BlockId>(chunk->blocks, VoxelChunkVolume));
    }
    std::vector<VoxelCell> out;
    out.reserve(cells.size());
    for (auto& entry : cells) {
        VoxelCell cell;
        cell.x = entry.first.x;
        cell.z = entry.first.z;
        cell.blockSize = blockSize;
        cell.grid = std::move(entry.second);
        out.push_back(std::move(cell));
    }
    return out;
}

core::DAABB voxelCellBounds(const VoxelCell& cell, u32 cellChunks) noexcept
{
    const f64 chunkMetres = static_cast<f64>(VoxelChunkEdge) * static_cast<f64>(cell.blockSize);
    const f64 side = static_cast<f64>(cellChunks) * chunkMetres;
    i32 lowest = 0;
    i32 highest = 0;
    bool any = false;
    for (const VoxelChunkKey key : cell.grid.chunkKeys()) {
        lowest = any ? std::min(lowest, key.y) : key.y;
        highest = any ? std::max(highest, key.y) : key.y;
        any = true;
    }
    core::DAABB bounds;
    bounds.min = core::DVec3{static_cast<f64>(cell.x) * side, static_cast<f64>(lowest) * chunkMetres,
                             static_cast<f64>(cell.z) * side};
    bounds.max = core::DVec3{bounds.min.x + side, static_cast<f64>(highest + 1) * chunkMetres, bounds.min.z + side};
    return bounds;
}

bool voxelCellUntouched(const VoxelGrid& grid, const VoxelCell& cell, u32 cellChunks) noexcept
{
    const ChunkId id{cell.x, cell.z, FieldLayerVoxels};
    for (const VoxelChunkKey key : grid.chunkKeys()) {
        if (!(voxelCellOf(key, cellChunks) == id))
            continue;
        if (grid.findChunk(key) != cell.grid.findChunk(key))
            return false;
    }
    for (const VoxelChunkKey key : cell.grid.chunkKeys()) {
        if (grid.findChunk(key) == nullptr)
            return false;
    }
    return true;
}

void removeVoxelCell(VoxelGrid& grid, const VoxelCell& cell)
{
    const std::vector<VoxelChunkKey> keys = cell.grid.chunkKeys();
    grid.removeAll(keys);
}

std::vector<std::byte> encodeVoxelCell(const VoxelCell& cell)
{
    std::vector<std::byte> out;
    const std::vector<VoxelChunkKey> keys = cell.grid.chunkKeys();
    putWord(out, VoxelCellMagic);
    putWord(out, VoxelCellFormatVersion);
    putWord(out, static_cast<u32>(cell.x));
    putWord(out, static_cast<u32>(cell.z));
    u32 sizeBits = 0;
    std::memcpy(&sizeBits, &cell.blockSize, sizeof(sizeBits));
    putWord(out, sizeBits);
    putWord(out, static_cast<u32>(keys.size()));
    for (const VoxelChunkKey key : keys) {
        const std::vector<u8> runs = encodeVoxelChunk(*cell.grid.findChunk(key));
        putWord(out, static_cast<u32>(key.x));
        putWord(out, static_cast<u32>(key.y));
        putWord(out, static_cast<u32>(key.z));
        putWord(out, static_cast<u32>(runs.size()));
        for (const u8 byte : runs)
            out.push_back(static_cast<std::byte>(byte));
    }
    return out;
}

std::optional<core::EngineError> decodeVoxelCell(std::span<const std::byte> bytes, VoxelCell& out)
{
    Reader reader(bytes);
    if (reader.word() != VoxelCellMagic)
        return malformedVoxels();
    if (const u32 version = reader.word(); version != VoxelCellFormatVersion) {
        const core::I18nArg args[] = {{"expected", static_cast<core::i64>(VoxelCellFormatVersion)},
                                      {"found", static_cast<core::i64>(version)}};
        return core::makeError(ENG_TR("asset.voxel.err.cell_version"), args);
    }
    VoxelCell cell;
    cell.x = static_cast<i32>(reader.word());
    cell.z = static_cast<i32>(reader.word());
    const u32 sizeBits = reader.word();
    std::memcpy(&cell.blockSize, &sizeBits, sizeof(sizeBits));
    const u32 count = reader.word();
    if (!reader.ok() || count > MaxVoxelCellChunks || !(cell.blockSize > 0.0f))
        return malformedVoxels();

    std::vector<BlockId> blocks;
    bool first = true;
    VoxelChunkKey previous;
    for (u32 index = 0; index < count; ++index) {
        VoxelChunkKey key;
        key.x = static_cast<i32>(reader.word());
        key.y = static_cast<i32>(reader.word());
        key.z = static_cast<i32>(reader.word());
        const u32 length = reader.word();
        const std::span<const std::byte> runs = reader.take(length);
        if (!reader.ok() || (!first && !(previous < key)))
            return malformedVoxels();
        const std::span<const u8> view(reinterpret_cast<const u8*>(runs.data()), runs.size());
        if (!decodeVoxelChunk(view, blocks))
            return malformedVoxels();
        cell.grid.setChunk(key, blocks);
        previous = key;
        first = false;
    }
    if (!reader.finished())
        return malformedVoxels();
    out = std::move(cell);
    return std::nullopt;
}

// --- A streamed terrain's cells, for drawing (ADR 0144) -----------------------

namespace {

// The order the cells are kept in: by x, then z.
[[nodiscard]] bool cellBefore(const ChunkId& a, const ChunkId& b) noexcept
{
    return a.x != b.x ? a.x < b.x : a.z < b.z;
}

} // namespace

TerrainCellSource::TerrainCellSource(u32 cellChunks, std::vector<ChunkId> cells, Reader read)
    : TerrainCellSource(cellChunks, std::move(cells), {}, std::move(read))
{}

TerrainCellSource::TerrainCellSource(u32 cellChunks, std::vector<ChunkId> cells, std::vector<core::u64> signatures,
                                     Reader read)
    : m_cellChunks(std::max<u32>(cellChunks, 1u)), m_read(std::move(read))
{
    // Sorted together: a signature stays its cell's.
    signatures.resize(cells.size(), 0);
    std::vector<usize> order(cells.size());
    for (usize at = 0; at < order.size(); ++at)
        order[at] = at;
    std::sort(order.begin(), order.end(), [&](usize a, usize b) { return cellBefore(cells[a], cells[b]); });
    m_cells.reserve(cells.size());
    m_signatures.reserve(cells.size());
    for (const usize at : order) {
        m_cells.push_back(cells[at]);
        m_signatures.push_back(signatures[at]);
    }
    bool any = false;
    for (const ChunkId& cell : m_cells) {
        m_lowZ = any ? std::min(m_lowZ, cell.z) : cell.z;
        m_highZ = any ? std::max(m_highZ, cell.z) : cell.z;
        any = true;
    }
}

void TerrainCellSource::update(std::span<const ChunkId> changed, std::span<const ChunkId> gone,
                               std::span<const core::u64> signatures)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_revision.fetch_add(1, std::memory_order_acq_rel);
    for (const ChunkId& cell : gone) {
        const auto at = std::lower_bound(m_cells.begin(), m_cells.end(), cell, cellBefore);
        if (at != m_cells.end() && at->x == cell.x && at->z == cell.z) {
            m_signatures.erase(m_signatures.begin() + (at - m_cells.begin()));
            m_cells.erase(at);
        }
        m_summaries.erase({cell.x, cell.z});
    }
    for (usize index = 0; index < changed.size(); ++index) {
        const ChunkId& cell = changed[index];
        const core::u64 signature = index < signatures.size() ? signatures[index] : 0;
        auto at = std::lower_bound(m_cells.begin(), m_cells.end(), cell, cellBefore);
        if (at == m_cells.end() || at->x != cell.x || at->z != cell.z) {
            const auto offset = at - m_cells.begin();
            m_lowZ = m_cells.empty() ? cell.z : std::min(m_lowZ, cell.z);
            m_highZ = m_cells.empty() ? cell.z : std::max(m_highZ, cell.z);
            m_cells.insert(at, cell);
            m_signatures.insert(m_signatures.begin() + offset, signature);
        }
        else {
            m_signatures[static_cast<usize>(at - m_cells.begin())] = signature;
        }
        m_summaries.erase({cell.x, cell.z});
    }
}

void TerrainCellSource::signaturesIn(i32 x0, i32 x1, i32 z0, i32 z1,
                                     std::vector<std::pair<ChunkId, core::u64>>& out) const
{
    const auto n = static_cast<i32>(m_cellChunks);
    const i32 cx0 = floorDivide(x0, n);
    const i32 cx1 = floorDivide(x1, n);
    const i32 cz0 = floorDivide(z0, n);
    const i32 cz1 = floorDivide(z1, n);
    const std::lock_guard<std::mutex> lock(m_lock);
    for (i32 x = cx0; x <= cx1; ++x) {
        auto at = std::lower_bound(m_cells.begin(), m_cells.end(), ChunkId{x, cz0, FieldLayerTerrain}, cellBefore);
        for (; at != m_cells.end() && at->x == x && at->z <= cz1; ++at)
            out.emplace_back(*at, m_signatures[static_cast<usize>(at - m_cells.begin())]);
    }
}

core::u64 TerrainCellSource::signature(ChunkId id) const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    const auto at = std::lower_bound(m_cells.begin(), m_cells.end(), id, cellBefore);
    if (at == m_cells.end() || at->x != id.x || at->z != id.z)
        return 0;
    return m_signatures[static_cast<usize>(at - m_cells.begin())];
}

void TerrainCellSource::learnSignature(ChunkId id, core::u64 signature) const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    const auto at = std::lower_bound(m_cells.begin(), m_cells.end(), id, cellBefore);
    if (at == m_cells.end() || at->x != id.x || at->z != id.z)
        return;
    core::u64& known = m_signatures[static_cast<usize>(at - m_cells.begin())];
    if (known == 0) {
        known = signature;
        m_revision.fetch_add(1, std::memory_order_acq_rel);
    }
}

std::vector<ChunkId> TerrainCellSource::cells() const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    return m_cells;
}

std::optional<std::array<i32, 4>> TerrainCellSource::extent() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);
    if (m_cells.empty())
        return std::nullopt;
    const auto n = static_cast<i32>(m_cellChunks);
    return std::array<i32, 4>{m_cells.front().x * n, m_cells.back().x * n + n - 1, m_lowZ * n, m_highZ * n + n - 1};
}

void TerrainCellSource::cellsIn(i32 x0, i32 x1, i32 z0, i32 z1, std::vector<ChunkId>& out) const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    cellsWithin(x0, x1, z0, z1, out);
}

void TerrainCellSource::cellsWithin(i32 x0, i32 x1, i32 z0, i32 z1, std::vector<ChunkId>& out) const
{
    const auto n = static_cast<i32>(m_cellChunks);
    const i32 cx0 = floorDivide(x0, n);
    const i32 cx1 = floorDivide(x1, n);
    const i32 cz0 = floorDivide(z0, n);
    const i32 cz1 = floorDivide(z1, n);
    // **A search a column of cells, not a walk of it**: the cells are sorted by
    // x, then z, and a world sixteen kilometres across has 256 in each column
    // -- walked for every node of the selection every frame, that was the
    // frame.
    for (i32 x = cx0; x <= cx1; ++x) {
        auto at = std::lower_bound(m_cells.begin(), m_cells.end(), ChunkId{x, cz0, FieldLayerTerrain}, cellBefore);
        for (; at != m_cells.end() && at->x == x && at->z <= cz1; ++at)
            out.push_back(*at);
    }
}

bool TerrainCellSource::covers(i32 x0, i32 x1, i32 z0, i32 z1) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);
    const auto n = static_cast<i32>(m_cellChunks);
    const i32 cx0 = floorDivide(x0, n);
    const i32 cx1 = floorDivide(x1, n);
    const i32 cz0 = floorDivide(z0, n);
    const i32 cz1 = floorDivide(z1, n);
    for (i32 x = cx0; x <= cx1; ++x) {
        const auto at =
            std::lower_bound(m_cells.begin(), m_cells.end(), ChunkId{x, cz0, FieldLayerTerrain}, cellBefore);
        if (at != m_cells.end() && at->x == x && at->z <= cz1)
            return true;
    }
    return false;
}

std::optional<TerrainCell> TerrainCellSource::read(ChunkId id) const
{
    if (!m_read)
        return std::nullopt;
    return m_read(id);
}

std::shared_ptr<const TerrainCellSource::Summaries> TerrainCellSource::summaries(ChunkId id) const
{
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        if (const auto found = m_summaries.find({id.x, id.z}); found != m_summaries.end()) {
            found->second.used = ++m_summaryClock;
            return found->second.summaries;
        }
    }
    std::optional<TerrainCell> cell = read(id);
    if (!cell.has_value()) {
        // Nothing of it will be drawn, and that is known now: a cell nobody
        // could read stayed "not read yet", and the node over it was built
        // again every frame.
        learnSignature(id, terrainCellSignature(TerrainField{}, id.x, id.z, m_cellChunks));
        return nullptr;
    }
    auto made = std::make_shared<Summaries>();
    made->reserve(cell->field.chunks().size());
    for (const TerrainField::Entry& entry : cell->field.chunks())
        made->emplace_back(entry.first, entry.second->summary());
    // What it holds, learnt from the read (ADR 0150).
    const core::u64 held = terrainCellSignature(cell->field, id.x, id.z, m_cellChunks);
    const std::lock_guard<std::mutex> lock(m_lock);
    if (const auto at = std::lower_bound(m_cells.begin(), m_cells.end(), id, cellBefore);
        at != m_cells.end() && at->x == id.x && at->z == id.z) {
        core::u64& known = m_signatures[static_cast<usize>(at - m_cells.begin())];
        if (known == 0) {
            known = held;
            m_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    // The least lately asked for go, a quarter at once.
    if (m_summaries.size() >= SummariesKept) {
        std::vector<std::pair<core::u64, std::pair<i32, i32>>> byAge;
        byAge.reserve(m_summaries.size());
        for (const auto& [at, kept] : m_summaries)
            byAge.emplace_back(kept.used, at);
        std::sort(byAge.begin(), byAge.end());
        for (usize at = 0; at < byAge.size() / 4; ++at)
            m_summaries.erase(byAge[at].second);
    }
    // Two threads that both read it keep the first: the same cell, the same
    // summaries.
    return m_summaries.emplace(std::pair{id.x, id.z}, KeptSummaries{std::move(made), ++m_summaryClock})
        .first->second.summaries;
}

void TerrainCellSource::digestsIn(i32 x0, i32 x1, i32 z0, i32 z1,
                                  std::vector<std::pair<ChunkKey, core::u64>>& out) const
{
    std::vector<ChunkId> cells;
    const usize first = out.size();
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        cellsWithin(x0, x1, z0, z1, cells);
        for (const ChunkId& cell : cells) {
            const auto found = m_summaries.find({cell.x, cell.z});
            if (found == m_summaries.end())
                continue;
            for (const TerrainField::Entry& entry : *found->second.summaries) {
                if (entry.first.x >= x0 && entry.first.x <= x1 && entry.first.z >= z0 && entry.first.z <= z1)
                    out.emplace_back(entry.first, entry.second->digest());
            }
        }
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

bool TerrainCellSource::summarised(i32 x0, i32 x1, i32 z0, i32 z1) const
{
    std::vector<ChunkId> cells;
    const std::lock_guard<std::mutex> lock(m_lock);
    cellsWithin(x0, x1, z0, z1, cells);
    for (const ChunkId& cell : cells) {
        if (!m_summaries.contains({cell.x, cell.z}))
            return false;
    }
    return true;
}

} // namespace engine::asset
