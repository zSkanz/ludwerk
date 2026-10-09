#pragma once

#include <algorithm>

#include "engine/render/render_world.h"

namespace engine::render {

// Match readPlane's branch even while maps are loading. Recompute for the
// current view so another terrain or a material edit cannot lose hex sampling.
[[nodiscard]] inline bool terrainNeedsHexSampling(const RenderWorld& world)
{
    return std::any_of(world.terrains.begin(), world.terrains.end(), [](const RenderTerrain& terrain) {
        return std::any_of(terrain.layers.begin(), terrain.layers.end(),
                           [](const RenderTerrainLayer& layer) { return layer.tiling[2] > 0.5f; });
    });
}

[[nodiscard]] inline bool terrainIsTriplanarOnly(const RenderWorld& world)
{
    return std::all_of(world.terrains.begin(), world.terrains.end(), [](const RenderTerrain& terrain) {
        return std::all_of(terrain.layers.begin(), terrain.layers.end(), [](const RenderTerrainLayer& layer) {
            return (static_cast<core::u32>(layer.surface[3] + 0.5f) & 1u) != 0;
        });
    });
}

} // namespace engine::render
