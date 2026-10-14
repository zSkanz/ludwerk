// The meshes water is drawn on (ADR 0118).
//
// **One flat grid, shared by every tile of every water**: a sea is drawn as
// rings of it round the camera, larger and coarser further out, and a lake as
// one stretched over its box -- the waves are the surface shader's, evaluated
// at each vertex's world column, so a tile holds no water of its own. A river
// has a ribbon along its points, rebuilt when they change.
#pragma once

#include <string>
#include <vector>

#include "engine/asset/model.h"
#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/rhi/device.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"

namespace engine::render {

// Quads across the shared grid: 96 m of sea at 0.375 m, as the ocean example
// drew it, for the finest ring.
inline constexpr core::u32 WaterGridQuads = 256;

[[nodiscard]] std::string waterGridUrn();
[[nodiscard]] std::string waterRiverUrn(core::InstanceId water);

// A flat square one unit across, centred on the origin, facing up.
[[nodiscard]] asset::Mesh waterGrid(core::u32 quads);
// A ribbon `width` wide along `points`' columns, at height zero, in world
// coordinates, a row every `step` metres or so.
// **A river's ribbon** (ADR 0146): a row of vertices across each sample of its
// course, as wide as the river is there and as high, square to the way it
// runs -- in the world's own coordinates. `step` is how far apart the
// vertices of a row are, at most.
[[nodiscard]] asset::Mesh riverRibbon(const scene::WaterCourse& course, float step);
// **A lake's surface**: a grid over its outline's bounds at `level`, the cells
// outside the outline left out and the vertices just outside it brought onto
// it, so the edge is the curve and the middle has vertices for the waves.
[[nodiscard]] asset::Mesh lakeSurface(const scene::WaterCourse& course, double level);

class WaterLoader
{
public:
    // The grid, once, and each river's ribbon and each lake's surface when its
    // points change. Answers how many meshes it built.
    core::u32 sync(rhi::IDevice& device, rhi::ICmdList& cmd, const scene::World& world, core::AtomTable& atoms,
                   MeshCache& cache, MeshLibrary& library);
    void destroy(rhi::IDevice& device, MeshCache& cache, MeshLibrary& library);

private:
    MeshHandle m_grid;
    core::NameAtom m_gridUrn;
    struct River
    {
        core::InstanceId water;
        core::u64 shape = 0;
        MeshHandle mesh;
        core::NameAtom urn;
        bool seen = false;
    };
    std::vector<River> m_rivers;
};

} // namespace engine::render
