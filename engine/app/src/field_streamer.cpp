#include "engine/app/field_streamer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

#include "engine/app/streaming_host.h"
#include "engine/app/terrain_cells.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/scene/components.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/voxel_fluid.h"
#include "engine/scene/world.h"

namespace engine::app {
namespace {

[[nodiscard]] f64 millisecondsSince(u64 startedNs)
{
    return static_cast<f64>(platform::nowNs() - startedNs) / 1.0e6;
}

} // namespace

void FieldStreamer::setIndex(const asset::ChunkIndex& index, const CellResolver& resolve)
{
    clearSession();
    m_paths.clear();
    for (const asset::ChunkIndexEntry& entry : index.chunks) {
        if (const std::optional<std::filesystem::path> path = resolve(entry); path.has_value())
            m_paths.emplace(entry.id, *path);
    }
    m_manager.setIndex(index);
    m_active = !index.chunks.empty();
    m_primed = !m_active;
    m_cellSourceStale = true;
    installCallbacks();
}

void FieldStreamer::reset()
{
    for (const asset::StreamingManager::ChunkView& cell : m_manager.view()) {
        if (cell.state == asset::ChunkState::Resident)
            evict(cell.id);
    }
    for (const auto& read : m_reads)
        platform::cancelIo(read.first);
    m_reads.clear();
    clearSession();
    m_paths.clear();
    m_terrainCells.clear();
    m_voxelCells.clear();
    m_readForEdit.clear();
    m_looseValid = false;
    m_manager.setIndex(asset::ChunkIndex{});
    m_active = false;
    m_primed = true;
    if (scene::TerrainComponent* component = m_world != nullptr ? terrain() : nullptr;
        component != nullptr && component->cellSource == m_cellSource)
        component->cellSource.reset();
    m_cellSource.reset();
    m_cellFiles.reset();
    m_cellsChanged.clear();
    m_cellSourceStale = true;
}

void FieldStreamer::adoptTerrain(const asset::ChunkIndex& index, const CellResolver& resolve, bool keepSession)
{
    if (!keepSession)
        clearSession();
    asset::ChunkIndex merged;
    merged.chunkSize = static_cast<core::f32>(asset::FieldCellMetres);
    // The session's rows and files, kept as they are over the new index.
    std::map<asset::ChunkId, std::pair<asset::ChunkIndexEntry, std::filesystem::path>> session;
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer != asset::FieldLayerTerrain)
            merged.chunks.push_back(entry);
        else if (m_session.contains(entry.id))
            session[entry.id] = {entry, m_paths[entry.id]};
    }
    for (auto at = m_paths.begin(); at != m_paths.end();) {
        at = at->first.layer == asset::FieldLayerTerrain ? m_paths.erase(at) : std::next(at);
    }
    for (const asset::ChunkIndexEntry& entry : index.chunks) {
        if (entry.id.layer != asset::FieldLayerTerrain || session.contains(entry.id))
            continue;
        merged.chunks.push_back(entry);
        if (const std::optional<std::filesystem::path> path = resolve(entry); path.has_value())
            m_paths.emplace(entry.id, *path);
    }
    for (const auto& kept : session) {
        merged.chunks.push_back(kept.second.first);
        m_paths[kept.first] = kept.second.second;
    }
    // A different terrain's cells are nobody's now: its field went with it.
    m_terrainCells.clear();
    m_looseValid = false;
    // Its bounds were drawn where it stood when they were saved.
    m_boundsOrigin.reset();
    if (!m_active) {
        m_manager.setIndex(merged);
        installCallbacks();
    }
    else {
        m_manager.replaceIndex(merged);
    }
    m_active = !merged.chunks.empty();
    m_primed = m_primed || !m_active;
    m_cellSourceStale = true;
}

void FieldStreamer::shareCells()
{
    scene::TerrainComponent* component = terrain();
    if (component == nullptr)
        return;
    if (m_cellSourceStale || (m_cellSource == nullptr && !m_cellsChanged.empty())) {
        m_cellSourceStale = false;
        m_cellsChanged.clear();
        std::vector<asset::ChunkId> cells;
        auto files = std::make_shared<CellFiles>();
        for (const auto& [id, path] : m_paths) {
            if (id.layer != asset::FieldLayerTerrain)
                continue;
            cells.push_back(id);
            files->paths.emplace(id, path);
        }
        const asset::FieldSettings settings = component->field.settings();
        // Read from any thread, by the renderer's builds: the files, under
        // their own lock, the settings a cell must have, and nothing else of
        // the streamer's. The file is read outside the lock.
        asset::TerrainCellSource::Reader read = [files,
                                                 settings](asset::ChunkId id) -> std::optional<asset::TerrainCell> {
            std::filesystem::path file;
            {
                const std::lock_guard<std::mutex> lock(files->lock);
                const auto path = files->paths.find(id);
                if (path == files->paths.end())
                    return std::nullopt;
                file = path->second;
            }
            std::vector<std::byte> bytes;
            if (!platform::readFile(file, bytes))
                return std::nullopt;
            asset::TerrainCell cell;
            if (asset::decodeTerrainCell(bytes, cell).has_value() || cell.settings.voxelSize != settings.voxelSize ||
                cell.settings.minHeight != settings.minHeight || cell.settings.maxHeight != settings.maxHeight)
                return std::nullopt;
            return cell;
        };
        m_cellFiles = cells.empty() ? nullptr : files;
        m_cellSource = cells.empty()
                           ? nullptr
                           : std::make_shared<asset::TerrainCellSource>(asset::terrainCellChunks(settings.voxelSize),
                                                                        std::move(cells), std::move(read));
    }
    else if (!m_cellsChanged.empty()) {
        // **The source is told, not made again** (ADR 0149): what it kept of
        // every other cell stands.
        std::vector<asset::ChunkId> changed;
        std::vector<asset::ChunkId> gone;
        {
            const std::lock_guard<std::mutex> lock(m_cellFiles->lock);
            for (const asset::ChunkId id : m_cellsChanged) {
                if (const auto path = m_paths.find(id); path != m_paths.end()) {
                    m_cellFiles->paths[id] = path->second;
                    changed.push_back(id);
                }
                else {
                    m_cellFiles->paths.erase(id);
                    gone.push_back(id);
                }
            }
        }
        m_cellSource->update(changed, gone);
        m_cellsChanged.clear();
    }
    component->cellSource = m_cellSource;
}

void FieldStreamer::reconcile()
{
    if (m_world == nullptr || m_terrainCells.empty())
        return;
    scene::TerrainComponent* component = terrain();
    if (component == nullptr)
        return;
    // Less what was removed on purpose: a chunk dug to nothing in the world
    // put back is gone there, and the cell's copy of it must not return it
    // (terrain audit G2).
    for (const auto& held : m_terrainCells) {
        component->field.shareFrom(held.second.field, component->shipped);
        component->shipped.shareFrom(held.second.field);
    }
    component->fieldRevision += 1;
    // What a kept cell held is whatever the snapshot held of it.
    m_keptWhole.clear();
    m_looseValid = false;
}

FieldStreamer::TerrainSaveReport FieldStreamer::saveTerrain(const TerrainCellWriter& writer)
{
    TerrainSaveReport report;
    scene::TerrainComponent* component = m_world != nullptr ? terrain() : nullptr;
    if (component == nullptr) {
        report.ok = false;
        return report;
    }
    // The rows kept as they are must stand where the ones written will.
    followTerrainOrigin();
    const asset::FieldSettings settings = component->field.settings();
    const core::u32 across = asset::terrainCellChunks(settings.voxelSize);

    // What the index holds now, terrain rows only, by id.
    std::map<asset::ChunkId, asset::ChunkIndexEntry> rows;
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer == asset::FieldLayerTerrain)
            rows.emplace(entry.id, entry);
    }

    std::vector<asset::TerrainCell> cells = asset::splitTerrain(component->field);
    std::set<asset::ChunkId> present;
    for (asset::TerrainCell& cell : cells) {
        const asset::ChunkId id{cell.x, cell.z, asset::FieldLayerTerrain};
        present.insert(id);
        const auto held = m_terrainCells.find(id);
        // **A cell in the session cache is written whatever it looks like**
        // (ADR 0149): untouched since it streamed back, it is untouched
        // against the cache's copy, and the scene's file is the older one.
        if (held != m_terrainCells.end() && !m_session.contains(id) &&
            asset::terrainCellUntouched(component->field, held->second, across)) {
            report.unchanged += 1;
            continue;
        }
        // **Ground the field does not hold is still the cell's.** A cell that
        // is not resident -- nobody loaded it, or it was let go -- may still
        // have ground written into its square (Generate Flat Ground over a
        // large square reaches past what is loaded). The file is the rest of
        // it, and what the field holds wins, because it is newer.
        //
        // **A file that will not read is not written over** (audit A12): the
        // cell kept only its loaded part, and everything else the file held
        // was gone. It stays as it is on disk, its row with it, and the save
        // says not everything was written.
        if (held == m_terrainCells.end()) {
            if (const auto row = rows.find(id); row != rows.end() && writer.read) {
                // The rest of a cell in the session cache is in the cache.
                std::optional<std::vector<std::byte>> bytes;
                if (m_session.contains(id)) {
                    std::vector<std::byte> cached;
                    if (const auto path = m_paths.find(id);
                        path != m_paths.end() && platform::readFile(path->second, cached))
                        bytes = std::move(cached);
                }
                else {
                    bytes = writer.read(row->second);
                }
                asset::TerrainCell onDisk;
                if (!bytes.has_value() || asset::decodeTerrainCell(*bytes, onDisk).has_value()) {
                    report.ok = false;
                    continue;
                }
                // Less every chunk this session loaded and then removed:
                // the file still holds it, and the field is the truth
                // (terrain audit G3).
                cell.field.shareFrom(onDisk.field, component->shipped);
            }
        }
        cell.settings = settings;
        // **More chunks than a cell may load is not written** (terrain audit
        // TA16f): it was checked on decode only, so a cell saved past it never
        // loaded again -- and the next save, refusing to write over a file it
        // could not read, lost the ground. It stays as it is on disk, and the
        // save says not everything was written.
        if (cell.field.chunks().size() > asset::MaxCellChunks) {
            report.ok = false;
            continue;
        }
        const std::vector<std::byte> bytes = asset::encodeTerrainCell(cell);
        const std::optional<std::string> urn = writer.write(id, bytes);
        if (!urn.has_value()) {
            report.ok = false;
            continue;
        }
        asset::ChunkIndexEntry entry;
        entry.id = id;
        entry.bounds = asset::terrainCellBounds(cell, across, component->origin);
        entry.urn = *urn;
        entry.bytes = static_cast<core::u32>(bytes.size());
        rows[id] = entry;
        // The cache's copy of it, which the scene's now replaces.
        std::optional<std::filesystem::path> cachedFile;
        if (const auto path = m_paths.find(id); path != m_paths.end() && m_session.contains(id))
            cachedFile = path->second;
        if (writer.resolve) {
            if (const std::optional<std::filesystem::path> path = writer.resolve(entry); path.has_value())
                m_paths[id] = *path;
        }
        // An ordinary cell again: what it holds is what its file holds, so the
        // camera moving away may let it go.
        m_terrainCells[id] = std::move(cell);
        report.written += 1;
        // And what the far ground kept of it is of the file before (D406).
        m_cellsChanged.insert(id);
        m_keptWhole.erase(id);
        m_sessionOver.erase(id);
        m_session.erase(id);
        if (m_layer.has_value())
            m_layer->before.erase(id);
        if (cachedFile.has_value()) {
            std::error_code ignored;
            std::filesystem::remove(*cachedFile, ignored);
        }
    }

    // **The session cache, committed** (ADR 0149): every cell still in it is
    // one nobody is near -- its bytes go to the scene's own cells as they
    // are, its row names the scene's file, and the cache lets it go.
    const std::set<asset::ChunkId> cached = m_session;
    for (const asset::ChunkId id : cached) {
        const auto path = m_paths.find(id);
        const auto row = rows.find(id);
        std::vector<std::byte> bytes;
        std::optional<std::string> urn;
        core::u64 size = 0;
        if (path != m_paths.end() && row != rows.end()) {
            // Moved where it can be, copied where it cannot.
            std::error_code sizeError;
            const std::uintmax_t onDisk = std::filesystem::file_size(path->second, sizeError);
            if (writer.adopt && !sizeError)
                urn = writer.adopt(id, path->second);
            if (urn.has_value()) {
                size = onDisk;
            }
            else if (platform::readFile(path->second, bytes)) {
                urn = writer.write(id, bytes);
                size = bytes.size();
            }
        }
        if (!urn.has_value()) {
            // Not written: the save says so, and the cell stays the cache's.
            report.ok = false;
            if (row != rows.end())
                rows.erase(row);
            continue;
        }
        const std::filesystem::path was = path->second;
        row->second.urn = *urn;
        row->second.bytes = static_cast<core::u32>(size);
        if (writer.resolve) {
            if (const std::optional<std::filesystem::path> resolved = writer.resolve(row->second); resolved.has_value())
                m_paths[id] = *resolved;
        }
        m_session.erase(id);
        m_sessionOver.erase(id);
        if (m_layer.has_value())
            m_layer->before.erase(id);
        std::error_code ignored;
        std::filesystem::remove(was, ignored);
        report.written += 1;
        m_cellsChanged.insert(id);
    }

    // A cell this streamer loaded whose square is now empty was dug to
    // nothing: its file goes, and its row with it.
    for (auto held = m_terrainCells.begin(); held != m_terrainCells.end();) {
        if (present.contains(held->first)) {
            ++held;
            continue;
        }
        // Its file goes only once the index that no longer names it is on
        // disk (audit A12): the caller removes what `emptied` lists after
        // writing the index, so a save that stops half way never leaves an
        // index naming files that are gone.
        if (const auto row = rows.find(held->first); row != rows.end()) {
            report.emptied.push_back(row->second);
            rows.erase(row);
            report.removed += 1;
        }
        m_paths.erase(held->first);
        m_cellsChanged.insert(held->first);
        held = m_terrainCells.erase(held);
    }

    // **A cell dug to nothing after it was let go** is empty too: the field
    // holds none of it, and the package's copy says it was loaded -- so what
    // its file holds is ground somebody removed (terrain audit G3). A cell
    // never loaded is simply not here, and keeps its file.
    std::set<asset::ChunkId> loaded;
    for (const asset::TerrainField::Entry& entry : component->shipped.chunks())
        loaded.insert(asset::terrainCellOf(entry.first, across));
    for (auto row = rows.begin(); row != rows.end();) {
        if (present.contains(row->first) || m_terrainCells.contains(row->first) || !loaded.contains(row->first)) {
            ++row;
            continue;
        }
        report.emptied.push_back(row->second);
        report.removed += 1;
        m_paths.erase(row->first);
        m_cellsChanged.insert(row->first);
        row = rows.erase(row);
    }

    report.index.chunkSize = static_cast<core::f32>(asset::FieldCellMetres);
    for (const auto& row : rows)
        report.index.chunks.push_back(row.second);

    asset::ChunkIndex merged = report.index;
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer != asset::FieldLayerTerrain)
            merged.chunks.push_back(entry);
    }
    if (!m_active) {
        m_manager.setIndex(merged);
        installCallbacks();
    }
    else {
        m_manager.replaceIndex(merged);
    }
    m_active = !merged.chunks.empty();
    return report;
}

void FieldStreamer::setWorld(scene::World* world, core::InstanceId workspace)
{
    if (m_world == world && m_workspace == workspace)
        return;
    // **Another world**: what this one changed and did not save went with it,
    // in the cache as in memory (ADR 0149).
    if (m_world != world)
        dropSession();
    m_world = world;
    m_workspace = workspace;
    m_terrainCells.clear();
    m_voxelCells.clear();
    m_readForEdit.clear();
    m_looseValid = false;
    m_manager.forgetResidency();
    // A new world has none of its ground yet, and waits for it like the first.
    m_primed = !m_active;
    m_waitingSinceNs = platform::nowNs();
}

void FieldStreamer::installCallbacks()
{
    asset::StreamingCallbacks callbacks;
    callbacks.beginLoad = [this](asset::ChunkId id, const asset::ChunkIndexEntry&) {
        // Refused rather than failed when the IO service is saturated, on
        // `StreamingHost`'s terms: a full queue is not a broken cell.
        const platform::IoStats io = platform::ioStats();
        if (io.queued + io.inFlight >= platform::MaxIoRequests / 2)
            return false;
        // **High, above the parts' Normal**: a missing collider under a
        // character is a fall, and a missing prop is not.
        const auto path = m_paths.find(id);
        const platform::IoRequest request = path != m_paths.end()
                                                ? platform::readFileAsync(path->second, platform::IoPriority::High)
                                                : platform::IoRequest{};
        // A start that failed is reported AFTER the tick (D158): reported here,
        // the manager would mark the cell loading as this returns and it would
        // wait for a read that never began.
        if (!request.valid())
            m_failedStarts.push_back(id);
        else
            m_reads.emplace_back(request, id);
        return true;
    };
    callbacks.materializeBytes = [this](asset::ChunkId id, std::span<const std::byte> bytes) {
        return materialize(id, bytes);
    };
    callbacks.evict = [this](asset::ChunkId id) { evict(id); };
    m_manager.setCallbacks(std::move(callbacks));
}

scene::TerrainComponent* FieldStreamer::terrain() const
{
    // The one directly under the workspace, which is where a `Terrain` lives.
    scene::TerrainComponent* found = nullptr;
    m_world->terrains().forEach([&](core::InstanceId id, scene::TerrainComponent& component) {
        if (found == nullptr && m_world->parentOf(id) == m_workspace)
            found = &component;
    });
    return found;
}

scene::VoxelComponent* FieldStreamer::voxels() const
{
    scene::VoxelComponent* found = nullptr;
    m_world->voxels().forEach([&](core::InstanceId, scene::VoxelComponent& component) {
        if (found == nullptr)
            found = &component;
    });
    return found;
}

bool FieldStreamer::loadNow(core::DVec3 low, core::DVec3 high, core::u32 maxCells)
{
    if (!m_active || m_world == nullptr)
        return true;
    followTerrainOrigin();
    // The cells over the square not held yet, counted before any is read: an
    // edit that cannot have all of them has none, and refuses untouched.
    std::vector<const asset::ChunkIndexEntry*> wanted;
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        const bool ground = entry.id.layer == asset::FieldLayerTerrain;
        if (!ground && entry.id.layer != asset::FieldLayerVoxels)
            continue;
        if (ground ? m_terrainCells.contains(entry.id) : m_voxelCells.contains(entry.id))
            continue;
        if (entry.bounds.max.x < low.x || entry.bounds.min.x > high.x || entry.bounds.max.z < low.z ||
            entry.bounds.min.z > high.z)
            continue;
        wanted.push_back(&entry);
        if (wanted.size() > maxCells)
            return false;
    }
    for (const asset::ChunkIndexEntry* entry : wanted) {
        const auto path = m_paths.find(entry->id);
        std::vector<std::byte> bytes;
        // A cell that cannot be read is reported where the streamer reports
        // one, and stays out: nothing better is on disk to wait for.
        if (path == m_paths.end() || !platform::readFile(path->second, bytes))
            continue;
        (void)materialize(entry->id, bytes);
        m_readForEdit.insert(entry->id);
    }
    return true;
}

f64 FieldStreamer::materialize(asset::ChunkId id, std::span<const std::byte> bytes)
{
    const u64 started = platform::nowNs();
    // **Held already**, read for an edit (`loadNow`) before this read came
    // back: the field has all of it that was not removed on purpose.
    if (id.layer == asset::FieldLayerTerrain ? m_terrainCells.contains(id) : m_voxelCells.contains(id))
        return millisecondsSince(started);
    if (id.layer == asset::FieldLayerTerrain) {
        asset::TerrainCell cell;
        if (asset::decodeTerrainCell(bytes, cell).has_value())
            return -1.0;
        // A world whose script removed its terrain has nowhere to put the
        // ground; the cell counts as resident, and costs what reading it did.
        if (scene::TerrainComponent* component = terrain(); component != nullptr) {
            // **A cell of other settings is refused, never merged** (terrain
            // audit TA16b): its voxels are another size, or its band another
            // height, and merged they were ground at the wrong scale -- saved
            // that way. It is reported as one that could not be read.
            const asset::FieldSettings& mine = component->field.settings();
            if (cell.settings.voxelSize != mine.voxelSize || cell.settings.minHeight != mine.minHeight ||
                cell.settings.maxHeight != mine.maxHeight)
                return -1.0;
            // **Less every chunk the package's copy already holds**: the
            // field has it, or it was removed on purpose -- dug to nothing,
            // or removed by the authority -- and a cell that went out and came
            // back must not bring it back (terrain audit G1).
            component->field.shareFrom(cell.field, component->shipped);
            // What the package ships, for the ground's replication (ADR 0135).
            component->shipped.shareFrom(cell.field);
            component->fieldRevision += 1;
            m_terrainCells[id] = std::move(cell);
        }
        return millisecondsSince(started);
    }

    asset::VoxelCell cell;
    if (asset::decodeVoxelCell(bytes, cell).has_value())
        return -1.0;
    if (scene::VoxelComponent* component = voxels(); component != nullptr) {
        component->grid.shareFrom(cell.grid, component->shipped);
        component->shipped.shareFrom(cell.grid);
        component->revision += 1;
        // Its water was saved without the steps it was due. A still lake
        // settles in one look; water that moves makes the cell one somebody
        // changed, which is what it now is.
        scene::wakeFluidsIn(*component, cell.grid);
        m_voxelCells[id] = std::move(cell);
    }
    return millisecondsSince(started);
}

void FieldStreamer::evict(asset::ChunkId id)
{
    if (id.layer == asset::FieldLayerTerrain) {
        const auto held = m_terrainCells.find(id);
        if (held == m_terrainCells.end())
            return;
        if (scene::TerrainComponent* component = terrain(); component != nullptr) {
            const core::u32 across = asset::terrainCellChunks(held->second.settings.voxelSize);
            if (asset::terrainCellUntouched(component->field, held->second, across)) {
                asset::removeTerrainCell(component->field, held->second);
                asset::removeTerrainCell(component->shipped, held->second);
                component->fieldRevision += 1;
            }
            else {
                ++m_kept;
                m_keptWhole.insert(id);
            }
        }
        m_terrainCells.erase(held);
        return;
    }

    const auto held = m_voxelCells.find(id);
    if (held == m_voxelCells.end())
        return;
    if (scene::VoxelComponent* component = voxels(); component != nullptr) {
        const core::u32 across = asset::voxelCellChunks(held->second.blockSize);
        if (asset::voxelCellUntouched(component->grid, held->second, across)) {
            asset::removeVoxelCell(component->grid, held->second);
            asset::removeVoxelCell(component->shipped, held->second);
            component->revision += 1;
        }
        else {
            ++m_kept;
        }
    }
    m_voxelCells.erase(held);
}

void FieldStreamer::followTerrainOrigin()
{
    const scene::TerrainComponent* component = terrain();
    if (component == nullptr)
        return;
    if (!m_boundsOrigin.has_value()) {
        m_boundsOrigin = component->origin;
        return;
    }
    const core::DVec3 delta{component->origin.x - m_boundsOrigin->x, component->origin.y - m_boundsOrigin->y,
                            component->origin.z - m_boundsOrigin->z};
    if (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0)
        return;
    asset::ChunkIndex moved = m_manager.index();
    for (asset::ChunkIndexEntry& entry : moved.chunks) {
        if (entry.id.layer != asset::FieldLayerTerrain)
            continue;
        entry.bounds.min =
            core::DVec3{entry.bounds.min.x + delta.x, entry.bounds.min.y + delta.y, entry.bounds.min.z + delta.z};
        entry.bounds.max =
            core::DVec3{entry.bounds.max.x + delta.x, entry.bounds.max.y + delta.y, entry.bounds.max.z + delta.z};
    }
    m_manager.replaceIndex(moved);
    m_boundsOrigin = component->origin;
}

void FieldStreamer::clearSession()
{
    std::error_code ignored;
    for (const asset::ChunkId id : m_session) {
        if (const auto path = m_paths.find(id); path != m_paths.end())
            std::filesystem::remove(path->second, ignored);
    }
    if (!m_sessionFolder.empty())
        std::filesystem::remove_all(m_sessionFolder, ignored);
    m_session.clear();
    m_sessionOver.clear();
    m_layer.reset();
    m_keptWhole.clear();
}

void FieldStreamer::beginSessionLayer()
{
    if (m_layer.has_value() || m_sessionFolder.empty())
        return;
    m_layers += 1;
    m_layer = SessionLayer{};
    m_layer->folder = m_sessionFolder / ("layer-" + std::to_string(m_layers));
}

void FieldStreamer::dropSessionLayer()
{
    m_looseValid = false;
    if (!m_layer.has_value())
        return;
    const SessionLayer layer = std::move(*m_layer);
    m_layer.reset();
    std::error_code ignored;
    std::filesystem::remove_all(layer.folder, ignored);
    if (layer.before.empty())
        return;
    // A read of one of its files still in flight is nobody's.
    for (std::size_t at = 0; at < m_reads.size();) {
        if (!layer.before.contains(m_reads[at].second)) {
            ++at;
            continue;
        }
        platform::cancelIo(m_reads[at].first);
        m_reads.erase(m_reads.begin() + static_cast<std::ptrdiff_t>(at));
    }
    // **Out of the index and back in**, which is how one cell's residency is
    // forgotten (`replaceIndex`): what the layer's file brought is not what
    // the cell is, and it is read again from what it was before.
    asset::ChunkIndex without;
    without.chunkSize = static_cast<core::f32>(asset::FieldCellMetres);
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (!layer.before.contains(entry.id))
            without.chunks.push_back(entry);
    }
    asset::ChunkIndex restored = without;
    m_manager.replaceIndex(std::move(without));
    for (const auto& [id, before] : layer.before) {
        m_terrainCells.erase(id);
        m_keptWhole.erase(id);
        m_cellsChanged.insert(id);
        if (before.row.has_value()) {
            restored.chunks.push_back(before.row->first);
            m_paths[id] = before.row->second;
        }
        else {
            m_paths.erase(id);
        }
        if (!before.session)
            m_session.erase(id);
        if (before.over.has_value())
            m_sessionOver[id] = *before.over;
        else
            m_sessionOver.erase(id);
    }
    m_active = !restored.chunks.empty();
    m_manager.replaceIndex(std::move(restored));
    m_primed = m_primed || !m_active;
}

void FieldStreamer::dropSession()
{
    m_layer.reset();
    m_keptWhole.clear();
    if (m_session.empty())
        return;
    asset::ChunkIndex merged;
    merged.chunkSize = static_cast<core::f32>(asset::FieldCellMetres);
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer != asset::FieldLayerTerrain || !m_session.contains(entry.id)) {
            merged.chunks.push_back(entry);
            continue;
        }
        const std::filesystem::path cached = m_paths[entry.id];
        if (const auto over = m_sessionOver.find(entry.id); over != m_sessionOver.end()) {
            merged.chunks.push_back(over->second.first);
            m_paths[entry.id] = over->second.second;
        }
        else {
            m_paths.erase(entry.id);
        }
        std::error_code ignored;
        std::filesystem::remove(cached, ignored);
    }
    m_session.clear();
    m_sessionOver.clear();
    if (!m_sessionFolder.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(m_sessionFolder, ignored);
    }
    m_manager.replaceIndex(merged);
    m_active = !merged.chunks.empty();
    m_cellSourceStale = true;
}

void FieldStreamer::spillFarGround(std::span<const asset::StreamingFocus> foci, f64 budgetMilliseconds)
{
    if (!m_spillAllowed || m_sessionFolder.empty() || foci.empty())
        return;
    scene::TerrainComponent* component = terrain();
    if (component == nullptr || component->field.chunks().empty())
        return;
    const asset::FieldSettings settings = component->field.settings();
    const core::u32 across = asset::terrainCellChunks(settings.voxelSize);

    // **The ground no loaded cell accounts for**: written where nothing was,
    // or changed and kept when its cell's eviction was refused. A cell this
    // streamer holds is the manager's to let go -- untouched it simply goes,
    // and touched it is refused, and is then one of these.
    //
    // **Found when the ground changes, not every frame**: every writer moves
    // the field's revision, and so does a cell coming in or going; a refused
    // eviction and a cell taken or let go by a save are counted beside it.
    // Between those the walk over every chunk -- on a small terrain every one
    // of them is loose -- would find what it found.
    if (!m_looseValid || m_looseRevision != component->fieldRevision || m_looseKept != m_kept ||
        m_looseHeld != m_terrainCells.size()) {
        m_loose.clear();
        for (const asset::TerrainField::Entry& entry : component->field.chunks()) {
            const asset::ChunkId id = asset::terrainCellOf(entry.first, across);
            if (!m_terrainCells.contains(id))
                m_loose.insert(id);
        }
        m_looseValid = true;
        m_looseRevision = component->fieldRevision;
        m_looseKept = m_kept;
        m_looseHeld = m_terrainCells.size();
    }
    const std::set<asset::ChunkId>& loose = m_loose;
    if (loose.empty())
        return;

    // **Only a terrain large enough to stream when saved** (ADR 0087): its
    // cells on disk and the ones only memory holds, together.
    core::usize known = 0;
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer == asset::FieldLayerTerrain)
            ++known;
    }
    core::usize unknown = 0;
    for (const asset::ChunkId id : loose)
        unknown += m_paths.contains(id) ? 0 : 1;
    if (known + unknown < InlineTerrainCells)
        return;

    // Past every focus's reach by two cells, so ground a camera is about to
    // want is not written out to be read back.
    const auto boundsOf = [&](asset::ChunkId id) {
        core::DAABB bounds;
        bounds.min = core::DVec3{component->origin.x + static_cast<f64>(id.x) * asset::FieldCellMetres,
                                 component->origin.y + static_cast<f64>(settings.minHeight),
                                 component->origin.z + static_cast<f64>(id.z) * asset::FieldCellMetres};
        bounds.max = core::DVec3{bounds.min.x + asset::FieldCellMetres,
                                 component->origin.y + static_cast<f64>(settings.maxHeight),
                                 bounds.min.z + asset::FieldCellMetres};
        return bounds;
    };
    // **And not from under anything that can fall**: a body that is not
    // anchored, or a character, far from every camera stood on generated
    // ground before any of it could be written out, and stands on it still.
    // The cells its collision reaches (`TerrainCollisionReach`) and one more.
    std::set<std::pair<core::i32, core::i32>> stoodOn;
    m_world->rigidBodies().forEach([&](core::InstanceId id, const scene::RigidBodyComponent& body) {
        if (body.anchored && m_world->characterBodies().find(id) == nullptr)
            return;
        const scene::PartComponent* part = m_world->parts().find(id);
        if (part == nullptr)
            return;
        const f64 reach = scene::PhysicsSync::TerrainCollisionReach + asset::FieldCellMetres;
        const auto cellAt = [](f64 metres) {
            return static_cast<core::i32>(std::floor(metres / asset::FieldCellMetres));
        };
        const f64 x = part->cframe.position.x - component->origin.x;
        const f64 z = part->cframe.position.z - component->origin.z;
        for (core::i32 cz = cellAt(z - reach); cz <= cellAt(z + reach); ++cz) {
            for (core::i32 cx = cellAt(x - reach); cx <= cellAt(x + reach); ++cx)
                stoodOn.emplace(cx, cz);
        }
    });

    std::vector<asset::ChunkId> far;
    for (const asset::ChunkId id : loose) {
        const asset::ChunkState state = m_manager.stateOf(id);
        if (state == asset::ChunkState::Loading || state == asset::ChunkState::Decoded)
            continue;
        if (stoodOn.contains({id.x, id.z}))
            continue;
        const core::DAABB bounds = boundsOf(id);
        bool near = false;
        for (const asset::StreamingFocus& focus : foci) {
            const f64 keep = focus.loadRadiusFor(asset::FieldLayerTerrain) + 2.0 * asset::FieldCellMetres;
            near = near || focus.distanceSquaredTo(bounds, asset::FieldLayerTerrain) <= keep * keep;
        }
        if (!near)
            far.push_back(id);
    }
    if (far.empty())
        return;

    // **Bounded, not best-effort**: inside the budget while a few wait, and
    // all of them once many do -- a generator writing a quarter of a square
    // kilometre a frame outruns a pump that writes a handful, and what waits
    // is memory: some six megabytes a cell of metre voxels.
    constexpr core::usize HighWater = 96;
    const bool all = far.size() > HighWater;
    const u64 started = platform::nowNs();
    std::error_code ignored;
    std::map<asset::ChunkId, asset::ChunkIndexEntry> rows;
    bool wrote = false;
    for (const asset::ChunkId id : far) {
        if (!all && wrote && budgetMilliseconds > 0.0 && millisecondsSince(started) >= budgetMilliseconds)
            break;
        asset::TerrainCell cell;
        cell.x = id.x;
        cell.z = id.z;
        cell.settings = settings;
        cell.field = asset::TerrainField(settings);
        for (core::i32 cz = id.z * static_cast<core::i32>(across); cz < (id.z + 1) * static_cast<core::i32>(across);
             ++cz) {
            for (core::i32 cx = id.x * static_cast<core::i32>(across); cx < (id.x + 1) * static_cast<core::i32>(across);
                 ++cx) {
                for (const asset::TerrainField::Entry& entry : component->field.column(cx, cz))
                    cell.field.setChunk(entry.first, entry.second);
            }
        }
        if (cell.field.chunks().empty())
            continue;
        // What the cell is now: its row and its file, the scene's or the
        // cache's, or neither for ground that is new.
        std::optional<SessionRow> was;
        if (const auto path = m_paths.find(id); path != m_paths.end()) {
            if (const asset::ChunkIndexEntry* row = m_manager.index().find(id); row != nullptr)
                was = SessionRow{*row, path->second};
        }
        // **A part of a cell is written with the rest of it**: ground put
        // into a square whose cell was not loaded -- nothing asked for it
        // first -- is what the field holds over what the file holds, the rule
        // a save follows. A file that will not read is not written over, and
        // the ground stays in memory.
        if (was.has_value() && !m_keptWhole.contains(id)) {
            std::vector<std::byte> onDiskBytes;
            asset::TerrainCell onDisk;
            if (!platform::readFile(was->second, onDiskBytes) ||
                asset::decodeTerrainCell(onDiskBytes, onDisk).has_value())
                continue;
            cell.field.shareFrom(onDisk.field, component->shipped);
        }
        // More than a cell may load back is not written: it stays in memory,
        // as a cell past that limit stays unsaved (terrain audit TA16f).
        if (cell.field.chunks().size() > asset::MaxCellChunks)
            continue;
        const std::vector<std::byte> bytes = asset::encodeTerrainCell(cell);
        const std::string name = "cell_" + std::to_string(id.x) + "_" + std::to_string(id.z) + ".lterrain";
        // Over its own file when the cache -- or this layer of it -- has the
        // cell already; beside it otherwise, so what it was is still there.
        const bool mine = m_session.contains(id) && (!m_layer.has_value() || m_layer->before.contains(id));
        const std::filesystem::path folder = m_layer.has_value() ? m_layer->folder : m_sessionFolder;
        const std::filesystem::path file = mine ? was->second : folder / name;
        if (!mine)
            std::filesystem::create_directories(folder, ignored);
        if (!platform::writeFile(file, bytes))
            continue;
        asset::ChunkIndexEntry entry;
        entry.id = id;
        entry.bounds = asset::terrainCellBounds(cell, across, component->origin);
        entry.urn = "session/" + name;
        entry.bytes = static_cast<core::u32>(bytes.size());
        rows[id] = entry;
        if (m_layer.has_value() && !m_layer->before.contains(id)) {
            SessionLayer::Before before;
            before.row = was;
            before.session = m_session.contains(id);
            if (const auto over = m_sessionOver.find(id); over != m_sessionOver.end())
                before.over = over->second;
            m_layer->before.emplace(id, std::move(before));
        }
        // What the scene's own file and row were, the first time.
        if (!m_session.contains(id) && was.has_value())
            m_sessionOver[id] = *was;
        m_paths[id] = file;
        m_session.insert(id);
        m_cellsChanged.insert(id);
        m_keptWhole.erase(id);
        m_spilled += 1;
        // Out of the field, and out of the package's copy of that square:
        // what streams back is the cache's cell, whole.
        asset::removeTerrainCell(component->field, cell);
        asset::TerrainCell shippedCell;
        shippedCell.field = asset::TerrainField(settings);
        for (core::i32 cz = id.z * static_cast<core::i32>(across); cz < (id.z + 1) * static_cast<core::i32>(across);
             ++cz) {
            for (core::i32 cx = id.x * static_cast<core::i32>(across); cx < (id.x + 1) * static_cast<core::i32>(across);
                 ++cx) {
                for (const asset::TerrainField::Entry& held : component->shipped.column(cx, cz))
                    shippedCell.field.setChunk(held.first, held.second);
            }
        }
        asset::removeTerrainCell(component->shipped, shippedCell);
        wrote = true;
    }
    m_spillNs += platform::nowNs() - started;
    if (rows.empty())
        return;
    component->fieldRevision += 1;

    // The index with the cache's rows in it: a row replaced where the cell
    // had one, added where the ground was new.
    asset::ChunkIndex merged;
    merged.chunkSize = static_cast<core::f32>(asset::FieldCellMetres);
    for (const asset::ChunkIndexEntry& entry : m_manager.index().chunks) {
        if (entry.id.layer == asset::FieldLayerTerrain && rows.contains(entry.id))
            continue;
        merged.chunks.push_back(entry);
    }
    for (const auto& row : rows)
        merged.chunks.push_back(row.second);
    if (!m_active) {
        m_manager.setIndex(merged);
        installCallbacks();
        m_active = true;
        // Nothing waits for ground that was just let go.
        m_primed = true;
        if (!m_boundsOrigin.has_value())
            m_boundsOrigin = component->origin;
    }
    else {
        m_manager.replaceIndex(merged);
    }
}

void FieldStreamer::pump(f64 budgetMilliseconds)
{
    if (m_world == nullptr)
        return;
    // **The terrain radii for both kinds of cell.** `TerrainLoadRadius` and
    // `TerrainMinRadius` have named "cells of terrain" since they were
    // reserved; the block world is ground too. A zero there follows the focus's
    // own pair, which is the rule every layer has.
    std::vector<asset::StreamingFocus> foci =
        m_focusOverride.has_value() ? std::vector<asset::StreamingFocus>{streamingFocusAt(*m_world, *m_focusOverride)}
                                    : collectStreamingFoci(*m_world, m_workspace);
    //
    // **And across the ground** (terrain audit T5): a cell of ground is as
    // near as the column it stands in, however high the camera is.
    for (asset::StreamingFocus& focus : foci) {
        focus.layers[asset::FieldLayerTerrain] = focus.layers[2];
        focus.layers[asset::FieldLayerVoxels] = focus.layers[2];
        focus.layers[asset::FieldLayerTerrain].planar = true;
        focus.layers[asset::FieldLayerVoxels].planar = true;
    }
    // Changed ground nobody is near goes to the session cache (ADR 0149),
    // which is what makes a streamer of a world no file describes yet.
    spillFarGround(foci, budgetMilliseconds > 0.0 ? budgetMilliseconds : 4.0);
    if (!m_active)
        return;
    followTerrainOrigin();
    shareCells();
    const u64 started = platform::nowNs();

    // Finished reads, inside the budget, on `StreamingHost::pump`'s terms
    // (D127 and D131: the drain is budgeted, and a failed read gives its slot
    // back).
    platform::pumpIo();
    for (std::size_t index = 0; index < m_reads.size();) {
        if (budgetMilliseconds > 0.0 && millisecondsSince(started) >= budgetMilliseconds)
            break;
        const platform::IoStatus status = platform::ioStatus(m_reads[index].first);
        if (status == platform::IoStatus::Pending) {
            ++index;
            continue;
        }
        std::vector<std::byte> bytes;
        if (status == platform::IoStatus::Ready && platform::takeIoResult(m_reads[index].first, bytes)) {
            m_manager.onChunkLoaded(m_reads[index].second, bytes);
        }
        else {
            platform::cancelIo(m_reads[index].first);
            m_manager.onChunkFailed(m_reads[index].second);
        }
        m_reads.erase(m_reads.begin() + static_cast<std::ptrdiff_t>(index));
    }

    m_manager.setFoci(foci);
    m_manager.setEnabled(m_world->engineState().streamingEnabled);

    asset::StreamingBudget budget;
    budget.milliseconds = std::max(0.0, budgetMilliseconds - millisecondsSince(started));
    // **Thirty-two reads open, not the parts' eight.** A cell of ground is a
    // few kilobytes and the minimum ring is a couple of hundred of them, all of
    // which the simulation is waiting for; eight at a time made the first load
    // a matter of seconds for nothing but queueing.
    budget.maxInFlight = 32;
    m_manager.tick(budget);
    for (const asset::ChunkId id : m_failedStarts)
        m_manager.onChunkFailed(id);
    m_failedStarts.clear();

    // What an edit read and the manager never did, let go where nobody is.
    for (auto at = m_readForEdit.begin(); at != m_readForEdit.end();) {
        const asset::ChunkId id = *at;
        const asset::ChunkState state = m_manager.stateOf(id);
        const bool held =
            id.layer == asset::FieldLayerTerrain ? m_terrainCells.contains(id) : m_voxelCells.contains(id);
        // The manager's now, or nobody's.
        if (state == asset::ChunkState::Resident || !held) {
            at = m_readForEdit.erase(at);
            continue;
        }
        const asset::ChunkIndexEntry* row = m_manager.index().find(id);
        bool near = state == asset::ChunkState::Loading || state == asset::ChunkState::Decoded;
        for (const asset::StreamingFocus& focus : foci) {
            if (near || row == nullptr)
                break;
            const f64 keep = focus.loadRadiusFor(id.layer) + 2.0 * asset::FieldCellMetres;
            near = focus.distanceSquaredTo(row->bounds, id.layer) <= keep * keep;
        }
        if (near) {
            ++at;
            continue;
        }
        evict(id);
        at = m_readForEdit.erase(at);
    }

    // **No focus, no wait** (D169). The first load holds the simulation so a
    // character standing on streamed ground does not fall through it -- and a
    // world with no camera and no focus has nobody standing anywhere, and never
    // will have a ring to be resident. Held, it waited for ever: a project whose
    // scene carried terrain and no camera ran with its scripts' Heartbeat never
    // firing. Scripts have run by the first pump (`WorldHost::boot`), so a game
    // that makes its own camera has one by now and still waits for its ground.
    if (!m_primed && foci.empty()) {
        m_primed = true;
        core::log(core::LogLevel::Warn, ENG_TR("app.warn.field_no_focus"), {});
    }
    if (!m_primed && !foci.empty() && m_manager.minimumRingResident()) {
        m_primed = true;
        // Said once, because how long the first load took is the number a
        // person tuning `TerrainMinRadius` needs.
        const core::I18nArg args[] = {{"cells", static_cast<core::i64>(m_manager.stats().chunksLoaded)},
                                      {"ms", static_cast<core::i64>(millisecondsSince(m_waitingSinceNs))}};
        core::log(core::LogLevel::Info, ENG_TR("app.info.field_primed"), args);
    }
}

} // namespace engine::app
