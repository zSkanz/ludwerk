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
[[nodiscard]] asset::Mesh riverRibbon(const std::vector<core::Vec3>& points, float width, float step);

class WaterLoader
{
public:
    // The grid, once, and each river's ribbon when its points change. Answers
    // how many meshes it built.
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
