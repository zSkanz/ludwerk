// The block world on the GPU (V1): what the loader meshes, and when.
#include <doctest/doctest.h>

#include "engine/asset/voxel.h"
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
