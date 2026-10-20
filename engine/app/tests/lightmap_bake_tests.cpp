// Light baked into what stands still (ADR 0253): the rectangles a page is
// cut into, and a bake's light and mask against a world whose answers are
// known.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/app/indirect_light.h"
#include "engine/app/lightmap_bake.h"
#include "engine/app/lightmap_file.h"
#include "engine/asset/primitives.h"
#include "engine/asset/texture.h"
#include "engine/audio/scene_types.h"
#include "engine/physics/backends.h"
#include "engine/render/environment.h"
#include "engine/render/renderer.h"
#include "engine/render/scene_types.h"
#include "engine/scene/physics_sync.h"

using engine::app::BakeHit;
using engine::app::BakeLight;
using engine::app::BakeTrace;
using engine::app::LightmapBake;
using engine::app::LightmapLayout;
using engine::app::LightmapRect;
using engine::app::LightmapSurface;
using engine::core::Color3;
using engine::core::DVec3;
using engine::core::f32;
using engine::core::f64;
using engine::core::u32;
using engine::core::usize;
using engine::core::Vec2;
using engine::core::Vec3;

namespace {

// A square of floor, `side` metres, facing up, its lightmap the whole square.
struct Floor
{
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices{0, 2, 1, 0, 3, 2};

    explicit Floor(f32 side)
    {
        const f32 half = side * 0.5f;
        positions = {{-half, 0.0f, -half}, {half, 0.0f, -half}, {half, 0.0f, half}, {-half, 0.0f, half}};
        normals.assign(4, Vec3{0.0f, 1.0f, 0.0f});
        uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    }

    [[nodiscard]] LightmapSurface surface(LightmapRect rect) const
    {
        LightmapSurface out;
        out.positions = positions;
        out.normals = normals;
        out.uvs = uvs;
        out.indices = indices;
        out.rects = {rect};
        return out;
    }
};

[[nodiscard]] BakeLight skyOf(Color3 sky)
{
    BakeLight light;
    light.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
    light.sunRadiance = Color3{0.0f, 0.0f, 0.0f};
    light.sky = [sky](Vec3) { return sky; };
    return light;
}

[[nodiscard]] bool nearly(f32 a, f32 b, f32 within = 1.0e-4f)
{
    return std::fabs(a - b) < within;
}

// The page's light at a texel.
[[nodiscard]] Color3 lightAt(const engine::app::LightmapPage& page, u32 x, u32 y)
{
    const f32* at = &page.light[(static_cast<usize>(y) * page.size + x) * 4];
    return Color3{at[0], at[1], at[2]};
}

} // namespace

TEST_CASE("every chart is given a rectangle of its own, as long each way as it is, and none overlaps another")
{
    const std::vector<Vec2> sizes{{1.0f, 1.0f}, {2.0f, 2.0f}, {0.5f, 0.5f}, {3.0f, 3.0f},
                                  {1.0f, 1.0f}, {0.1f, 0.1f}, {4.0f, 0.2f}};
    const std::vector<f32> scales{1.0f, 1.0f, 2.0f, 1.0f, 0.5f, 1.0f, 1.0f};
    const LightmapLayout layout = engine::app::layOutLightmaps(sizes, scales, 16.0f, 256, 4);
    REQUIRE(layout.rects.size() == sizes.size());
    CHECK_FALSE(layout.lowered);
    CHECK(layout.rects[1].width == 32);
    CHECK(layout.rects[2].width == 16);
    CHECK(layout.rects[4].width == 8);
    // The smallest a rectangle is.
    CHECK(layout.rects[5].width == 4);
    // A wall's edge: long one way, a few texels the other.
    CHECK(layout.rects[6].width == 64);
    CHECK(layout.rects[6].height == 4);
    for (usize a = 0; a < layout.rects.size(); ++a) {
        const LightmapRect& one = layout.rects[a];
        CHECK(one.x + one.width <= 256);
        CHECK(one.y + one.height <= 256);
        for (usize b = a + 1; b < layout.rects.size(); ++b) {
            const LightmapRect& two = layout.rects[b];
            if (one.page != two.page)
                continue;
            // Apart by the padding at least.
            const bool apart = one.x + one.width + engine::app::kLightmapPadding <= two.x ||
                               two.x + two.width + engine::app::kLightmapPadding <= one.x ||
                               one.y + one.height + engine::app::kLightmapPadding <= two.y ||
                               two.y + two.height + engine::app::kLightmapPadding <= one.y;
            CHECK(apart);
        }
    }
}

TEST_CASE("surfaces that ask for more than the pages hold are given less density, and the layout says so")
{
    const std::vector<Vec2> sizes(64, Vec2{4.0f, 4.0f});
    const std::vector<f32> scales(64, 1.0f);
    const LightmapLayout layout = engine::app::layOutLightmaps(sizes, scales, 16.0f, 256, 1);
    CHECK(layout.lowered);
    CHECK(layout.density < 16.0f);
    CHECK(layout.pages == 1);
}

TEST_CASE("a floor under an open sky is lit by the sky alone, and sees the whole sun")
{
    const Floor floor(2.0f);
    const LightmapSurface surface = floor.surface(LightmapRect{0, 4, 4, 32, 32});
    LightmapBake bake;
    bake.begin(std::span<const LightmapSurface>(&surface, 1), 64, 1, 1);
    const BakeTrace nothing = [](const DVec3&, Vec3, f32) { return BakeHit{}; };
    REQUIRE(bake.step(nothing, skyOf(Color3{0.5f, 0.25f, 1.0f}), 0));
    CHECK(bake.texelCount() == 32u * 32u);
    const auto& page = bake.pages()[0];
    // A sky the same every way gives a surface facing it that light, over
    // pi: the mean of what its rays see.
    for (const u32 y : {4u, 20u, 35u}) {
        for (const u32 x : {4u, 20u, 35u}) {
            const Color3 light = lightAt(page, x, y);
            CHECK(nearly(light.r, 0.5f));
            CHECK(nearly(light.g, 0.25f));
            CHECK(nearly(light.b, 1.0f));
            CHECK(page.mask[static_cast<usize>(y) * 64 + x] == 1.0f);
        }
    }
    // The gaps beyond the padding are no surface.
    CHECK(page.light[(0 * 64 + 0) * 4 + 3] == 0.0f);
    CHECK(page.light[(20 * 64 + 50) * 4 + 3] == 0.0f);
    // The padding is filled from the chart's edge.
    CHECK(page.light[(20 * 64 + 3) * 4 + 3] == 1.0f);
}

TEST_CASE("a roof over half the floor shades that half in the mask, and darkens its light")
{
    const Floor floor(2.0f);
    const LightmapSurface surface = floor.surface(LightmapRect{0, 0, 0, 32, 32});
    LightmapBake bake;
    bake.begin(std::span<const LightmapSurface>(&surface, 1), 32, 1, 0);
    // A black roof a metre up over x < 0, as far as the rays go.
    const BakeTrace roof = [](const DVec3& origin, Vec3 direction, f32 reach) {
        BakeHit met;
        if (direction.y <= 1.0e-6f)
            return met;
        const auto along = static_cast<f32>((1.0 - origin.y) / static_cast<double>(direction.y));
        if (along < 0.0f || along > reach)
            return met;
        const f32 x = static_cast<f32>(origin.x) + direction.x * along;
        if (x >= 0.0f)
            return met;
        met.hit = true;
        met.position =
            origin + engine::core::toDVec3(Vec3{direction.x * along, direction.y * along, direction.z * along});
        met.normal = Vec3{0.0f, -1.0f, 0.0f};
        met.albedo = Color3{0.0f, 0.0f, 0.0f};
        return met;
    };
    BakeLight light = skyOf(Color3{1.0f, 1.0f, 1.0f});
    light.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
    light.sunRadiance = Color3{3.0f, 3.0f, 3.0f};
    REQUIRE(bake.step(roof, light, 0));
    const auto& page = bake.pages()[0];
    // Well under the roof, and well clear of it.
    CHECK(page.mask[16 * 32 + 4] == 0.0f);
    CHECK(page.mask[16 * 32 + 28] == 1.0f);
    CHECK(lightAt(page, 4, 16).r < 0.6f);
    CHECK(lightAt(page, 28, 16).r > lightAt(page, 4, 16).r);
    CHECK(bake.sunDirection().y == 1.0f);
}

TEST_CASE("a bake in pieces under a budget comes out as the bake done whole")
{
    const Floor floor(2.0f);
    const LightmapSurface surface = floor.surface(LightmapRect{0, 0, 0, 64, 64});
    const BakeTrace slope = [](const DVec3& origin, Vec3 direction, f32) {
        BakeHit met;
        // Ground beyond x > 0.5 seen from a ray going that way, lit grey.
        if (direction.x > 0.3f && origin.x > 0.0) {
            met.hit = true;
            met.position = origin + engine::core::toDVec3(direction);
            met.normal = Vec3{-1.0f, 0.0f, 0.0f};
            met.albedo = Color3{0.5f, 0.5f, 0.5f};
            met.emissive = Color3{0.2f, 0.1f, 0.0f};
        }
        return met;
    };
    const BakeLight light = skyOf(Color3{0.3f, 0.3f, 0.3f});
    LightmapBake whole;
    whole.begin(std::span<const LightmapSurface>(&surface, 1), 64, 1, 2);
    REQUIRE(whole.step(slope, light, 0));
    LightmapBake pieces;
    pieces.begin(std::span<const LightmapSurface>(&surface, 1), 64, 1, 2);
    u32 steps = 0;
    while (!pieces.step(slope, light, 1)) {
        ++steps;
        CHECK(pieces.progress() <= 1.0f);
        REQUIRE(steps < 100000);
    }
    CHECK(steps > 0);
    CHECK(pieces.pages()[0].light == whole.pages()[0].light);
    CHECK(pieces.pages()[0].mask == whole.pages()[0].mask);
}

namespace {

// A renderer that draws nothing: a frame of the indirect light asks one for
// its settings.
class SettingsOnly final : public engine::render::IRenderer
{
public:
    std::optional<engine::core::EngineError> create(engine::rhi::IDevice&, const engine::render::ShaderLibrary&,
                                                    engine::rhi::TextureFormat) override
    {
        return std::nullopt;
    }
    void destroy(engine::rhi::IDevice&) override {}
    void render(engine::rhi::IDevice&, engine::rhi::ICmdList&, const engine::render::RenderTarget&,
                const engine::render::RenderWorld&, const engine::render::MeshCache&) override
    {}
    [[nodiscard]] bool valid() const noexcept override { return true; }
    [[nodiscard]] f32 shadowRadius() const noexcept override { return 0.0f; }
    void setSettings(const engine::render::GraphicsSettings& settings) override { m_settings = settings; }
    [[nodiscard]] const engine::render::GraphicsSettings& settings() const noexcept override { return m_settings; }
    [[nodiscard]] engine::render::RendererStats stats() const noexcept override { return {}; }

private:
    engine::render::GraphicsSettings m_settings;
};

// A floor of baked blocks under a noon sun, and what the bake of it says.
struct Scene
{
    engine::core::AtomTable atoms;
    engine::scene::ClassRegistry classes;
    engine::scene::EnumRegistry enums;
    std::optional<engine::scene::World> world;
    engine::physics::PhysicsResult backend;
    std::optional<engine::scene::PhysicsSync> physics;
    engine::core::InstanceId workspace;
    engine::render::MeshLibrary meshes;
    SettingsOnly renderer;
    engine::render::RenderWorld snapshot;
    engine::app::IndirectLight light;
    engine::core::u64 tick = 0;

    Scene()
    {
        using namespace engine;
        scene::generated::registerClasses(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        workspace = make("Folder", {});
        backend = physics::createJoltPhysics();
        REQUIRE(backend != nullptr);
        physics.emplace(*world, *backend);
        physics->setWorkspace(workspace);
        render::GraphicsSettings settings;
        settings.indirectLight = false;
        settings.reflectionProbes = 0;
        renderer.setSettings(settings);
        snapshot.camera.valid = true;
        snapshot.environment.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
        // The block as the mesh loader gives it, its lightmap with it.
        const asset::Mesh block = asset::makePrimitive(asset::PrimitiveShape::Block);
        render::MeshLibrary::Entry entry;
        // Uploaded, as far as a lookup can tell: nothing here draws it.
        entry.mesh = render::MeshHandle{0, 1};
        entry.bounds = block.bounds;
        for (const asset::Vertex& vertex : block.vertices) {
            entry.positions.push_back(vertex.position);
            entry.normals.push_back(vertex.normal);
        }
        for (const asset::LightmapUv& uv : block.lightmapUvs)
            entry.lightmapUvs.push_back(Vec2{uv.u, uv.v});
        entry.lightmapColumns = block.lightmapColumns;
        entry.lightmapRows = block.lightmapRows;
        entry.triangles = block.indices;
        entry.bounces.push_back(render::MeshLibrary::Entry::Bounce{
            .firstIndex = 0,
            .indexCount = static_cast<u32>(block.indices.size()),
            .albedo = Color3{1.0f, 1.0f, 1.0f},
            .emissive = Color3{0.0f, 0.0f, 0.0f},
            .clear = false,
        });
        meshes.set(atoms.intern(render::primitiveContent(static_cast<engine::core::i32>(asset::PrimitiveShape::Block))),
                   entry);
    }

    engine::core::InstanceId make(const char* className, engine::core::InstanceId parent)
    {
        const engine::scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != engine::scene::InvalidClass);
        const engine::core::InstanceId made = world->create(id);
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // An anchored block, asked of by its own triangles rather than by the
    // physics world, which this scene never steps.
    engine::core::InstanceId block(DVec3 at, Vec3 size, bool baked, bool anchored = true)
    {
        const engine::core::InstanceId id = make("Part", workspace);
        engine::scene::PartComponent& part = *world->parts().find(id);
        part.cframe.position = at;
        part.size = size;
        part.bakedLight = baked;
        if (engine::scene::RigidBodyComponent* body = world->rigidBodies().find(id); body != nullptr) {
            body->anchored = anchored;
            body->canQuery = false;
        }
        return id;
    }

    // One frame of the indirect light, baking or not.
    void step(bool baking)
    {
        engine::app::IndirectLight::Frame asked;
        asked.world = &*world;
        asked.physics = &*physics;
        asked.renderer = &renderer;
        asked.snapshot = &snapshot;
        asked.workspace = workspace;
        asked.meshes = &meshes;
        asked.tick = tick++;
        asked.bakingLightmaps = baking;
        light.update(asked);
    }

    // Frames until the bake is whole, and what it made.
    [[nodiscard]] engine::app::LightmapBakeResult bake()
    {
        engine::app::LightmapBakeResult out;
        for (int frame = 0; frame < 2000; ++frame) {
            step(true);
            if (light.takeLightmapBake(out))
                return out;
        }
        FAIL("the bake never finished");
        return out;
    }
};

// How many texels of one baked part's rectangles see the whole sun.
[[nodiscard]] usize sunlit(const engine::app::LightmapBakeResult& baked, engine::core::InstanceId id)
{
    usize count = 0;
    for (const engine::app::LightmapBakeResult::Part& part : baked.parts) {
        if (!(part.id == id))
            continue;
        for (const LightmapRect& rect : part.rects) {
            const engine::app::LightmapPage& page = baked.pages[rect.page];
            for (u32 y = rect.y; y < rect.y + rect.height; ++y) {
                for (u32 x = rect.x; x < rect.x + rect.width; ++x) {
                    const usize at = static_cast<usize>(y) * page.size + x;
                    if (page.light[at * 4 + 3] > 0.0f && page.mask[at] >= 1.0f)
                        ++count;
                }
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("the parts set to be baked are laid out and baked, and only a baked roof shades the floor")
{
    Scene open;
    const engine::core::InstanceId floor = open.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    // Not anchored, and not set: neither is baked.
    (void)open.block(DVec3{10.0, 0.0, 0.0}, Vec3{1.0f, 1.0f, 1.0f}, true, false);
    (void)open.block(DVec3{-10.0, 0.0, 0.0}, Vec3{1.0f, 1.0f, 1.0f}, false);
    // A mesh with no lightmap coordinates is said, and not baked.
    const engine::core::InstanceId mesh = open.make("MeshPart", open.workspace);
    open.world->parts().find(mesh)->bakedLight = true;
    if (engine::scene::RigidBodyComponent* body = open.world->rigidBodies().find(mesh); body != nullptr)
        body->anchored = true;
    const engine::app::LightmapBakeResult whole = open.bake();
    REQUIRE(whole.parts.size() == 1);
    CHECK(whole.parts[0].id == floor);
    // A block's six faces, each its own rectangle: the top four metres by
    // four, the edges four by a fifth.
    REQUIRE(whole.parts[0].rects.size() == 6);
    u32 broad = 0;
    for (const LightmapRect& rect : whole.parts[0].rects)
        broad += rect.width >= 55 && rect.height >= 55 ? 1u : 0u;
    CHECK(broad == 2);
    REQUIRE(whole.unmapped.size() == 1);
    CHECK(whole.unmapped[0] == mesh);
    REQUIRE(whole.pages.size() == 1);
    CHECK(whole.texels > 0);
    CHECK(whole.sunDirection.y > 0.99f);
    // The floor's top, four metres by four at sixteen texels a metre.
    const usize lit = sunlit(whole, floor);
    CHECK(lit > 3000);

    // **What is not baked casts nothing the bake keeps**: a roof over half
    // the floor that is not set to be baked leaves the floor's mask whole.
    Scene unbakedRoof;
    const engine::core::InstanceId floorTwo = unbakedRoof.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    (void)unbakedRoof.block(DVec3{-1.0, 1.5, 0.0}, Vec3{2.0f, 0.2f, 6.0f}, false);
    CHECK(sunlit(unbakedRoof.bake(), floorTwo) == lit);

    // Baked, it shades the half of the floor it is over, a metre up.
    Scene shaded;
    const engine::core::InstanceId floorThree = shaded.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    (void)shaded.block(DVec3{-1.0, 1.5, 0.0}, Vec3{2.0f, 0.2f, 6.0f}, true);
    const usize under = sunlit(shaded.bake(), floorThree);
    CHECK(under < lit * 3 / 4);
    CHECK(under > lit / 4);
}

// **A frame draws a baked part with its light** (ADR 0253): its draws are
// given its rows, every other draw none; the mask holds while the sun is
// within three degrees of where it was baked; and a part that moves is drawn
// as a moving one from then on.
TEST_CASE("a baked part is drawn with its light until it moves, and with its mask until the sun turns")
{
    Scene scene;
    const engine::core::InstanceId floor = scene.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    const engine::core::InstanceId crate = scene.block(DVec3{0.0, 1.0, 0.0}, Vec3{1.0f, 1.0f, 1.0f}, false);
    const engine::app::LightmapBakeResult baked = scene.bake();
    REQUIRE(baked.parts.size() == 1);
    scene.light.giveLightmap(baked);

    const auto keyOf = [](engine::core::InstanceId id) {
        return (static_cast<engine::core::u64>(id.generation) << 32) | static_cast<engine::core::u64>(id.index);
    };
    const auto drawn = [&]() {
        scene.step(false);
        engine::render::RenderWorld out;
        out.camera.valid = true;
        engine::render::DrawItem floorDraw;
        floorDraw.motionKey = keyOf(floor);
        engine::render::DrawItem crateDraw;
        crateDraw.motionKey = keyOf(crate);
        out.draws = {floorDraw, crateDraw};
        scene.light.append(out);
        return out;
    };

    const engine::render::RenderWorld lit = drawn();
    CHECK(lit.lightmap.stamp != 0);
    REQUIRE(lit.lightmap.pages != nullptr);
    CHECK(lit.lightmap.pages->count == 1);
    CHECK(lit.lightmap.maskHolds);
    // Its grid -- a block's three by two -- and a rectangle a face.
    REQUIRE(lit.lightmap.rects.size() == 4u * 7u);
    CHECK(lit.lightmap.rects[0] == 3.0f);
    CHECK(lit.lightmap.rects[1] == 2.0f);
    CHECK(lit.draws[0].lightmap == 0u);
    CHECK(lit.draws[1].lightmap == engine::render::NoLightmap);

    // Ten degrees off: the mask is set aside, the light kept.
    const f32 turn = 10.0f * 3.14159265f / 180.0f;
    scene.snapshot.environment.sunDirection = Vec3{std::sin(turn), std::cos(turn), 0.0f};
    const engine::render::RenderWorld turned = drawn();
    CHECK_FALSE(turned.lightmap.maskHolds);
    CHECK(turned.draws[0].lightmap == 0u);

    // Moved: lit as a moving part from then on.
    scene.world->parts().find(floor)->cframe.position = DVec3{0.0, 0.5, 0.0};
    const engine::render::RenderWorld moved = drawn();
    CHECK(moved.draws[0].lightmap == engine::render::NoLightmap);
}

// **The grid what moves is shadowed by** (ADR 0253 §4): its points see the
// sun where nothing baked stands between, and none of it under a baked
// roof; and a frame drawn with the bake carries it while the mask holds.
TEST_CASE("the bake's grid of the sun is dark under a baked roof and lit beside it, and a frame carries it")
{
    Scene scene;
    (void)scene.block(DVec3{0.0, 0.0, 0.0}, Vec3{6.0f, 0.2f, 6.0f}, true);
    (void)scene.block(DVec3{-1.5, 2.0, 0.0}, Vec3{3.0f, 0.2f, 6.0f}, true);
    const engine::app::LightmapBakeResult baked = scene.bake();
    const engine::app::LightmapSunGrid& grid = baked.sunGrid;
    REQUIRE(grid.counts[0] >= 2);
    REQUIRE(grid.sun.size() == static_cast<usize>(grid.counts[0]) * grid.counts[1] * grid.counts[2]);
    // The point nearest a place a metre up: under the roof, and in the open.
    const auto at = [&](f64 x, f64 y, f64 z) {
        const auto index = [&](f64 value, f64 low, u32 count) {
            const auto step = static_cast<f64>(grid.spacing);
            return std::min<u32>(count - 1, static_cast<u32>(std::lround(std::max(0.0, (value - low) / step))));
        };
        const u32 ix = index(x, grid.low.x, grid.counts[0]);
        const u32 iy = index(y, grid.low.y, grid.counts[1]);
        const u32 iz = index(z, grid.low.z, grid.counts[2]);
        return grid.sun[(static_cast<usize>(iz) * grid.counts[1] + iy) * grid.counts[0] + ix];
    };
    CHECK(at(-1.5, 1.0, 0.0) < 0.5f);
    CHECK(at(2.0, 1.0, 0.0) > 0.5f);

    scene.light.giveLightmap(baked);
    scene.step(false);
    engine::render::RenderWorld out;
    out.camera.valid = true;
    scene.light.append(out);
    REQUIRE(out.lightmap.sun != nullptr);
    CHECK(out.lightmap.sun->size() == grid.sun.size());
    CHECK(out.lightmap.sunCounts[0] == grid.counts[0]);
    CHECK(out.lightmap.sunScale > 0.0f);
}

// **A bake kept in a file** (ADR 0253 §6): read back as written, and found
// again in the scene by each part's names and where it stood -- a part moved
// since is not found, and is said.
TEST_CASE("a bake written to a file reads back, and finds its parts again only where they stood")
{
    Scene scene;
    const engine::core::InstanceId floor = scene.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    const engine::core::InstanceId wall = scene.block(DVec3{0.0, 1.0, -2.0}, Vec3{4.0f, 2.0f, 0.2f}, true);
    scene.world->setName(floor, scene.atoms.intern("Floor"));
    scene.world->setName(wall, scene.atoms.intern("Wall"));
    const engine::app::LightmapBakeResult baked = scene.bake();
    REQUIRE(baked.parts.size() == 2);

    const engine::app::KeptLightmap kept = engine::app::keptLightmapOf(*scene.world, baked);
    const std::vector<std::byte> bytes = engine::app::writeLightmap(kept);
    engine::app::KeptLightmap read;
    REQUIRE(engine::app::readLightmap(bytes, read));
    CHECK(read.pageSize == kept.pageSize);
    CHECK(read.pageCount == kept.pageCount);
    CHECK(read.texels == kept.texels);
    REQUIRE(read.parts.size() == 2);
    CHECK(read.parts[0].path == kept.parts[0].path);
    CHECK(read.sunGrid.sun.size() == kept.sunGrid.sun.size());
    // Cut short, or another kind of file: refused.
    CHECK_FALSE(engine::app::readLightmap(std::span(bytes).first(bytes.size() - 1), read));
    CHECK_FALSE(engine::app::readLightmap(std::span(bytes).subspan(1), read));

    engine::core::u32 missing = 0;
    const engine::app::LightmapBakeResult found = engine::app::lightmapFromKept(*scene.world, kept, missing);
    CHECK(missing == 0);
    CHECK(found.parts.size() == 2);
    CHECK(found.pages.size() == baked.pages.size());

    // The wall moved: the floor is found, the wall is not.
    scene.world->parts().find(wall)->cframe.position = DVec3{0.0, 1.0, -3.0};
    const engine::app::LightmapBakeResult partly = engine::app::lightmapFromKept(*scene.world, kept, missing);
    CHECK(missing == 1);
    REQUIRE(partly.parts.size() == 1);
    CHECK(partly.parts[0].id == floor);
}

TEST_CASE("a bake kept as UASTC HDR is a fraction of the file, reads back as kept, and is near the light it was")
{
    Scene scene;
    const engine::core::InstanceId floor = scene.block(DVec3{0.0, 0.0, 0.0}, Vec3{4.0f, 0.2f, 4.0f}, true);
    const engine::core::InstanceId wall = scene.block(DVec3{0.0, 1.0, -2.0}, Vec3{4.0f, 2.0f, 0.2f}, true);
    scene.world->setName(floor, scene.atoms.intern("Floor"));
    scene.world->setName(wall, scene.atoms.intern("Wall"));
    const engine::app::LightmapBakeResult baked = scene.bake();
    REQUIRE(baked.parts.size() == 2);
    engine::app::KeptLightmap kept = engine::app::keptLightmapOf(*scene.world, baked);
    const std::vector<std::byte> halves = engine::app::writeLightmap(kept);

    // A file of the first version -- half floats, and no word for how --
    // still reads.
    {
        std::vector<std::byte> first(halves);
        const engine::core::usize words = kept.texels.size() * sizeof(engine::core::u16);
        first.erase(first.end() - static_cast<std::ptrdiff_t>(words) - 4,
                    first.end() - static_cast<std::ptrdiff_t>(words));
        const engine::core::u32 version = 1;
        std::memcpy(first.data() + 8, &version, sizeof(version));
        engine::app::KeptLightmap read;
        REQUIRE(engine::app::readLightmap(first, read));
        CHECK(read.texels == kept.texels);
    }

#if ENG_DEBUG_UI
    const std::vector<engine::core::u16> before = kept.texels;
    REQUIRE(engine::app::compressLightmap(kept));
    CHECK(kept.texels.empty());
    REQUIRE(kept.light.size() == kept.pageCount);
    REQUIRE(kept.masks.size() == kept.pageCount);
    const std::vector<std::byte> bytes = engine::app::writeLightmap(kept);
    CHECK(bytes.size() * 4 < halves.size());
    engine::app::KeptLightmap read;
    REQUIRE(engine::app::readLightmap(bytes, read));
    CHECK(read.light == kept.light);
    CHECK(read.masks == kept.masks);
    CHECK(read.texels.empty());
    CHECK_FALSE(engine::app::readLightmap(std::span(bytes).first(bytes.size() - 1), read));

    // Near the light and the mask it was, wherever the bake lit.
    const engine::core::usize texels = static_cast<engine::core::usize>(kept.pageSize) * kept.pageSize;
    engine::core::u32 width = 0;
    engine::core::u32 height = 0;
    std::vector<std::byte> light;
    std::vector<std::byte> mask;
    REQUIRE(engine::asset::transcodeHdrPicture(kept.light[0], engine::asset::HdrBlocks::Half, width, height, light));
    REQUIRE(engine::asset::transcodeHdrPicture(kept.masks[0], engine::asset::HdrBlocks::Half, width, height, mask));
    CHECK(width == kept.pageSize);
    double lightError = 0.0;
    double maskError = 0.0;
    engine::core::usize lit = 0;
    for (engine::core::usize texel = 0; texel < texels; ++texel) {
        const auto half = [](const std::vector<std::byte>& from, engine::core::usize at) {
            engine::core::u16 value = 0;
            std::memcpy(&value, from.data() + at * 2, 2);
            return engine::render::halfToFloat(value);
        };
        const f32 was = engine::render::halfToFloat(before[texel * 4]);
        if (was <= 0.0f)
            continue;
        ++lit;
        lightError += static_cast<double>(std::abs(half(light, texel * 4) - was) / (was + 0.05f));
        maskError +=
            static_cast<double>(std::abs(half(mask, texel * 4) - engine::render::halfToFloat(before[texel * 4 + 3])));
    }
    REQUIRE(lit > 500);
    CHECK(lightError / static_cast<double>(lit) < 0.03);
    CHECK(maskError / static_cast<double>(lit) < 0.02);

    // Handed on as kept: transcoded only where it is uploaded.
    engine::core::u32 missing = 0;
    const engine::app::LightmapBakeResult found = engine::app::lightmapFromKept(*scene.world, read, missing);
    CHECK(missing == 0);
    CHECK(found.pages.empty());
    CHECK(found.encodedSize == kept.pageSize);
    CHECK(found.encodedLight.size() == kept.pageCount);
#else
    // A game makes no bake, and has no encoder to keep one with.
    CHECK_FALSE(engine::app::compressLightmap(kept));
    CHECK_FALSE(kept.texels.empty());
#endif
}
