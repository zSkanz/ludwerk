#include "engine/render/foliage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "engine/asset/foliage.h"
#include "engine/asset/terrain.h"
#include "engine/asset/terrain_mesher.h"
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

// One tile's growth, run on a worker: everything it reads is fixed for the
// length of the sync.
struct Growth
{
    const asset::TerrainField* field = nullptr;
    f32 worldY = 0.0f;
    const std::vector<asset::TerrainRule>* terrainRules = nullptr;
    asset::FoliageRules rules;
    i32 x = 0;
    i32 z = 0;
    usize tile = 0;
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

    std::vector<Growth> growths;
    for (const Want& want : wants) {
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
        if (growths.size() >= m_settings.growthsPerSync)
            continue;
        const scene::TerrainComponent* terrain = world.terrains().find(want.terrain);
        const scene::FoliageLayerComponent* layer = world.foliageLayers().find(want.layer);
        Growth growth;
        growth.field = &terrain->field;
        growth.worldY = static_cast<f32>(terrain->origin.y);
        growth.terrainRules = &terrain->rules;
        tile->meshes = meshesOf(world, want.layer);
        growth.rules = placementOf(world, *layer, tile->meshes);
        growth.rules.voxelSize = terrain->field.settings().voxelSize;
        if (const std::vector<core::u8>* mask = maskOf(*layer, want.x, want.z); mask != nullptr)
            growth.rules.mask = *mask;
        growth.x = want.x;
        growth.z = want.z;
        growth.tile = static_cast<usize>(tile - m_tiles.data());
        tile->content = want.content;
        tile->rules = want.rules;
        growths.push_back(std::move(growth));
    }

    // The surfaces their ground reads, gathered first (ADR 0140): the tiles
    // then only read them.
    std::vector<MissingSurface> missing;
    std::vector<asset::SurfaceWant> surfaceWants;
    for (const Growth& growth : growths) {
        if (const std::optional<asset::MeshRegion> region = regionOf(growth); region.has_value()) {
            surfaceWants.clear();
            asset::missingSurfaces(*growth.field, *region, surfaceWants);
            for (const asset::SurfaceWant& want : surfaceWants)
                missing.push_back(MissingSurface{growth.field, want});
        }
    }
    gatherTerrainSurfaces(std::move(missing));
    jobs::parallelFor("foliage.grow", jobs::Domain::Render, 0, growths.size(), 1,
                      [&growths](usize begin, usize end, u32) noexcept {
                          for (usize at = begin; at < end; ++at)
                              grow(growths[at]);
                      });

    // Uploaded on this thread, nearest first.
    for (Growth& growth : growths) {
        Tile& tile = m_tiles[growth.tile];
        if (tile.instances.valid()) {
            device.destroy(tile.instances);
            tile.instances = {};
        }
        tile.count = static_cast<u32>(growth.grown.instances.size());
        tile.meshStart = growth.grown.meshStart;
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
    }

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
    for (Tile& tile : m_tiles) {
        if (tile.instances.valid())
            device.destroy(tile.instances);
    }
    m_tiles.clear();
}

} // namespace engine::render
