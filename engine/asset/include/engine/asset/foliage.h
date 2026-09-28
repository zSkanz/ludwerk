#pragma once

// Where foliage grows (ADR 0116).
//
// **A pure function of the ground, the rules and the seed.** Every machine
// that has the same terrain grows the same field, so nothing of it is saved,
// replicated or hashed: a replica, a hot reload and a streamed-in cell each
// grow it again and get the same instances.
//
// **One tile at a time**, a column of terrain chunks, from that column's own
// level-0 surface -- so an edit regrows only the tiles it touched, and tiles
// grow on the job threads independently of one another.
//
// **Integer randomness.** Every choice is drawn from an integer hash of the
// seed, the tile and the candidate, never from a float hash or a library
// generator, so the same seed gives the same instances on every compiler and
// CPU. Float arithmetic follows from those draws in a fixed order.

#include <span>
#include <vector>

#include "engine/asset/terrain_mesher.h"
#include "engine/asset/terrain_rules.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::asset {

// What a layer asks for, as the placement reads it.
struct FoliageRules
{
    core::f32 density = 2.0f;
    core::f32 slopeMin = 0.0f;
    core::f32 slopeMax = 35.0f;
    core::f32 heightMin = -10000.0f;
    core::f32 heightMax = 10000.0f;
    core::f32 clumping = 0.0f;
    core::f32 minSpacing = 0.25f;
    core::u32 seed = 0;
    // Multipliers of `density` by terrain material, indexed by id; empty is 1
    // for every material.
    std::vector<core::f32> materialDensity;
    // Each mesh's share, in the layer's order.
    std::vector<core::f32> meshWeights;
    // **The density painted on this tile**, a byte per voxel column, x
    // fastest, `ChunkEdge` squared: 255 keeps what the rules grow, 0 keeps
    // none. Empty is 255 everywhere.
    std::vector<core::u8> mask;
    // The voxel's side, for which column of the mask a point is in.
    core::f32 voxelSize = 1.0f;
};

// **The sky a point must see to grow**, of the mesher's sky term: below it the
// ground is under a roof -- a cave, an overhang.
inline constexpr core::f32 FoliageMinSky = 0.5f;

// One instance. 32 bytes, the layout the GPU reads it in.
struct FoliageInstance
{
    // In the field's own metres, like the mesh it was placed on.
    core::Vec3 position;
    // Turn about the up axis, in radians.
    core::f32 yaw = 0.0f;
    // The ground's normal where it stands, for `AlignToNormal`.
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    // 0 to 1, drawn once: the instance's own random value -- its scale within
    // the mesh's range, its sway phase, what a surface shader reads -- and its
    // rank for thinning. `[render] foliage_density` keeps the instances whose
    // `random` is below it, so lowering it thins the field rather than
    // reshuffling it.
    core::f32 random = 0.0f;
};
static_assert(sizeof(FoliageInstance) == 32, "the instance layout is a GPU buffer layout");

// A tile's instances, grouped by mesh: `meshStart[m]` is where mesh `m`'s run
// begins and `meshStart[m + 1]` where it ends.
struct FoliageTile
{
    std::vector<FoliageInstance> instances;
    std::vector<core::u32> meshStart;
};

// **Grows one tile** over `surface`, a level-0 terrain mesh in field space,
// whose `sectionMaterials` say each submesh's material. `worldY` is the field's
// origin height, for the height bounds and the rules; `terrainRules` are the
// terrain's own (a triangle grows by the material it is DRAWN as).
// `tileX` and `tileZ` name the tile in the seed.
[[nodiscard]] FoliageTile growFoliage(const TerrainMesh& surface, core::f32 worldY,
                                      std::span<const TerrainRule> terrainRules, const FoliageRules& rules,
                                      core::i32 tileX, core::i32 tileZ);

// The integer hash every choice above is drawn from, exposed for tests.
[[nodiscard]] core::u32 foliageHash(core::u32 a, core::u32 b, core::u32 c, core::u32 d) noexcept;

} // namespace engine::asset
