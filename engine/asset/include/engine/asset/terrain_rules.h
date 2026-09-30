#pragma once

// **Rules paint a terrain by slope and height** (ADR 0113 §2).
//
// A terrain carries an ordered list of rules. Each says: where the ground is
// this steep and this high, draw it as that layer -- over the layers it names,
// with an edge this wide and this ragged. They are evaluated per pixel by the
// terrain shader, in order, each covering what came before, and they do NOT
// write voxels; `applyRules` does that, for an author who wants it permanent.
//
// **The game sees what is drawn**: the same evaluation runs here, on the CPU,
// from the same numbers and the same noise -- an integer hash, so the GPU and
// this agree bit for bit on the lattice -- which is what lets a raycast on
// steep grass drawn as rock say rock.
//
// A new terrain's first rule is the slope rock every terrain had before rules:
// steep ground turns to layer 3 over every layer but rock and basalt.

#include <array>
#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::asset {

class TerrainField;
struct EditReport;
struct Voxel;

struct TerrainRule
{
    bool enabled = true;
    // The layer the rule draws with, 1 to 255.
    core::u8 material = 3;
    // Degrees from level, 0 to 90.
    core::f32 slopeMin = 0.0f;
    core::f32 slopeMax = 90.0f;
    // World metres.
    core::f32 heightMin = -100000.0f;
    core::f32 heightMax = 100000.0f;
    // How wide the edge is: in degrees across a slope bound, in metres across a
    // height bound.
    core::f32 blend = 10.0f;
    // How ragged the edge is, 0 for a clean line.
    core::f32 noise = 0.12f;
    // The layers it may cover; empty is every layer but its own.
    std::vector<core::u8> appliesTo;

    [[nodiscard]] bool operator==(const TerrainRule&) const = default;
};

inline constexpr core::usize MaxTerrainRules = 16;

// What a new terrain's rules are: the slope rock of old, and nothing else.
[[nodiscard]] std::vector<TerrainRule> defaultTerrainRules();

// **A rule as the shader reads it**: the four edges of each band, already in
// the terms the pixel compares against -- slope as `1 - normal.y`, height in
// metres -- so the cosines are taken once, here, with the engine's own maths.
// An open bound is an edge nothing crosses.
struct TerrainRuleShape
{
    // Rising edge start, rising edge end, falling edge start, falling edge end.
    std::array<core::f32, 4> slope{};
    std::array<core::f32, 4> height{};
    core::f32 material = 0.0f;
    core::f32 noise = 0.0f;
    // Metres of height jitter per unit of noise.
    core::f32 heightJitter = 0.0f;
    core::f32 enabled = 0.0f;
    // Which layers it covers, one bit per id.
    std::array<core::u32, 8> appliesTo{};
};

[[nodiscard]] TerrainRuleShape shapeOf(const TerrainRule& rule) noexcept;

// The ragged-edge noise at a point of the field's own space, 0 to 1: value
// noise on a lattice of 4.3 m over x and z. The shader's `terrainRuleNoise` is
// this, line for line.
[[nodiscard]] core::f32 terrainRuleNoise(core::f32 x, core::f32 z) noexcept;

// How much of a point the rule covers, 0 to 1, before `appliesTo`: `normal` is
// the surface's, `ground` the point in field space, `worldY` its height in the
// world.
[[nodiscard]] core::f32 ruleCoverage(const TerrainRuleShape& shape, core::Vec3 normal, core::Vec3 ground,
                                     core::f32 worldY) noexcept;

[[nodiscard]] bool ruleCovers(const TerrainRuleShape& shape, core::u8 material) noexcept;

// **The layer a point is drawn as**, as the shader draws it (ADR 0113 §2, ADR
// 0114): the voxel's own, or what is painted over it where the paint covers
// half or more; then any rule that covers it by more than half, the last such
// rule winning -- the order they paint in. A rule covers by the voxel's own
// layer, as the shader weighs the triangle's corners, not by its paint.
[[nodiscard]] core::u8 drawnMaterial(std::span<const TerrainRule> rules, Voxel voxel, core::Vec3 normal,
                                     core::Vec3 ground, core::f32 worldY) noexcept;
// The same for a voxel with no paint.
[[nodiscard]] core::u8 drawnMaterial(std::span<const TerrainRule> rules, core::u8 material, core::Vec3 normal,
                                     core::Vec3 ground, core::f32 worldY) noexcept;

// **The rules, written into the voxels** of a box in field space: every solid
// voxel at the surface becomes what it is drawn as. `originY` places the field
// in the world, for the height bounds.
EditReport applyRules(TerrainField& field, std::span<const TerrainRule> rules, core::DVec3 minCorner,
                      core::DVec3 maxCorner, double originY);

} // namespace engine::asset
