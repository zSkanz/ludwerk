#include "engine/asset/foliage.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace engine::asset {
namespace {

using core::f32;
using core::i32;
using core::u32;
using core::u64;
using core::u8;
using core::usize;
using core::Vec3;

// murmur3's finaliser: every input bit reaches every output bit.
[[nodiscard]] constexpr u32 mix(u32 h) noexcept
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// A draw in [0, 1), from the top 24 bits: exactly representable, so the same
// bits make the same float everywhere.
[[nodiscard]] f32 unit(u32 bits) noexcept
{
    return static_cast<f32>(bits >> 8) * (1.0f / 16777216.0f);
}

[[nodiscard]] Vec3 minus(Vec3 a, Vec3 b) noexcept
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec3 crossOf(Vec3 a, Vec3 b) noexcept
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// **The minimum spacing, as a grid of cells `spacing` on a side**: a candidate
// is refused when an instance already kept lies closer than `spacing` in its
// cell or a neighbour. Lookups only, in the order candidates arrive -- the
// map is never iterated, so its order cannot reach the output.
class Spacing
{
public:
    explicit Spacing(f32 spacing) noexcept : m_spacing(spacing) {}

    [[nodiscard]] bool admit(Vec3 point)
    {
        if (m_spacing <= 0.0f)
            return true;
        const i32 cx = cell(point.x);
        const i32 cy = cell(point.y);
        const i32 cz = cell(point.z);
        const f32 limit = m_spacing * m_spacing;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const auto found = m_cells.find(key(cx + dx, cy + dy, cz + dz));
                    if (found == m_cells.end())
                        continue;
                    for (const Vec3& other : found->second) {
                        const Vec3 d = minus(point, other);
                        if (d.x * d.x + d.y * d.y + d.z * d.z < limit)
                            return false;
                    }
                }
            }
        }
        m_cells[key(cx, cy, cz)].push_back(point);
        return true;
    }

private:
    [[nodiscard]] i32 cell(f32 value) const noexcept { return static_cast<i32>(std::floor(value / m_spacing)); }
    [[nodiscard]] static u64 key(i32 x, i32 y, i32 z) noexcept
    {
        return (static_cast<u64>(static_cast<u32>(x) & 0x1FFFFFu) << 42) |
               (static_cast<u64>(static_cast<u32>(y) & 0x1FFFFFu) << 21) |
               static_cast<u64>(static_cast<u32>(z) & 0x1FFFFFu);
    }

    f32 m_spacing;
    std::map<u64, std::vector<Vec3>> m_cells;
};

} // namespace

u32 foliageHash(u32 a, u32 b, u32 c, u32 d) noexcept
{
    return mix(a ^ mix(b ^ mix(c ^ mix(d + 0x9E3779B9u))));
}

FoliageTile growFoliage(const TerrainMesh& surface, f32 worldY, std::span<const TerrainRule> terrainRules,
                        const FoliageRules& rules, i32 tileX, i32 tileZ)
{
    FoliageTile tile;
    const usize meshCount = rules.meshWeights.size();
    f32 totalWeight = 0.0f;
    for (const f32 weight : rules.meshWeights)
        totalWeight += std::max(0.0f, weight);
    tile.meshStart.assign(meshCount + 1, 0);
    if (meshCount == 0 || totalWeight <= 0.0f || rules.density <= 0.0f)
        return tile;

    // The slope bounds as bounds on the normal's height, taken once.
    constexpr f32 Degrees = std::numbers::pi_v<f32> / 180.0f;
    const f32 steepest = std::cos(std::clamp(rules.slopeMax, 0.0f, 90.0f) * Degrees) - 1e-5f;
    const f32 flattest = std::cos(std::clamp(rules.slopeMin, 0.0f, 90.0f) * Degrees) + 1e-5f;

    const u32 tileSeed = foliageHash(rules.seed, static_cast<u32>(tileX), static_cast<u32>(tileZ), 0x464F4C49u);
    std::vector<std::vector<FoliageInstance>> byMesh(meshCount);
    Spacing spacing(rules.minSpacing);
    const Mesh& mesh = surface.mesh;
    u32 triangle = 0;

    for (usize section = 0; section < mesh.submeshes.size(); ++section) {
        const u8 voxelMaterial = section < surface.sectionMaterials.size() ? surface.sectionMaterials[section] : 0;
        const Submesh& submesh = mesh.submeshes[section];
        for (u32 at = submesh.firstIndex; at + 2 < submesh.firstIndex + submesh.indexCount; at += 3, ++triangle) {
            const Vertex& va = mesh.vertices[mesh.indices[at]];
            const Vertex& vb = mesh.vertices[mesh.indices[at + 1]];
            const Vertex& vc = mesh.vertices[mesh.indices[at + 2]];
            const Vec3 e1 = minus(vb.position, va.position);
            const Vec3 e2 = minus(vc.position, va.position);
            const Vec3 c = crossOf(e1, e2);
            const f32 length = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
            if (length <= 0.0f)
                continue;
            const Vec3 normal{c.x / length, c.y / length, c.z / length};
            if (normal.y < steepest || normal.y > flattest)
                continue;
            const Vec3 centre{(va.position.x + vb.position.x + vc.position.x) / 3.0f,
                              (va.position.y + vb.position.y + vc.position.y) / 3.0f,
                              (va.position.z + vb.position.z + vc.position.z) / 3.0f};
            const f32 height = worldY + centre.y;
            if (height < rules.heightMin || height > rules.heightMax)
                continue;
            // Under a roof -- a cave, an overhang -- nothing grows.
            if ((va.tangent[1] + vb.tangent[1] + vc.tangent[1]) / 3.0f < FoliageMinSky)
                continue;
            // By the material the ground is DRAWN as, which a rule may change.
            const u8 material = drawnMaterial(terrainRules, voxelMaterial, normal, centre, height);
            f32 multiplier = 1.0f;
            if (!rules.materialDensity.empty())
                multiplier = material < rules.materialDensity.size() ? rules.materialDensity[material] : 0.0f;
            if (multiplier <= 0.0f)
                continue;

            const f32 expected = 0.5f * length * rules.density * multiplier;
            const u32 triangleSeed = foliageHash(tileSeed, triangle, 0u, 0u);
            u32 count = static_cast<u32>(expected);
            if (unit(foliageHash(triangleSeed, 0xFFFFFFFFu, 0u, 0u)) < expected - static_cast<f32>(count))
                ++count;

            for (u32 k = 0; k < count; ++k) {
                const auto draw = [&](u32 channel) { return unit(foliageHash(triangleSeed, k, channel, 1u)); };
                f32 r1 = draw(1);
                f32 r2 = draw(2);
                if (r1 + r2 > 1.0f) {
                    r1 = 1.0f - r1;
                    r2 = 1.0f - r2;
                }
                const Vec3 point{va.position.x + e1.x * r1 + e2.x * r2, va.position.y + e1.y * r1 + e2.y * r2,
                                 va.position.z + e1.z * r1 + e2.z * r2};
                // Clumping: the ground's own noise, at a coarser scale, decides
                // where patches are; 0 ignores it.
                if (rules.clumping > 0.0f) {
                    const f32 patch =
                        std::clamp(terrainRuleNoise(point.x * 0.25f, point.z * 0.25f) * 2.0f - 0.5f, 0.0f, 1.0f);
                    const f32 keep = 1.0f + (patch - 1.0f) * rules.clumping;
                    if (draw(3) >= keep)
                        continue;
                }
                // The painted density, by the column the point stands in.
                if (!rules.mask.empty() && rules.voxelSize > 0.0f) {
                    constexpr auto edge = static_cast<i32>(ChunkEdge);
                    const i32 column = static_cast<i32>(std::floor(point.x / rules.voxelSize)) - tileX * edge;
                    const i32 row = static_cast<i32>(std::floor(point.z / rules.voxelSize)) - tileZ * edge;
                    const i32 cx = std::clamp(column, 0, edge - 1);
                    const i32 cz = std::clamp(row, 0, edge - 1);
                    const auto cell = static_cast<usize>(cz * edge + cx);
                    const f32 keep = cell < rules.mask.size() ? static_cast<f32>(rules.mask[cell]) / 255.0f : 1.0f;
                    if (draw(7) >= keep)
                        continue;
                }
                if (!spacing.admit(point))
                    continue;
                // Which mesh, by weight.
                f32 pick = draw(4) * totalWeight;
                usize chosen = meshCount - 1;
                for (usize m = 0; m < meshCount; ++m) {
                    const f32 weight = std::max(0.0f, rules.meshWeights[m]);
                    if (pick < weight) {
                        chosen = m;
                        break;
                    }
                    pick -= weight;
                }
                FoliageInstance instance;
                instance.position = point;
                instance.yaw = draw(5) * 2.0f * std::numbers::pi_v<f32>;
                instance.normal = normal;
                instance.random = draw(6);
                byMesh[chosen].push_back(instance);
            }
        }
    }

    for (usize m = 0; m < meshCount; ++m) {
        tile.meshStart[m] = static_cast<u32>(tile.instances.size());
        tile.instances.insert(tile.instances.end(), byMesh[m].begin(), byMesh[m].end());
    }
    tile.meshStart[meshCount] = static_cast<u32>(tile.instances.size());
    return tile;
}

} // namespace engine::asset
