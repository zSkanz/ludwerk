#pragma once

// Foliage over terrain (ADR 0116): grown per tile, drawn by a GPU cull.
//
// **A tile is one column of terrain chunks**, grown for one `FoliageLayer`
// from that column's level-0 surface by `asset::growFoliage` -- a pure
// function, so nothing here is saved or replicated. A tile is grown when it
// comes within the layer's draw distance, grown again only when the chunks it
// was grown from change (a sculpt, a paint) or the layer does, and let go when
// the camera leaves it.
//
// **Growth runs on the job threads**, a bounded number of tiles a sync, nearest
// first; the instances go up as one buffer per tile. What the frame draws is
// appended to the `RenderWorld` as runs (a tile's instances of one mesh) and
// buckets (one per mesh), which the renderer's cull turns into indirect draws.

#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/rhi/device.h"

namespace engine::scene {
class World;
}

namespace engine::render {

class MeshLibrary;
class TextureLibrary;
struct RenderWorld;

struct FoliageSettings
{
    // `[render] foliage_density`: the fraction of every layer's instances
    // drawn, thinned in the stable order `FoliageInstance::random` gives.
    core::f32 density = 1.0f;
    // `[render] foliage_shadow_distance`: past it, foliage casts no shadow.
    core::f32 shadowDistance = 30.0f;
    // Tiles grown in one sync, at most. The rest wait for the next frame.
    core::u32 growthsPerSync = 8;
};

struct FoliageStats
{
    core::u32 tilesResident = 0;
    core::u32 instancesResident = 0;
    core::u32 tilesGrownLastSync = 0;
};

class FoliageSystem
{
public:
    FoliageSystem() = default;
    FoliageSystem(const FoliageSystem&) = delete;
    FoliageSystem& operator=(const FoliageSystem&) = delete;

    void setSettings(const FoliageSettings& settings) noexcept { m_settings = settings; }
    [[nodiscard]] const FoliageSettings& settings() const noexcept { return m_settings; }

    // Where tiles are wanted around: the camera, a frame late, as the terrain
    // loader's focus is.
    void setFocus(core::DVec3 focus) noexcept
    {
        m_focus = focus;
        m_hasFocus = true;
    }

    // Grows the tiles this focus wants and lets go of those it does not.
    // Outside any pass, because it uploads.
    void sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world);

    // What this frame draws of `world`: runs and buckets, and the materials of
    // the meshes -- a `FoliageMesh`'s own `Material` for every section when it
    // names one, the mesh's otherwise -- appended to `out.materials`.
    void append(const scene::World& world, const MeshLibrary& meshes, RenderWorld& out,
                const TextureLibrary* textures = nullptr) const;

    void destroy(rhi::IDevice& device);

    [[nodiscard]] const FoliageStats& stats() const noexcept { return m_stats; }

private:
    struct Tile
    {
        const scene::World* world = nullptr;
        core::InstanceId terrain;
        core::InstanceId layer;
        core::i32 x = 0;
        core::i32 z = 0;
        // What it was grown from: the chunks' digests and the layer's rules.
        core::u64 content = 0;
        core::u64 rules = 0;
        rhi::BufferHandle instances;
        core::u32 count = 0;
        // Each of the layer's meshes' run, as `FoliageTile::meshStart` gave it.
        std::vector<core::u32> meshStart;
        // The layer's meshes when it was grown, in order, so a run names its
        // bucket by instance.
        std::vector<core::InstanceId> meshes;
        bool wanted = false;
    };

    [[nodiscard]] Tile* find(const scene::World* world, core::InstanceId layer, core::i32 x, core::i32 z) noexcept;

    FoliageSettings m_settings;
    FoliageStats m_stats;
    core::DVec3 m_focus;
    bool m_hasFocus = false;
    std::vector<Tile> m_tiles;
};

} // namespace engine::render
