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
// first; the instances go up as one buffer per tile. **And the frame does not
// wait for it** (`setAsync`, D542): a batch of tiles is handed over and put up
// by the `sync` that finds it done -- except a few tiles grown AGAIN, an edit
// somebody is watching, which are grown where they are found. What the frame draws is
// appended to the `RenderWorld` as runs (a tile's instances of one mesh) and
// buckets (one per mesh), which the renderer's cull turns into indirect draws.

#include <memory>
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
    // Of those, the ones the `sync` grew itself, the frame waiting for them.
    core::u32 tilesGrownInFrame = 0;
};

class FoliageSystem
{
public:
    FoliageSystem();
    ~FoliageSystem();
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

    // **Whether tiles are grown off the calling thread.** Not in a run that
    // takes a picture, which is of what the frames before it grew, not of what
    // a worker happened to finish by then -- the terrain loader's own rule.
    void setAsync(bool async) noexcept { m_async = async; }
    // Behind a loading curtain there is no frame of play to keep smooth: a
    // batch is as many tiles as there are.
    void setFastGrowth(bool fast) noexcept { m_fast = fast; }
    // Whether the last `sync` left a tile it wanted ungrown.
    [[nodiscard]] bool pending() const noexcept { return m_pending; }

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
        // Grown once: what it is asked for next is a change to what is drawn.
        bool grown = false;
    };
    // Tiles being grown off the calling thread.
    struct Batch;

    [[nodiscard]] Tile* find(const scene::World* world, core::InstanceId layer, core::i32 x, core::i32 z) noexcept;

    FoliageSettings m_settings;
    FoliageStats m_stats;
    core::DVec3 m_focus;
    bool m_hasFocus = false;
    bool m_async = false;
    bool m_fast = false;
    bool m_pending = false;
    // More tiles are being grown again than an edit is (`sync`).
    bool m_regrowing = false;
    std::vector<Tile> m_tiles;
    std::unique_ptr<Batch> m_batch;
};

} // namespace engine::render
