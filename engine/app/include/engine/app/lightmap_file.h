// **A scene's light baked into what stands still, kept** (ADR 0253 §6): the
// pages, the grid of the sun and each baked part's rectangles, as the file
// `Lighting.Baked` names and a game ships.
//
// A part is found again by where it stands in the scene -- its names from
// the workspace down -- and by where it stood and how large it was: a part
// that is not where the file says is not the part the light was baked for,
// and is drawn as a moving one.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "engine/app/lightmap_bake.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {
class World;
}

namespace engine::app {

struct KeptLightmapPart
{
    std::string path;
    core::CFrameD place;
    core::Vec3 size{1.0f, 1.0f, 1.0f};
    core::u32 columns = 0;
    core::u32 rows = 0;
    std::vector<LightmapRect> rects;
};

struct KeptLightmap
{
    core::Vec3 sunDirection{0.0f, 1.0f, 0.0f};
    core::f32 density = kLightmapDensity;
    core::u32 pageSize = 0;
    core::u32 pageCount = 0;
    // Four half floats a texel -- the light, over pi, and the mask -- page
    // after page, rows from the top. Or none, and each page as UASTC HDR
    // (`compressLightmap`): its light, and its mask as grey.
    std::vector<core::u16> texels;
    std::vector<std::vector<std::byte>> light;
    std::vector<std::vector<std::byte>> masks;
    std::vector<KeptLightmapPart> parts;
    LightmapSunGrid sunGrid;
};

// The file's bytes, and back. False for bytes that are not such a file, are
// of a version this build does not read, are cut short, or hold a number
// that is not one.
[[nodiscard]] std::vector<std::byte> writeLightmap(const KeptLightmap& kept);
[[nodiscard]] bool readLightmap(std::span<const std::byte> bytes, KeptLightmap& out);

// **The pages as UASTC HDR** (ADR 0253 §6): each page's light, and its mask
// as grey, a picture each, where it kept four half floats a texel -- a byte a
// texel on the card where those were eight, and a file a fraction of the
// size. False where this build has no encoder -- a game, which reads a bake
// and makes none -- or it could not, and `kept` as it was.
[[nodiscard]] bool compressLightmap(KeptLightmap& kept);

// **What a bake made, as the file keeps it**: each part by its names in
// `world`, the pages as half floats with the mask in their alpha.
[[nodiscard]] KeptLightmap keptLightmapOf(const scene::World& world, const LightmapBakeResult& baked);

// **And the file, as a bake of `world`**: each part it keeps found by its
// names, and where it stood and how large it was. One that is not found is
// left out, and said in `missing`.
[[nodiscard]] LightmapBakeResult lightmapFromKept(const scene::World& world, const KeptLightmap& kept,
                                                  core::u32& missing);

} // namespace engine::app
