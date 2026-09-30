#pragma once

// The terrain: one sparse grid of voxels, each a material and an occupancy
// (ADR 0082).
//
// **The whole design in one paragraph.** Space is cut into cubes `VoxelSize`
// on a side. Each one says what it is made of and how full of it it is, from
// empty to full. The surface is wherever that fullness crosses one half, found
// by interpolating between neighbouring voxels -- so a ball carved out of a
// hillside lands where the brush put it rather than on the lattice. Caves,
// arches, overhangs and flat ground are all the same data, and every consumer
// -- the mesher, the colliders, the raycast, the brushes, the save file --
// asks the same grid.
//
// This replaced a hybrid (ADR 0067) that kept most ground as a height layer and
// switched columns to voxel bricks where it stopped being one. Every defect the
// owner found in it was at the join between the two, and there is no join here.
//
// **Voxel `i` covers `[i * v, (i + 1) * v)` and its sample sits at its centre**,
// `(i + 0.5) * v`. That is the convention every function below uses when it
// turns an index into metres, and `voxelCenter` is the one place that says so.
//
// **Immutable and shared, which is what keeps undo affordable.** `UndoStack`
// snapshots the whole world by value; a chunk is a `shared_ptr`, so a snapshot
// copies pointers, and an edit clones only the chunks it writes.

#include <array>
#include <atomic>
#include <compare>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::asset {

// --- Voxels ------------------------------------------------------------------

// A voxel that is entirely ground.
inline constexpr core::u8 FullOccupancy = 255;

// What one voxel holds (ADR 0114): how full it is, the material under
// (`material`, the ADR's `base`), and a material over it (`top`) showing by
// `cover`, 0 to 255 -- so paint can go on a little at a time, and over
// something else.
//
// **Air is all zero, always.** A voxel emptied by a dig keeps no memory of what
// it was made of: the digest is over bytes, and two equal worlds must hash
// equal. And **a voxel of one material has no top**: `cover` zero, or `top`
// the material under, is written `top = cover = 0`. Every write path goes
// through `canonical`.
struct Voxel
{
    core::u8 occupancy = 0;
    core::u8 material = 0;
    core::u8 top = 0;
    core::u8 cover = 0;

    [[nodiscard]] constexpr bool operator==(const Voxel&) const noexcept = default;
};

[[nodiscard]] constexpr Voxel canonical(Voxel voxel) noexcept
{
    if (voxel.occupancy == 0 || voxel.material == 0)
        return Voxel{};
    if (voxel.top == 0 || voxel.cover == 0 || voxel.top == voxel.material)
        return Voxel{voxel.occupancy, voxel.material, 0, 0};
    return voxel;
}

// Two bytes, occupancy low: the voxel under its paint. What a chunk stores and
// what the file carries.
[[nodiscard]] constexpr core::u16 packVoxel(Voxel voxel) noexcept
{
    return static_cast<core::u16>(voxel.occupancy | (voxel.material << 8));
}

[[nodiscard]] constexpr Voxel unpackVoxel(core::u16 packed) noexcept
{
    return Voxel{static_cast<core::u8>(packed & 0xFF), static_cast<core::u8>(packed >> 8)};
}

// And its paint, two bytes more, `top` low -- stored apart, in a chunk that has
// any, so ground nobody painted costs what it did.
[[nodiscard]] constexpr core::u16 packPaint(Voxel voxel) noexcept
{
    return static_cast<core::u16>(voxel.top | (voxel.cover << 8));
}

[[nodiscard]] constexpr Voxel withPaint(Voxel voxel, core::u16 paint) noexcept
{
    voxel.top = static_cast<core::u8>(paint & 0xFF);
    voxel.cover = static_cast<core::u8>(paint >> 8);
    return voxel;
}

// Occupancy as a fraction, and back. Rounded to nearest, so a round trip of a
// byte is exact.
[[nodiscard]] constexpr float occupancyOf(Voxel voxel) noexcept
{
    return static_cast<float>(voxel.occupancy) / static_cast<float>(FullOccupancy);
}

[[nodiscard]] core::u8 quantiseOccupancy(float fraction) noexcept;

// **How many voxels occupancy takes to go from full to empty across a surface.**
//
// Four, not one, and the difference is measured rather than preferred. A voxel's
// occupancy is written from a distance to the surface, and ground laid from a
// height -- a heightmap, a raised hill, a converted world -- knows only its
// VERTICAL distance. With a one-voxel ramp, the voxel beside one on a slope
// steeper than 45 degrees is already clamped full or empty, the crossing between
// them lands in the wrong place, and every hillside came out as a staircase of
// terraces a voxel high. Across four voxels the field stays linear between
// neighbours on slopes to 63 degrees, which puts every crossing exactly on the
// surface; `writeHeights` also divides by the slope, which covers the rest.
//
// What it costs is a few more rows of partial voxels under each surface, and a
// chunk of flat ground a few kilobytes larger. The reference platform ramps over
// one voxel and its hillsides show the terraces.
inline constexpr float RampVoxels = 4.0f;

// The occupancy of a voxel whose centre is `distance` metres from a surface,
// negative inside: one half on the surface, full at `RampVoxels / 2` voxels in,
// empty as far out. Every brush's shape goes through this.
[[nodiscard]] constexpr float rampOccupancy(double distance, double voxelSize) noexcept
{
    const double value = 0.5 - distance / (static_cast<double>(RampVoxels) * voxelSize);
    return static_cast<float>(value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value));
}

// --- Chunks ------------------------------------------------------------------

// A chunk is 32 voxels on a side: 32 m at the default metre voxel. The
// reference platform's storage chunk, and the size a column of the level-of-
// detail quadtree's leaves is.
inline constexpr core::u32 ChunkEdge = 32;
inline constexpr core::u32 ChunkRows = ChunkEdge * ChunkEdge;
inline constexpr core::u32 ChunkVolume = ChunkRows * ChunkEdge;

// **How deep ground laid on empty terrain goes**, in metres under the lowest
// surface a verb lays: `writeHeights`, `fillFlat` and a first raise on empty
// columns. Rounded down to a chunk boundary and never past the world's floor.
//
// A slab and not a pillar to the floor, and the owner is why: ground laid from
// the floor at -256 m stood as a tower wherever it met the air, and ground
// extended sideways with a brush stood as a different one beside it. A slab
// this deep has walls and a bottom of the same height all round, and room
// under the surface for any cave a person digs by hand.
inline constexpr float LaidDepth = 32.0f;

// Level 0 is the voxels themselves; level L averages 2^L on a side, down to one
// value for the whole chunk at level 5.
inline constexpr core::u32 ChunkLevels = 6;

// Deliberately `i32` and padding-free: hashed with `Hasher::pod`.
//
// **Sorted x, then z, then y**, not in declaration order, so one column of
// chunks is one contiguous run of a sorted list -- which is what `HeightAt`,
// the level-of-detail columns and the colliders walk.
struct ChunkKey
{
    core::i32 x = 0;
    core::i32 y = 0;
    core::i32 z = 0;

    [[nodiscard]] constexpr std::strong_ordering operator<=>(const ChunkKey& other) const noexcept
    {
        if (const auto order = x <=> other.x; order != 0)
            return order;
        if (const auto order = z <=> other.z; order != 0)
            return order;
        return y <=> other.y;
    }
    [[nodiscard]] constexpr bool operator==(const ChunkKey&) const noexcept = default;
};

// **How far a field reaches** (audit F12): a chunk key within a million of the
// origin either way. Past it, `key * ChunkEdge` overflows an `i32` in the
// mesher and the samplers; a file naming a chunk there is malformed.
inline constexpr core::i32 MaxChunkKey = 1 << 20;
[[nodiscard]] constexpr bool chunkKeyInRange(ChunkKey key) noexcept
{
    const auto in = [](core::i32 value) { return value >= -MaxChunkKey && value <= MaxChunkKey; };
    return in(key.x) && in(key.y) && in(key.z);
}

// Floor division, which `-1 / 32` in C++ is not. Here once so a world with its
// origin in the middle does not put the left half of every cave in the wrong
// chunk.
[[nodiscard]] constexpr core::i32 floorDiv(core::i32 value, core::i32 divisor) noexcept
{
    const core::i32 quotient = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] constexpr core::i32 floorMod(core::i32 value, core::i32 divisor) noexcept
{
    const core::i32 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

[[nodiscard]] constexpr ChunkKey chunkOf(core::i32 x, core::i32 y, core::i32 z) noexcept
{
    constexpr auto edge = static_cast<core::i32>(ChunkEdge);
    return ChunkKey{floorDiv(x, edge), floorDiv(y, edge), floorDiv(z, edge)};
}

// One 32-cubed block of voxels, stored by rows.
//
// **A row is 32 voxels along x, at one (y, z)**, and it is either one value or
// 32 of them. Ground is mostly rows that are all ground or all air: flat ground
// at a metre is one row in thirty-two that is not uniform, and a chunk of it is
// about four kilobytes rather than sixty-four. The reference platform measured
// the same trick at a sixth of the memory. A chunk whose every row is the same
// single value is just that value, with no rows at all.
//
// **Mutable only through a `TerrainField`**, which clones a chunk another
// snapshot still holds before it writes. The lazy digests are dropped by every
// write.
class TerrainChunk
{
public:
    TerrainChunk() = default;
    explicit TerrainChunk(Voxel fill) noexcept
    {
        const Voxel voxel = canonical(fill);
        m_main.value = packVoxel(voxel);
        m_paint.value = packPaint(voxel);
    }

    [[nodiscard]] Voxel get(core::u32 x, core::u32 y, core::u32 z) const noexcept;

    // Writes one voxel, paint and all, answering whether it changed. Leaves the
    // chunk un-normalised: call `normalize` when a batch of writes is done.
    bool set(core::u32 x, core::u32 y, core::u32 z, Voxel voxel);

    // The 32 voxels of one row, packed: under their paint, and their paint.
    void readRow(core::u32 y, core::u32 z, std::span<core::u16, ChunkEdge> out) const noexcept;
    void readPaintRow(core::u32 y, core::u32 z, std::span<core::u16, ChunkEdge> out) const noexcept;
    // Replaces one row. Stored uniform when it is. A row written under paint
    // keeps the paint, and `normalize` drops any the row made non-canonical.
    void writeRow(core::u32 y, core::u32 z, std::span<const core::u16, ChunkEdge> row);
    void writePaintRow(core::u32 y, core::u32 z, std::span<const core::u16, ChunkEdge> row);

    // Collapses rows that became uniform, compacts the dense storage in row
    // order and collapses a chunk of one value; drops paint on air and paint of
    // the material under. **Canonical afterwards**, so two chunks holding the
    // same voxels hold the same bytes -- which is what the digest and the save
    // file rely on.
    void normalize();

    [[nodiscard]] bool uniform() const noexcept { return m_main.rows.empty() && m_paint.rows.empty(); }
    // The one value, when `uniform`.
    [[nodiscard]] Voxel value() const noexcept { return withPaint(unpackVoxel(m_main.value), m_paint.value); }
    [[nodiscard]] bool empty() const noexcept { return uniform() && m_main.value == 0; }
    // Whether any voxel of it carries paint (ADR 0114).
    [[nodiscard]] bool painted() const noexcept { return !m_paint.rows.empty() || m_paint.value != 0; }

    // How many rows are stored voxel by voxel. For the memory figures and tests.
    [[nodiscard]] core::usize denseRows() const noexcept { return m_main.dense.size() / ChunkEdge; }
    [[nodiscard]] core::usize bytes() const noexcept;

    // xxh3 over the canonical form. Lazy, and dropped by any write.
    [[nodiscard]] core::u64 digest() const noexcept;

    // **The digest of the part of this chunk a neighbour's mesh reads.** The
    // mesher reaches two voxels past a region's side, so a chunk at offset
    // (`dx`, `dy`, `dz`) from this one -- each -1, 0 or 1 -- reads the two
    // layers of this chunk against it on every axis it is offset along, and all
    // of it on the others: a face, an edge or a corner. A mesh keyed on these
    // rather than on its neighbours' whole digests is not rebuilt for an edit
    // in the middle of the chunk next door. (0, 0, 0) is `digest`. Lazy.
    [[nodiscard]] core::u64 borderDigest(core::i32 dx, core::i32 dy, core::i32 dz) const noexcept;

private:
    [[nodiscard]] static core::u32 rowIndex(core::u32 y, core::u32 z) noexcept { return y * ChunkEdge + z; }
    void invalidate() noexcept;
    // Paint the voxels under it cannot carry, dropped.
    void canonicalizePaint();

    // **Sixteen bits a voxel, stored by rows.** Uniform: `rows` empty and
    // `value` the value. Otherwise one entry per row: bit 31 set means dense,
    // and the low bits index `dense` in whole rows; clear means the low sixteen
    // bits are the row's one value. A chunk has two: the voxels under their
    // paint, and the paint, which is all zero -- and costs nothing -- where
    // nobody painted.
    struct Layer
    {
        core::u16 value = 0;
        std::vector<core::u32> rows;
        std::vector<core::u16> dense;

        [[nodiscard]] core::u16 get(core::u32 x, core::u32 row) const noexcept;
        void readRow(core::u32 row, std::span<core::u16, ChunkEdge> out) const noexcept;
        bool set(core::u32 x, core::u32 row, core::u16 packed);
        void writeRow(core::u32 row, std::span<const core::u16, ChunkEdge> values);
        void normalize();
    };
    Layer m_main;
    Layer m_paint;

    // **The lazy digests, safe from any thread** (terrain audit TA14): a chunk
    // two snapshots of a field share is read by the one meshed off the main
    // thread while the other is read on it. Two threads that both work one out
    // store the same value, and every store is atomic; a digest is published
    // before the bit that says it is there.
    struct Digests
    {
        std::atomic<core::u64> whole{0};
        std::atomic<bool> wholeValid{false};
        std::array<std::atomic<core::u64>, 27> borders{};
        std::atomic<core::u32> bordersValid{0};

        Digests() = default;
        Digests(const Digests& other) noexcept { copyFrom(other); }
        Digests& operator=(const Digests& other) noexcept
        {
            if (this != &other)
                copyFrom(other);
            return *this;
        }
        ~Digests() = default;
        void clear() noexcept
        {
            wholeValid.store(false, std::memory_order_relaxed);
            bordersValid.store(0, std::memory_order_relaxed);
        }

    private:
        void copyFrom(const Digests& other) noexcept
        {
            whole.store(other.whole.load(std::memory_order_acquire), std::memory_order_relaxed);
            wholeValid.store(other.wholeValid.load(std::memory_order_acquire), std::memory_order_release);
            for (core::usize at = 0; at < borders.size(); ++at)
                borders[at].store(other.borders[at].load(std::memory_order_acquire), std::memory_order_relaxed);
            bordersValid.store(other.bordersValid.load(std::memory_order_acquire), std::memory_order_release);
        }
    };
    mutable Digests m_digests;
};

// --- The field ---------------------------------------------------------------

struct FieldSettings
{
    // The side of one voxel, in metres.
    float voxelSize = 1.0f;

    // **The world's floor and ceiling, in metres.** No write lands outside
    // them, and a column with no ground is filled from the floor by the verbs
    // that lay ground rather than add to it (`fillFlat`, `writeHeights`,
    // `raiseBall` on empty ground).
    float minHeight = -256.0f;
    float maxHeight = 256.0f;
};

// **Settings a field can be built from** (audit F9), checked after they are
// floats: a voxel between a centimetre and 64 m, a floor and ceiling within a
// million metres, the ceiling above the floor. `voxelSize > 0` alone let an
// infinity or a denormal through -- a division by it in every sampler, and a
// loop of a billion steps across one column.
inline constexpr float MinVoxelSize = 0.01f;
inline constexpr float MaxVoxelSize = 64.0f;
inline constexpr float MaxFieldHeight = 1.0e6f;
// **A terrain's voxel is at least ten centimetres**, the smallest the editor
// offers (terrain audit P2). What reads round a point is reached in metres --
// the sky a vertex sees, the ground a body collides with -- so its cost in
// voxels grows with the cube of how small they are: at a centimetre, one
// region's mesh wanted a quarter of a gigabyte and a walking character three
// million chunk keys a tick.
inline constexpr float MinTerrainVoxelSize = 0.1f;
[[nodiscard]] inline bool saneFieldSettings(const FieldSettings& settings) noexcept
{
    const auto within = [](float value, float low, float high) { return value >= low && value <= high; };
    return within(settings.voxelSize, MinTerrainVoxelSize, MaxVoxelSize) &&
           within(settings.minHeight, -MaxFieldHeight, MaxFieldHeight) &&
           within(settings.maxHeight, -MaxFieldHeight, MaxFieldHeight) && settings.maxHeight > settings.minHeight;
}

// A sample of the field: a signed distance in metres, and what it is made of.
//
// **The distance is `(0.5 - occupancy) * RampVoxels * VoxelSize`**: negative
// inside, zero on the surface, and a distance only within the ramp round it.
// That is all the mesher and the raycast need -- they look for where it changes
// sign -- and it keeps one type between them and a grid that stores fullness.
struct FieldSample
{
    float distance = 0.0f;
    core::u8 material = 0;
    // What is painted over it, and how much (ADR 0114).
    core::u8 top = 0;
    core::u8 cover = 0;
};

// --- The surface under a chunk, gathered by level (ADR 0140) -----------------
//
// **What a coarse level draws is the level-0 surface, clustered.** Each level-0
// cell the surface passes through has a surface-net vertex; a cell at level L
// holds the `2^L` cube of them and draws their mean, their commonest material
// and their paint. A coarse lattice edge carries how many of the level-0 edges
// along it the surface crosses each way, which is where clustering keeps a
// quad. So flat ground is where it is at every level, a thin feature thins to a
// sheet and never vanishes, and a coarse cell is made of its surface.

// One coarse cell the surface passes through. Positions are summed as offsets
// from the cell's low lattice corner, in level-0 voxels, so a sum of 32 768
// stays exact enough in a float.
struct SurfaceCell
{
    // `(z * edge + y) * edge + x` inside the chunk, at the level's `edge`.
    core::u16 index = 0;
    core::u32 count = 0;
    float offset[3]{};
    float normal[3]{};
    // **The tangent planes of its level-0 vertices, summed** (ADR 0140): each
    // vertex's normal `n` and point `p` add `n n^T` (xx, xy, xz, yy, yz, zz)
    // and `n (n . p)`. Where the vertex goes, below.
    float quadric[6]{};
    float plane[3]{};
    // And `(n . p)^2`, so the planes' distance from a point is known.
    float constant = 0.0f;
    // The commonest materials under the paint, and their votes.
    std::array<core::u8, 4> materials{};
    std::array<core::u16, 4> votes{};
    // What is painted over it: the commonest paint, and the cover summed.
    core::u8 top = 0;
    core::u32 topVotes = 0;
    float cover = 0.0f;
    // `point(span)` and `error(span)` at its level, worked out once when it is
    // gathered: every node that draws it, and every child sliding onto it,
    // reads them.
    std::array<float, 3> placed{};
    float deviation = 0.0f;

    // The material most of its level-0 vertices carry, lowest id on a tie.
    [[nodiscard]] core::u8 material() const noexcept;

    // **Where its vertex goes**, as an offset from its low corner in level-0
    // voxels, inside `[0, span]`: the point nearest every one of its
    // vertices' tangent planes -- so an edge or a corner the cell holds is
    // where it is, and a gathered slab's rim does not shrink in by half a
    // cell -- and the mean of them along any direction the planes say
    // nothing about. Flat ground's is the mean exactly.
    [[nodiscard]] std::array<float, 3> point(core::u32 span) const noexcept;

    // **How far its level-0 surface is from its vertex**, in level-0 voxels:
    // the root mean square of their tangent planes' distances from `point`,
    // and half the cell where it holds more than one material or any paint,
    // which a coarse cell blends across its whole width. What a level of
    // detail is chosen by (ADR 0140): flat ground is nothing, a feature the
    // cell gathered to a point is its size.
    [[nodiscard]] float error(core::u32 span) const noexcept;
    // The same, with `point(span)` already in hand.
    [[nodiscard]] float error(core::u32 span, std::array<float, 3> at) const noexcept;
};

// One coarse lattice edge the surface crosses: from lattice point `index` one
// level cell along `axis`. `up` counts the level-0 crossings with ground on the
// low side, `down` those with it on the high side.
struct SurfaceEdge
{
    core::u16 index = 0;
    core::u8 axis = 0;
    core::u16 up = 0;
    core::u16 down = 0;
};

struct SurfaceLevel
{
    // Sorted by index, then axis; only what the surface touches.
    std::vector<SurfaceCell> cells;
    std::vector<SurfaceEdge> edges;

    [[nodiscard]] const SurfaceCell* cell(core::u16 index) const noexcept;
    [[nodiscard]] const SurfaceEdge* edge(core::u16 index, core::u8 axis) const noexcept;
};

class TerrainField
{
public:
    using Entry = std::pair<ChunkKey, std::shared_ptr<TerrainChunk>>;

    TerrainField() = default;
    explicit TerrainField(FieldSettings settings) noexcept : m_settings(settings) {}

    [[nodiscard]] const FieldSettings& settings() const noexcept { return m_settings; }

    // Where voxel `index` is centred on one axis, in the field's own metres.
    [[nodiscard]] double voxelCenter(core::i32 index) const noexcept
    {
        return (static_cast<double>(index) + 0.5) * static_cast<double>(m_settings.voxelSize);
    }
    // The voxel a coordinate in metres falls in, on one axis.
    [[nodiscard]] core::i32 voxelIndex(double metres) const noexcept;

    // --- Reading ---------------------------------------------------------

    [[nodiscard]] Voxel voxel(core::i32 x, core::i32 y, core::i32 z) const noexcept;
    [[nodiscard]] FieldSample sample(core::i32 x, core::i32 y, core::i32 z) const noexcept;

    [[nodiscard]] const TerrainChunk* findChunk(ChunkKey key) const noexcept;
    [[nodiscard]] core::usize chunkCount() const noexcept { return m_chunks.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_chunks.empty(); }
    [[nodiscard]] std::span<const Entry> chunks() const noexcept { return m_chunks; }
    // **The lowest and highest chunk keys on each axis**, kept as chunks come
    // and go (terrain audit T5): a raycast walked every chunk for the field's
    // box, every cast. Only for a field that is not `empty()`.
    [[nodiscard]] std::pair<ChunkKey, ChunkKey> chunkBox() const noexcept { return {m_boxLow, m_boxHigh}; }
    // Every key, sorted.
    [[nodiscard]] std::vector<ChunkKey> chunkKeys() const;
    // The chunks of one column, lowest first.
    [[nodiscard]] std::span<const Entry> column(core::i32 chunkX, core::i32 chunkZ) const noexcept;

    // **The top of the ground in one voxel column**, in the field's metres: the
    // highest place its occupancy crosses one half, going up. Nothing where the
    // column holds no ground at all. Caves under it do not change the answer.
    [[nodiscard]] std::optional<float> columnTop(core::i32 x, core::i32 z) const noexcept;
    // The bottom of the ground in one voxel column: the lowest place occupancy
    // crosses one half, going down. Nothing where the column holds no ground.
    [[nodiscard]] std::optional<float> columnBottom(core::i32 x, core::i32 z) const noexcept;

    // xxh3 over every chunk's key and digest, in key order.
    [[nodiscard]] core::u64 digest() const noexcept;

    // Bytes held by every chunk. Shared chunks are counted in full.
    [[nodiscard]] core::usize bytes() const noexcept;

    // --- Writing ---------------------------------------------------------
    //
    // A write clones the chunk it touches when another snapshot holds it, and
    // leaves every other chunk shared.

    // One voxel. Normalises its chunk afterwards, so this is for single writes;
    // a brush goes through `FieldWriter`.
    bool setVoxel(core::i32 x, core::i32 y, core::i32 z, Voxel voxel);

    // A chunk to write into, cloned if shared and created as air if absent.
    // **Valid until the next call that inserts or removes a chunk.** Whoever
    // writes through it calls `finishChunk` afterwards.
    [[nodiscard]] TerrainChunk* chunkFor(ChunkKey key);
    // Normalises a written chunk, and drops it when it is all air.
    void finishChunk(ChunkKey key);

    // Replaces a chunk wholesale -- the load path. An empty chunk is dropped.
    void setChunk(ChunkKey key, std::shared_ptr<TerrainChunk> chunk);
    void removeChunk(ChunkKey key);

    // **Takes every chunk of `from` this field does not already hold, SHARING
    // it** -- the load path of a streamed cell. What this field holds wins,
    // because it is newer; and because the chunk is shared, whoever keeps
    // `from` keeps a reference, so the first edit here clones it. That is how
    // an evicting streamer tells a cell somebody changed from one nobody did.
    void shareFrom(const TerrainField& from);
    // **The same, less every key `known` already holds** -- the load path
    // proper, with `known` the package's ground as far as it was ever loaded
    // (`TerrainComponent::shipped`). A key there and not here was removed on
    // purpose: dug away, or removed by the authority. Without it, "dug to
    // nothing" and "not loaded" were the same fact, and a cell streamed out
    // and back brought every hole's ground back (terrain audit G1).
    void shareFrom(const TerrainField& from, const TerrainField& known);
    // **Each chunk this field holds, as `newer` holds it** -- where it holds
    // it -- and no key added: a world put back keeps the keys it had loaded,
    // with what the package on disk now holds at them.
    void refreshFrom(const TerrainField& newer);

    // Drops every chunk named, in one pass. `keys` sorted.
    void removeAll(std::span<const ChunkKey> keys);

    // Widens or narrows the world's floor and ceiling. Resamples nothing.
    void setHeightRange(float minHeight, float maxHeight) noexcept;

    // **One chunk's gathered surface at one level** (ADR 0140), if gathered
    // for `content` -- the digest of what it was gathered from, its own voxels
    // and its neighbours' borders. Content-addressed, so copies of a field
    // share the cache and a copy that differs simply misses. Written on one
    // thread (`prepareSurface`, `cacheSurfaces`), before anything meshes from
    // it.
    // **Safe from any thread** (terrain audit TA14): a field meshed off the
    // main thread shares this cache with the one edited on it. What it hands
    // back is held, so an entry replaced meanwhile stays whole for its reader.
    [[nodiscard]] std::shared_ptr<const SurfaceLevel> cachedSurface(ChunkKey key, core::u32 level,
                                                                    core::u64 content) const;
    void cacheSurface(ChunkKey key, core::u32 level, core::u64 content,
                      std::shared_ptr<const SurfaceLevel> surface) const;

private:
    struct SurfaceEntry
    {
        core::u64 content = 0;
        std::shared_ptr<const SurfaceLevel> surface;
    };
    struct SurfaceCache
    {
        mutable std::shared_mutex lock;
        std::map<std::pair<ChunkKey, core::u32>, SurfaceEntry> entries;
    };

    FieldSettings m_settings;
    // Sorted by key. Never a hash map (R10): the mesher, the save file and the
    // world hash all walk this, and the walk must be a fact about the world.
    std::vector<Entry> m_chunks;
    ChunkKey m_boxLow{};
    ChunkKey m_boxHigh{};
    // Widens the box to `key`, for a chunk added; and works it out again from
    // every chunk, for one taken away.
    void growBox(ChunkKey key) noexcept;
    void rebox() noexcept;
    // Shared by copies, looked up by key and never walked: nothing about the
    // world reaches it but what it was asked.
    std::shared_ptr<SurfaceCache> m_surfaces = std::make_shared<SurfaceCache>();
};

// A batch of voxel writes: caches the chunk it is in, and normalises every
// chunk it touched when it is done. **Every brush writes through one**, which
// is what makes a stroke clone each chunk once and leave it canonical.
class FieldWriter
{
public:
    explicit FieldWriter(TerrainField& field) noexcept : m_field(field) {}
    ~FieldWriter() { finish(); }
    FieldWriter(const FieldWriter&) = delete;
    FieldWriter& operator=(const FieldWriter&) = delete;

    [[nodiscard]] Voxel get(core::i32 x, core::i32 y, core::i32 z) const noexcept { return m_field.voxel(x, y, z); }
    // Answers whether the voxel changed. A voxel written with no paint over
    // the material it already is keeps the paint it had (ADR 0114).
    bool set(core::i32 x, core::i32 y, core::i32 z, Voxel voxel);
    // Written as it is, paint and all: the paint verbs.
    bool setExact(core::i32 x, core::i32 y, core::i32 z, Voxel voxel);

    // Normalises and drops what the batch left empty. Idempotent.
    void finish();

    [[nodiscard]] core::u32 changed() const noexcept { return m_changed; }
    // The chunks written, sorted.
    [[nodiscard]] const std::vector<ChunkKey>& touched() const noexcept { return m_touched; }

private:
    [[nodiscard]] TerrainChunk* chunkAt(core::i32 x, core::i32 y, core::i32 z, bool air);

    TerrainField& m_field;
    std::vector<ChunkKey> m_touched;
    ChunkKey m_lastKey{};
    TerrainChunk* m_last = nullptr;
    core::u32 m_changed = 0;
};

// --- Editing -----------------------------------------------------------------
//
// Every verb writes occupancies over its brush's box with the ramp (`RampVoxels`) at
// the boundary, so the surface lands where the brush says to within the
// interpolation, not on a lattice line. **Additions take the larger occupancy
// and removals the smaller**, so a brush never undoes ground it did not reach.

struct EditReport
{
    // Voxels whose value changed.
    core::u32 touched = 0;
    // **Refused whole, for its size** (audit S10): a brush whose box holds
    // more than `MaxEditVoxels` changes nothing. `FillBall(v, 1e5, 1)` walked
    // eight quadrillion voxels in C++, where no watchdog reaches, until the
    // machine ran out of memory.
    bool refused = false;
    // The most the verb that refused it walks, for the message that says so.
    core::u64 limit = 0;
};

// A box of 512 voxels a side: far past any brush a person holds, and small
// enough that the worst edit is a pause rather than a hang.
inline constexpr core::u64 MaxEditVoxels = 512ull * 512ull * 512ull;

// **Smoothing's own bound**, 160 voxels a side: it holds three buffers over
// its box and a margin, ten bytes a voxel, so `MaxEditVoxels` let one call
// ask for a gigabyte and a half (terrain audit B5). A brush past it is past
// what a smooth stamp does in a frame anyway.
inline constexpr core::u64 MaxSmoothVoxels = 160ull * 160ull * 160ull;

// **What one `writeHeights` may lay** (terrain audit TA15), in voxels, as it
// estimates them from its table before it writes: four edits' worth, since a
// heightmap is a world rather than a brush -- four kilometres of rolling
// ground at a metre voxel fit. A table steep everywhere, every chunk of every
// column dense, does not: a 4096-square checkerboard asked for a quarter of a
// million dense chunks.
inline constexpr core::u64 MaxHeightVoxels = 4ull * MaxEditVoxels;

// Adds a ball of ground, or removes one when `material` is zero.
EditReport fillBall(TerrainField& field, core::DVec3 center, double radius, core::u8 material);

// The same, as an axis-aligned box. `size` is the full extent.
EditReport fillBlock(TerrainField& field, core::DVec3 center, core::Vec3 size, core::u8 material);

// The same, as an upright cylinder `height` tall.
EditReport fillCylinder(TerrainField& field, core::DVec3 center, double height, double radius, core::u8 material);

// **Moves the top of every column in a square to `height`**, as volume: ground
// is added from the column's top up to it, or taken from the top down to it,
// and whatever is under the top -- a tunnel -- stays. A column with no ground at
// all is filled from the world's floor.
EditReport fillFlat(TerrainField& field, core::DVec3 center, float size, float height, core::u8 material);

// **A heightmap, written whole**: one height per voxel column, row after row
// along +z, starting at column (`firstX`, `firstZ`), `columns` wide. On
// `fillFlat`'s terms, column by column. Heights clamp into the field's range; a
// NaN is skipped. Chunks the ground covers entirely are written as one value,
// so a square kilometre is not a billion voxel writes.
EditReport writeHeights(TerrainField& field, core::i32 firstX, core::i32 firstZ, core::u32 columns,
                        std::span<const float> heights, core::u8 material);

// The same with a material per column, parallel to `heights`. A column whose
// material is zero is skipped. What a converted hybrid terrain is laid with.
EditReport writeHeights(TerrainField& field, core::i32 firstX, core::i32 firstZ, core::u32 columns,
                        std::span<const float> heights, std::span<const core::u8> materials);

// Softens the ground in a ball: every voxel moves towards the mean of its
// twenty-seven neighbours by `strength`, less towards the rim. Clamped to 0..1;
// a ball of more than `MaxSmoothVoxels` is refused.
EditReport smoothBall(TerrainField& field, core::DVec3 center, double radius, float strength);

// Pulls the ground in a ball towards a level plane at `height` -- taking what
// is above it away and filling what is below it -- by `strength`, less towards
// the rim. The height is given rather than sampled so a stroke dragged across
// a slope levels to where it started.
EditReport flattenBall(TerrainField& field, core::DVec3 center, double radius, float height, float strength);

// Raises the ground under a disc by `amount` metres at the centre, falling
// smoothly to nothing at the rim -- or lowers it, when `amount` is negative.
//
// **Moves the surface, not a ball**: each column inside the disc is shifted up
// by its falloff, within the brush's reach above and below `center`, and the
// larger of the old and the shifted occupancy is kept (the smaller when
// lowering). So a hill rises without an overhang at its rim, and a tunnel below
// the reach stays where it is. Where a column in reach holds no ground and
// `material` is not zero, raising lays ground from `center`'s height up.
EditReport raiseBall(TerrainField& field, core::DVec3 center, double radius, float amount, core::u8 material = 0);

// Grows the ground in a ball outwards by `amount` metres at the centre, falling
// smoothly to nothing at the rim -- or wears it away, when `amount` is negative.
//
// **Along the surface's own normal, not up**: a field rises, a cliff comes out
// sideways and an overhang grows down, which is what `raiseBall` cannot do. It
// grows from ground that is there, so over nothing it does nothing, and one call
// moves the surface at most half the ramp (two voxels). New ground takes its
// neighbour's material, or `material` where there is none (1 if that is zero).
EditReport growBall(TerrainField& field, core::DVec3 center, double radius, float amount, core::u8 material = 0);

// **How a stroke of paint goes on** (ADR 0114): over what is there, a little at
// a time, under it, or off it.
enum class PaintMode : core::u8
{
    // The ground becomes `material`, whatever showed before.
    Replace,
    // `material` shows more over what is there, by the stroke's strength.
    Blend,
    // `material` goes under: what was painted over it stays.
    Under,
    // What was painted over the ground shows less, revealing what is under.
    Erase,
};

// **Where a stroke may paint at all** (the terrain tools, B4): only ground
// within a slope, within a height, or of chosen materials -- a road that stays
// off the cliffs, snow above a line, moss over rock and nothing else.
struct PaintMask
{
    bool bySlope = false;
    // Degrees from level, both ends included.
    float slopeMin = 0.0f;
    float slopeMax = 90.0f;
    bool byHeight = false;
    // The field's own metres.
    float heightMin = 0.0f;
    float heightMax = 0.0f;
    bool byMaterial = false;
    // Which materials under the paint it may touch, one bit an id.
    std::array<core::u64, 4> materials{};

    [[nodiscard]] bool allows(core::u8 material) const noexcept
    {
        return !byMaterial || (materials[material / 64u] & (core::u64{1} << (material % 64u))) != 0;
    }
    void allow(core::u8 material) noexcept { materials[material / 64u] |= core::u64{1} << (material % 64u); }
};

struct PaintOptions
{
    PaintMode mode = PaintMode::Replace;
    // 0 to 1: how much a `Blend` or an `Erase` stamp moves the cover, at the
    // middle of the ball.
    float strength = 1.0f;
    // 0 hard to 1 soft: how much less it moves towards the rim.
    float falloff = 0.0f;
    PaintMask mask;
};

// Changes what the ground is made of in a ball, without moving it. Zero is
// refused rather than treated as erase (`Erase` is the mode for that).
EditReport paintBall(TerrainField& field, core::DVec3 center, double radius, core::u8 material,
                     PaintOptions options = {});

// **Hills from noise** (the terrain tools, B4): a height for each of
// `columns` x `rows` columns from `firstX`, `firstZ`, layered value noise --
// `octaves` of it, the largest `scale` metres across -- between `low` and
// `high` field metres. A pure function of its arguments and the seed: the same
// hills on every machine, for `writeHeights`.
struct HillSettings
{
    core::u32 seed = 1;
    core::u32 octaves = 5;
    float scale = 128.0f;
    float low = 0.0f;
    float high = 32.0f;
};
[[nodiscard]] std::vector<float> hillHeights(const TerrainField& field, core::i32 firstX, core::i32 firstZ,
                                             core::u32 columns, core::u32 rows, const HillSettings& settings);

// **The top of each column** of a square, field metres, first row first -- a
// column with no ground at `empty`. What a heightmap is exported from.
[[nodiscard]] std::vector<float> columnHeights(const TerrainField& field, core::i32 firstX, core::i32 firstZ,
                                               core::u32 columns, core::u32 rows, float empty);

// Every voxel of material `from` in the box between the two corners becomes
// `to`. Zero for either is refused.
EditReport replaceMaterial(TerrainField& field, core::DVec3 minCorner, core::DVec3 maxCorner, core::u8 from,
                           core::u8 to);

// The height of the top of the ground at this point, in metres, interpolated
// between the four columns around it -- or nothing where there is no ground.
[[nodiscard]] std::optional<float> heightAt(const TerrainField& field, double x, double z);

// --- Sampling and raycasting -------------------------------------------------

// The field at any point, trilinear between the eight voxel centres around it.
[[nodiscard]] FieldSample sampleField(const TerrainField& field, core::DVec3 at);

struct TerrainHit
{
    // Field space, f64 (ADR 0014).
    core::DVec3 position;
    // The field's gradient at the hit.
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    double distance = 0.0;
    core::u8 material = 0;
    // What is painted over the voxel hit, and how much (ADR 0114): what the
    // ground is drawn as, with the rules (`drawnMaterial`).
    core::u8 top = 0;
    core::u8 cover = 0;
};

// **Casts a ray at the field itself, with no physics involved.** Marched a half
// voxel at a time and bisected at the crossing; chunks of air are skipped whole.
// A ray starting inside the ground hits at once.
[[nodiscard]] std::optional<TerrainHit> raycastField(const TerrainField& field, core::DVec3 origin,
                                                     core::Vec3 direction, double maxDistance);

} // namespace engine::asset
