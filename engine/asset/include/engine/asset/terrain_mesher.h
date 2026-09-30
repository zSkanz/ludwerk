#pragma once

// Turning a `TerrainField` into triangles (ADR 0082).
//
// **The isosurface is extracted as a SURFACE NET** over occupancy: one vertex
// in each cell of the lattice of voxel centres that the surface passes through,
// at the mean of the crossings on that cell's edges, and one quad around each
// lattice edge the surface crosses, joining the four cells that share it. The
// surface is where occupancy crosses one half.
//
// It replaced marching tetrahedra in 2026-09: six tetrahedra round each cube's
// diagonal put that diagonal into every curved wall as a zig-zag. A surface net
// has no table, no ambiguous case and no preferred direction.
//
// **A region owns lattice points, and two regions side by side agree.** A region
// owns the points `[min, min + n)` on each axis and emits the quads of the edges
// that START on a point it owns. It builds cells from `min - 1`, so every such
// quad has its four cells -- and the ring of cells at `min - 1` is the ring its
// neighbour builds as its last, from the same samples, into the same vertices.
// So meshing the world a region at a time is watertight with no stitching.

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "engine/asset/model.h"
#include "engine/asset/terrain.h"
#include "engine/core/types.h"

namespace engine::asset {

// What to mesh, and how finely. Every coordinate is in the voxels of `level`:
// a level-L index is the level-0 index divided by `2^L`.
struct MeshRegion
{
    // The first lattice point the region owns, on each axis.
    core::i32 minX = 0;
    core::i32 minY = 0;
    core::i32 minZ = 0;

    // How many lattice points it owns on each axis.
    core::u32 cellsX = 0;
    core::u32 cellsY = 0;
    core::u32 cellsZ = 0;

    // **The level of detail.** Zero reads the voxels; level L reads each
    // chunk's level-L mip, the mean of `2^L` voxels a side, so a node far away
    // has an eighth of the triangles per level and a thin wall thins rather
    // than vanishing.
    core::u32 level = 0;

    // **The level drawn beside each side and corner, where it is coarser**
    // (ADR 0140): low x, high x, low z, high z, then the corners low x and low
    // z, high x and low z, low x and high z, high x and high z; zero where the
    // neighbour is this level or finer. The region gathers the cells it shares
    // with such a neighbour -- those inside the neighbour's own cell that
    // straddles the seam -- at the neighbour's level, so the vertices on the
    // seam are the neighbour's own and the meshes share them: stitched, with
    // no gap and no skirt. A corner's cell is shared by four nodes, and takes
    // the coarsest of them.
    std::array<core::u8, 8> sideLevels{};
};

// **How many of a level's cells past a region's sides `meshField` reads**, on
// x and z: the samples its cells and gradients need, or the column map its
// openness rays march over, whichever reaches further. Whoever keys a mesh on
// what it read, or prepares what it reads before meshing in parallel, asks
// this rather than knowing it.
[[nodiscard]] core::i32 meshReach(const FieldSettings& settings, core::u32 level) noexcept;

// The triangles, and what a collider needs from them.
struct TerrainMesh
{
    // Ready for `MeshCache`: 48-byte vertices and u32 indices, in the field's
    // own metres. Each vertex carries its material in the tangent's x and how
    // much sky it sees in its y -- the terrain shader reads both.
    Mesh mesh;

    // What each submesh is made of, parallel to `mesh.submeshes`: one section
    // per material, in id order.
    std::vector<core::u8> sectionMaterials;
    // **Where each vertex slides to as its node gives way to its parent**
    // (ADR 0140): the offset to the parent's vertex, parallel to
    // `mesh.vertices`. Zero at the top level and on a stitched seam.
    std::vector<core::Vec3> morphs;
    // **Who else draws each vertex**, parallel to `vertices`: bits 0 to 3 for
    // the sides it sits on (low x, high x, low z, high z; two for a corner),
    // and bits 4 to 6 for the level it was gathered at. A vertex on a seam is
    // drawn by every node there, and slides with the range they all agree on
    // (ADR 0140), or the seam opens while it slides.
    std::vector<core::u16> morphTags;

    // **How far this mesh is from the level-0 surface it stands for**, in
    // metres: its cells' largest error (`SurfaceCell::error`), zero at level
    // 0. What decides when a node shows its children (ADR 0140).
    float error = 0.0f;

    // The same surface as a plain position list and index triples, which is
    // what `ShapeType::TriangleMesh` takes.
    std::vector<core::Vec3> colliderPoints;
    std::vector<core::u32> colliderIndices;
};

// **A column's solid runs, as the sky term sees them** (ADR 0140): from the
// gathered surfaces over it -- each `(height, facing)`, facing the mean normal's
// y -- and the highest and lowest of the level's own vertices over it (NaN for
// none), the runs of ground as `(bottom, top)`, lowest first. A surface facing
// down opens a run and one facing up closes it; one steeper than either says
// nothing. Here so a test can hold its rules.
[[nodiscard]] std::vector<std::pair<float, float>> terrainColumnRuns(std::vector<std::pair<float, float>> surfaces,
                                                                     float top, float bottom);

// Extracts the surface of `field` over `region`. Writes nothing, so any number
// run at once over one field -- once the surfaces they read are prepared
// (`prepareRegion`, or `missingSurfaces` and the rest), whose keys fill the
// chunks' lazy digests.
[[nodiscard]] TerrainMesh meshField(const TerrainField& field, const MeshRegion& region);

// **Gathers one chunk's level-0 surface at `level`** (ADR 0140) into the
// field's cache, unless it is there for what the chunk and its neighbours hold
// now. `key` need not be a chunk the field stores: a surface can sit in the
// air beside one. **Not thread-safe**: done on one thread before meshing on
// several, which then only read.
void prepareSurface(const TerrainField& field, ChunkKey key, core::u32 level);

// Everything `meshField(field, region)` reads of the gathered surfaces,
// prepared, on one thread. `meshField` itself only reads the cache, and
// gathers what is missing for that call alone -- so a caller that meshes the
// same ground again prepares it first, and keeps what it gathered.
void prepareRegion(const TerrainField& field, const MeshRegion& region);

// **The same work, split so the costly part runs on many threads.**
// `missingSurfaces` appends the chunks `region` reads that the cache lacks,
// with the levels it lacks (bit `L` for level `L`; a chunk can repeat across
// calls); `surfaceContent` is what a chunk's surfaces are cached under;
// `buildSurfaces` gathers one chunk at the levels asked from one read of its
// voxels, and only reads the field, so any number run at once once
// `surfaceContent` has been asked for their keys on one thread (it fills the
// chunks' lazy digests); `cacheSurfaces` stores them, on one thread.
struct SurfaceWant
{
    ChunkKey key;
    core::u32 levels = 0;
};
using SurfaceLevels = std::array<std::shared_ptr<const SurfaceLevel>, ChunkLevels>;
void missingSurfaces(const TerrainField& field, const MeshRegion& region, std::vector<SurfaceWant>& out);
[[nodiscard]] core::u64 surfaceContent(const TerrainField& field, ChunkKey key) noexcept;
[[nodiscard]] SurfaceLevels buildSurfaces(const TerrainField& field, ChunkKey key, core::u32 levels);
void cacheSurfaces(const TerrainField& field, ChunkKey key, core::u64 content, const SurfaceLevels& surfaces);

// One chunk's gathered surface at `level`, as prepared, or null where it is
// not -- or where nothing is.
[[nodiscard]] const SurfaceLevel* surfaceOf(const TerrainField& field, ChunkKey key, core::u32 level) noexcept;

// **The runs of level-0 voxel rows a column of chunks can have a surface in**,
// lowest first and each inclusive, over the chunk columns from (`chunkX`,
// `chunkZ`) `across` wide and a ring of one chunk round them. A layer of chunks
// can when one of them is not all one value, or is solid with something other
// than solid ground against any face: air above, the air past the terrain's
// edge beside it, nothing under it -- which is what gives terrain walls and a
// bottom of one kind all round.
//
// What a renderer meshes a whole column of chunks by, one run at a time,
// without walking the solid rock between its surface and its bottom.
[[nodiscard]] std::vector<std::pair<core::i32, core::i32>> activeRuns(const TerrainField& field, core::i32 chunkX,
                                                                      core::i32 chunkZ, core::i32 across);

// The lowest and highest rows of `activeRuns`, or nothing when there are none.
[[nodiscard]] std::optional<std::pair<core::i32, core::i32>> activeRows(const TerrainField& field, core::i32 chunkX,
                                                                        core::i32 chunkZ, core::i32 across);

// Appends one mesh to another, keeping one section per material in id order.
void appendMesh(TerrainMesh& into, const TerrainMesh& from);

} // namespace engine::asset
