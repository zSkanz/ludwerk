// The brushes (ADR 0082).
//
// Every verb here is the same shape: walk the voxels in the brush's box, work
// out a new occupancy from the old one, and write it through a `FieldWriter`.
// Nothing examines a column, promotes anything or re-derives an encoding --
// that was where every defect of the hybrid lived (D161, D162, D163).
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/core/dmath.h"
#include "engine/jobs/jobs.h"

namespace engine::asset {
namespace {

using core::DVec3;
using core::i32;
using core::u32;
using core::u8;
using core::usize;

// **The ramp**: how full a voxel is when its centre is `distance` metres from a
// surface (negative inside) -- `rampOccupancy`, which says why it is four voxels
// wide. Every brush's shape is a signed distance fed through this.
[[nodiscard]] float ramp(double distance, double voxel) noexcept
{
    return rampOccupancy(distance, voxel);
}

// How many voxels past a surface a write has to reach for the ramp to be whole:
// its half width, and one more for the voxel the surface is in.
constexpr i32 RampReach = static_cast<i32>(RampVoxels / 2.0f) + 1;

// 1 at the centre, 0 at the rim, flat at both ends.
[[nodiscard]] float falloff(double distance, double radius) noexcept
{
    if (!(radius > 0.0) || distance >= radius)
        return 0.0f;
    const double s = 1.0 - distance / radius;
    return static_cast<float>(s * s * (3.0 - 2.0 * s));
}

// The voxels whose centres lie in the world's floor-to-ceiling band.
struct Band
{
    i32 low = 0;
    i32 high = -1;
};

[[nodiscard]] Band bandOf(const TerrainField& field) noexcept
{
    const double voxel = static_cast<double>(field.settings().voxelSize);
    // Centre `(i + 0.5) v` inside `[min, max]`.
    return Band{static_cast<i32>(std::ceil(static_cast<double>(field.settings().minHeight) / voxel - 0.5)),
                static_cast<i32>(std::floor(static_cast<double>(field.settings().maxHeight) / voxel - 0.5))};
}

// Where ground laid on empty columns starts, as a voxel row: `LaidDepth` under
// `lowest`, rounded down to a chunk boundary so the slab's bottom is whole
// chunks, and never under the floor.
[[nodiscard]] i32 laidBase(const TerrainField& field, double lowest, const Band& band) noexcept
{
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    const i32 row = field.voxelIndex(lowest - static_cast<double>(LaidDepth));
    return std::max(floorDiv(row, edge) * edge, band.low);
}

// The inclusive voxel box around `[low, high]` in metres, widened by the ramp's
// reach so its outer half is written too, and clamped to the band on y.
struct Box
{
    i32 minX = 0;
    i32 minY = 0;
    i32 minZ = 0;
    i32 maxX = -1;
    i32 maxY = -1;
    i32 maxZ = -1;
    // Past `limit`, and so emptied.
    bool refused = false;
    core::u64 limit = MaxEditVoxels;

    [[nodiscard]] bool empty() const noexcept { return maxX < minX || maxY < minY || maxZ < minZ; }

    // Emptied and marked when it holds more than an edit may walk.
    void bound() noexcept
    {
        if (empty())
            return;
        const auto extent = [](i32 low, i32 high) {
            return static_cast<core::u64>(static_cast<core::i64>(high) - low + 1);
        };
        const core::u64 x = extent(minX, maxX);
        const core::u64 y = extent(minY, maxY);
        const core::u64 z = extent(minZ, maxZ);
        // Multiplied only while it cannot overflow: each side is at most 2^31.
        if (x > limit || y > limit || z > limit || x * y > limit || x * y * z > limit) {
            refused = true;
            maxX = minX - 1;
        }
    }
};

[[nodiscard]] Box boxOf(const TerrainField& field, DVec3 low, DVec3 high) noexcept
{
    const double reach = static_cast<double>(field.settings().voxelSize) * static_cast<double>(RampReach);
    const Band band = bandOf(field);
    Box box;
    // A brush somewhere that is not a place: nothing, rather than the voxels
    // round the origin a NaN's index falls on (terrain audit B6).
    if (!std::isfinite(low.x) || !std::isfinite(low.y) || !std::isfinite(low.z) || !std::isfinite(high.x) ||
        !std::isfinite(high.y) || !std::isfinite(high.z))
        return box;
    box.minX = field.voxelIndex(low.x - reach);
    box.minY = std::max(field.voxelIndex(low.y - reach), band.low);
    box.minZ = field.voxelIndex(low.z - reach);
    box.maxX = field.voxelIndex(high.x + reach);
    box.maxY = std::min(field.voxelIndex(high.y + reach), band.high);
    box.maxZ = field.voxelIndex(high.z + reach);
    box.bound();
    return box;
}

// **Walks a box in chunk-then-row order**: z, then y, then x innermost, so
// consecutive voxels share a chunk and a row and the writer's cached chunk
// stays warm.
template <class Visit>
void walk(const Box& box, Visit&& visit)
{
    for (i32 z = box.minZ; z <= box.maxZ; ++z) {
        for (i32 y = box.minY; y <= box.maxY; ++y) {
            for (i32 x = box.minX; x <= box.maxX; ++x)
                visit(x, y, z);
        }
    }
}

// Adds (material not zero) or removes (material zero) a shape given as a signed
// distance at each voxel centre.
template <class Distance>
EditReport applyShape(TerrainField& field, const Box& box, u8 material, Distance&& distanceAt)
{
    EditReport report;
    if (box.empty()) {
        report.refused = box.refused;
        return report;
    }
    const double voxel = static_cast<double>(field.settings().voxelSize);
    FieldWriter writer(field);
    walk(box, [&](i32 x, i32 y, i32 z) {
        const DVec3 centre{field.voxelCenter(x), field.voxelCenter(y), field.voxelCenter(z)};
        const float inside = ramp(distanceAt(centre), voxel);
        if (inside <= 0.0f && material != 0)
            return;
        const Voxel old = writer.get(x, y, z);
        if (material != 0) {
            // A union: the larger occupancy, and the brush's material where the
            // brush is what made it larger.
            const u8 occupancy = quantiseOccupancy(inside);
            if (occupancy > old.occupancy)
                writer.set(x, y, z, Voxel{occupancy, material});
        }
        else {
            // A subtraction: never fuller than the brush leaves room for.
            const u8 room = quantiseOccupancy(1.0f - inside);
            if (room < old.occupancy)
                writer.set(x, y, z, Voxel{room, old.material});
        }
    });
    writer.finish();
    report.touched = writer.changed();
    return report;
}

[[nodiscard]] double length(double x, double y, double z) noexcept
{
    return std::sqrt(x * x + y * y + z * z);
}

// **The voxels of a box, read a chunk at a time**, x fastest, then y, then z.
// Read voxel by voxel it is a search of the chunk table per voxel, and a big
// brush's box is a hundred thousand of them: the owner's "smooth stalls".
void readBox(const TerrainField& field, i32 x0, i32 y0, i32 z0, i32 sizeX, i32 sizeY, i32 sizeZ,
             std::vector<Voxel>& out)
{
    out.assign(static_cast<usize>(sizeX) * static_cast<usize>(sizeY) * static_cast<usize>(sizeZ), Voxel{});
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    const auto slot = [&](i32 x, i32 y, i32 z) {
        return (static_cast<usize>(z - z0) * static_cast<usize>(sizeY) + static_cast<usize>(y - y0)) *
                   static_cast<usize>(sizeX) +
               static_cast<usize>(x - x0);
    };
    // **A chunk column a range, on the pool**: each column fills its own part
    // of `out`, and the field is only read.
    const i32 firstColumnX = floorDiv(x0, edge);
    const i32 firstColumnZ = floorDiv(z0, edge);
    const auto columnsX = static_cast<usize>(floorDiv(x0 + sizeX - 1, edge) - firstColumnX + 1);
    const auto columnsZ = static_cast<usize>(floorDiv(z0 + sizeZ - 1, edge) - firstColumnZ + 1);
    const auto readColumns = [&](usize firstColumn, usize endColumn, u32) noexcept {
        std::array<core::u16, ChunkEdge> row{};
        std::array<core::u16, ChunkEdge> paint{};
        for (usize column = firstColumn; column < endColumn; ++column) {
            const i32 cz = firstColumnZ + static_cast<i32>(column / columnsX);
            const i32 cx = firstColumnX + static_cast<i32>(column % columnsX);
            for (const TerrainField::Entry& entry : field.column(cx, cz)) {
                const i32 cy = entry.first.y;
                const i32 lowY = std::max(y0, cy * edge);
                const i32 highY = std::min(y0 + sizeY, (cy + 1) * edge);
                if (lowY >= highY)
                    continue;
                const i32 lowX = std::max(x0, cx * edge);
                const i32 highX = std::min(x0 + sizeX, (cx + 1) * edge);
                const i32 lowZ = std::max(z0, cz * edge);
                const i32 highZ = std::min(z0 + sizeZ, (cz + 1) * edge);
                const TerrainChunk& chunk = *entry.second;
                for (i32 z = lowZ; z < highZ; ++z) {
                    for (i32 y = lowY; y < highY; ++y) {
                        Voxel* first = &out[slot(lowX, y, z)];
                        if (chunk.uniform()) {
                            std::fill(first, first + (highX - lowX), chunk.value());
                            continue;
                        }
                        const auto ly = static_cast<u32>(y - cy * edge);
                        const auto lz = static_cast<u32>(z - cz * edge);
                        chunk.readRow(ly, lz, row);
                        // With its paint (ADR 0114), which a smooth carries.
                        if (chunk.painted()) {
                            chunk.readPaintRow(ly, lz, paint);
                            for (i32 x = lowX; x < highX; ++x) {
                                const auto at = static_cast<usize>(x - cx * edge);
                                first[x - lowX] = withPaint(unpackVoxel(row[at]), paint[at]);
                            }
                            continue;
                        }
                        for (i32 x = lowX; x < highX; ++x)
                            first[x - lowX] = unpackVoxel(row[static_cast<usize>(x - cx * edge)]);
                    }
                }
            }
        }
    };
    jobs::parallelFor("terrain.readBox", jobs::Domain::SimVisible, 0, columnsX * columnsZ, 1, readColumns);
}

// The occupancies of one voxel column over `[low, high]`, read once so a brush
// that shifts or blurs reads the field as it was before the stroke.
struct ColumnBuffer
{
    i32 low = 0;
    std::vector<Voxel> voxels;

    [[nodiscard]] Voxel at(i32 y) const noexcept
    {
        const i32 index = y - low;
        if (index < 0)
            return voxels.empty() ? Voxel{} : voxels.front();
        if (index >= static_cast<i32>(voxels.size()))
            return voxels.empty() ? Voxel{} : voxels.back();
        return voxels[static_cast<usize>(index)];
    }
};

// Where to lay ground under a column's new top, and whether it had one. The
// shared core of `fillFlat` and `writeHeights`.
//
// **The top moves; what is under it stays.** A column whose top is below the
// target gains ground from its top up; one above it loses ground from its top
// down; one with no ground at all is filled from the floor. A tunnel under the
// top is below everything written (D162's promise, kept).
//
// `slope` is how much steeper than flat the ground is here, `sqrt(1 + |grad H|^2)`:
// a height's vertical distance divided by it is the distance to the surface,
// which is what the ramp wants (`RampVoxels`).
void moveColumnTop(FieldWriter& writer, const TerrainField& field, i32 x, i32 z, float target, u8 material,
                   const Band& band, double slope)
{
    const double voxel = static_cast<double>(field.settings().voxelSize);
    const double height = std::clamp(static_cast<double>(target), static_cast<double>(field.settings().minHeight),
                                     static_cast<double>(field.settings().maxHeight));
    const std::optional<float> top = field.columnTop(x, z);
    const i32 targetIndex = field.voxelIndex(height);
    // The ramp's reach along the vertical, which the slope stretches.
    const auto reach = static_cast<i32>(std::ceil(static_cast<double>(RampReach) * slope));
    const i32 oldTop = top.has_value() ? field.voxelIndex(static_cast<double>(*top)) : band.low;
    const i32 from = std::max(std::min(oldTop, targetIndex) - reach, band.low);
    const i32 to = std::min(std::max(oldTop, targetIndex) + reach, band.high);
    const bool filling = !top.has_value() || static_cast<double>(*top) < height;
    for (i32 y = top.has_value() ? from : band.low; y <= to; ++y) {
        const Voxel old = writer.get(x, y, z);
        const u8 wanted = quantiseOccupancy(ramp((field.voxelCenter(y) - height) / slope, voxel));
        // **Within the ramp round the new top, the ramp exactly.** Taking the
        // larger or the smaller of the old ramp and the new one put the top in
        // neither place where their slopes differ: a column levelled between
        // two steep neighbours came out a third of a metre low. Outside it,
        // only ever more ground under the top and less above it, so a tunnel
        // deeper than the ramp is left alone.
        u8 occupancy = old.occupancy;
        if (std::abs(y - targetIndex) <= reach)
            occupancy = wanted;
        else if (filling)
            occupancy = std::max(old.occupancy, wanted);
        else
            occupancy = std::min(old.occupancy, wanted);
        if (occupancy == old.occupancy)
            continue;
        writer.set(x, y, z, Voxel{occupancy, occupancy > old.occupancy || old.material == 0 ? material : old.material});
    }
}

// **Chunks a block of heights covers entirely, written as one value.** A
// heightmap laid on empty ground is otherwise a write per voxel from the floor
// up -- a kilometre square at a metre over a 256 m band is a quarter of a
// billion of them. Here a chunk every column of which is empty and fills past
// its top becomes one uniform value, and only the chunks the surface passes
// through are written voxel by voxel.
//
// Answers the first voxel still to be written in every column of the block,
// which is the floor when nothing was covered.
[[nodiscard]] i32 fillCoveredChunks(TerrainField& field, i32 chunkX, i32 chunkZ, std::span<const float> blockHeights,
                                    u8 material, const Band& band, double steepest, EditReport& report)
{
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    // Only on a chunk-aligned floor: the voxels under an unaligned one would
    // need writing one by one anyway, and the saving is in the chunks.
    if (floorMod(band.low, edge) != 0)
        return band.low;
    float lowest = std::numeric_limits<float>::max();
    for (const float height : blockHeights) {
        if (std::isnan(height))
            return band.low;
        lowest = std::min(lowest, height);
    }
    const double voxel = static_cast<double>(field.settings().voxelSize);
    // The highest voxel that is full in every column: its centre deeper than
    // the ramp reaches under the lowest height, however steep the ground.
    const i32 fullBelow =
        field.voxelIndex(static_cast<double>(lowest) - voxel * static_cast<double>(RampReach) * steepest) - 1;
    i32 next = band.low;
    for (i32 chunkY = band.low / edge;; ++chunkY) {
        const i32 top = chunkY * edge + edge - 1;
        if (top > fullBelow || top > band.high)
            break;
        field.setChunk(ChunkKey{chunkX, chunkY, chunkZ},
                       std::make_shared<TerrainChunk>(Voxel{FullOccupancy, material}));
        report.touched += ChunkVolume;
        next = top + 1;
    }
    return next;
}

// The commonest non-zero value, lowest on a tie: what a chunk laid whole under
// columns of several materials is made of. Nobody sees it until they dig.
[[nodiscard]] u8 majority(std::span<const u8> materials) noexcept
{
    std::array<u32, 256> counts{};
    for (const u8 material : materials)
        counts[material] += 1;
    u8 best = 1;
    u32 most = 0;
    for (u32 id = 1; id < 256; ++id) {
        if (counts[id] > most) {
            most = counts[id];
            best = static_cast<u8>(id);
        }
    }
    return best;
}

} // namespace

EditReport fillBall(TerrainField& field, DVec3 center, double radius, u8 material)
{
    if (!(radius > 0.0))
        return {};
    const Box box = boxOf(field, DVec3{center.x - radius, center.y - radius, center.z - radius},
                          DVec3{center.x + radius, center.y + radius, center.z + radius});
    return applyShape(field, box, material,
                      [&](DVec3 p) { return length(p.x - center.x, p.y - center.y, p.z - center.z) - radius; });
}

EditReport fillBlock(TerrainField& field, DVec3 center, core::Vec3 size, u8 material)
{
    const double halfX = static_cast<double>(size.x) * 0.5;
    const double halfY = static_cast<double>(size.y) * 0.5;
    const double halfZ = static_cast<double>(size.z) * 0.5;
    if (!(halfX > 0.0) || !(halfY > 0.0) || !(halfZ > 0.0))
        return {};
    const Box box = boxOf(field, DVec3{center.x - halfX, center.y - halfY, center.z - halfZ},
                          DVec3{center.x + halfX, center.y + halfY, center.z + halfZ});
    return applyShape(field, box, material, [&](DVec3 p) {
        return std::max(
            {std::abs(p.x - center.x) - halfX, std::abs(p.y - center.y) - halfY, std::abs(p.z - center.z) - halfZ});
    });
}

EditReport fillCylinder(TerrainField& field, DVec3 center, double height, double radius, u8 material)
{
    const double half = height * 0.5;
    if (!(radius > 0.0) || !(half > 0.0))
        return {};
    const Box box = boxOf(field, DVec3{center.x - radius, center.y - half, center.z - radius},
                          DVec3{center.x + radius, center.y + half, center.z + radius});
    return applyShape(field, box, material, [&](DVec3 p) {
        const double radial = std::sqrt((p.x - center.x) * (p.x - center.x) + (p.z - center.z) * (p.z - center.z));
        return std::max(radial - radius, std::abs(p.y - center.y) - half);
    });
}

EditReport fillFlat(TerrainField& field, DVec3 center, float size, float height, u8 material)
{
    if (!(size > 0.0f) || material == 0 || std::isnan(height))
        return {};
    const double half = static_cast<double>(size) * 0.5;
    const i32 firstX = field.voxelIndex(center.x - half);
    const i32 firstZ = field.voxelIndex(center.z - half);
    const i32 lastX = field.voxelIndex(center.x + half) - 1;
    const i32 lastZ = field.voxelIndex(center.z + half) - 1;
    if (lastX < firstX || lastZ < firstZ)
        return {};
    // Columns, not voxels, bound this one (audit S10): its columns are laid
    // shared, so five kilometres of ground is cheap -- and a size a world
    // does not have is not.
    if ((static_cast<double>(lastX) - firstX + 1.0) * (static_cast<double>(lastZ) - firstZ + 1.0) >
        static_cast<double>(MaxEditVoxels)) {
        EditReport refused;
        refused.refused = true;
        return refused;
    }
    // **Chunk column by chunk column, and every one that is whole and empty is
    // the SAME column.** Flat ground over empty columns comes out identical in
    // each (the slab's base and the slope are the same everywhere), so it is
    // laid once and shared, as a snapshot shares it: the first edit to a chunk
    // clones that chunk alone. Written column by column, 5 km of ground was
    // thirty seconds and a quarter of a gigabyte before anybody touched it --
    // the owner's "it froze". Every other column (at the square's edge, or
    // holding ground already) goes through `writeHeights` on its own block,
    // which for flat ground is the same result as one call over the square.
    // Counted wide: 5 km of ground is more voxels than a `u32` holds, and the
    // report says so by saturating rather than wrapping.
    core::u64 touched = 0;
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    std::vector<TerrainField::Entry> column;
    const auto laidColumn = [&]() -> const std::vector<TerrainField::Entry>& {
        if (column.empty()) {
            TerrainField scratch(field.settings());
            const std::vector<float> block(ChunkRows, height);
            (void)writeHeights(scratch, 0, 0, ChunkEdge, block, material);
            column.assign(scratch.chunks().begin(), scratch.chunks().end());
        }
        return column;
    };
    TerrainField shared(field.settings());
    std::vector<float> block;
    for (i32 chunkX = floorDiv(firstX, edge); chunkX <= floorDiv(lastX, edge); ++chunkX) {
        for (i32 chunkZ = floorDiv(firstZ, edge); chunkZ <= floorDiv(lastZ, edge); ++chunkZ) {
            const i32 lowX = std::max(firstX, chunkX * edge);
            const i32 highX = std::min(lastX, chunkX * edge + edge - 1);
            const i32 lowZ = std::max(firstZ, chunkZ * edge);
            const i32 highZ = std::min(lastZ, chunkZ * edge + edge - 1);
            const bool whole = highX - lowX + 1 == edge && highZ - lowZ + 1 == edge;
            if (whole && field.column(chunkX, chunkZ).empty()) {
                // In key order (x, then z, then y), so each lands at the end.
                for (const TerrainField::Entry& laid : laidColumn()) {
                    shared.setChunk(ChunkKey{chunkX, laid.first.y, chunkZ}, laid.second);
                    touched += ChunkVolume;
                }
                continue;
            }
            const auto columns = static_cast<u32>(highX - lowX + 1);
            block.assign(static_cast<usize>(columns) * static_cast<usize>(highZ - lowZ + 1), height);
            touched += writeHeights(field, lowX, lowZ, columns, block, material).touched;
        }
    }
    field.shareFrom(shared);
    return EditReport{static_cast<u32>(std::min<core::u64>(touched, std::numeric_limits<u32>::max()))};
}

namespace {

EditReport writeHeightsWithin(TerrainField& field, i32 firstX, i32 firstZ, u32 columns, std::span<const float> heights,
                              std::span<const u8> materials, const HeightWindow* window);

} // namespace

EditReport writeHeights(TerrainField& field, i32 firstX, i32 firstZ, u32 columns, std::span<const float> heights,
                        u8 material)
{
    if (material == 0)
        return {};
    const u8 one[1] = {material};
    return writeHeightsWithin(field, firstX, firstZ, columns, heights, std::span<const u8>(one), nullptr);
}

EditReport writeHeights(TerrainField& field, i32 firstX, i32 firstZ, u32 columns, std::span<const float> heights,
                        std::span<const u8> materials)
{
    return writeHeightsWithin(field, firstX, firstZ, columns, heights, materials, nullptr);
}

EditReport writeHeights(TerrainField& field, i32 firstX, i32 firstZ, u32 columns, std::span<const float> heights,
                        u8 material, HeightWindow window)
{
    if (material == 0)
        return {};
    const u8 one[1] = {material};
    return writeHeightsWithin(field, firstX, firstZ, columns, heights, std::span<const u8>(one), &window);
}

namespace {

EditReport writeHeightsWithin(TerrainField& field, i32 firstX, i32 firstZ, u32 columns, std::span<const float> heights,
                              std::span<const u8> materials, const HeightWindow* window)
{
    EditReport report;
    if (columns == 0 || heights.empty() || materials.empty())
        return report;
    const auto rows = static_cast<u32>(heights.size() / columns);
    if (rows == 0)
        return report;
    // A window past its table is no window: nothing of it is laid.
    if (window != nullptr && (window->columns == 0 || window->rows == 0 || window->x + window->columns > columns ||
                              window->z + window->rows > rows))
        return report;
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    // **Empty columns are laid as a slab** (`LaidDepth`) under the lowest height
    // of the whole table, so a heightmap's valleys and its peaks stand on one
    // bottom. Everything below reads the band from there up.
    Band band = bandOf(field);
    {
        double lowest = std::numeric_limits<double>::max();
        for (usize at = 0; at < heights.size(); ++at) {
            if (!std::isnan(heights[at]) && (materials.size() == 1 || (at < materials.size() && materials[at] != 0)))
                lowest = std::min(lowest, static_cast<double>(heights[at]));
        }
        if (lowest == std::numeric_limits<double>::max())
            return report;
        lowest = std::clamp(lowest, static_cast<double>(field.settings().minHeight),
                            static_cast<double>(field.settings().maxHeight));
        band.low = laidBase(field, lowest, band);
    }

    // Chunk column by chunk column, so the covered-chunk shortcut can see a
    // whole block of heights at once.
    const i32 lastX = firstX + static_cast<i32>(columns) - 1;
    const i32 lastZ = firstZ + static_cast<i32>(rows) - 1;
    // What is laid: the whole table, or the window of it (`HeightWindow`).
    const i32 laidFirstX = firstX + (window != nullptr ? static_cast<i32>(window->x) : 0);
    const i32 laidFirstZ = firstZ + (window != nullptr ? static_cast<i32>(window->z) : 0);
    const i32 laidLastX = window != nullptr ? laidFirstX + static_cast<i32>(window->columns) - 1 : lastX;
    const i32 laidLastZ = window != nullptr ? laidFirstZ + static_cast<i32>(window->rows) - 1 : lastZ;
    std::vector<float> blockHeights(ChunkRows);
    std::vector<u8> blockMaterials(ChunkRows);
    const auto materialAt = [&](usize index) {
        return materials.size() == 1 ? materials[0] : (index < materials.size() ? materials[index] : u8{0});
    };
    // **How steep the table is at a column**, `sqrt(1 + |grad H|^2)`, from its
    // neighbours in the table -- one-sided at an edge, flat where there is
    // none. A height is a vertical distance; divided by this it is the
    // distance to the surface, which keeps a steep slope's voxels on the ramp
    // rather than clamped into terraces (`RampVoxels`).
    const double voxel = static_cast<double>(field.settings().voxelSize);
    const auto heightOf = [&](i32 x, i32 z) -> double {
        if (x < firstX || x > lastX || z < firstZ || z > lastZ)
            return std::numeric_limits<double>::quiet_NaN();
        const usize index = static_cast<usize>(z - firstZ) * columns + static_cast<usize>(x - firstX);
        if (materialAt(index) == 0 || std::isnan(heights[index]))
            return std::numeric_limits<double>::quiet_NaN();
        // Clamped as the column will be, so a height past the ceiling does not
        // make its neighbours' slope look steeper than the ground they get.
        return std::clamp(static_cast<double>(heights[index]), static_cast<double>(field.settings().minHeight),
                          static_cast<double>(field.settings().maxHeight));
    };
    const auto slopeAt = [&](i32 x, i32 z) {
        const double here = heightOf(x, z);
        const auto along = [&](i32 dx, i32 dz) {
            const double ahead = heightOf(x + dx, z + dz);
            const double behind = heightOf(x - dx, z - dz);
            if (!std::isnan(ahead) && !std::isnan(behind))
                return (ahead - behind) / (2.0 * voxel);
            if (!std::isnan(ahead))
                return (ahead - here) / voxel;
            if (!std::isnan(behind))
                return (here - behind) / voxel;
            return 0.0;
        };
        const double gx = along(1, 0);
        const double gz = along(0, 1);
        const double slope = std::sqrt(1.0 + gx * gx + gz * gz);
        return std::isfinite(slope) ? slope : 1.0;
    };
    // **What it would lay, estimated before a voxel is** (terrain audit
    // TA15): per block of a chunk column, its columns times the rows between
    // its lowest and highest top, the ramp either side at the block's
    // steepest, and a chunk for the rows a base leaves partial. A table that
    // asks for more than `MaxHeightVoxels` is refused whole.
    {
        double estimate = 0.0;
        for (i32 chunkZ = floorDiv(laidFirstZ, edge); chunkZ <= floorDiv(laidLastZ, edge); ++chunkZ) {
            for (i32 chunkX = floorDiv(laidFirstX, edge); chunkX <= floorDiv(laidLastX, edge); ++chunkX) {
                double low = std::numeric_limits<double>::max();
                double high = std::numeric_limits<double>::lowest();
                double steepest = 1.0;
                double count = 0.0;
                for (i32 z = std::max(laidFirstZ, chunkZ * edge); z <= std::min(laidLastZ, chunkZ * edge + edge - 1);
                     ++z) {
                    for (i32 x = std::max(laidFirstX, chunkX * edge);
                         x <= std::min(laidLastX, chunkX * edge + edge - 1); ++x) {
                        const double height = heightOf(x, z);
                        if (std::isnan(height))
                            continue;
                        low = std::min(low, height);
                        high = std::max(high, height);
                        steepest = std::max(steepest, slopeAt(x, z));
                        count += 1.0;
                    }
                }
                if (count == 0.0)
                    continue;
                const double depth = (high - low) / voxel + 2.0 * static_cast<double>(RampReach) * steepest +
                                     static_cast<double>(ChunkEdge);
                estimate += count * depth;
                if (estimate > static_cast<double>(MaxHeightVoxels)) {
                    report.refused = true;
                    report.limit = MaxHeightVoxels;
                    return report;
                }
            }
        }
    }
    for (i32 chunkZ = floorDiv(laidFirstZ, edge); chunkZ <= floorDiv(laidLastZ, edge); ++chunkZ) {
        for (i32 chunkX = floorDiv(laidFirstX, edge); chunkX <= floorDiv(laidLastX, edge); ++chunkX) {
            // The block's heights, NaN where what is laid does not reach.
            const bool columnEmpty = field.column(chunkX, chunkZ).empty();
            for (i32 localZ = 0; localZ < edge; ++localZ) {
                for (i32 localX = 0; localX < edge; ++localX) {
                    const i32 x = chunkX * edge + localX;
                    const i32 z = chunkZ * edge + localZ;
                    const usize slot = static_cast<usize>(localZ) * ChunkEdge + static_cast<usize>(localX);
                    if (x < laidFirstX || x > laidLastX || z < laidFirstZ || z > laidLastZ) {
                        blockHeights[slot] = std::numeric_limits<float>::quiet_NaN();
                        continue;
                    }
                    const usize index = static_cast<usize>(z - firstZ) * columns + static_cast<usize>(x - firstX);
                    blockMaterials[slot] = materialAt(index);
                    // A column with no material is not ground: not written.
                    blockHeights[slot] =
                        blockMaterials[slot] == 0 ? std::numeric_limits<float>::quiet_NaN() : heights[index];
                }
            }
            // **A block of empty ground is laid by chunks first**, where it can
            // be: see `fillCoveredChunks`.
            double steepest = 1.0;
            if (columnEmpty) {
                for (i32 localZ = 0; localZ < edge; ++localZ) {
                    for (i32 localX = 0; localX < edge; ++localX)
                        steepest = std::max(steepest, slopeAt(chunkX * edge + localX, chunkZ * edge + localZ));
                }
            }
            const i32 start = columnEmpty ? fillCoveredChunks(field, chunkX, chunkZ, blockHeights,
                                                              majority(blockMaterials), band, steepest, report)
                                          : band.low;

            FieldWriter writer(field);
            const bool shortcut = columnEmpty;
            for (i32 localZ = 0; localZ < edge; ++localZ) {
                for (i32 localX = 0; localX < edge; ++localX) {
                    const usize slot = static_cast<usize>(localZ) * ChunkEdge + static_cast<usize>(localX);
                    const float height = blockHeights[slot];
                    if (std::isnan(height))
                        continue;
                    const u8 material = blockMaterials[slot];
                    const i32 x = chunkX * edge + localX;
                    const i32 z = chunkZ * edge + localZ;
                    const double slope = slopeAt(x, z);
                    if (shortcut) {
                        // An empty column of chunks: nothing to move, only to
                        // lay, from where the covered chunks stop.
                        const double clamped =
                            std::clamp(static_cast<double>(height), static_cast<double>(field.settings().minHeight),
                                       static_cast<double>(field.settings().maxHeight));
                        const auto reach = static_cast<i32>(std::ceil(static_cast<double>(RampReach) * slope));
                        const i32 to = std::min(field.voxelIndex(clamped) + reach, band.high);
                        for (i32 y = start; y <= to; ++y) {
                            const u8 occupancy =
                                quantiseOccupancy(ramp((field.voxelCenter(y) - clamped) / slope, voxel));
                            if (occupancy > writer.get(x, y, z).occupancy)
                                writer.set(x, y, z, Voxel{occupancy, material});
                        }
                        continue;
                    }
                    moveColumnTop(writer, field, x, z, height, material, band, slope);
                }
            }
            writer.finish();
            report.touched += writer.changed();
        }
    }
    return report;
}

} // namespace

EditReport smoothBall(TerrainField& field, DVec3 center, double radius, float strength)
{
    EditReport report;
    // A NaN clamps to itself and turned the blend into a dig (terrain audit B6).
    const float amount = std::clamp(strength, 0.0f, 1.0f);
    if (!(radius > 0.0) || !(amount > 0.0f))
        return report;
    Box box = boxOf(field, DVec3{center.x - radius, center.y - radius, center.z - radius},
                    DVec3{center.x + radius, center.y + radius, center.z + radius});
    if (!box.refused) {
        box.limit = MaxSmoothVoxels;
        box.bound();
    }
    if (box.empty()) {
        report.refused = box.refused;
        report.limit = box.refused && box.limit == MaxSmoothVoxels ? MaxSmoothVoxels : MaxEditVoxels;
        return report;
    }

    // **Each surface smoothed on its own, along the axis it faces** (terrain
    // audit TA4). Where the occupancy crosses one half along a row of voxels
    // is where a surface is; each such crossing moves towards the average of
    // the same surface's crossings in the rows round it, and the ramp round it
    // moves with it, unchanged in shape. A crossing is moved along the axis
    // its surface faces most -- up and down on ground, sideways on a wall.
    //
    // **Why not a blur of the distance, as before**: a blur mixes every surface
    // in reach, and inside a slab thinner than the blur the distance to its
    // top and to its bottom meet in a V, whose blur is air -- a smooth on
    // ground 4 m thick made a hole through it, and each pass on a ball sank
    // the ground round it. Here the top and the bottom of a slab are two
    // surfaces that never mix, a flat one has nothing to move towards, and
    // an average is never lower than the lowest crossing it averages: nothing
    // is dug below the ground a brush smooths towards.
    const double voxel = static_cast<double>(field.settings().voxelSize);
    // A gaussian half the brush wide across the surface, in voxels, **at most
    // four voxels**: past that a stamp reaches a margin as large as the brush,
    // for a softening a few stamps give anyway.
    const double sigma = std::clamp(radius / 2.0, voxel, 4.0 * voxel) / voxel;
    const i32 kernel = static_cast<i32>(std::ceil(2.5 * sigma));
    // How far along its axis a neighbour's crossing may be and still count as
    // the same surface: across the kernel at a slope of one, and the ramp.
    const i32 window = 2 * kernel + RampReach;
    // **What a stamp reads, and no more** (the owner's editing lag,
    // 2026-10-01: a stamp at radius 8 read 800 000 voxels to move a few
    // hundred, 82 ms). Along a row, a crossing in the box looks `window` past
    // itself for its neighbours' and its ramp reaches `RampReach` further;
    // across, it looks `kernel` rows away and `facing` one more. The margin
    // is the larger of the two, not their sum.
    const i32 pad = std::max(window + RampReach + 2, kernel + 1);
    const i32 x0 = box.minX - pad;
    const i32 y0 = box.minY - pad;
    const i32 z0 = box.minZ - pad;
    const std::array<i32, 3> size{box.maxX - box.minX + 1 + 2 * pad, box.maxY - box.minY + 1 + 2 * pad,
                                  box.maxZ - box.minZ + 1 + 2 * pad};
    const std::array<i32, 3> origin{x0, y0, z0};
    const std::array<i32, 3> low{box.minX, box.minY, box.minZ};
    const std::array<i32, 3> high{box.maxX, box.maxY, box.maxZ};
    const std::array<usize, 3> stride{1, static_cast<usize>(size[0]),
                                      static_cast<usize>(size[0]) * static_cast<usize>(size[1])};
    std::vector<Voxel> copy;
    readBox(field, x0, y0, z0, size[0], size[1], size[2], copy);
    // What the box held, to write back only what changed: the box's own
    // voxels, not the margin round it, which is never written.
    const std::array<i32, 3> boxSize{box.maxX - box.minX + 1, box.maxY - box.minY + 1, box.maxZ - box.minZ + 1};
    const auto boxSlot = [&](i32 x, i32 y, i32 z) {
        return (static_cast<usize>(z - box.minZ) * static_cast<usize>(boxSize[1]) + static_cast<usize>(y - box.minY)) *
                   static_cast<usize>(boxSize[0]) +
               static_cast<usize>(x - box.minX);
    };
    std::vector<Voxel> original(static_cast<usize>(boxSize[0]) * static_cast<usize>(boxSize[1]) *
                                static_cast<usize>(boxSize[2]));
    const auto at = [&](std::array<i32, 3> p) {
        return static_cast<usize>(p[0]) * stride[0] + static_cast<usize>(p[1]) * stride[1] +
               static_cast<usize>(p[2]) * stride[2];
    };
    walk(box, [&](i32 x, i32 y, i32 z) { original[boxSlot(x, y, z)] = copy[at({x - x0, y - y0, z - z0})]; });
    const auto occupancyAt = [&](std::array<i32, 3> p) {
        for (int axis = 0; axis < 3; ++axis)
            p[static_cast<usize>(axis)] =
                std::clamp(p[static_cast<usize>(axis)], 0, size[static_cast<usize>(axis)] - 1);
        return static_cast<float>(copy[at(p)].occupancy) / static_cast<float>(FullOccupancy);
    };
    // The axis the surface faces most at a local point, from occupancy's
    // gradient; y on a tie, then x.
    const auto facing = [&](std::array<i32, 3> p) {
        std::array<float, 3> g{};
        for (int axis = 0; axis < 3; ++axis) {
            std::array<i32, 3> ahead = p;
            std::array<i32, 3> behind = p;
            ahead[static_cast<usize>(axis)] += 1;
            behind[static_cast<usize>(axis)] -= 1;
            g[static_cast<usize>(axis)] = std::abs(occupancyAt(ahead) - occupancyAt(behind));
        }
        if (g[1] >= g[0] && g[1] >= g[2])
            return 1;
        return g[0] >= g[2] ? 0 : 2;
    };

    // One occupancy step, as a distance along a row, in voxels: the smallest
    // move a write makes.
    const double step = static_cast<double>(RampVoxels) / static_cast<double>(FullOccupancy);
    // The gaussian across the surface, and its sum without the middle -- what a
    // crossing's exchanges are divided by, so at full strength and full weight
    // it goes to its neighbours' average and never past it.
    const auto kernelWeight = [sigma](i32 du, i32 dv) {
        const double w = std::exp(-static_cast<double>(du * du + dv * dv) / (2.0 * sigma * sigma));
        return w < 1e-4 ? 0.0 : w;
    };
    double kernelTotal = 0.0;
    for (i32 dv = -kernel; dv <= kernel; ++dv) {
        for (i32 du = -kernel; du <= kernel; ++du)
            kernelTotal += du == 0 && dv == 0 ? 0.0 : kernelWeight(du, dv);
    }
    // The same weights as a table, row by row, so the loop over a crossing's
    // neighbours reads them rather than taking an exponential each.
    const i32 kernelWidth = 2 * kernel + 1;
    std::vector<double> kernelTable(static_cast<usize>(kernelWidth) * static_cast<usize>(kernelWidth));
    for (i32 dv = -kernel; dv <= kernel; ++dv) {
        for (i32 du = -kernel; du <= kernel; ++du)
            kernelTable[static_cast<usize>(dv + kernel) * static_cast<usize>(kernelWidth) +
                        static_cast<usize>(du + kernel)] = kernelWeight(du, dv);
    }
    struct Crossing
    {
        double at = 0.0;     // along the row, in voxels from the row's start
        double weight = 0.0; // the brush's there
        core::i8 sense = 0;  // +1 ground below, air above; -1 the other way
        bool owned = false;  // its surface faces this row's axis most
    };
    // Half full or more, as a byte: what `occupancy >= 0.5` reads.
    constexpr u8 HalfFull = 128;
    static_assert(static_cast<float>(HalfFull) / static_cast<float>(FullOccupancy) >= 0.5f &&
                  static_cast<float>(HalfFull - 1) / static_cast<float>(FullOccupancy) < 0.5f);
    std::vector<Crossing> crossings;
    std::vector<usize> firstOf;

    for (const int axis : {1, 0, 2}) {
        const auto a = static_cast<usize>(axis);
        const auto u = static_cast<usize>(axis == 0 ? 1 : 0);
        const auto v = static_cast<usize>(axis == 2 ? 1 : 2);
        const i32 span = size[a];
        // Every row along `axis` over the padded box: its crossings.
        const auto rowIndex = [&](i32 cu, i32 cv) {
            return static_cast<usize>(cv) * static_cast<usize>(size[u]) + static_cast<usize>(cu);
        };
        const usize rows = static_cast<usize>(size[u]) * static_cast<usize>(size[v]);
        // How far a crossing is from the brush's centre, in metres.
        const auto distanceOf = [&](double along, i32 cu, i32 cv) {
            std::array<double, 3> metres{};
            metres[a] = (along + static_cast<double>(origin[a]) + 0.5) * voxel;
            metres[u] = (static_cast<double>(cu + origin[u]) + 0.5) * voxel;
            metres[v] = (static_cast<double>(cv + origin[v]) + 0.5) * voxel;
            return length(metres[0] - center.x, metres[1] - center.y, metres[2] - center.z);
        };
        crossings.clear();
        firstOf.assign(rows + 1, 0);
        // **Only the rows a crossing in the box reads**: those within `kernel`
        // of the box across. The rest of the padded box is there for the
        // length of these rows, and a row outside the band holds no crossing
        // anything asks for.
        const i32 bandLowU = low[u] - origin[u] - kernel;
        const i32 bandHighU = high[u] - origin[u] + kernel;
        const i32 bandLowV = low[v] - origin[v] - kernel;
        const i32 bandHighV = high[v] - origin[v] + kernel;
        // **The band's rows on the pool**, each range's crossings in its own
        // bucket and merged in range order -- the order of the rows -- so the
        // list is the same whichever worker scanned which (R10).
        const i32 bandU = std::clamp(bandHighU, -1, size[u] - 1) - std::clamp(bandLowU, 0, size[u]) + 1;
        const i32 bandV = std::clamp(bandHighV, -1, size[v] - 1) - std::clamp(bandLowV, 0, size[v]) + 1;
        const i32 bandFirstU = std::clamp(bandLowU, 0, size[u]);
        const i32 bandFirstV = std::clamp(bandLowV, 0, size[v]);
        const usize bandRows = bandU > 0 && bandV > 0 ? static_cast<usize>(bandU) * static_cast<usize>(bandV) : 0;
        constexpr usize ScanGrain = 64;
        std::vector<std::vector<Crossing>> buckets(jobs::rangeCount(0, bandRows, ScanGrain));
        std::vector<u32> rowCounts(bandRows, 0);
        const auto scanRows = [&](usize firstRow, usize endRow, u32 range) noexcept {
            std::vector<Crossing>& found = buckets[range];
            for (usize band = firstRow; band < endRow; ++band) {
                const i32 cv = bandFirstV + static_cast<i32>(band / static_cast<usize>(bandU));
                const i32 cu = bandFirstU + static_cast<i32>(band % static_cast<usize>(bandU));
                const usize before = found.size();
                std::array<i32, 3> p{};
                p[u] = cu;
                p[v] = cv;
                // **Along the row by its stride, a byte at a time**: a crossing
                // is where one voxel is half full and the next is not, and the
                // fractions are taken only there. Reading each voxel through a
                // clamped three-axis lookup was most of a stamp's cost.
                p[a] = 0;
                const usize base = at(p);
                const usize stepAlong = stride[a];
                u8 hereByte = copy[base].occupancy;
                for (i32 k = 0; k + 1 < span; ++k) {
                    const u8 nextByte = copy[base + static_cast<usize>(k + 1) * stepAlong].occupancy;
                    const bool solid = hereByte >= HalfFull;
                    if (solid == (nextByte >= HalfFull)) {
                        hereByte = nextByte;
                        continue;
                    }
                    const float here = static_cast<float>(hereByte) / static_cast<float>(FullOccupancy);
                    const float next = static_cast<float>(nextByte) / static_cast<float>(FullOccupancy);
                    hereByte = nextByte;
                    const double t = static_cast<double>((0.5f - here) / (next - here));
                    Crossing crossing;
                    crossing.at = static_cast<double>(k) + t;
                    crossing.sense = solid ? core::i8{1} : core::i8{-1};
                    crossing.weight = static_cast<double>(falloff(distanceOf(crossing.at, cu, cv), radius));
                    p[a] = t < 0.5 ? k : k + 1;
                    crossing.owned = facing(p) == axis;
                    found.push_back(crossing);
                }
                rowCounts[band] = static_cast<u32>(found.size() - before);
            }
        };
        jobs::parallelFor("terrain.smoothScan", jobs::Domain::SimVisible, 0, bandRows, ScanGrain, scanRows);
        for (const std::vector<Crossing>& bucket : buckets)
            crossings.insert(crossings.end(), bucket.begin(), bucket.end());
        // Every row's first crossing, the band's from their counts and the
        // rest empty, in the order `rowIndex` numbers them.
        usize running = 0;
        for (i32 cv = 0; cv < size[v]; ++cv) {
            for (i32 cu = 0; cu < size[u]; ++cu) {
                firstOf[rowIndex(cu, cv)] = running;
                if (cu < bandFirstU || cu >= bandFirstU + bandU || cv < bandFirstV || cv >= bandFirstV + bandV)
                    continue;
                running += rowCounts[static_cast<usize>(cv - bandFirstV) * static_cast<usize>(bandU) +
                                     static_cast<usize>(cu - bandFirstU)];
            }
        }
        firstOf[rows] = crossings.size();

        // Each owned crossing inside the box: where the same surface is in the
        // rows round it, averaged, and how far towards that this stamp moves it.
        //
        // **Row by row on the pool** (the owner's editing lag): a row's moves
        // read only the crossings, which no row writes, and its rewrite
        // touches only its own voxels -- so the rows are independent, and
        // the ground they leave is the same whichever worker ran which (R10).
        const i32 firstU = low[u] - origin[u];
        const i32 firstV = low[v] - origin[v];
        const auto countU = static_cast<usize>(high[u] - low[u] + 1);
        const auto countV = static_cast<usize>(high[v] - low[v] + 1);
        const auto moveRows = [&](usize firstRow, usize endRow, u32) noexcept {
            std::vector<double> moves;
            std::vector<float> row;
            std::vector<Voxel> rowVoxels;
            for (usize line = firstRow; line < endRow; ++line) {
                const i32 cv = firstV + static_cast<i32>(line / countU);
                const i32 cu = firstU + static_cast<i32>(line % countU);
                const usize rowAt = rowIndex(cu, cv);
                const usize begin = firstOf[rowAt];
                const usize end = firstOf[rowAt + 1];
                bool rowMoved = false;
                moves.assign(end - begin, 0.0);
                for (usize c = begin; c < end; ++c) {
                    const Crossing& own = crossings[c];
                    const i32 cell = static_cast<i32>(std::floor(own.at + 0.5));
                    if (!own.owned || cell + origin[a] < low[a] || cell + origin[a] > high[a])
                        continue;
                    const double weightHere = own.weight;
                    if (weightHere <= 0.0)
                        continue;
                    // **In flux form** (terrain audit TA4): each neighbour gives
                    // or takes by the two brush weights' geometric mean, the same
                    // either way, so what one crossing loses a neighbour gains and
                    // the ground's volume is kept -- where a plain average ate a
                    // ball down to a third in twenty passes.
                    double flux = 0.0;
                    double sum = 0.0;
                    double weights = 0.0;
                    for (i32 dv = -kernel; dv <= kernel; ++dv) {
                        for (i32 du = -kernel; du <= kernel; ++du) {
                            const i32 nu = cu + du;
                            const i32 nv = cv + dv;
                            if ((du == 0 && dv == 0) || nu < 0 || nv < 0 || nu >= size[u] || nv >= size[v])
                                continue;
                            const double k =
                                kernelTable[static_cast<usize>(dv + kernel) * static_cast<usize>(kernelWidth) +
                                            static_cast<usize>(du + kernel)];
                            if (k <= 0.0)
                                continue;
                            // The same surface there: the nearest owned crossing
                            // of the same sense within the window.
                            const usize other = rowIndex(nu, nv);
                            double nearest = std::numeric_limits<double>::max();
                            double weightThere = 0.0;
                            for (usize n = firstOf[other]; n < firstOf[other + 1]; ++n) {
                                if (crossings[n].sense != own.sense || !crossings[n].owned)
                                    continue;
                                if (std::abs(crossings[n].at - own.at) < std::abs(nearest - own.at)) {
                                    nearest = crossings[n].at;
                                    weightThere = crossings[n].weight;
                                }
                            }
                            if (nearest == std::numeric_limits<double>::max() ||
                                std::abs(nearest - own.at) > static_cast<double>(window))
                                continue;
                            flux += k * std::sqrt(weightHere * weightThere) * (nearest - own.at);
                            sum += k * nearest;
                            weights += k;
                        }
                    }
                    if (weights <= 0.0)
                        continue;
                    const double target = sum / weights;
                    double move = static_cast<double>(amount) * flux / kernelTotal;
                    const double weight = weightHere * static_cast<double>(amount);
                    // **Never stuck a step short** (why a stroke used to stall):
                    // a surface with somewhere to go moves at least one step.
                    if (std::abs(target - own.at) > 2.0 * step && std::abs(move) < step &&
                        std::abs(target - own.at) * weight > 0.1 * step)
                        move = target < own.at ? -step : step;
                    // **Never past the next surface in its row**: a slab thins
                    // to a voxel, and is never cut through.
                    const double floorAt = c > begin ? crossings[c - 1].at + 1.0 : -std::numeric_limits<double>::max();
                    const double ceilingAt =
                        c + 1 < end ? crossings[c + 1].at - 1.0 : std::numeric_limits<double>::max();
                    const double moved =
                        std::clamp(own.at + move, std::min(floorAt, own.at), std::max(ceilingAt, own.at));
                    moves[c - begin] = moved - own.at;
                    rowMoved = rowMoved || std::abs(moves[c - begin]) > 1e-9;
                }
                if (!rowMoved)
                    continue;

                // The row as it was, then each moved crossing's stretch of it --
                // half-way to its neighbours -- shifted by its move.
                std::array<i32, 3> p{};
                p[u] = cu;
                p[v] = cv;
                row.resize(static_cast<usize>(span));
                rowVoxels.resize(static_cast<usize>(span));
                for (i32 k = 0; k < span; ++k) {
                    p[a] = k;
                    rowVoxels[static_cast<usize>(k)] = copy[at(p)];
                    row[static_cast<usize>(k)] = occupancyOf(copy[at(p)]);
                }
                const auto sample = [&](double s, i32 first, i32 last) {
                    const double clamped = std::clamp(s, static_cast<double>(first), static_cast<double>(last));
                    const auto k = static_cast<i32>(std::floor(clamped));
                    const i32 next = std::min(k + 1, last);
                    const double f = clamped - static_cast<double>(k);
                    return static_cast<float>(static_cast<double>(row[static_cast<usize>(k)]) * (1.0 - f) +
                                              static_cast<double>(row[static_cast<usize>(next)]) * f);
                };
                for (usize c = begin; c < end; ++c) {
                    const double move = moves[c - begin];
                    if (std::abs(move) <= 1e-9)
                        continue;
                    const Crossing& own = crossings[c];
                    const i32 first = c > begin ? static_cast<i32>(std::ceil((crossings[c - 1].at + own.at) / 2.0)) : 0;
                    const i32 last =
                        c + 1 < end ? static_cast<i32>(std::floor((own.at + crossings[c + 1].at) / 2.0)) : span - 1;
                    // The solid side's material, for ground that appears.
                    const i32 solidSide = std::clamp(
                        static_cast<i32>(own.sense > 0 ? std::floor(own.at) : std::ceil(own.at)), 0, span - 1);
                    for (i32 k = first; k <= last; ++k) {
                        const float value = sample(static_cast<double>(k) - move, first, last);
                        const u8 occupancy = quantiseOccupancy(value);
                        p[a] = k;
                        Voxel& target = copy[at(p)];
                        if (occupancy == target.occupancy)
                            continue;
                        const i32 source =
                            std::clamp(static_cast<i32>(std::lround(static_cast<double>(k) - move)), first, last);
                        Voxel next = rowVoxels[static_cast<usize>(source)];
                        if (next.material == 0)
                            next = rowVoxels[static_cast<usize>(solidSide)];
                        next.occupancy = occupancy;
                        target = canonical(next);
                    }
                }
            }
        };
        jobs::parallelFor("terrain.smooth", jobs::Domain::SimVisible, 0, countU * countV, 32, moveRows);
    }

    // What changed in the box, written.
    FieldWriter writer(field);
    walk(box, [&](i32 x, i32 y, i32 z) {
        const usize here = at({x - x0, y - y0, z - z0});
        if (copy[here] == original[boxSlot(x, y, z)])
            return;
        writer.set(x, y, z, copy[here]);
    });
    writer.finish();
    report.touched = writer.changed();
    return report;
}

EditReport flattenBall(TerrainField& field, DVec3 center, double radius, float height, float strength)
{
    EditReport report;
    const float amount = std::clamp(strength, 0.0f, 1.0f);
    if (!(radius > 0.0) || !(amount > 0.0f) || !std::isfinite(height))
        return report;
    const Box box = boxOf(field, DVec3{center.x - radius, center.y - radius, center.z - radius},
                          DVec3{center.x + radius, center.y + radius, center.z + radius});
    if (box.empty()) {
        report.refused = box.refused;
        return report;
    }
    const double voxel = static_cast<double>(field.settings().voxelSize);
    // What ground laid under the plane is made of: whatever is under the brush.
    u8 fill = sampleField(field, DVec3{center.x, static_cast<double>(height) - voxel, center.z}).material;
    if (fill == 0)
        fill = sampleField(field, center).material;
    if (fill == 0)
        fill = 1;

    FieldWriter writer(field);
    walk(box, [&](i32 x, i32 y, i32 z) {
        const double cy = field.voxelCenter(y);
        const double distance = length(field.voxelCenter(x) - center.x, cy - center.y, field.voxelCenter(z) - center.z);
        const float weight = falloff(distance, radius) * amount;
        if (weight <= 0.0f)
            return;
        const float target = ramp(cy - static_cast<double>(height), voxel);
        const Voxel old = writer.get(x, y, z);
        const float before = occupancyOf(old);
        const u8 occupancy = quantiseOccupancy(before + (target - before) * weight);
        if (occupancy == old.occupancy)
            return;
        writer.set(x, y, z, Voxel{occupancy, old.material != 0 ? old.material : fill});
    });
    writer.finish();
    report.touched = writer.changed();
    return report;
}

EditReport raiseBall(TerrainField& field, DVec3 center, double radius, float amount, u8 material)
{
    EditReport report;
    if (!(radius > 0.0) || amount == 0.0f || std::isnan(amount))
        return report;
    // **No further than the band is high** (terrain audit B1): every column
    // reads a shift's worth past its window, and `RaiseBall(p, 1, 1e9)` asked
    // for two billion voxels a column.
    const float bandHeight = field.settings().maxHeight - field.settings().minHeight;
    amount = std::clamp(amount, -bandHeight, bandHeight);
    const double voxel = static_cast<double>(field.settings().voxelSize);
    const double reach = radius + std::abs(static_cast<double>(amount));
    const Band band = bandOf(field);
    const Box box = boxOf(field, DVec3{center.x - radius, center.y - reach, center.z - radius},
                          DVec3{center.x + radius, center.y + reach, center.z + radius});
    if (box.empty()) {
        report.refused = box.refused;
        return report;
    }
    const bool raising = amount > 0.0f;

    FieldWriter writer(field);
    ColumnBuffer column;
    for (i32 z = box.minZ; z <= box.maxZ; ++z) {
        for (i32 x = box.minX; x <= box.maxX; ++x) {
            const double dx = field.voxelCenter(x) - center.x;
            const double dz = field.voxelCenter(z) - center.z;
            const double shift =
                static_cast<double>(amount) * static_cast<double>(falloff(std::sqrt(dx * dx + dz * dz), radius));
            if (std::abs(shift) < 1e-6)
                continue;

            // **The reach follows the surface it is moving.** A brush centred on
            // the ground reaches `radius` above and below it -- but a script
            // raising the same spot again and again would otherwise see its
            // hill stop growing at the reach's ceiling and go flat on top. So
            // when the column is solid all the way from inside the reach up to
            // its top, the window runs up to the top; and when lowering finds
            // the top already under the reach, down to it. A column with air
            // between the brush and its top -- a brush in a cave -- keeps the
            // plain reach, so raising a cave's floor does not lift the hill
            // over it.
            const i32 extra = static_cast<i32>(std::ceil(std::abs(shift) / voxel)) + RampReach;
            i32 lowY = box.minY;
            i32 highY = box.maxY;
            if (const std::optional<float> top = field.columnTop(x, z); top.has_value()) {
                const i32 topIndex = field.voxelIndex(static_cast<double>(*top));
                if (raising && topIndex + extra > highY && topIndex >= box.minY) {
                    bool solid = true;
                    for (i32 y = highY; y < topIndex && solid; ++y)
                        solid = field.voxel(x, y, z).occupancy >= 128;
                    if (solid)
                        highY = std::min(topIndex + extra, band.high);
                }
                if (!raising && topIndex - extra < lowY)
                    lowY = std::max(topIndex - extra, band.low);
            }

            // The column as it was, a shift's worth wider than the window on
            // both ends so the shifted read never runs off it.
            column.low = lowY - extra;
            column.voxels.clear();
            bool anyGround = false;
            for (i32 y = column.low; y <= highY + extra; ++y) {
                const Voxel got = field.voxel(x, y, z);
                anyGround = anyGround || got.occupancy > 0;
                column.voxels.push_back(got);
            }

            // **An empty column is laid from `center`'s height up**, when there
            // is a material to lay: the first stroke on an empty terrain makes
            // ground rather than nothing. As the slab every other laying verb
            // makes (`LaidDepth`), so ground extended sideways from generated
            // ground has the same bottom.
            if (!anyGround) {
                if (!raising || material == 0 || field.columnTop(x, z).has_value())
                    continue;
                const double top = center.y + shift;
                const i32 to = std::min(field.voxelIndex(top) + RampReach, band.high);
                for (i32 y = laidBase(field, center.y, band); y <= to; ++y) {
                    const u8 occupancy = quantiseOccupancy(ramp(field.voxelCenter(y) - top, voxel));
                    if (occupancy > writer.get(x, y, z).occupancy)
                        writer.set(x, y, z, Voxel{occupancy, material});
                }
                continue;
            }

            // The column shifted by `shift`: the occupancy at `y - shift`,
            // interpolated between the two voxel centres around it.
            const double steps = shift / voxel;
            for (i32 y = lowY; y <= highY; ++y) {
                const double source = static_cast<double>(y) - steps;
                const auto below = static_cast<i32>(std::floor(source));
                const auto t = static_cast<float>(source - static_cast<double>(below));
                const Voxel a = column.at(below);
                const Voxel b = column.at(below + 1);
                const float shifted = occupancyOf(a) + (occupancyOf(b) - occupancyOf(a)) * t;
                const u8 occupancy = quantiseOccupancy(shifted);
                const Voxel old = column.at(y);
                if (raising && occupancy > old.occupancy) {
                    // The material of whichever source voxel is fuller: raising
                    // grass keeps grass on top.
                    u8 from = a.occupancy >= b.occupancy ? a.material : b.material;
                    if (from == 0)
                        from = a.material != 0 ? a.material : b.material;
                    if (from == 0)
                        from = old.material != 0 ? old.material : (material != 0 ? material : 1);
                    writer.set(x, y, z, Voxel{occupancy, from});
                }
                else if (!raising && occupancy < old.occupancy) {
                    writer.set(x, y, z, Voxel{occupancy, old.material});
                }
            }
        }
    }
    writer.finish();
    report.touched = writer.changed();
    return report;
}

EditReport growBall(TerrainField& field, DVec3 center, double radius, float amount, u8 material)
{
    EditReport report;
    if (!(radius > 0.0) || amount == 0.0f || !std::isfinite(amount))
        return report;
    // **Held to smoothing's bound** (terrain audit TA15): it copies its box
    // before it writes, as a smooth does, and the edit limit let one stamp
    // copy half a gigabyte.
    Box box = boxOf(field, DVec3{center.x - radius, center.y - radius, center.z - radius},
                    DVec3{center.x + radius, center.y + radius, center.z + radius});
    if (!box.refused) {
        box.limit = MaxSmoothVoxels;
        box.bound();
    }
    if (box.empty()) {
        report.refused = box.refused;
        report.limit = box.refused && box.limit == MaxSmoothVoxels ? MaxSmoothVoxels : MaxEditVoxels;
        return report;
    }
    const double voxel = static_cast<double>(field.settings().voxelSize);
    const bool growing = amount > 0.0f;
    // **Capped at the ramp's outer half.** Past it the air holds no occupancy
    // to grow from, so a bigger step would only stop at the same place.
    const double step = std::min(std::abs(static_cast<double>(amount)), voxel * static_cast<double>(RampVoxels) * 0.5);

    // Read once, a voxel wider than the box, so every voxel sees its six
    // neighbours as they were before the stamp.
    const i32 x0 = box.minX - 1;
    const i32 y0 = box.minY - 1;
    const i32 z0 = box.minZ - 1;
    const i32 sizeX = box.maxX - box.minX + 3;
    const i32 sizeY = box.maxY - box.minY + 3;
    const i32 sizeZ = box.maxZ - box.minZ + 3;
    const auto index = [&](i32 x, i32 y, i32 z) {
        return (static_cast<usize>(z - z0) * static_cast<usize>(sizeY) + static_cast<usize>(y - y0)) *
                   static_cast<usize>(sizeX) +
               static_cast<usize>(x - x0);
    };
    std::vector<Voxel> copy(static_cast<usize>(sizeX) * static_cast<usize>(sizeY) * static_cast<usize>(sizeZ));
    for (i32 z = z0; z < z0 + sizeZ; ++z) {
        for (i32 y = y0; y < y0 + sizeY; ++y) {
            for (i32 x = x0; x < x0 + sizeX; ++x)
                copy[index(x, y, z)] = field.voxel(x, y, z);
        }
    }

    static constexpr std::array<std::array<i32, 3>, 6> Faces{{
        {-1, 0, 0},
        {1, 0, 0},
        {0, -1, 0},
        {0, 1, 0},
        {0, 0, -1},
        {0, 0, 1},
    }};
    // One voxel's worth of ramp: how much a voxel's occupancy may differ from
    // its neighbour's where the surface is square to the axis between them.
    const float slope = 1.0f / RampVoxels;

    FieldWriter writer(field);
    walk(box, [&](i32 x, i32 y, i32 z) {
        const double distance =
            length(field.voxelCenter(x) - center.x, field.voxelCenter(y) - center.y, field.voxelCenter(z) - center.z);
        const float weight = falloff(distance, radius);
        if (weight <= 0.0f)
            return;
        // **The surface moves along its own normal, by `step` at the centre.**
        // Inside the ramp, occupancy is distance, so adding `step / (4 v)` moves
        // the half crossing out by `step` whichever way the ground faces -- up
        // on a field, sideways on a cliff, down under an overhang. A voxel is
        // first raised to its fullest neighbour less one voxel of ramp, so air
        // next to ground has something to grow from and ground next to air
        // something to wear from; in a ramp already whole, that changes nothing.
        const Voxel old = copy[index(x, y, z)];
        const float before = occupancyOf(old);
        const float delta = static_cast<float>(step / (voxel * static_cast<double>(RampVoxels))) * weight;
        float base = before;
        Voxel fullest{};
        for (const std::array<i32, 3>& face : Faces) {
            const Voxel near = copy[index(x + face[0], y + face[1], z + face[2])];
            if (growing) {
                base = std::max(base, occupancyOf(near) - slope);
                if (near.occupancy > fullest.occupancy)
                    fullest = near;
            }
            else {
                base = std::min(base, occupancyOf(near) + slope);
            }
        }
        if (growing) {
            // Nothing to grow from: air with no ground within a voxel of it.
            if (base <= 0.0f)
                return;
            const u8 occupancy = quantiseOccupancy(base + delta);
            if (occupancy <= old.occupancy)
                return;
            // New ground is made of what it grew from: growing grass grows
            // grass.
            u8 made = old.material != 0 ? old.material : fullest.material;
            if (made == 0)
                made = material != 0 ? material : 1;
            writer.set(x, y, z, Voxel{occupancy, made});
        }
        else {
            if (base >= 1.0f)
                return;
            const u8 occupancy = quantiseOccupancy(base - delta);
            if (occupancy >= old.occupancy)
                return;
            writer.set(x, y, z, Voxel{occupancy, old.material});
        }
    });
    writer.finish();
    report.touched = writer.changed();
    return report;
}

namespace {

// **One voxel painted** (ADR 0114): what `mode` makes of it, `weight` of the
// way -- 0 to 1, the stroke's strength less its falloff there.
// `edge` is how far inside the brush's rim the voxel is, from 1 half a voxel
// in to 0 half a voxel out (`paintBall`): what of the paint may show there.
[[nodiscard]] Voxel paintedVoxel(Voxel old, u8 material, PaintMode mode, float weight, float edge) noexcept
{
    const float amount = std::clamp(weight, 0.0f, 1.0f);
    // The most that may show here, and the least of what was over it.
    const auto cap = static_cast<i32>(std::lround(255.0f * std::clamp(edge, 0.0f, 1.0f)));
    // **Towards the end by a share of what is left** (D328): a stamp takes
    // `weight` of the way from where the cover is to all or none, as an
    // opacity brush does, and never less than one step. The stamps of a stroke
    // summed, so a soft brush's fading edge reached whole cover a stamp or two
    // behind its middle and the edge came out hard; a share of what is left
    // keeps the brush's own profile across its edge however many stamps pass.
    const auto towards = [amount](i32 from, i32 to) {
        const i32 gap = to - from;
        if (gap == 0 || amount <= 0.0f)
            return from;
        const auto step = static_cast<i32>(std::lround(static_cast<float>(gap) * amount));
        return from + (step != 0 ? step : (gap > 0 ? 1 : -1));
    };
    Voxel voxel = old;
    // `Replace` on the rim is the paint laid over what is there, as far as the
    // rim lets it show: the whole strength, and the blend below.
    const bool wholly = mode == PaintMode::Replace && cap < 255;
    switch (mode) {
    case PaintMode::Replace:
        if (cap >= 255)
            return Voxel{old.occupancy, material, 0, 0};
        break;
    case PaintMode::Under:
        // What is under has no edge to soften: the half of the rim inside it.
        if (cap < 128)
            return old;
        voxel.material = material;
        return canonical(voxel);
    case PaintMode::Erase: {
        // **All of it inside the radius**, as an eraser always took, and the
        // ramp in the half voxel outside: what a brush painted to its rim,
        // the same brush takes off whole.
        const auto off = static_cast<i32>(std::lround(255.0f * std::clamp(2.0f * edge, 0.0f, 1.0f)));
        const i32 least = std::min<i32>(old.cover, 255 - off);
        voxel.cover = static_cast<u8>(std::max(towards(old.cover, 0), least));
        return canonical(voxel);
    }
    case PaintMode::Blend:
        break;
    }
    if ((amount <= 0.0f && !wholly) || cap <= 0)
        return old;
    const auto reach = [&](i32 from, i32 to) { return wholly ? to : towards(from, to); };
    // The material under painted over itself: what is over it shows less.
    if (old.material == material) {
        const i32 least = std::min<i32>(old.cover, 255 - cap);
        voxel.cover = static_cast<u8>(std::max(reach(old.cover, 0), least));
        return canonical(voxel);
    }
    // The same over it again, or nothing over it yet: it shows more -- up to
    // what the rim lets show, and never less than it showed.
    if (old.cover == 0 || old.top == material) {
        const i32 from = old.top == material ? static_cast<i32>(old.cover) : 0;
        if (from >= cap)
            return old;
        const i32 cover = reach(from, cap);
        // Covered wholly, it is simply what the ground is made of.
        if (cover >= 255)
            return Voxel{old.occupancy, material, 0, 0};
        return canonical(Voxel{old.occupancy, old.material, material, static_cast<u8>(cover)});
    }
    // **A third over two**: the one that shows more goes under first, and
    // the new one starts over it.
    const u8 under = old.cover >= 128 ? old.top : old.material;
    const i32 cover = reach(0, cap);
    if (cover >= 255)
        return Voxel{old.occupancy, material, 0, 0};
    return canonical(Voxel{old.occupancy, under, material, static_cast<u8>(cover)});
}

} // namespace

EditReport paintBall(TerrainField& field, DVec3 center, double radius, u8 material, PaintOptions options)
{
    EditReport report;
    if (!(radius > 0.0) || (material == 0 && options.mode != PaintMode::Erase))
        return report;
    const float strength = std::clamp(options.strength, 0.0f, 1.0f);
    const float soft = std::clamp(options.falloff, 0.0f, 1.0f);
    // A NaN in either is no stroke at all (terrain audit B6's rule).
    if (!(strength == strength) || !(soft == soft))
        return report;
    const Box box = boxOf(field, DVec3{center.x - radius, center.y - radius, center.z - radius},
                          DVec3{center.x + radius, center.y + radius, center.z + radius});
    if (box.empty()) {
        report.refused = box.refused;
        return report;
    }
    FieldWriter writer(field);
    // **The ball's rows, not its box's** (the owner's editing lag): a row is
    // walked only across the chord the ball cuts in it, a voxel wider each
    // side, so the corners of the box -- half of it -- are never read. The
    // distance test below still decides each voxel.
    const double voxelSize = static_cast<double>(field.settings().voxelSize);
    const auto visit = [&](i32 x, i32 y, i32 z) {
        const double distance =
            length(field.voxelCenter(x) - center.x, field.voxelCenter(y) - center.y, field.voxelCenter(z) - center.z);
        // **The rim is a ramp a voxel wide, not a step** (the owner's picture:
        // a hard round brush left a polygon with triangular teeth). A voxel
        // was painted or it was not, by which side of the radius its middle
        // fell, and the mesh drew the staircase that makes. How far inside the
        // rim a voxel is -- whole half a voxel in, none half a voxel out -- is
        // how much of the paint may show there, so the half-way line of what
        // is drawn is the circle itself, to a fraction of a voxel.
        const double edge = std::clamp(0.5 + (radius - distance) / voxelSize, 0.0, 1.0);
        if (!(edge > 0.0))
            return;
        const Voxel old = writer.get(x, y, z);
        if (old.occupancy == 0 || !options.mask.allows(old.material))
            return;
        const PaintMask& mask = options.mask;
        if (mask.byHeight) {
            const double height = field.voxelCenter(y);
            if (height < static_cast<double>(mask.heightMin) || height > static_cast<double>(mask.heightMax))
                return;
        }
        if (mask.bySlope) {
            // The ground's slope at the voxel, from its occupancy's gradient:
            // level inside the ground, where there is none, and nothing there
            // shows anyway.
            const auto occupancyAt = [&](i32 ox, i32 oy, i32 oz) {
                return static_cast<double>(writer.get(ox, oy, oz).occupancy);
            };
            const double gx = occupancyAt(x - 1, y, z) - occupancyAt(x + 1, y, z);
            const double gy = occupancyAt(x, y - 1, z) - occupancyAt(x, y + 1, z);
            const double gz = occupancyAt(x, y, z - 1) - occupancyAt(x, y, z + 1);
            const double size = std::sqrt(gx * gx + gy * gy + gz * gz);
            const double degrees =
                size < 1e-6 ? 0.0 : core::dmath::acos(std::clamp(gy / size, -1.0, 1.0)) * 180.0 / std::numbers::pi;
            if (degrees < static_cast<double>(mask.slopeMin) || degrees > static_cast<double>(mask.slopeMax))
                return;
        }
        // **Softness is how much of the radius fades** (D328): the inner
        // `1 - soft` of it is the whole strength, and the rest falls away
        // smoothly to nothing at the rim -- hard is the strength to the rim,
        // wholly soft falls from the centre. It was a mix of the two, so at
        // one half the rim still took half the strength, and the stamps of a
        // stroke summed there to a hard edge.
        const double core = radius * (1.0 - static_cast<double>(soft));
        // The ramp's outer half is past the radius: a hard brush is as strong
        // there as at its rim, and the ramp is what holds it back.
        const double within = std::min(distance, radius);
        const float weight =
            within <= core || !(radius > core) ? strength : strength * falloff(within - core, radius - core);
        const Voxel painted = paintedVoxel(old, material, options.mode, weight, static_cast<float>(edge));
        if (!(painted == old))
            writer.setExact(x, y, z, painted);
    };
    for (i32 z = box.minZ; z <= box.maxZ; ++z) {
        const double dz = field.voxelCenter(z) - center.z;
        for (i32 y = box.minY; y <= box.maxY; ++y) {
            const double dy = field.voxelCenter(y) - center.y;
            // Half a voxel past the radius, where the rim's ramp ends.
            const double reach = radius + 0.5 * voxelSize;
            const double rest = reach * reach - dy * dy - dz * dz;
            if (rest < -voxelSize * voxelSize)
                continue;
            const double half = std::sqrt(std::max(rest, 0.0)) + voxelSize;
            const i32 first = std::max(box.minX, field.voxelIndex(center.x - half));
            const i32 last = std::min(box.maxX, field.voxelIndex(center.x + half));
            for (i32 x = first; x <= last; ++x)
                visit(x, y, z);
        }
    }
    writer.finish();
    report.touched = writer.changed();
    return report;
}

namespace {

// An integer hash of a lattice point and a seed: the hills' noise, the same
// bits everywhere (R10).
[[nodiscard]] core::u32 latticeHash(i32 x, i32 z, core::u32 seed) noexcept
{
    core::u32 h =
        static_cast<core::u32>(x) * 0x8DA6B343u ^ static_cast<core::u32>(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

// Value noise at a point in lattice units, 0 to 1, smoothly between corners.
[[nodiscard]] double valueNoise(double px, double pz, core::u32 seed) noexcept
{
    const double cx = std::floor(px);
    const double cz = std::floor(pz);
    const double fx = px - cx;
    const double fz = pz - cz;
    const double ux = fx * fx * (3.0 - 2.0 * fx);
    const double uz = fz * fz * (3.0 - 2.0 * fz);
    const auto ix = static_cast<i32>(cx);
    const auto iz = static_cast<i32>(cz);
    const auto unit = [seed](i32 x, i32 z) {
        return static_cast<double>(latticeHash(x, z, seed) & 0xFFFFFFu) / 16777215.0;
    };
    const double near = unit(ix, iz) + (unit(ix + 1, iz) - unit(ix, iz)) * ux;
    const double far = unit(ix, iz + 1) + (unit(ix + 1, iz + 1) - unit(ix, iz + 1)) * ux;
    return near + (far - near) * uz;
}

} // namespace

std::vector<float> hillHeights(const TerrainField& field, i32 firstX, i32 firstZ, core::u32 columns, core::u32 rows,
                               const HillSettings& settings)
{
    std::vector<float> heights(static_cast<usize>(columns) * rows);
    const double voxel = static_cast<double>(field.settings().voxelSize);
    const core::u32 octaves = std::clamp<core::u32>(settings.octaves, 1, 10);
    const double scale = std::max(static_cast<double>(settings.scale), voxel);
    // The largest amplitude first; the sum of them all is what 1 is.
    double total = 0.0;
    for (core::u32 octave = 0; octave < octaves; ++octave)
        total += std::ldexp(1.0, -static_cast<int>(octave));
    for (core::u32 row = 0; row < rows; ++row) {
        for (core::u32 column = 0; column < columns; ++column) {
            const double x = field.voxelCenter(firstX + static_cast<i32>(column)) / scale;
            const double z = field.voxelCenter(firstZ + static_cast<i32>(row)) / scale;
            double sum = 0.0;
            for (core::u32 octave = 0; octave < octaves; ++octave) {
                const double frequency = std::ldexp(1.0, static_cast<int>(octave));
                sum += valueNoise(x * frequency, z * frequency, settings.seed + octave * 7919u) *
                       std::ldexp(1.0, -static_cast<int>(octave));
            }
            const double unit = sum / total;
            heights[static_cast<usize>(row) * columns + column] = static_cast<float>(
                static_cast<double>(settings.low) + unit * static_cast<double>(settings.high - settings.low));
        }
    }
    return heights;
}

std::vector<float> columnHeights(const TerrainField& field, i32 firstX, i32 firstZ, core::u32 columns, core::u32 rows,
                                 float empty)
{
    std::vector<float> heights(static_cast<usize>(columns) * rows, empty);
    for (core::u32 row = 0; row < rows; ++row) {
        for (core::u32 column = 0; column < columns; ++column) {
            if (const std::optional<float> top =
                    field.columnTop(firstX + static_cast<i32>(column), firstZ + static_cast<i32>(row));
                top.has_value())
                heights[static_cast<usize>(row) * columns + column] = *top;
        }
    }
    return heights;
}

EditReport replaceMaterial(TerrainField& field, DVec3 minCorner, DVec3 maxCorner, u8 from, u8 to)
{
    EditReport report;
    if (from == 0 || to == 0 || from == to)
        return report;
    Box box;
    box.minX = field.voxelIndex(std::min(minCorner.x, maxCorner.x));
    box.minY = field.voxelIndex(std::min(minCorner.y, maxCorner.y));
    box.minZ = field.voxelIndex(std::min(minCorner.z, maxCorner.z));
    box.maxX = field.voxelIndex(std::max(minCorner.x, maxCorner.x));
    box.maxY = field.voxelIndex(std::max(minCorner.y, maxCorner.y));
    box.maxZ = field.voxelIndex(std::max(minCorner.z, maxCorner.z));
    box.bound();
    if (box.empty()) {
        report.refused = box.refused;
        return report;
    }
    FieldWriter writer(field);
    walk(box, [&](i32 x, i32 y, i32 z) {
        const Voxel old = writer.get(x, y, z);
        if (old.occupancy > 0 && old.material == from)
            writer.set(x, y, z, Voxel{old.occupancy, to});
    });
    writer.finish();
    report.touched = writer.changed();
    return report;
}

std::optional<float> heightAt(const TerrainField& field, double x, double z)
{
    const double voxel = static_cast<double>(field.settings().voxelSize);
    if (!(voxel > 0.0))
        return std::nullopt;
    // Bilinear between the four column centres around the point. The column the
    // point is IN decides whether there is ground here at all; a neighbour with
    // none stands in with its value, so the edge of a plateau does not sag.
    const std::optional<float> own = field.columnTop(field.voxelIndex(x), field.voxelIndex(z));
    if (!own.has_value())
        return std::nullopt;
    const double gridX = x / voxel - 0.5;
    const double gridZ = z / voxel - 0.5;
    const auto lowX = static_cast<i32>(std::floor(gridX));
    const auto lowZ = static_cast<i32>(std::floor(gridZ));
    const auto tx = static_cast<float>(gridX - std::floor(gridX));
    const auto tz = static_cast<float>(gridZ - std::floor(gridZ));
    const auto top = [&](i32 cx, i32 cz) { return field.columnTop(cx, cz).value_or(*own); };
    const float a = top(lowX, lowZ);
    const float b = top(lowX + 1, lowZ);
    const float c = top(lowX, lowZ + 1);
    const float d = top(lowX + 1, lowZ + 1);
    const float near = a + (b - a) * tx;
    const float far = c + (d - c) * tx;
    return near + (far - near) * tz;
}

} // namespace engine::asset
