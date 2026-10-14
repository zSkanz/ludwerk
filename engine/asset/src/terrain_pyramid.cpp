#include "engine/asset/terrain_pyramid.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "engine/asset/terrain_mesher.h"
#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/jobs/jobs.h"

namespace engine::asset {
namespace {

using core::i32;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

constexpr u32 NodeMagic = 0x4E50474Cu; // "LGPN", little-endian
constexpr u32 NodeVersion = 2;
// A corrupted count meets a named limit, not an allocator.
constexpr u32 MaxNodeChunks = 1u << 20;
constexpr u32 MaxNodeCells = 1u << 16;

constexpr i32 BlockColumns = static_cast<i32>(1u << PyramidTopLevel);

class Writer
{
public:
    void u8v(u8 value) { m_out.push_back(static_cast<std::byte>(value)); }
    void u16v(u16 value)
    {
        u8v(static_cast<u8>(value & 0xFFu));
        u8v(static_cast<u8>(value >> 8));
    }
    void u32v(u32 value)
    {
        for (u32 shift = 0; shift < 32; shift += 8)
            u8v(static_cast<u8>((value >> shift) & 0xFFu));
    }
    void i32v(i32 value) { u32v(static_cast<u32>(value)); }
    void u64v(u64 value)
    {
        u32v(static_cast<u32>(value & 0xFFFFFFFFull));
        u32v(static_cast<u32>(value >> 32));
    }
    void f32v(float value)
    {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32v(bits);
    }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(m_out); }

private:
    std::vector<std::byte> m_out;
};

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] u8 u8v() noexcept
    {
        if (m_at + 1 > m_bytes.size()) {
            m_ok = false;
            return 0;
        }
        return static_cast<u8>(m_bytes[m_at++]);
    }
    [[nodiscard]] u16 u16v() noexcept
    {
        const u16 low = u8v();
        return static_cast<u16>(low | (static_cast<u16>(u8v()) << 8));
    }
    [[nodiscard]] u32 u32v() noexcept
    {
        u32 value = 0;
        for (u32 shift = 0; shift < 32; shift += 8)
            value |= static_cast<u32>(u8v()) << shift;
        return value;
    }
    [[nodiscard]] i32 i32v() noexcept { return static_cast<i32>(u32v()); }
    [[nodiscard]] u64 u64v() noexcept
    {
        const u64 low = u32v();
        return low | (static_cast<u64>(u32v()) << 32);
    }
    [[nodiscard]] float f32v() noexcept
    {
        const u32 bits = u32v();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool finished() const noexcept { return m_ok && m_at == m_bytes.size(); }
    [[nodiscard]] usize left() const noexcept { return m_bytes.size() - m_at; }

private:
    std::span<const std::byte> m_bytes;
    usize m_at = 0;
    bool m_ok = true;
};

[[nodiscard]] core::EngineError malformed()
{
    return core::makeError(ENG_TR("asset.terrain.err.pyramid_malformed"));
}

[[nodiscard]] i32 floorDivide(i32 value, i32 divisor) noexcept
{
    const i32 quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// Which bit of `PyramidNode::files` a finer node of a block is.
[[nodiscard]] u32 fileBit(u32 level, i32 x, i32 z, i32 blockX, i32 blockZ) noexcept
{
    const i32 across = 1 << (PyramidTopLevel - level);
    const i32 localX = x - blockX * across;
    const i32 localZ = z - blockZ * across;
    // Level 4: bits 0 to 3. Level 3: bits 4 to 19.
    return level + 1 == PyramidTopLevel ? 1u << static_cast<u32>(localZ * 2 + localX)
                                        : 1u << static_cast<u32>(4 + localZ * 4 + localX);
}

// The surface chunks of a node hold nothing of: no cells, no crossings.
[[nodiscard]] bool hasSurface(const std::shared_ptr<const SurfaceLevel>& surface) noexcept
{
    return surface != nullptr && (!surface->cells.empty() || !surface->edges.empty());
}

} // namespace

std::vector<std::byte> encodePyramidNode(const PyramidNode& node)
{
    Writer out;
    out.u32v(NodeMagic);
    out.u32v(NodeVersion);
    out.u32v(node.level);
    out.i32v(node.x);
    out.i32v(node.z);
    out.f32v(node.settings.voxelSize);
    out.f32v(node.settings.minHeight);
    out.f32v(node.settings.maxHeight);
    out.u32v(node.files);
    out.u32v(static_cast<u32>(node.cells.size()));
    for (const PyramidCell& cell : node.cells) {
        out.i32v(cell.x);
        out.i32v(cell.z);
        out.u64v(cell.signature);
        out.u64v(cell.stamp);
    }
    out.u32v(static_cast<u32>(node.chunks.size()));
    for (const PyramidChunk& chunk : node.chunks) {
        out.i32v(chunk.key.x);
        out.i32v(chunk.key.y);
        out.i32v(chunk.key.z);
        const bool surface = chunk.surface != nullptr;
        out.u8v(static_cast<u8>((chunk.stored ? 1u : 0u) | (chunk.kept.rows ? 2u : 0u) | (surface ? 4u : 0u)));
        if (chunk.stored) {
            out.u8v(chunk.kept.solidFaces);
            out.u16v(chunk.kept.main);
            out.u16v(chunk.kept.paint);
            out.u64v(chunk.kept.digest);
        }
        if (!surface)
            continue;
        out.u16v(static_cast<u16>(chunk.surface->cells.size()));
        out.u16v(static_cast<u16>(chunk.surface->edges.size()));
        // **What the mesher reads of a gathered cell, and no more**: where its
        // vertex is and how far the fine surface is from it are worked out
        // when it is gathered, so the quadric and the planes that gave them
        // are not kept -- forty bytes of a hundred and twelve.
        for (const SurfaceCell& cell : chunk.surface->cells) {
            out.u16v(cell.index);
            out.u32v(cell.count);
            for (const float value : cell.offset)
                out.f32v(value);
            for (const float value : cell.normal)
                out.f32v(value);
            for (const u8 material : cell.materials)
                out.u8v(material);
            for (const u16 votes : cell.votes)
                out.u16v(votes);
            out.u8v(cell.top);
            out.u32v(cell.topVotes);
            out.f32v(cell.cover);
            for (const float value : cell.placed)
                out.f32v(value);
            out.f32v(cell.deviation);
        }
        for (const SurfaceEdge& edge : chunk.surface->edges) {
            out.u16v(edge.index);
            out.u8v(edge.axis);
            out.u16v(edge.up);
            out.u16v(edge.down);
        }
    }
    return out.take();
}

std::optional<core::EngineError> decodePyramidNode(std::span<const std::byte> bytes, PyramidNode& out)
{
    Reader in(bytes);
    if (in.u32v() != NodeMagic || in.u32v() != NodeVersion)
        return malformed();
    PyramidNode node;
    node.level = in.u32v();
    node.x = in.i32v();
    node.z = in.i32v();
    node.settings.voxelSize = in.f32v();
    node.settings.minHeight = in.f32v();
    node.settings.maxHeight = in.f32v();
    node.files = in.u32v();
    if (!in.ok() || node.level < PyramidFirstLevel || node.level > PyramidTopLevel)
        return malformed();
    const u32 cells = in.u32v();
    if (!in.ok() || cells > MaxNodeCells || static_cast<usize>(cells) * 24u > in.left())
        return malformed();
    node.cells.resize(cells);
    for (PyramidCell& cell : node.cells) {
        cell.x = in.i32v();
        cell.z = in.i32v();
        cell.signature = in.u64v();
        cell.stamp = in.u64v();
    }
    const u32 chunks = in.u32v();
    if (!in.ok() || chunks > MaxNodeChunks || static_cast<usize>(chunks) * 13u > in.left())
        return malformed();
    // A level's cells and edges in one chunk: `edge` cubed, and three each.
    const u32 edge = static_cast<u32>(ChunkEdge) >> node.level;
    const u32 maxCells = edge * edge * edge;
    node.chunks.resize(chunks);
    for (PyramidChunk& chunk : node.chunks) {
        chunk.key.x = in.i32v();
        chunk.key.y = in.i32v();
        chunk.key.z = in.i32v();
        const u8 flags = in.u8v();
        chunk.stored = (flags & 1u) != 0;
        chunk.kept.rows = (flags & 2u) != 0;
        if (chunk.stored) {
            chunk.kept.solidFaces = in.u8v();
            chunk.kept.main = in.u16v();
            chunk.kept.paint = in.u16v();
            chunk.kept.digest = in.u64v();
        }
        if (!in.ok() || !chunkKeyInRange(chunk.key))
            return malformed();
        if ((flags & 4u) == 0)
            continue;
        const u32 cellCount = in.u16v();
        const u32 edgeCount = in.u16v();
        if (!in.ok() || cellCount > maxCells || edgeCount > maxCells * 3u ||
            static_cast<usize>(cellCount) * 67u + static_cast<usize>(edgeCount) * 7u > in.left())
            return malformed();
        auto surface = std::make_shared<SurfaceLevel>();
        surface->cells.resize(cellCount);
        for (SurfaceCell& cell : surface->cells) {
            cell.index = in.u16v();
            cell.count = in.u32v();
            for (float& value : cell.offset)
                value = in.f32v();
            for (float& value : cell.normal)
                value = in.f32v();
            for (u8& material : cell.materials)
                material = in.u8v();
            for (u16& votes : cell.votes)
                votes = in.u16v();
            cell.top = in.u8v();
            cell.topVotes = in.u32v();
            cell.cover = in.f32v();
            for (float& value : cell.placed)
                value = in.f32v();
            cell.deviation = in.f32v();
            if (cell.index >= maxCells)
                return malformed();
        }
        surface->edges.resize(edgeCount);
        for (SurfaceEdge& crossed : surface->edges) {
            crossed.index = in.u16v();
            crossed.axis = in.u8v();
            crossed.up = in.u16v();
            crossed.down = in.u16v();
            if (crossed.index >= maxCells || crossed.axis > 2)
                return malformed();
        }
        chunk.surface = std::move(surface);
    }
    if (!in.finished())
        return malformed();
    out = std::move(node);
    return std::nullopt;
}

TerrainPyramid::TerrainPyramid(FieldSettings settings, u32 cellChunks, Store store)
    : m_settings(settings), m_cellChunks(std::max<u32>(cellChunks, 1u)), m_store(std::move(store))
{}

TerrainPyramid::Stats TerrainPyramid::stats() const
{
    const std::lock_guard<std::mutex> lock(m_lock);
    return m_stats;
}

void TerrainPyramid::keep(const NodeKey& key, std::shared_ptr<const PyramidNode> node)
{
    const std::lock_guard<std::mutex> lock(m_lock);
    m_nodes[key] = Kept{std::move(node), ++m_clock};
    if (m_nodes.size() <= NodesKept)
        return;
    // The least lately asked for goes; a quarter at once, so the walk is rare.
    std::vector<std::pair<u64, NodeKey>> byAge;
    byAge.reserve(m_nodes.size());
    for (const auto& [kept, entry] : m_nodes)
        byAge.emplace_back(entry.used, kept);
    std::sort(byAge.begin(), byAge.end());
    for (usize at = 0; at < byAge.size() / 4; ++at)
        m_nodes.erase(byAge[at].second);
}

std::shared_ptr<const PyramidNode> TerrainPyramid::node(u32 level, i32 x, i32 z)
{
    const NodeKey key{level, x, z};
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        if (const auto found = m_nodes.find(key); found != m_nodes.end()) {
            found->second.used = ++m_clock;
            return found->second.node;
        }
    }
    std::vector<std::byte> bytes;
    if (!m_store.read || !m_store.read(level, x, z, bytes))
        return nullptr;
    auto decoded = std::make_shared<PyramidNode>();
    if (decodePyramidNode(bytes, *decoded).has_value() || decoded->level != level || decoded->x != x ||
        decoded->z != z || decoded->settings.voxelSize != m_settings.voxelSize ||
        decoded->settings.minHeight != m_settings.minHeight || decoded->settings.maxHeight != m_settings.maxHeight)
        return nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        m_stats.filesRead += 1;
    }
    keep(key, decoded);
    return decoded;
}

// What of a block is out of date: its columns, as a grid, and the cells as
// they are now.
struct TerrainPyramid::Stale
{
    bool any = false;
    // The block has no top-level file: all of it is gathered.
    bool fresh = false;
    std::array<bool, static_cast<usize>(BlockColumns) * static_cast<usize>(BlockColumns)> columns{};
    std::vector<std::pair<ChunkId, u64>> cells;
    std::shared_ptr<const PyramidNode> top;
};

bool TerrainPyramid::staleOf(const TerrainCellSource& source, i32 blockX, i32 blockZ, bool learn, Stale& out)
{
    const i32 x0 = blockX * BlockColumns;
    const i32 z0 = blockZ * BlockColumns;
    const auto n = static_cast<i32>(m_cellChunks);
    source.signaturesIn(x0 - 1, x0 + BlockColumns, z0 - 1, z0 + BlockColumns, out.cells);
    out.top = node(PyramidTopLevel, blockX, blockZ);
    out.fresh = out.top == nullptr;
    for (auto& [id, signature] : out.cells) {
        if (signature != 0)
            continue;
        if (!learn)
            return false;
        // **A cell whose row does not say what it holds**: what the block
        // kept of it stands while its file is the file it was read from.
        if (out.top != nullptr) {
            const auto was = std::lower_bound(out.top->cells.begin(), out.top->cells.end(), id,
                                              [](const PyramidCell& cell, const ChunkId& probe) {
                                                  return cell.x != probe.x ? cell.x < probe.x : cell.z < probe.z;
                                              });
            if (was != out.top->cells.end() && was->x == id.x && was->z == id.z && was->stamp != 0 &&
                was->signature != 0 && source.stampOf(id) == was->stamp) {
                source.learnSignature(id, was->signature);
                signature = source.signature(id);
                continue;
            }
        }
        // Read to learn it; a cell that will not read is known as one with
        // nothing in it (`TerrainCellSource::summaries`).
        (void)source.summaries(id);
        signature = source.signature(id);
    }
    const auto mark = [&](i32 cellX, i32 cellZ) {
        // The cell's columns and the one beside them on every side: a chunk's
        // surface reads two layers of its neighbours.
        for (i32 z = cellZ * n - 1; z <= cellZ * n + n; ++z) {
            for (i32 x = cellX * n - 1; x <= cellX * n + n; ++x) {
                if (x < x0 || x >= x0 + BlockColumns || z < z0 || z >= z0 + BlockColumns)
                    continue;
                out.columns[static_cast<usize>(z - z0) * static_cast<usize>(BlockColumns) +
                            static_cast<usize>(x - x0)] = true;
                out.any = true;
            }
        }
    };
    if (out.fresh) {
        out.columns.fill(true);
        out.any = true;
        return true;
    }
    // Both by (x, z): one walk finds what changed, what is new and what went.
    usize kept = 0;
    usize now = 0;
    const std::vector<PyramidCell>& was = out.top->cells;
    while (kept < was.size() || now < out.cells.size()) {
        const bool takeKept =
            now >= out.cells.size() ||
            (kept < was.size() && (was[kept].x != out.cells[now].first.x ? was[kept].x < out.cells[now].first.x
                                                                         : was[kept].z < out.cells[now].first.z));
        const bool takeNow =
            kept >= was.size() ||
            (now < out.cells.size() && (out.cells[now].first.x != was[kept].x ? out.cells[now].first.x < was[kept].x
                                                                              : out.cells[now].first.z < was[kept].z));
        if (takeKept) {
            mark(was[kept].x, was[kept].z);
            ++kept;
        }
        else if (takeNow) {
            mark(out.cells[now].first.x, out.cells[now].first.z);
            ++now;
        }
        else {
            if (was[kept].signature != out.cells[now].second)
                mark(was[kept].x, was[kept].z);
            ++kept;
            ++now;
        }
    }
    return true;
}

bool TerrainPyramid::current(const TerrainCellSource& source, i32 blockX, i32 blockZ)
{
    Stale stale;
    return staleOf(source, blockX, blockZ, false, stale) && !stale.any;
}

bool TerrainPyramid::ensure(const TerrainCellSource& source, i32 blockX, i32 blockZ, bool wide)
{
    std::shared_ptr<std::mutex> blockLock;
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        std::shared_ptr<std::mutex>& held = m_blockLocks[{blockX, blockZ}];
        if (held == nullptr)
            held = std::make_shared<std::mutex>();
        blockLock = held;
    }
    const std::lock_guard<std::mutex> building(*blockLock);
    Stale stale;
    if (!staleOf(source, blockX, blockZ, true, stale))
        return false;
    if (!stale.any)
        return true;

    const i32 x0 = blockX * BlockColumns;
    const i32 z0 = blockZ * BlockColumns;
    const auto n = static_cast<i32>(m_cellChunks);
    const auto staleAt = [&](i32 x, i32 z) {
        return x >= x0 && x < x0 + BlockColumns && z >= z0 && z < z0 + BlockColumns &&
               stale
                   .columns[static_cast<usize>(z - z0) * static_cast<usize>(BlockColumns) + static_cast<usize>(x - x0)];
    };

    // **The cells whose voxels the gathering reads**: every cell over a stale
    // column or the column beside one. By row, three rows at a time, so what
    // is decoded at once is three rows across the block however much of it is
    // stale.
    std::set<std::pair<i32, i32>> wanted;
    for (i32 z = z0; z < z0 + BlockColumns; ++z) {
        for (i32 x = x0; x < x0 + BlockColumns; ++x) {
            if (!staleAt(x, z))
                continue;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx)
                    wanted.emplace(floorDivide(z + dz, n), floorDivide(x + dx, n));
            }
        }
    }
    const auto known = [&](i32 cellX, i32 cellZ) {
        const auto found = std::lower_bound(stale.cells.begin(), stale.cells.end(), std::pair{cellX, cellZ},
                                            [](const auto& entry, const std::pair<i32, i32>& probe) {
                                                return entry.first.x != probe.first ? entry.first.x < probe.first
                                                                                    : entry.first.z < probe.second;
                                            });
        return found != stale.cells.end() && found->first.x == cellX && found->first.z == cellZ;
    };

    struct Gathered
    {
        ChunkKey key;
        bool stored = false;
        TerrainChunk::Kept kept;
        SurfaceLevels surfaces{};
    };
    std::vector<Gathered> gathered;
    TerrainField field(m_settings);
    std::map<i32, std::vector<TerrainCell>> rows;
    const auto readRow = [&](i32 row) {
        if (rows.contains(row))
            return;
        std::vector<TerrainCell>& held = rows[row];
        for (auto at = wanted.lower_bound({row, std::numeric_limits<i32>::min()});
             at != wanted.end() && at->first == row; ++at) {
            if (!known(at->second, row))
                continue;
            std::optional<TerrainCell> cell = source.read(ChunkId{at->second, row, FieldLayerTerrain});
            if (!cell.has_value())
                continue;
            for (const TerrainField::Entry& entry : cell->field.chunks())
                field.setChunk(entry.first, entry.second);
            held.push_back(std::move(*cell));
            const std::lock_guard<std::mutex> lock(m_lock);
            m_stats.cellsRead += 1;
        }
    };
    const auto letGo = [&](i32 below) {
        for (auto row = rows.begin(); row != rows.end() && row->first < below;) {
            for (const TerrainCell& cell : row->second)
                removeTerrainCell(field, cell);
            row = rows.erase(row);
        }
    };
    const u32 levels = ((1u << (PyramidTopLevel + 1u)) - 1u) & ~((1u << PyramidFirstLevel) - 1u);
    const i32 firstRow = floorDivide(z0, n);
    const i32 lastRow = floorDivide(z0 + BlockColumns - 1, n);
    for (i32 row = firstRow; row <= lastRow; ++row) {
        // The stale columns of this row of cells.
        std::vector<std::pair<i32, i32>> columns;
        for (i32 z = std::max(z0, row * n); z < std::min(z0 + BlockColumns, (row + 1) * n); ++z) {
            for (i32 x = x0; x < x0 + BlockColumns; ++x) {
                if (staleAt(x, z))
                    columns.emplace_back(x, z);
            }
        }
        if (columns.empty())
            continue;
        letGo(row - 1);
        readRow(row - 1);
        readRow(row);
        readRow(row + 1);
        // Every chunk a surface could sit in: from one under the lowest chunk
        // of the column and the eight round it to one over the highest.
        const usize first = gathered.size();
        for (const auto& [x, z] : columns) {
            i32 low = std::numeric_limits<i32>::max();
            i32 high = std::numeric_limits<i32>::min();
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (const TerrainField::Entry& entry : field.column(x + dx, z + dz)) {
                        low = std::min(low, entry.first.y);
                        high = std::max(high, entry.first.y);
                    }
                }
            }
            if (low > high)
                continue;
            for (i32 y = low - 1; y <= high + 1; ++y)
                gathered.push_back(Gathered{ChunkKey{x, y, z}, false, {}, {}});
        }
        // Keyed on this thread -- the key fills the chunks' lazy digests --
        // then gathered, on every worker when asked.
        for (usize at = first; at < gathered.size(); ++at)
            (void)surfaceContent(field, gathered[at].key);
        const auto gather = [&](usize begin, usize end, u32) noexcept {
            for (usize at = begin; at < end; ++at) {
                Gathered& chunk = gathered[at];
                if (const TerrainChunk* held = field.findChunk(chunk.key); held != nullptr) {
                    chunk.stored = true;
                    chunk.kept = held->kept();
                }
                chunk.surfaces = buildSurfaces(field, chunk.key, levels);
            }
        };
        if (wide)
            jobs::parallelFor("terrain.pyramid", jobs::Domain::Render, first, gathered.size(), 4, gather);
        else
            gather(first, gathered.size(), 0);
    }
    letGo(std::numeric_limits<i32>::max());

    // **Each file of the block that holds a stale column, written again**:
    // what it kept of its other columns, and what was gathered of these. The
    // top one last -- it is what says the block is current.
    std::sort(gathered.begin(), gathered.end(), [](const Gathered& a, const Gathered& b) { return a.key < b.key; });
    u32 files = stale.top != nullptr ? stale.top->files : 0u;
    u64 written = 0;
    for (u32 level = PyramidFirstLevel; level <= PyramidTopLevel; ++level) {
        const i32 span = 1 << level;
        const i32 across = 1 << (PyramidTopLevel - level);
        for (i32 localZ = 0; localZ < across; ++localZ) {
            for (i32 localX = 0; localX < across; ++localX) {
                const i32 nodeX = blockX * across + localX;
                const i32 nodeZ = blockZ * across + localZ;
                const i32 nx0 = nodeX * span;
                const i32 nz0 = nodeZ * span;
                bool touched = level == PyramidTopLevel;
                for (i32 z = nz0; z < nz0 + span && !touched; ++z) {
                    for (i32 x = nx0; x < nx0 + span && !touched; ++x)
                        touched = staleAt(x, z);
                }
                if (!touched)
                    continue;
                PyramidNode out;
                out.level = level;
                out.x = nodeX;
                out.z = nodeZ;
                out.settings = m_settings;
                // What it kept of the columns that are not stale.
                const bool had = level == PyramidTopLevel ? stale.top != nullptr
                                                          : (files & fileBit(level, nodeX, nodeZ, blockX, blockZ)) != 0;
                if (const std::shared_ptr<const PyramidNode> was = had ? node(level, nodeX, nodeZ) : nullptr;
                    was != nullptr) {
                    for (const PyramidChunk& chunk : was->chunks) {
                        if (!staleAt(chunk.key.x, chunk.key.z))
                            out.chunks.push_back(chunk);
                    }
                }
                for (const Gathered& chunk : gathered) {
                    if (chunk.key.x < nx0 || chunk.key.x >= nx0 + span || chunk.key.z < nz0 ||
                        chunk.key.z >= nz0 + span)
                        continue;
                    PyramidChunk kept;
                    kept.key = chunk.key;
                    kept.stored = chunk.stored;
                    kept.kept = chunk.kept;
                    kept.surface = chunk.surfaces[level];
                    if (kept.stored || kept.surface != nullptr)
                        out.chunks.push_back(std::move(kept));
                }
                std::sort(out.chunks.begin(), out.chunks.end(),
                          [](const PyramidChunk& a, const PyramidChunk& b) { return a.key < b.key; });
                // A node with no ground and no surface under it has no file.
                const bool nothing = std::none_of(out.chunks.begin(), out.chunks.end(), [](const PyramidChunk& chunk) {
                    return chunk.stored || hasSurface(chunk.surface);
                });
                if (level != PyramidTopLevel) {
                    const u32 bit = fileBit(level, nodeX, nodeZ, blockX, blockZ);
                    if (nothing) {
                        // Nothing there now: the top file says so, and the
                        // old file is never read again.
                        files &= ~bit;
                        const std::lock_guard<std::mutex> lock(m_lock);
                        m_nodes.erase(NodeKey{level, nodeX, nodeZ});
                        continue;
                    }
                    files |= bit;
                }
                else {
                    out.files = files;
                    out.cells.reserve(stale.cells.size());
                    for (const auto& [id, signature] : stale.cells)
                        out.cells.push_back(PyramidCell{id.x, id.z, signature, source.stampOf(id)});
                }
                const std::vector<std::byte> bytes = encodePyramidNode(out);
                if (!m_store.write || !m_store.write(level, nodeX, nodeZ, bytes))
                    return false;
                written += 1;
                keep(NodeKey{level, nodeX, nodeZ}, std::make_shared<const PyramidNode>(std::move(out)));
            }
        }
    }
    const std::lock_guard<std::mutex> lock(m_lock);
    m_stats.filesWritten += written;
    m_stats.chunksGathered += gathered.size();
    m_stats.blocksBuilt += 1;
    return true;
}

bool TerrainPyramid::seed(TerrainField& field, i32 x0, i32 x1, i32 z0, i32 z1, u32 level, const ColumnTest& summaryHere,
                          const ColumnTest& surfaceHere)
{
    if (level < PyramidFirstLevel || level > PyramidTopLevel)
        return false;
    for (u32 at = level; at <= PyramidTopLevel; ++at) {
        const i32 span = 1 << at;
        const i32 across = 1 << (PyramidTopLevel - at);
        for (i32 nodeZ = floorDivide(z0, span); nodeZ <= floorDivide(z1, span); ++nodeZ) {
            for (i32 nodeX = floorDivide(x0, span); nodeX <= floorDivide(x1, span); ++nodeX) {
                const i32 blockX = floorDivide(nodeX, across);
                const i32 blockZ = floorDivide(nodeZ, across);
                const std::shared_ptr<const PyramidNode> top = node(PyramidTopLevel, blockX, blockZ);
                if (top == nullptr)
                    return false;
                // A node of no ground has no file, and the block says so.
                if (at != PyramidTopLevel && (top->files & fileBit(at, nodeX, nodeZ, blockX, blockZ)) == 0)
                    continue;
                const std::shared_ptr<const PyramidNode> kept = at == PyramidTopLevel ? top : node(at, nodeX, nodeZ);
                if (kept == nullptr)
                    return false;
                for (const PyramidChunk& chunk : kept->chunks) {
                    if (chunk.key.x < x0 || chunk.key.x > x1 || chunk.key.z < z0 || chunk.key.z > z1)
                        continue;
                    if (at == level && chunk.stored && summaryHere(chunk.key.x, chunk.key.z) &&
                        field.findChunk(chunk.key) == nullptr)
                        field.setChunk(chunk.key, TerrainChunk::summaryOf(chunk.kept));
                    if (chunk.surface != nullptr && surfaceHere(chunk.key.x, chunk.key.z))
                        field.adoptSurface(chunk.key, at, chunk.surface);
                }
            }
        }
    }
    return true;
}

} // namespace engine::asset
