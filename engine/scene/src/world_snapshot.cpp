// `World::snapshot` and `World::restore` (ADR 0016, and the promises
// `components.h`, `component_pool.h`, `value.h` and `enum_registry.h` were
// written against).
//
// There is no traversal here and that is the point. Every piece of a world's
// state is either a dense array of POD, a slot map of trivially copyable
// records, or a flat map of vectors -- so both directions are assignments, and
// the cost is proportional to what the world holds rather than to how deeply it
// is nested. The design work happened in the headers; this file is what those
// headers were for.
#include <algorithm>
#include <utility>
#include <vector>

#include "engine/scene/world.h"

namespace engine::scene {

WorldSnapshot World::snapshot() const
{
    WorldSnapshot out;
    eachPool(*this, out, [](const auto& pool, auto& target) { target = pool; });

    out.instances = m_instances;
    out.tagged = m_tagged;
    out.pendingRetire = m_pendingRetire;
    out.streamingFoci = m_streamingFoci;
    out.collisionGroups = m_collisionGroups;
    out.engineState = m_engineState;
    out.rngState = m_rng.state();
    out.rngIncrement = m_rng.increment();
    out.materialClones = m_materialClones;
    out.lastMaterialClone = m_lastMaterialClone;
    out.partShaderParameters = m_partShaderParameters;

    // The change queue is deliberately absent. A snapshot is taken at a frame
    // boundary where the queue is empty, and a queue captured anywhere else
    // would be a list of facts about a world the restore is about to replace.
    return out;
}

void World::restore(const WorldSnapshot& snapshot)
{
    // **The ground's revisions go forward, and its package is the disk's**
    // (terrain audit G2, P6 and R5). A revision put back to an older number is
    // one the renderer, the physics mirror and the navmesh have already seen
    // with other ground behind it, so after a restore each counts on from past
    // every number any of them has met. And `shipped` is the package's ground
    // as far as this world had loaded it: which keys comes back with the
    // world -- a key there and not in the field was dug, one not there was not
    // loaded yet -- but what is at them is what the disk holds now, which an
    // undo after a save does not change.
    u64 floor = m_groundRevisionFloor;
    std::vector<std::pair<core::InstanceId, asset::TerrainField>> terrainPackages;
    std::vector<std::pair<core::InstanceId, asset::VoxelGrid>> voxelPackages;
    m_terrains.forEach([&](core::InstanceId id, TerrainComponent& terrain) {
        floor = std::max({floor, terrain.fieldRevision, terrain.layersRevision});
        terrainPackages.emplace_back(id, std::move(terrain.shipped));
    });
    m_voxels.forEach([&](core::InstanceId id, VoxelComponent& voxels) {
        floor = std::max(floor, voxels.revision);
        voxelPackages.emplace_back(id, std::move(voxels.shipped));
    });

    eachPool(*this, snapshot, [](auto& pool, const auto& source) { pool = source; });

    floor += 1;
    m_groundRevisionFloor = floor;
    for (const auto& [id, package] : terrainPackages) {
        if (TerrainComponent* terrain = m_terrains.find(id); terrain != nullptr)
            terrain->shipped.refreshFrom(package);
    }
    for (const auto& [id, package] : voxelPackages) {
        if (VoxelComponent* voxels = m_voxels.find(id); voxels != nullptr)
            voxels->shipped.refreshFrom(package);
    }
    m_terrains.forEach([floor](core::InstanceId, TerrainComponent& terrain) {
        terrain.fieldRevision = std::max(terrain.fieldRevision, floor);
        terrain.layersRevision = std::max(terrain.layersRevision, floor);
    });
    m_voxels.forEach(
        [floor](core::InstanceId, VoxelComponent& voxels) { voxels.revision = std::max(voxels.revision, floor); });

    // Not an assignment: the instance map is the one container where putting
    // the bytes back is not enough, because generations decide what an
    // outstanding handle means. See `SlotMap::restoreFrom`.
    m_instances.restoreFrom(snapshot.instances);

    m_tagged = snapshot.tagged;
    m_pendingRetire = snapshot.pendingRetire;
    m_streamingFoci = snapshot.streamingFoci;
    m_collisionGroups = snapshot.collisionGroups;
    m_engineState = snapshot.engineState;
    m_rng.setState(snapshot.rngState, snapshot.rngIncrement);
    // The clones the restored parts wear come back with them. The holds do
    // not: they are the VM's, and the caller rebuilds the VM.
    m_materialClones = snapshot.materialClones;
    m_lastMaterialClone = snapshot.lastMaterialClone;
    m_partShaderParameters = snapshot.partShaderParameters;
    m_sweepMaterials = true;

    // Cleared rather than restored, and cleared rather than left alone: the
    // entries describe instances that may no longer exist, and the only thing
    // that reads them is the VM the caller rebuilds after this returns.
    m_changes.clear();
    ++m_restores;
    ++m_mutations;
}

} // namespace engine::scene
