// Terrain and block worlds streamed from disk (ADR 0075).
//
// End to end over real files and the real asynchronous reader: cells arrive
// around the camera, leave behind it, and a cell somebody changed stays -- the
// one property the whole design is for, since a streamer that dropped an edit
// would lose a player's work without saying so.
#include <doctest/doctest.h>
#include <filesystem>
#include <map>
#include <system_error>
#include <thread>

#include "engine/app/field_streamer.h"
#include "engine/asset/field_cells.h"
#include "engine/asset/terrain_cell.h"
#include "engine/platform/async_io.h"
#include "engine/platform/file.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "inspector_fixture.h"

using namespace engine;

namespace {

struct IoScope
{
    IoScope() { REQUIRE(platform::initIo(4)); }
    ~IoScope() { platform::shutdownIo(); }
};

struct StreamedWorld
{
    app::testing::Fixture fixture;
    scene::World world{fixture.classes, fixture.enums, fixture.atoms, 1234u};
    core::InstanceId workspace;
    core::InstanceId camera;
    core::InstanceId ground;
    std::filesystem::path directory;
    asset::ChunkIndex index;
    app::FieldStreamer streamer;

    // A kilometre square of two-metre ground -- one 64 m chunk column a cell -- and a
    // strip of blocks along the x axis, both written as cells the way a
    // partition writes them. `metres` makes it wider.
    explicit StreamedWorld(float metres = 1024.0f)
    {
        workspace = world.create(fixture.workspaceClass);
        world.workspaces().add(workspace, scene::WorkspaceComponent{});

        camera = world.create(fixture.cameraClass);
        world.cameras().add(camera, scene::CameraComponent{});
        (void)world.setParent(camera, workspace);
        world.workspaces().find(workspace)->currentCamera = camera;

        const asset::FieldSettings settings{.voxelSize = 2.0f, .minHeight = -32.0f, .maxHeight = 32.0f};
        asset::TerrainField source(settings);
        (void)asset::fillBlock(source, core::DVec3{0.0, -20.0, 0.0}, core::Vec3{metres, 40.0f, metres}, 1);
        asset::VoxelGrid blocks;
        (void)blocks.fill(-300, 0, 0, 300, 2, 3, 1);

        ground = world.create(fixture.folderClass);
        scene::TerrainComponent terrain;
        terrain.field = asset::TerrainField(settings);
        world.terrains().add(ground, std::move(terrain));
        (void)world.setParent(ground, workspace);
        world.voxels().add(workspace, scene::VoxelComponent{});

        directory = std::filesystem::temp_directory_path() / "engine-field-streamer-tests";
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        REQUIRE(platform::createDirectories(directory));
        const core::u32 tiles = asset::terrainCellChunks(settings.voxelSize);
        for (const asset::TerrainCell& cell : asset::splitTerrain(source))
            add(asset::ChunkId{cell.x, cell.z, asset::FieldLayerTerrain}, asset::encodeTerrainCell(cell),
                asset::terrainCellBounds(cell, tiles, core::DVec3{}));
        const core::u32 chunks = asset::voxelCellChunks(1.0f);
        for (const asset::VoxelCell& cell : asset::splitVoxels(blocks, 1.0f))
            add(asset::ChunkId{cell.x, cell.z, asset::FieldLayerVoxels}, asset::encodeVoxelCell(cell),
                asset::voxelCellBounds(cell, chunks));
        std::sort(index.chunks.begin(), index.chunks.end(),
                  [](const asset::ChunkIndexEntry& a, const asset::ChunkIndexEntry& b) { return a.id < b.id; });

        scene::EngineState& state = world.engineState();
        state.streamingLoadRadius = 160.0;
        state.streamingMinRadius = 96.0;

        streamer.setIndex(index, [this](const asset::ChunkIndexEntry& entry) {
            return std::optional<std::filesystem::path>(directory / entry.urn);
        });
        streamer.setWorld(&world, workspace);
    }

    ~StreamedWorld()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    void add(asset::ChunkId id, const std::vector<std::byte>& bytes, core::DAABB bounds)
    {
        const std::string name = std::to_string(id.layer) + "_" + std::to_string(id.x) + "_" + std::to_string(id.z);
        REQUIRE(platform::writeFile(directory / name, bytes));
        asset::ChunkIndexEntry entry;
        entry.id = id;
        entry.bounds = bounds;
        entry.urn = name;
        entry.bytes = static_cast<core::u32>(bytes.size());
        index.chunks.push_back(entry);
    }

    void lookFrom(core::DVec3 position) { world.cameras().find(camera)->cframe.position = position; }

    [[nodiscard]] const asset::TerrainField& field() { return world.terrains().find(ground)->field; }
    // Whether the ground holds any chunk in the column of chunks at (x, z).
    [[nodiscard]] bool holds(core::i32 x, core::i32 z) { return !field().column(x, z).empty(); }
    [[nodiscard]] const asset::VoxelGrid& grid() { return world.voxels().find(workspace)->grid; }

    // Pumps until `done`, the way frames would, with the reader's threads
    // given time to finish.
    template <typename Done>
    [[nodiscard]] bool pumpUntil(Done&& done)
    {
        for (int frame = 0; frame < 20000; ++frame) {
            streamer.pump(50.0);
            if (done())
                return true;
            std::this_thread::yield();
        }
        return false;
    }
};

} // namespace

TEST_CASE("the ground streams in around the camera, and the simulation waits for it")
{
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.streamer.active());
    CHECK_FALSE(streamed.streamer.primed());

    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    // The ground under the camera is here, and ground four hundred metres off
    // is not.
    CHECK(streamed.holds(0, 0));
    CHECK(streamed.holds(-1, -1));
    CHECK_FALSE(streamed.holds(6, 6));
    // And so are the blocks under it, and not the ones at the strip's far end.
    CHECK(streamed.grid().get(2, 1, 1) == 1);
    CHECK(streamed.grid().get(290, 1, 1) == asset::AirBlock);
}

TEST_CASE("ground left behind is dropped, and ground somebody changed is kept")
{
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    // Build on the cell under the camera -- which clones the chunk, since the
    // streamer holds the one the cell brought -- and place a block.
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, 5, 4, asset::Voxel{asset::FullOccupancy, 3});
    (void)streamed.world.voxels().find(streamed.workspace)->grid.set(5, 3, 1, 2);

    // Walk four hundred metres away.
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(6, 6) && !streamed.holds(-1, -1); }));

    // The untouched cell went; the dug one, and the one with the block, stayed.
    CHECK_FALSE(streamed.holds(-1, -1));
    REQUIRE(streamed.holds(0, 0));
    CHECK(streamed.field().voxel(4, 5, 4).material == 3);
    CHECK(streamed.grid().get(5, 3, 1) == 2);
    CHECK(streamed.grid().get(-40, 1, 1) == asset::AirBlock);
    CHECK(streamed.streamer.kept() >= 2);

    // And walking back brings the rest of the ground back without undoing the
    // dig: what the field holds wins over what a cell brings.
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(-1, -1); }));
    CHECK(streamed.field().voxel(4, 5, 4).material == 3);
}

TEST_CASE("ground dug to nothing stays dug when its cell goes out and comes back (terrain audit G1)")
{
    // A digging game on a streamed world: a chunk dug to air is dropped, and
    // a cell that streamed out and back merged its file's copy in wherever the
    // field held nothing -- so the hole filled in behind the player.
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    const asset::ChunkKey dug{0, 0, 0};
    REQUIRE(streamed.field().findChunk(dug) != nullptr);
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    // Dug to air, which drops the chunk as an edit's `finishChunk` does.
    terrain.field.removeChunk(dug);
    terrain.fieldRevision += 1;
    // And a block chunk mined to air.
    scene::VoxelComponent& voxels = *streamed.world.voxels().find(streamed.workspace);
    REQUIRE(voxels.grid.findChunk(asset::VoxelChunkKey{0, 0, 0}) != nullptr);
    voxels.grid.removeChunk(asset::VoxelChunkKey{0, 0, 0});
    voxels.revision += 1;

    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] {
        return streamed.holds(6, 6) && !streamed.holds(-1, -1) &&
               streamed.streamer.stateOf(asset::ChunkId{0, 0, asset::FieldLayerTerrain}) != asset::ChunkState::Resident;
    }));
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    // Until both cells the edits were in have been read again.
    REQUIRE(streamed.pumpUntil([&] {
        return streamed.streamer.stateOf(asset::ChunkId{0, 0, asset::FieldLayerTerrain}) ==
                   asset::ChunkState::Resident &&
               streamed.streamer.stateOf(asset::ChunkId{0, 0, asset::FieldLayerVoxels}) == asset::ChunkState::Resident;
    }));

    CHECK(streamed.field().findChunk(dug) == nullptr);
    CHECK(streamed.grid().findChunk(asset::VoxelChunkKey{0, 0, 0}) == nullptr);
}

TEST_CASE("a world put back does not bring back a chunk dug before it was taken (terrain audit G2)")
{
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    scene::TerrainComponent* terrain = streamed.world.terrains().find(streamed.ground);
    terrain->field.removeChunk(asset::ChunkKey{0, 0, 0});
    terrain->fieldRevision += 1;
    const core::u64 revision = terrain->fieldRevision;

    // An undo of something else, or a Stop: the world put back as it was
    // after the dig, and the streamer's cells shared back into it.
    const scene::WorldSnapshot after = streamed.world.snapshot();
    (void)terrain->field.setVoxel(-4, -4, -4, asset::Voxel{});
    terrain->fieldRevision += 1;
    streamed.world.restore(after);
    streamed.streamer.reconcile();

    terrain = streamed.world.terrains().find(streamed.ground);
    CHECK(streamed.field().findChunk(asset::ChunkKey{0, 0, 0}) == nullptr);
    CHECK(streamed.holds(-1, -1));
    // And its revision is past any it had, so nothing trusts a build of
    // ground that the restore put somewhere else (terrain audit P6).
    CHECK(terrain->fieldRevision > revision + 1);
}

TEST_CASE("a save does not write back a chunk dug in a cell that was let go (terrain audit G3)")
{
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    terrain.field.removeChunk(asset::ChunkKey{0, 0, 0});
    terrain.fieldRevision += 1;
    // Away: the cell is kept for the dig, and let go of by the streamer.
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(6, 6) && !streamed.holds(-1, -1); }));

    std::map<asset::ChunkId, std::vector<std::byte>> written;
    app::FieldStreamer::TerrainCellWriter writer;
    writer.write = [&written](asset::ChunkId id, std::span<const std::byte> bytes) -> std::optional<std::string> {
        written[id] = std::vector<std::byte>(bytes.begin(), bytes.end());
        return "written_" + std::to_string(id.x) + "_" + std::to_string(id.z);
    };
    writer.read = [&streamed](const asset::ChunkIndexEntry& entry) -> std::optional<std::vector<std::byte>> {
        std::vector<std::byte> bytes;
        if (!platform::readFile(streamed.directory / entry.urn, bytes))
            return std::nullopt;
        return bytes;
    };
    writer.remove = [](const asset::ChunkIndexEntry&) {};
    const app::FieldStreamer::TerrainSaveReport report = streamed.streamer.saveTerrain(writer);
    CHECK(report.ok);

    const auto cell = written.find(asset::ChunkId{0, 0, asset::FieldLayerTerrain});
    REQUIRE(cell != written.end());
    asset::TerrainCell decoded;
    REQUIRE_FALSE(asset::decodeTerrainCell(cell->second, decoded).has_value());
    CHECK(decoded.field.findChunk(asset::ChunkKey{0, 0, 0}) == nullptr);
    CHECK(decoded.field.findChunk(asset::ChunkKey{0, -1, 0}) != nullptr);
}

TEST_CASE("an edit where the ground is not loaded yet reads it first (terrain audit U1)")
{
    // A script building four hundred metres from the camera: the ball made a
    // chunk of only itself, which then stood in for the cell's own chunk -- the
    // ground under the ball was gone once the cell came in.
    IoScope io;
    StreamedWorld streamed;
    streamed.world.setGroundLoader([&streamed](core::DVec3 low, core::DVec3 high, core::u32 cells) {
        return streamed.streamer.loadNow(low, high, cells);
    });
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    REQUIRE_FALSE(streamed.holds(6, 6));

    const core::DVec3 at{420.0, 0.0, 420.0};
    REQUIRE(streamed.world.loadGround(core::DVec3{at.x - 12.0, 0.0, at.z - 12.0},
                                      core::DVec3{at.x + 12.0, 0.0, at.z + 12.0}));
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    (void)asset::fillBall(terrain.field, at, 4.0, 2);
    terrain.fieldRevision += 1;

    // Then the camera goes there, and the cell's reads land.
    streamed.lookFrom(at);
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(7, 7); }));
    CHECK(asset::sampleField(streamed.field(), core::DVec3{at.x, -10.0, at.z}).distance < 0.0f);
}

TEST_CASE("a world with no camera and no focus does not hold the simulation for ground (D169)")
{
    // **The owner's project**: a scene carrying terrain and no camera, run
    // outside the editor. The first load waited for a ring around a focus that
    // did not exist, so it waited for ever, and the scripts' Heartbeat never
    // fired. Nobody stands anywhere in such a world, so there is nothing to
    // wait for.
    IoScope io;
    StreamedWorld streamed;
    streamed.world.workspaces().find(streamed.workspace)->currentCamera = {};
    REQUIRE(streamed.streamer.active());
    CHECK_FALSE(streamed.streamer.primed());

    streamed.streamer.pump(50.0);
    CHECK(streamed.streamer.primed());
    // And nothing was loaded for a focus that is not there.
    CHECK_FALSE(streamed.holds(0, 0));
}

// --- Ground on disk, whole or not at all (terrain audit TA16) ------------------

TEST_CASE("an edit's ground is loaded whole however many cells it reaches, or none of it is (TA16a)")
{
    // Twenty-one cells a side, 441 of them: an edit over all of it read the
    // first 256 and wrote only itself over the rest -- which a save kept.
    IoScope io;
    StreamedWorld streamed(1344.0f);
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    REQUIRE_FALSE(streamed.holds(10, 10));

    // More than a load may read: nothing is, and it says so.
    CHECK_FALSE(streamed.streamer.loadNow(core::DVec3{-700.0, 0.0, -700.0}, core::DVec3{700.0, 0.0, 700.0}, 100));
    CHECK_FALSE(streamed.holds(10, 10));
    CHECK_FALSE(streamed.holds(-10, -10));

    CHECK(streamed.streamer.loadNow(core::DVec3{-700.0, 0.0, -700.0}, core::DVec3{700.0, 0.0, 700.0}, 4096));
    for (int x = -10; x <= 10; x += 5) {
        for (int z = -10; z <= 10; z += 5) {
            CAPTURE(x);
            CAPTURE(z);
            CHECK(streamed.holds(x, z));
        }
    }
}

TEST_CASE("a cell saved at another voxel size is refused, never merged at the wrong scale (TA16b)")
{
    IoScope io;
    StreamedWorld streamed;
    // The terrain's voxels a metre, its cells' two.
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    terrain.field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 32.0f});
    for (int frame = 0; frame < 2000 && !streamed.streamer.primed(); ++frame) {
        streamed.streamer.pump(50.0);
        std::this_thread::yield();
    }
    CHECK(streamed.field().empty());
}

TEST_CASE("a streamed terrain moved goes on streaming round the camera where it is now (TA16d)")
{
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    REQUIRE_FALSE(streamed.holds(6, 6));
    // A kilometre east, and the camera over what was 420 m in, 420 m across.
    streamed.world.terrains().find(streamed.ground)->origin = core::DVec3{1000.0, 0.0, 0.0};
    streamed.lookFrom(core::DVec3{1420.0, 0.0, 420.0});
    CHECK(streamed.pumpUntil([&] { return streamed.holds(6, 6); }));
}

// --- Changed ground, kept on disk for the session (ADR 0149) ------------------
//
// A cell somebody changed was never let go, which is right for a crater and
// capped every generated or heavily edited world at the machine's memory: a
// script writing 16 km of ground at a metre voxel held twelve gigabytes of it.
// With a session folder the changed cell is written there when the camera
// leaves, and streams back from it like any saved cell.

TEST_CASE("ground somebody changed goes to the session cache when the camera leaves, and comes back changed")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, 5, 4, asset::Voxel{asset::FullOccupancy, 3});

    // Walk four hundred metres away: the dug cell goes too, to the cache.
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(6, 6) && !streamed.holds(0, 0); }));
    CHECK(streamed.streamer.sessionCells() == 1u);
    CHECK(streamed.streamer.spilled() == 1u);
    CHECK(std::filesystem::exists(streamed.directory / "session" / "cell_0_0.lterrain"));

    // And back: the ground is there again, with the dig in it.
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(0, 0); }));
    CHECK(streamed.field().voxel(4, 5, 4).material == 3);
    // Untouched since it streamed back, it simply goes the next time: nothing
    // more is written.
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0); }));
    CHECK(streamed.streamer.spilled() == 1u);
}

TEST_CASE("ground written where no cell was is the session's too")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    // A hill a kilometre and a half off, where the saved ground never reached:
    // chunk column 25 at this voxel size.
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)asset::fillBlock(field, core::DVec3{1632.0, 0.0, 1632.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 2);
    streamed.world.terrains().find(streamed.ground)->fieldRevision += 1;
    REQUIRE(streamed.holds(25, 25));

    // Nobody is near it: it is written out and let go.
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(25, 25); }));
    CHECK(streamed.streamer.sessionCells() >= 1u);

    // And walking there brings it back.
    streamed.lookFrom(core::DVec3{1632.0, 0.0, 1632.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(25, 25); }));
    CHECK(streamed.field().voxel(816, 1, 816).material == 2);
}

TEST_CASE("saving commits the session cache to the scene's own cells")
{
    IoScope io;
    StreamedWorld streamed;
    const std::filesystem::path session = streamed.directory / "session";
    streamed.streamer.setSessionFolder(session);
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, 5, 4, asset::Voxel{asset::FullOccupancy, 3});
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0); }));
    REQUIRE(streamed.streamer.sessionCells() == 1u);

    // The save, with the camera still away from the cell in the cache.
    std::map<std::pair<core::i32, core::i32>, std::vector<std::byte>> written;
    app::FieldStreamer::TerrainCellWriter writer;
    writer.write = [&](asset::ChunkId id, std::span<const std::byte> bytes) -> std::optional<std::string> {
        written[{id.x, id.z}] = std::vector<std::byte>(bytes.begin(), bytes.end());
        const std::string name = "saved_" + std::to_string(id.x) + "_" + std::to_string(id.z);
        REQUIRE(platform::writeFile(streamed.directory / name, bytes));
        return name;
    };
    writer.read = [&](const asset::ChunkIndexEntry& entry) -> std::optional<std::vector<std::byte>> {
        std::vector<std::byte> bytes;
        if (!platform::readFile(streamed.directory / entry.urn, bytes))
            return std::nullopt;
        return bytes;
    };
    writer.remove = [](const asset::ChunkIndexEntry&) {};
    writer.resolve = [&](const asset::ChunkIndexEntry& entry) {
        return std::optional<std::filesystem::path>(streamed.directory / entry.urn);
    };
    const app::FieldStreamer::TerrainSaveReport report = streamed.streamer.saveTerrain(writer);
    CHECK(report.ok);

    // The cell the cache held is written, with the dig; the cache is empty,
    // its file gone; and the index names the scene's file for it.
    REQUIRE(written.contains({0, 0}));
    asset::TerrainCell saved;
    REQUIRE_FALSE(asset::decodeTerrainCell(written[{0, 0}], saved).has_value());
    CHECK(saved.field.voxel(4, 5, 4).material == 3);
    CHECK(streamed.streamer.sessionCells() == 0u);
    CHECK_FALSE(std::filesystem::exists(session / "cell_0_0.lterrain"));
    bool named = false;
    for (const asset::ChunkIndexEntry& entry : report.index.chunks) {
        if (entry.id.x == 0 && entry.id.z == 0 && entry.id.layer == asset::FieldLayerTerrain)
            named = entry.urn == "saved_0_0";
        CHECK(entry.urn.find("session/") == std::string::npos);
    }
    CHECK(named);

    // And it streams back from the scene's file.
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(0, 0); }));
    CHECK(streamed.field().voxel(4, 5, 4).material == 3);
}

TEST_CASE("a world no file describes streams once its far ground is written out, and a small one is held whole")
{
    IoScope io;
    for (const float metres : {1152.0f, 512.0f}) {
        CAPTURE(metres);
        StreamedWorld streamed;
        // No cells on disk at all: the ground is made in memory, as a script
        // or the editor's Generate makes it.
        streamed.streamer.reset();
        streamed.streamer.setWorld(&streamed.world, streamed.workspace);
        streamed.streamer.setSessionFolder(streamed.directory / "session");
        asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
        (void)asset::fillBlock(field, core::DVec3{0.0, -20.0, 0.0}, core::Vec3{metres, 40.0f, metres}, 1);
        streamed.world.terrains().find(streamed.ground)->fieldRevision += 1;
        const std::size_t whole = field.chunks().size();
        REQUIRE_FALSE(streamed.streamer.active());

        for (int frame = 0; frame < 200; ++frame)
            streamed.streamer.pump(50.0);

        if (metres > 1024.0f) {
            // 324 cells: the far ones went to the cache, and what is left is
            // what the camera's radius holds.
            CHECK(streamed.streamer.active());
            CHECK(streamed.streamer.sessionCells() > 200u);
            CHECK(field.chunks().size() < whole / 2);
            CHECK(streamed.holds(0, 0));
            CHECK_FALSE(streamed.holds(8, 8));
            // The far corner comes back when somebody goes there.
            streamed.lookFrom(core::DVec3{520.0, 0.0, 520.0});
            REQUIRE(streamed.pumpUntil([&] { return streamed.holds(8, 8); }));
        }
        else {
            // 64 cells: smaller than what streams when saved. Held whole.
            CHECK_FALSE(streamed.streamer.active());
            CHECK(streamed.streamer.sessionCells() == 0u);
            CHECK(field.chunks().size() == whole);
        }
    }
}

TEST_CASE("in a match changed ground stays in memory, cache or no cache")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    streamed.streamer.setSpillAllowed(false);
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, 5, 4, asset::Voxel{asset::FullOccupancy, 3});
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(6, 6) && !streamed.holds(-1, -1); }));
    CHECK(streamed.holds(0, 0));
    CHECK(streamed.streamer.kept() >= 1);
    CHECK(streamed.streamer.sessionCells() == 0u);
}

TEST_CASE("a part of a cell written where it was not loaded goes to the cache with the rest of it")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    REQUIRE_FALSE(streamed.holds(6, 6));

    // Straight into the field, four hundred metres off, with nothing asking
    // for the cell first: a voxel above ground that is on disk and not here,
    // in a chunk of its own -- a chunk is the unit, and the field's wins.
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(200, 3, 200, asset::Voxel{asset::FullOccupancy, 3});
    streamed.world.terrains().find(streamed.ground)->fieldRevision += 1;
    REQUIRE(streamed.holds(6, 6));
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(6, 6); }));
    CHECK(streamed.streamer.sessionCells() == 1u);

    // What comes back is the whole cell: the voxel, and the ground under it
    // that only the file held.
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(6, 6); }));
    CHECK(streamed.field().voxel(200, 3, 200).material == 3);
    CHECK(streamed.field().voxel(210, -2, 210).material == 1);
    CHECK(streamed.field().voxel(210, -9, 210).material == 1);
}

TEST_CASE("a layer of the cache dropped puts every cell back as it was before it")
{
    // Play in the editor, and an import cancelled: what was written out while
    // the layer was open is taken back whole.
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    const core::u8 before = streamed.field().voxel(4, -2, 4).material;

    streamed.streamer.beginSessionLayer();
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, -2, 4, asset::Voxel{asset::FullOccupancy, 3});
    // And ground that is new, a kilometre and a half off.
    (void)asset::fillBlock(field, core::DVec3{1632.0, 0.0, 1632.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 2);
    streamed.world.terrains().find(streamed.ground)->fieldRevision += 1;
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0) && !streamed.holds(25, 25); }));
    CHECK(streamed.streamer.sessionCells() == 2u);

    streamed.streamer.dropSessionLayer();
    CHECK(streamed.streamer.sessionCells() == 0u);
    CHECK_FALSE(std::filesystem::exists(streamed.directory / "session" / "layer-1"));

    // The cell is the scene's again, and the new ground is nowhere.
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(0, 0); }));
    CHECK(streamed.field().voxel(4, -2, 4).material == before);
    streamed.lookFrom(core::DVec3{1632.0, 0.0, 1632.0});
    for (int frame = 0; frame < 400; ++frame)
        streamed.streamer.pump(50.0);
    CHECK_FALSE(streamed.holds(25, 25));
}

TEST_CASE("a layer kept is the cache's, and a second layer over it leaves it alone")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    // An import: written out under a layer, and kept.
    streamed.streamer.beginSessionLayer();
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(4, -2, 4, asset::Voxel{asset::FullOccupancy, 3});
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0); }));
    streamed.streamer.keepSessionLayer();
    REQUIRE(streamed.streamer.sessionCells() == 1u);

    // Then Play: the cell comes back, is changed again, goes out -- and Stop.
    streamed.streamer.beginSessionLayer();
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(0, 0); }));
    (void)streamed.world.terrains()
        .find(streamed.ground)
        ->field.setVoxel(4, -2, 4, asset::Voxel{asset::FullOccupancy, 4});
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0); }));
    CHECK(streamed.streamer.spilled() == 2u);
    streamed.streamer.dropSessionLayer();

    // What the import wrote is what the cell is.
    CHECK(streamed.streamer.sessionCells() == 1u);
    streamed.lookFrom(core::DVec3{0.0, 0.0, 0.0});
    REQUIRE(streamed.pumpUntil([&] { return streamed.holds(0, 0); }));
    CHECK(streamed.field().voxel(4, -2, 4).material == 3);
}

TEST_CASE("ground under a body that can fall is not written out, however far the camera is")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));

    // Two hills a kilometre and a half off, and a crate standing on one.
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)asset::fillBlock(field, core::DVec3{1632.0, 0.0, 1632.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 2);
    (void)asset::fillBlock(field, core::DVec3{1632.0, 0.0, -1632.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 2);
    streamed.world.terrains().find(streamed.ground)->fieldRevision += 1;
    const core::InstanceId crate = streamed.world.create(streamed.fixture.partClass);
    scene::PartComponent part;
    part.cframe.position = core::DVec3{1632.0, 12.0, 1632.0};
    streamed.world.parts().add(crate, part);
    streamed.world.rigidBodies().add(crate, scene::RigidBodyComponent{});
    (void)streamed.world.setParent(crate, streamed.workspace);

    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(25, -26); }));
    for (int frame = 0; frame < 50; ++frame)
        streamed.streamer.pump(50.0);
    CHECK(streamed.holds(25, 25));

    // Anchored, nothing falls: the hill goes like the other.
    streamed.world.rigidBodies().find(crate)->anchored = true;
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(25, 25); }));
}

TEST_CASE("ground read for an edit far from the camera is let go once nobody is near it")
{
    // `loadNow` reads cells the manager never asked for, so the manager never
    // let them go: a generator laying a world tile by tile read the cells
    // along every tile's edge and kept them all -- two chunks a cell of the
    // whole world, which is the memory the session cache is there to bound.
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    REQUIRE_FALSE(streamed.holds(6, 6));
    REQUIRE(streamed.streamer.loadNow(core::DVec3{390.0, 0.0, 390.0}, core::DVec3{440.0, 0.0, 440.0}, 64));
    REQUIRE(streamed.holds(6, 6));

    // Untouched, it goes at the next pump; touched, it is kept like any cell
    // somebody changed.
    streamed.streamer.pump(50.0);
    CHECK_FALSE(streamed.holds(6, 6));

    REQUIRE(streamed.streamer.loadNow(core::DVec3{390.0, 0.0, 390.0}, core::DVec3{440.0, 0.0, 440.0}, 64));
    asset::TerrainField& field = streamed.world.terrains().find(streamed.ground)->field;
    (void)field.setVoxel(200, -2, 200, asset::Voxel{asset::FullOccupancy, 3});
    const core::u64 kept = streamed.streamer.kept();
    streamed.streamer.pump(50.0);
    CHECK(streamed.holds(6, 6));
    CHECK(streamed.streamer.kept() == kept + 1);
    CHECK(streamed.field().voxel(200, -2, 200).material == 3);
}

namespace {

// The digest the far ground holds for one chunk of a cell, or nothing.
[[nodiscard]] std::optional<core::u64> summarisedDigest(const asset::TerrainCellSource& source, asset::ChunkId cell,
                                                        asset::ChunkKey chunk)
{
    const std::shared_ptr<const asset::TerrainCellSource::Summaries> summaries = source.summaries(cell);
    if (summaries == nullptr)
        return std::nullopt;
    for (const asset::TerrainField::Entry& entry : *summaries) {
        if (entry.first == chunk)
            return entry.second->digest();
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("the far ground reads a saved cell again, and keeps what it read of every other (D406)")
{
    // What the far ground draws a cell that is not resident from is a summary
    // read once and kept. A save wrote the cell and the summary stayed the one
    // of the file before: dig, save, walk away, and the horizon showed the
    // ground undug.
    IoScope io;
    StreamedWorld streamed;
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    const std::shared_ptr<const asset::TerrainCellSource> source = terrain.cellSource;
    REQUIRE(source != nullptr);
    const asset::ChunkId under{0, 0, asset::FieldLayerTerrain};
    const asset::ChunkId away{6, 6, asset::FieldLayerTerrain};
    const std::optional<core::u64> before = summarisedDigest(*source, under, asset::ChunkKey{0, -1, 0});
    REQUIRE(before.has_value());
    const std::shared_ptr<const asset::TerrainCellSource::Summaries> kept = source->summaries(away);
    REQUIRE(kept != nullptr);

    (void)terrain.field.setVoxel(4, -2, 4, asset::Voxel{asset::FullOccupancy, 3});
    terrain.fieldRevision += 1;
    app::FieldStreamer::TerrainCellWriter writer;
    writer.write = [&](asset::ChunkId id, std::span<const std::byte> bytes) -> std::optional<std::string> {
        const std::string name = "saved_" + std::to_string(id.x) + "_" + std::to_string(id.z);
        REQUIRE(platform::writeFile(streamed.directory / name, bytes));
        return name;
    };
    writer.read = [&](const asset::ChunkIndexEntry& entry) -> std::optional<std::vector<std::byte>> {
        std::vector<std::byte> bytes;
        if (!platform::readFile(streamed.directory / entry.urn, bytes))
            return std::nullopt;
        return bytes;
    };
    writer.remove = [](const asset::ChunkIndexEntry&) {};
    writer.resolve = [&](const asset::ChunkIndexEntry& entry) {
        return std::optional<std::filesystem::path>(streamed.directory / entry.urn);
    };
    const app::FieldStreamer::TerrainSaveReport report = streamed.streamer.saveTerrain(writer);
    REQUIRE(report.ok);
    REQUIRE(report.written == 1u);
    streamed.streamer.pump(50.0);

    // The same source, told: the saved cell is read again, the other is the
    // very summary it was.
    CHECK(terrain.cellSource.get() == source.get());
    const std::optional<core::u64> after = summarisedDigest(*source, under, asset::ChunkKey{0, -1, 0});
    REQUIRE(after.has_value());
    CHECK(*after != *before);
    CHECK(source->summaries(away).get() == kept.get());
}

TEST_CASE("the far ground is told of a cell that goes to the session cache, and of ground that is new")
{
    IoScope io;
    StreamedWorld streamed;
    streamed.streamer.setSessionFolder(streamed.directory / "session");
    REQUIRE(streamed.pumpUntil([&] { return streamed.streamer.primed(); }));
    scene::TerrainComponent& terrain = *streamed.world.terrains().find(streamed.ground);
    const std::shared_ptr<const asset::TerrainCellSource> source = terrain.cellSource;
    REQUIRE(source != nullptr);
    const asset::ChunkId under{0, 0, asset::FieldLayerTerrain};
    const asset::ChunkId beside{1, 0, asset::FieldLayerTerrain};
    const std::optional<core::u64> before = summarisedDigest(*source, under, asset::ChunkKey{0, -1, 0});
    REQUIRE(before.has_value());
    const std::shared_ptr<const asset::TerrainCellSource::Summaries> kept = source->summaries(beside);
    REQUIRE(kept != nullptr);
    const std::size_t cells = source->cells().size();

    (void)terrain.field.setVoxel(4, -2, 4, asset::Voxel{asset::FullOccupancy, 3});
    (void)asset::fillBlock(terrain.field, core::DVec3{1632.0, 0.0, 1632.0}, core::Vec3{40.0f, 16.0f, 40.0f}, 2);
    terrain.fieldRevision += 1;
    streamed.lookFrom(core::DVec3{420.0, 0.0, 420.0});
    REQUIRE(streamed.pumpUntil([&] { return !streamed.holds(0, 0) && !streamed.holds(25, 25); }));
    streamed.streamer.pump(50.0);

    CHECK(terrain.cellSource.get() == source.get());
    // The dug cell is read from the cache; the hill is a cell it did not have.
    const std::optional<core::u64> after = summarisedDigest(*source, under, asset::ChunkKey{0, -1, 0});
    REQUIRE(after.has_value());
    CHECK(*after != *before);
    CHECK(source->cells().size() == cells + 1);
    CHECK(source->summaries(asset::ChunkId{25, 25, asset::FieldLayerTerrain}) != nullptr);
    CHECK(source->summaries(beside).get() == kept.get());
}
