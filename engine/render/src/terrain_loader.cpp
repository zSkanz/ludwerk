#include "engine/render/terrain_loader.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>

#include "engine/asset/field_cells.h"
#include "engine/asset/terrain_layers.h"
#include "engine/asset/terrain_mesher.h"
#include "engine/asset/terrain_palette.h"
#include "engine/asset/terrain_pyramid.h"
#include "engine/core/log.h"
#include "engine/jobs/jobs.h"

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// How many frames a node may go undrawn and unwanted before its mesh is let
// go. Long enough that looking away and back does not rebuild anything.
constexpr u64 EvictAfterFrames = 180;

// At most this many EDITED nodes rebuilt in one frame. An edit rebuilds every
// node it changed in the frame it lands (`sync`), and this is only the ceiling
// that keeps a brush the size of the world from stalling one frame for all of
// it; the rest follow in the next.
constexpr u32 MaxEditBuildsPerSync = 64;
// **An edit of at most this many nodes is built in the frame that finds it**
// (the owner: "everything in the terrain editor feels delayed"). A brush
// stamp changes a node and the eight round it, two rows of them on a seam:
// built off the main thread and put up six a frame, that was three frames
// before the ground under the brush moved. Built here, on every worker, it is
// none. More than this -- a brush the size of a hill, a script rewriting a
// field -- is built off the main thread as before: a frame is not held for it.
constexpr usize MaxEditBuildsInFrame = 24;
// **And of at most this much ground**, counted in finest nodes: a node a
// level coarser is four times the ground to gather and mesh. Sixteen is the
// nine nodes round a brush close up, or four a level out, or one two levels
// out. A count, never a clock.
constexpr u32 MaxEditGroundInFrame = 16;
// **An edit built off the main thread goes up this many meshes a frame**, not
// `UploadsPerSync`: none of it is drawn until all of it is up, and at six a
// frame a brush stamp of ten nodes was a frame late for that alone.
constexpr usize EditUploadsPerSync = 24;
// **How many meshes built off the main thread go up in one frame** (TA14): an
// upload is the one part of a build the frame pays for, and a batch of sixty
// at once was a hitch of its own. A count, never a clock.
constexpr usize UploadsPerSync = 6;
// **Nodes built from cells on disk, a batch** (ADR 0144): far ground, built
// beside the near ground on a lane of its own, never ahead of it.
constexpr u32 MaxFarBuildsPerSync = 32;
// How many nodes nobody has drawn for a while let their meshes go in one
// frame (ADR 0150): a count, never a clock.
constexpr u32 MaxReleasesPerSync = 16;
// The eight nodes round a node, in `MeshRegion::sideLevels`' order: the sides,
// low x, high x, low z, high z, then the corners.
constexpr std::array<std::array<i32, 2>, 8> TerrainNeighbours{
    {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
// How much further than its split distance a split node goes before it joins
// again (ADR 0140).
constexpr f64 SplitHysteresis = 1.25;

// Order-sensitive, which is what a key built from an ordered walk wants.
[[nodiscard]] u64 combine(u64 seed, u64 value) noexcept
{
    u64 z = seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

[[nodiscard]] i32 across(u32 level) noexcept
{
    return static_cast<i32>(1u << level);
}

// Walks the chunks of the columns `[x0, x1] x [z0, z1]`, inclusive, in key
// order: one search per x, then along the sorted list.
template <class Visit>
void forChunksIn(const asset::TerrainField& field, i32 x0, i32 x1, i32 z0, i32 z1, Visit&& visit)
{
    const std::span<const asset::TerrainField::Entry> chunks = field.chunks();
    for (i32 x = x0; x <= x1; ++x) {
        const asset::ChunkKey low{x, std::numeric_limits<i32>::min(), z0};
        auto at = std::lower_bound(
            chunks.begin(), chunks.end(), low,
            [](const asset::TerrainField::Entry& entry, const asset::ChunkKey& probe) { return entry.first < probe; });
        for (; at != chunks.end() && at->first.x == x && at->first.z <= z1; ++at)
            visit(*at);
    }
}

// **Whether a node can have anything to draw**: a chunk in its footprint or in
// the column just past its high sides, whose surface against this node's air is
// still this node's to mesh.
[[nodiscard]] bool occupied(const asset::TerrainField& field, TerrainNodeKey key) noexcept
{
    // **The first chunk found answers**: walked to the end, a top-level node
    // over resident ground was a thousand columns' chunks, asked of every
    // node the selection passed and of its children, every frame.
    const i32 n = across(key.level);
    const std::span<const asset::TerrainField::Entry> chunks = field.chunks();
    for (i32 x = key.x * n; x <= key.x * n + n; ++x) {
        const asset::ChunkKey low{x, std::numeric_limits<i32>::min(), key.z * n};
        const auto at = std::lower_bound(
            chunks.begin(), chunks.end(), low,
            [](const asset::TerrainField::Entry& entry, const asset::ChunkKey& probe) { return entry.first < probe; });
        if (at != chunks.end() && at->first.x == x && at->first.z <= key.z * n + n)
            return true;
    }
    return false;
}

// **The chunk columns a node's mesh reads**, on one axis, inclusive: its own,
// and as far past each side as the mesher reaches (`asset::meshReach`) -- one
// column at full detail, two at the coarsest, more for small voxels. The key a
// node is rebuilt by, and what is prepared before meshing in parallel.
struct ChunkSpan
{
    i32 low = 0;
    i32 high = -1;
};

[[nodiscard]] ChunkSpan readSpan(const asset::TerrainField& field, TerrainNodeKey key, i32 index) noexcept
{
    const i32 reach = asset::meshReach(field.settings(), key.level);
    const auto cells = static_cast<i32>(asset::ChunkEdge);
    const auto scale = static_cast<i32>(1u << key.level);
    const i32 first = index * cells - reach;
    const i32 last = index * cells + cells - 1 + reach;
    return ChunkSpan{asset::floorDiv(first * scale, cells), asset::floorDiv((last + 1) * scale - 1, cells)};
}

// What a node's mesh reads (`readSpan`), **whole**.
//
// The ring used to count at full detail only by the two layers of voxels that
// face the node, which is all the triangles read. The openness baked into every
// vertex reads further: the tops and bottoms of the columns up to 12 m round it
// (`terrain_mesher.cpp`). So an edit a few metres into the next column changed
// what a node's vertices should look like and left the node as it was -- dark
// where the ground had since gone, bright where it had since come -- until
// something else rebuilt it: the owner's "the shadows go wrong while I edit".
// Whole costs the eight nodes round an edit a rebuild, which `sync` does in
// parallel and in the same frame.
[[nodiscard]] u64 contentOf(const asset::TerrainField& field, TerrainNodeKey key) noexcept
{
    const ChunkSpan xs = readSpan(field, key, key.x);
    const ChunkSpan zs = readSpan(field, key, key.z);
    u64 content = combine(combine(key.level, static_cast<u64>(static_cast<u32>(key.x))),
                          static_cast<u64>(static_cast<u32>(key.z)));
    forChunksIn(field, xs.low, xs.high, zs.low, zs.high, [&](const asset::TerrainField::Entry& entry) {
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.x)));
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.y)));
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.z)));
        content = combine(content, entry.second->digest());
    });
    return content;
}

// **Whether a cell's ground is resident**: any chunk of its columns in the
// field. A streamed cell comes in whole (terrain audit TA16a).
[[nodiscard]] bool cellResident(const asset::TerrainField& field, asset::ChunkId cell, i32 cellChunks) noexcept
{
    const std::span<const asset::TerrainField::Entry> chunks = field.chunks();
    const i32 lastZ = cell.z * cellChunks + cellChunks - 1;
    for (i32 x = cell.x * cellChunks; x < (cell.x + 1) * cellChunks; ++x) {
        const asset::ChunkKey low{x, std::numeric_limits<i32>::min(), cell.z * cellChunks};
        const auto at = std::lower_bound(
            chunks.begin(), chunks.end(), low,
            [](const asset::TerrainField::Entry& entry, const asset::ChunkKey& probe) { return entry.first < probe; });
        if (at != chunks.end() && at->first.x == x && at->first.z <= lastZ)
            return true;
    }
    return false;
}

// **What a node reads, wherever its ground is** (ADR 0144, 0150): each cell
// its span reaches by what the cell holds (`asset::terrainCellSignature`) --
// worked out from the field for a cell that is resident, and as the source
// knows it for one that is not, which is the same number while the cell is
// untouched. So a node is the same node before its cell comes in, while it is
// in, and after it is evicted; and naming it costs a walk of its cells, not of
// every chunk under it -- at 65 536 cells that walk, per node per frame, was
// the frame. Ground no cell accounts for -- written and not yet written out --
// is counted chunk by chunk. A cell never read makes the content one no build
// has, so the node is built.
[[nodiscard]] u64 contentOf(const asset::TerrainField& field, const asset::TerrainCellSource* source,
                            TerrainNodeKey key, bool fromField = false)
{
    if (source == nullptr)
        return contentOf(field, key);
    const ChunkSpan xs = readSpan(field, key, key.x);
    const ChunkSpan zs = readSpan(field, key, key.z);
    const auto n = static_cast<i32>(source->cellChunks());
    std::vector<std::pair<asset::ChunkId, u64>> cells;
    source->signaturesIn(xs.low, xs.high, zs.low, zs.high, cells);
    u64 content = combine(combine(key.level, static_cast<u64>(static_cast<u32>(key.x))),
                          static_cast<u64>(static_cast<u32>(key.z)));
    bool unread = false;
    for (const auto& [cell, known] : cells) {
        // `fromField`: a build's own field holds every cell it read, as
        // summaries, and names them by what it read.
        const u64 held = fromField || cellResident(field, cell, n)
                             ? asset::terrainCellSignature(field, cell.x, cell.z, source->cellChunks())
                             : known;
        if (held == 0) {
            unread = true;
            continue;
        }
        content = combine(content, static_cast<u64>(static_cast<u32>(cell.x)));
        content = combine(content, static_cast<u64>(static_cast<u32>(cell.z)));
        content = combine(content, held);
    }
    const auto inCell = [&](asset::ChunkKey chunk) {
        const std::pair<i32, i32> at{asset::floorDiv(chunk.x, n), asset::floorDiv(chunk.z, n)};
        const auto found = std::lower_bound(cells.begin(), cells.end(), at, [](const auto& entry, const auto& probe) {
            return entry.first.x != probe.first ? entry.first.x < probe.first : entry.first.z < probe.second;
        });
        return found != cells.end() && found->first.x == at.first && found->first.z == at.second;
    };
    forChunksIn(field, xs.low, xs.high, zs.low, zs.high, [&](const asset::TerrainField::Entry& entry) {
        if (inCell(entry.first))
            return;
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.x)));
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.y)));
        content = combine(content, static_cast<u64>(static_cast<u32>(entry.first.z)));
        content = combine(content, entry.second->digest());
    });
    return unread ? combine(content, 0xFA7F1E1Dull) : content;
}

// **Whether a node's ground is partly not resident** -- what it reads reaches a
// cell on disk -- so it is built from the cells (ADR 0144).
[[nodiscard]] bool readsCells(const asset::TerrainField& field, const asset::TerrainCellSource* source,
                              TerrainNodeKey key)
{
    if (source == nullptr)
        return false;
    const ChunkSpan xs = readSpan(field, key, key.x);
    const ChunkSpan zs = readSpan(field, key, key.z);
    const auto n = static_cast<i32>(source->cellChunks());
    std::vector<asset::ChunkId> cells;
    source->cellsIn(xs.low - 1, xs.high + 1, zs.low - 1, zs.high + 1, cells);
    for (const asset::ChunkId& cell : cells) {
        if (!cellResident(field, cell, n))
            return true;
    }
    return false;
}

// Registers a meshed node under its URN, one material per section tinted
// from the palette. Answers the handle, invalid when there was nothing.
// **A normal in two floats**, octahedral: the unit sphere folded onto a square.
[[nodiscard]] std::array<f32, 2> octahedral(core::Vec3 n) noexcept
{
    const f32 sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (!(sum > 0.0f))
        return {0.0f, 0.0f};
    f32 x = n.x / sum;
    f32 y = n.y / sum;
    if (n.z < 0.0f) {
        const f32 foldedX = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const f32 foldedY = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = foldedX;
        y = foldedY;
    }
    return {x, y};
}

// **The terrain's own reading of the 48-byte vertex** (ADR 0140), made here
// on the way to the GPU: every static mesh has the same layout, asserted, and
// the terrain needs three more floats for the geomorph's offset. The normal
// goes octahedral into x and y, freeing z; the tangent's x -- the vertex's own
// material, which no shader read -- goes; and the triangle corner's index,
// which was the UV's y, rides in the sky's float with the vertex's seams
// (`TerrainMesh::morphTags`) as `sky + 2 * corner + 8 * tag`, every part a
// whole number of steps a float holds exactly. So the offset is `(normal.z,
// tangent.x, uv.y)`. `engine_terrain_morph.hlsli` reads it so.
[[nodiscard]] asset::Mesh packedForGpu(const asset::TerrainMesh& meshed)
{
    asset::Mesh packed = meshed.mesh;
    for (usize at = 0; at < packed.vertices.size(); ++at) {
        asset::Vertex& vertex = packed.vertices[at];
        const core::Vec3 morph = at < meshed.morphs.size() ? meshed.morphs[at] : core::Vec3{0.0f, 0.0f, 0.0f};
        const std::array<f32, 2> normal = octahedral(vertex.normal);
        const f32 corner = vertex.uv[1];
        const auto tag = static_cast<f32>(at < meshed.morphTags.size() ? meshed.morphTags[at] : 0);
        vertex.normal = core::Vec3{normal[0], normal[1], morph.x};
        vertex.tangent[0] = morph.y;
        vertex.tangent[1] = std::clamp(vertex.tangent[1], 0.0f, 1.0f) + 2.0f * corner + 8.0f * tag;
        vertex.uv[1] = morph.z;
    }
    return packed;
}

// `packed` is `packedForGpu(meshed)`, when a worker made it already.
[[nodiscard]] MeshHandle upload(rhi::IDevice& device, rhi::ICmdList& cmd, MeshCache& cache, MeshLibrary& library,
                                core::NameAtom urn, const asset::TerrainMesh& meshed,
                                const asset::Mesh* packed = nullptr)
{
    if (meshed.mesh.indices.empty())
        return {};
    core::EngineError uploadError;
    const MeshHandle handle =
        cache.create(device, cmd, packed != nullptr ? *packed : packedForGpu(meshed), MeshUsage::Static, &uploadError);
    if (!handle.valid()) {
        core::logText(core::LogLevel::Warn, uploadError.message);
        return {};
    }
    MeshLibrary::Entry entry;
    entry.mesh = handle;
    entry.bounds = meshed.mesh.bounds;
    entry.sectionCount = static_cast<u32>(meshed.mesh.submeshes.size());
    entry.sectionMaterial.resize(entry.sectionCount);
    entry.materials.reserve(entry.sectionCount);
    for (u32 section = 0; section < entry.sectionCount; ++section) {
        const core::u8 materialId = section < meshed.sectionMaterials.size() ? meshed.sectionMaterials[section] : 0;
        const core::Vec3 tint = asset::terrainColorOf(materialId);
        RenderMaterial material;
        material.uniforms.baseColor[0] = tint.x;
        material.uniforms.baseColor[1] = tint.y;
        material.uniforms.baseColor[2] = tint.z;
        material.uniforms.baseColor[3] = 1.0f;
        material.uniforms.metallicRoughnessNormalCutoff[0] = 0.0f;
        material.uniforms.metallicRoughnessNormalCutoff[1] = 0.92f;
        entry.sectionMaterial[section] = section;
        entry.materials.push_back(material);
    }
    library.set(urn, std::move(entry));
    return handle;
}

// Whether `id` is `root` or under it.
[[nodiscard]] bool inWorld(const scene::World& world, core::InstanceId id, core::InstanceId root) noexcept
{
    for (core::InstanceId at = id; at.valid(); at = world.parentOf(at)) {
        if (at == root)
            return true;
    }
    return false;
}

} // namespace

std::string terrainNodeUrn(core::InstanceId terrain, TerrainNodeKey node)
{
    // The slot's generation too (terrain audit P6): a terrain destroyed and
    // made again in the same slot between two syncs filed its new node under
    // the old one's URN, and the old one's release then removed it.
    return "terrain://" + std::to_string(terrain.index) + "." + std::to_string(terrain.generation) + "/" +
           std::to_string(node.level) + "/" + std::to_string(node.x) + "," + std::to_string(node.z);
}

namespace {

// The regions a node is meshed as: one per run of rows that can hold a surface.
[[nodiscard]] std::vector<asset::MeshRegion> nodeRegions(const asset::TerrainField& field, TerrainNodeKey node,
                                                         TerrainSides sides)
{
    std::vector<asset::MeshRegion> regions;
    if (node.level > TerrainTopLevel)
        return regions;
    const i32 n = across(node.level);
    const i32 step = across(node.level);
    // **One region per run of rows that can hold a surface** (`activeRuns`):
    // the ground's top and the bottom of the terrain far under it are meshed,
    // and the solid rock between is not walked. Runs that meet at this level's
    // coarser rows are merged first, so no two regions own the same row.
    std::vector<std::pair<i32, i32>> runs;
    for (const auto& [low, high] : asset::activeRuns(field, node.x * n, node.z * n, n)) {
        const i32 first = asset::floorDiv(low, step);
        const i32 last = asset::floorDiv(high, step);
        if (!runs.empty() && first <= runs.back().second + 1)
            runs.back().second = std::max(runs.back().second, last);
        else
            runs.emplace_back(first, last);
    }
    for (const auto& [first, last] : runs) {
        asset::MeshRegion region;
        region.level = node.level;
        // A node is `n` chunks of 32 voxels, which at its own level is 32
        // voxels: every node, whatever its level, is 32 cells a side.
        region.minX = node.x * static_cast<i32>(asset::ChunkEdge);
        region.minZ = node.z * static_cast<i32>(asset::ChunkEdge);
        region.cellsX = asset::ChunkEdge;
        region.cellsZ = asset::ChunkEdge;
        region.minY = first;
        region.cellsY = static_cast<u32>(last - first + 1);
        // **Two of this level's cells**: enough to reach under the widest
        // crack a coarser neighbour can leave, and hidden in the ground.
        // **Stitched** (ADR 0140): along a side a coarser node is drawn
        // beside, the node takes that node's own vertices.
        region.sideLevels = sides;
        regions.push_back(region);
    }
    return regions;
}

} // namespace

asset::TerrainMesh meshTerrainNode(const asset::TerrainField& field, TerrainNodeKey node, TerrainSides sides)
{
    asset::TerrainMesh out;
    for (const asset::MeshRegion& region : nodeRegions(field, node, sides))
        asset::appendMesh(out, asset::meshField(field, region));
    return out;
}

void missingTerrainSurfaces(const asset::TerrainField& field, TerrainNodeKey node, TerrainSides sides,
                            std::vector<asset::SurfaceWant>& out)
{
    for (const asset::MeshRegion& region : nodeRegions(field, node, sides))
        asset::missingSurfaces(field, region, out);
}

void gatherTerrainSurfaces(std::vector<MissingSurface> missing)
{
    std::sort(missing.begin(), missing.end(), [](const MissingSurface& a, const MissingSurface& b) {
        return a.field != b.field ? std::less<>{}(a.field, b.field) : a.want.key < b.want.key;
    });
    struct Gather
    {
        MissingSurface at;
        u64 content = 0;
        asset::SurfaceLevels surfaces{};
    };
    // One per chunk, with every level any node asked of it.
    std::vector<Gather> gathers;
    for (const MissingSurface& next : missing) {
        if (!gathers.empty() && gathers.back().at.field == next.field && gathers.back().at.want.key == next.want.key)
            gathers.back().at.want.levels |= next.want.levels;
        else
            gathers.push_back(Gather{next, asset::surfaceContent(*next.field, next.want.key), {}});
    }
    jobs::parallelFor("terrain.surfaces", jobs::Domain::Render, 0, gathers.size(), 1,
                      [&gathers](usize begin, usize end, u32) noexcept {
                          for (usize at = begin; at < end; ++at)
                              gathers[at].surfaces = asset::buildSurfaces(
                                  *gathers[at].at.field, gathers[at].at.want.key, gathers[at].at.want.levels);
                      });
    for (const Gather& gather : gathers)
        asset::cacheSurfaces(*gather.at.field, gather.at.want.key, gather.content, gather.surfaces);
}

TerrainSides terrainStitchSides(std::span<const TerrainNodeKey> drawn, TerrainNodeKey key) noexcept
{
    const auto coarserAt = [drawn](TerrainNodeKey cell) -> core::u8 {
        for (u32 level = cell.level + 1; level <= TerrainTopLevel; ++level) {
            const i32 span = 1 << (level - cell.level);
            const TerrainNodeKey ancestor{level, asset::floorDiv(cell.x, span), asset::floorDiv(cell.z, span)};
            if (std::binary_search(drawn.begin(), drawn.end(), ancestor))
                return static_cast<core::u8>(level);
        }
        return 0;
    };
    TerrainSides sides{};
    for (usize at = 0; at < TerrainNeighbours.size(); ++at)
        sides[at] =
            coarserAt(TerrainNodeKey{key.level, key.x + TerrainNeighbours[at][0], key.z + TerrainNeighbours[at][1]});
    return sides;
}

TerrainMorph terrainMorphOf(std::span<const TerrainNodeKey> drawn, TerrainNodeKey key,
                            const std::function<std::array<f32, 2>(TerrainNodeKey)>& rangeOf)
{
    TerrainMorph morph;
    const std::array<f32, 2> own = rangeOf(key);
    morph.set(0, own[0], own[1], static_cast<f32>(key.level));
    for (usize at = 0; at < TerrainNeighbours.size(); ++at) {
        const TerrainNodeKey beside{key.level, key.x + TerrainNeighbours[at][0], key.z + TerrainNeighbours[at][1]};
        morph.set(at + 1, 0.0f, 0.0f, -1.0f);
        for (u32 level = key.level; level <= TerrainTopLevel; ++level) {
            const i32 span = 1 << (level - key.level);
            const TerrainNodeKey over{level, asset::floorDiv(beside.x, span), asset::floorDiv(beside.z, span)};
            if (std::binary_search(drawn.begin(), drawn.end(), over)) {
                const std::array<f32, 2> range = rangeOf(over);
                morph.set(at + 1, range[0], range[1], static_cast<f32>(level));
                break;
            }
        }
    }
    return morph;
}

core::Vec3 terrainSlid(core::Vec3 position, core::Vec3 offset, core::u16 tag, const TerrainMorph& morph,
                       f32 distance) noexcept
{
    const auto level = static_cast<f32>(tag >> 4);
    const u32 seams = tag & 15u;
    f32 start = 3.0e38f;
    f32 end = 3.0e38f;
    bool drawn = false;
    for (u32 row = 0; row < 9; ++row) {
        bool shares = row == 0;
        if (row >= 1 && row <= 4)
            shares = (seams & (1u << (row - 1u))) != 0u;
        else if (row == 5)
            shares = (seams & 5u) == 5u;
        else if (row == 6)
            shares = (seams & 6u) == 6u;
        else if (row == 7)
            shares = (seams & 9u) == 9u;
        else if (row == 8)
            shares = (seams & 10u) == 10u;
        if (shares && std::abs(morph.rows[row * 4 + 2] - level) < 0.5f) {
            start = std::min(start, morph.rows[row * 4]);
            end = std::min(end, morph.rows[row * 4 + 1]);
            drawn = true;
        }
    }
    if (!drawn || end <= start)
        return position;
    const f32 slide = std::clamp((distance - start) / (end - start), 0.0f, 1.0f);
    return core::Vec3{position.x + offset.x * slide, position.y + offset.y * slide, position.z + offset.z * slide};
}

TerrainLoader::Node* TerrainLoader::find(const scene::World* world, core::InstanceId terrain,
                                         TerrainNodeKey key) noexcept
{
    const auto& self = *this;
    return const_cast<Node*>(self.find(world, terrain, key));
}

const TerrainLoader::Node* TerrainLoader::find(const scene::World* world, core::InstanceId terrain,
                                               TerrainNodeKey key) const noexcept
{
    const auto found = m_nodes.find(indexOf(world, terrain, key));
    return found == m_nodes.end() ? nullptr : &found->second;
}

void TerrainLoader::countBuilt(const scene::World* world, core::InstanceId terrain, TerrainNodeKey key, bool built)
{
    for (u32 level = key.level + 1; level <= TerrainTopLevel; ++level) {
        const i32 span = 1 << (level - key.level);
        const TerrainNodeKey over{level, asset::floorDiv(key.x, span), asset::floorDiv(key.z, span)};
        if (built) {
            // Made here where it is not there yet: its own names are given
            // when the selection first asks for it (`nodeFor`).
            Node& node = m_nodes[indexOf(world, terrain, over)];
            if (node.world == nullptr) {
                node.world = world;
                node.terrain = terrain;
                node.key = over;
                node.used = m_frame;
            }
            node.builtBelow += 1;
        }
        else if (Node* node = find(world, terrain, over); node != nullptr && node->builtBelow > 0) {
            node->builtBelow -= 1;
        }
    }
}

void TerrainLoader::release(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library, Node& node)
{
    for (Variant& variant : node.variants) {
        if (variant.mesh.valid())
            cache.release(device, variant.mesh);
        if (variant.urn.valid())
            library.remove(variant.urn);
        variant.mesh = {};
    }
}

namespace {

// **A coarse node of ground that is partly on disk, from the far ground's
// files** (ADR 0150): the blocks its span reaches are brought up to date --
// which reads nothing when they are -- and the field it is meshed against is
// the resident one with a summary for every chunk that is not, and each
// chunk's gathered surface given to it as the files kept it. No cell is read
// and no surface gathered, whatever the node spans.
//
// **But for ground somebody has changed and not written out**: a resident
// cell that holds something else than its file -- a brush's, a script's -- is
// meshed from its voxels as it is now, and so are the columns beside it, whose
// surfaces read its border. Those are gathered here, with the cells round
// them read whole for the purpose; everything else comes from the files.
//
// Nothing when the files cannot give the node -- a block that will not build,
// a file gone missing: the caller builds it from the cells, as before.
[[nodiscard]] std::optional<TerrainNodeBuild> buildTerrainNodeFromPyramid(const asset::TerrainField& resident,
                                                                          const asset::TerrainCellSource& source,
                                                                          asset::TerrainPyramid& pyramid,
                                                                          TerrainNodeKey node, TerrainSides sides)
{
    asset::TerrainField field = resident;
    const auto n = static_cast<i32>(source.cellChunks());
    const ChunkSpan xs = readSpan(field, node, node.x);
    const ChunkSpan zs = readSpan(field, node, node.z);
    // Two columns past what the node reads at its own level: its seams are
    // gathered at the coarser levels beside it, whose cells reach further.
    const i32 x0 = xs.low - 2;
    const i32 x1 = xs.high + 2;
    const i32 z0 = zs.low - 2;
    const i32 z1 = zs.high + 2;
    const i32 block = across(TerrainTopLevel);
    {
        for (i32 blockZ = asset::floorDiv(z0, block); blockZ <= asset::floorDiv(z1, block); ++blockZ) {
            for (i32 blockX = asset::floorDiv(x0, block); blockX <= asset::floorDiv(x1, block); ++blockX) {
                if (!pyramid.ensure(source, blockX, blockZ))
                    return std::nullopt;
            }
        }
    }

    // The cells that are resident, and of those the ones that hold something
    // else than their file. Ground in no cell at all -- written, and not yet
    // written out -- is the same: only the field has it.
    std::vector<std::pair<asset::ChunkId, u64>> cells;
    source.signaturesIn(x0 - 1, x1 + 1, z0 - 1, z1 + 1, cells);
    std::set<std::pair<i32, i32>> here;
    std::set<std::pair<i32, i32>> changed;
    for (const auto& [cell, known] : cells) {
        if (!cellResident(resident, cell, n))
            continue;
        here.emplace(cell.x, cell.z);
        if (asset::terrainCellSignature(resident, cell.x, cell.z, source.cellChunks()) != known)
            changed.emplace(cell.x, cell.z);
    }
    const auto cellOf = [n](i32 chunkX, i32 chunkZ) {
        return std::pair{asset::floorDiv(chunkX, n), asset::floorDiv(chunkZ, n)};
    };
    {
        std::set<std::pair<i32, i32>> named;
        for (const auto& [cell, known] : cells)
            named.emplace(cell.x, cell.z);
        forChunksIn(resident, x0 - 1, x1 + 1, z0 - 1, z1 + 1, [&](const asset::TerrainField::Entry& entry) {
            const std::pair<i32, i32> cell = cellOf(entry.first.x, entry.first.z);
            if (!named.contains(cell)) {
                here.insert(cell);
                changed.insert(cell);
            }
        });
    }
    const auto dirty = [&](i32 chunkX, i32 chunkZ) {
        if (changed.empty())
            return false;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (changed.contains(cellOf(chunkX + dx, chunkZ + dz)))
                    return true;
            }
        }
        return false;
    };
    {
        if (!pyramid.seed(
                field, x0, x1, z0, z1, node.level,
                [&](i32 chunkX, i32 chunkZ) { return !here.contains(cellOf(chunkX, chunkZ)); },
                [&](i32 chunkX, i32 chunkZ) { return !dirty(chunkX, chunkZ); }))
            return std::nullopt;
    }
    // **Nothing is lacking where nothing was changed**: a block brought up to
    // date kept every chunk a surface could sit in, with what it gathered
    // there or with nothing, and the mesher knows a chunk further from the
    // ground than that has no surface without asking. So the asking -- every
    // chunk a node and its seams read, some thousands -- is done only round
    // ground somebody changed and has not written out.

    std::vector<asset::SurfaceWant> wants;
    const auto lacking = [&] {
        wants.clear();
        missingTerrainSurfaces(field, node, sides, wants);
        for (u32 level = node.level + 1; level <= TerrainTopLevel; ++level) {
            const auto band = static_cast<core::u8>(level);
            missingTerrainSurfaces(field, node, TerrainSides{band, band, band, band, band, band, band, band}, wants);
        }
        std::sort(wants.begin(), wants.end(),
                  [](const asset::SurfaceWant& a, const asset::SurfaceWant& b) { return a.key < b.key; });
        return !wants.empty();
    };
    if (!changed.empty() && lacking()) {
        // **A surface the files hold nothing of is a surface with nothing in
        // it**: a block brought up to date gathered every chunk a surface
        // could sit in, and kept those that had one.
        static const auto Nothing = std::make_shared<const asset::SurfaceLevel>();
        std::set<std::pair<i32, i32>> read;
        std::vector<asset::SurfaceWant> gather;
        for (const asset::SurfaceWant& want : wants) {
            if (!dirty(want.key.x, want.key.z)) {
                for (u32 level = 1; level < asset::ChunkLevels; ++level) {
                    if ((want.levels & (1u << level)) != 0)
                        field.adoptSurface(want.key, level, Nothing);
                }
                continue;
            }
            if (!gather.empty() && gather.back().key == want.key)
                gather.back().levels |= want.levels;
            else
                gather.push_back(want);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const std::pair<i32, i32> cell = cellOf(want.key.x + dx, want.key.z + dz);
                    if (!here.contains(cell))
                        read.insert(cell);
                }
            }
        }
        // The voxels round changed ground, where they are not resident.
        for (const auto& [cellX, cellZ] : read) {
            const std::optional<asset::TerrainCell> cell =
                source.read(asset::ChunkId{cellX, cellZ, asset::FieldLayerTerrain});
            if (!cell.has_value())
                continue;
            for (const asset::TerrainField::Entry& entry : cell->field.chunks())
                field.setChunk(entry.first, entry.second);
        }
        for (const asset::SurfaceWant& want : gather) {
            const asset::SurfaceLevels gathered = asset::buildSurfaces(field, want.key, want.levels);
            for (u32 level = 1; level < asset::ChunkLevels; ++level) {
                if ((want.levels & (1u << level)) != 0)
                    field.adoptSurface(want.key, level, gathered[level] != nullptr ? gathered[level] : Nothing);
            }
        }
    }

    TerrainNodeBuild out;
    {
        out.mesh = meshTerrainNode(field, node, sides);
    }
    out.content = contentOf(resident, &source, node);
    return out;
}

} // namespace

TerrainNodeBuild buildTerrainNodeFromCells(const asset::TerrainField& resident, const asset::TerrainCellSource& source,
                                           TerrainNodeKey node, TerrainSides sides)
{
    // **From the far ground's files, where the terrain has them** (ADR 0150):
    // a coarse node, which otherwise reads every cell under it.
    if (node.level >= asset::PyramidFirstLevel && source.pyramid() != nullptr) {
        if (std::optional<TerrainNodeBuild> built =
                buildTerrainNodeFromPyramid(resident, source, *source.pyramid(), node, sides);
            built.has_value())
            return std::move(*built);
    }
    asset::TerrainField field = resident;
    const auto n = static_cast<i32>(source.cellChunks());
    const ChunkSpan xs = readSpan(field, node, node.x);
    const ChunkSpan zs = readSpan(field, node, node.z);
    std::vector<asset::ChunkId> cells;
    source.cellsIn(xs.low - 1, xs.high + 1, zs.low - 1, zs.high + 1, cells);
    std::map<std::pair<i32, i32>, std::shared_ptr<const asset::TerrainCellSource::Summaries>> away;
    for (const asset::ChunkId& cell : cells) {
        if (cellResident(field, cell, n))
            continue;
        std::shared_ptr<const asset::TerrainCellSource::Summaries> summaries = source.summaries(cell);
        if (summaries == nullptr)
            continue;
        for (const asset::TerrainField::Entry& entry : *summaries)
            field.setChunk(entry.first, entry.second);
        away.emplace(std::pair{cell.x, cell.z}, std::move(summaries));
    }

    // What the node and its seams at every coarser level read and the cache
    // lacks, by the row of cells each chunk is in.
    std::vector<asset::SurfaceWant> wants;
    const auto lacking = [&] {
        wants.clear();
        missingTerrainSurfaces(field, node, sides, wants);
        for (u32 level = node.level + 1; level <= TerrainTopLevel; ++level) {
            const auto band = static_cast<core::u8>(level);
            missingTerrainSurfaces(field, node, TerrainSides{band, band, band, band, band, band, band, band}, wants);
        }
        std::sort(wants.begin(), wants.end(), [n](const asset::SurfaceWant& a, const asset::SurfaceWant& b) {
            const i32 rowA = asset::floorDiv(a.key.z, n);
            const i32 rowB = asset::floorDiv(b.key.z, n);
            return rowA != rowB ? rowA < rowB : a.key < b.key;
        });
        return !wants.empty();
    };

    // The cells read whole, by row: put in, and later swapped back for
    // their summaries.
    std::map<i32, std::vector<std::pair<i32, i32>>> whole;
    // Only the cells round what is lacking: most of a far node's surfaces are
    // cached by the nodes built before it.
    i32 lowCell = 0;
    i32 highCell = -1;
    const auto readRow = [&](i32 row) {
        std::vector<std::pair<i32, i32>>& held = whole[row];
        for (const auto& [at, summaries] : away) {
            if (at.second != row || at.first < lowCell || at.first > highCell ||
                std::find(held.begin(), held.end(), at) != held.end())
                continue;
            const std::optional<asset::TerrainCell> cell =
                source.read(asset::ChunkId{at.first, at.second, asset::FieldLayerTerrain});
            if (!cell.has_value())
                continue;
            for (const asset::TerrainField::Entry& entry : cell->field.chunks())
                field.setChunk(entry.first, entry.second);
            held.push_back(at);
        }
    };
    const auto letGo = [&](i32 below) {
        for (auto row = whole.begin(); row != whole.end() && row->first < below;) {
            for (const auto& at : row->second) {
                for (const asset::TerrainField::Entry& entry : *away.at(at))
                    field.setChunk(entry.first, entry.second);
            }
            row = whole.erase(row);
        }
    };
    // **Gathered until nothing is lacking**: a surface no gather here put in
    // would be gathered by the mesher from a summary, which has no voxels to
    // gather -- a hole. Another build can put a chunk's surface under other
    // content between this gather and the mesh (`SurfaceContentsKept`), so
    // what is lacking is asked again; a second round is rare, a third rarer.
    for (int round = 0; round < 3 && lacking(); ++round) {
        // One gather a chunk, with every level asked of it.
        std::vector<asset::SurfaceWant> merged;
        for (const asset::SurfaceWant& want : wants) {
            if (!merged.empty() && merged.back().key == want.key)
                merged.back().levels |= want.levels;
            else
                merged.push_back(want);
        }
        for (usize at = 0; at < merged.size();) {
            const i32 row = asset::floorDiv(merged[at].key.z, n);
            lowCell = std::numeric_limits<i32>::max();
            highCell = std::numeric_limits<i32>::min();
            for (usize next = at; next < merged.size() && asset::floorDiv(merged[next].key.z, n) == row; ++next) {
                lowCell = std::min(lowCell, asset::floorDiv(merged[next].key.x - 1, n));
                highCell = std::max(highCell, asset::floorDiv(merged[next].key.x + 1, n));
            }
            letGo(row - 1);
            readRow(row - 1);
            readRow(row);
            readRow(row + 1);
            for (; at < merged.size() && asset::floorDiv(merged[at].key.z, n) == row; ++at) {
                const asset::SurfaceWant& want = merged[at];
                const u64 content = asset::surfaceContent(field, want.key);
                asset::cacheSurfaces(field, want.key, content, asset::buildSurfaces(field, want.key, want.levels));
            }
        }
        letGo(std::numeric_limits<i32>::max());
    }

    TerrainNodeBuild out;
    out.mesh = meshTerrainNode(field, node, sides);
    out.content = contentOf(field, &source, node, true);
    return out;
}

bool terrainNodeReadsCells(const asset::TerrainField& resident, const asset::TerrainCellSource* source,
                           TerrainNodeKey node)
{
    return readsCells(resident, source, node);
}

u64 terrainNodeContent(const asset::TerrainField& resident, const asset::TerrainCellSource* source, TerrainNodeKey node)
{
    return contentOf(resident, source, node);
}

// **A batch of nodes built off the main thread** (TA14). Each field is a
// snapshot: its chunks are shared with the live one, which clones a chunk
// before it writes to one another snapshot holds, so what a worker reads never
// changes under it.
struct TerrainLoader::Batch
{
    struct Item
    {
        const scene::World* world = nullptr;
        core::InstanceId terrain;
        TerrainNodeKey key;
        TerrainSides sides{};
        u64 revision = 0;
        // The edits the ground had had at that revision, and where the
        // terrain's cells stood (ADR 0150).
        u64 edits = 0;
        u64 cellsRevision = 0;
        std::shared_ptr<const asset::TerrainField> field;
        // A node whose ground is partly on disk, and where it is (ADR 0144).
        std::shared_ptr<const asset::TerrainCellSource> source;
        // The terrain's cells, whether or not this node reads any: what its
        // content is named by (ADR 0150).
        std::shared_ptr<const asset::TerrainCellSource> cells;
        bool far = false;
        asset::TerrainMesh mesh;
        // The same, in the GPU's layout: made here rather than on the frame.
        asset::Mesh packed;
        u64 content = 0;
    };
    std::vector<Item> items;
    std::vector<jobs::JobHandle> lanes;
    u32 laneCount = 1;
    // Whether a node's build also gathers the surfaces its seams would read
    // at every coarser level. Off the main thread it does, so a later change
    // of level only meshes; a batch a frame is waiting on does not.
    bool seamsAhead = true;
    // Whether it holds a node rebuilt for an edit: somebody is watching it.
    bool edit = false;
    // The next to put up: a batch goes up over a few frames, a few meshes a
    // frame (`UploadsPerSync`).
    usize next = 0;

    // One lane's share, in order: the surfaces its nodes read, gathered and
    // cached, then the nodes. A few lanes, never every worker: the frame's own
    // work runs beside them.
    void run(u32 lane) noexcept
    {
        std::vector<asset::SurfaceWant> wants;
        for (usize at = lane; at < items.size(); at += laneCount) {
            Item& item = items[at];
            if (item.far) {
                buildFar(item);
                continue;
            }
            wants.clear();
            missingTerrainSurfaces(*item.field, item.key, item.sides, wants);
            for (const asset::SurfaceWant& want : wants) {
                const u64 content = asset::surfaceContent(*item.field, want.key);
                asset::cacheSurfaces(*item.field, want.key, content,
                                     asset::buildSurfaces(*item.field, want.key, want.levels));
            }
            item.mesh = meshTerrainNode(*item.field, item.key, item.sides);
            item.packed = packedForGpu(item.mesh);
            item.content = contentOf(*item.field, item.cells.get(), item.key);
            // **And the surfaces its seams would read**, at every coarser
            // level: when the levels beside it change, the rebuild that must
            // happen in that frame then only meshes, and reads no voxel.
            for (u32 level = item.key.level + 1; seamsAhead && level <= TerrainTopLevel; ++level) {
                const auto band = static_cast<core::u8>(level);
                wants.clear();
                missingTerrainSurfaces(*item.field, item.key,
                                       TerrainSides{band, band, band, band, band, band, band, band}, wants);
                for (const asset::SurfaceWant& want : wants) {
                    const u64 content = asset::surfaceContent(*item.field, want.key);
                    asset::cacheSurfaces(*item.field, want.key, content,
                                         asset::buildSurfaces(*item.field, want.key, want.levels));
                }
            }
        }
    }

    // **A node built from the cells its ground is in** (ADR 0144), memory held
    // to three rows of cells whatever the node spans. The field it is meshed
    // against is the resident one with every cell it reads that is not
    // resident put in as summaries -- what a coarse mesh reads of a chunk once
    // its surfaces are gathered. The surfaces are gathered a row of cells at a
    // time, with the cells round that row read whole and then let go, under
    // the content they have whether resident or not: the same surfaces, the
    // same cache. Then the node is meshed as any node is.
    static void buildFar(Item& item)
    {
        TerrainNodeBuild built = buildTerrainNodeFromCells(*item.field, *item.source, item.key, item.sides);
        item.mesh = std::move(built.mesh);
        item.packed = packedForGpu(item.mesh);
        item.content = built.content;
    }
};

TerrainLoader::TerrainLoader() = default;

TerrainLoader::~TerrainLoader()
{
    waitBatch();
}

bool TerrainLoader::batchFinished(const std::unique_ptr<Batch>& batch) const noexcept
{
    if (batch == nullptr)
        return false;
    for (const jobs::JobHandle lane : batch->lanes) {
        if (!jobs::finished(lane))
            return false;
    }
    return true;
}

void TerrainLoader::waitBatch() noexcept
{
    for (const std::unique_ptr<Batch>* batch : {&m_batch, &m_farBatch}) {
        if (*batch != nullptr && !(*batch)->lanes.empty())
            jobs::waitAll((*batch)->lanes);
    }
}

u32 TerrainLoader::integrate(rhi::IDevice& device, rhi::ICmdList& cmd, MeshCache& cache, MeshLibrary& library,
                             usize budget, std::unique_ptr<Batch>& batch)
{
    if (batch == nullptr)
        return 0;
    if (!batch->lanes.empty())
        jobs::waitAll(batch->lanes);
    u32 count = 0;
    // Uploaded in request order, on this thread: nearest first, as ever -- a
    // few a frame. **None is drawn until all are up**: what is drawn is judged
    // against the ground the batch was built from only once it is all there.
    const usize last = std::min(batch->items.size(), batch->next + budget);
    for (; batch->next < last; ++batch->next) {
        Batch::Item& item = batch->items[batch->next];
        Node* node = find(item.world, item.terrain, item.key);
        if (node == nullptr)
            continue;
        node->queued = false;
        if (item.world->terrains().find(item.terrain) == nullptr)
            continue;
        // **Never the mesh a drawn set shows**: the other one.
        u8 shown = 2;
        for (const Drawn& drawn : m_drawn) {
            if (drawn.world == item.world && drawn.draw.terrain == item.terrain && drawn.key == item.key)
                shown = drawn.slot;
        }
        const u8 slot = shown == 0 ? 1 : (shown == 1 ? 0 : (node->variants[0].built ? 1 : 0));
        Variant& variant = node->variants[slot];
        if (variant.mesh.valid())
            cache.release(device, variant.mesh);
        variant.mesh = upload(device, cmd, cache, library, variant.urn, item.mesh, &item.packed);
        if (!variant.mesh.valid())
            library.remove(variant.urn);
        variant.content = item.content;
        variant.revision = item.revision;
        variant.shownRevision = item.revision;
        variant.checkedEdits = item.edits;
        variant.checkedCells = item.cellsRevision;
        variant.shownEdits = item.edits;
        variant.shownCells = item.cellsRevision;
        variant.sides = item.sides;
        variant.built = true;
        if (!node->built)
            countBuilt(item.world, item.terrain, item.key, true);
        node->built = true;
        node->error = static_cast<f64>(item.mesh.error);
        count += 1;
    }
    if (batch->next < batch->items.size())
        return count;
    // All of it up: the ground put up is now this batch's -- not a batch of
    // far ground, built from an older snapshot beside the near ground's.
    for (const Batch::Item& item : batch->items) {
        if (item.far)
            continue;
        auto putUp = std::find_if(m_shown.begin(), m_shown.end(), [&](const Shown& entry) {
            return entry.world == item.world && entry.terrain == item.terrain;
        });
        if (putUp == m_shown.end())
            putUp =
                m_shown.insert(m_shown.end(), Shown{item.world, item.terrain, item.field, item.revision, item.edits});
        // **Never back**: a batch built off the main thread from older ground
        // can finish after an edit built in its own frame has gone up.
        if (item.revision < putUp->revision)
            continue;
        putUp->field = item.field;
        putUp->revision = item.revision;
        putUp->edits = item.edits;
    }
    batch.reset();
    return count;
}

TerrainLoader::EditLatency TerrainLoader::editLatency() const noexcept
{
    EditLatency out;
    out.edits = m_editCount;
    if (m_editCount == 0)
        return out;
    const usize held = static_cast<usize>(std::min<u64>(m_editCount, m_editSamples.size()));
    const auto& last = m_editSamples[static_cast<usize>((m_editCount - 1) % m_editSamples.size())];
    out.lastMs = last.first;
    out.lastFrames = last.second;
    for (usize at = 0; at < held; ++at) {
        out.worstMs = std::max(out.worstMs, m_editSamples[at].first);
        out.worstFrames = std::max(out.worstFrames, m_editSamples[at].second);
    }
    return out;
}

TerrainLodSettings terrainLodFor(const TerrainLodSettings& base, const core::Mat4& projection, core::f32 farPlane,
                                 core::u32 targetHeight, double pixelError) noexcept
{
    TerrainLodSettings lod = base;
    lod.pixelScale = projection.m[3][3] == 0.0f && targetHeight > 0
                         ? static_cast<f64>(projection.m[1][1]) * 0.5 * static_cast<f64>(targetHeight)
                         : 0.0;
    lod.pixelError = pixelError;
    // Past the far plane nothing is drawn whatever is selected; short of it,
    // what is not selected is a hole in the ground. **To the far plane's
    // corners, not its middle**: the plane is flat and a node's distance is
    // round, so a point on the plane at the side of the picture is further
    // from the camera than the plane is -- by the frustum's half-widths -- and
    // a node there dropped at the plane's own distance left the ground
    // scalloped at the sides. A node is let go only wholly past the plane;
    // the clip ends the ground on a line.
    if (farPlane > 0.0f && std::isfinite(farPlane)) {
        f64 reach = 1.0;
        if (projection.m[3][3] == 0.0f && projection.m[0][0] != 0.0f && projection.m[1][1] != 0.0f) {
            const f64 halfWidth = 1.0 / static_cast<f64>(projection.m[0][0]);
            const f64 halfHeight = 1.0 / static_cast<f64>(projection.m[1][1]);
            reach = std::sqrt(1.0 + halfWidth * halfWidth + halfHeight * halfHeight);
        }
        lod.viewDistance = static_cast<f64>(farPlane) * reach;
    }
    return lod;
}

u32 TerrainLoader::sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                        MeshCache& cache, MeshLibrary& library)
{
    // When this `sync` began: an edit built in it has taken this long.
    const auto syncStarted = std::chrono::steady_clock::now();
    m_frame += 1;
    m_lastBuilds = 0;
    m_pending = false;
    // **A batch built off the main thread, done** (TA14): all of it goes up
    // now, before anything is chosen, so it is drawn this frame.
    if (m_batch != nullptr && batchFinished(m_batch))
        m_lastBuilds += integrate(device, cmd, cache, library,
                                  m_async ? (m_batch->edit ? EditUploadsPerSync : UploadsPerSync) : ~usize{0}, m_batch);
    if (m_farBatch != nullptr && batchFinished(m_farBatch))
        m_lastBuilds += integrate(device, cmd, cache, library, m_async ? UploadsPerSync : ~usize{0}, m_farBatch);

    struct Request
    {
        core::InstanceId terrain;
        TerrainNodeKey key;
        f64 distance = 0.0;
        TerrainSides sides{};
        // Built before: its ground or its seams changed.
        bool rebuild = false;
    };

    // **What to draw, and what to build.** Once a frame -- and again after
    // building here and now, so what was built is drawn in the same frame.
    const auto choose = [&](std::vector<Request>& requests) {
        // What was drawn of this world: what a place goes back to while what is
        // wanted there is not built.
        std::vector<Drawn> before;
        for (const Drawn& drawn : m_drawn) {
            if (drawn.world == &world)
                before.push_back(drawn);
        }
        std::erase_if(m_drawn, [&](const Drawn& drawn) { return drawn.world == &world; });

        world.terrains().forEach([&](core::InstanceId id, const scene::TerrainComponent& terrain) {
            const asset::TerrainField& field = terrain.field;
            // **The whole terrain, resident or not** (ADR 0144).
            const asset::TerrainCellSource* source = terrain.cellSource.get();
            const std::optional<std::array<i32, 4>> cellExtent =
                source != nullptr ? source->extent() : std::optional<std::array<i32, 4>>{};
            const f64 voxel = static_cast<f64>(field.settings().voxelSize);
            if ((field.empty() && !cellExtent.has_value()) || !(voxel > 0.0) || !m_hasFocus)
                return;
            const f64 chunkMetres = voxel * static_cast<f64>(asset::ChunkEdge);
            const core::DVec3 focus{m_focus.x - terrain.origin.x, m_focus.y - terrain.origin.y,
                                    m_focus.z - terrain.origin.z};

            // The terrain's extent, in chunks: the resident field's -- x from the
            // sorted ends, z and y by a walk -- and its cells'. What the roots
            // cover and how tall every node's box is; a node over cells alone
            // is as tall as the terrain's floor to its ceiling.
            const std::span<const asset::TerrainField::Entry> chunks = field.chunks();
            i32 minX = std::numeric_limits<i32>::max();
            i32 maxX = std::numeric_limits<i32>::min();
            i32 minZ = std::numeric_limits<i32>::max();
            i32 maxZ = std::numeric_limits<i32>::min();
            f64 lowMetres = static_cast<f64>(field.settings().minHeight);
            f64 highMetres = static_cast<f64>(field.settings().maxHeight);
            if (!chunks.empty()) {
                minX = chunks.front().first.x;
                maxX = chunks.back().first.x;
                i32 minY = chunks.front().first.y;
                i32 maxY = minY;
                for (const asset::TerrainField::Entry& entry : chunks) {
                    minZ = std::min(minZ, entry.first.z);
                    maxZ = std::max(maxZ, entry.first.z);
                    minY = std::min(minY, entry.first.y);
                    maxY = std::max(maxY, entry.first.y);
                }
                if (!cellExtent.has_value()) {
                    lowMetres = static_cast<f64>(minY) * chunkMetres;
                    highMetres = static_cast<f64>(maxY + 1) * chunkMetres;
                }
            }
            if (cellExtent.has_value()) {
                minX = std::min(minX, (*cellExtent)[0]);
                maxX = std::max(maxX, (*cellExtent)[1]);
                minZ = std::min(minZ, (*cellExtent)[2]);
                maxZ = std::max(maxZ, (*cellExtent)[3]);
            }
            // A node has ground to draw when a resident chunk is under it, or a
            // cell is.
            const auto occupiedHere = [&](TerrainNodeKey key) {
                if (occupied(field, key))
                    return true;
                const i32 n = across(key.level);
                return source != nullptr && source->covers(key.x * n, key.x * n + n, key.z * n, key.z * n + n);
            };
            // **Full detail only over resident ground**: a node at level 0 reads
            // voxels, and a cell on disk is drawn from its coarse levels.
            const auto detailHere = [&](TerrainNodeKey key) {
                if (key.level > 0 || source == nullptr)
                    return true;
                Node* node = find(&world, id, key);
                if (node == nullptr)
                    return !readsCells(field, source, key);
                if (node->cellsRevision != terrain.fieldRevision) {
                    node->cellsRevision = terrain.fieldRevision;
                    node->readsCells = readsCells(field, source, key);
                }
                return !node->readsCells;
            };

            // A node is current when its content matches what it was built from;
            // checked only when the field has been written since.
            const auto nodeFor = [&](TerrainNodeKey key) -> Node& {
                if (Node* found = find(&world, id, key)) {
                    // One the count of built nodes made (`countBuilt`).
                    if (!found->variants[0].urn.valid()) {
                        const std::string urn = terrainNodeUrn(id, key);
                        found->variants[0].urn = atoms.intern(urn);
                        found->variants[1].urn = atoms.intern(urn + "#1");
                    }
                    return *found;
                }
                Node fresh;
                fresh.world = &world;
                fresh.terrain = id;
                fresh.key = key;
                const std::string urn = terrainNodeUrn(id, key);
                fresh.variants[0].urn = atoms.intern(urn);
                fresh.variants[1].urn = atoms.intern(urn + "#1");
                return m_nodes.emplace(indexOf(&world, id, key), fresh).first->second;
            };
            // **A mesh is current** when the ground it was built from is the
            // ground now -- checked only when the field has been written since.
            // **And only when it was edited, or a cell on disk changed** (ADR
            // 0150): ground coming in and going out as it is on disk moves the
            // revision and changes no node, and over a large world that is
            // most frames -- every drawn node's cells walked for nothing.
            const u64 edits = terrain.fieldRevision - terrain.streamedRevisions;
            const u64 cellsRevision = source != nullptr ? source->revision() : 0;
            const auto current = [&](Variant& variant, TerrainNodeKey key) {
                if (!variant.built)
                    return false;
                if (variant.revision == terrain.fieldRevision)
                    return true;
                if (variant.checkedEdits != edits || variant.checkedCells != cellsRevision) {
                    if (contentOf(field, source, key) != variant.content)
                        return false;
                    variant.checkedEdits = edits;
                    variant.checkedCells = cellsRevision;
                }
                variant.revision = terrain.fieldRevision;
                return true;
            };
            // And against the ground as last put up, which is what is drawn.
            const Shown* shown = nullptr;
            for (const Shown& entry : m_shown) {
                if (entry.world == &world && entry.terrain == id)
                    shown = &entry;
            }
            const auto currentShown = [&](Variant& variant, TerrainNodeKey key) {
                if (shown == nullptr)
                    return current(variant, key);
                if (!variant.built)
                    return false;
                if (variant.shownRevision == shown->revision)
                    return true;
                if (variant.shownEdits != shown->edits || variant.shownCells != cellsRevision) {
                    if (contentOf(*shown->field, source, key) != variant.content)
                        return false;
                    variant.shownEdits = shown->edits;
                    variant.shownCells = cellsRevision;
                }
                variant.shownRevision = shown->revision;
                return true;
            };
            // **A node's own height** (ADR 0140, TA6): the rows its chunk columns
            // hold, not the whole field's -- a node of low ground under a camera
            // above a mountain range was measured from the range's top.
            std::map<TerrainNodeKey, std::pair<f64, f64>> heights;
            const auto heightOf = [&](TerrainNodeKey key) {
                if (const auto found = heights.find(key); found != heights.end())
                    return found->second;
                const i32 n = across(key.level);
                i32 low = std::numeric_limits<i32>::max();
                i32 high = std::numeric_limits<i32>::min();
                forChunksIn(field, key.x * n, key.x * n + n - 1, key.z * n, key.z * n + n - 1,
                            [&](const asset::TerrainField::Entry& entry) {
                                low = std::min(low, entry.first.y);
                                high = std::max(high, entry.first.y);
                            });
                const std::pair<f64, f64> range = low > high ? std::pair{lowMetres, highMetres}
                                                             : std::pair{static_cast<f64>(low) * chunkMetres,
                                                                         static_cast<f64>(high + 1) * chunkMetres};
                heights.emplace(key, range);
                return range;
            };
            const auto distanceTo = [&](TerrainNodeKey key) {
                const f64 width = static_cast<f64>(across(key.level)) * chunkMetres;
                const f64 x0 = static_cast<f64>(key.x) * width;
                const f64 z0 = static_cast<f64>(key.z) * width;
                const auto [bottom, top] = heightOf(key);
                const f64 dx = std::max({x0 - focus.x, 0.0, focus.x - (x0 + width)});
                const f64 dy = std::max({bottom - focus.y, 0.0, focus.y - top});
                const f64 dz = std::max({z0 - focus.z, 0.0, focus.z - (z0 + width)});
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            // **The distance under which a node shows its children** (ADR 0140):
            // where its error, projected, passes the budget. Flat ground's error
            // is nothing and it stays coarse; a bump, an edge, a feature a coarse
            // cell gathered to a point, paint -- each is its own size. A node not
            // built yet counts its whole cell.
            const auto splitDistance = [&](TerrainNodeKey key) {
                const f64 cell = voxel * static_cast<f64>(1u << key.level);
                if (m_lod.pixelScale > 0.0) {
                    const Node* node = find(&world, id, key);
                    const f64 error = node != nullptr && node->built ? std::max(node->error, 0.01 * cell) : cell;
                    return error * m_lod.pixelScale / std::max(m_lod.pixelError, 1.0e-3);
                }
                return m_lod.splitFactor * static_cast<f64>(across(key.level)) * chunkMetres;
            };
            // A node never built, wanted: a load. One built is rebuilt when it
            // is to be drawn with ground or seams it has no mesh for (below) --
            // or when all it has is nothing, built from ground that has since
            // changed.
            const auto request = [&](TerrainNodeKey key, f64 distance) {
                Node& node = nodeFor(key);
                node.used = m_frame;
                if (node.queued)
                    return;
                if (!node.built) {
                    requests.push_back(Request{id, key, distance, {}, false});
                    return;
                }
                // **Built empty, and its ground no longer is** (the plates on
                // the owner's place, 2026-09-30): a node built before its ground
                // had a surface -- a streamed cell come in before the one above
                // it -- has a mesh of nothing, and nothing draws it, so the
                // rebuild below, asked for what is drawn, never came; its
                // children are not shown while it stands. The ground under it was
                // never drawn, and what was drawn beside it floated.
                bool drawable = false;
                bool live = false;
                for (Variant& variant : node.variants) {
                    drawable = drawable || (variant.built && variant.mesh.valid());
                    live = live || current(variant, key);
                }
                if (!drawable && !live)
                    requests.push_back(Request{id, key, distance, {}, true});
            };
            const auto resident = [&](TerrainNodeKey key) {
                const Node* node = find(&world, id, key);
                return node != nullptr && node->built;
            };
            // **Sliding onto its parent** as it nears the distance its parent is
            // drawn at in its place (ADR 0140): the parent takes over past its
            // own split distance, so a node has slid all the way there, starting
            // at 85 per cent of it -- a narrow band, since what a node shows while
            // sliding is up to its parent's error, over the budget by as much as
            // the band is wide -- and never before its own children would have
            // taken over, so it is itself when they swap. The top level has no
            // parent, and slides nowhere.
            const auto morphRange = [&](TerrainNodeKey key) -> std::array<f32, 2> {
                if (key.level >= TerrainTopLevel || m_lod.fullDetail)
                    return {0.0f, 0.0f};
                const TerrainNodeKey parent{key.level + 1, asset::floorDiv(key.x, 2), asset::floorDiv(key.z, 2)};
                const f64 end = splitDistance(parent);
                const f64 start = std::max(key.level > 0 ? splitDistance(key) : 0.0, 0.85 * end);
                if (end <= start)
                    return {0.0f, 0.0f};
                return {static_cast<f32>(start), static_cast<f32>(end)};
            };
            // What the selection would draw now (below: drawn where it is built).
            std::vector<TerrainNodeKey> wanted;
            const auto draw = [&](TerrainNodeKey key) {
                Node& node = nodeFor(key);
                node.used = m_frame;
                // Not ground with nothing on it -- which it is whatever its seams:
                // it is not beside anything, as it never was.
                bool drawable = false;
                bool emptyNow = false;
                for (Variant& variant : node.variants) {
                    if (!variant.built)
                        continue;
                    if (currentShown(variant, key))
                        (variant.mesh.valid() ? drawable : emptyNow) = true;
                    else if (variant.mesh.valid())
                        drawable = true;
                }
                if (drawable && !emptyNow)
                    wanted.push_back(key);
            };

            const auto childrenOf = [&](TerrainNodeKey key, std::array<TerrainNodeKey, 4>& out) {
                usize count = 0;
                if (key.level == 0)
                    return count;
                for (i32 dz = 0; dz < 2; ++dz) {
                    for (i32 dx = 0; dx < 2; ++dx) {
                        const TerrainNodeKey child{key.level - 1, key.x * 2 + dx, key.z * 2 + dz};
                        if (occupiedHere(child))
                            out[count++] = child;
                    }
                }
                return count;
            };
            // **Whether a node's ground can be drawn without a hole**: its own mesh,
            // or meshes further down that cover all of it. Asking only whether the
            // children had meshes of their own was the flicker the owner saw in the
            // cave: an ancestor let go while its descendants were drawn came back,
            // counted as ready, and was drawn over the whole close-up for the
            // frames its own children took to be rebuilt.
            const auto coverable = [&](const auto& self, TerrainNodeKey key) -> bool {
                const Node* here = find(&world, id, key);
                if (here != nullptr && here->built)
                    return true;
                // Nothing built under it: nothing that could cover it.
                if (here == nullptr || here->builtBelow == 0)
                    return false;
                std::array<TerrainNodeKey, 4> children{};
                const usize count = childrenOf(key, children);
                if (count == 0)
                    return false;
                for (usize at = 0; at < count; ++at) {
                    if (!self(self, children[at]))
                        return false;
                }
                return true;
            };
            // Draws a node, or whatever is built underneath it.
            const auto drawCovering = [&](const auto& self, TerrainNodeKey key) -> void {
                const Node* here = find(&world, id, key);
                if (here != nullptr && here->built) {
                    draw(key);
                    return;
                }
                if (here == nullptr || here->builtBelow == 0)
                    return;
                std::array<TerrainNodeKey, 4> children{};
                const usize count = childrenOf(key, children);
                for (usize at = 0; at < count; ++at)
                    self(self, children[at]);
            };

            // **The selection, depth first in key order** (R10: the order draws and
            // requests are made in is a function of the world and the camera).
            const auto select = [&](const auto& self, TerrainNodeKey key) -> void {
                // Distance first: it is arithmetic, and `occupied` is a search per
                // chunk column under the node.
                const f64 distance = distanceTo(key);
                if (distance > m_lod.viewDistance)
                    return;
                if (!occupiedHere(key))
                    return;
                // Every node the walk passes through is in use, drawn or not: an
                // ancestor let go is one the next step back has to rebuild first.
                if (Node* visited = find(&world, id, key))
                    visited->used = m_frame;
                std::array<TerrainNodeKey, 4> children{};
                const usize childCount = childrenOf(key, children);
                const auto childrenCoverable = [&] {
                    for (usize at = 0; at < childCount; ++at) {
                        if (!coverable(coverable, children[at]))
                            return false;
                    }
                    return true;
                };

                // **Split where the node's cell covers more than the error budget**
                // (ADR 0140), and join again only past 1.25 times the distance
                // that is -- so a camera on the line does not flip it every frame.
                Node& here = nodeFor(key);
                here.used = m_frame;
                // Not into full detail over ground that is not resident (ADR
                // 0144): this node stands for it until its cells come in.
                bool detailUnder = true;
                for (usize at = 0; at < childCount; ++at)
                    detailUnder = detailUnder && detailHere(children[at]);
                // **Far ground is built from the top down** (ADR 0150): a node
                // over ground that is not resident, not built yet, is asked
                // for and not split. Unbuilt, its error is its whole cell, and
                // split by that the selection asked for every fine node under
                // it across the view -- thousands, each read from cells --
                // before the coarse one that would have said the ground there
                // is flat and needs none of them.
                bool farUnbuilt = false;
                if (!here.built && source != nullptr && key.level > 0) {
                    if (here.cellsRevision != terrain.fieldRevision) {
                        here.cellsRevision = terrain.fieldRevision;
                        here.readsCells = readsCells(field, source, key);
                    }
                    farUnbuilt = here.readsCells;
                }
                const bool splitting =
                    key.level > 0 && detailUnder && !farUnbuilt &&
                    (m_lod.fullDetail || distance < splitDistance(key) * (here.split ? SplitHysteresis : 1.0));
                here.split = splitting;
                if (splitting) {
                    for (usize at = 0; at < childCount; ++at) {
                        if (!resident(children[at]))
                            request(children[at], distanceTo(children[at]));
                    }
                    if (childrenCoverable()) {
                        for (usize at = 0; at < childCount; ++at)
                            self(self, children[at]);
                        return;
                    }
                    // Not ready: this node stands in for them if it can -- or they
                    // draw what they already have.
                    if (resident(key)) {
                        draw(key);
                        return;
                    }
                    for (usize at = 0; at < childCount; ++at)
                        self(self, children[at]);
                    return;
                }

                request(key, distance);
                // Its own mesh, or finer ones while it is not there yet.
                drawCovering(drawCovering, key);
            };

            // **The roots within sight, not the field's whole extent** (terrain
            // audit P3): two edits a million metres apart made a box of millions
            // of roots, every one visited every frame.
            const i32 top = across(TerrainTopLevel);
            const f64 rootMetres = static_cast<f64>(top) * chunkMetres;
            const auto rootAt = [rootMetres](f64 metres) {
                return static_cast<i32>(std::clamp(std::floor(metres / rootMetres), -1.0e9, 1.0e9));
            };
            const i32 firstX = std::max(asset::floorDiv(minX, top), rootAt(focus.x - m_lod.viewDistance));
            const i32 lastX = std::min(asset::floorDiv(maxX + 1, top), rootAt(focus.x + m_lod.viewDistance));
            const i32 firstZ = std::max(asset::floorDiv(minZ, top), rootAt(focus.z - m_lod.viewDistance));
            const i32 lastZ = std::min(asset::floorDiv(maxZ + 1, top), rootAt(focus.z + m_lod.viewDistance));
            for (i32 rootZ = firstZ; rootZ <= lastZ; ++rootZ) {
                for (i32 rootX = firstX; rootX <= lastX; ++rootX)
                    select(select, TerrainNodeKey{TerrainTopLevel, rootX, rootZ});
            }

            // **Drawn where it is built** (TA14): each node of what the selection
            // wants is drawn with a mesh for the ground now and the coarser levels
            // beside it -- stitched to them (ADR 0140). Where one has none yet, the
            // ground under it is drawn as the frame before drew it, and so are the
            // nodes beside it whose levels its seams depend on, until everything
            // drawn is built for what is drawn beside it: no seam open, no edit half
            // done, while the rest is built off the main thread.
            const auto overlaps = [](TerrainNodeKey a, TerrainNodeKey b) {
                const i32 spanA = across(a.level);
                const i32 spanB = across(b.level);
                return a.x * spanA < (b.x + 1) * spanB && b.x * spanB < (a.x + 1) * spanA &&
                       a.z * spanA < (b.z + 1) * spanB && b.z * spanB < (a.z + 1) * spanA;
            };
            // Whether `b`, as coarse as `a` or coarser, is drawn beside it: its
            // levels decide `a`'s seams.
            const auto shapes = [](TerrainNodeKey a, TerrainNodeKey b) {
                if (b.level < a.level)
                    return false;
                const i32 spanA = across(a.level);
                const i32 spanB = across(b.level);
                return (a.x - 1) * spanA < (b.x + 1) * spanB && b.x * spanB < (a.x + 2) * spanA &&
                       (a.z - 1) * spanA < (b.z + 1) * spanB && b.z * spanB < (a.z + 2) * spanA && !(a == b);
            };
            std::map<TerrainNodeKey, u8> previous;
            for (const Drawn& drawn : before) {
                if (drawn.draw.terrain != id)
                    continue;
                const Node* node = find(&world, id, drawn.key);
                if (node != nullptr && node->variants[drawn.slot].built)
                    previous.emplace(drawn.key, drawn.slot);
            }
            // The set drawn, and which of its nodes are the frame before's.
            std::vector<TerrainNodeKey> set = wanted;
            std::map<TerrainNodeKey, u8> kept;
            std::map<TerrainNodeKey, u8> slots;
            const auto revert = [&](TerrainNodeKey key) {
                // The frame before's nodes over its ground, in place of what is over
                // it now.
                std::vector<std::pair<TerrainNodeKey, u8>> cover;
                for (const auto& [old, slot] : previous) {
                    if (overlaps(old, key))
                        cover.emplace_back(old, slot);
                }
                std::erase_if(set, [&](TerrainNodeKey member) {
                    if (overlaps(member, key))
                        return true;
                    for (const auto& [old, slot] : cover) {
                        if (overlaps(member, old))
                            return true;
                    }
                    return false;
                });
                for (const auto& [old, slot] : cover) {
                    set.push_back(old);
                    kept[old] = slot;
                }
            };
            bool settled = false;
            for (int round = 0; round < 32 && !settled; ++round) {
                std::sort(set.begin(), set.end());
                set.erase(std::unique(set.begin(), set.end()), set.end());
                slots.clear();
                std::vector<TerrainNodeKey> unready;
                for (const TerrainNodeKey key : set) {
                    Node& node = nodeFor(key);
                    const TerrainSides sides = terrainStitchSides(set, key);
                    u8 slot = 2;
                    if (const auto was = kept.find(key); was != kept.end()) {
                        // As it was drawn: its ground may since have changed.
                        if (node.variants[was->second].sides == sides)
                            slot = was->second;
                    }
                    for (u8 at = 0; at < 2 && slot == 2; ++at) {
                        if (node.variants[at].sides == sides && currentShown(node.variants[at], key))
                            slot = at;
                    }
                    // **Built for the ground now** and what is wanted -- the first
                    // round's set -- and never for a set on the way back to the
                    // frame before's.
                    if (round == 0 && (!node.queued || node.queuedSides != sides)) {
                        bool live = false;
                        for (u8 at = 0; at < 2 && !live; ++at)
                            live = node.variants[at].sides == sides && current(node.variants[at], key);
                        if (!live)
                            requests.push_back(Request{id, key, distanceTo(key), sides, true});
                    }
                    if (slot == 2) {
                        unready.push_back(key);
                        continue;
                    }
                    slots.emplace(key, slot);
                }
                // **Ground with nothing to draw is not beside anything**: a node
                // built empty leaves the set, as it always did, and what was beside
                // it is stitched without it.
                const auto empty = std::erase_if(set, [&](TerrainNodeKey key) {
                    const auto found = slots.find(key);
                    const bool gone = found != slots.end() && !nodeFor(key).variants[found->second].mesh.valid();
                    return gone;
                });
                settled = unready.empty() && empty == 0;
                for (const TerrainNodeKey key : unready) {
                    if (!kept.contains(key)) {
                        revert(key);
                        continue;
                    }
                    // A node drawn before, now beside other levels: the nodes that
                    // shape its seams go back too.
                    std::vector<TerrainNodeKey> near;
                    for (const TerrainNodeKey member : set) {
                        if (!kept.contains(member) && shapes(key, member))
                            near.push_back(member);
                    }
                    for (const TerrainNodeKey member : near)
                        revert(member);
                    if (near.empty()) {
                        // Nothing left to take back: what was drawn, all of it.
                        set.clear();
                        kept.clear();
                        for (const auto& [old, slot] : previous) {
                            set.push_back(old);
                            kept[old] = slot;
                        }
                    }
                }
            }
            if (!settled) {
                // Still unsettled: the frame before's, which was.
                slots = previous;
                set.clear();
                for (const auto& [old, slot] : previous)
                    set.push_back(old);
            }
            std::sort(set.begin(), set.end());
            set.erase(std::unique(set.begin(), set.end()), set.end());
            // **And each node's neighbours' geomorph** (ADR 0140): the node drawn
            // beside each side and corner, if it is this level or coarser -- the
            // ones that share a seam's vertices with it at their level.
            for (const TerrainNodeKey key : set) {
                const auto found = slots.find(key);
                if (found == slots.end())
                    continue;
                Node& node = nodeFor(key);
                node.used = m_frame;
                const Variant& variant = node.variants[found->second];
                if (!variant.mesh.valid())
                    continue;
                TerrainNodeDraw drawn{id, variant.urn};
                drawn.morph = terrainMorphOf(set, key, morphRange);
                m_drawn.push_back(Drawn{&world, drawn, key, found->second});
            }
        });
    };

    std::vector<Request> requests;
    choose(requests);

    // **Nearest first, a fixed count** -- ties by level, terrain and key, so the
    // order is a fact about the world and the camera.
    std::sort(requests.begin(), requests.end(), [](const Request& a, const Request& b) {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        if (a.key.level != b.key.level)
            return a.key.level < b.key.level;
        if (a.terrain.index != b.terrain.index)
            return a.terrain.index < b.terrain.index;
        return a.key < b.key;
    });
    requests.erase(
        std::unique(requests.begin(), requests.end(),
                    [](const Request& a, const Request& b) { return a.terrain == b.terrain && a.key == b.key; }),
        requests.end());

    // **A small edit is built here and now** (`MaxEditBuildsInFrame`), whatever
    // is being built off the main thread: every node of it, on every worker,
    // put up at once and drawn in this frame.
    if (m_async) {
        std::vector<Request> edits;
        for (const Request& next : requests) {
            if (!next.rebuild)
                continue;
            const scene::TerrainComponent* terrain = world.terrains().find(next.terrain);
            if (terrain == nullptr || find(&world, next.terrain, next.key) == nullptr ||
                readsCells(terrain->field, terrain->cellSource.get(), next.key))
                continue;
            edits.push_back(next);
        }
        u32 ground = 0;
        for (const Request& next : edits)
            ground += 1u << (2u * std::min<u32>(next.key.level, 8u));
        if (!edits.empty() && edits.size() <= MaxEditBuildsInFrame && ground <= MaxEditGroundInFrame) {
            auto batch = std::make_unique<Batch>();
            std::map<u64, std::shared_ptr<const asset::TerrainField>> snapshots;
            for (const Request& next : edits) {
                const scene::TerrainComponent* terrain = world.terrains().find(next.terrain);
                Node* node = find(&world, next.terrain, next.key);
                const u64 slot = (static_cast<u64>(next.terrain.index) << 32) | next.terrain.generation;
                std::shared_ptr<const asset::TerrainField>& snapshot = snapshots[slot];
                if (snapshot == nullptr)
                    snapshot = std::make_shared<const asset::TerrainField>(terrain->field);
                node->queued = true;
                node->queuedSides = next.sides;
                Batch::Item item;
                item.world = &world;
                item.terrain = next.terrain;
                item.key = next.key;
                item.sides = next.sides;
                item.revision = terrain->fieldRevision;
                item.edits = terrain->fieldRevision - terrain->streamedRevisions;
                item.cellsRevision = terrain->cellSource != nullptr ? terrain->cellSource->revision() : 0;
                item.field = snapshot;
                item.cells = terrain->cellSource;
                batch->items.push_back(std::move(item));
            }
            Batch* running = batch.get();
            running->seamsAhead = false;
            running->laneCount = std::clamp<u32>(jobs::workerCount(), 1u, static_cast<u32>(running->items.size()));
            jobs::parallelFor("terrain.edit", jobs::Domain::Render, 0, running->laneCount, 1,
                              [running](usize begin, usize end, u32) noexcept {
                                  for (usize lane = begin; lane < end; ++lane)
                                      running->run(static_cast<u32>(lane));
                              });
            m_lastBuilds += integrate(device, cmd, cache, library, ~usize{0}, batch);
            // And drawn in this frame: chosen again, with what was built.
            requests.clear();
            choose(requests);
            std::sort(requests.begin(), requests.end(), [](const Request& a, const Request& b) {
                if (a.distance != b.distance)
                    return a.distance < b.distance;
                if (a.key.level != b.key.level)
                    return a.key.level < b.key.level;
                if (a.terrain.index != b.terrain.index)
                    return a.terrain.index < b.terrain.index;
                return a.key < b.key;
            });
            requests.erase(std::unique(requests.begin(), requests.end(),
                                       [](const Request& a, const Request& b) {
                                           return a.terrain == b.terrain && a.key == b.key;
                                       }),
                           requests.end());
        }
    }
    // **Which to build.** A node built before is wanted for ground or seams it
    // has no mesh for, and **every one of those goes in one batch**, up to
    // `MaxEditBuildsPerSync`: a batch goes up in one frame, so neighbouring
    // nodes never show two versions of one edit -- the flicker the owner saw
    // while editing. A node never built is loading, and loading keeps the
    // budget, nearest first.
    if (m_batch == nullptr || m_farBatch == nullptr) {
        auto batch = std::make_unique<Batch>();
        // **Far ground, a batch of its own** (ADR 0144): built from cells on
        // disk, on one lane beside the near ground, so a node near the camera
        // is never queued behind one far away. Synchronously -- a picture --
        // it is built with the rest, now.
        auto farBatch = std::make_unique<Batch>();
        std::map<u64, std::shared_ptr<const asset::TerrainField>> snapshots;
        u32 rebuilds = 0;
        u32 loads = 0;
        u32 far = 0;
        std::vector<Request> left;
        for (const Request& next : requests) {
            const scene::TerrainComponent* terrain = world.terrains().find(next.terrain);
            Node* node = find(&world, next.terrain, next.key);
            if (terrain == nullptr || node == nullptr)
                continue;
            const bool fromCells = readsCells(terrain->field, terrain->cellSource.get(), next.key);
            const bool apart = fromCells && m_async;
            u32& count = apart ? far : (next.rebuild ? rebuilds : loads);
            const u32 limit = apart ? MaxFarBuildsPerSync : (next.rebuild ? MaxEditBuildsPerSync : m_buildsPerSync);
            if ((apart ? m_farBatch : m_batch) != nullptr || count >= limit) {
                left.push_back(next);
                continue;
            }
            count += 1;
            if (next.rebuild && !apart)
                batch->edit = true;
            const u64 slot = (static_cast<u64>(next.terrain.index) << 32) | next.terrain.generation;
            std::shared_ptr<const asset::TerrainField>& snapshot = snapshots[slot];
            if (snapshot == nullptr)
                snapshot = std::make_shared<const asset::TerrainField>(terrain->field);
            node->queued = true;
            node->queuedSides = next.sides;
            Batch::Item item;
            item.world = &world;
            item.terrain = next.terrain;
            item.key = next.key;
            item.sides = next.sides;
            item.revision = terrain->fieldRevision;
            item.edits = terrain->fieldRevision - terrain->streamedRevisions;
            // Before the build reads a cell: one that changes under it moves
            // this on, and the node is checked again.
            item.cellsRevision = terrain->cellSource != nullptr ? terrain->cellSource->revision() : 0;
            item.field = snapshot;
            item.source = fromCells ? terrain->cellSource : nullptr;
            item.cells = terrain->cellSource;
            item.far = fromCells;
            (apart ? *farBatch : *batch).items.push_back(std::move(item));
        }
        requests = std::move(left);
        if (m_async && !farBatch->items.empty()) {
            // Two lanes at most, and one on a machine of eight threads or
            // fewer: far ground is a few seconds' work once, and the near
            // ground's lanes and the frame's own jobs keep the rest.
            Batch* running = farBatch.get();
            running->laneCount =
                std::min<u32>(jobs::workerCount() > 8u ? 2u : 1u, static_cast<u32>(running->items.size()));
            for (u32 lane = 0; lane < running->laneCount; ++lane)
                running->lanes.push_back(jobs::schedule("terrain.build.far", jobs::Domain::Render,
                                                        [running, lane]() noexcept { running->run(lane); }));
            m_farBatch = std::move(farBatch);
        }
        if (!batch->items.empty()) {
            if (m_async) {
                // **A few lanes**: a quarter of the workers, at least one and
                // at most four, so the frame's own jobs are never queued behind
                // the ground.
                // **An edit gets half of them**, up to eight: somebody is
                // watching the ground under a brush, and the frame's jobs are
                // short beside a mesh.
                batch->laneCount = batch->edit ? std::clamp<u32>(jobs::workerCount() / 2u, 1u, 8u)
                                               : std::clamp<u32>(jobs::workerCount() / 4u, 1u, 4u);
                batch->laneCount = std::min<u32>(batch->laneCount, static_cast<u32>(batch->items.size()));
                Batch* running = batch.get();
                for (u32 lane = 0; lane < running->laneCount; ++lane)
                    running->lanes.push_back(jobs::schedule("terrain.build", jobs::Domain::Render,
                                                            [running, lane]() noexcept { running->run(lane); }));
                m_batch = std::move(batch);
            }
            else {
                // **Here and now, on every worker**: the surfaces first, then
                // the meshes, each wide; then drawn in this frame. A node from
                // cells gathers its own, a row of cells at a time.
                std::vector<MissingSurface> missing;
                std::vector<asset::SurfaceWant> wants;
                for (const Batch::Item& item : batch->items) {
                    if (item.far)
                        continue;
                    wants.clear();
                    missingTerrainSurfaces(*item.field, item.key, item.sides, wants);
                    for (const asset::SurfaceWant& want : wants)
                        missing.push_back(MissingSurface{item.field.get(), want});
                }
                gatherTerrainSurfaces(std::move(missing));
                Batch& built = *batch;
                jobs::parallelFor("terrain.mesh", jobs::Domain::Render, 0, built.items.size(), 1,
                                  [&built](usize begin, usize end, u32) noexcept {
                                      for (usize at = begin; at < end; ++at) {
                                          Batch::Item& item = built.items[at];
                                          if (item.far) {
                                              Batch::buildFar(item);
                                              continue;
                                          }
                                          item.mesh = meshTerrainNode(*item.field, item.key, item.sides);
                                          item.packed = packedForGpu(item.mesh);
                                          item.content = contentOf(*item.field, item.cells.get(), item.key);
                                      }
                                  });
                m_batch = std::move(batch);
                m_lastBuilds += integrate(device, cmd, cache, library, ~usize{0}, m_batch);
                std::vector<Request> after;
                choose(after);
                requests.insert(requests.end(), after.begin(), after.end());
            }
        }
    }
    m_pending = m_batch != nullptr || m_farBatch != nullptr || !requests.empty();

    // **How long an edit takes to be seen** (`editLatency`). Stamped by the
    // `sync` that first finds the ground edited; over when the ground put up
    // has that edit in it, or when nothing drawn is waiting on a rebuild --
    // an edit that changed nothing in sight.
    {
        const auto now = std::chrono::steady_clock::now();
        const bool rebuilding =
            m_batch != nullptr ||
            std::any_of(requests.begin(), requests.end(), [](const Request& request) { return request.rebuild; });
        world.terrains().forEach([&](core::InstanceId id, const scene::TerrainComponent& terrain) {
            const u64 edits = terrain.fieldRevision - terrain.streamedRevisions;
            auto stamp = std::find_if(m_editStamps.begin(), m_editStamps.end(), [&](const EditStamp& entry) {
                return entry.world == &world && entry.terrain == id;
            });
            if (stamp == m_editStamps.end()) {
                EditStamp fresh;
                fresh.world = &world;
                fresh.terrain = id;
                fresh.seen = edits;
                m_editStamps.push_back(fresh);
                return;
            }
            if (edits != stamp->seen && !stamp->pending) {
                stamp->pending = true;
                stamp->edits = edits;
                stamp->at = syncStarted;
                stamp->frame = m_frame;
            }
            stamp->seen = edits;
            if (!stamp->pending)
                return;
            const auto shown = std::find_if(m_shown.begin(), m_shown.end(), [&](const Shown& entry) {
                return entry.world == &world && entry.terrain == id;
            });
            if (!((shown != m_shown.end() && shown->edits >= stamp->edits) || !rebuilding))
                return;
            stamp->pending = false;
            m_editSamples[static_cast<usize>(m_editCount % m_editSamples.size())] =
                std::pair{std::chrono::duration<double, std::milli>(now - stamp->at).count(),
                          static_cast<u32>(m_frame - stamp->frame)};
            m_editCount += 1;
        });
    }

    // Nodes of this world nobody has drawn or wanted for a while, or whose
    // terrain is gone, let their meshes go.
    // In key order, which is by level: a node's descendants are met, and
    // uncounted, before it is.
    //
    // **A few a frame** (ADR 0150): a camera that travels far in one frame
    // leaves every node of where it was unused at once, and three seconds
    // later they all came due together -- some hundreds of meshes let go in
    // one frame, 35 to 49 ms of it waiting on the device. A terrain that is
    // gone lets all of its go at once, as before: nothing is drawn of it.
    u32 released = 0;
    for (auto at = m_nodes.begin(); at != m_nodes.end();) {
        Node& node = at->second;
        const bool gone = world.terrains().find(node.terrain) == nullptr;
        if (node.world != &world || (!gone && node.used + EvictAfterFrames >= m_frame)) {
            ++at;
            continue;
        }
        const bool holdsMesh = node.variants[0].mesh.valid() || node.variants[1].mesh.valid();
        if (!gone && holdsMesh) {
            if (released >= MaxReleasesPerSync) {
                ++at;
                continue;
            }
            released += 1;
        }
        release(device, cache, library, node);
        if (node.built) {
            node.built = false;
            for (Variant& variant : node.variants) {
                variant.built = false;
                variant.revision = ~0ull;
                variant.shownRevision = ~0ull;
            }
            if (!gone)
                countBuilt(node.world, node.terrain, node.key, false);
        }
        // Kept while anything under it is built: it is what says so.
        if (!gone && node.builtBelow > 0) {
            ++at;
            continue;
        }
        at = m_nodes.erase(at);
    }
    std::erase_if(m_shown, [&](const Shown& entry) {
        return entry.world == &world && world.terrains().find(entry.terrain) == nullptr;
    });
    return m_lastBuilds;
}

std::vector<std::pair<TerrainNodeKey, TerrainSides>> TerrainLoader::drawnSeams(const scene::World& world) const
{
    std::vector<std::pair<TerrainNodeKey, TerrainSides>> out;
    for (const Drawn& drawn : m_drawn) {
        if (drawn.world != &world)
            continue;
        const Node* node = find(&world, drawn.draw.terrain, drawn.key);
        if (node != nullptr)
            out.emplace_back(drawn.key, node->variants[drawn.slot].sides);
    }
    return out;
}

std::vector<TerrainNodeKey> TerrainLoader::drawnOutOfDate(const scene::World& world) const
{
    std::vector<TerrainNodeKey> out;
    for (const Drawn& drawn : m_drawn) {
        if (drawn.world != &world)
            continue;
        const Node* node = find(&world, drawn.draw.terrain, drawn.key);
        const scene::TerrainComponent* terrain = world.terrains().find(drawn.draw.terrain);
        if (node == nullptr || terrain == nullptr ||
            contentOf(terrain->field, terrain->cellSource.get(), drawn.key) != node->variants[drawn.slot].content)
            out.push_back(drawn.key);
    }
    return out;
}

std::vector<TerrainNodeDraw> TerrainLoader::draws(const scene::World& world) const
{
    std::vector<TerrainNodeDraw> out;
    out.reserve(m_drawn.size());
    for (const Drawn& drawn : m_drawn) {
        if (drawn.world != &world)
            continue;
        TerrainNodeDraw draw = drawn.draw;
        draw.level = static_cast<core::u8>(drawn.key.level);
        out.push_back(draw);
    }
    return out;
}

void TerrainLoader::appendRenderTerrains(const scene::World& world, core::InstanceId root, RenderWorld& out,
                                         const TextureLibrary* textures) const
{
    const std::vector<std::string> engineLayers = asset::defaultTerrainLayers();
    world.terrains().forEach([&](core::InstanceId id, const scene::TerrainComponent& terrain) {
        // **With no ground yet too** (terrain audit T5): a streamed terrain
        // before its first cell has its pipelines and its layers' arrays made
        // while the world loads, not in the first frame its ground is drawn.
        if (!inWorld(world, id, root))
            return;
        RenderTerrain entry;
        entry.id = id;
        entry.origin = terrain.origin;
        entry.layers.reserve(terrain.layers.size());
        for (const std::string& urn : terrain.layers) {
            RenderTerrainLayer layer;
            const core::NameAtom atom = world.atoms().lookup(urn);
            const asset::ResolvedMaterial material =
                atom.id != 0 ? world.resolveMaterial(atom, 0) : asset::ResolvedMaterial{};
            const asset::MaterialProperties& p = material.properties;
            // Flat, the engine's own are the old palette's colours -- what a
            // world looked like before it had textures, and what it looks like
            // for the frame or two before they load. Anything else is its
            // colour factor.
            const auto engineIndex = std::find(engineLayers.begin(), engineLayers.end(), urn);
            const core::Vec3 flat =
                engineIndex != engineLayers.end()
                    ? asset::terrainColorOf(static_cast<core::u8>(engineIndex - engineLayers.begin() + 1))
                    : core::Vec3{p.color.r, p.color.g, p.color.b};
            layer.flat[0] = flat.x;
            layer.flat[1] = flat.y;
            layer.flat[2] = flat.z;
            // The flat colour's free slot: how hard paint of this layer meets
            // what is under it (ADR 0114), the material's own `BlendSharpness`.
            layer.flat[3] = std::clamp(p.blendSharpness, 0.0f, 1.0f);
            layer.tint[0] = p.color.r;
            layer.tint[1] = p.color.g;
            layer.tint[2] = p.color.b;
            layer.tint[3] = 1.0f / (p.tileSize > 0.0f ? p.tileSize : 4.0f);
            layer.surface[0] = p.roughness;
            layer.surface[1] = p.metalness;
            layer.surface[2] = p.normalScale;
            // Flags: 1 triplanar, 2 a height map -- which the ground's
            // occlusion is read from, and without which it has none (TA13).
            layer.surface[3] = (p.triplanar ? 1.0f : 0.0f) + (p.heightMap.empty() ? 0.0f : 2.0f);
            // **How its repeat is broken up** (ADR 0113's amendment): read by
            // a textured layer only -- plain ground never samples a layer.
            layer.tiling[0] = std::clamp(p.tilingVariation, 0.0f, 1.0f);
            layer.tiling[1] = 1.0f / std::clamp(p.tilingFarScale, 1.0f, 32.0f);
            layer.tiling[2] = p.hexTiling ? 1.0f : 0.0f;
            const std::array<const std::string*, 4> maps{&p.colorMap, &p.normalMap, &p.metallicRoughnessMap,
                                                         &p.heightMap};
            for (usize slot = 0; slot < maps.size(); ++slot) {
                if (maps[slot]->empty())
                    continue;
                const core::NameAtom map = world.atoms().lookup(*maps[slot]);
                layer.maps[slot] = textures != nullptr && map.id != 0 ? textures->find(map) : rhi::TextureHandle{};
                layer.waiting = layer.waiting || !layer.maps[slot].valid();
            }
            entry.layers.push_back(layer);
        }
        entry.rules.reserve(terrain.rules.size());
        for (const asset::TerrainRule& rule : terrain.rules)
            entry.rules.push_back(asset::shapeOf(rule));
        out.terrains.push_back(std::move(entry));
    });
}

void TerrainLoader::destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library)
{
    waitBatch();
    m_batch.reset();
    m_farBatch.reset();
    m_shown.clear();
    for (auto& [index, node] : m_nodes)
        release(device, cache, library, node);
    m_nodes.clear();
    m_drawn.clear();
}

usize TerrainLoader::residentCount() const noexcept
{
    usize count = 0;
    for (const auto& [index, node] : m_nodes) {
        if (node.variants[0].mesh.valid() || node.variants[1].mesh.valid())
            ++count;
    }
    return count;
}

bool TerrainLoader::nodeResident(core::InstanceId terrain, TerrainNodeKey key) const noexcept
{
    for (const auto& [index, node] : m_nodes) {
        if (node.terrain == terrain && node.key == key && node.built)
            return true;
    }
    return false;
}

} // namespace engine::render
