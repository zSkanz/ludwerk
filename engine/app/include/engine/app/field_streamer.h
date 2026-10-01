// Terrain and block worlds streamed from disk (ADR 0075; F1 Part E).
//
// **The same policy as parts, over a grid of its own.** A partition cuts the
// field into cells of about 64 metres (`asset/field_cells.h`) and this streams
// them with an `asset::StreamingManager` of its own -- its own index, because a
// `ChunkId` on this grid names a different square from the same id on the
// parts' 256 m one -- scored by the same squared distance, evicted by the same
// hysteresis, and held to the same frame budget.
//
// **What goes in, and what comes out.** A cell that arrives is shared into the
// live field: what the field already holds there wins, because it is newer.
// The cell is then KEPT, and that second reference is the whole of how an edit
// is noticed -- the field is copy-on-write, so the first write to anything the
// cell brought clones it, and at eviction a cell whose objects are no longer
// the ones it brought, or whose square holds something it did not bring, is
// left where it is. Nobody raises a flag, so nobody can forget to: a cell a
// script dug into, or a player built on, is never streamed away with the work.
//
// **The ground arrives before the simulation runs on it.** Until the minimum
// ring around every focus has been resident once, `primed` is false, and the
// host holds its ticks -- the initial load every streamed world has, so a
// character spawned on streamed ground does not fall through it on frame one.
// After that, `StreamingService.PauseOutsideLoadedArea` decides, as it does
// for parts.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/asset/chunk.h"
#include "engine/asset/field_cells.h"
#include "engine/asset/streaming.h"
#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/platform/async_io.h"

namespace engine::scene {
class World;
struct TerrainComponent;
struct VoxelComponent;
} // namespace engine::scene

namespace engine::app {

using core::f64;
using core::u64;

class FieldStreamer
{
public:
    using CellResolver = std::function<std::optional<std::filesystem::path>(const asset::ChunkIndexEntry&)>;

    // The partition's field index, each entry's file resolved once. An empty
    // index leaves the streamer inactive, which every world without a large
    // field is.
    void setIndex(const asset::ChunkIndex& index, const CellResolver& resolve);
    // Every cell let go and the index forgotten, as `StreamingHost::reset`
    // does it (audit A4).
    void reset();
    [[nodiscard]] bool active() const noexcept { return m_active; }

    // The world the cells go into, and the workspace whose `Terrain` and whose
    // camera they are about. A different world forgets what was resident: its
    // field went with it (`StreamingHost::setWorld` says the same).
    void setWorld(scene::World* world, core::InstanceId workspace);

    // One frame: finished reads, the foci, and the manager's tick.
    void pump(f64 budgetMilliseconds);

    // True once the minimum ring around the foci has been resident. Stays true.
    [[nodiscard]] bool primed() const noexcept { return m_primed; }
    [[nodiscard]] bool minimumRingResident() const noexcept { return m_manager.minimumRingResident(); }

    [[nodiscard]] const asset::StreamingStats& stats() const noexcept { return m_manager.stats(); }
    [[nodiscard]] asset::ChunkState stateOf(asset::ChunkId id) const noexcept { return m_manager.stateOf(id); }
    [[nodiscard]] const asset::ChunkIndex& index() const noexcept { return m_manager.index(); }

    // Cells an eviction left in place because somebody had changed them.
    [[nodiscard]] u64 kept() const noexcept { return m_kept; }

    // --- Changed ground, kept on disk for the session (ADR 0149) -------------

    // **Where a changed cell past the load radius is written before it is let
    // go**, to stream back from like any saved cell: this run's folder. Empty
    // -- the default -- and none is written: a changed cell stays in memory,
    // as it did before. A terrain smaller than what streams when saved (256
    // cells) is held whole either way.
    void setSessionFolder(std::filesystem::path folder) { m_sessionFolder = std::move(folder); }
    // **Only where nothing could want the ground back as it was.** Not in a
    // match (ADR 0149 §1.8): the ground's replication reads what changed from
    // memory. Not while editing by hand either (§1.6): the editor's undo is
    // the world as it was, and the cache is not in it -- a cell changed with
    // a brush stays in memory until it is saved, as it always did.
    void setSpillAllowed(bool allowed) noexcept { m_spillAllowed = allowed; }

    // **A layer over the cache, for what may be taken back whole** (§1.6):
    // Play in the editor, and an import that may be cancelled. Cells written
    // while one is open go to a folder of its own and leave what they
    // replaced alone -- the scene's file, or the cache's -- so `drop` puts
    // every one of them back as it was, and `keep` makes them the cache's
    // like any other. `drop` is called AFTER the world was put back: it
    // forgets what the layer's cells brought, and they are read again from
    // what they were before.
    void beginSessionLayer();
    void keepSessionLayer() noexcept { m_layer.reset(); }
    void dropSessionLayer();
    [[nodiscard]] bool sessionLayerOpen() const noexcept { return m_layer.has_value(); }
    // How many cells are in the session cache now, and how many have been
    // written to it in all.
    [[nodiscard]] core::usize sessionCells() const noexcept { return m_session.size(); }
    [[nodiscard]] u64 spilled() const noexcept { return m_spilled; }
    // What writing them has cost, in all.
    [[nodiscard]] f64 spillMilliseconds() const noexcept { return static_cast<f64>(m_spillNs) / 1.0e6; }
    // The cache's files removed and its cells forgotten -- a run ending, or a
    // world replaced. What it held and nothing saved is gone, as what memory
    // held is.
    void clearSession();
    // The cache dropped and every cell it had taken over given back to the
    // scene's own file; ground only the cache held is forgotten.
    void dropSession();

    // --- A terrain saved as cells, in the editor (ADR 0087) -------------------

    // Streams around this point rather than the world's own foci: the editor's
    // camera, which is not the scene's. Nothing is the world's foci again.
    void setFocusOverride(std::optional<core::DVec3> position) noexcept { m_focusOverride = position; }

    // **Takes a terrain's cell index as this streamer's terrain**, in place of
    // any it had, keeping every other kind of cell and what is resident of
    // them. An empty index stops streaming terrain. Called whenever the
    // workspace's terrain names a different index -- a scene opened, a terrain
    // converted to cells on its first large save, a terrain cleared.
    //
    // `keepSession`: the same ground under a new name -- a Save As -- whose
    // cells in the session cache stay the session's until the save that
    // follows commits them. Otherwise the cache was another terrain's, and
    // goes (ADR 0149).
    void adoptTerrain(const asset::ChunkIndex& index, const CellResolver& resolve, bool keepSession = false);

    // **After the world was put back** -- an undo, a redo, a stop: the field is
    // whatever the snapshot held, which lacks every cell loaded since it was
    // taken. Each cell this streamer holds is shared back into the field, where
    // the field has nothing newer, so the ground loaded around the camera is
    // there again rather than a hole the streamer believes is filled.
    void reconcile();

    // **Reads now every cell over a square that is not held** -- terrain and
    // block alike -- for an edit about to be made there (`World::loadGround`,
    // terrain audit U1). A read later for one of them finds it held and
    // changes nothing. **All or none** (TA16): more than `maxCells` to read,
    // and none is read and the answer is false.
    [[nodiscard]] bool loadNow(core::DVec3 low, core::DVec3 high, core::u32 maxCells);

    // Where a save writes one cell, and how it removes one.
    struct TerrainCellWriter
    {
        // Writes a cell's bytes; answers the URN it is at, or nothing on failure.
        std::function<std::optional<std::string>(asset::ChunkId, std::span<const std::byte>)> write;
        // Reads a cell as it is on disk, for merging ground that is not resident.
        std::function<std::optional<std::vector<std::byte>>(const asset::ChunkIndexEntry&)> read;
        // Removes the file of a cell nothing is left in -- the caller's, once
        // the new index is written (`TerrainSaveReport::emptied`).
        std::function<void(const asset::ChunkIndexEntry&)> remove;
        // How a written URN resolves for a later load.
        CellResolver resolve;
        // **Takes a cell's file as it is** (ADR 0149): the session cache's
        // file for a cell nobody is near, moved to where `write` would have
        // put its bytes. Answers the URN, or nothing when it could not -- and
        // the bytes are then read and written. Optional: a world's worth of
        // cells committed by reading and writing each is a world's worth of
        // copying, where a move on one disk is a rename.
        std::function<std::optional<std::string>(asset::ChunkId, const std::filesystem::path&)> adopt;
    };

    struct TerrainSaveReport
    {
        u64 written = 0;
        u64 removed = 0;
        u64 unchanged = 0;
        bool ok = true;
        // The cells emptied by this save, whose files go after the index.
        std::vector<asset::ChunkIndexEntry> emptied;
        // The terrain's whole index after the save, to be written beside the
        // cells: every cell the terrain has, resident or not.
        asset::ChunkIndex index;
    };

    // **Writes every cell whose ground differs from its file, and no other**
    // (ADR 0087): a cell loaded and not touched costs a comparison, a cell
    // nobody loaded costs nothing, and a cell somebody edited -- resident, or
    // kept in memory after its eviction was refused -- is written and becomes
    // an ordinary cell again, which the camera moving away may now let go.
    [[nodiscard]] TerrainSaveReport saveTerrain(const TerrainCellWriter& writer);

private:
    void installCallbacks();
    [[nodiscard]] scene::TerrainComponent* terrain() const;
    [[nodiscard]] scene::VoxelComponent* voxels() const;
    [[nodiscard]] f64 materialize(asset::ChunkId id, std::span<const std::byte> bytes);
    void evict(asset::ChunkId id);
    // **The terrain cells' bounds follow the terrain** (terrain audit TA16d):
    // they are in world space, drawn where the terrain stood, and a terrain
    // moved stopped streaming in round the player. Moved by the difference.
    void followTerrainOrigin();
    // **The terrain's cells, for drawing** (ADR 0144): made again when the
    // index changes, and handed to the terrain the streamer fills.
    void shareCells();
    // Writes out and lets go of the ground the field holds that no loaded
    // cell accounts for and no focus is near (ADR 0149).
    void spillFarGround(std::span<const asset::StreamingFocus> foci, f64 budgetMilliseconds);

    asset::StreamingManager m_manager;
    // **The terrain's cells, for drawing** (ADR 0144): one source while the
    // terrain's index stands, told of each cell that changes (ADR 0149) --
    // and made again only when the index is another (`m_cellSourceStale`).
    // The files it reads are kept beside it, under a lock of their own: it is
    // read from the renderer's threads.
    struct CellFiles
    {
        std::mutex lock;
        std::map<asset::ChunkId, std::filesystem::path> paths;
    };
    std::shared_ptr<asset::TerrainCellSource> m_cellSource;
    std::shared_ptr<CellFiles> m_cellFiles;
    bool m_cellSourceStale = true;
    // Cells whose file or whose being there changed since the source was told.
    std::set<asset::ChunkId> m_cellsChanged;
    std::map<asset::ChunkId, std::filesystem::path> m_paths;
    // What each resident cell brought, held for the reason the header gives.
    std::map<asset::ChunkId, asset::TerrainCell> m_terrainCells;
    std::map<asset::ChunkId, asset::VoxelCell> m_voxelCells;
    std::vector<std::pair<platform::IoRequest, asset::ChunkId>> m_reads;
    // Loads the manager started whose read could not, reported after its tick.
    std::vector<asset::ChunkId> m_failedStarts;
    scene::World* m_world = nullptr;
    core::InstanceId m_workspace;
    bool m_active = false;
    bool m_primed = false;
    u64 m_kept = 0;
    // When the current wait for the ground began, for the log line that ends it.
    u64 m_waitingSinceNs = 0;
    std::optional<core::DVec3> m_focusOverride;
    // Where the terrain stood when its cells' bounds were drawn.
    std::optional<core::DVec3> m_boundsOrigin;
    // The session cache: its folder, the cells whose file is in it, and how
    // many were ever written there.
    std::filesystem::path m_sessionFolder;
    std::set<asset::ChunkId> m_session;
    // The row and the file a cell had before the cache's took their place --
    // the scene's own -- for a cache dropped without a save: the world it was
    // changed in is gone, and what the scene says is the ground again.
    using SessionRow = std::pair<asset::ChunkIndexEntry, std::filesystem::path>;
    std::map<asset::ChunkId, SessionRow> m_sessionOver;
    struct SessionLayer
    {
        std::filesystem::path folder;
        // What each cell first written in this layer was before it: its row
        // and file (none for ground that was new), whether those were the
        // cache's already, and what the cache had taken over if so.
        struct Before
        {
            std::optional<SessionRow> row;
            bool session = false;
            std::optional<SessionRow> over;
        };
        std::map<asset::ChunkId, Before> before;
    };
    std::optional<SessionLayer> m_layer;
    core::u32 m_layers = 0;
    // **Cells read for an edit** (`loadNow`) and not by the manager, which
    // therefore never lets them go: each is let go here once no focus is
    // near, on an eviction's terms. A generator laying a world a tile at a
    // time reads the cells along every tile's edge, and kept them all.
    std::set<asset::ChunkId> m_readForEdit;
    // Cells whose eviction was refused: the field holds all of each, so
    // writing one out needs nothing of its file. Any other ground in a square
    // a file describes is a part, and the file is the rest.
    std::set<asset::ChunkId> m_keptWhole;
    bool m_spillAllowed = true;
    u64 m_spilled = 0;
    u64 m_spillNs = 0;
    // The cells of ground no loaded cell accounts for, found when the ground
    // changes rather than every frame: the field's revision, the evictions
    // refused and the cells held, as they were when it was last worked out.
    std::set<asset::ChunkId> m_loose;
    bool m_looseValid = false;
    u64 m_looseRevision = 0;
    u64 m_looseKept = 0;
    core::usize m_looseHeld = 0;
};

} // namespace engine::app
