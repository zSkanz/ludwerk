// The block world on the GPU (V1): what the loader meshes, and when.
#include <chrono>
#include <doctest/doctest.h>
#include <thread>
#include <vector>

#include "engine/asset/voxel.h"
#include "engine/jobs/jobs.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/render/voxel_loader.h"
#include "engine/rhi/backends.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::render;

namespace {

struct VoxelFixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId serviceClass = scene::InvalidClass;
    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    rhi::ICmdList* cmd = nullptr;
    scene::World world;
    core::InstanceId service;
    MeshCache cache;
    MeshLibrary library;
    VoxelLoader loader;

    VoxelFixture()
        : serviceClass(classes.registerClass(
              {.name = atoms.intern("VoxelService"), .defaultName = atoms.intern("VoxelService")})),
          world(classes, enums, atoms, 1234u)
    {
        REQUIRE(device != nullptr);
        cmd = device->beginFrame();
        REQUIRE(cmd != nullptr);
        service = world.create(serviceClass);
        world.voxels().add(service, scene::VoxelComponent{});
    }

    ~VoxelFixture()
    {
        loader.destroy(*device, cache, library);
        cache.destroy(*device);
    }

    VoxelFixture(const VoxelFixture&) = delete;
    VoxelFixture& operator=(const VoxelFixture&) = delete;

    scene::VoxelComponent& voxels() { return *world.voxels().find(service); }
    core::u32 sync() { return loader.sync(*device, *cmd, world, atoms, cache, library); }
};

} // namespace

TEST_CASE("every chunk with blocks is meshed once, and a quiet frame meshes nothing")
{
    VoxelFixture fixture;
    (void)fixture.voxels().grid.fill(0, 0, 0, 47, 3, 15, 1);
    REQUIRE(fixture.voxels().grid.chunkCount() == 3);
    CHECK(fixture.sync() == 3);
    CHECK(fixture.loader.residentCount() == 3);
    CHECK(fixture.library.size() == 3);
    CHECK(fixture.sync() == 0);
}

TEST_CASE("breaking a block re-meshes its chunk and the neighbour whose face it touches")
{
    // The block at x = 15 sits against the chunk boundary: breaking it opens a
    // face on the neighbour's block at x = 16, so both chunks re-mesh, and the
    // third, two chunks away, does not.
    VoxelFixture fixture;
    (void)fixture.voxels().grid.fill(0, 0, 0, 47, 3, 15, 1);
    (void)fixture.sync();

    (void)fixture.voxels().grid.set(15, 1, 8, asset::AirBlock);
    CHECK(fixture.sync() == 2);
}

TEST_CASE("breaking a block inside a chunk re-meshes that chunk alone (audit P1)")
{
    // Nothing a neighbour's mesh reads changed -- only its one-block shell is
    // read -- so the neighbours keep their meshes. Every one of them used to
    // re-mesh, since their key digested this chunk whole.
    VoxelFixture fixture;
    (void)fixture.voxels().grid.fill(0, 0, 0, 47, 3, 15, 1);
    (void)fixture.sync();

    (void)fixture.voxels().grid.set(24, 1, 8, asset::AirBlock);
    CHECK(fixture.sync() == 1);
    CHECK(fixture.sync() == 0);
}

TEST_CASE("a chunk emptied of blocks gives its mesh back")
{
    VoxelFixture fixture;
    (void)fixture.voxels().grid.set(4, 4, 4, 1);
    (void)fixture.sync();
    REQUIRE(fixture.library.size() == 1);
    (void)fixture.voxels().grid.set(4, 4, 4, asset::AirBlock);
    (void)fixture.sync();
    CHECK(fixture.loader.residentCount() == 0);
    CHECK(fixture.library.size() == 0);
}

TEST_CASE("only chunks within the view distance are meshed")
{
    VoxelFixture fixture;
    (void)fixture.voxels().grid.set(0, 0, 0, 1);
    (void)fixture.voxels().grid.set(1600, 0, 0, 1);
    fixture.loader.setFocus(core::DVec3{0.0, 0.0, 0.0});
    fixture.loader.setViewDistance(200.0);
    (void)fixture.sync();
    CHECK(fixture.loader.residentCount() == 1);
}

namespace {

// Workers for the length of a case, and the pool as it was after it.
struct Workers
{
    bool wasUp = jobs::initialized();
    Workers()
    {
        if (!wasUp)
            jobs::init(4);
    }
    ~Workers()
    {
        if (!wasUp)
            jobs::shutdown();
    }
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;
};

// Frames until the loader has nothing left to do, and what each swapped in.
[[nodiscard]] std::vector<core::u32> frames(VoxelFixture& fixture)
{
    std::vector<core::u32> swapped;
    // The first frame is the one that finds what changed.
    for (int frame = 0; frame < 4000; ++frame) {
        swapped.push_back(fixture.sync());
        if (fixture.loader.pendingCount() == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return swapped;
}

} // namespace

TEST_CASE("D449: a chunk that changes is drawn from its old mesh until every chunk asked with it is ready")
{
    // "A lot of lag while the game is building the chunks": thirty-two chunks
    // were meshed and given two buffers each inside the frame that found
    // them changed -- twenty-six milliseconds of it.
    const Workers workers;
    VoxelFixture fixture;
    // Twelve chunks in a row, and the first sight of them settled.
    (void)fixture.voxels().grid.fill(0, 0, 0, 191, 3, 15, 1);
    REQUIRE(fixture.voxels().grid.chunkCount() == 12);
    fixture.loader.settleNext();
    CHECK(fixture.sync() == 12);
    CHECK(fixture.loader.pendingCount() == 0);
    CHECK(fixture.loader.residentCount() == 12);
    CHECK(fixture.library.size() == 12);
    // All of it in two shared buffers, not twenty-four of its own.
    CHECK(fixture.cache.poolPageCount() == 2u);
    // A quiet frame asks for nothing.
    CHECK(fixture.sync() == 0);
    CHECK(fixture.loader.pendingCount() == 0);

    // Every chunk changes at once -- a generator at work.
    (void)fixture.voxels().grid.fill(0, 4, 0, 191, 5, 15, 2);
    const std::vector<core::u32> swapped = frames(fixture);
    REQUIRE_FALSE(swapped.empty());
    // **The old meshes were there the whole time**, and the new ones came in
    // one frame, all twelve: never a frame with some of each.
    core::u32 total = 0;
    int framesThatSwapped = 0;
    for (const core::u32 count : swapped) {
        total += count;
        framesThatSwapped += count > 0 ? 1 : 0;
    }
    CHECK(total == 12);
    CHECK(framesThatSwapped == 1);
    CHECK(fixture.loader.residentCount() == 12);
    CHECK(fixture.library.size() == 12);
    // Remade into the slices the old ones gave back.
    CHECK(fixture.cache.poolPageCount() == 2u);
}

TEST_CASE("D449: a block broken is in the picture the frame it is broken, workers or not")
{
    const Workers workers;
    VoxelFixture fixture;
    (void)fixture.voxels().grid.fill(0, 0, 0, 47, 3, 15, 1);
    fixture.loader.settleNext();
    (void)fixture.sync();

    // At a chunk's edge: the chunk and the neighbour whose face it opens,
    // both in this frame.
    (void)fixture.voxels().grid.set(15, 1, 8, asset::AirBlock);
    CHECK(fixture.sync() == 2);
    CHECK(fixture.loader.pendingCount() == 0);
    // Inside a chunk: that chunk alone -- the workers found the others'
    // meshes would read what they read before, and made nothing.
    (void)fixture.voxels().grid.set(24, 1, 8, asset::AirBlock);
    CHECK(fixture.sync() == 1);
    CHECK(fixture.sync() == 0);
}

TEST_CASE("D449: a picture's frame waits for every chunk, however many")
{
    const Workers workers;
    VoxelFixture fixture;
    // A hundred chunks: more than two batches hold.
    (void)fixture.voxels().grid.fill(0, 0, 0, 159, 3, 159, 1);
    REQUIRE(fixture.voxels().grid.chunkCount() == 100);
    // An interactive frame asks for the nearest and goes on.
    (void)fixture.sync();
    CHECK(fixture.loader.pendingCount() > 0);
    // The frame a screenshot is taken from does not.
    fixture.loader.settleNext();
    (void)fixture.sync();
    CHECK(fixture.loader.pendingCount() == 0);
    CHECK(fixture.loader.residentCount() == 100);

    // A world that went away while its chunks were at the workers: what
    // comes back is for nothing, and nothing is left behind.
    (void)fixture.voxels().grid.fill(0, 4, 0, 159, 4, 159, 2);
    (void)fixture.sync();
    fixture.voxels().grid.clear();
    (void)frames(fixture);
    (void)fixture.sync();
    CHECK(fixture.loader.residentCount() == 0);
    CHECK(fixture.library.size() == 0);
}
