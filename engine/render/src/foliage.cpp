#include "engine/render/foliage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "engine/asset/foliage.h"
#include "engine/asset/terrain.h"
#include "engine/asset/terrain_mesher.h"
#include "engine/core/profile.h"
#include "engine/jobs/jobs.h"
#include "engine/render/render_world.h"
#include "engine/render/terrain_loader.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::u64;
using core::usize;

// FNV-1a over bytes, 64-bit: a fingerprint of what a tile was grown from, not
// a security property.
class Fingerprint
{
public:
    template <typename T>
    void add(const T& value) noexcept
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (usize at = 0; at < sizeof(T); ++at) {
            m_value ^= bytes[at];
            m_value *= 0x100000001B3ull;
        }
    }
    [[nodiscard]] u64 value() const noexcept { return m_value; }

private:
    u64 m_value = 0xCBF29CE484222325ull;
};

constexpr i32 Edge = static_cast<i32>(asset::ChunkEdge);

// The layer's meshes, in child order.
[[nodiscard]] std::vector<core::InstanceId> meshesOf(const scene::World& world, core::InstanceId layer)
{
    std::vector<core::InstanceId> meshes;
    for (core::InstanceId child = world.firstChild(layer); child.valid(); child = world.nextSibling(child)) {
        if (world.foliageMeshes().find(child) != nullptr)
            meshes.push_back(child);
    }
    return meshes;
}

// **What a layer grows by**: its rules, its meshes' weights and the terrain's
// own rules, which decide the material a triangle is drawn as.
[[nodiscard]] u64 rulesOf(const scene::World& world, const scene::FoliageLayerComponent& layer,
                          const scene::TerrainComponent& terrain, const std::vector<core::InstanceId>& meshes)
{
    Fingerprint print;
    print.add(layer.enabled);
    print.add(layer.density);
    print.add(layer.slopeMin);
    print.add(layer.slopeMax);
    print.add(layer.heightMin);
    print.add(layer.heightMax);
    print.add(layer.clumping);
    print.add(layer.minSpacing);
    print.add(layer.seed);
    for (const scene::FoliageMaterial& entry : layer.materials) {
        print.add(entry.material);
        print.add(entry.density);
    }
    for (const core::InstanceId mesh : meshes) {
        print.add(mesh.index);
        print.add(world.foliageMeshes().find(mesh)->weight);
    }
    for (const asset::TerrainRule& rule : terrain.rules) {
        print.add(rule.enabled);
        print.add(rule.material);
        print.add(rule.slopeMin);
        print.add(rule.slopeMax);
        print.add(rule.heightMin);
        print.add(rule.heightMax);
        print.add(rule.blend);
        print.add(rule.noise);
        for (const core::u8 layerId : rule.appliesTo)
            print.add(layerId);
    }
    const f64 originY = terrain.origin.y;
    print.add(originY);
    return print.value();
}

// **What a tile's surface is meshed from**: the chunks of its column and of
// the columns round it, which the mesher reads two voxels into.
[[nodiscard]] u64 contentOf(const asset::TerrainField& field, i32 x, i32 z)
{
    Fingerprint print;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            for (const asset::TerrainField::Entry& entry : field.column(x + dx, z + dz)) {
                print.add(entry.first.x);
                print.add(entry.first.y);
                print.add(entry.first.z);
                print.add(entry.second->digest());
            }
        }
    }
    return print.value();
}

// The density painted on one tile, or none.
[[nodiscard]] const std::vector<core::u8>* maskOf(const scene::FoliageLayerComponent& layer, i32 x, i32 z) noexcept
{
    for (const scene::FoliageMaskColumn& column : layer.mask) {
        if (column.x == x && column.z == z)
            return &column.density;
    }
    return nullptr;
}

// A tile's painted density, folded into what it was grown from: painting one
// tile regrows that tile.
[[nodiscard]] u64 withMask(u64 content, const std::vector<core::u8>* mask)
{
    if (mask == nullptr)
        return content;
    Fingerprint print;
    print.add(content);
    for (const core::u8 value : *mask)
        print.add(value);
    return print.value();
}

[[nodiscard]] asset::FoliageRules placementOf(const scene::World& world, const scene::FoliageLayerComponent& layer,
                                              const std::vector<core::InstanceId>& meshes)
{
    asset::FoliageRules rules;
    rules.density = layer.density;
    rules.slopeMin = layer.slopeMin;
    rules.slopeMax = layer.slopeMax;
    rules.heightMin = layer.heightMin;
    rules.heightMax = layer.heightMax;
    rules.clumping = layer.clumping;
    rules.minSpacing = layer.minSpacing;
    rules.seed = static_cast<u32>(static_cast<i32>(std::floor(layer.seed)));
    if (!layer.materials.empty()) {
        rules.materialDensity.assign(256, 0.0f);
        for (const scene::FoliageMaterial& entry : layer.materials)
            rules.materialDensity[entry.material] = entry.density;
    }
    for (const core::InstanceId mesh : meshes)
        rules.meshWeights.push_back(world.foliageMeshes().find(mesh)->weight);
    return rules;
}

// **A tile grown again is an edit while there are this few of them**: a brush
// stamp on the ground or on a layer's density touches a tile and the ring
// round it, and those are grown in the frame that finds them, as the terrain
// under them is. More -- a layer's rule changed, a script rewriting the
// ground -- is grown off the calling thread, as a tile never grown is.
constexpr usize MaxRegrowthsInFrame = 9;
// How many tiles a batch holds behind a loading curtain (`setFastGrowth`).
constexpr u32 FastGrowthsPerSync = 256;

// One tile's growth, run on a worker: everything it reads is fixed for as long
// as it runs -- the length of the sync, or a snapshot the batch holds.
struct Growth
{
    const asset::TerrainField* field = nullptr;
    f32 worldY = 0.0f;
    const std::vector<asset::TerrainRule>* terrainRules = nullptr;
    asset::FoliageRules rules;
    // The tile it is: by name, since a tile can be let go while it grows.
    const scene::World* world = nullptr;
    core::InstanceId layer;
    i32 x = 0;
    i32 z = 0;
    // What it was grown from, as the tile was asked for it.
    u64 content = 0;
    u64 print = 0;
    std::vector<core::InstanceId> meshes;
    asset::FoliageTile grown;
};

// The ground a tile grows on, or none where its column holds nothing.
[[nodiscard]] std::optional<asset::MeshRegion> regionOf(const Growth& growth)
{
    const std::optional<std::pair<i32, i32>> rows = asset::activeRows(*growth.field, growth.x, growth.z, 1);
    if (!rows.has_value())
        return std::nullopt;
    return asset::MeshRegion{
        .minX = growth.x * Edge,
        .minY = rows->first,
        .minZ = growth.z * Edge,
        .cellsX = static_cast<u32>(Edge),
        .cellsY = static_cast<u32>(rows->second - rows->first + 1),
        .cellsZ = static_cast<u32>(Edge),
        .level = 0,
    };
}

void grow(Growth& growth)
{
    const std::optional<asset::MeshRegion> region = regionOf(growth);
    if (!region.has_value())
        return;
    const asset::TerrainMesh surface = asset::meshField(*growth.field, *region);
    growth.grown = asset::growFoliage(surface, growth.worldY, *growth.terrainRules, growth.rules, growth.x, growth.z);
}

} // namespace

// **A batch of tiles grown off the calling thread** (D542). It holds what its
// growths read: the ground as it was when they were asked for, and each
// terrain's rules -- a script may write either while a worker reads.
struct FoliageSystem::Batch
{
    std::vector<Growth> growths;
    std::vector<std::shared_ptr<const asset::TerrainField>> fields;
    std::vector<std::shared_ptr<const std::vector<asset::TerrainRule>>> rules;
    std::vector<jobs::JobHandle> lanes;
    u32 laneCount = 1;

    // One lane's share: the surfaces a tile's ground reads, gathered and
    // cached as the terrain loader's lanes do, then the tile.
    void run(u32 lane) noexcept
    {
        std::vector<asset::SurfaceWant> wants;
        for (usize at = lane; at < growths.size(); at += laneCount) {
            Growth& growth = growths[at];
            if (const std::optional<asset::MeshRegion> region = regionOf(growth); region.has_value()) {
                wants.clear();
                asset::missingSurfaces(*growth.field, *region, wants);
                for (const asset::SurfaceWant& want : wants) {
                    const u64 content = asset::surfaceContent(*growth.field, want.key);
                    asset::cacheSurfaces(*growth.field, want.key, content,
                                         asset::buildSurfaces(*growth.field, want.key, want.levels));
                }
            }
            grow(growth);
        }
    }

    [[nodiscard]] bool finished() const noexcept
    {
        for (const jobs::JobHandle lane : lanes) {
            if (!jobs::finished(lane))
                return false;
        }
        return true;
    }
};

FoliageSystem::FoliageSystem() = default;

FoliageSystem::~FoliageSystem()
{
    if (m_batch != nullptr && !m_batch->lanes.empty())
        jobs::waitAll(m_batch->lanes);
}

FoliageSystem::Tile* FoliageSystem::find(const scene::World* world, core::InstanceId layer, i32 x, i32 z) noexcept
{
    for (Tile& tile : m_tiles) {
        if (tile.world == world && tile.layer == layer && tile.x == x && tile.z == z)
            return &tile;
    }
    return nullptr;
}

void FoliageSystem::sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world)
{
    m_stats.tilesGrownLastSync = 0;
    m_stats.tilesGrownInFrame = 0;
    core::profile::Sections stretch;

    // One tile's instances, up: on this thread, outside any pass.
    const auto putUp = [&](Growth& growth) {
        Tile* const found = find(growth.world, growth.layer, growth.x, growth.z);
        // Let go while it grew, or asked for again since -- an edit grown in
        // its own frame: this is of ground that is gone.
        if (found == nullptr || found->content != growth.content || found->rules != growth.print)
            return;
        Tile& tile = *found;
        if (tile.instances.valid()) {
            device.destroy(tile.instances);
            tile.instances = {};
        }
        tile.count = static_cast<u32>(growth.grown.instances.size());
        tile.meshStart = growth.grown.meshStart;
        tile.meshes = std::move(growth.meshes);
        tile.grown = true;
        if (tile.count != 0) {
            const auto bytes = static_cast<u32>(tile.count * sizeof(asset::FoliageInstance));
            tile.instances = device.createBuffer(
                {.usage = rhi::BufferUsage::ComputeStorageRead, .sizeBytes = bytes, .debugName = "foliage.tile"});
            if (tile.instances.valid()) {
                cmd.upload(tile.instances,
                           std::span<const std::byte>(reinterpret_cast<const std::byte*>(growth.grown.instances.data()),
                                                      bytes),
                           0);
            }
            else {
                tile.count = 0;
            }
        }
        m_stats.tilesGrownLastSync += 1;
    };

    // **A batch grown off this thread, done**: all of it goes up now, whichever
    // world this `sync` is of -- a tile is found by name.
    ENG_PROFILE_NEXT(stretch, "foliage.arrive");
    if (m_batch != nullptr && m_batch->finished()) {
        for (Growth& growth : m_batch->growths)
            putUp(growth);
        m_batch.reset();
    }

    ENG_PROFILE_NEXT(stretch, "foliage.choose");
    for (Tile& tile : m_tiles) {
        if (tile.world == &world)
            tile.wanted = false;
    }

    struct Want
    {
        f64 distance = 0.0;
        core::InstanceId terrain;
        core::InstanceId layer;
        i32 x = 0;
        i32 z = 0;
        u64 content = 0;
        u64 rules = 0;
    };
    std::vector<Want> wants;

    if (m_hasFocus) {
        world.terrains().forEach([&](core::InstanceId terrainId, const scene::TerrainComponent& terrain) {
            const f64 voxel = static_cast<f64>(terrain.field.settings().voxelSize);
            const f64 tileSize = voxel * static_cast<f64>(Edge);
            if (tileSize <= 0.0)
                return;
            // The focus in the field's own metres.
            const f64 fx = m_focus.x - terrain.origin.x;
            const f64 fz = m_focus.z - terrain.origin.z;
            for (core::InstanceId child = world.firstChild(terrainId); child.valid();
                 child = world.nextSibling(child)) {
                const scene::FoliageLayerComponent* layer = world.foliageLayers().find(child);
                if (layer == nullptr || !layer->enabled)
                    continue;
                const std::vector<core::InstanceId> meshes = meshesOf(world, child);
                if (meshes.empty())
                    continue;
                const u64 rules = rulesOf(world, *layer, terrain, meshes);
                const f64 reach = static_cast<f64>(layer->drawDistance);
                const i32 lowX = static_cast<i32>(std::floor((fx - reach) / tileSize));
                const i32 highX = static_cast<i32>(std::floor((fx + reach) / tileSize));
                const i32 lowZ = static_cast<i32>(std::floor((fz - reach) / tileSize));
                const i32 highZ = static_cast<i32>(std::floor((fz + reach) / tileSize));
                for (i32 z = lowZ; z <= highZ; ++z) {
                    for (i32 x = lowX; x <= highX; ++x) {
                        // The nearest point of the tile's square to the focus.
                        const f64 nx = std::clamp(fx, x * tileSize, (x + 1) * tileSize);
                        const f64 nz = std::clamp(fz, z * tileSize, (z + 1) * tileSize);
                        const f64 distance = std::sqrt((nx - fx) * (nx - fx) + (nz - fz) * (nz - fz));
                        if (distance > reach || terrain.field.column(x, z).empty())
                            continue;
                        wants.push_back(Want{distance, terrainId, child, x, z,
                                             withMask(contentOf(terrain.field, x, z), maskOf(*layer, x, z)), rules});
                    }
                }
            }
        });
    }

    // Nearest first, and in a fixed order among equals.
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        if (!(a.layer == b.layer))
            return a.layer.index < b.layer.index;
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });

    // **What to grow, and where.** A tile never grown is loading, and loads
    // off this thread when that is allowed, a batch at a time. A few tiles
    // grown AGAIN are an edit, and are grown here and now (`MaxRegrowthsInFrame`).
    struct Need
    {
        usize want = 0;
        bool regrowth = false;
    };
    std::vector<Need> needs;
    usize regrowths = 0;
    for (usize at = 0; at < wants.size(); ++at) {
        const Want& want = wants[at];
        Tile* tile = find(&world, want.layer, want.x, want.z);
        if (tile == nullptr) {
            Tile fresh;
            fresh.world = &world;
            fresh.terrain = want.terrain;
            fresh.layer = want.layer;
            fresh.x = want.x;
            fresh.z = want.z;
            // Never matches, so a new tile always grows.
            fresh.content = ~want.content;
            m_tiles.push_back(std::move(fresh));
            tile = &m_tiles.back();
        }
        tile->wanted = true;
        if (tile->content == want.content && tile->rules == want.rules)
            continue;
        needs.push_back(Need{at, tile->grown});
        regrowths += tile->grown ? 1u : 0u;
    }
    // **More than a few is not an edit, and stays not one while it lasts**:
    // what is left of every tile of a layer, a batch later, is a few tiles too.
    if (regrowths > MaxRegrowthsInFrame)
        m_regrowing = true;
    else if (regrowths == 0 && m_batch == nullptr)
        m_regrowing = false;
    const bool editHere = m_async && !m_regrowing && regrowths != 0;
    const usize awayLimit = m_batch != nullptr ? 0u : (m_fast ? FastGrowthsPerSync : m_settings.growthsPerSync);

    std::vector<Growth> here;
    std::unique_ptr<Batch> batch;
    std::vector<core::InstanceId> held;
    bool left = false;
    for (const Need& need : needs) {
        const Want& want = wants[need.want];
        const bool inFrame = !m_async || (need.regrowth && editHere);
        const usize handed = batch != nullptr ? batch->growths.size() : 0u;
        if (inFrame ? (!m_async && here.size() >= m_settings.growthsPerSync) : handed >= awayLimit) {
            left = true;
            continue;
        }
        Tile* const tile = find(&world, want.layer, want.x, want.z);
        const scene::TerrainComponent* terrain = world.terrains().find(want.terrain);
        const scene::FoliageLayerComponent* layer = world.foliageLayers().find(want.layer);
        Growth growth;
        growth.field = &terrain->field;
        growth.terrainRules = &terrain->rules;
        if (!inFrame) {
            if (batch == nullptr)
                batch = std::make_unique<Batch>();
            // The ground and the rules as they are now, one copy a terrain.
            usize slot = 0;
            while (slot < held.size() && !(held[slot] == want.terrain))
                slot += 1;
            if (slot == held.size()) {
                held.push_back(want.terrain);
                batch->fields.push_back(std::make_shared<const asset::TerrainField>(terrain->field));
                batch->rules.push_back(std::make_shared<const std::vector<asset::TerrainRule>>(terrain->rules));
            }
            growth.field = batch->fields[slot].get();
            growth.terrainRules = batch->rules[slot].get();
        }
        growth.worldY = static_cast<f32>(terrain->origin.y);
        growth.meshes = meshesOf(world, want.layer);
        growth.rules = placementOf(world, *layer, growth.meshes);
        growth.rules.voxelSize = terrain->field.settings().voxelSize;
        if (const std::vector<core::u8>* mask = maskOf(*layer, want.x, want.z); mask != nullptr)
            growth.rules.mask = *mask;
        growth.world = &world;
        growth.layer = want.layer;
        growth.x = want.x;
        growth.z = want.z;
        growth.content = want.content;
        growth.print = want.rules;
        tile->content = want.content;
        tile->rules = want.rules;
        (inFrame ? here : batch->growths).push_back(std::move(growth));
    }

    // **Here and now**: the surfaces their ground reads, gathered first (ADR
    // 0140) so the tiles then only read them; every worker, the frame waiting.
    if (!here.empty()) {
        ENG_PROFILE_NEXT(stretch, "foliage.surfaces");
        std::vector<MissingSurface> missing;
        std::vector<asset::SurfaceWant> surfaceWants;
        for (const Growth& growth : here) {
            if (const std::optional<asset::MeshRegion> region = regionOf(growth); region.has_value()) {
                surfaceWants.clear();
                asset::missingSurfaces(*growth.field, *region, surfaceWants);
                for (const asset::SurfaceWant& want : surfaceWants)
                    missing.push_back(MissingSurface{growth.field, want});
            }
        }
        gatherTerrainSurfaces(std::move(missing));
        ENG_PROFILE_NEXT(stretch, "foliage.grow");
        jobs::parallelFor("foliage.grow", jobs::Domain::Render, 0, here.size(), 1,
                          [&here](usize begin, usize end, u32) noexcept {
                              for (usize at = begin; at < end; ++at)
                                  grow(here[at]);
                          });
        // Uploaded on this thread, nearest first.
        ENG_PROFILE_NEXT(stretch, "foliage.upload");
        const u32 before = m_stats.tilesGrownLastSync;
        for (Growth& growth : here)
            putUp(growth);
        m_stats.tilesGrownInFrame = m_stats.tilesGrownLastSync - before;
    }

    // **And the rest handed over** (D542): a few lanes, never every worker --
    // the frame's own jobs run beside them -- and put up by the `sync` that
    // finds them done.
    ENG_PROFILE_NEXT(stretch, "foliage.hand");
    if (batch != nullptr) {
        Batch* const running = batch.get();
        running->laneCount =
            std::clamp<u32>(std::max(jobs::workerCount(), 2u) / 2u, 1u, static_cast<u32>(running->growths.size()));
        for (u32 lane = 0; lane < running->laneCount; ++lane)
            running->lanes.push_back(jobs::schedule("foliage.grow", jobs::Domain::Render,
                                                    [running, lane]() noexcept { running->run(lane); }));
        m_batch = std::move(batch);
    }
    m_pending = m_batch != nullptr || left;
    stretch.close();

    // Tiles of this world nobody wants any more let their instances go.
    for (usize at = m_tiles.size(); at > 0; --at) {
        Tile& tile = m_tiles[at - 1];
        if (tile.world != &world || tile.wanted)
            continue;
        if (tile.instances.valid())
            device.destroy(tile.instances);
        m_tiles.erase(m_tiles.begin() + static_cast<std::ptrdiff_t>(at - 1));
    }

    m_stats.tilesResident = 0;
    m_stats.instancesResident = 0;
    for (const Tile& tile : m_tiles) {
        m_stats.tilesResident += 1;
        m_stats.instancesResident += tile.count;
    }
}

void FoliageSystem::append(const scene::World& world, const MeshLibrary& meshes, RenderWorld& out,
                           const TextureLibrary* textures) const
{
    out.foliageDensity = std::clamp(m_settings.density, 0.0f, 1.0f);
    out.foliageShadowDistance = std::max(0.0f, m_settings.shadowDistance);
    // One bucket per `FoliageMesh` whose mesh has loaded, in the order tiles
    // name them.
    std::vector<core::InstanceId> bucketMeshes;
    const auto bucketOf = [&](core::InstanceId meshId) -> std::optional<u32> {
        for (usize at = 0; at < bucketMeshes.size(); ++at) {
            if (bucketMeshes[at] == meshId)
                return static_cast<u32>(at);
        }
        const scene::FoliageMeshComponent* mesh = world.foliageMeshes().find(meshId);
        if (mesh == nullptr || !mesh->mesh.valid())
            return std::nullopt;
        const MeshLibrary::Entry* entry = meshes.find(mesh->mesh);
        if (entry == nullptr || !entry->mesh.valid() || entry->sectionCount == 0)
            return std::nullopt;
        RenderFoliageBucket bucket;
        bucket.mesh = entry->mesh;
        bucket.meshMinY = entry->bounds.min.y;
        bucket.meshHeight = std::max(1e-3f, entry->bounds.max.y - entry->bounds.min.y);
        const auto reach = [](core::Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
        bucket.radius = std::max(reach(entry->bounds.min), reach(entry->bounds.max));
        bucket.windResponse = mesh->windResponse;
        bucket.stiffness = mesh->stiffness;
        bucket.castShadow = mesh->castShadow;
        if (const scene::FoliageLayerComponent* layer = world.foliageLayers().find(world.parentOf(meshId));
            layer != nullptr)
            bucket.receivesDecals = layer->receivesDecals;
        // Its own material, once for every section, when it names one.
        std::optional<u32> worn;
        if (mesh->material.valid()) {
            worn = static_cast<u32>(out.materials.size());
            out.materials.push_back(materialBlockOf(world, mesh->material, textures));
        }
        for (u32 section = 0; section < entry->sectionCount; ++section) {
            if (worn.has_value()) {
                bucket.sectionMaterials.push_back(*worn);
                continue;
            }
            const u32 local = section < entry->sectionMaterial.size() ? entry->sectionMaterial[section] : 0u;
            bucket.sectionMaterials.push_back(static_cast<u32>(out.materials.size()));
            out.materials.push_back(local < entry->materials.size() ? entry->materials[local] : RenderMaterial{});
        }
        bucketMeshes.push_back(meshId);
        out.foliageBuckets.push_back(std::move(bucket));
        return static_cast<u32>(out.foliageBuckets.size() - 1);
    };

    for (const Tile& tile : m_tiles) {
        if (tile.world != &world || tile.count == 0 || !tile.instances.valid())
            continue;
        const scene::TerrainComponent* terrain = world.terrains().find(tile.terrain);
        const scene::FoliageLayerComponent* layer = world.foliageLayers().find(tile.layer);
        if (terrain == nullptr || layer == nullptr || !layer->enabled)
            continue;
        for (usize m = 0; m < tile.meshes.size() && m + 1 < tile.meshStart.size(); ++m) {
            const u32 first = tile.meshStart[m];
            const u32 count = tile.meshStart[m + 1] - first;
            if (count == 0)
                continue;
            const std::optional<u32> bucket = bucketOf(tile.meshes[m]);
            if (!bucket.has_value())
                continue;
            const scene::FoliageMeshComponent* mesh = world.foliageMeshes().find(tile.meshes[m]);
            out.foliageBuckets[*bucket].capacity += count;
            out.foliageRuns.push_back(RenderFoliageRun{
                .instances = tile.instances,
                .first = first,
                .count = count,
                .bucket = *bucket,
                .origin = terrain->origin,
                .drawDistance = layer->drawDistance,
                .fadeDistance = layer->fadeDistance,
                .scaleMin = mesh->scaleMin,
                .scaleMax = std::max(mesh->scaleMin, mesh->scaleMax),
                .sink = mesh->sink,
                .alignToNormal = mesh->alignToNormal,
                .randomRotation = mesh->randomRotation,
            });
        }
    }
}

void FoliageSystem::destroy(rhi::IDevice& device)
{
    if (m_batch != nullptr && !m_batch->lanes.empty())
        jobs::waitAll(m_batch->lanes);
    m_batch.reset();
    for (Tile& tile : m_tiles) {
        if (tile.instances.valid())
            device.destroy(tile.instances);
    }
    m_tiles.clear();
}

} // namespace engine::render
