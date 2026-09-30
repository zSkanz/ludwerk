#pragma once

// Terrain on the GPU (ADR 0082).
//
// **The ground is meshes**, built on the CPU from the voxels by the mesher the
// colliders use, and drawn like any static mesh with the terrain's own shader.
// There is no height atlas and no vertex-shader level of detail any more: a cave,
// an overhang and a hillside are the same data, so they are the same draw.
//
// **A quadtree of columns of chunks carries the level of detail.** A leaf is one
// column of 32-voxel chunks, meshed at full detail; a node at level L covers
// `2^L` by `2^L` leaves and is meshed from their level-0 surface gathered at
// level L (ADR 0140) -- a quarter of the triangles per level, flat ground where
// it is, and a thin wall thinned to a sheet rather than gone. Each node spans
// the whole height its chunks occupy, so two levels only ever meet on a
// vertical side, where the finer one is stitched to the coarser.
//
// **A change of level never shows a hole**: a node is drawn until all of its
// children are ready, and children until their parent is. Meshes are built
// nearest first, a fixed number a frame -- a count, never a clock -- and an edit
// rebuilds only the nodes whose chunks it changed.

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/asset/terrain_mesher.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/rhi/device.h"
#include "engine/scene/world.h"

namespace engine::render {

// One node of a terrain's quadtree: at `level`, covering the chunk columns
// `[x * 2^level, (x + 1) * 2^level)` by the same in z.
struct TerrainNodeKey
{
    core::u32 level = 0;
    core::i32 x = 0;
    core::i32 z = 0;

    [[nodiscard]] constexpr auto operator<=>(const TerrainNodeKey&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const TerrainNodeKey&) const noexcept = default;
};

// The coarsest level: a level-5 node is 32 by 32 chunk columns, a kilometre at
// the default voxel, meshed from each chunk's single level-5 value.
inline constexpr core::u32 TerrainTopLevel = asset::ChunkLevels - 1;

// The URN a node's mesh is filed under: `terrain://<slot>.<generation>/<level>/<x>,<z>`.
[[nodiscard]] std::string terrainNodeUrn(core::InstanceId terrain, TerrainNodeKey node);

// **The mesh one node is drawn with**, exactly as `sync` builds it -- the
// region, the level and the stitching. Here so a test can hold what is DRAWN against
// the field and the collider. Empty when the node has nothing to draw.
// **`sides`**: the level drawn beside each side and corner where it is coarser
// -- low x, high x, low z, high z, then the corners (`MeshRegion::sideLevels`);
// zero where not -- which the node stitches to (ADR 0140).
using TerrainSides = std::array<core::u8, 8>;
[[nodiscard]] asset::TerrainMesh meshTerrainNode(const asset::TerrainField& field, TerrainNodeKey node,
                                                 TerrainSides sides = {});
// **What meshing a node reads, prepared** (ADR 0140): its gathered surfaces,
// on one thread, before nodes are meshed on several.
void missingTerrainSurfaces(const asset::TerrainField& field, TerrainNodeKey node, TerrainSides sides,
                            std::vector<asset::SurfaceWant>& out);

// **Gathers the surfaces meshes are about to read** (ADR 0140), before they
// are meshed on many threads: each chunk named -- by field, repeats and all --
// is keyed on this thread, gathered on every thread, and cached on this one
// again. Called where `meshField` then runs in parallel, which only reads.
struct MissingSurface
{
    const asset::TerrainField* field = nullptr;
    asset::SurfaceWant want;
};
void gatherTerrainSurfaces(std::vector<MissingSurface> missing);

// **The level drawn beside each side of `key` where it is coarser** (ADR
// 0140), from the nodes drawn -- sorted. What the node is stitched to.
[[nodiscard]] TerrainSides terrainStitchSides(std::span<const TerrainNodeKey> drawn, TerrainNodeKey key) noexcept;

// **A node's geomorph, as the shaders get it** (ADR 0140): its own range, then
// the node drawn beside each side and corner if it is this level or coarser,
// each from `rangeOf` -- `(start, end)` metres, both zero for none. `drawn`
// sorted.
[[nodiscard]] TerrainMorph terrainMorphOf(std::span<const TerrainNodeKey> drawn, TerrainNodeKey key,
                                          const std::function<std::array<core::f32, 2>(TerrainNodeKey)>& rangeOf);
// **Where the shaders draw a vertex** of a node drawn with `morph`: its
// position, its offset to its parent's and its seams (`TerrainMesh::morphs`,
// `morphTags`), `distance` metres from the camera. What
// `engine_terrain_morph.hlsli` works out, in the same steps, so a test can
// hold two nodes' meshes against each other while they slide.
[[nodiscard]] core::Vec3 terrainSlid(core::Vec3 position, core::Vec3 offset, core::u16 tag, const TerrainMorph& morph,
                                     core::f32 distance) noexcept;

struct TerrainLodSettings
{
    // **Without a view** (`pixelScale` zero, as in a test): a node is split
    // into its children when the viewer is nearer to it than this many times
    // its own width. Two puts full detail out to 128 m at the default metre
    // voxel -- a node there is 32 m wide and its box is measured from its
    // nearest point -- and each level doubles it.
    double splitFactor = 2.0;
    // **With one** (ADR 0140): the node's error -- how far its mesh is from
    // the level-0 surface, or its cell's width until it is built -- projected,
    // may cover at most `pixelError` pixels, or the node shows its children.
    // `pixelScale` is pixels per metre at a metre's distance -- the viewport's
    // height over twice the tangent of half its vertical field of view -- so
    // `e` metres at distance `d` cover `e * pixelScale / d` pixels.
    double pixelScale = 0.0;
    double pixelError = 1.5;
    // Every node at its finest, whatever the distance: the reference a coarse
    // level is held against (`--terrain-detail=full`).
    bool fullDetail = false;
    // Nothing further than this is drawn.
    double viewDistance = 4096.0;
};

class TerrainLoader
{
public:
    // Brings the meshes of every terrain in `world` up to date for the current
    // focus, and decides which nodes are drawn this frame. Answers how many
    // meshes it built.
    core::u32 sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                   MeshCache& cache, MeshLibrary& library);

    // The nodes to draw for `world`, as the last `sync` of it chose them --
    // what `extract` turns into draws.
    [[nodiscard]] std::vector<TerrainNodeDraw> draws(const scene::World& world) const;

    // Appends one `RenderTerrain` per terrain in `world` under `root`: its
    // palette, which the terrain shader reads.
    // `textures` resolves each layer's maps; null leaves them unresolved, and
    // the terrain flat.
    void appendRenderTerrains(const scene::World& world, core::InstanceId root, RenderWorld& out,
                              const TextureLibrary* textures = nullptr) const;

    // **Where the viewer is**, which decides every level. Set once a frame from
    // the camera the last frame was drawn through. With no focus nothing is
    // drawn: a headless run with no camera has nobody to draw for.
    void setFocus(core::DVec3 focus) noexcept
    {
        m_focus = focus;
        m_hasFocus = true;
    }
    void clearFocus() noexcept { m_hasFocus = false; }

    void setLodSettings(const TerrainLodSettings& settings) noexcept { m_lod = settings; }
    [[nodiscard]] const TerrainLodSettings& lodSettings() const noexcept { return m_lod; }

    // How many meshes one `sync` may build. A count, never a clock.
    void setBuildsPerSync(core::u32 count) noexcept { m_buildsPerSync = count == 0 ? 1 : count; }

    // Releases everything this uploaded. Called once, by whoever owns it.
    void destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library);

    // How many nodes hold a mesh, across every terrain.
    [[nodiscard]] core::usize residentCount() const noexcept;
    // Whether one node of one terrain holds a mesh (it may be empty ground).
    [[nodiscard]] bool nodeResident(core::InstanceId terrain, TerrainNodeKey key) const noexcept;
    // Meshes built by the last `sync`, for tests and the perf overlay.
    [[nodiscard]] core::u32 lastBuilds() const noexcept { return m_lastBuilds; }
    // Whether the last `sync` left anything it wanted unbuilt.
    [[nodiscard]] bool pending() const noexcept { return m_pending; }

private:
    struct Node
    {
        const scene::World* world = nullptr;
        core::InstanceId terrain;
        TerrainNodeKey key;
        core::NameAtom urn;
        MeshHandle mesh;
        // What it was built from, and at which revision that was last checked.
        core::u64 content = 0;
        core::u64 revision = ~0ull;
        // Built at least once: a node of empty ground is built and has no mesh.
        bool built = false;
        // The coarser levels beside it it was built stitched to, and those it
        // is drawn beside now (ADR 0140): a node whose neighbours changed is
        // rebuilt, as one whose ground did.
        TerrainSides builtSides{};
        TerrainSides wantedSides{};
        // Whether its children were shown last frame: a node splits under the
        // error budget and joins again only past 1.25 times it (hysteresis).
        bool split = false;
        // How far its mesh is from the level-0 surface, in metres, once built
        // (`asset::TerrainMesh::error`).
        double error = 0.0;
        // The last frame this node was drawn or wanted.
        core::u64 used = 0;
    };

    [[nodiscard]] Node* find(const scene::World* world, core::InstanceId terrain, TerrainNodeKey key) noexcept;
    [[nodiscard]] const Node* find(const scene::World* world, core::InstanceId terrain,
                                   TerrainNodeKey key) const noexcept;
    void release(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library, Node& node);

    core::DVec3 m_focus;
    bool m_hasFocus = false;
    TerrainLodSettings m_lod;
    core::u32 m_buildsPerSync = 4;
    core::u32 m_lastBuilds = 0;
    bool m_pending = false;
    core::u64 m_frame = 0;

    // Sorted by (world, terrain index, key): never a hash map (R10).
    // **Looked up, not walked** (ADR 0140): a view drawn to its error budget
    // holds thousands of nodes, and a walk per lookup made the selection
    // quadratic. Keyed by world, terrain and node; the order of a map is never
    // what is drawn in (the selection's own order is).
    using NodeIndex = std::tuple<std::uintptr_t, core::u64, TerrainNodeKey>;
    [[nodiscard]] static NodeIndex indexOf(const scene::World* world, core::InstanceId terrain,
                                           TerrainNodeKey key) noexcept
    {
        return NodeIndex{reinterpret_cast<std::uintptr_t>(world),
                         (static_cast<core::u64>(terrain.index) << 32) | terrain.generation, key};
    }
    std::map<NodeIndex, Node> m_nodes;
    struct Drawn
    {
        const scene::World* world = nullptr;
        TerrainNodeDraw draw;
        // Where in the quadtree, so `draws` can tell which neighbours are
        // coarser.
        TerrainNodeKey key;
    };
    std::vector<Drawn> m_drawn;
};

} // namespace engine::render
