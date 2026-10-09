#include "../src/terrain_variants.h"
// Terrain on the GPU (ADR 0082): which nodes are drawn, and what is meshed.
//
// **Testable with no device worth the name.** The loader runs against the null
// device and is judged by what it built and what it chose to draw: every piece
// of ground drawn once, finest under the viewer, nothing drawn where a node is
// not ready yet, and an edit rebuilding the node it touched and not the rest.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/asset/terrain_palette.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/render_world.h"
#include "engine/render/terrain_loader.h"
#include "engine/rhi/backends.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"

using namespace engine;
using namespace engine::render;

namespace {

struct LoaderFixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId workspaceClass = scene::InvalidClass;
    scene::ClassId terrainClass = scene::InvalidClass;
    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    rhi::ICmdList* cmd = nullptr;
    scene::World world;
    core::InstanceId root;
    core::InstanceId terrain;
    MeshCache cache;
    MeshLibrary library;
    TerrainLoader loader;

    // Flat ground `size` metres across, at a metre voxel.
    explicit LoaderFixture(float size = 256.0f)
        : workspaceClass(
              classes.registerClass({.name = atoms.intern("Workspace"), .defaultName = atoms.intern("Workspace")})),
          terrainClass(
              classes.registerClass({.name = atoms.intern("Terrain"), .defaultName = atoms.intern("Terrain")})),
          world(classes, enums, atoms, 1234u)
    {
        REQUIRE(device != nullptr);
        cmd = device->beginFrame();
        REQUIRE(cmd != nullptr);
        root = world.create(workspaceClass);
        terrain = world.create(terrainClass);
        scene::TerrainComponent component;
        component.field =
            asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 64.0f});
        (void)asset::fillFlat(component.field, core::DVec3{0.0, 0.0, 0.0}, size, 0.0f, 1);
        world.terrains().add(terrain, std::move(component));
        REQUIRE(world.setParent(terrain, root) == std::nullopt);
        loader.setBuildsPerSync(1000);
    }

    ~LoaderFixture()
    {
        loader.destroy(*device, cache, library);
        cache.destroy(*device);
    }

    LoaderFixture(const LoaderFixture&) = delete;
    LoaderFixture& operator=(const LoaderFixture&) = delete;

    scene::TerrainComponent& component() { return *world.terrains().find(terrain); }
    core::u32 sync() { return loader.sync(*device, *cmd, world, atoms, cache, library); }

    // Syncs until nothing is left to build, and answers the draws.
    std::vector<TerrainNodeDraw> settle()
    {
        for (int frame = 0; frame < 64; ++frame) {
            (void)sync();
            if (!loader.pending() && loader.lastBuilds() == 0)
                break;
        }
        return loader.draws(world);
    }
};

// The node a URN names: `terrain://<slot>.<generation>/<level>/<x>,<z>`.
[[nodiscard]] TerrainNodeKey keyOf(const core::AtomTable& atoms, core::NameAtom urn)
{
    const std::string text(atoms.text(urn));
    TerrainNodeKey key;
    const auto first = text.find('/', 10);
    const auto second = text.find('/', first + 1);
    const auto comma = text.find(',', second + 1);
    key.level = static_cast<core::u32>(std::stoul(text.substr(first + 1, second - first - 1)));
    key.x = std::stoi(text.substr(second + 1, comma - second - 1));
    key.z = std::stoi(text.substr(comma + 1));
    return key;
}

// Every chunk column the draws cover, and how many times.
[[nodiscard]] std::map<std::pair<core::i32, core::i32>, int> coverage(const core::AtomTable& atoms,
                                                                      const std::vector<TerrainNodeDraw>& draws)
{
    std::map<std::pair<core::i32, core::i32>, int> covered;
    for (const TerrainNodeDraw& draw : draws) {
        const TerrainNodeKey key = keyOf(atoms, draw.urn);
        const auto across = static_cast<core::i32>(1u << key.level);
        for (core::i32 z = 0; z < across; ++z) {
            for (core::i32 x = 0; x < across; ++x)
                covered[{key.x * across + x, key.z * across + z}] += 1;
        }
    }
    return covered;
}

} // namespace

TEST_CASE("with no viewer nothing is meshed or drawn")
{
    LoaderFixture fixture;
    (void)fixture.sync();
    CHECK(fixture.loader.residentCount() == 0);
    CHECK(fixture.loader.draws(fixture.world).empty());
}

TEST_CASE("the ground is drawn once everywhere, finest under the viewer")
{
    LoaderFixture fixture;
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    const std::vector<TerrainNodeDraw> draws = fixture.settle();
    REQUIRE_FALSE(draws.empty());

    // 256 m of 32 m chunk columns is eight a side, each drawn exactly once.
    const auto covered = coverage(fixture.atoms, draws);
    for (core::i32 z = -4; z < 4; ++z) {
        for (core::i32 x = -4; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(z);
            REQUIRE(covered.contains({x, z}));
            CHECK(covered.at({x, z}) == 1);
        }
    }
    // The column under the viewer at full detail; one far away coarser.
    core::u32 under = 99;
    core::u32 far = 0;
    for (const TerrainNodeDraw& draw : draws) {
        const TerrainNodeKey key = keyOf(fixture.atoms, draw.urn);
        const auto across = static_cast<core::i32>(1u << key.level);
        if (key.x * across <= 0 && 0 < (key.x + 1) * across && key.z * across <= 0 && 0 < (key.z + 1) * across)
            under = key.level;
        far = std::max(far, key.level);
    }
    CHECK(under == 0);
    CHECK(far > 0);
}

TEST_CASE("a quiet frame builds nothing, and an edit rebuilds every node it touched in one frame")
{
    LoaderFixture fixture;
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    (void)fixture.settle();
    (void)fixture.sync();
    CHECK(fixture.loader.lastBuilds() == 0);

    // A dig in the middle of one column: that column's node and the eight
    // round it, whose openness reads 12 m into it -- **all in the frame the dig
    // lands**, whatever the loading budget. Spread over frames, neighbours
    // showed two versions of one edit: the owner's flicker while editing.
    fixture.loader.setBuildsPerSync(1);
    (void)asset::fillBall(fixture.component().field, core::DVec3{16.0, 0.0, 16.0}, 3.0, 0);
    fixture.component().fieldRevision += 1;
    (void)fixture.sync();
    CHECK(fixture.loader.lastBuilds() == 9);
    (void)fixture.sync();
    CHECK(fixture.loader.lastBuilds() == 0);
}

TEST_CASE("a node is drawn until its children are ready, so a change of level shows no hole")
{
    LoaderFixture fixture;
    // Far away first: coarse nodes only.
    fixture.loader.setFocus(core::DVec3{2000.0, 4.0, 2000.0});
    const std::vector<TerrainNodeDraw> far = fixture.settle();
    REQUIRE_FALSE(far.empty());

    // Then close, one build a frame: every frame still covers every column.
    fixture.loader.setBuildsPerSync(1);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    for (int frame = 0; frame < 40; ++frame) {
        (void)fixture.sync();
        const auto covered = coverage(fixture.atoms, fixture.loader.draws(fixture.world));
        for (core::i32 z = -4; z < 4; ++z) {
            for (core::i32 x = -4; x < 4; ++x) {
                CAPTURE(frame);
                CAPTURE(x);
                CAPTURE(z);
                CHECK(covered.contains({x, z}));
            }
        }
    }
}

TEST_CASE("the ground under the viewer stays at full detail for as long as the viewer stays")
{
    // **The owner's flicker in the cave example.** The ancestors of the nodes
    // drawn close up were let go after a few seconds undrawn; one rebuilt
    // counted as ready and was drawn over the whole close-up, coarse, for the
    // frames its children took to come back -- every few seconds, a pop from
    // the finest ground to the coarsest and back.
    LoaderFixture fixture;
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    (void)fixture.settle();
    fixture.loader.setBuildsPerSync(1);
    for (int frame = 0; frame < 600; ++frame) {
        // Drifting slowly, as a walking camera does.
        fixture.loader.setFocus(core::DVec3{8.0 + frame * 0.01, 4.0, 8.0});
        (void)fixture.sync();
        core::u32 under = 99;
        for (const TerrainNodeDraw& draw : fixture.loader.draws(fixture.world)) {
            const TerrainNodeKey key = keyOf(fixture.atoms, draw.urn);
            const auto across = static_cast<core::i32>(1u << key.level);
            if (key.x * across <= 0 && 0 < (key.x + 1) * across && key.z * across <= 0 && 0 < (key.z + 1) * across)
                under = key.level;
        }
        CAPTURE(frame);
        REQUIRE(under == 0);
    }
}

TEST_CASE("the mesh a node is drawn with is its ground, stitched rather than skirted")
{
    LoaderFixture fixture;
    const asset::TerrainMesh leaf = meshTerrainNode(fixture.component().field, TerrainNodeKey{0, 0, 0});
    REQUIRE_FALSE(leaf.mesh.indices.empty());
    // No skirts (ADR 0140): what is drawn is the collider's surface.
    CHECK(leaf.mesh.indices.size() == leaf.colliderIndices.size());
    const asset::TerrainMesh nothing = meshTerrainNode(fixture.component().field, TerrainNodeKey{0, 40, 40});
    CHECK(nothing.mesh.indices.empty());
}

TEST_CASE("the render terrain carries its layers, and only for terrain in the world")
{
    LoaderFixture fixture;
    // The engine's eight and the slope rock, set as a scene from before
    // terrains started empty reads them.
    fixture.component().layers = asset::defaultTerrainLayers();
    fixture.component().rules = asset::defaultTerrainRules();
    RenderWorld snapshot;
    fixture.loader.appendRenderTerrains(fixture.world, fixture.root, snapshot);
    REQUIRE(snapshot.terrains.size() == 1);
    CHECK(snapshot.terrains.front().id == fixture.terrain);
    // The engine's eight (ADR 0113), flat in the old palette's colours until
    // their textures load, and the slope rock.
    REQUIRE(snapshot.terrains.front().layers.size() == 8);
    const core::Vec3 grass = asset::terrainColorOf(1);
    CHECK(snapshot.terrains.front().layers[0].flat[1] == grass.y);
    REQUIRE(snapshot.terrains.front().rules.size() == 1);
    CHECK(snapshot.terrains.front().rules[0].material == 3.0f);

    RenderWorld elsewhere;
    fixture.loader.appendRenderTerrains(fixture.world, core::InstanceId{}, elsewhere);
    CHECK(elsewhere.terrains.empty());
}

TEST_CASE("a terrain that goes away takes its meshes with it")
{
    LoaderFixture fixture;
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    (void)fixture.settle();
    REQUIRE(fixture.loader.residentCount() > 0);
    fixture.world.terrains().remove(fixture.terrain);
    (void)fixture.sync();
    CHECK(fixture.loader.residentCount() == 0);
    CHECK(fixture.library.size() == 0);
}

// --- What a dig costs to draw, measured (D164) ---------------------------------
//
// The owner's report was a dig that stalled the editor. A dig now rebuilds the
// mesh of the node it touched, so the number that decides whether it stalls is
// what meshing one node costs. Skipped by default and run on demand, the shape
// every cost measurement in this repository takes: a number that varies with
// the machine must not gate anything.
//
//     engine_render_tests --test-case="what meshing a terrain node costs" --no-skip
TEST_CASE("what meshing a terrain node costs" * doctest::skip())
{
    for (const float voxel : {1.0f, 0.5f}) {
        asset::TerrainField field(asset::FieldSettings{.voxelSize = voxel, .minHeight = -64.0f, .maxHeight = 64.0f});
        const float size = 256.0f * voxel;
        // In doubles for the brushes, which take world positions (R9).
        const auto v = static_cast<double>(voxel);
        (void)asset::fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, size, 0.0f, 1);
        (void)asset::raiseBall(field, core::DVec3{16.0 * v, 0.0, 16.0 * v}, 12.0 * v, 6.0f * voxel);
        (void)asset::fillBall(field, core::DVec3{16.0 * v, -2.0 * v, 16.0 * v}, 5.0 * v, 0);

        const auto start = std::chrono::steady_clock::now();
        constexpr int Runs = 20;
        std::size_t triangles = 0;
        for (int run = 0; run < Runs; ++run)
            triangles = meshTerrainNode(field, TerrainNodeKey{0, 0, 0}).mesh.indices.size() / 3;
        const double leaf =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / Runs;

        const auto brushStart = std::chrono::steady_clock::now();
        for (int run = 0; run < Runs; ++run)
            (void)asset::fillBall(field, core::DVec3{8.0 * v, 0.0, 8.0 * v}, 6.0 * v,
                                  static_cast<core::u8>(run % 2 == 0 ? 0 : 1));
        const double brush =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - brushStart).count() / Runs;

        MESSAGE("voxel " << voxel << " m: a full-detail node meshes in " << leaf << " ms (" << triangles
                         << " triangles); a ball of radius 6 voxels writes in " << brush << " ms");
    }
}

TEST_CASE("a node is stitched to the level drawn beside it where that is coarser, and to nothing else")
{
    // **Stitching** (ADR 0140): only a coarser neighbour's vertices differ
    // from a node's own; the same level shares its edge, and a finer
    // neighbour stitches itself.
    //
    // A level-0 node at x 1, z 2: the same level on its low x and its high
    // z, a level-1 node over its high x -- and so over its high-x, high-z
    // corner too, which that node's cells straddle -- and nothing on its low z.
    std::vector<TerrainNodeKey> drawn;
    drawn = {
        TerrainNodeKey{0, 1, 2}, // the subject
        TerrainNodeKey{0, 0, 2}, // same level, low x
        TerrainNodeKey{1, 1, 1}, // coarser: level-0 x 2..3, z 2..3 -- the high x
        TerrainNodeKey{0, 1, 3}, // same level, high z
    };
    std::sort(drawn.begin(), drawn.end());
    CHECK(terrainStitchSides(drawn, TerrainNodeKey{0, 1, 2}) == TerrainSides{0, 1, 0, 0, 0, 0, 0, 1});

    // A node with only finer neighbours is stitched to nothing.
    std::vector<TerrainNodeKey> finer{TerrainNodeKey{1, 0, 0}, TerrainNodeKey{0, 2, 0}, TerrainNodeKey{0, 2, 1}};
    std::sort(finer.begin(), finer.end());
    CHECK(terrainStitchSides(finer, TerrainNodeKey{1, 0, 0}) == TerrainSides{});
    // And the finer one beside it is stitched to the coarse one's level, on
    // its low x and on the corner the coarse one also covers.
    CHECK(terrainStitchSides(finer, TerrainNodeKey{0, 2, 0}) == TerrainSides{1, 0, 0, 0, 0, 0, 1, 0});
}

TEST_CASE("built off the main thread, the ground is drawn once it is built, whole")
{
    // **TA14**: `sync` hands its builds to workers and draws what it has.
    // Once they are back, the ground is drawn, every column of it covered.
    LoaderFixture fixture;
    fixture.loader.setAsync(true);
    fixture.loader.setBuildsPerSync(4);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    std::vector<TerrainNodeDraw> drawn;
    for (int frame = 0; frame < 400 && (drawn.empty() || fixture.loader.pending()); ++frame) {
        (void)fixture.sync();
        drawn = fixture.loader.draws(fixture.world);
    }
    CHECK_FALSE(fixture.loader.pending());
    REQUIRE_FALSE(drawn.empty());
    const auto covered = coverage(fixture.atoms, drawn);
    for (core::i32 z = -4; z < 4; ++z) {
        for (core::i32 x = -4; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(z);
            CHECK(covered.contains({x, z}));
        }
    }
}

TEST_CASE("areaMeshed: the ground round a place is meshed only once every node wanted there is drawn")
{
    // ADR 0159: what `Terrain:WaitForMeshAsync` and the loading curtain wait
    // for -- not that the field is resident, which it is from the start, but
    // that what will be drawn there has its mesh built and put up.
    LoaderFixture fixture;
    fixture.loader.setAsync(true);
    fixture.loader.setBuildsPerSync(2);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    // Before anything is built, nothing is meshed where the ground is.
    (void)fixture.sync();
    REQUIRE(fixture.loader.pending());
    CHECK_FALSE(fixture.loader.areaMeshed(fixture.world, core::InstanceId{}, core::DVec3{8.0, 4.0, 8.0}, 8.0));
    // Never said while the loader still wants ground: the levels are refined
    // one a `sync`, and ground with nothing missing in one frame can still be
    // sharpening in the next.
    int frames = 0;
    for (; frames < 400 && fixture.loader.pending(); ++frames) {
        CHECK_FALSE(fixture.loader.areaMeshed(fixture.world, core::InstanceId{}, core::DVec3{8.0, 4.0, 8.0}, 8.0));
        (void)fixture.sync();
    }
    REQUIRE_FALSE(fixture.loader.pending());
    CHECK(frames > 1);
    CHECK(fixture.loader.areaMeshed(fixture.world, core::InstanceId{}, core::DVec3{8.0, 4.0, 8.0}, 8.0));
    CHECK(fixture.loader.areaMeshed(fixture.world, core::InstanceId{}, core::DVec3{8.0, 4.0, 8.0}, 64.0));
    // And far from all of it, where nothing is wanted.
    CHECK(fixture.loader.areaMeshed(fixture.world, core::InstanceId{}, core::DVec3{5000.0, 0.0, 5000.0}, 8.0));
}

TEST_CASE("a brush stamp is drawn in the frame that finds it, built off the main thread or not")
{
    // The owner's "everything in the terrain editor feels delayed": an edit
    // was handed to workers and put up six meshes a frame, three frames after
    // the stamp. A small edit is built where it is found.
    LoaderFixture fixture;
    fixture.loader.setAsync(true);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    for (int frame = 0; frame < 400 && (frame < 2 || fixture.loader.pending()); ++frame)
        (void)fixture.sync();
    REQUIRE_FALSE(fixture.loader.pending());
    REQUIRE(fixture.loader.drawnOutOfDate(fixture.world).empty());
    const core::u64 measured = fixture.loader.editLatency().edits;

    // A brush of four metres, where four chunks meet: the nodes round it.
    (void)asset::fillBall(fixture.component().field, core::DVec3{0.0, 0.0, 0.0}, 4.0, 1);
    fixture.component().fieldRevision += 1;
    (void)fixture.sync();

    // One `sync`, and nothing drawn is of the ground as it was.
    CHECK(fixture.loader.drawnOutOfDate(fixture.world).empty());
    CHECK_FALSE(fixture.loader.pending());
    const TerrainLoader::EditLatency latency = fixture.loader.editLatency();
    CHECK(latency.edits == measured + 1);
    CHECK(latency.lastFrames == 0);
}

TEST_CASE("built off the main thread, every node drawn is built for the seams it is drawn with, every frame")
{
    // **TA14**: a change of level moves the levels drawn beside a node, and a
    // node drawn with seams built for other levels opens a crack. Built off
    // the main thread, the change is drawn only once every node it touches is
    // built for it -- here, flying in from far away and out again, every frame.
    LoaderFixture fixture;
    fixture.loader.setAsync(true);
    fixture.loader.setBuildsPerSync(3);
    // Hills, so the levels differ between neighbours.
    for (const double x : {-60.0, 0.0, 70.0})
        (void)asset::fillBall(fixture.component().field, core::DVec3{x, -6.0, 20.0}, 22.0, 1);
    fixture.component().fieldRevision += 1;
    for (int frame = 0; frame < 240; ++frame) {
        const double t = static_cast<double>(frame < 120 ? 120 - frame : frame - 120) / 120.0;
        fixture.loader.setFocus(core::DVec3{8.0, 4.0 + 900.0 * t, 8.0});
        (void)fixture.sync();
        const auto seams = fixture.loader.drawnSeams(fixture.world);
        std::vector<TerrainNodeKey> keys;
        for (const auto& [key, sides] : seams)
            keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& [key, sides] : seams) {
            CAPTURE(frame);
            CAPTURE(key.level);
            CAPTURE(key.x);
            CAPTURE(key.z);
            CHECK(sides == terrainStitchSides(keys, key));
        }
    }
}

TEST_CASE("a change of seams is built off the main thread: only an edit is built in the frame that finds it")
{
    // **D541**: a node built before and wanted with other seams -- the level
    // drawn beside it changed -- was asked for as an edit is, and a few of
    // them were "a small edit": built in the frame that found them, on every
    // worker, the frame waiting. A camera that moved, and a map coming in a
    // batch at a time, paid 7 to 32 ms a frame for it -- thirty-five frames
    // running, on a game's first second in its map.
    LoaderFixture fixture;
    fixture.loader.setAsync(true);
    fixture.loader.setBuildsPerSync(3);
    for (const double x : {-60.0, 0.0, 70.0})
        (void)asset::fillBall(fixture.component().field, core::DVec3{x, -6.0, 20.0}, 22.0, 1);
    fixture.component().fieldRevision += 1;
    core::u32 builtInFrame = 0;
    for (int frame = 0; frame < 240; ++frame) {
        const double t = static_cast<double>(frame < 120 ? 120 - frame : frame - 120) / 120.0;
        fixture.loader.setFocus(core::DVec3{8.0, 4.0 + 900.0 * t, 8.0});
        (void)fixture.sync();
        builtInFrame += fixture.loader.lastBuildsInFrame();
    }
    CHECK(builtInFrame == 0);

    // **Ground a script wrote where there was none**, as a game that makes
    // its map does: it comes in off the main thread, seams and all.
    LoaderFixture written;
    written.loader.setAsync(true);
    written.loader.setBuildsPerSync(3);
    written.component().field =
        asset::TerrainField(asset::FieldSettings{.voxelSize = 1.0f, .minHeight = -32.0f, .maxHeight = 64.0f});
    written.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    (void)written.sync();
    (void)asset::fillFlat(written.component().field, core::DVec3{0.0, 0.0, 0.0}, 256.0f, 0.0f, 1);
    for (const double x : {-60.0, 0.0, 70.0})
        (void)asset::fillBall(written.component().field, core::DVec3{x, -6.0, 20.0}, 22.0, 1);
    written.component().fieldRevision += 1;
    builtInFrame = 0;
    int frames = 0;
    for (; frames < 400 && (frames < 2 || written.loader.pending()); ++frames) {
        (void)written.sync();
        builtInFrame += written.loader.lastBuildsInFrame();
    }
    REQUIRE_FALSE(written.loader.pending());
    CHECK(builtInFrame == 0);

    // And an edit still is: a brush stamp on that ground, in one `sync`.
    (void)asset::fillBall(written.component().field, core::DVec3{0.0, 0.0, 0.0}, 4.0, 1);
    written.component().fieldRevision += 1;
    (void)written.sync();
    CHECK(written.loader.lastBuildsInFrame() > 0);
    CHECK(written.loader.drawnOutOfDate(written.world).empty());
}

TEST_CASE("ground a streamer let go is not drawn from the meshes it had, however the camera moved")
{
    // ludwerk-08's plates on the owner's place (2026-09-30): pale squares of
    // ground past the loaded edge, a different few each run. The streamer
    // evicts the cells a moving camera leaves behind, and a node drawn before
    // was drawn again in place of one not built yet -- with the ground it had,
    // which was gone.
    for (const bool async : {false, true}) {
        CAPTURE(async);
        LoaderFixture fixture(512.0f);
        fixture.loader.setAsync(async);
        fixture.loader.setBuildsPerSync(async ? 6 : 1000);
        // Hills, so every level has something to be built for.
        for (const double x : {-150.0, -40.0, 90.0, 180.0})
            (void)asset::fillBall(fixture.component().field, core::DVec3{x, -10.0, x * 0.5}, 30.0, 1);
        fixture.component().fieldRevision += 1;
        fixture.loader.setFocus(core::DVec3{-180.0, 60.0, -180.0});
        for (int frame = 0; frame < 120; ++frame)
            (void)fixture.sync();

        // The camera jumps across, and the cells it left are let go.
        asset::TerrainField& field = fixture.component().field;
        std::vector<asset::ChunkKey> gone;
        for (const asset::TerrainField::Entry& entry : field.chunks()) {
            if (entry.first.x < 0 && entry.first.z < 0)
                gone.push_back(entry.first);
        }
        REQUIRE_FALSE(gone.empty());
        for (const asset::ChunkKey key : gone)
            field.removeChunk(key);
        fixture.component().fieldRevision += 1;
        fixture.loader.setFocus(core::DVec3{200.0, 60.0, 200.0});
        std::vector<TerrainNodeKey> stale;
        for (int frame = 0; frame < 120; ++frame) {
            (void)fixture.sync();
            stale = fixture.loader.drawnOutOfDate(fixture.world);
        }
        for (const TerrainNodeKey& key : stale) {
            CAPTURE(key.level);
            CAPTURE(key.x);
            CAPTURE(key.z);
            CHECK(false);
        }
        CHECK(stale.empty());
    }
}

TEST_CASE("a node built empty is built again when ground comes back under it")
{
    // ludwerk-08's plates on the owner's place (2026-09-30): pale squares of
    // ground floating past the loaded edge, a different few each run. A node
    // whose first mesh was of nothing -- ground dug to air leaves its chunks,
    // air; a streamed cell comes in before the one with the surface -- was
    // drawn by nothing, and a load was asked for only of a node never built:
    // when the ground came, the node went on having nothing, its children were
    // not shown while it stood, and the ground under it was never drawn. What
    // was drawn round it floated.
    for (const bool async : {false, true}) {
        CAPTURE(async);
        LoaderFixture fixture;
        fixture.loader.setAsync(async);
        fixture.loader.setBuildsPerSync(async ? 6 : 1000);
        // A corner dug to air from the start: its chunks are there, and there
        // is nothing in them to draw.
        asset::TerrainField& field = fixture.component().field;
        (void)asset::fillBlock(field, core::DVec3{64.0, 16.0, 64.0}, core::Vec3{128.0f, 96.0f, 128.0f}, 0);
        fixture.component().fieldRevision += 1;
        fixture.loader.setFocus(core::DVec3{8.0, 400.0, 8.0});
        for (int frame = 0; frame < 60; ++frame)
            (void)fixture.sync();

        // And then the ground comes.
        (void)asset::fillFlat(field, core::DVec3{64.0, 0.0, 64.0}, 128.0f, 2.0f, 1);
        fixture.component().fieldRevision += 1;
        for (int frame = 0; frame < 60; ++frame)
            (void)fixture.sync();
        const auto after = coverage(fixture.atoms, fixture.loader.draws(fixture.world));
        // Every chunk column of the corner is drawn: 128 m of 32 m chunks.
        for (core::i32 z = 0; z < 4; ++z) {
            for (core::i32 x = 0; x < 4; ++x) {
                CAPTURE(x);
                CAPTURE(z);
                CHECK(after.contains({x, z}));
            }
        }
    }
}

TEST_CASE("the ground is drawn as far as the camera sees")
{
    // **An 8 km world ended half-way across** (terrain-editing ledger, P5):
    // the loader drew nothing past 4096 m whatever the camera's far plane, and
    // from a corner of the world the far ground was a scalloped edge of whole
    // nodes against the haze -- ludwerk-08's "V-shaped notches". The settings
    // a camera gives carry its far plane.
    LoaderFixture fixture(64.0f);
    // A second patch of ground nine kilometres off.
    asset::TerrainField& field = fixture.component().field;
    (void)asset::fillFlat(field, core::DVec3{9000.0, 0.0, 0.0}, 64.0f, 0.0f, 1);
    fixture.component().fieldRevision += 1;
    fixture.loader.setFocus(core::DVec3{0.0, 40.0, 0.0});
    const core::Mat4 projection = core::perspective(1.0f, 16.0f / 9.0f, 0.1f, 20000.0f);
    const auto farPatchDrawn = [&] {
        for (int frame = 0; frame < 30; ++frame)
            (void)fixture.sync();
        // Chunk columns are 32 m: the far patch is past the 250th.
        for (const auto& [column, times] : coverage(fixture.atoms, fixture.loader.draws(fixture.world))) {
            if (column.first > 250 && times > 0)
                return true;
        }
        return false;
    };

    // The camera of every scene so far: five kilometres. **Out to the far
    // plane's corners**, which through this lens are half as far again -- a
    // node at the side of the picture is inside the plane and further than
    // it, and dropped at the plane's distance it left the ground scalloped.
    fixture.loader.setLodSettings(terrainLodFor(TerrainLodSettings{}, projection, 5000.0f, 720, 1.5));
    const double halfHeight = std::tan(0.5);
    const double halfWidth = halfHeight * 16.0 / 9.0;
    const double reach = std::sqrt(1.0 + halfWidth * halfWidth + halfHeight * halfHeight);
    CHECK(fixture.loader.lodSettings().viewDistance == doctest::Approx(5000.0 * reach).epsilon(0.001));
    CHECK(reach > 1.4);
    CHECK_FALSE(farPatchDrawn());

    // One that sees twenty.
    fixture.loader.setLodSettings(terrainLodFor(TerrainLodSettings{}, projection, 20000.0f, 720, 1.5));
    CHECK(farPatchDrawn());
    // An orthographic view keeps the distance rule and sees as far as its
    // plane, and a far plane of nothing keeps the distance it had.
    core::Mat4 flat = projection;
    flat.m[3][3] = 1.0f;
    CHECK(terrainLodFor(TerrainLodSettings{}, flat, 300.0f, 720, 1.5).viewDistance == doctest::Approx(300.0));
    const TerrainLodSettings kept = terrainLodFor(TerrainLodSettings{}, flat, 0.0f, 720, 1.5);
    CHECK(kept.pixelScale == 0.0);
    CHECK(kept.viewDistance == doctest::Approx(4096.0));
}

// --- Ground drawn from cells on disk (ADR 0144) --------------------------------

TEST_CASE("a node of ground on disk is the node the resident ground gives, before and after its cells come in")
{
    // Rolling ground over a dozen cells a side, a 2 m slab and a ball across
    // the cells' borders, at a metre voxel.
    const asset::FieldSettings settings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f};
    asset::TerrainField whole(settings);
    // A slab 2 m thick, floating -- its underside on a chunk's face -- as the
    // owner's place is; and ground filled from the floor beside it.
    (void)asset::fillBlock(whole, core::DVec3{0.0, 1.0, 0.0}, core::Vec3{768.0f, 2.0f, 512.0f}, 1);
    (void)asset::fillFlat(whole, core::DVec3{0.0, 0.0, 512.0}, 512.0f, 0.0f, 1);
    (void)asset::fillBlock(whole, core::DVec3{-40.0, 3.0, 20.0}, core::Vec3{160.0f, 2.0f, 70.0f}, 2);
    (void)asset::fillBall(whole, core::DVec3{64.0, 6.0, -64.0}, 24.0, 3);
    const std::vector<asset::TerrainCell> cells = asset::splitTerrain(whole);
    REQUIRE(cells.size() > 100);

    std::vector<asset::ChunkId> ids;
    std::map<std::pair<core::i32, core::i32>, const asset::TerrainCell*> byId;
    for (const asset::TerrainCell& cell : cells) {
        ids.push_back(asset::ChunkId{cell.x, cell.z, asset::FieldLayerTerrain});
        byId[{cell.x, cell.z}] = &cell;
    }
    int reads = 0;
    const asset::TerrainCellSource source(
        asset::terrainCellChunks(settings.voxelSize), ids, [&](asset::ChunkId id) -> std::optional<asset::TerrainCell> {
            ++reads;
            const auto found = byId.find({id.x, id.z});
            return found == byId.end() ? std::nullopt : std::optional<asset::TerrainCell>(*found->second);
        });

    // Resident: the cells west of x = 0, as a streamer round a camera there
    // would hold them; none, the camera far off; or three by three round one.
    const auto residentWhere = [&](const std::function<bool(const asset::TerrainCell&)>& kept) {
        asset::TerrainField resident(settings);
        for (const asset::TerrainCell& cell : cells) {
            if (kept(cell)) {
                for (const asset::TerrainField::Entry& entry : cell.field.chunks())
                    resident.setChunk(entry.first, entry.second);
            }
        }
        return resident;
    };
    const std::vector<asset::TerrainField> residents{
        residentWhere([](const asset::TerrainCell& cell) { return cell.x < 0; }),
        residentWhere([](const asset::TerrainCell&) { return false; }),
        residentWhere(
            [](const asset::TerrainCell& cell) { return std::abs(cell.x - 2) <= 1 && std::abs(cell.z + 1) <= 1; }),
    };

    // What the whole ground gives a node, meshed once and asked of by every
    // resident set: the dearest half of this test, done three times over.
    std::map<std::tuple<core::u32, core::i32, core::i32>, std::pair<asset::TerrainMesh, core::u64>> expectedOf;
    const auto expectedFor = [&](const TerrainNodeKey& key) -> const std::pair<asset::TerrainMesh, core::u64>& {
        const auto at = std::tuple{key.level, key.x, key.z};
        auto found = expectedOf.find(at);
        if (found == expectedOf.end())
            // The whole ground, resident, of the same cells: a node is named by
            // what its cells hold (ADR 0150), wherever they are.
            found =
                expectedOf.emplace(at, std::pair{meshTerrainNode(whole, key), terrainNodeContent(whole, &source, key)})
                    .first;
        return found->second;
    };
    int compared = 0;
    for (const asset::TerrainField& resident : residents) {
        for (core::u32 level = 1; level <= TerrainTopLevel; ++level) {
            const core::i32 n = 1 << level;
            for (core::i32 z = -12 / n - 1; z <= 12 / n; ++z) {
                for (core::i32 x = -12 / n - 1; x <= 12 / n; ++x) {
                    const TerrainNodeKey key{level, x, z};
                    if (!terrainNodeReadsCells(resident, &source, key))
                        continue;
                    CAPTURE(level);
                    CAPTURE(x);
                    CAPTURE(z);
                    const auto& [expected, wholeContent] = expectedFor(key);
                    const TerrainNodeBuild built = buildTerrainNodeFromCells(resident, source, key);
                    REQUIRE(built.mesh.mesh.vertices.size() == expected.mesh.vertices.size());
                    REQUIRE(built.mesh.mesh.indices == expected.mesh.indices);
                    bool same = true;
                    for (std::size_t at = 0; at < expected.mesh.vertices.size(); ++at) {
                        const asset::Vertex& a = built.mesh.mesh.vertices[at];
                        const asset::Vertex& b = expected.mesh.vertices[at];
                        same = same && a.position.x == b.position.x && a.position.y == b.position.y &&
                               a.position.z == b.position.z && a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
                               a.normal.z == b.normal.z && a.tangent[0] == b.tangent[0] &&
                               a.tangent[1] == b.tangent[1] && a.tangent[2] == b.tangent[2] &&
                               a.tangent[3] == b.tangent[3];
                    }
                    CHECK(same);
                    CHECK(built.mesh.morphs == expected.morphs);
                    // What it was built from is what the whole ground gives, what
                    // the ground on disk gives now its cells have been read, and
                    // what it will give once they are resident.
                    CHECK(built.content == wholeContent);
                    CHECK(terrainNodeContent(resident, &source, key) == built.content);
                    ++compared;
                }
            }
        }
    }
    CHECK(compared > 10);
    // The cells were read, and summarised once each: not once a node.
    CHECK(reads > 0);
}

TEST_CASE("a streamed terrain is drawn whole, once everywhere, however little of it is resident")
{
    LoaderFixture fixture(768.0f);
    scene::TerrainComponent& component = fixture.component();
    const asset::FieldSettings settings = component.field.settings();
    const std::vector<asset::TerrainCell> cells = asset::splitTerrain(component.field);
    std::vector<asset::ChunkId> ids;
    std::map<std::pair<core::i32, core::i32>, asset::TerrainCell> byId;
    for (const asset::TerrainCell& cell : cells) {
        ids.push_back(asset::ChunkId{cell.x, cell.z, asset::FieldLayerTerrain});
        byId[{cell.x, cell.z}] = cell;
    }
    component.cellSource = std::make_shared<const asset::TerrainCellSource>(
        asset::terrainCellChunks(settings.voxelSize), ids,
        [byId](asset::ChunkId id) -> std::optional<asset::TerrainCell> {
            const auto found = byId.find({id.x, id.z});
            return found == byId.end() ? std::nullopt : std::optional<asset::TerrainCell>(found->second);
        });
    // Resident: three cells by three round the camera.
    asset::TerrainField resident(settings);
    for (const asset::TerrainCell& cell : cells) {
        if (std::abs(cell.x) <= 1 && std::abs(cell.z) <= 1) {
            for (const asset::TerrainField::Entry& entry : cell.field.chunks())
                resident.setChunk(entry.first, entry.second);
        }
    }
    component.field = resident;
    component.fieldRevision += 1;

    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    const std::vector<TerrainNodeDraw> draws = fixture.settle();
    const auto covered = coverage(fixture.atoms, draws);
    // 768 m of 32 m chunk columns is 24 a side, each drawn exactly once.
    int missing = 0;
    int twice = 0;
    for (core::i32 z = -12; z < 12; ++z) {
        for (core::i32 x = -12; x < 12; ++x) {
            const auto found = covered.find({x, z});
            missing += found == covered.end() ? 1 : 0;
            twice += found != covered.end() && found->second > 1 ? 1 : 0;
        }
    }
    CHECK(missing == 0);
    CHECK(twice == 0);
}

TEST_CASE("terrain shader permutations preserve hex layers across views and live material edits")
{
    engine::render::RenderWorld world;
    CHECK_FALSE(engine::render::terrainNeedsHexSampling(world));
    world.terrains.resize(2);
    world.terrains[0].layers.resize(2);
    world.terrains[1].layers.resize(2);
    CHECK_FALSE(engine::render::terrainNeedsHexSampling(world));
    auto& layer = world.terrains[1].layers[1];
    layer.tiling[2] = 0.5f;
    CHECK_FALSE(engine::render::terrainNeedsHexSampling(world));
    layer.tiling[2] = 1.0f;
    layer.waiting = true;
    CHECK(engine::render::terrainNeedsHexSampling(world));
    layer.waiting = false;
    CHECK(engine::render::terrainNeedsHexSampling(world));
    layer.tiling[2] = 0.0f;
    CHECK_FALSE(engine::render::terrainNeedsHexSampling(world));
    world.terrains[0].layers[0].tiling[2] = 1.0f;
    CHECK(engine::render::terrainNeedsHexSampling(world));
    world.terrains.erase(world.terrains.begin());
    CHECK_FALSE(engine::render::terrainNeedsHexSampling(world));
}

TEST_CASE("triplanar terrain permutation falls back for a planar layer without losing height-map flags")
{
    engine::render::RenderWorld world;
    world.terrains.resize(2);
    world.terrains[0].layers.resize(1);
    world.terrains[1].layers.resize(1);
    world.terrains[0].layers[0].surface[3] = 1.0f;
    world.terrains[1].layers[0].surface[3] = 3.0f;
    CHECK(engine::render::terrainIsTriplanarOnly(world));
    world.terrains[1].layers[0].surface[3] = 2.0f;
    CHECK_FALSE(engine::render::terrainIsTriplanarOnly(world));
    world.terrains[1].layers[0].surface[3] = 0.0f;
    CHECK_FALSE(engine::render::terrainIsTriplanarOnly(world));
    world.terrains[1].layers[0].surface[3] = 1.0f;
    CHECK(engine::render::terrainIsTriplanarOnly(world));
}

TEST_CASE("terrain geometry reuse survives scene replacement without retaining instance handles")
{
    LoaderFixture fixture(64.0f);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    const auto first = fixture.settle();
    REQUIRE_FALSE(first.empty());
    REQUIRE(fixture.loader.meshReuseBytes() > 0);
    const auto old = fixture.terrain;
    const auto component = fixture.component();
    const auto hits = fixture.loader.meshReuseHits();
    fixture.world.terrains().remove(old);
    REQUIRE(fixture.world.destroy(old));
    fixture.terrain = fixture.world.create(fixture.terrainClass);
    fixture.world.terrains().add(fixture.terrain, component);
    REQUIRE(fixture.world.setParent(fixture.terrain, fixture.root) == std::nullopt);
    const auto next = fixture.settle();
    REQUIRE(next.size() == first.size());
    CHECK(fixture.loader.meshReuseHits() > hits);
    CHECK(fixture.loader.meshReuseBytes() <= 64 * 1024 * 1024);
    for (const auto& draw : next) {
        CHECK(draw.terrain == fixture.terrain);
        REQUIRE(fixture.library.find(draw.urn) != nullptr);
        CHECK(fixture.cache.resolve(fixture.library.find(draw.urn)->mesh) != nullptr);
    }
    for (const auto& draw : first)
        CHECK(fixture.library.find(draw.urn) == nullptr);
    CHECK(fixture.loader.drawnOutOfDate(fixture.world).empty());
    fixture.loader.setMeshReuseBudget(0);
    CHECK(fixture.loader.meshReuseBytes() == 0);
}

TEST_CASE("terrain geometry reuse respects edits paint scale and its memory ceiling")
{
    LoaderFixture fixture(64.0f);
    fixture.loader.setFocus(core::DVec3{8.0, 4.0, 8.0});
    (void)fixture.settle();
    const auto original = fixture.component().field;
    const auto replace = [&](asset::TerrainField field) {
        fixture.world.terrains().remove(fixture.terrain);
        REQUIRE(fixture.world.destroy(fixture.terrain));
        fixture.terrain = fixture.world.create(fixture.terrainClass);
        scene::TerrainComponent component;
        component.field = std::move(field);
        fixture.world.terrains().add(fixture.terrain, std::move(component));
        REQUIRE(fixture.world.setParent(fixture.terrain, fixture.root) == std::nullopt);
        (void)fixture.settle();
        CHECK(fixture.loader.drawnOutOfDate(fixture.world).empty());
    };
    auto changed = original;
    (void)asset::fillBall(changed, core::DVec3{0.0, 0.0, 0.0}, 100.0, 2);
    const auto beforePaint = fixture.loader.meshReuseHits();
    replace(changed);
    CHECK(fixture.loader.meshReuseHits() == beforePaint);
    const auto beforeRestore = fixture.loader.meshReuseHits();
    replace(original);
    CHECK(fixture.loader.meshReuseHits() > beforeRestore);
    asset::TerrainField scaled(asset::FieldSettings{.voxelSize = 2.0f, .minHeight = -32.0f, .maxHeight = 64.0f});
    (void)asset::fillFlat(scaled, core::DVec3{}, 128.0f, 0.0f, 1);
    const auto beforeScale = fixture.loader.meshReuseHits();
    replace(std::move(scaled));
    CHECK(fixture.loader.meshReuseHits() == beforeScale);
    fixture.loader.setMeshReuseBudget(1024);
    CHECK(fixture.loader.meshReuseBytes() <= 1024);
    replace(original);
    CHECK(fixture.loader.meshReuseBytes() <= 1024);
    fixture.loader.destroy(*fixture.device, fixture.cache, fixture.library);
    CHECK(fixture.loader.meshReuseBytes() == 0);
}
