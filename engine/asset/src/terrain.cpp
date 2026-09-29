#include "engine/asset/terrain.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace engine::asset {
namespace {

using core::i32;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

constexpr u32 DenseFlag = 0x80000000u;
constexpr auto Edge = static_cast<i32>(ChunkEdge);

[[nodiscard]] bool isDense(u32 row) noexcept
{
    return (row & DenseFlag) != 0;
}

// The lower bound of a key in the sorted chunk list.
template <class Vector>
[[nodiscard]] auto lowerBound(Vector& entries, const ChunkKey& key)
{
    return std::lower_bound(entries.begin(), entries.end(), key,
                            [](const auto& entry, const ChunkKey& probe) { return entry.first < probe; });
}

} // namespace

u8 quantiseOccupancy(float fraction) noexcept
{
    if (!(fraction > 0.0f))
        return 0;
    if (fraction >= 1.0f)
        return FullOccupancy;
    return static_cast<u8>(std::lround(fraction * static_cast<float>(FullOccupancy)));
}

// --- TerrainChunk --------------------------------------------------------------

u16 TerrainChunk::Layer::get(u32 x, u32 row) const noexcept
{
    if (rows.empty())
        return value;
    const u32 entry = rows[row];
    if (!isDense(entry))
        return static_cast<u16>(entry);
    return dense[static_cast<usize>(entry & ~DenseFlag) * ChunkEdge + x];
}

void TerrainChunk::Layer::readRow(u32 row, std::span<u16, ChunkEdge> out) const noexcept
{
    if (rows.empty()) {
        std::fill(out.begin(), out.end(), value);
        return;
    }
    const u32 entry = rows[row];
    if (!isDense(entry)) {
        std::fill(out.begin(), out.end(), static_cast<u16>(entry));
        return;
    }
    const u16* first = &dense[static_cast<usize>(entry & ~DenseFlag) * ChunkEdge];
    std::copy(first, first + ChunkEdge, out.begin());
}

bool TerrainChunk::Layer::set(u32 x, u32 row, u16 packed)
{
    if (rows.empty()) {
        if (packed == value)
            return false;
        rows.assign(ChunkRows, static_cast<u32>(value));
    }
    u32& entry = rows[row];
    if (!isDense(entry)) {
        if (static_cast<u16>(entry) == packed)
            return false;
        // The row becomes voxel by voxel, appended; `normalize` puts the dense
        // rows back in row order.
        const auto index = static_cast<u32>(dense.size() / ChunkEdge);
        dense.insert(dense.end(), ChunkEdge, static_cast<u16>(entry));
        entry = DenseFlag | index;
    }
    u16& slot = dense[static_cast<usize>(entry & ~DenseFlag) * ChunkEdge + x];
    if (slot == packed)
        return false;
    slot = packed;
    return true;
}

void TerrainChunk::Layer::writeRow(u32 row, std::span<const u16, ChunkEdge> values)
{
    const u16 first = values[0];
    const bool same = std::all_of(values.begin(), values.end(), [first](u16 at) { return at == first; });
    if (rows.empty()) {
        if (same && first == value)
            return;
        rows.assign(ChunkRows, static_cast<u32>(value));
    }
    u32& entry = rows[row];
    if (same) {
        // Left as a stale dense row if it was one; `normalize` drops it.
        entry = static_cast<u32>(first);
    }
    else {
        if (!isDense(entry)) {
            const auto index = static_cast<u32>(dense.size() / ChunkEdge);
            dense.insert(dense.end(), ChunkEdge, 0);
            entry = DenseFlag | index;
        }
        std::copy(values.begin(), values.end(), dense.begin() + static_cast<std::ptrdiff_t>(entry & ~DenseFlag) * Edge);
    }
}

void TerrainChunk::Layer::normalize()
{
    if (rows.empty())
        return;
    std::vector<u16> compact;
    compact.reserve(dense.size());
    bool allSame = true;
    u32 firstValue = 0xFFFFFFFFu;
    for (u32& entry : rows) {
        if (isDense(entry)) {
            const u16* voxels = &dense[static_cast<usize>(entry & ~DenseFlag) * ChunkEdge];
            const u16 head = voxels[0];
            if (std::all_of(voxels, voxels + ChunkEdge, [head](u16 at) { return at == head; })) {
                entry = static_cast<u32>(head);
            }
            else {
                const auto index = static_cast<u32>(compact.size() / ChunkEdge);
                compact.insert(compact.end(), voxels, voxels + ChunkEdge);
                entry = DenseFlag | index;
            }
        }
        if (isDense(entry)) {
            allSame = false;
        }
        else if (firstValue == 0xFFFFFFFFu) {
            firstValue = entry;
        }
        else if (entry != firstValue) {
            allSame = false;
        }
    }
    if (allSame) {
        value = static_cast<u16>(firstValue);
        rows.clear();
        rows.shrink_to_fit();
        dense.clear();
        dense.shrink_to_fit();
    }
    else {
        dense = std::move(compact);
    }
}

Voxel TerrainChunk::get(u32 x, u32 y, u32 z) const noexcept
{
    const u32 row = rowIndex(y, z);
    return withPaint(unpackVoxel(m_main.get(x, row)), m_paint.get(x, row));
}

void TerrainChunk::readRow(u32 y, u32 z, std::span<u16, ChunkEdge> out) const noexcept
{
    m_main.readRow(rowIndex(y, z), out);
}

void TerrainChunk::readPaintRow(u32 y, u32 z, std::span<u16, ChunkEdge> out) const noexcept
{
    m_paint.readRow(rowIndex(y, z), out);
}

void TerrainChunk::invalidate() noexcept
{
    m_digestValid = false;
    m_bordersValid = 0;
    for (std::vector<u32>& level : m_mips)
        level.clear();
}

bool TerrainChunk::set(u32 x, u32 y, u32 z, Voxel voxel)
{
    const Voxel written = canonical(voxel);
    const u32 row = rowIndex(y, z);
    const bool under = m_main.set(x, row, packVoxel(written));
    const bool paint = m_paint.set(x, row, packPaint(written));
    if (!under && !paint)
        return false;
    invalidate();
    return true;
}

void TerrainChunk::writeRow(u32 y, u32 z, std::span<const u16, ChunkEdge> values)
{
    m_main.writeRow(rowIndex(y, z), values);
    invalidate();
}

void TerrainChunk::writePaintRow(u32 y, u32 z, std::span<const u16, ChunkEdge> values)
{
    m_paint.writeRow(rowIndex(y, z), values);
    invalidate();
}

void TerrainChunk::canonicalizePaint()
{
    if (!painted())
        return;
    std::array<u16, ChunkEdge> under{};
    std::array<u16, ChunkEdge> paint{};
    for (u32 row = 0; row < ChunkRows; ++row) {
        m_paint.readRow(row, paint);
        if (std::all_of(paint.begin(), paint.end(), [](u16 at) { return at == 0; }))
            continue;
        m_main.readRow(row, under);
        bool changed = false;
        for (u32 x = 0; x < ChunkEdge; ++x) {
            const u16 kept = packPaint(canonical(withPaint(unpackVoxel(under[x]), paint[x])));
            changed = changed || kept != paint[x];
            paint[x] = kept;
        }
        if (changed)
            m_paint.writeRow(row, paint);
    }
}

void TerrainChunk::normalize()
{
    canonicalizePaint();
    m_main.normalize();
    m_paint.normalize();
    // The bytes are the same voxels either way; only the digest's input moved.
    m_digestValid = false;
    m_bordersValid = 0;
}

usize TerrainChunk::bytes() const noexcept
{
    usize total = sizeof(TerrainChunk);
    for (const Layer* layer : {&m_main, &m_paint})
        total += layer->rows.capacity() * sizeof(u32) + layer->dense.capacity() * sizeof(u16);
    for (const std::vector<u32>& level : m_mips)
        total += level.capacity() * sizeof(u32);
    return total;
}

u64 TerrainChunk::digest() const noexcept
{
    if (!m_digestValid) {
        // Over the canonical form: a uniform chunk is its value, and a rowed one
        // is its row table and its dense rows. Two chunks with the same voxels
        // only share a digest once both are normalised, which every write path
        // does before anything reads this. **Paint is hashed only where there
        // is some**, so ground nobody painted hashes as it did before paint
        // existed (ADR 0114) -- and every recorded trace still holds.
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        const u8 kind = m_main.rows.empty() ? 0 : 1;
        XXH3_64bits_update(&state, &kind, 1);
        if (m_main.rows.empty()) {
            XXH3_64bits_update(&state, &m_main.value, sizeof(m_main.value));
        }
        else {
            XXH3_64bits_update(&state, m_main.rows.data(), m_main.rows.size() * sizeof(u32));
            XXH3_64bits_update(&state, m_main.dense.data(), m_main.dense.size() * sizeof(u16));
        }
        if (painted()) {
            const u8 paintKind = m_paint.rows.empty() ? 2 : 3;
            XXH3_64bits_update(&state, &paintKind, 1);
            if (m_paint.rows.empty()) {
                XXH3_64bits_update(&state, &m_paint.value, sizeof(m_paint.value));
            }
            else {
                XXH3_64bits_update(&state, m_paint.rows.data(), m_paint.rows.size() * sizeof(u32));
                XXH3_64bits_update(&state, m_paint.dense.data(), m_paint.dense.size() * sizeof(u16));
            }
        }
        m_digest = XXH3_64bits_digest(&state);
        m_digestValid = true;
    }
    return m_digest;
}

u64 TerrainChunk::borderDigest(i32 dx, i32 dy, i32 dz) const noexcept
{
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || dz < -1 || dz > 1)
        return 0;
    if (dx == 0 && dy == 0 && dz == 0)
        return digest();
    const auto slot = static_cast<u32>((dx + 1) + 3 * (dy + 1) + 9 * (dz + 1));
    if ((m_bordersValid & (1u << slot)) == 0) {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        XXH3_64bits_update(&state, &slot, sizeof(slot));
        const bool painted = this->painted();
        if (uniform()) {
            XXH3_64bits_update(&state, &m_main.value, sizeof(m_main.value));
            if (painted)
                XXH3_64bits_update(&state, &m_paint.value, sizeof(m_paint.value));
        }
        else {
            // The layers the neighbour at that offset reads: the two against
            // it on an axis it is offset along -- low layers for a neighbour on
            // the low side -- and every layer on the others. Read voxel by
            // voxel in a fixed order, never the row table, so the answer does
            // not depend on how they happen to be stored.
            const auto range = [](i32 offset) {
                if (offset < 0)
                    return std::pair<u32, u32>{0, 2};
                if (offset > 0)
                    return std::pair<u32, u32>{ChunkEdge - 2, ChunkEdge};
                return std::pair<u32, u32>{0, ChunkEdge};
            };
            const auto [x0, x1] = range(dx);
            const auto [y0, y1] = range(dy);
            const auto [z0, z1] = range(dz);
            std::vector<u16> part;
            part.reserve(static_cast<usize>(x1 - x0) * (y1 - y0) * (z1 - z0) * (painted ? 2u : 1u));
            for (u32 y = y0; y < y1; ++y) {
                for (u32 z = z0; z < z1; ++z) {
                    for (u32 x = x0; x < x1; ++x) {
                        const Voxel voxel = get(x, y, z);
                        part.push_back(packVoxel(voxel));
                        if (painted)
                            part.push_back(packPaint(voxel));
                    }
                }
            }
            XXH3_64bits_update(&state, part.data(), part.size() * sizeof(u16));
        }
        m_borders[slot] = XXH3_64bits_digest(&state);
        m_bordersValid |= 1u << slot;
    }
    return m_borders[slot];
}

void TerrainChunk::prepareMip(u32 level) const
{
    if (level == 0 || level >= ChunkLevels || uniform() || !m_mips[level].empty())
        return;
    const u32 edge = ChunkEdge >> level;
    const u32 span = 1u << level;
    std::vector<u32>& out = m_mips[level];
    out.assign(static_cast<usize>(edge) * edge * edge, 0);
    // Sums per level cell, read row by row so the rows' own storage is walked
    // once in order.
    std::vector<u32> sums(out.size(), 0);
    std::vector<u8> best(out.size(), 0);
    std::vector<Voxel> fullest(out.size());
    std::array<u16, ChunkEdge> row{};
    std::array<u16, ChunkEdge> paint{};
    const bool painted = this->painted();
    for (u32 y = 0; y < ChunkEdge; ++y) {
        for (u32 z = 0; z < ChunkEdge; ++z) {
            readRow(y, z, row);
            if (painted)
                readPaintRow(y, z, paint);
            const usize base = (static_cast<usize>(y / span) * edge + z / span) * edge;
            for (u32 x = 0; x < ChunkEdge; ++x) {
                const Voxel voxel = painted ? withPaint(unpackVoxel(row[x]), paint[x]) : unpackVoxel(row[x]);
                const usize cell = base + x / span;
                sums[cell] += voxel.occupancy;
                // The fullest voxel's materials, the first one met on a tie:
                // the walk order is fixed, so so is the answer.
                if (voxel.occupancy > best[cell]) {
                    best[cell] = voxel.occupancy;
                    fullest[cell] = voxel;
                }
            }
        }
    }
    const u32 count = span * span * span;
    for (usize at = 0; at < out.size(); ++at) {
        const auto occupancy = static_cast<u8>((sums[at] + count / 2) / count);
        const Voxel voxel = canonical(Voxel{occupancy, fullest[at].material, fullest[at].top, fullest[at].cover});
        out[at] = static_cast<u32>(packVoxel(voxel)) | (static_cast<u32>(packPaint(voxel)) << 16);
    }
}

Voxel TerrainChunk::mip(u32 level, u32 x, u32 y, u32 z) const noexcept
{
    if (level == 0)
        return get(x, y, z);
    if (uniform())
        return value();
    if (m_mips[level].empty())
        prepareMip(level);
    const u32 edge = ChunkEdge >> level;
    const u32 packed = m_mips[level][(static_cast<usize>(y) * edge + z) * edge + x];
    return withPaint(unpackVoxel(static_cast<u16>(packed & 0xFFFF)), static_cast<u16>(packed >> 16));
}

// --- TerrainField --------------------------------------------------------------

i32 TerrainField::voxelIndex(double metres) const noexcept
{
    // **Clamped before the cast** (audit S10): a double past an `i32` is
    // undefined behaviour to convert, and a NaN is nowhere. **And to the
    // chunks a field may hold** (terrain audit B4): an edit far enough out made
    // a chunk past `MaxChunkKey`, which a save then refused with its whole cell
    // and the wire with its whole message.
    const double index = std::floor(metres / static_cast<double>(m_settings.voxelSize));
    if (!(index == index))
        return 0;
    constexpr double Limit = static_cast<double>(MaxChunkKey) * static_cast<double>(ChunkEdge);
    return static_cast<i32>(std::clamp(index, -Limit, Limit + static_cast<double>(ChunkEdge) - 1.0));
}

const TerrainChunk* TerrainField::findChunk(ChunkKey key) const noexcept
{
    const auto at = lowerBound(m_chunks, key);
    return (at != m_chunks.end() && at->first == key) ? at->second.get() : nullptr;
}

Voxel TerrainField::voxel(i32 x, i32 y, i32 z) const noexcept
{
    const TerrainChunk* chunk = findChunk(chunkOf(x, y, z));
    if (chunk == nullptr)
        return Voxel{};
    return chunk->get(static_cast<u32>(floorMod(x, Edge)), static_cast<u32>(floorMod(y, Edge)),
                      static_cast<u32>(floorMod(z, Edge)));
}

FieldSample TerrainField::sample(i32 x, i32 y, i32 z) const noexcept
{
    const Voxel got = voxel(x, y, z);
    return FieldSample{(0.5f - occupancyOf(got)) * RampVoxels * m_settings.voxelSize, got.material};
}

Voxel TerrainField::voxelAt(u32 level, i32 x, i32 y, i32 z) const noexcept
{
    if (level == 0)
        return voxel(x, y, z);
    const i32 edge = Edge >> level;
    const TerrainChunk* chunk = findChunk(ChunkKey{floorDiv(x, edge), floorDiv(y, edge), floorDiv(z, edge)});
    if (chunk == nullptr)
        return Voxel{};
    return chunk->mip(level, static_cast<u32>(floorMod(x, edge)), static_cast<u32>(floorMod(y, edge)),
                      static_cast<u32>(floorMod(z, edge)));
}

std::vector<ChunkKey> TerrainField::chunkKeys() const
{
    std::vector<ChunkKey> keys;
    keys.reserve(m_chunks.size());
    for (const Entry& entry : m_chunks)
        keys.push_back(entry.first);
    return keys;
}

std::span<const TerrainField::Entry> TerrainField::column(i32 chunkX, i32 chunkZ) const noexcept
{
    const ChunkKey low{chunkX, std::numeric_limits<i32>::min(), chunkZ};
    const auto first = lowerBound(m_chunks, low);
    auto last = first;
    while (last != m_chunks.end() && last->first.x == chunkX && last->first.z == chunkZ)
        ++last;
    return std::span<const Entry>(first, last);
}

std::optional<float> TerrainField::columnTop(i32 x, i32 z) const noexcept
{
    const std::span<const Entry> chunks = column(floorDiv(x, Edge), floorDiv(z, Edge));
    const auto localX = static_cast<u32>(floorMod(x, Edge));
    const auto localZ = static_cast<u32>(floorMod(z, Edge));
    const float voxel = m_settings.voxelSize;
    // Walked top down: the first voxel at least half full is the top, and the
    // crossing is between it and the one above it.
    float above = 0.0f; // Occupancy of the voxel above the one being read.
    for (auto at = chunks.rbegin(); at != chunks.rend(); ++at) {
        const TerrainChunk& chunk = *at->second;
        const i32 baseY = at->first.y * Edge;
        // A chunk directly under a gap in the column starts from air above.
        const auto next = at.base();
        if (next == chunks.end() || next->first.y != at->first.y + 1)
            above = 0.0f;
        if (chunk.uniform() && chunk.value().occupancy < 128) {
            above = occupancyOf(chunk.value());
            continue;
        }
        for (i32 y = Edge - 1; y >= 0; --y) {
            const float here = occupancyOf(chunk.get(localX, static_cast<u32>(y), localZ));
            if (here >= 0.5f) {
                // Between this centre and the one above, where occupancy
                // passes one half.
                const float t = here - above > 1e-6f ? (here - 0.5f) / (here - above) : 0.0f;
                return (static_cast<float>(baseY + y) + 0.5f + t) * voxel;
            }
            above = here;
        }
    }
    return std::nullopt;
}

std::optional<float> TerrainField::columnBottom(i32 x, i32 z) const noexcept
{
    const std::span<const Entry> chunks = column(floorDiv(x, Edge), floorDiv(z, Edge));
    const auto localX = static_cast<u32>(floorMod(x, Edge));
    const auto localZ = static_cast<u32>(floorMod(z, Edge));
    const float voxel = m_settings.voxelSize;
    // `columnTop` upside down: walked bottom up, the first voxel at least half
    // full is the bottom, and the crossing is between it and the one below.
    float below = 0.0f;
    for (auto at = chunks.begin(); at != chunks.end(); ++at) {
        const TerrainChunk& chunk = *at->second;
        const i32 baseY = at->first.y * Edge;
        if (at == chunks.begin() || std::prev(at)->first.y != at->first.y - 1)
            below = 0.0f;
        if (chunk.uniform() && chunk.value().occupancy < 128) {
            below = occupancyOf(chunk.value());
            continue;
        }
        for (i32 y = 0; y < Edge; ++y) {
            const float here = occupancyOf(chunk.get(localX, static_cast<u32>(y), localZ));
            if (here >= 0.5f) {
                const float t = here - below > 1e-6f ? (here - 0.5f) / (here - below) : 0.0f;
                return (static_cast<float>(baseY + y) + 0.5f - t) * voxel;
            }
            below = here;
        }
    }
    return std::nullopt;
}

u64 TerrainField::digest() const noexcept
{
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (const Entry& entry : m_chunks) {
        XXH3_64bits_update(&state, &entry.first, sizeof(entry.first));
        const u64 chunk = entry.second->digest();
        XXH3_64bits_update(&state, &chunk, sizeof(chunk));
    }
    return XXH3_64bits_digest(&state);
}

usize TerrainField::bytes() const noexcept
{
    usize total = m_chunks.capacity() * sizeof(Entry);
    for (const Entry& entry : m_chunks)
        total += entry.second->bytes();
    return total;
}

TerrainChunk* TerrainField::chunkFor(ChunkKey key)
{
    const auto at = lowerBound(m_chunks, key);
    if (at != m_chunks.end() && at->first == key) {
        if (at->second.use_count() > 1)
            at->second = std::make_shared<TerrainChunk>(*at->second);
        return at->second.get();
    }
    return m_chunks.insert(at, {key, std::make_shared<TerrainChunk>()})->second.get();
}

void TerrainField::finishChunk(ChunkKey key)
{
    const auto at = lowerBound(m_chunks, key);
    if (at == m_chunks.end() || at->first != key)
        return;
    at->second->normalize();
    if (at->second->empty())
        m_chunks.erase(at);
}

bool TerrainField::setVoxel(i32 x, i32 y, i32 z, Voxel voxel)
{
    const ChunkKey key = chunkOf(x, y, z);
    if (canonical(voxel) == Voxel{} && findChunk(key) == nullptr)
        return false;
    TerrainChunk* chunk = chunkFor(key);
    const bool changed = chunk->set(static_cast<u32>(floorMod(x, Edge)), static_cast<u32>(floorMod(y, Edge)),
                                    static_cast<u32>(floorMod(z, Edge)), voxel);
    finishChunk(key);
    return changed;
}

void TerrainField::setChunk(ChunkKey key, std::shared_ptr<TerrainChunk> chunk)
{
    if (!chunkKeyInRange(key))
        return;
    const auto at = lowerBound(m_chunks, key);
    const bool exists = at != m_chunks.end() && at->first == key;
    if (chunk == nullptr || chunk->empty()) {
        if (exists)
            m_chunks.erase(at);
        return;
    }
    if (exists)
        at->second = std::move(chunk);
    else
        m_chunks.insert(at, {key, std::move(chunk)});
}

void TerrainField::removeChunk(ChunkKey key)
{
    const auto at = lowerBound(m_chunks, key);
    if (at != m_chunks.end() && at->first == key)
        m_chunks.erase(at);
}

void TerrainField::shareFrom(const TerrainField& from)
{
    shareFrom(from, TerrainField{});
}

void TerrainField::shareFrom(const TerrainField& from, const TerrainField& known)
{
    // One merge of the two sorted lists rather than an insertion per chunk:
    // a cell streamed into a large field would otherwise shift the tail of the
    // vector once per chunk it brings.
    if (from.m_chunks.empty())
        return;
    std::vector<Entry> merged;
    merged.reserve(m_chunks.size() + from.m_chunks.size());
    auto held = m_chunks.begin();
    auto seen = known.m_chunks.begin();
    for (const Entry& entry : from.m_chunks) {
        while (held != m_chunks.end() && held->first < entry.first)
            merged.push_back(std::move(*held++));
        if (held != m_chunks.end() && held->first == entry.first)
            continue;
        while (seen != known.m_chunks.end() && seen->first < entry.first)
            ++seen;
        if (seen != known.m_chunks.end() && seen->first == entry.first)
            continue;
        merged.push_back(entry);
    }
    while (held != m_chunks.end())
        merged.push_back(std::move(*held++));
    m_chunks = std::move(merged);
}

void TerrainField::refreshFrom(const TerrainField& newer)
{
    auto theirs = newer.m_chunks.begin();
    for (Entry& entry : m_chunks) {
        while (theirs != newer.m_chunks.end() && theirs->first < entry.first)
            ++theirs;
        if (theirs != newer.m_chunks.end() && theirs->first == entry.first)
            entry.second = theirs->second;
    }
}

void TerrainField::removeAll(std::span<const ChunkKey> keys)
{
    if (keys.empty())
        return;
    auto key = keys.begin();
    auto kept = m_chunks.begin();
    for (auto at = m_chunks.begin(); at != m_chunks.end(); ++at) {
        while (key != keys.end() && *key < at->first)
            ++key;
        if (key != keys.end() && *key == at->first)
            continue;
        if (kept != at)
            *kept = std::move(*at);
        ++kept;
    }
    m_chunks.erase(kept, m_chunks.end());
}

void TerrainField::setHeightRange(float minHeight, float maxHeight) noexcept
{
    if (!(maxHeight > minHeight))
        return;
    m_settings.minHeight = minHeight;
    m_settings.maxHeight = maxHeight;
}

// --- FieldWriter ---------------------------------------------------------------

TerrainChunk* FieldWriter::chunkAt(i32 x, i32 y, i32 z, bool air)
{
    const ChunkKey key = chunkOf(x, y, z);
    if (m_last == nullptr || !(key == m_lastKey)) {
        // Never a chunk no save or message can carry (terrain audit B4).
        if (!chunkKeyInRange(key))
            return nullptr;
        if (air && m_field.findChunk(key) == nullptr)
            return nullptr;
        m_last = m_field.chunkFor(key);
        m_lastKey = key;
        const auto at = std::lower_bound(m_touched.begin(), m_touched.end(), key);
        if (at == m_touched.end() || !(*at == key))
            m_touched.insert(at, key);
    }
    return m_last;
}

bool FieldWriter::set(i32 x, i32 y, i32 z, Voxel voxel)
{
    TerrainChunk* chunk = chunkAt(x, y, z, canonical(voxel) == Voxel{});
    if (chunk == nullptr)
        return false;
    const auto lx = static_cast<u32>(floorMod(x, Edge));
    const auto ly = static_cast<u32>(floorMod(y, Edge));
    const auto lz = static_cast<u32>(floorMod(z, Edge));
    // **A sculpt keeps the paint it moves** (ADR 0114): a voxel written with
    // no paint over the material it already is keeps the paint it had, so a
    // raise or a smooth does not scrub a painted hillside back to its base.
    // The paint verbs write it whole (`setExact`).
    if (voxel.top == 0 && voxel.cover == 0 && chunk->painted()) {
        const Voxel was = chunk->get(lx, ly, lz);
        if (was.material == voxel.material && was.cover != 0) {
            voxel.top = was.top;
            voxel.cover = was.cover;
        }
    }
    const bool changed = chunk->set(lx, ly, lz, voxel);
    if (changed)
        m_changed += 1;
    return changed;
}

bool FieldWriter::setExact(i32 x, i32 y, i32 z, Voxel voxel)
{
    TerrainChunk* chunk = chunkAt(x, y, z, canonical(voxel) == Voxel{});
    if (chunk == nullptr)
        return false;
    const bool changed = chunk->set(static_cast<u32>(floorMod(x, Edge)), static_cast<u32>(floorMod(y, Edge)),
                                    static_cast<u32>(floorMod(z, Edge)), voxel);
    if (changed)
        m_changed += 1;
    return changed;
}

void FieldWriter::finish()
{
    // `chunkFor` may have inserted chunks and moved the others, so the cached
    // pointer is dropped before anything else.
    m_last = nullptr;
    for (const ChunkKey key : m_touched)
        m_field.finishChunk(key);
}

} // namespace engine::asset
