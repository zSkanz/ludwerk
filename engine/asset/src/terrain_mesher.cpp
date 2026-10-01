#include "engine/asset/terrain_mesher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::asset {
namespace {

// **A corner whose own ground is what is painted over the others counts as
// covered wholly by it** (D396). Paint blended until it is all that shows
// becomes the ground, with nothing over it; averaged as a cover of nothing
// beside corners still painted, it halved the cover across the cell and the
// ground under the paint showed through along the line between them. Only a
// material painted somewhere in the cell: an unpainted corner of the ground
// under a stroke still thins the stroke at its edge.
void coverWithOwnGround(const std::array<core::u8, 8>& bare, const std::array<core::u8, 8>& overs,
                        std::array<int, 8>& covers, int kinds) noexcept
{
    for (const core::u8 own : bare) {
        if (own == 0)
            continue;
        for (int slot = 0; slot < kinds; ++slot) {
            if (overs[static_cast<core::usize>(slot)] == own)
                covers[static_cast<core::usize>(slot)] += 255;
        }
    }
}

using core::i32;
using core::u16;
using core::u32;
using core::u8;
using core::usize;
using core::Vec3;

// The eight corners of a lattice cell: bit 0 is x, bit 1 is y, bit 2 is z.
constexpr std::array<std::array<i32, 3>, 8> CornerOffsets{{
    {0, 0, 0},
    {1, 0, 0},
    {0, 1, 0},
    {1, 1, 0},
    {0, 0, 1},
    {1, 0, 1},
    {0, 1, 1},
    {1, 1, 1},
}};

// The twelve edges of a cell, as corner pairs.
constexpr std::array<std::array<int, 2>, 12> CellEdges{{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7}, // along x
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7}, // along y
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7}, // along z
}};

// The edge of a cell that runs along `axis` at `(a, b)` on its other two axes,
// lower axis first: which of `CellEdges` it is.
constexpr std::array<std::array<std::array<u8, 2>, 2>, 3> EdgeAt = [] {
    std::array<std::array<std::array<u8, 2>, 2>, 3> table{};
    for (usize edge = 0; edge < CellEdges.size(); ++edge) {
        const auto& from = CornerOffsets[static_cast<usize>(CellEdges[edge][0])];
        const auto& to = CornerOffsets[static_cast<usize>(CellEdges[edge][1])];
        for (usize axis = 0; axis < 3; ++axis) {
            if (from[axis] == to[axis])
                continue;
            const usize first = axis == 0 ? 1 : 0;
            const usize second = axis == 2 ? 1 : 2;
            table[axis][static_cast<usize>(from[first])][static_cast<usize>(from[second])] = static_cast<u8>(edge);
        }
    }
    return table;
}();

// The six faces of a cell: their four corners in order round the face, and
// the edge from each corner to the next.
struct CellFace
{
    std::array<u8, 4> corners{};
    std::array<u8, 4> edges{};
};
constexpr std::array<CellFace, 6> CellFaces = [] {
    std::array<CellFace, 6> faces{};
    constexpr std::array<std::array<int, 2>, 4> Round{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (usize axis = 0; axis < 3; ++axis) {
        const usize first = axis == 0 ? 1 : 0;
        const usize second = axis == 2 ? 1 : 2;
        for (int side = 0; side < 2; ++side) {
            CellFace& face = faces[axis * 2 + static_cast<usize>(side)];
            for (usize at = 0; at < 4; ++at) {
                for (usize corner = 0; corner < CornerOffsets.size(); ++corner) {
                    const auto& offset = CornerOffsets[corner];
                    if (offset[axis] == side && offset[first] == Round[at][0] && offset[second] == Round[at][1])
                        face.corners[at] = static_cast<u8>(corner);
                }
            }
            for (usize at = 0; at < 4; ++at) {
                const int a = face.corners[at];
                const int b = face.corners[(at + 1) % 4];
                for (usize edge = 0; edge < CellEdges.size(); ++edge) {
                    if ((CellEdges[edge][0] == a && CellEdges[edge][1] == b) ||
                        (CellEdges[edge][0] == b && CellEdges[edge][1] == a))
                        face.edges[at] = static_cast<u8>(edge);
                }
            }
        }
    }
    return faces;
}();

// **The sheets of surface a cell holds** (terrain-editing ledger, P3): which
// of its crossed edges belong together, as a component number per edge, and
// how many there are.
//
// A cell with one vertex joins every sheet that passes through it: where two
// meet in a cell -- a wall a voxel thin, two hollows a corner apart -- their
// quads shared that one vertex and the edge beside it, four faces to an edge,
// and the surface crossed itself there (the owner's "uma malha se cruzando
// formando um X"). A sheet is traced face by face: a face two of whose edges
// are crossed joins those two; a face with all four crossed has its ground
// corners on a diagonal, and joins the edges round each air corner when the
// ground runs across the face (its four corners average half full or more),
// round each ground corner when the air does. Both cells that share the face
// read the same four corners, so they agree, and the two sheets stay two.
[[nodiscard]] int cellSheets(int inside, const std::array<float, 8>& corner, std::array<u8, 12>& sheetOf) noexcept
{
    std::array<u8, 12> parent{};
    std::array<bool, 12> crossed{};
    for (usize edge = 0; edge < 12; ++edge) {
        parent[edge] = static_cast<u8>(edge);
        crossed[edge] = ((inside >> CellEdges[edge][0]) & 1) != ((inside >> CellEdges[edge][1]) & 1);
    }
    const auto find = [&parent](u8 edge) {
        while (parent[edge] != edge)
            edge = parent[edge];
        return edge;
    };
    const auto join = [&](u8 a, u8 b) {
        const u8 ra = find(a);
        const u8 rb = find(b);
        // The lower edge leads: the numbering is the same whatever the order.
        if (ra < rb)
            parent[rb] = ra;
        else
            parent[ra] = rb;
    };
    for (const CellFace& face : CellFaces) {
        int count = 0;
        for (const u8 edge : face.edges)
            count += crossed[edge] ? 1 : 0;
        if (count == 2) {
            u8 first = 0xFF;
            for (const u8 edge : face.edges) {
                if (!crossed[edge])
                    continue;
                if (first == 0xFF)
                    first = edge;
                else
                    join(first, edge);
            }
        }
        else if (count == 4) {
            // In doubles, where four bytes over 255 add exactly: both cells
            // get the same sum whatever order their corners come in.
            double sum = 0.0;
            for (const u8 at : face.corners)
                sum += static_cast<double>(corner[at]);
            const bool groundAcross = sum >= 2.0;
            for (usize at = 0; at < 4; ++at) {
                const bool ground = ((inside >> face.corners[at]) & 1) != 0;
                // The two edges that meet at this corner.
                if (ground != groundAcross)
                    join(face.edges[at], face.edges[(at + 3) % 4]);
            }
        }
    }
    int sheets = 0;
    std::array<u8, 12> number{};
    number.fill(0xFF);
    sheetOf.fill(0xFF);
    for (u8 edge = 0; edge < 12; ++edge) {
        if (!crossed[edge])
            continue;
        const u8 root = find(edge);
        if (number[root] == 0xFF)
            number[root] = static_cast<u8>(sheets++);
        sheetOf[edge] = number[root];
    }
    return sheets;
}

// Where along an edge occupancy passes one half. Guarded for two equal ends,
// which cannot straddle the half -- but a caller that asks should get the
// middle, not a division by zero.
[[nodiscard]] float crossingAt(float a, float b) noexcept
{
    const float delta = b - a;
    if (std::abs(delta) < 1e-6f)
        return 0.5f;
    return std::clamp((0.5f - a) / delta, 0.0f, 1.0f);
}

// **The voxels of a box, read a chunk at a time.** Sampling voxel by voxel
// through the field is a binary search per sample; a node is tens of
// thousands of samples, and this is one search per chunk the box touches.
// **And their paint beside them** (ADR 0114), into `paint` -- left empty when
// no chunk the box touches has any, which is every region nobody painted.
// Level 0 only: a coarse level reads the level-0 surface, gathered (ADR 0140).
void gather(const TerrainField& field, i32 x0, i32 y0, i32 z0, i32 sizeX, i32 sizeY, i32 sizeZ, std::vector<u16>& out,
            std::vector<u16>& paint)
{
    out.assign(static_cast<usize>(sizeX) * static_cast<usize>(sizeY) * static_cast<usize>(sizeZ), 0);
    paint.clear();
    const auto painting = [&] {
        if (paint.empty())
            paint.assign(out.size(), 0);
    };
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    const auto slot = [&](i32 x, i32 y, i32 z) {
        return (static_cast<usize>(z - z0) * static_cast<usize>(sizeY) + static_cast<usize>(y - y0)) *
                   static_cast<usize>(sizeX) +
               static_cast<usize>(x - x0);
    };
    std::array<u16, ChunkEdge> row{};
    for (i32 cz = floorDiv(z0, edge); cz <= floorDiv(z0 + sizeZ - 1, edge); ++cz) {
        for (i32 cx = floorDiv(x0, edge); cx <= floorDiv(x0 + sizeX - 1, edge); ++cx) {
            // One column of chunks: its entries in y order, walked alongside.
            const std::span<const TerrainField::Entry> column = field.column(cx, cz);
            for (const TerrainField::Entry& entry : column) {
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
                const bool painted = chunk.painted();
                if (painted)
                    painting();
                if (chunk.uniform()) {
                    const Voxel voxel = chunk.value();
                    const u16 value = packVoxel(voxel);
                    for (i32 z = lowZ; z < highZ; ++z) {
                        for (i32 y = lowY; y < highY; ++y) {
                            u16* first = &out[slot(lowX, y, z)];
                            std::fill(first, first + (highX - lowX), value);
                            if (painted)
                                std::fill(&paint[slot(lowX, y, z)], &paint[slot(lowX, y, z)] + (highX - lowX),
                                          packPaint(voxel));
                        }
                    }
                    continue;
                }
                for (i32 z = lowZ; z < highZ; ++z) {
                    for (i32 y = lowY; y < highY; ++y) {
                        const auto ly = static_cast<u32>(y - cy * edge);
                        const auto lz = static_cast<u32>(z - cz * edge);
                        chunk.readRow(ly, lz, row);
                        for (i32 x = lowX; x < highX; ++x)
                            out[slot(x, y, z)] = row[static_cast<usize>(x - cx * edge)];
                        if (painted) {
                            chunk.readPaintRow(ly, lz, row);
                            for (i32 x = lowX; x < highX; ++x)
                                paint[slot(x, y, z)] = row[static_cast<usize>(x - cx * edge)];
                        }
                    }
                }
            }
        }
    }
}

} // namespace

namespace {

// Whether one face layer of a chunk is ground all the way across: `axis` 0, 1
// or 2 for x, y or z, and `high` for the layer at 31 rather than 0. The
// chunk's own answer, which a summary keeps (ADR 0144).
[[nodiscard]] bool faceSolid(const TerrainChunk& chunk, u32 axis, bool high) noexcept
{
    return chunk.faceSolid(axis, high);
}

} // namespace

std::vector<std::pair<i32, i32>> activeRuns(const TerrainField& field, i32 chunkX, i32 chunkZ, i32 across)
{
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    // **Which layers of chunks can hold a surface**: one that is not all one
    // value, or one that is solid and has something other than solid ground
    // against any of its six faces -- air above it, the air past the edge of
    // the terrain beside it, or nothing under it. The last two are what give
    // the terrain walls and a bottom of the same kind all round; without them
    // an edge showed a wall where its chunks happened to be rows and an open
    // side where they happened to be whole.
    std::vector<i32> layers;
    for (i32 cz = chunkZ - 1; cz <= chunkZ + across; ++cz) {
        for (i32 cx = chunkX - 1; cx <= chunkX + across; ++cx) {
            for (const TerrainField::Entry& entry : field.column(cx, cz)) {
                const TerrainChunk& chunk = *entry.second;
                bool interesting = !chunk.uniform();
                if (!interesting && chunk.value().occupancy >= 128) {
                    static constexpr std::array<std::array<i32, 3>, 6> Faces{{
                        {-1, 0, 0},
                        {1, 0, 0},
                        {0, -1, 0},
                        {0, 1, 0},
                        {0, 0, -1},
                        {0, 0, 1},
                    }};
                    for (const std::array<i32, 3>& face : Faces) {
                        const TerrainChunk* near = field.findChunk(
                            ChunkKey{entry.first.x + face[0], entry.first.y + face[1], entry.first.z + face[2]});
                        const u32 axis = face[0] != 0 ? 0u : (face[1] != 0 ? 1u : 2u);
                        const bool nearHigh = (face[0] + face[1] + face[2]) < 0;
                        if (near == nullptr || !faceSolid(*near, axis, nearHigh)) {
                            interesting = true;
                            break;
                        }
                    }
                }
                if (interesting)
                    layers.push_back(entry.first.y);
            }
        }
    }
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

    // Each layer's rows, a voxel either side -- a surface on a chunk's face is
    // between its last voxel and the next chunk's first -- merged where they
    // touch.
    std::vector<std::pair<i32, i32>> runs;
    for (const i32 layer : layers) {
        const i32 low = layer * edge - 2;
        const i32 high = layer * edge + edge + 1;
        if (!runs.empty() && low <= runs.back().second + 1)
            runs.back().second = std::max(runs.back().second, high);
        else
            runs.emplace_back(low, high);
    }
    return runs;
}

std::optional<std::pair<i32, i32>> activeRows(const TerrainField& field, i32 chunkX, i32 chunkZ, i32 across)
{
    const std::vector<std::pair<i32, i32>> runs = activeRuns(field, chunkX, chunkZ, across);
    if (runs.empty())
        return std::nullopt;
    return std::pair{runs.front().first, runs.back().second};
}

void appendMesh(TerrainMesh& into, const TerrainMesh& from)
{
    if (from.mesh.indices.empty())
        return;
    if (into.mesh.indices.empty()) {
        into = from;
        return;
    }
    // Sections stay one per material, in id order: both meshes' triangles of a
    // material are gathered into one run.
    const auto offset = static_cast<u32>(into.mesh.vertices.size());
    std::map<u8, std::vector<u32>> buckets;
    const auto gather = [&](const TerrainMesh& mesh, u32 shift) {
        for (usize section = 0; section < mesh.mesh.submeshes.size(); ++section) {
            const Submesh& sub = mesh.mesh.submeshes[section];
            std::vector<u32>& bucket = buckets[mesh.sectionMaterials[section]];
            for (u32 at = 0; at < sub.indexCount; ++at)
                bucket.push_back(mesh.mesh.indices[sub.firstIndex + at] + shift);
        }
    };
    gather(into, 0);
    gather(from, offset);
    into.mesh.vertices.insert(into.mesh.vertices.end(), from.mesh.vertices.begin(), from.mesh.vertices.end());
    into.morphs.insert(into.morphs.end(), from.morphs.begin(), from.morphs.end());
    into.morphTags.insert(into.morphTags.end(), from.morphTags.begin(), from.morphTags.end());
    into.error = std::max(into.error, from.error);
    into.mesh.indices.clear();
    into.mesh.submeshes.clear();
    into.sectionMaterials.clear();
    for (const auto& [key, indices] : buckets) {
        Submesh section;
        section.firstIndex = static_cast<u32>(into.mesh.indices.size());
        section.indexCount = static_cast<u32>(indices.size());
        into.mesh.indices.insert(into.mesh.indices.end(), indices.begin(), indices.end());
        into.mesh.submeshes.push_back(section);
        into.sectionMaterials.push_back(key);
    }
    const auto colliderOffset = static_cast<u32>(into.colliderPoints.size());
    into.colliderPoints.insert(into.colliderPoints.end(), from.colliderPoints.begin(), from.colliderPoints.end());
    for (const u32 index : from.colliderIndices)
        into.colliderIndices.push_back(index + colliderOffset);
    into.mesh.bounds.min.x = std::min(into.mesh.bounds.min.x, from.mesh.bounds.min.x);
    into.mesh.bounds.min.y = std::min(into.mesh.bounds.min.y, from.mesh.bounds.min.y);
    into.mesh.bounds.min.z = std::min(into.mesh.bounds.min.z, from.mesh.bounds.min.z);
    into.mesh.bounds.max.x = std::max(into.mesh.bounds.max.x, from.mesh.bounds.max.x);
    into.mesh.bounds.max.y = std::max(into.mesh.bounds.max.y, from.mesh.bounds.max.y);
    into.mesh.bounds.max.z = std::max(into.mesh.bounds.max.z, from.mesh.bounds.max.z);
}

namespace {

// How far an openness ray is followed across, and the columns it looks at along
// the way: close together near the point, where a wall beside it decides most,
// and further apart out where only a hill can.
constexpr float SkyReach = 12.0f;
constexpr std::array<float, 8> SkyStops{1.0f, 2.0f, 3.0f, 4.5f, 6.0f, 8.0f, 10.0f, SkyReach};

// **The largest value within `reach` of each place in `values`**, in time that
// does not grow with the reach (van Herk and Gil-Werman): each block of one
// window's width gives its running maximum from the left and from the right,
// and a window is the right-hand run of the block it starts in beside the
// left-hand run of the block it ends in. The naive walk was a window's width
// per value, which at small voxels was most of a region's cost (terrain audit
// P2).
void slidingMax(std::span<const float> values, i32 reach, std::vector<float>& out)
{
    const auto count = static_cast<i32>(values.size());
    const i32 width = 2 * reach + 1;
    // Padded with nothing either side, so a window at an end is a window.
    const i32 padded = count + 2 * reach;
    const auto at = [&](i32 index) {
        const i32 source = index - reach;
        return source >= 0 && source < count ? values[static_cast<usize>(source)]
                                             : -std::numeric_limits<float>::infinity();
    };
    std::vector<float> fromLeft(static_cast<usize>(padded));
    std::vector<float> fromRight(static_cast<usize>(padded));
    for (i32 index = 0; index < padded; ++index) {
        const float value = at(index);
        fromLeft[static_cast<usize>(index)] =
            index % width == 0 ? value : std::max(fromLeft[static_cast<usize>(index - 1)], value);
    }
    for (i32 index = padded - 1; index >= 0; --index) {
        const float value = at(index);
        fromRight[static_cast<usize>(index)] = (index + 1) % width == 0 || index == padded - 1
                                                   ? value
                                                   : std::max(fromRight[static_cast<usize>(index + 1)], value);
    }
    out.resize(static_cast<usize>(count));
    for (i32 index = 0; index < count; ++index) {
        // The window over `values[index - reach, index + reach]` is
        // `padded[index, index + width - 1]`.
        out[static_cast<usize>(index)] =
            std::max(fromRight[static_cast<usize>(index)], fromLeft[static_cast<usize>(index + width - 1)]);
    }
}

// The column map's margin round a region, in cells of `step` metres: the
// longest ray, the lift off the surface and a cell to spare.
[[nodiscard]] i32 skyMargin(float step) noexcept
{
    return static_cast<i32>(std::ceil((SkyReach + 2.5f) / step)) + 1;
}

} // namespace

i32 meshReach(const FieldSettings& settings, u32 level) noexcept
{
    // Samples run two cells below a region and three past it when its sides
    // are closed (`meshField`).
    return std::max(3, skyMargin(settings.voxelSize * static_cast<float>(1u << level)));
}

// --- The surface under a chunk, gathered by level (ADR 0140) -----------------

u8 SurfaceCell::material() const noexcept
{
    u8 best = materials[0];
    u16 most = votes[0];
    for (usize at = 1; at < materials.size(); ++at) {
        if (votes[at] > most || (votes[at] == most && votes[at] > 0 && materials[at] < best)) {
            best = materials[at];
            most = votes[at];
        }
    }
    return best;
}

float SurfaceCell::paintCover() const noexcept
{
    float sum = cover;
    for (usize at = 0; at < materials.size(); ++at) {
        if (top != 0 && votes[at] > 0 && materials[at] == top)
            sum += 255.0f * static_cast<float>(votes[at]);
    }
    return sum;
}

std::array<float, 3> SurfaceCell::point(u32 span) const noexcept
{
    const auto wide = [](float value) { return static_cast<double>(value); };
    const double inverse = 1.0 / static_cast<double>(std::max<u32>(count, 1));
    const std::array<double, 3> mean{wide(offset[0]) * inverse, wide(offset[1]) * inverse, wide(offset[2]) * inverse};
    // The quadric as a matrix, and what it asks of the mean.
    std::array<std::array<double, 3>, 3> a{{
        {wide(quadric[0]), wide(quadric[1]), wide(quadric[2])},
        {wide(quadric[1]), wide(quadric[3]), wide(quadric[4])},
        {wide(quadric[2]), wide(quadric[4]), wide(quadric[5])},
    }};
    std::array<double, 3> residual{};
    for (int row = 0; row < 3; ++row) {
        residual[static_cast<usize>(row)] = wide(plane[row]);
        for (int column = 0; column < 3; ++column)
            residual[static_cast<usize>(row)] -=
                a[static_cast<usize>(row)][static_cast<usize>(column)] * mean[static_cast<usize>(column)];
    }
    // Its eigenvectors, by Jacobi rotations: at most a fixed number of sweeps,
    // ended early only by the matrix itself, so the same cell places its
    // vertex at the same point everywhere. Most cells are diagonal after two.
    std::array<std::array<double, 3>, 3> v{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
    for (int sweep = 0; sweep < 12; ++sweep) {
        const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        const double scale = std::abs(a[0][0]) + std::abs(a[1][1]) + std::abs(a[2][2]);
        if (off <= 1e-15 * scale)
            break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const auto up = static_cast<usize>(p);
                const auto uq = static_cast<usize>(q);
                if (std::abs(a[up][uq]) < 1e-12)
                    continue;
                const double theta = (a[uq][uq] - a[up][up]) / (2.0 * a[up][uq]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (usize k = 0; k < 3; ++k) {
                    const double akp = a[k][up];
                    const double akq = a[k][uq];
                    a[k][up] = c * akp - s * akq;
                    a[k][uq] = s * akp + c * akq;
                }
                for (usize k = 0; k < 3; ++k) {
                    const double apk = a[up][k];
                    const double aqk = a[uq][k];
                    a[up][k] = c * apk - s * aqk;
                    a[uq][k] = s * apk + c * aqk;
                }
                for (usize k = 0; k < 3; ++k) {
                    const double vkp = v[k][up];
                    const double vkq = v[k][uq];
                    v[k][up] = c * vkp - s * vkq;
                    v[k][uq] = s * vkp + c * vkq;
                }
            }
        }
    }
    const double largest = std::max({a[0][0], a[1][1], a[2][2]});
    std::array<double, 3> out = mean;
    if (largest > 1e-9) {
        for (usize e = 0; e < 3; ++e) {
            const double lambda = a[e][e];
            // A direction the planes hardly constrain keeps the mean. Two per
            // cent, not ten: a block's rim is a cell with many vertices on its
            // top and a few on its side, and at ten the side was dropped and
            // the rim drawn half a cell in (the terrain gallery, far away).
            if (lambda < 0.02 * largest)
                continue;
            const double along = (v[0][e] * residual[0] + v[1][e] * residual[1] + v[2][e] * residual[2]) / lambda;
            for (usize k = 0; k < 3; ++k)
                out[k] += v[k][e] * along;
        }
    }
    const auto limit = static_cast<double>(span);
    return {static_cast<float>(std::clamp(out[0], 0.0, limit)), static_cast<float>(std::clamp(out[1], 0.0, limit)),
            static_cast<float>(std::clamp(out[2], 0.0, limit))};
}

float SurfaceCell::error(u32 span) const noexcept
{
    return error(span, point(span));
}

float SurfaceCell::error(u32 span, std::array<float, 3> at) const noexcept
{
    const auto wide = [](float value) { return static_cast<double>(value); };
    const double x = wide(at[0]);
    const double y = wide(at[1]);
    const double z = wide(at[2]);
    const std::array<double, 6> q{wide(quadric[0]), wide(quadric[1]), wide(quadric[2]),
                                  wide(quadric[3]), wide(quadric[4]), wide(quadric[5])};
    // x'Ax - 2 b'x + c: the sum of the squared plane distances.
    const double axx = q[0] * x * x + q[3] * y * y + q[5] * z * z + 2.0 * (q[1] * x * y + q[2] * x * z + q[4] * y * z);
    const double sum = axx - 2.0 * (wide(plane[0]) * x + wide(plane[1]) * y + wide(plane[2]) * z) + wide(constant);
    auto rms = static_cast<float>(std::sqrt(std::max(sum, 0.0) / static_cast<double>(std::max<u32>(count, 1))));
    int materialsSeen = 0;
    for (const u16 vote : votes)
        materialsSeen += vote > 0 ? 1 : 0;
    if (materialsSeen > 1 || topVotes > 0)
        rms = std::max(rms, 0.5f * static_cast<float>(span));
    return rms;
}

const SurfaceCell* SurfaceLevel::cell(u16 index) const noexcept
{
    const auto at = std::lower_bound(cells.begin(), cells.end(), index,
                                     [](const SurfaceCell& c, u16 wanted) { return c.index < wanted; });
    return at != cells.end() && at->index == index ? &*at : nullptr;
}

const SurfaceEdge* SurfaceLevel::edge(u16 index, u8 axis) const noexcept
{
    const auto key = [](u16 i, u8 a) { return (static_cast<u32>(i) << 2) | a; };
    const u32 wanted = key(index, axis);
    const auto at = std::lower_bound(edges.begin(), edges.end(), wanted,
                                     [&](const SurfaceEdge& e, u32 w) { return key(e.index, e.axis) < w; });
    return at != edges.end() && key(at->index, at->axis) == wanted ? &*at : nullptr;
}

namespace {

// What a chunk's gathered surface depends on: its own voxels and the border of
// every neighbour it reads -- the voxel past each face, edge and corner.
[[nodiscard]] core::u64 surfaceKey(const TerrainField& field, ChunkKey key) noexcept
{
    core::u64 h = 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](core::u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const TerrainChunk* near = field.findChunk(ChunkKey{key.x + dx, key.y + dy, key.z + dz});
                // Ourselves whole; a neighbour by the part of it against us.
                mix(near == nullptr ? 0x51ull : near->borderDigest(-dx, -dy, -dz));
            }
        }
    }
    return h;
}

// **No surface to gather** where a chunk and its 26 neighbours are each one
// value, all on the same side of the surface: air, or rock under it, which is
// most of what a view holds. Known from the chunks alone -- no voxel read, no
// digest -- where reading the voxels costs as much as a chunk with a surface.
[[nodiscard]] bool plainAround(const TerrainField& field, ChunkKey key) noexcept
{
    const auto sideOf = [&](i32 dx, i32 dy, i32 dz) -> int {
        const TerrainChunk* near = field.findChunk(ChunkKey{key.x + dx, key.y + dy, key.z + dz});
        if (near == nullptr)
            return 0;
        if (!near->uniform())
            return -1;
        return 2u * static_cast<u32>(near->value().occupancy) > FullOccupancy ? 1 : 0;
    };
    const int side = sideOf(0, 0, 0);
    if (side < 0)
        return false;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (sideOf(dx, dy, dz) != side)
                    return false;
            }
        }
    }
    return true;
}

void addVote(SurfaceCell& cell, u8 material)
{
    for (usize at = 0; at < cell.materials.size(); ++at) {
        if (cell.votes[at] > 0 && cell.materials[at] == material) {
            cell.votes[at] += 1;
            return;
        }
    }
    // A new material takes the emptiest seat -- or the weakest, which on a
    // cell of five materials is the one it would not have won anyway.
    usize seat = 0;
    for (usize at = 1; at < cell.materials.size(); ++at) {
        if (cell.votes[at] < cell.votes[seat])
            seat = at;
    }
    if (cell.votes[seat] == 0) {
        cell.materials[seat] = material;
        cell.votes[seat] = 1;
    }
}

} // namespace

SurfaceLevels buildSurfaces(const TerrainField& field, ChunkKey key, u32 levels)
{
    SurfaceLevels out{};
    const auto wanted = [levels](u32 level) {
        return level > 0 && level < ChunkLevels && ((levels >> level) & 1u) != 0;
    };

    if (plainAround(field, key)) {
        static const auto Nothing = std::make_shared<const SurfaceLevel>();
        for (u32 level = 1; level < ChunkLevels; ++level) {
            if (wanted(level))
                out[level] = Nothing;
        }
        return out;
    }

    // The chunk's voxels and one more on every side: a cell's far corners and
    // the gradient at its near ones.
    constexpr auto edge = static_cast<i32>(ChunkEdge);
    constexpr i32 size = edge + 3;
    const i32 x0 = key.x * edge - 1;
    const i32 y0 = key.y * edge - 1;
    const i32 z0 = key.z * edge - 1;
    // **Each thread's own scratch, kept**: a level-1 grid alone is some
    // 400 KB, and allocated and cleared for every chunk it cost more than the
    // chunk's surface did. Only the cells written are cleared after.
    thread_local std::vector<u16> samples;
    thread_local std::vector<u16> paints;
    thread_local std::array<std::vector<SurfaceCell>, ChunkLevels> dense;
    thread_local std::array<std::vector<u16>, ChunkLevels> touched;
    gather(field, x0, y0, z0, size, size, size, samples, paints);
    const auto sampleIndex = [](i32 sx, i32 sy, i32 sz) {
        return (static_cast<usize>(sz) * size + static_cast<usize>(sy)) * size + static_cast<usize>(sx);
    };
    // The same quotient as dividing each time, worked out once per byte.
    static const std::array<float, 256> Occupancies = [] {
        std::array<float, 256> table{};
        for (usize at = 0; at < table.size(); ++at)
            table[at] = static_cast<float>(at) / static_cast<float>(FullOccupancy);
        return table;
    }();
    const auto occupancy = [&](i32 sx, i32 sy, i32 sz) { return Occupancies[samples[sampleIndex(sx, sy, sz)] & 0xFF]; };
    // `occupancy >= 0.5`, exactly, without the division: FullOccupancy is odd,
    // so no byte sits on the half.
    static_assert(FullOccupancy % 2 == 1);
    const auto solidAt = [&](i32 sx, i32 sy, i32 sz) {
        return 2u * static_cast<u32>(samples[sampleIndex(sx, sy, sz)] & 0xFF) > FullOccupancy;
    };
    const auto materialAt = [&](i32 sx, i32 sy, i32 sz) {
        return static_cast<u8>(samples[sampleIndex(sx, sy, sz)] >> 8);
    };
    const auto paintAt = [&](i32 sx, i32 sy, i32 sz) -> u16 {
        return paints.empty() ? u16{0} : paints[sampleIndex(sx, sy, sz)];
    };
    const auto normalAt = [&](i32 sx, i32 sy, i32 sz) {
        const auto clampIndex = [](i32 v) { return std::clamp(v, 0, size - 1); };
        return Vec3{occupancy(clampIndex(sx - 1), sy, sz) - occupancy(clampIndex(sx + 1), sy, sz),
                    occupancy(sx, clampIndex(sy - 1), sz) - occupancy(sx, clampIndex(sy + 1), sz),
                    occupancy(sx, sy, clampIndex(sz - 1)) - occupancy(sx, sy, clampIndex(sz + 1))};
    };

    // **Every level asked for from one read** (ADR 0140): a level-0 cell's
    // vertex is added to the cell above it at each, so what reading and
    // meshing level 0 costs is paid once for all of them.
    for (u32 level = 1; level < ChunkLevels; ++level) {
        const auto levelEdge = static_cast<usize>(edge >> level);
        dense[level].resize(levelEdge * levelEdge * levelEdge);
        touched[level].clear();
    }
    // Level-0 cell `c` (0..31 on each axis) has its low corner at sample `c + 1`.
    for (i32 cz = 0; cz < edge; ++cz) {
        for (i32 cy = 0; cy < edge; ++cy) {
            for (i32 cx = 0; cx < edge; ++cx) {
                // Which corners are ground, by the bytes: most cells have
                // none or all, and are done before any arithmetic.
                int inside = 0;
                for (int at = 0; at < 8; ++at) {
                    const auto& o = CornerOffsets[static_cast<usize>(at)];
                    if (solidAt(cx + 1 + o[0], cy + 1 + o[1], cz + 1 + o[2]))
                        inside |= 1 << at;
                }
                if (inside == 0 || inside == 0xFF)
                    continue;
                std::array<float, 8> corner{};
                for (int at = 0; at < 8; ++at) {
                    const auto& o = CornerOffsets[static_cast<usize>(at)];
                    corner[static_cast<usize>(at)] = occupancy(cx + 1 + o[0], cy + 1 + o[1], cz + 1 + o[2]);
                }
                float sum[3]{};
                Vec3 normal{0.0f, 0.0f, 0.0f};
                int crossings = 0;
                for (const std::array<int, 2>& corners : CellEdges) {
                    const auto a = static_cast<usize>(corners[0]);
                    const auto b = static_cast<usize>(corners[1]);
                    if (((inside >> a) & 1) == ((inside >> b) & 1))
                        continue;
                    const float t = crossingAt(corner[a], corner[b]);
                    const auto& oa = CornerOffsets[a];
                    const auto& ob = CornerOffsets[b];
                    for (int axis = 0; axis < 3; ++axis)
                        sum[axis] +=
                            static_cast<float>(oa[static_cast<usize>(axis)]) +
                            static_cast<float>(ob[static_cast<usize>(axis)] - oa[static_cast<usize>(axis)]) * t;
                    const Vec3 na = normalAt(cx + 1 + oa[0], cy + 1 + oa[1], cz + 1 + oa[2]);
                    const Vec3 nb = normalAt(cx + 1 + ob[0], cy + 1 + ob[1], cz + 1 + ob[2]);
                    normal.x += na.x + (nb.x - na.x) * t;
                    normal.y += na.y + (nb.y - na.y) * t;
                    normal.z += na.z + (nb.z - na.z) * t;
                    ++crossings;
                }
                const float inverse = 1.0f / static_cast<float>(crossings);
                const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

                // The commonest material among the level-0 cell's solid
                // corners, as level 0 draws it; and what is painted over it.
                std::array<u8, 8> seen{};
                std::array<int, 8> counts{};
                int kinds = 0;
                int solid = 0;
                std::array<u8, 8> overs{};
                std::array<int, 8> covers{};
                std::array<u8, 8> bare{};
                int overKinds = 0;
                for (int at = 0; at < 8; ++at) {
                    if ((inside & (1 << at)) == 0)
                        continue;
                    ++solid;
                    const auto& o = CornerOffsets[static_cast<usize>(at)];
                    const u8 m = materialAt(cx + 1 + o[0], cy + 1 + o[1], cz + 1 + o[2]);
                    int slot = 0;
                    while (slot < kinds && seen[static_cast<usize>(slot)] != m)
                        ++slot;
                    if (slot == kinds)
                        seen[static_cast<usize>(kinds++)] = m;
                    ++counts[static_cast<usize>(slot)];
                    const u16 painted = paintAt(cx + 1 + o[0], cy + 1 + o[1], cz + 1 + o[2]);
                    const auto over = static_cast<u8>(painted & 0xFF);
                    if (over == 0 || (painted >> 8) == 0) {
                        bare[static_cast<usize>(at)] = m;
                        continue;
                    }
                    int p = 0;
                    while (p < overKinds && overs[static_cast<usize>(p)] != over)
                        ++p;
                    if (p == overKinds)
                        overs[static_cast<usize>(overKinds++)] = over;
                    covers[static_cast<usize>(p)] += painted >> 8;
                }
                coverWithOwnGround(bare, overs, covers, overKinds);
                u8 material = seen[0];
                int best = counts[0];
                for (int slot = 1; slot < kinds; ++slot) {
                    if (counts[static_cast<usize>(slot)] > best ||
                        (counts[static_cast<usize>(slot)] == best && seen[static_cast<usize>(slot)] < material)) {
                        material = seen[static_cast<usize>(slot)];
                        best = counts[static_cast<usize>(slot)];
                    }
                }
                u8 top = 0;
                int most = 0;
                for (int p = 0; p < overKinds; ++p) {
                    if (covers[static_cast<usize>(p)] > most ||
                        (covers[static_cast<usize>(p)] == most && overs[static_cast<usize>(p)] < top)) {
                        top = overs[static_cast<usize>(p)];
                        most = covers[static_cast<usize>(p)];
                    }
                }
                for (u32 level = 1; level < ChunkLevels; ++level) {
                    if (!wanted(level))
                        continue;
                    const i32 levelEdge = edge >> level;
                    const i32 lx = cx >> level;
                    const i32 ly = cy >> level;
                    const i32 lz = cz >> level;
                    const auto index = static_cast<u16>((lz * levelEdge + ly) * levelEdge + lx);
                    SurfaceCell& cell = dense[level][index];
                    if (cell.count == 0)
                        touched[level].push_back(index);
                    cell.count += 1;
                    const std::array<float, 3> vertexAt{static_cast<float>(cx - (lx << level)) + sum[0] * inverse,
                                                        static_cast<float>(cy - (ly << level)) + sum[1] * inverse,
                                                        static_cast<float>(cz - (lz << level)) + sum[2] * inverse};
                    cell.offset[0] += vertexAt[0];
                    cell.offset[1] += vertexAt[1];
                    cell.offset[2] += vertexAt[2];
                    if (length > 1e-8f) {
                        const std::array<float, 3> n{normal.x / length, normal.y / length, normal.z / length};
                        cell.normal[0] += n[0];
                        cell.normal[1] += n[1];
                        cell.normal[2] += n[2];
                        cell.quadric[0] += n[0] * n[0];
                        cell.quadric[1] += n[0] * n[1];
                        cell.quadric[2] += n[0] * n[2];
                        cell.quadric[3] += n[1] * n[1];
                        cell.quadric[4] += n[1] * n[2];
                        cell.quadric[5] += n[2] * n[2];
                        const float d = n[0] * vertexAt[0] + n[1] * vertexAt[1] + n[2] * vertexAt[2];
                        cell.plane[0] += n[0] * d;
                        cell.plane[1] += n[1] * d;
                        cell.plane[2] += n[2] * d;
                        cell.constant += d * d;
                    }
                    addVote(cell, material);
                    if (top != 0 && top != material && solid > 0) {
                        // Votes for the commonest paint, and its cover summed over
                        // every vertex -- zero where none is painted.
                        if (cell.top == top || cell.topVotes == 0) {
                            cell.top = top;
                            cell.topVotes += 1;
                        }
                        else if (--cell.topVotes == 0) {
                            cell.top = top;
                            cell.topVotes = 1;
                        }
                        cell.cover += static_cast<float>(std::min(255, (most + solid / 2) / solid));
                    }
                }
            }
        }
    }

    for (u32 level = 1; level < ChunkLevels; ++level) {
        if (!wanted(level))
            continue;
        const i32 span = 1 << level;
        const i32 levelEdge = edge >> level;
        auto gathered = std::make_shared<SurfaceLevel>();
        // In index order, as `SurfaceLevel::cell` finds them; and the scratch left
        // clear for the next chunk.
        std::sort(touched[level].begin(), touched[level].end());
        gathered->cells.reserve(touched[level].size());
        for (const u16 at : touched[level]) {
            SurfaceCell cell = dense[level][at];
            cell.index = at;
            cell.placed = cell.point(static_cast<u32>(span));
            cell.deviation = cell.error(static_cast<u32>(span), cell.placed);
            gathered->cells.push_back(cell);
            dense[level][at] = SurfaceCell{};
        }

        // The level's lattice edges: the level-0 edges on its lines, each way.
        for (i32 pz = 0; pz < levelEdge; ++pz) {
            for (i32 py = 0; py < levelEdge; ++py) {
                for (i32 px = 0; px < levelEdge; ++px) {
                    const auto index = static_cast<u16>((pz * levelEdge + py) * levelEdge + px);
                    for (u8 axis = 0; axis < 3; ++axis) {
                        SurfaceEdge crossed;
                        crossed.index = index;
                        crossed.axis = axis;
                        for (i32 step = 0; step < span; ++step) {
                            std::array<i32, 3> at{(px << level) + 1, (py << level) + 1, (pz << level) + 1};
                            at[axis] += step;
                            std::array<i32, 3> next = at;
                            next[axis] += 1;
                            const bool here = solidAt(at[0], at[1], at[2]);
                            const bool there = solidAt(next[0], next[1], next[2]);
                            if (here == there)
                                continue;
                            if (here)
                                crossed.up += 1;
                            else
                                crossed.down += 1;
                        }
                        if (crossed.up != 0 || crossed.down != 0)
                            gathered->edges.push_back(crossed);
                    }
                }
            }
        }
        out[level] = std::move(gathered);
    }
    return out;
}

void cacheSurfaces(const TerrainField& field, ChunkKey key, core::u64 content, const SurfaceLevels& surfaces)
{
    for (u32 level = 1; level < ChunkLevels; ++level) {
        if (surfaces[level] != nullptr)
            field.cacheSurface(key, level, content, surfaces[level]);
    }
}

core::u64 surfaceContent(const TerrainField& field, ChunkKey key) noexcept
{
    return surfaceKey(field, key);
}

void prepareSurface(const TerrainField& field, ChunkKey key, u32 level)
{
    if (level == 0 || level >= ChunkLevels)
        return;
    const core::u64 wanted = surfaceKey(field, key);
    if (field.cachedSurface(key, level, wanted) != nullptr)
        return;
    cacheSurfaces(field, key, wanted, buildSurfaces(field, key, 1u << level));
}

std::shared_ptr<const SurfaceLevel> surfaceOf(const TerrainField& field, ChunkKey key, u32 level)
{
    // One given comes first (ADR 0150): it is the chunk's surface whatever
    // the field holds round it now.
    if (std::shared_ptr<const SurfaceLevel> given = field.adoptedSurface(key, level); given != nullptr)
        return given;
    return field.cachedSurface(key, level, surfaceKey(field, key));
}

namespace {

// Every chunk whose gathered surface a coarse region reads -- its cells and
// edges, the ring past them and the columns its sky rays cross -- in key order.
[[nodiscard]] std::vector<ChunkKey> surfaceKeysOf(const TerrainField& field, const MeshRegion& region)
{
    std::vector<ChunkKey> keys;
    if (region.level == 0 || region.level >= ChunkLevels)
        return keys;
    const i32 levelEdge = static_cast<i32>(ChunkEdge) >> region.level;
    const float step = field.settings().voxelSize * static_cast<float>(1u << region.level);
    const i32 margin = std::max(2, skyMargin(step));
    const i32 lowX = floorDiv(region.minX - 1 - margin, levelEdge);
    const i32 highX = floorDiv(region.minX + static_cast<i32>(region.cellsX) + margin, levelEdge);
    const i32 lowZ = floorDiv(region.minZ - 1 - margin, levelEdge);
    const i32 highZ = floorDiv(region.minZ + static_cast<i32>(region.cellsZ) + margin, levelEdge);
    // The chunk columns in reach, and one more round: a cell's surface can
    // belong to a chunk nobody stored, beside one somebody did. **Only
    // there**: a chunk nobody stored with none stored round it is air, and has
    // no surface to gather -- in a tall region, most of the rows.
    std::vector<i32> rows;
    const i32 lowY = floorDiv(region.minY - 1, levelEdge);
    const i32 highY = floorDiv(region.minY + static_cast<i32>(region.cellsY), levelEdge);
    for (i32 kz = lowZ; kz <= highZ; ++kz) {
        for (i32 kx = lowX; kx <= highX; ++kx) {
            rows.clear();
            for (i32 y = lowY; y <= highY; ++y)
                rows.push_back(y);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (const TerrainField::Entry& entry : field.column(kx + dx, kz + dz)) {
                        rows.push_back(entry.first.y - 1);
                        rows.push_back(entry.first.y);
                        rows.push_back(entry.first.y + 1);
                    }
                }
            }
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            for (const i32 y : rows)
                keys.push_back(ChunkKey{kx, y, kz});
        }
    }
    return keys;
}

} // namespace

namespace {

// Every chunk and level whose gathered surface `meshField(field, region)`
// reads, to `visit`.
template <typename Visit>
void forEachSurface(const TerrainField& field, const MeshRegion& region, Visit&& visit)
{
    for (const ChunkKey key : surfaceKeysOf(field, region))
        visit(key, region.level);
    // Each vertex's parent, one level up (the geomorph's target).
    if (region.level > 0 && region.level + 1 < ChunkLevels) {
        MeshRegion up = region;
        up.level = region.level + 1;
        up.minX = floorDiv(region.minX - 1, 2);
        up.minY = floorDiv(region.minY - 1, 2);
        up.minZ = floorDiv(region.minZ - 1, 2);
        up.cellsX = static_cast<u32>(floorDiv(region.minX + static_cast<i32>(region.cellsX), 2) - up.minX + 1);
        up.cellsY = static_cast<u32>(floorDiv(region.minY + static_cast<i32>(region.cellsY), 2) - up.minY + 1);
        up.cellsZ = static_cast<u32>(floorDiv(region.minZ + static_cast<i32>(region.cellsZ), 2) - up.minZ + 1);
        for (const ChunkKey key : surfaceKeysOf(field, up))
            visit(key, up.level);
    }
    // Level 0's sky reads its columns' runs from level 1, and its vertices'
    // parents are there too. A collider reads neither.
    if (region.level == 0 && !region.collider) {
        MeshRegion one = region;
        one.level = 1;
        one.minX = floorDiv(region.minX, 2);
        one.minY = floorDiv(region.minY, 2);
        one.minZ = floorDiv(region.minZ, 2);
        one.cellsX = static_cast<u32>(floorDiv(region.minX + static_cast<i32>(region.cellsX), 2) - one.minX + 1);
        one.cellsY = static_cast<u32>(floorDiv(region.minY + static_cast<i32>(region.cellsY), 2) - one.minY + 1);
        one.cellsZ = static_cast<u32>(floorDiv(region.minZ + static_cast<i32>(region.cellsZ), 2) - one.minZ + 1);
        for (const ChunkKey key : surfaceKeysOf(field, one))
            visit(key, 1u);
    }
    // And each coarser neighbour's level, over the same ground, for the cells
    // stitched to it.
    for (const core::u8 neighbour : region.sideLevels) {
        if (neighbour <= region.level || neighbour >= ChunkLevels)
            continue;
        const i32 k = static_cast<i32>(neighbour - region.level);
        MeshRegion wide = region;
        wide.level = neighbour;
        wide.minX = floorDiv(region.minX - 1, 1 << k);
        wide.minY = floorDiv(region.minY - 1, 1 << k);
        wide.minZ = floorDiv(region.minZ - 1, 1 << k);
        wide.cellsX = static_cast<u32>(floorDiv(region.minX + static_cast<i32>(region.cellsX), 1 << k) - wide.minX + 1);
        wide.cellsY = static_cast<u32>(floorDiv(region.minY + static_cast<i32>(region.cellsY), 1 << k) - wide.minY + 1);
        wide.cellsZ = static_cast<u32>(floorDiv(region.minZ + static_cast<i32>(region.cellsZ), 1 << k) - wide.minZ + 1);
        for (const ChunkKey key : surfaceKeysOf(field, wide))
            visit(key, static_cast<u32>(neighbour));
        // And a level above it, where the neighbour's seam vertices slide to.
        if (neighbour + 1u < ChunkLevels) {
            MeshRegion above = wide;
            above.level = neighbour + 1u;
            above.minX = floorDiv(wide.minX - 1, 2);
            above.minY = floorDiv(wide.minY - 1, 2);
            above.minZ = floorDiv(wide.minZ - 1, 2);
            above.cellsX = static_cast<u32>(floorDiv(wide.minX + static_cast<i32>(wide.cellsX), 2) - above.minX + 1);
            above.cellsY = static_cast<u32>(floorDiv(wide.minY + static_cast<i32>(wide.cellsY), 2) - above.minY + 1);
            above.cellsZ = static_cast<u32>(floorDiv(wide.minZ + static_cast<i32>(wide.cellsZ), 2) - above.minZ + 1);
            for (const ChunkKey key : surfaceKeysOf(field, above))
                visit(key, above.level);
        }
    }
}

} // namespace

void prepareRegion(const TerrainField& field, const MeshRegion& region)
{
    std::vector<SurfaceWant> missing;
    missingSurfaces(field, region, missing);
    for (const SurfaceWant& want : missing)
        cacheSurfaces(field, want.key, surfaceKey(field, want.key), buildSurfaces(field, want.key, want.levels));
}

void missingSurfaces(const TerrainField& field, const MeshRegion& region, std::vector<SurfaceWant>& out)
{
    // Each chunk's content keyed once, however many levels read it: the key
    // is most of what a lookup costs.
    std::vector<std::pair<ChunkKey, u32>> wanted;
    forEachSurface(field, region, [&](ChunkKey key, u32 level) {
        if (level > 0 && level < ChunkLevels)
            wanted.emplace_back(key, level);
    });
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (usize at = 0; at < wanted.size();) {
        const ChunkKey key = wanted[at].first;
        // **What was given is not asked after** (ADR 0150): neither whether
        // the chunk is plain all round nor what it is keyed by -- 27
        // neighbours each, for every chunk of a node read from a file.
        u32 open = 0;
        for (; at < wanted.size() && wanted[at].first == key; ++at) {
            if (field.adoptedSurface(key, wanted[at].second) == nullptr)
                open |= 1u << wanted[at].second;
        }
        // Air or rock all round has nothing to gather, and `meshField` knows
        // it without asking the cache.
        if (open == 0 || plainAround(field, key))
            continue;
        const core::u64 content = surfaceKey(field, key);
        u32 levels = 0;
        for (u32 level = 1; level < ChunkLevels; ++level) {
            if ((open & (1u << level)) != 0 && field.cachedSurface(key, level, content) == nullptr)
                levels |= 1u << level;
        }
        if (levels != 0)
            out.push_back(SurfaceWant{key, levels});
    }
}

std::vector<std::pair<float, float>> terrainColumnRuns(std::vector<std::pair<float, float>> surfaces, float top,
                                                       float bottom)
{
    constexpr float Unbounded = 1.0e30f;
    std::sort(surfaces.begin(), surfaces.end());
    std::vector<std::pair<float, float>> found;
    float start = std::numeric_limits<float>::quiet_NaN();
    bool below = true;
    for (const auto& [y, facing] : surfaces) {
        if (facing < -0.3f) {
            start = y;
            below = false;
        }
        else if (facing > 0.3f) {
            found.emplace_back(std::isnan(start) ? (below ? -Unbounded : y) : start, y);
            start = std::numeric_limits<float>::quiet_NaN();
            below = false;
        }
    }
    // **A run nothing closes ends at the column's own top** (the owner's
    // place, 500 m): under the bulge of a mound a column holds the underside,
    // facing down, while the wall above it is too steep to count and the cap
    // is in the next column -- and the run went to the sky, a pillar every ray
    // past it met, in a row of dark spots down the flank. Only with nothing of
    // the column above it is it the ground to the top of the world.
    if (!std::isnan(start))
        found.emplace_back(start, !std::isnan(top) && top > start ? top : Unbounded);
    // Where the gathered surface said nothing, the column's own extent.
    if (found.empty() && !std::isnan(top))
        found.emplace_back(std::isnan(bottom) ? -Unbounded : bottom, top);
    return found;
}

TerrainMesh meshField(const TerrainField& field, const MeshRegion& region)
{
    TerrainMesh out;
    if (region.cellsX == 0 || region.cellsY == 0 || region.cellsZ == 0 || region.level >= ChunkLevels)
        return out;
    const u32 level = region.level;
    // Whether anything reads the sky and the geomorph: not a collider.
    const bool drawn = !region.collider;
    const float step = field.settings().voxelSize * static_cast<float>(1u << level);
    const auto nx = static_cast<i32>(region.cellsX);
    const auto ny = static_cast<i32>(region.cellsY);
    const auto nz = static_cast<i32>(region.cellsZ);

    // **The samples: the owned points, a cell ring below them, and one more on
    // every side for the gradient.** Owned points are `[min, min + n)`; cells
    // run from `min - 1` to `min + n - 1` and read points `min - 1` to
    // `min + n`; the gradient at those reads one further out.
    const i32 sx0 = region.minX - 2;
    const i32 sy0 = region.minY - 2;
    const i32 sz0 = region.minZ - 2;
    const i32 sizeX = nx + 4;
    const i32 sizeY = ny + 4;
    const i32 sizeZ = nz + 4;
    std::vector<u16> samples;
    std::vector<u16> paints;
    // Level 0 reads its voxels; a coarse level reads the level-0 surface,
    // gathered (ADR 0140), and no samples at all.
    if (level == 0)
        gather(field, sx0, sy0, sz0, sizeX, sizeY, sizeZ, samples, paints);
    // **The gathered surfaces it reads, looked up once each** (ADR 0140): from
    // the field's cache, or -- where nobody prepared them -- gathered here and
    // kept for this call only. Meshing never writes to the field, so any
    // number of meshes are made at once; one per vertex, the lookup was most
    // of what a coarse node cost.
    // Held here, so an entry the cache replaces meanwhile stays whole.
    std::map<std::pair<ChunkKey, u32>, std::shared_ptr<const SurfaceLevel>> surfaceMemo;
    const auto surfaceIn = [&](const TerrainField& from, ChunkKey key, u32 at) -> const SurfaceLevel* {
        if (at == 0 || at >= ChunkLevels)
            return nullptr;
        if (const auto found = surfaceMemo.find({key, at}); found != surfaceMemo.end())
            return found->second.get();
        // One given (ADR 0150) is taken as it is, with nothing asked of the
        // chunks round it.
        if (std::shared_ptr<const SurfaceLevel> given = from.adoptedSurface(key, at); given != nullptr)
            return surfaceMemo.emplace(std::pair{key, at}, std::move(given)).first->second.get();
        // Air or rock all round first: nothing to find, and nothing of the
        // chunks' lazily kept digests touched for it.
        if (plainAround(from, key)) {
            surfaceMemo.emplace(std::pair{key, at}, nullptr);
            return nullptr;
        }
        std::shared_ptr<const SurfaceLevel> surface = asset::surfaceOf(from, key, at);
        if (surface == nullptr)
            surface = buildSurfaces(from, key, 1u << at)[at];
        return surfaceMemo.emplace(std::pair{key, at}, std::move(surface)).first->second.get();
    };
    const auto sampleIndex = [&](i32 sx, i32 sy, i32 sz) {
        return (static_cast<usize>(sz) * static_cast<usize>(sizeY) + static_cast<usize>(sy)) *
                   static_cast<usize>(sizeX) +
               static_cast<usize>(sx);
    };
    // Local sample coordinates throughout: `s = lattice - (min - 2)`.
    const auto occupancy = [&](i32 sx, i32 sy, i32 sz) {
        return static_cast<float>(samples[sampleIndex(sx, sy, sz)] & 0xFF) / static_cast<float>(FullOccupancy);
    };
    const auto materialAt = [&](i32 sx, i32 sy, i32 sz) {
        return static_cast<u8>(samples[sampleIndex(sx, sy, sz)] >> 8);
    };
    // A sample's paint (ADR 0114): `top` low, `cover` high; zero where nobody
    // painted, and everywhere in a region with none.
    const auto paintAt = [&](i32 sx, i32 sy, i32 sz) -> u16 {
        return paints.empty() ? u16{0} : paints[sampleIndex(sx, sy, sz)];
    };
    // The surface normal at a sample: the negative gradient of occupancy, by
    // central differences. Read from the field rather than from the triangles,
    // so it is smooth across them -- and the same in two neighbouring regions,
    // which read the same samples.
    const auto normalAt = [&](i32 sx, i32 sy, i32 sz) {
        return Vec3{occupancy(sx - 1, sy, sz) - occupancy(sx + 1, sy, sz),
                    occupancy(sx, sy - 1, sz) - occupancy(sx, sy + 1, sz),
                    occupancy(sx, sy, sz - 1) - occupancy(sx, sy, sz + 1)};
    };

    // **The column tops the sky term reads**, one per level column over the
    // region and far enough round it for the longest sky ray. Built once, where
    // a search per ray step per vertex would be most of a region's cost.
    //
    // A sky ray is followed `SkyReach` metres across, looking at the columns
    // `SkyStops` along it.
    const i32 margin = skyMargin(step);
    const i32 mapX0 = region.minX - margin;
    const i32 mapZ0 = region.minZ - margin;
    const i32 mapW = nx + 2 * margin;
    const i32 mapD = nz + 2 * margin;
    std::vector<float> tops(static_cast<usize>(mapW) * static_cast<usize>(mapD));
    std::vector<float> bottoms(tops.size());
    if (!drawn) {
        // Nothing reads the sky.
    }
    else if (level == 0) {
        for (i32 z = 0; z < mapD; ++z) {
            for (i32 x = 0; x < mapW; ++x) {
                const usize slot = static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x);
                tops[slot] = field.columnTop(mapX0 + x, mapZ0 + z).value_or(std::numeric_limits<float>::quiet_NaN());
                bottoms[slot] =
                    std::isnan(tops[slot])
                        ? tops[slot]
                        : field.columnBottom(mapX0 + x, mapZ0 + z).value_or(std::numeric_limits<float>::quiet_NaN());
            }
        }
    }
    else {
        // **This level's own columns** (ADR 0140, TA3): the highest and the
        // lowest of its own vertices over each, so the sky a vertex sees is
        // shaded by what this level draws -- and a feature it does not draw
        // shades nothing.
        std::fill(tops.begin(), tops.end(), std::numeric_limits<float>::quiet_NaN());
        std::fill(bottoms.begin(), bottoms.end(), std::numeric_limits<float>::quiet_NaN());
        const i32 levelEdge = static_cast<i32>(ChunkEdge) >> level;
        const double voxel = static_cast<double>(field.settings().voxelSize);
        const i32 span = 1 << level;
        for (const ChunkKey key : surfaceKeysOf(field, region)) {
            const SurfaceLevel* surface = surfaceIn(field, key, level);
            if (surface == nullptr)
                continue;
            for (const SurfaceCell& cell : surface->cells) {
                const i32 lx = cell.index % levelEdge;
                const i32 ly = (cell.index / levelEdge) % levelEdge;
                const i32 lz = cell.index / (levelEdge * levelEdge);
                const i32 x = key.x * levelEdge + lx - mapX0;
                const i32 z = key.z * levelEdge + lz - mapZ0;
                if (x < 0 || z < 0 || x >= mapW || z >= mapD || cell.count == 0)
                    continue;
                const auto y =
                    static_cast<float>((static_cast<double>(key.y * levelEdge + ly) * span +
                                        static_cast<double>(cell.offset[1] / static_cast<float>(cell.count)) + 0.5) *
                                       voxel);
                const usize slot = static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x);
                tops[slot] = std::isnan(tops[slot]) ? y : std::max(tops[slot], y);
                bottoms[slot] = std::isnan(bottoms[slot]) ? y : std::min(bottoms[slot], y);
            }
        }
    }
    const auto slotAt = [&](float x, float z) {
        const i32 kx = std::clamp(static_cast<i32>(std::floor(x / step)) - mapX0, 0, mapW - 1);
        const i32 kz = std::clamp(static_cast<i32>(std::floor(z / step)) - mapZ0, 0, mapD - 1);
        return static_cast<usize>(kz) * static_cast<usize>(mapW) + static_cast<usize>(kx);
    };
    // **Each column's solid runs, as many as it has, up to four** (ADR 0140,
    // TA11): air under an overhang is air to a sky ray, where the column's one
    // span from its bottom to its top counted it as rock. Built from the
    // gathered surface -- at this level, or at level 1 for level 0 -- by
    // walking each column's surfaces upwards: one facing down starts a run of
    // ground, one facing up ends it.
    constexpr usize MaxRuns = 4;
    struct Runs
    {
        std::array<float, MaxRuns * 2> spans{};
        u8 count = 0;
    };
    std::vector<Runs> runs(tops.size());
    if (drawn) {
        const u32 gathered = std::max<u32>(level, 1);
        const i32 levelEdge = static_cast<i32>(ChunkEdge) >> gathered;
        const double voxel = static_cast<double>(field.settings().voxelSize);
        const i32 share = 1 << (gathered - level);
        std::vector<std::vector<std::pair<float, float>>> surfaces(tops.size());
        MeshRegion wide = region;
        wide.level = gathered;
        wide.minX = floorDiv(region.minX, share);
        wide.minY = floorDiv(region.minY, share);
        wide.minZ = floorDiv(region.minZ, share);
        wide.cellsX = static_cast<u32>(floorDiv(region.minX + nx, share) - wide.minX + 1);
        wide.cellsY = static_cast<u32>(floorDiv(region.minY + ny, share) - wide.minY + 1);
        wide.cellsZ = static_cast<u32>(floorDiv(region.minZ + nz, share) - wide.minZ + 1);
        for (const ChunkKey key : surfaceKeysOf(field, wide)) {
            const SurfaceLevel* surface = surfaceIn(field, key, gathered);
            if (surface == nullptr)
                continue;
            for (const SurfaceCell& cell : surface->cells) {
                if (cell.count == 0)
                    continue;
                const i32 lx = cell.index % levelEdge;
                const i32 ly = (cell.index / levelEdge) % levelEdge;
                const i32 lz = cell.index / (levelEdge * levelEdge);
                const float inverse = 1.0f / static_cast<float>(cell.count);
                const auto y = static_cast<float>(
                    (static_cast<double>(static_cast<core::i64>(key.y * levelEdge + ly) << gathered) +
                     static_cast<double>(cell.offset[1] * inverse) + 0.5) *
                    voxel);
                const float facing = cell.normal[1] * inverse;
                // A gathered cell covers `share` of this level's columns a side.
                for (i32 dz = 0; dz < share; ++dz) {
                    for (i32 dx = 0; dx < share; ++dx) {
                        const i32 x = (key.x * levelEdge + lx) * share + dx - mapX0;
                        const i32 z = (key.z * levelEdge + lz) * share + dz - mapZ0;
                        if (x < 0 || z < 0 || x >= mapW || z >= mapD)
                            continue;
                        surfaces[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)].emplace_back(
                            y, facing);
                    }
                }
            }
        }
        for (usize at = 0; at < runs.size(); ++at) {
            const std::vector<std::pair<float, float>> found =
                terrainColumnRuns(std::move(surfaces[at]), tops[at], bottoms[at]);
            // The highest four: what a sky ray from above meets first.
            const usize first = found.size() > MaxRuns ? found.size() - MaxRuns : 0;
            for (usize run = first; run < found.size(); ++run) {
                runs[at].spans[runs[at].count * 2] = found[run].first;
                runs[at].spans[runs[at].count * 2 + 1] = found[run].second;
                runs[at].count += 1;
            }
        }
    }

    // **The highest top within a sky ray's reach of each column** -- a max
    // filter over the tops, one axis at a time. A point above it sees the whole
    // sky without marching a ray, and that is almost every point of open
    // ground: the rays are paid for on walls and in caves, where they matter.
    std::vector<float> nearTops(tops.size(), -std::numeric_limits<float>::infinity());
    if (drawn) {
        const i32 reach = static_cast<i32>(std::ceil(SkyReach / step)) + 1;
        std::vector<float> across(tops.size(), -std::numeric_limits<float>::infinity());
        std::vector<float> line;
        std::vector<float> window;
        for (i32 z = 0; z < mapD; ++z) {
            line.resize(static_cast<usize>(mapW));
            for (i32 x = 0; x < mapW; ++x) {
                const float top = tops[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)];
                line[static_cast<usize>(x)] = std::isnan(top) ? -std::numeric_limits<float>::infinity() : top;
            }
            slidingMax(line, reach, window);
            for (i32 x = 0; x < mapW; ++x)
                across[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)] =
                    window[static_cast<usize>(x)];
        }
        line.resize(static_cast<usize>(mapD));
        for (i32 x = 0; x < mapW; ++x) {
            for (i32 z = 0; z < mapD; ++z)
                line[static_cast<usize>(z)] =
                    across[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)];
            slidingMax(line, reach, window);
            for (i32 z = 0; z < mapD; ++z)
                nearTops[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)] =
                    window[static_cast<usize>(z)];
        }
    }

    // And the lowest bottom within reach, for the rays that go down: one below
    // it has passed under every column's ground it could meet.
    std::vector<float> nearBottoms(bottoms.size(), std::numeric_limits<float>::infinity());
    if (drawn) {
        const i32 reach = static_cast<i32>(std::ceil(SkyReach / step)) + 1;
        std::vector<float> across(bottoms.size(), std::numeric_limits<float>::infinity());
        for (i32 z = 0; z < mapD; ++z) {
            for (i32 x = 0; x < mapW; ++x) {
                float lowest = std::numeric_limits<float>::infinity();
                for (i32 k = std::max(x - reach, 0); k <= std::min(x + reach, mapW - 1); ++k) {
                    const float bottom =
                        bottoms[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(k)];
                    if (!std::isnan(bottom))
                        lowest = std::min(lowest, bottom);
                }
                across[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)] = lowest;
            }
        }
        for (i32 z = 0; z < mapD; ++z) {
            for (i32 x = 0; x < mapW; ++x) {
                float lowest = std::numeric_limits<float>::infinity();
                for (i32 k = std::max(z - reach, 0); k <= std::min(z + reach, mapD - 1); ++k)
                    lowest = std::min(lowest,
                                      across[static_cast<usize>(k) * static_cast<usize>(mapW) + static_cast<usize>(x)]);
                nearBottoms[static_cast<usize>(z) * static_cast<usize>(mapW) + static_cast<usize>(x)] = lowest;
            }
        }
    }

    // **How open a vertex is**, from 0 to 1: a fixed fan of rays over the
    // whole sphere, from a point lifted off the surface along its normal, each
    // marched across the column map and blocked where it passes between a
    // column's bottom and its top, and each weighed by how squarely the surface
    // faces it. So a surface counts only the half of the world in front of it,
    // and every surface is judged by one rule: open ground and an open wall see
    // all of what they face, the underside of the terrain sees the open air
    // below it, and deep in a tunnel every ray meets rock.
    //
    // **Rays, not the ground straight above** (the owner's picture): a ball on
    // the side of the terrain stood a dark stripe down the wall under it when
    // every point below a column's ground counted as under a roof. **And the
    // whole sphere, not only the sky** (the owner's second picture): the rim
    // under an overhang alternated between vertices facing up enough to count
    // the sky and vertices facing down that took a fixed half, and the triangles
    // between them drew a row of teeth. With every direction weighed the same
    // way, the answer turns smoothly as the normal does.
    //
    // A fixed pattern with written-out constants (no `std::cos`), so the same
    // field meshes to the same bytes on every platform.
    const auto skyVisibility = [&](Vec3 position, Vec3 normal) {
        struct Bearing
        {
            float x;
            float z;
        };
        // **Sixteen bearings** (ADR 0140, TA11): eight drew lobes along the
        // axes and the diagonals round anything tall.
        static constexpr float D = 0.70710678f;
        static constexpr float C = 0.92387953f;
        static constexpr float S = 0.38268343f;
        static constexpr std::array<Bearing, 16> Bearings{{
            {1.0f, 0.0f},
            {C, S},
            {D, D},
            {S, C},
            {0.0f, 1.0f},
            {-S, C},
            {-D, D},
            {-C, S},
            {-1.0f, 0.0f},
            {-C, -S},
            {-D, -D},
            {-S, -C},
            {0.0f, -1.0f},
            {S, -C},
            {D, -D},
            {C, -S},
        }};
        // Six rings, at 60, 30 and 10 degrees above the horizon and below it:
        // cosine, sine and rise per metre across.
        struct Ring
        {
            float across;
            float up;
            float rise;
        };
        static constexpr std::array<Ring, 6> Rings{{
            {0.5f, 0.8660254f, 1.7320508f},
            {0.8660254f, 0.5f, 0.5773503f},
            {0.9848078f, 0.1736482f, 0.1763270f},
            {0.9848078f, -0.1736482f, -0.1763270f},
            {0.8660254f, -0.5f, -0.5773503f},
            {0.5f, -0.8660254f, -1.7320508f},
        }};
        constexpr float Lift = 1.5f;
        const Vec3 from{position.x + normal.x * Lift, position.y + normal.y * Lift, position.z + normal.z * Lift};
        const float highest = nearTops[slotAt(from.x, from.z)];
        const float lowest = nearBottoms[slotAt(from.x, from.z)];
        // Facing up past every downward ring, above every top in reach: open,
        // without a ray. That is almost all open ground.
        if (highest <= from.y && normal.y >= 0.9848078f)
            return 1.0f;
        const Runs& own = runs[slotAt(from.x, from.z)];
        // Whether a run of ground meets the heights `[low, high]` a ray crossed
        // between two stops -- a plate thinner than the step between them is
        // still in the way.
        const auto blocks = [](const Runs& column, float low, float high) {
            for (u8 run = 0; run < column.count; ++run) {
                if (high >= column.spans[run * 2u] && low <= column.spans[run * 2u + 1u])
                    return true;
            }
            return false;
        };

        const auto facingOf = [&](Bearing bearing, const Ring& ring) {
            return std::max(
                bearing.x * ring.across * normal.x + ring.up * normal.y + bearing.z * ring.across * normal.z, 0.0f);
        };

        float seen = 0.0f;
        float total = 0.0f;
        // Straight up and straight down: open when nothing in this column is
        // on that side of the point.
        // **A run round the point itself is not a roof** (the owner's place,
        // 1 000 m): a column is a whole cell -- eight metres at level 3 -- and a
        // vertex on a ball fell inside the ball's own run, the whole ball as
        // that column sees it, or not, by where in the cell it was. Only ground
        // wholly above the point shades it from above, and wholly below it
        // from below.
        if (const float up = std::max(normal.y, 0.0f); up > 0.0f) {
            total += up;
            bool open = true;
            for (u8 run = 0; run < own.count; ++run)
                open = open && own.spans[run * 2u] <= from.y;
            if (open)
                seen += up;
        }
        if (const float down = std::max(-normal.y, 0.0f); down > 0.0f) {
            total += down;
            bool open = true;
            for (u8 run = 0; run < own.count; ++run)
                open = open && own.spans[run * 2u + 1u] >= from.y;
            if (open)
                seen += down;
        }
        // **One march per bearing, every ring at once**: a column is looked up
        // once per stop, and each ring still unsettled is checked against it --
        // blocked where it passes between the column's bottom and top, open once
        // it is above every top in reach going up or below every bottom going
        // down. In map cells, so a stop is an add and a truncation: the margin
        // keeps every stop inside the map and on its positive side.
        const float cellX = from.x / step - static_cast<float>(mapX0);
        const float cellZ = from.z / step - static_cast<float>(mapZ0);
        const float perCell = 1.0f / step;
        // **Nor does a ray meet the column it starts in** (the same place): on
        // a wall that column's run is the feature the wall bounds, and a ray
        // leaving the wall began inside it or not by where in the cell the
        // vertex was. What is above and below the point there is the straight
        // up and down, above.
        const i32 ownX = std::clamp(static_cast<i32>(cellX), 0, mapW - 1);
        const i32 ownZ = std::clamp(static_cast<i32>(cellZ), 0, mapD - 1);
        for (const Bearing& bearing : Bearings) {
            std::array<float, Rings.size()> weight{};
            std::array<bool, Rings.size()> settled{};
            int unsettled = 0;
            for (usize r = 0; r < Rings.size(); ++r) {
                weight[r] = facingOf(bearing, Rings[r]);
                settled[r] = weight[r] <= 0.0f;
                unsettled += settled[r] ? 0 : 1;
                total += weight[r];
            }
            for (usize at = 0; at < SkyStops.size() && unsettled > 0; ++at) {
                const float d = SkyStops[at];
                const i32 kx = std::clamp(static_cast<i32>(cellX + bearing.x * d * perCell), 0, mapW - 1);
                const i32 kz = std::clamp(static_cast<i32>(cellZ + bearing.z * d * perCell), 0, mapD - 1);
                if (kx == ownX && kz == ownZ)
                    continue;
                const Runs& column = runs[static_cast<usize>(kz) * static_cast<usize>(mapW) + static_cast<usize>(kx)];
                const float before = at == 0 ? 0.0f : SkyStops[at - 1];
                for (usize r = 0; r < Rings.size(); ++r) {
                    if (settled[r])
                        continue;
                    const float y = from.y + Rings[r].rise * d;
                    const float was = from.y + Rings[r].rise * before;
                    const bool rising = Rings[r].rise > 0.0f;
                    // Blocked on the way first; only a ray past everything
                    // in reach unhindered is open.
                    if (blocks(column, std::min(was, y), std::max(was, y))) {
                        settled[r] = true;
                        unsettled -= 1;
                    }
                    else if ((rising && y > highest) || (!rising && y < lowest)) {
                        settled[r] = true;
                        seen += weight[r];
                        unsettled -= 1;
                    }
                }
            }
            // A ray that met nothing within reach leaves.
            for (usize r = 0; r < Rings.size(); ++r) {
                if (!settled[r])
                    seen += weight[r];
            }
        }
        return total > 0.0f ? seen / total : 1.0f;
    };

    // --- Vertices: one per cell the surface passes through ---------------
    //
    // Cells `[min - 1, min + n)` on each axis, n + 1 of them; a cell's local
    // index `c` has its low corner at local sample `c + 1`.
    const i32 cellsX = nx + 1;
    const i32 cellsY = ny + 1;
    const i32 cellsZ = nz + 1;
    constexpr u32 NoVertex = 0xFFFFFFFFu;
    // A coarse level's cells start unseen: its vertices are made as its quads
    // reach them.
    constexpr u32 Unseen = 0xFFFFFFFEu;
    std::vector<u32> cellVertex(static_cast<usize>(cellsX) * static_cast<usize>(cellsY) * static_cast<usize>(cellsZ),
                                level == 0 ? NoVertex : Unseen);
    const auto cellIndex = [&](i32 x, i32 y, i32 z) {
        return (static_cast<usize>(z) * static_cast<usize>(cellsY) + static_cast<usize>(y)) *
                   static_cast<usize>(cellsX) +
               static_cast<usize>(x);
    };
    std::vector<u8> vertexMaterial;
    // What is painted over it, and how much (ADR 0114).
    std::vector<u8> vertexTop;
    std::vector<u8> vertexCover;

    // --- The geomorph's targets (ADR 0140) ------------------------------------
    //
    // **Where each vertex goes as its node gives way to its parent**: the mean
    // of the gathered cell one level up that holds it. The node that replaces
    // this one is then the geometry already drawn, and a change of level shows
    // nothing. Carried beside the vertices, as offsets.
    std::vector<Vec3> morphs;
    std::vector<u16> morphTags;
    // **Which seams a vertex of level `at`'s cell `(x, z)` sits on**: the
    // cells of that level straddling each of the region's sides, and its
    // level (`TerrainMesh::morphTags`).
    const auto tagOf = [&](u32 at, i32 x, i32 z) -> u16 {
        const i32 k = static_cast<i32>(at - level);
        u16 sides = 0;
        if (x == floorDiv(region.minX - 1, 1 << k))
            sides |= 1u;
        if (x == floorDiv(region.minX + nx - 1, 1 << k))
            sides |= 2u;
        if (z == floorDiv(region.minZ - 1, 1 << k))
            sides |= 4u;
        if (z == floorDiv(region.minZ + nz - 1, 1 << k))
            sides |= 8u;
        return static_cast<u16>(sides | (at << 4));
    };
    const auto clusterAt = [&](u32 at, i32 x, i32 y, i32 z) -> std::optional<Vec3> {
        if (at == 0 || at >= ChunkLevels)
            return std::nullopt;
        const i32 edgeAt = static_cast<i32>(ChunkEdge) >> at;
        const ChunkKey chunk{floorDiv(x, edgeAt), floorDiv(y, edgeAt), floorDiv(z, edgeAt)};
        const SurfaceLevel* surface = surfaceIn(field, chunk, at);
        const auto local = static_cast<u16>(((z - chunk.z * edgeAt) * edgeAt + (y - chunk.y * edgeAt)) * edgeAt +
                                            (x - chunk.x * edgeAt));
        const SurfaceCell* cell = surface != nullptr ? surface->cell(local) : nullptr;
        if (cell == nullptr || cell->count == 0)
            return std::nullopt;
        const double voxel = static_cast<double>(field.settings().voxelSize);
        const float inverse = 1.0f / static_cast<float>(cell->count);
        (void)inverse;
        const auto corner = [&](i32 v) { return static_cast<double>(static_cast<core::i64>(v) << at); };
        const std::array<float, 3> placed = cell->placed;
        return Vec3{static_cast<float>((corner(x) + static_cast<double>(placed[0]) + 0.5) * voxel),
                    static_cast<float>((corner(y) + static_cast<double>(placed[1]) + 0.5) * voxel),
                    static_cast<float>((corner(z) + static_cast<double>(placed[2]) + 0.5) * voxel)};
    };
    // A vertex of level `at`'s cell `(x, y, z)` at `position`: the offset to its
    // parent. None at the top level, which has no parent to give way to.
    const auto morphAt = [&](u32 at, i32 x, i32 y, i32 z, Vec3 position) {
        const std::optional<Vec3> parent = clusterAt(at + 1, floorDiv(x, 2), floorDiv(y, 2), floorDiv(z, 2));
        return parent.has_value() ? Vec3{parent->x - position.x, parent->y - position.y, parent->z - position.z}
                                  : Vec3{0.0f, 0.0f, 0.0f};
    };
    const auto morphFrom = [&](i32 x, i32 y, i32 z, Vec3 position) { return morphAt(level, x, y, z, position); };

    // --- Stitching to a coarser neighbour (ADR 0140) -------------------------
    //
    // **The level a cell is gathered at because a coarser node is drawn
    // beside it**, or zero: the cells that fall in the neighbour's own ring
    // cell along that side. Gathered at the neighbour's level, their vertex is
    // the neighbour's ring vertex -- the same mean of the same level-0 surface
    // -- so the two meshes share the seam's vertices. A corner in two bands
    // takes the coarser.
    const auto bandLevel = [&](i32 cx, i32 cz) -> u32 {
        u32 band = 0;
        const i32 x = region.minX - 1 + cx;
        const i32 z = region.minZ - 1 + cz;
        const i32 lowX = region.minX - 1;
        const i32 highX = region.minX + nx - 1;
        const i32 lowZ = region.minZ - 1;
        const i32 highZ = region.minZ + nz - 1;
        // Inside the neighbour's cell that straddles the seam, at its level.
        const auto within = [](i32 cell, i32 ring, i32 k) { return floorDiv(cell, 1 << k) == floorDiv(ring, 1 << k); };
        const auto side = [&](i32 cell, i32 ring, u8 neighbour) {
            if (neighbour > level && within(cell, ring, static_cast<i32>(neighbour - level)))
                band = std::max<u32>(band, neighbour);
        };
        const auto corner = [&](i32 ringX, i32 ringZ, u8 neighbour) {
            if (neighbour <= level)
                return;
            const auto k = static_cast<i32>(neighbour - level);
            if (within(x, ringX, k) && within(z, ringZ, k))
                band = std::max<u32>(band, neighbour);
        };
        side(x, lowX, region.sideLevels[0]);
        side(x, highX, region.sideLevels[1]);
        side(z, lowZ, region.sideLevels[2]);
        side(z, highZ, region.sideLevels[3]);
        corner(lowX, lowZ, region.sideLevels[4]);
        corner(highX, lowZ, region.sideLevels[5]);
        corner(lowX, highZ, region.sideLevels[6]);
        corner(highX, highZ, region.sideLevels[7]);
        return band;
    };
    std::map<std::array<i32, 4>, u32> stitched;
    const auto stitchedVertex = [&](u32 band, i32 cx, i32 cy, i32 cz) -> u32 {
        const i32 k = static_cast<i32>(band - level);
        const std::array<i32, 3> at{floorDiv(region.minX - 1 + cx, 1 << k), floorDiv(region.minY - 1 + cy, 1 << k),
                                    floorDiv(region.minZ - 1 + cz, 1 << k)};
        const std::array<i32, 4> key{static_cast<i32>(band), at[0], at[1], at[2]};
        if (const auto found = stitched.find(key); found != stitched.end())
            return found->second;
        const i32 bandEdge = static_cast<i32>(ChunkEdge) >> band;
        const ChunkKey chunk{floorDiv(at[0], bandEdge), floorDiv(at[1], bandEdge), floorDiv(at[2], bandEdge)};
        const SurfaceLevel* surface = surfaceIn(field, chunk, band);
        const auto local =
            static_cast<u16>(((at[2] - chunk.z * bandEdge) * bandEdge + (at[1] - chunk.y * bandEdge)) * bandEdge +
                             (at[0] - chunk.x * bandEdge));
        const SurfaceCell* cell = surface != nullptr ? surface->cell(local) : nullptr;
        if (cell == nullptr || cell->count == 0) {
            stitched.emplace(key, NoVertex);
            return NoVertex;
        }
        const double voxel = static_cast<double>(field.settings().voxelSize);
        const float inverse = 1.0f / static_cast<float>(cell->count);
        const auto corner = [&](int axis) {
            return static_cast<double>(static_cast<core::i64>(at[static_cast<usize>(axis)]) << band);
        };
        const std::array<float, 3> placed = cell->placed;
        Vertex vertex;
        vertex.position = Vec3{static_cast<float>((corner(0) + static_cast<double>(placed[0]) + 0.5) * voxel),
                               static_cast<float>((corner(1) + static_cast<double>(placed[1]) + 0.5) * voxel),
                               static_cast<float>((corner(2) + static_cast<double>(placed[2]) + 0.5) * voxel)};
        const float sx = cell->normal[0];
        const float sy = cell->normal[1];
        const float sz = cell->normal[2];
        const float length = std::sqrt(sx * sx + sy * sy + sz * sz);
        vertex.normal = length > 1e-6f ? Vec3{sx / length, sy / length, sz / length} : Vec3{0.0f, 1.0f, 0.0f};
        const u8 material = cell->material();
        vertex.tangent[0] = static_cast<float>(material);
        vertex.tangent[1] = drawn ? skyVisibility(vertex.position, vertex.normal) : 1.0f;
        vertex.tangent[2] = 0.0f;
        vertex.tangent[3] = 0.0f;
        const auto made = static_cast<u32>(out.mesh.vertices.size());
        out.mesh.vertices.push_back(vertex);
        // Towards its parent a level above the band, as the node it belongs to
        // slides it.
        morphs.push_back(drawn ? morphAt(band, at[0], at[1], at[2], vertex.position) : vertex.position);
        morphTags.push_back(tagOf(band, at[0], at[2]));
        out.colliderPoints.push_back(vertex.position);
        vertexMaterial.push_back(material);
        const bool painted = cell->topVotes > 0 && cell->top != material && cell->top != 0;
        vertexTop.push_back(painted ? cell->top : u8{0});
        vertexCover.push_back(
            painted ? static_cast<u8>(std::clamp<long>(std::lround(cell->paintCover() * inverse), 0, 255)) : u8{0});
        stitched.emplace(key, made);
        return made;
    };

    std::map<u8, std::vector<u32>> buckets;
    // Which level-0 vertices had no gradient to take a normal from.
    std::vector<bool> flatNormals;
    // The level-0 cells that hold more than one sheet, by cell: the vertex each
    // of their twelve edges takes. Looked up and never walked (R10).
    std::unordered_map<u32, std::array<u32, 12>> sheetCells;
    if (level == 0) {
        for (i32 cz = 0; cz < cellsZ; ++cz) {
            for (i32 cy = 0; cy < cellsY; ++cy) {
                for (i32 cx = 0; cx < cellsX; ++cx) {
                    std::array<float, 8> corner{};
                    int inside = 0;
                    for (int at = 0; at < 8; ++at) {
                        const auto& offset = CornerOffsets[static_cast<usize>(at)];
                        corner[static_cast<usize>(at)] =
                            occupancy(cx + 1 + offset[0], cy + 1 + offset[1], cz + 1 + offset[2]);
                        if (corner[static_cast<usize>(at)] >= 0.5f)
                            inside |= 1 << at;
                    }
                    if (inside == 0 || inside == 0xFF)
                        continue;

                    // **A vertex a sheet** (`cellSheets`): nearly every cell holds
                    // one, and its vertex is the mean of all its crossings as it
                    // always was; a cell two sheets pass through gets one each.
                    std::array<u8, 12> sheetOf{};
                    const int sheets = cellSheets(inside, corner, sheetOf);
                    std::array<Vec3, 4> sheetPosition{};
                    std::array<Vec3, 4> sheetNormal{};
                    std::array<bool, 4> sheetFlat{};
                    for (int sheet = 0; sheet < sheets && sheet < 4; ++sheet) {
                        float sumX = 0.0f;
                        float sumY = 0.0f;
                        float sumZ = 0.0f;
                        Vec3 normalSum{0.0f, 0.0f, 0.0f};
                        int crossings = 0;
                        for (usize edge = 0; edge < CellEdges.size(); ++edge) {
                            if (sheetOf[edge] != sheet)
                                continue;
                            const auto a = static_cast<usize>(CellEdges[edge][0]);
                            const auto b = static_cast<usize>(CellEdges[edge][1]);
                            const float t = crossingAt(corner[a], corner[b]);
                            const auto& oa = CornerOffsets[a];
                            const auto& ob = CornerOffsets[b];
                            sumX += static_cast<float>(oa[0]) + static_cast<float>(ob[0] - oa[0]) * t;
                            sumY += static_cast<float>(oa[1]) + static_cast<float>(ob[1] - oa[1]) * t;
                            sumZ += static_cast<float>(oa[2]) + static_cast<float>(ob[2] - oa[2]) * t;
                            const Vec3 na = normalAt(cx + 1 + oa[0], cy + 1 + oa[1], cz + 1 + oa[2]);
                            const Vec3 nb = normalAt(cx + 1 + ob[0], cy + 1 + ob[1], cz + 1 + ob[2]);
                            normalSum.x += na.x + (nb.x - na.x) * t;
                            normalSum.y += na.y + (nb.y - na.y) * t;
                            normalSum.z += na.z + (nb.z - na.z) * t;
                            ++crossings;
                        }
                        const float inverse = 1.0f / static_cast<float>(crossings);
                        // The cell's low corner is lattice point `min - 1 + c`,
                        // whose centre is half a step in.
                        sheetPosition[static_cast<usize>(sheet)] = Vec3{
                            (static_cast<float>(region.minX - 1 + cx) + sumX * inverse + 0.5f) * step,
                            (static_cast<float>(region.minY - 1 + cy) + sumY * inverse + 0.5f) * step,
                            (static_cast<float>(region.minZ - 1 + cz) + sumZ * inverse + 0.5f) * step,
                        };
                        const float normalLength = std::sqrt(normalSum.x * normalSum.x + normalSum.y * normalSum.y +
                                                             normalSum.z * normalSum.z);
                        sheetNormal[static_cast<usize>(sheet)] =
                            normalLength < 1e-8f ? Vec3{0.0f, 1.0f, 0.0f}
                                                 : Vec3{normalSum.x / normalLength, normalSum.y / normalLength,
                                                        normalSum.z / normalLength};
                        sheetFlat[static_cast<usize>(sheet)] = normalLength < 1e-8f;
                    }
                    const Vec3 position = sheetPosition[0];
                    const Vec3 normal = sheetNormal[0];
                    if (flatNormals.size() <= out.mesh.vertices.size())
                        flatNormals.resize(out.mesh.vertices.size() + 1, false);
                    flatNormals[out.mesh.vertices.size()] = sheetFlat[0];

                    // **The commonest material among the cell's SOLID corners**,
                    // lowest id on a tie. An air corner has no material.
                    std::array<u8, 8> seen{};
                    std::array<int, 8> votes{};
                    int kinds = 0;
                    for (int at = 0; at < 8; ++at) {
                        if ((inside & (1 << at)) == 0)
                            continue;
                        const auto& offset = CornerOffsets[static_cast<usize>(at)];
                        const u8 material = materialAt(cx + 1 + offset[0], cy + 1 + offset[1], cz + 1 + offset[2]);
                        int slot = 0;
                        while (slot < kinds && seen[static_cast<usize>(slot)] != material)
                            ++slot;
                        if (slot == kinds) {
                            seen[static_cast<usize>(kinds)] = material;
                            ++kinds;
                        }
                        ++votes[static_cast<usize>(slot)];
                    }
                    u8 material = seen[0];
                    int best = votes[0];
                    for (int slot = 1; slot < kinds; ++slot) {
                        const u8 candidate = seen[static_cast<usize>(slot)];
                        const int count = votes[static_cast<usize>(slot)];
                        if (count > best || (count == best && candidate < material)) {
                            material = candidate;
                            best = count;
                        }
                    }

                    // **What is painted over it, as a weight** (ADR 0114): the
                    // material painted most over the cell's solid corners, and how
                    // much of it shows across them -- a mean, not a vote, so a
                    // stroke fades across the ground rather than stepping a cell at
                    // a time. Lowest id on a tie.
                    u8 top = 0;
                    u8 cover = 0;
                    if (!paints.empty()) {
                        std::array<u8, 8> overs{};
                        std::array<int, 8> covers{};
                        int topKinds = 0;
                        int solid = 0;
                        std::array<u8, 8> bare{};
                        for (int at = 0; at < 8; ++at) {
                            if ((inside & (1 << at)) == 0)
                                continue;
                            ++solid;
                            const auto& offset = CornerOffsets[static_cast<usize>(at)];
                            const u16 painted = paintAt(cx + 1 + offset[0], cy + 1 + offset[1], cz + 1 + offset[2]);
                            const auto over = static_cast<u8>(painted & 0xFF);
                            if (over == 0 || (painted >> 8) == 0) {
                                bare[static_cast<usize>(at)] =
                                    materialAt(cx + 1 + offset[0], cy + 1 + offset[1], cz + 1 + offset[2]);
                                continue;
                            }
                            int slot = 0;
                            while (slot < topKinds && overs[static_cast<usize>(slot)] != over)
                                ++slot;
                            if (slot == topKinds) {
                                overs[static_cast<usize>(topKinds)] = over;
                                ++topKinds;
                            }
                            covers[static_cast<usize>(slot)] += painted >> 8;
                        }
                        coverWithOwnGround(bare, overs, covers, topKinds);
                        int most = 0;
                        for (int slot = 0; slot < topKinds; ++slot) {
                            const int sum = covers[static_cast<usize>(slot)];
                            const u8 candidate = overs[static_cast<usize>(slot)];
                            if (sum > most || (sum == most && candidate < top)) {
                                top = candidate;
                                most = sum;
                            }
                        }
                        if (top != 0 && solid > 0 && top != material)
                            cover = static_cast<u8>(std::min(255, (most + solid / 2) / solid));
                        else
                            top = 0;
                    }

                    Vertex vertex;
                    vertex.position = position;
                    vertex.normal = normal;
                    // The UVs carry the triangle's materials, not a position (ADR
                    // 0113): filled in below, once the triangles are known.
                    // **The material rides in the tangent's x and the sky in its
                    // y.** A terrain has no tangent frame of its own -- its shader
                    // builds one -- and `Vertex` is a GPU layout whose size is
                    // asserted, so this is where they fit without a second stream.
                    vertex.tangent[0] = static_cast<float>(material);
                    vertex.tangent[1] = drawn ? skyVisibility(position, normal) : 1.0f;
                    // The paint's two slots, filled with the triangle's three below.
                    vertex.tangent[2] = 0.0f;
                    vertex.tangent[3] = 0.0f;

                    cellVertex[cellIndex(cx, cy, cz)] = static_cast<u32>(out.mesh.vertices.size());
                    out.mesh.vertices.push_back(vertex);
                    morphs.push_back(
                        drawn ? morphFrom(region.minX - 1 + cx, region.minY - 1 + cy, region.minZ - 1 + cz, position)
                              : position);
                    morphTags.push_back(tagOf(level, region.minX - 1 + cx, region.minZ - 1 + cz));
                    out.colliderPoints.push_back(position);
                    vertexMaterial.push_back(material);
                    vertexTop.push_back(top);
                    vertexCover.push_back(cover);

                    // The cell's other sheets: the same ground, each where its
                    // own crossings put it, and which vertex each edge takes.
                    if (sheets > 1) {
                        std::array<u32, 12> byEdge{};
                        std::array<u32, 4> sheetVertex{};
                        sheetVertex[0] = cellVertex[cellIndex(cx, cy, cz)];
                        for (int sheet = 1; sheet < sheets && sheet < 4; ++sheet) {
                            const auto at = static_cast<usize>(sheet);
                            Vertex other = vertex;
                            other.position = sheetPosition[at];
                            other.normal = sheetNormal[at];
                            other.tangent[1] = drawn ? skyVisibility(other.position, other.normal) : 1.0f;
                            sheetVertex[at] = static_cast<u32>(out.mesh.vertices.size());
                            if (flatNormals.size() <= out.mesh.vertices.size())
                                flatNormals.resize(out.mesh.vertices.size() + 1, false);
                            flatNormals[out.mesh.vertices.size()] = sheetFlat[at];
                            out.mesh.vertices.push_back(other);
                            morphs.push_back(drawn ? morphFrom(region.minX - 1 + cx, region.minY - 1 + cy,
                                                               region.minZ - 1 + cz, other.position)
                                                   : other.position);
                            morphTags.push_back(tagOf(level, region.minX - 1 + cx, region.minZ - 1 + cz));
                            out.colliderPoints.push_back(other.position);
                            vertexMaterial.push_back(material);
                            vertexTop.push_back(top);
                            vertexCover.push_back(cover);
                        }
                        for (usize edge = 0; edge < 12; ++edge)
                            byEdge[edge] = sheetOf[edge] < 4 ? sheetVertex[sheetOf[edge]] : sheetVertex[0];
                        sheetCells.emplace(static_cast<u32>(cellIndex(cx, cy, cz)), byEdge);
                    }
                }
            }
        }
        // The cells beside a coarser neighbour take its vertices (stitching).
        for (i32 cz = 0; cz < cellsZ; ++cz) {
            for (i32 cx = 0; cx < cellsX; ++cx) {
                const u32 band = bandLevel(cx, cz);
                if (band == 0)
                    continue;
                for (i32 cy = 0; cy < cellsY; ++cy) {
                    u32& vertex = cellVertex[cellIndex(cx, cy, cz)];
                    if (vertex == NoVertex)
                        continue;
                    if (const u32 shared = stitchedVertex(band, cx, cy, cz); shared != NoVertex) {
                        vertex = shared;
                        // Every sheet of a stitched cell is the neighbour's vertex.
                        sheetCells.erase(static_cast<u32>(cellIndex(cx, cy, cz)));
                    }
                }
            }
        }

        // --- Quads: one round each crossed edge that starts on an owned point --
        // **Winding from the edge's sign** (ADR 0140, the mesh P2): a quad faces
        // the side of its edge the air is on, which the two ends of the edge say
        // exactly. It used to be judged against the vertices' normals, which a
        // thin feature has none of to speak of.
        std::vector<Vec3> faceSums;
        const auto emitTriangle = [&](u32 a, u32 second, u32 third) {
            if (a == second || second == third || a == third)
                return;
            const Vec3& pa = out.mesh.vertices[a].position;
            const Vec3& pb = out.mesh.vertices[second].position;
            const Vec3& pc = out.mesh.vertices[third].position;
            const Vec3 ab{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z};
            const Vec3 ac{pc.x - pa.x, pc.y - pa.y, pc.z - pa.z};
            const Vec3 face{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
            if (faceSums.size() < out.mesh.vertices.size())
                faceSums.resize(out.mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
            for (const u32 corner : {a, second, third}) {
                faceSums[corner].x += face.x;
                faceSums[corner].y += face.y;
                faceSums[corner].z += face.z;
            }

            // The majority of the three vertices' materials, lowest on a three-way
            // tie: a rule, so the visit order never reaches the mesh (R10).
            const u8 ma = vertexMaterial[a];
            const u8 mb = vertexMaterial[second];
            const u8 mc = vertexMaterial[third];
            u8 material = ma;
            if (mb == mc && mb != ma)
                material = mb;
            else if (ma != mb && ma != mc && mb != mc)
                material = std::min({ma, mb, mc});

            std::vector<u32>& bucket = buckets[material];
            bucket.push_back(a);
            bucket.push_back(second);
            bucket.push_back(third);
            out.colliderIndices.push_back(a);
            out.colliderIndices.push_back(second);
            out.colliderIndices.push_back(third);
        };
        // The four cells in the order that faces the edge's positive direction;
        // `reverse` faces it the other way.
        // The vertex of the cell `c` on the sheet that crosses the edge along
        // `axis` from owned point `o`: the cell's one vertex, or, where it
        // holds two sheets, the one that edge belongs to.
        const auto sheetVertexOf = [&](const std::array<i32, 3>& c, usize axis, const std::array<i32, 3>& o) {
            const usize seat = cellIndex(c[0], c[1], c[2]);
            const u32 vertex = cellVertex[seat];
            if (vertex == NoVertex || sheetCells.empty())
                return vertex;
            const auto split = sheetCells.find(static_cast<u32>(seat));
            if (split == sheetCells.end())
                return vertex;
            // The cell whose low corner is the point before `o` has local
            // index `o`, and the edge on its high side; the next, its low.
            const usize first = axis == 0 ? 1 : 0;
            const usize second = axis == 2 ? 1 : 2;
            const auto la = static_cast<usize>(1 - (c[first] - o[first]));
            const auto lb = static_cast<usize>(1 - (c[second] - o[second]));
            return split->second[EdgeAt[axis][la][lb]];
        };
        const auto quad = [&](std::array<i32, 3> c0, std::array<i32, 3> c1, std::array<i32, 3> c2,
                              std::array<i32, 3> c3, bool reverse, usize axis, std::array<i32, 3> o) {
            if (reverse)
                std::swap(c1, c3);
            const u32 a = sheetVertexOf(c0, axis, o);
            const u32 b = sheetVertexOf(c1, axis, o);
            const u32 c = sheetVertexOf(c2, axis, o);
            const u32 d = sheetVertexOf(c3, axis, o);
            if (a == NoVertex || b == NoVertex || c == NoVertex || d == NoVertex)
                return;
            // **Split along the shorter diagonal.** A quad whose four vertices
            // were pulled toward a sharp edge -- a ball set into a hole, the rim of
            // a cut -- is a long thin shape, and splitting it the long way gives
            // two slivers that shade as a streak (the owner's terrain report,
            // "stretched faces"; measured at an aspect of 28 before this). The
            // short way gives two sound triangles. **Near a tie, a-c always**
            // (ADR 0140, the mesh P2): along a crease the two diagonals of every
            // quad are about as long, and choosing each by a hair alternated
            // them into a saw; the fixed one draws the crease as one line. The
            // quad belongs to exactly one region, so every region splits it alike.
            const auto distance2 = [&](u32 p, u32 q) {
                const Vec3& from = out.mesh.vertices[p].position;
                const Vec3& to = out.mesh.vertices[q].position;
                const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
                return d.x * d.x + d.y * d.y + d.z * d.z;
            };
            if (distance2(b, d) < 0.8f * distance2(a, c)) {
                emitTriangle(a, b, d);
                emitTriangle(b, c, d);
            }
            else {
                emitTriangle(a, b, c);
                emitTriangle(a, c, d);
            }
        };
        // Owned point `p` (local `o` in [0, n)) is sample `o + 2`, and the cell
        // whose low corner is point `p - 1 + k` has local index `o + k`.
        //
        // **A band's points go second** (`MeshRegion::band`): the own ones,
        // then the ring's, keeping only its triangles that touch a point an
        // own one uses.
        const auto band = static_cast<i32>(region.band);
        const auto inRing = [&](i32 ox, i32 oy, i32 oz) {
            return ox < band || oy < band || oz < band || ox >= nx - band || oy >= ny - band || oz >= nz - band;
        };
        std::vector<bool> ownUse;
        for (int pass = 0; pass < (band > 0 ? 2 : 1); ++pass) {
            if (pass == 1) {
                out.colliderBandFirst = static_cast<u32>(out.colliderIndices.size() / 3);
                ownUse.assign(out.mesh.vertices.size(), false);
                for (const u32 v : out.colliderIndices)
                    ownUse[v] = true;
            }
            const usize passStart = out.colliderIndices.size();
            for (i32 oz = 0; oz < nz; ++oz) {
                for (i32 oy = 0; oy < ny; ++oy) {
                    for (i32 ox = 0; ox < nx; ++ox) {
                        if (band > 0 && inRing(ox, oy, oz) != (pass == 1))
                            continue;
                        const bool here = occupancy(ox + 2, oy + 2, oz + 2) >= 0.5f;
                        // Each order below faces its edge's positive direction -- the
                        // way the air is when the ground is at the edge's start, `here`.
                        // Along x: cells (x) by (y-1, y) by (z-1, z).
                        if (here != (occupancy(ox + 3, oy + 2, oz + 2) >= 0.5f))
                            quad({ox + 1, oy, oz}, {ox + 1, oy + 1, oz}, {ox + 1, oy + 1, oz + 1}, {ox + 1, oy, oz + 1},
                                 !here, 0, {ox, oy, oz});
                        // Along y: (x-1, x) by (y) by (z-1, z); this order faces down.
                        if (here != (occupancy(ox + 2, oy + 3, oz + 2) >= 0.5f))
                            quad({ox, oy + 1, oz}, {ox + 1, oy + 1, oz}, {ox + 1, oy + 1, oz + 1}, {ox, oy + 1, oz + 1},
                                 here, 1, {ox, oy, oz});
                        // Along z: (x-1, x) by (y-1, y) by (z).
                        if (here != (occupancy(ox + 2, oy + 2, oz + 3) >= 0.5f))
                            quad({ox, oy, oz + 1}, {ox + 1, oy, oz + 1}, {ox + 1, oy + 1, oz + 1}, {ox, oy + 1, oz + 1},
                                 !here, 2, {ox, oy, oz});
                    }
                }
            }
            if (pass == 1) {
                // The band's triangles that touch no own point lend no edge.
                usize kept = passStart;
                for (usize at = passStart; at + 2 < out.colliderIndices.size(); at += 3) {
                    const u32 a = out.colliderIndices[at];
                    const u32 b = out.colliderIndices[at + 1];
                    const u32 c = out.colliderIndices[at + 2];
                    const auto own = [&](u32 v) { return v < ownUse.size() && ownUse[v]; };
                    if (!own(a) && !own(b) && !own(c))
                        continue;
                    out.colliderIndices[kept] = a;
                    out.colliderIndices[kept + 1] = b;
                    out.colliderIndices[kept + 2] = c;
                    kept += 3;
                }
                out.colliderIndices.resize(kept);
            }
        }
        // **A vertex with no gradient takes its faces' normal** (the mesh P2): on
        // a feature a voxel thin, occupancy's two sides cancel, and straight up
        // lit a wall or an underside as a floor. **And one whose gradient points
        // against its own faces** (terrain-editing ledger, P3): beside a second
        // sheet a voxel away the gradient is the other sheet's as much as its
        // own, and a face lit from behind is a black triangle on the ground.
        for (usize index = 0; index < std::min(faceSums.size(), flatNormals.size()); ++index) {
            const Vec3 n = faceSums[index];
            const Vec3& own = out.mesh.vertices[index].normal;
            if (!flatNormals[index] && own.x * n.x + own.y * n.y + own.z * n.z >= 0.0f)
                continue;
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length > 1e-12f)
                out.mesh.vertices[index].normal = Vec3{n.x / length, n.y / length, n.z / length};
        }
    }
    // --- A coarse level: the level-0 surface, gathered (ADR 0140) ------------
    //
    // Each coarse cell the surface passes through is one vertex at the mean
    // of its level-0 vertices; each coarse lattice edge the surface crosses is
    // a quad round it, wound by which side the ground is on -- one each way
    // where a thin slab gathered its top and its bottom into one cell.
    else {
        const i32 levelEdge = static_cast<i32>(ChunkEdge) >> level;
        const i32 span = 1 << level;
        const double voxel = static_cast<double>(field.settings().voxelSize);
        const auto surfaceAt = [&](i32 x, i32 y, i32 z) -> std::pair<const SurfaceLevel*, u16> {
            const ChunkKey key{floorDiv(x, levelEdge), floorDiv(y, levelEdge), floorDiv(z, levelEdge)};
            const i32 lx = x - key.x * levelEdge;
            const i32 ly = y - key.y * levelEdge;
            const i32 lz = z - key.z * levelEdge;
            return {surfaceIn(field, key, level), static_cast<u16>((lz * levelEdge + ly) * levelEdge + lx)};
        };

        // A vertex per coarse cell and kind: one where the cell's level-0
        // normals agree -- a surface seen from one side -- and one per face
        // direction where they do not, so each side of a gathered slab is lit
        // as that side.
        //
        // **Looked up by cell**, four times a quad -- a tree and then a hash of
        // them were most of what meshing a node cost: a one-sided cell's
        // vertex, or its having none, in `cellVertex`; a two-sided cell's, by
        // kind, in `sides`, which is few.
        constexpr u32 TwoSided = 0xFFFFFFFDu;
        std::map<core::u64, u32> sides;
        // The largest error of a cell drawn, in level-0 voxels.
        float worst = 0.0f;
        std::vector<Vec3> faceNormals;
        const auto vertexOf = [&](i32 cx, i32 cy, i32 cz, u8 kind) -> u32 {
            if (const u32 band = bandLevel(cx, cz); band > 0)
                return stitchedVertex(band, cx, cy, cz);
            // **Made already**, which a corner almost always is.
            const usize seat = cellIndex(cx, cy, cz);
            const u32 known = cellVertex[seat];
            if (known != Unseen && known != TwoSided)
                return known;
            const core::u64 sideKey = static_cast<core::u64>(seat) * 8u + kind;
            if (known == TwoSided) {
                if (const auto found = sides.find(sideKey); found != sides.end())
                    return found->second;
            }
            const i32 x = region.minX - 1 + cx;
            const i32 y = region.minY - 1 + cy;
            const i32 z = region.minZ - 1 + cz;
            const auto [surface, index] = surfaceAt(x, y, z);
            const SurfaceCell* cell = surface != nullptr ? surface->cell(index) : nullptr;
            if (cell == nullptr || cell->count == 0) {
                cellVertex[seat] = NoVertex;
                return NoVertex;
            }
            const float inverse = 1.0f / static_cast<float>(cell->count);
            const Vec3 summed{cell->normal[0], cell->normal[1], cell->normal[2]};
            const float agreement =
                std::sqrt(summed.x * summed.x + summed.y * summed.y + summed.z * summed.z) * inverse;
            const bool oneSided = agreement >= 0.5f;
            Vertex vertex;
            const std::array<float, 3> placed = cell->placed;
            vertex.position = Vec3{
                static_cast<float>((static_cast<double>(x) * span + static_cast<double>(placed[0]) + 0.5) * voxel),
                static_cast<float>((static_cast<double>(y) * span + static_cast<double>(placed[1]) + 0.5) * voxel),
                static_cast<float>((static_cast<double>(z) * span + static_cast<double>(placed[2]) + 0.5) * voxel)};
            vertex.normal = oneSided ? Vec3{summed.x / (agreement * static_cast<float>(cell->count)),
                                            summed.y / (agreement * static_cast<float>(cell->count)),
                                            summed.z / (agreement * static_cast<float>(cell->count))}
                                     : Vec3{0.0f, 1.0f, 0.0f};
            const u8 material = cell->material();
            worst = std::max(worst, cell->deviation);
            vertex.tangent[0] = static_cast<float>(material);
            vertex.tangent[1] = 1.0f;
            vertex.tangent[2] = 0.0f;
            vertex.tangent[3] = 0.0f;
            const auto index32 = static_cast<u32>(out.mesh.vertices.size());
            out.mesh.vertices.push_back(vertex);
            morphs.push_back(drawn ? morphFrom(x, y, z, vertex.position) : vertex.position);
            morphTags.push_back(tagOf(level, x, z));
            out.colliderPoints.push_back(vertex.position);
            vertexMaterial.push_back(material);
            const bool painted = cell->topVotes > 0 && cell->top != material && cell->top != 0;
            vertexTop.push_back(painted ? cell->top : u8{0});
            vertexCover.push_back(
                painted ? static_cast<u8>(std::clamp(std::lround(cell->paintCover() * inverse), 0l, 255l)) : u8{0});
            faceNormals.push_back(Vec3{0.0f, 0.0f, 0.0f});
            if (oneSided) {
                cellVertex[seat] = index32;
            }
            else {
                cellVertex[seat] = TwoSided;
                sides.emplace(sideKey, index32);
            }
            return index32;
        };

        // The quads: round each coarse edge that starts on an owned point.
        // `(axis, u, v)` is right-handed, so the cells taken round the edge in
        // this order face `+axis`: the side the air is on when the ground is
        // low. Split along one diagonal always, so a crease is one straight
        // line and never a saw.
        const auto emit = [&](u32 a, u32 b, u32 c) {
            if (a == NoVertex || b == NoVertex || c == NoVertex || a == b || b == c || a == c)
                return;
            const u8 ma = vertexMaterial[a];
            const u8 mb = vertexMaterial[b];
            const u8 mc = vertexMaterial[c];
            u8 material = ma;
            if (mb == mc && mb != ma)
                material = mb;
            else if (ma != mb && ma != mc && mb != mc)
                material = std::min({ma, mb, mc});
            std::vector<u32>& bucket = buckets[material];
            bucket.insert(bucket.end(), {a, b, c});
            out.colliderIndices.insert(out.colliderIndices.end(), {a, b, c});
            const Vec3& pa = out.mesh.vertices[a].position;
            const Vec3& pb = out.mesh.vertices[b].position;
            const Vec3& pc = out.mesh.vertices[c].position;
            const Vec3 ab{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z};
            const Vec3 ac{pc.x - pa.x, pc.y - pa.y, pc.z - pa.z};
            const Vec3 face{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
            // Stitched vertices are made without a seat here.
            if (faceNormals.size() < out.mesh.vertices.size())
                faceNormals.resize(out.mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
            for (const u32 corner : {a, b, c}) {
                faceNormals[corner].x += face.x;
                faceNormals[corner].y += face.y;
                faceNormals[corner].z += face.z;
            }
        };
        // **The crossed edges, not every point** (ADR 0140): each chunk's
        // gathered edges are the ones the surface crosses, so the walk is as
        // long as the surface and not as the region's volume -- which, for a
        // tall one, was most of what meshing it cost. In the order the points
        // were walked before (z, then y, then x, then the axis), so the mesh
        // is the same bytes.
        struct Crossing
        {
            usize order = 0;
            i32 ox = 0;
            i32 oy = 0;
            i32 oz = 0;
            const SurfaceEdge* edge = nullptr;
        };
        std::vector<Crossing> crossings;
        for (i32 kz = floorDiv(region.minZ, levelEdge); kz <= floorDiv(region.minZ + nz - 1, levelEdge); ++kz) {
            for (i32 ky = floorDiv(region.minY, levelEdge); ky <= floorDiv(region.minY + ny - 1, levelEdge); ++ky) {
                for (i32 kx = floorDiv(region.minX, levelEdge); kx <= floorDiv(region.minX + nx - 1, levelEdge); ++kx) {
                    const SurfaceLevel* surface = surfaceIn(field, ChunkKey{kx, ky, kz}, level);
                    if (surface == nullptr)
                        continue;
                    for (const SurfaceEdge& edge : surface->edges) {
                        const i32 ox = kx * levelEdge + edge.index % levelEdge - region.minX;
                        const i32 oy = ky * levelEdge + (edge.index / levelEdge) % levelEdge - region.minY;
                        const i32 oz = kz * levelEdge + edge.index / (levelEdge * levelEdge) - region.minZ;
                        if (ox < 0 || ox >= nx || oy < 0 || oy >= ny || oz < 0 || oz >= nz)
                            continue;
                        const usize order =
                            ((static_cast<usize>(oz) * static_cast<usize>(ny) + static_cast<usize>(oy)) *
                                 static_cast<usize>(nx) +
                             static_cast<usize>(ox)) *
                                3u +
                            edge.axis;
                        crossings.push_back(Crossing{order, ox, oy, oz, &edge});
                    }
                }
            }
        }
        std::sort(crossings.begin(), crossings.end(),
                  [](const Crossing& a, const Crossing& b) { return a.order < b.order; });
        for (const Crossing& crossing : crossings) {
            const i32 ox = crossing.ox;
            const i32 oy = crossing.oy;
            const i32 oz = crossing.oz;
            const SurfaceEdge* crossed = crossing.edge;
            const u8 axis = crossed->axis;
            const u32 u = (axis + 1u) % 3u;
            const u32 v = (axis + 2u) % 3u;
            // Owned point `o` has cell-local index `o + 1`; the cells round
            // an edge are that less one on `u` and on `v`.
            const auto cellAt = [&](i32 du, i32 dv, u8 kind) {
                std::array<i32, 3> c{ox + 1, oy + 1, oz + 1};
                c[u] += du;
                c[v] += dv;
                return vertexOf(c[0], c[1], c[2], kind);
            };
            for (const bool up : {true, false}) {
                if ((up ? crossed->up : crossed->down) == 0)
                    continue;
                const auto kind = static_cast<u8>(1 + axis * 2 + (up ? 0 : 1));
                const u32 c0 = cellAt(-1, -1, kind);
                const u32 c1 = cellAt(0, -1, kind);
                const u32 c2 = cellAt(0, 0, kind);
                const u32 c3 = cellAt(-1, 0, kind);
                if (up) {
                    emit(c0, c1, c2);
                    emit(c0, c2, c3);
                }
                else {
                    emit(c0, c2, c1);
                    emit(c0, c3, c2);
                }
            }
        }
        // A two-sided cell's vertices, lit by the faces they belong to.
        for (const auto& entry : sides) {
            const Vec3 n = faceNormals[entry.second];
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length > 1e-12f)
                out.mesh.vertices[entry.second].normal = Vec3{n.x / length, n.y / length, n.z / length};
        }
        // The sky each vertex sees, against this level's own columns.
        for (Vertex& vertex : out.mesh.vertices)
            vertex.tangent[1] = drawn ? skyVisibility(vertex.position, vertex.normal) : 1.0f;
        out.error = worst * field.settings().voxelSize;
    }

    // --- The materials a pixel blends between (ADR 0113, 0114) --------------
    //
    // **Every vertex carries its triangle's three materials and which corner it
    // is**, so the shader can weigh the three layers by where in the triangle a
    // pixel is: `uv[0]` is the three ids packed as `a + b * 256 + c * 65536`
    // (exact in a float) and `uv[1]` the corner, 0 to 2. **And what is painted
    // over each corner**: the three tops packed the same way in the tangent's
    // z, and their three covers in its w. A vertex inside one material, painted
    // alike all round, says so three times and needs nothing more. A triangle
    // whose corners differ gets three vertices of its own -- only along the
    // seams and across a painted fade, so a field nobody painted costs what it
    // did.
    constexpr float PackB = 256.0f;
    constexpr float PackC = 65536.0f;
    const auto pack3 = [](u8 a, u8 b, u8 c) {
        return static_cast<float>(a) + static_cast<float>(b) * PackB + static_cast<float>(c) * PackC;
    };
    for (usize index = 0; index < out.mesh.vertices.size(); ++index) {
        out.mesh.vertices[index].uv[0] = pack3(vertexMaterial[index], vertexMaterial[index], vertexMaterial[index]);
        out.mesh.vertices[index].uv[1] = 0.0f;
        out.mesh.vertices[index].tangent[2] = pack3(vertexTop[index], vertexTop[index], vertexTop[index]);
        out.mesh.vertices[index].tangent[3] = pack3(vertexCover[index], vertexCover[index], vertexCover[index]);
    }
    const auto separate = [&](std::vector<u32>& list) {
        for (usize at = 0; at + 2 < list.size(); at += 3) {
            const u32 a = list[at];
            const u32 b = list[at + 1];
            const u32 c = list[at + 2];
            const u8 ma = vertexMaterial[a];
            const u8 mb = vertexMaterial[b];
            const u8 mc = vertexMaterial[c];
            const bool sameGround = ma == mb && mb == mc;
            const bool samePaint = vertexTop[a] == vertexTop[b] && vertexTop[b] == vertexTop[c] &&
                                   vertexCover[a] == vertexCover[b] && vertexCover[b] == vertexCover[c];
            if (sameGround && samePaint)
                continue;
            const float packed = pack3(ma, mb, mc);
            const float overs = pack3(vertexTop[a], vertexTop[b], vertexTop[c]);
            const float covers = pack3(vertexCover[a], vertexCover[b], vertexCover[c]);
            for (usize corner = 0; corner < 3; ++corner) {
                Vertex copy = out.mesh.vertices[list[at + corner]];
                copy.uv[0] = packed;
                copy.uv[1] = static_cast<float>(corner);
                copy.tangent[2] = overs;
                copy.tangent[3] = covers;
                const u32 source = list[at + corner];
                list[at + corner] = static_cast<u32>(out.mesh.vertices.size());
                out.mesh.vertices.push_back(copy);
                morphs.push_back(morphs[source]);
                morphTags.push_back(morphTags[source]);
            }
        }
    };
    for (auto& entry : buckets)
        separate(entry.second);

    if (!out.mesh.vertices.empty()) {
        Vec3 min = out.mesh.vertices.front().position;
        Vec3 max = min;
        for (const Vertex& vertex : out.mesh.vertices) {
            min.x = std::min(min.x, vertex.position.x);
            min.y = std::min(min.y, vertex.position.y);
            min.z = std::min(min.z, vertex.position.z);
            max.x = std::max(max.x, vertex.position.x);
            max.y = std::max(max.y, vertex.position.y);
            max.z = std::max(max.z, vertex.position.z);
        }
        out.mesh.bounds = core::AABB{min, max};
    }

    // **One section per material, in id order** -- a `std::map`, because the
    // section order reaches a GPU buffer and a world hash (R10).
    for (const auto& entry : buckets) {
        if (entry.second.empty())
            continue;
        Submesh section;
        section.firstIndex = static_cast<u32>(out.mesh.indices.size());
        section.indexCount = static_cast<u32>(entry.second.size());
        out.mesh.indices.insert(out.mesh.indices.end(), entry.second.begin(), entry.second.end());
        out.mesh.submeshes.push_back(section);
        out.sectionMaterials.push_back(entry.first);
    }
    out.morphs = std::move(morphs);
    out.morphTags = std::move(morphTags);
    return out;
}

TerrainCollider meshCollider(const TerrainField& field, ChunkKey key)
{
    // One mesh of the chunk grown by a point on every side, its own quads
    // first and the band's after: the points are the mesh's, so an own and a
    // band triangle meeting at the border share them by index already.
    MeshRegion region;
    region.minX = key.x * static_cast<i32>(ChunkEdge) - 1;
    region.minY = key.y * static_cast<i32>(ChunkEdge) - 1;
    region.minZ = key.z * static_cast<i32>(ChunkEdge) - 1;
    region.cellsX = ChunkEdge + 2;
    region.cellsY = ChunkEdge + 2;
    region.cellsZ = ChunkEdge + 2;
    region.collider = true;
    region.band = 1;
    TerrainMesh meshed = meshField(field, region);
    TerrainCollider out;
    if (meshed.colliderBandFirst == 0 || meshed.colliderIndices.size() < 3)
        return out;
    out.points = std::move(meshed.colliderPoints);
    out.indices = std::move(meshed.colliderIndices);
    out.bandFirst = meshed.colliderBandFirst;
    return out;
}

} // namespace engine::asset
