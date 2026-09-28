#include "engine/asset/terrain_rules.h"

#include <algorithm>
#include <cmath>

#include "engine/asset/terrain.h"
#include "engine/core/dmath.h"

namespace engine::asset {
namespace {

using core::f32;
using core::u32;

// `1 - cos` of an angle in degrees, clamped to a quarter turn: the slope
// measure a pixel compares, `1 - normal.y`.
[[nodiscard]] f32 slopeMeasure(f32 degrees) noexcept
{
    const double clamped = std::clamp(static_cast<double>(degrees), 0.0, 90.0);
    return static_cast<f32>(1.0 - core::dmath::cos(clamped * 3.14159265358979323846 / 180.0));
}

[[nodiscard]] f32 saturate(f32 value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

// HLSL's `smoothstep`, for edges the shape guarantees apart.
[[nodiscard]] f32 smoothstep(f32 edge0, f32 edge1, f32 x) noexcept
{
    const f32 t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] u32 ruleHash(int x, int z) noexcept
{
    u32 h = static_cast<u32>(x) * 0x8DA6B343u ^ static_cast<u32>(z) * 0xD8163841u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

[[nodiscard]] f32 unitOf(u32 h) noexcept
{
    return static_cast<f32>(h & 0xFFFFFFu) / 16777215.0f;
}

// The band's edges: open below `low`'s bound, open above `high`'s. A million,
// not more: `OpenLow + 1` has to be a different float, or the edge is a
// division by zero on the GPU.
constexpr f32 OpenLow = -1.0e6f;
constexpr f32 OpenHigh = 1.0e6f;

void band(std::array<f32, 4>& out, f32 low, f32 high, f32 width, bool lowOpen, bool highOpen) noexcept
{
    const f32 half = std::max(width, 1.0e-4f) * 0.5f;
    out[0] = lowOpen ? OpenLow : low - half;
    out[1] = lowOpen ? OpenLow + 1.0f : low + half;
    out[2] = highOpen ? OpenHigh : high - half;
    out[3] = highOpen ? OpenHigh + 1.0f : high + half;
}

} // namespace

std::vector<TerrainRule> defaultTerrainRules()
{
    // The slope rock of old: `smoothstep(0.24, 0.36, 1 - normal.y)`, which is
    // about 40.5 to 50.2 degrees, with the clump noise's 0.12 on its edge --
    // over everything but rock (3) and basalt (7).
    TerrainRule rock;
    rock.material = 3;
    rock.slopeMin = 45.35f;
    rock.slopeMax = 90.0f;
    rock.blend = 9.7f;
    rock.noise = 0.12f;
    rock.appliesTo = {1, 2, 4, 5, 6, 8};
    return {rock};
}

TerrainRuleShape shapeOf(const TerrainRule& rule) noexcept
{
    TerrainRuleShape shape;
    // Slope bands in `1 - normal.y`: the blend is degrees, taken either side of
    // each bound before the cosine, so a wide blend widens the band evenly.
    const f32 half = std::max(rule.blend, 0.0f) * 0.5f;
    const bool lowOpen = rule.slopeMin <= 0.0f;
    const bool highOpen = rule.slopeMax >= 90.0f;
    shape.slope[0] = lowOpen ? OpenLow : slopeMeasure(rule.slopeMin - half);
    shape.slope[1] = lowOpen ? OpenLow + 1.0f : std::max(slopeMeasure(rule.slopeMin + half), shape.slope[0] + 1.0e-4f);
    shape.slope[2] = highOpen ? OpenHigh : slopeMeasure(rule.slopeMax - half);
    shape.slope[3] =
        highOpen ? OpenHigh + 1.0f : std::max(slopeMeasure(rule.slopeMax + half), shape.slope[2] + 1.0e-4f);
    band(shape.height, rule.heightMin, rule.heightMax, rule.blend, rule.heightMin <= -99999.0f,
         rule.heightMax >= 99999.0f);
    shape.material = static_cast<f32>(rule.material);
    shape.noise = std::max(rule.noise, 0.0f);
    // A slope edge moves by `noise` in `1 - normal.y`; a height edge by as
    // many blend-widths, so a ragged snow line is as ragged as its blend.
    shape.heightJitter = std::max(rule.blend, 1.0f) * 4.0f;
    shape.enabled = rule.enabled && rule.material != 0 ? 1.0f : 0.0f;
    if (rule.appliesTo.empty()) {
        shape.appliesTo.fill(0xFFFFFFFFu);
        shape.appliesTo[rule.material / 32u] &= ~(1u << (rule.material % 32u));
    }
    else {
        for (const core::u8 id : rule.appliesTo)
            shape.appliesTo[id / 32u] |= 1u << (id % 32u);
    }
    // Air is never covered.
    shape.appliesTo[0] &= ~1u;
    return shape;
}

f32 terrainRuleNoise(f32 x, f32 z) noexcept
{
    const f32 px = x * (1.0f / 4.3f);
    const f32 pz = z * (1.0f / 4.3f);
    const f32 cx = std::floor(px);
    const f32 cz = std::floor(pz);
    const f32 fx = px - cx;
    const f32 fz = pz - cz;
    const f32 ux = fx * fx * (3.0f - 2.0f * fx);
    const f32 uz = fz * fz * (3.0f - 2.0f * fz);
    const int ix = static_cast<int>(cx);
    const int iz = static_cast<int>(cz);
    const f32 a = unitOf(ruleHash(ix, iz));
    const f32 b = unitOf(ruleHash(ix + 1, iz));
    const f32 c = unitOf(ruleHash(ix, iz + 1));
    const f32 d = unitOf(ruleHash(ix + 1, iz + 1));
    const f32 near = a + (b - a) * ux;
    const f32 far = c + (d - c) * ux;
    return near + (far - near) * uz;
}

f32 ruleCoverage(const TerrainRuleShape& shape, core::Vec3 normal, core::Vec3 ground, f32 worldY) noexcept
{
    if (shape.enabled < 0.5f)
        return 0.0f;
    const f32 jitter = (terrainRuleNoise(ground.x, ground.z) - 0.5f) * shape.noise;
    const f32 slope = 1.0f - saturate(normal.y) + jitter;
    const f32 height = worldY + jitter * shape.heightJitter;
    const f32 inSlope =
        smoothstep(shape.slope[0], shape.slope[1], slope) * (1.0f - smoothstep(shape.slope[2], shape.slope[3], slope));
    const f32 inHeight = smoothstep(shape.height[0], shape.height[1], height) *
                         (1.0f - smoothstep(shape.height[2], shape.height[3], height));
    return inSlope * inHeight;
}

bool ruleCovers(const TerrainRuleShape& shape, core::u8 material) noexcept
{
    return (shape.appliesTo[material / 32u] & (1u << (material % 32u))) != 0;
}

core::u8 drawnMaterial(std::span<const TerrainRule> rules, core::u8 material, core::Vec3 normal, core::Vec3 ground,
                       f32 worldY) noexcept
{
    core::u8 drawn = material;
    for (const TerrainRule& rule : rules) {
        const TerrainRuleShape shape = shapeOf(rule);
        // What the rule covers is the voxel's own layer, as the shader weighs
        // the triangle's corners -- not what an earlier rule drew over it.
        if (ruleCovers(shape, material) && ruleCoverage(shape, normal, ground, worldY) > 0.5f)
            drawn = rule.material;
    }
    return drawn;
}

EditReport applyRules(TerrainField& field, std::span<const TerrainRule> rules, core::DVec3 minCorner,
                      core::DVec3 maxCorner, double originY)
{
    EditReport report;
    if (rules.empty())
        return report;
    const core::i32 minX = field.voxelIndex(std::min(minCorner.x, maxCorner.x));
    const core::i32 minY = field.voxelIndex(std::min(minCorner.y, maxCorner.y));
    const core::i32 minZ = field.voxelIndex(std::min(minCorner.z, maxCorner.z));
    const core::i32 maxX = field.voxelIndex(std::max(minCorner.x, maxCorner.x));
    const core::i32 maxY = field.voxelIndex(std::max(minCorner.y, maxCorner.y));
    const core::i32 maxZ = field.voxelIndex(std::max(minCorner.z, maxCorner.z));
    // The same bound every brush has (audit S10).
    const auto extent = [](core::i32 low, core::i32 high) {
        return high < low ? 0.0 : static_cast<double>(high) - static_cast<double>(low) + 1.0;
    };
    if (extent(minX, maxX) * extent(minY, maxY) * extent(minZ, maxZ) > static_cast<double>(MaxEditVoxels)) {
        report.refused = true;
        return report;
    }
    const auto occupancy = [&](core::i32 x, core::i32 y, core::i32 z) { return occupancyOf(field.voxel(x, y, z)); };

    FieldWriter writer(field);
    for (core::i32 z = minZ; z <= maxZ; ++z) {
        for (core::i32 y = minY; y <= maxY; ++y) {
            for (core::i32 x = minX; x <= maxX; ++x) {
                const Voxel voxel = writer.get(x, y, z);
                if (voxel.occupancy == 0 || voxel.material == 0)
                    continue;
                // **Only the surface**: a voxel with air beside it. The buried
                // ground is never drawn, and repainting it would change what a
                // later dig uncovers.
                const float own = occupancyOf(voxel);
                const bool surface = occupancy(x + 1, y, z) < 0.5f || occupancy(x - 1, y, z) < 0.5f ||
                                     occupancy(x, y + 1, z) < 0.5f || occupancy(x, y - 1, z) < 0.5f ||
                                     occupancy(x, y, z + 1) < 0.5f || occupancy(x, y, z - 1) < 0.5f || own < 1.0f;
                if (!surface)
                    continue;
                // The normal points out of the ground: down the occupancy's slope.
                core::Vec3 normal{occupancy(x - 1, y, z) - occupancy(x + 1, y, z),
                                  occupancy(x, y - 1, z) - occupancy(x, y + 1, z),
                                  occupancy(x, y, z - 1) - occupancy(x, y, z + 1)};
                const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                normal = length > 1.0e-5f ? core::Vec3{normal.x / length, normal.y / length, normal.z / length}
                                          : core::Vec3{0.0f, 1.0f, 0.0f};
                const core::Vec3 ground{static_cast<f32>(field.voxelCenter(x)), static_cast<f32>(field.voxelCenter(y)),
                                        static_cast<f32>(field.voxelCenter(z))};
                const auto worldY = static_cast<f32>(field.voxelCenter(y) + originY);
                const core::u8 drawn = drawnMaterial(rules, voxel.material, normal, ground, worldY);
                if (drawn != voxel.material)
                    writer.set(x, y, z, Voxel{voxel.occupancy, drawn});
            }
        }
    }
    writer.finish();
    report.touched = writer.changed();
    return report;
}

} // namespace engine::asset
