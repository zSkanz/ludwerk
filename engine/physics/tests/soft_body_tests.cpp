// A cloth (ADR 0214): a sheet of vertices hung from its top row, in a world of
// its own. What is asked of it is what a cape needs -- it hangs from what pins
// it, at the length it was made, follows the joint that carries it, is pushed
// by the wind, and stays out of what it rests on -- and what the renderer
// needs: its vertices back, in the order they were given.

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

#include "engine/physics/backends.h"

using namespace engine;
using namespace engine::physics;

namespace {

constexpr f32 kFrame = 1.0f / 60.0f;

// A sheet a metre wide and `height` long, hanging down from y = 0 in the
// plane z = 0: `columns` by `rows` vertices, the top row pinned.
struct Sheet
{
    static constexpr u32 Columns = 9;
    static constexpr u32 Rows = 13;
    static constexpr f32 Width = 1.0f;
    static constexpr f32 Height = 1.2f;

    explicit Sheet(core::DVec3 at = core::DVec3{0.0, 3.0, 0.0}, f32 reach = 2.0f)
    {
        physics = createJoltPhysics();
        REQUIRE(physics != nullptr);
        world = physics->createWorld(WorldDesc{});
        REQUIRE(world.valid());
        where = at;

        SoftBodyDesc desc;
        desc.position = at;
        for (u32 row = 0; row < Rows; ++row) {
            for (u32 column = 0; column < Columns; ++column) {
                desc.vertices.push_back(core::Vec3{Width * (static_cast<f32>(column) / (Columns - 1) - 0.5f),
                                                   -Height * static_cast<f32>(row) / (Rows - 1), 0.0f});
                desc.maxDistance.push_back(row == 0 ? 0.0f : reach);
            }
        }
        for (u32 row = 0; row + 1 < Rows; ++row) {
            for (u32 column = 0; column + 1 < Columns; ++column) {
                const u32 a = row * Columns + column;
                desc.triangles.insert(desc.triangles.end(), {a, a + Columns, a + 1});
                desc.triangles.insert(desc.triangles.end(), {a + 1, a + Columns, a + Columns + 1});
            }
        }
        cloth = physics->createSoftBody(world, desc);
        REQUIRE(cloth.valid());
        carry(at, true);
    }

    // The joint the whole sheet follows, at `at`.
    void carry(core::DVec3 at, bool snap = false)
    {
        const core::Mat4 joint = core::toRenderMatrix(core::CFrameD{at, core::Mat3{}}, core::DVec3{});
        physics->skinSoftBody(world, cloth, core::DVec3{}, std::span<const core::Mat4>(&joint, 1), snap);
        where = at;
    }

    void frames(u32 count, core::DVec3 velocity = core::DVec3{})
    {
        for (u32 frame = 0; frame < count; ++frame) {
            if (!(velocity == core::DVec3{})) {
                const f64 step = static_cast<f64>(kFrame);
                carry(
                    core::DVec3{where.x + velocity.x * step, where.y + velocity.y * step, where.z + velocity.z * step});
            }
            physics->step(world, kFrame);
        }
    }

    [[nodiscard]] std::vector<core::Vec3> vertices() const
    {
        std::vector<core::Vec3> out;
        physics->softBodyVertices(world, cloth, core::DVec3{}, out);
        return out;
    }

    std::unique_ptr<IPhysics3D> physics;
    WorldHandle world;
    SoftBodyHandle cloth;
    core::DVec3 where;
};

[[nodiscard]] f32 lowest(const std::vector<core::Vec3>& vertices)
{
    f32 y = 1.0e9f;
    for (const core::Vec3& vertex : vertices)
        y = std::min(y, vertex.y);
    return y;
}

} // namespace

TEST_CASE("a cloth hangs from what pins it, at the length it was made")
{
    Sheet sheet;
    // Held out level first, then let fall: snapped flat, sideways.
    sheet.frames(240);
    const std::vector<core::Vec3> hung = sheet.vertices();
    REQUIRE(hung.size() == Sheet::Columns * Sheet::Rows);
    // The top row where its joint holds it.
    for (u32 column = 0; column < Sheet::Columns; ++column) {
        CHECK(static_cast<double>(hung[column].y) == doctest::Approx(3.0).epsilon(1e-3));
        CHECK(static_cast<double>(hung[column].x) ==
              doctest::Approx(static_cast<double>(Sheet::Width * (static_cast<f32>(column) / 8.0f - 0.5f)))
                  .epsilon(1e-3));
    }
    // Hanging straight down, and no longer than it was made: the hem a
    // little above where it would be if every link stretched.
    const f32 hem = lowest(hung);
    CHECK(hem < 3.0f - Sheet::Height * 0.95f);
    CHECK(hem > 3.0f - Sheet::Height * 1.03f);
}

TEST_CASE("a cloth follows its joint, trails behind it through the air, and settles where it went")
{
    Sheet sheet;
    // Still air: what a moving cloth pushes through. With none, a cloth
    // carried at a steady speed hangs straight, as it would in a vacuum.
    physics::SoftBodyWind air;
    air.drag = 1.2f;
    sheet.physics->blowSoftBody(sheet.world, sheet.cloth, air);
    sheet.frames(120);
    // Carried through its face at four metres a second for a second, as a
    // cape is by a runner: the hem trails behind the top. Carried edge-on,
    // it would barely trail, as cloth does not.
    sheet.frames(60, core::DVec3{0.0, 0.0, -4.0});
    std::vector<core::Vec3> moving = sheet.vertices();
    const core::Vec3 top = moving[Sheet::Columns / 2];
    const core::Vec3 hem = moving[(Sheet::Rows - 1) * Sheet::Columns + Sheet::Columns / 2];
    CHECK(static_cast<double>(top.z) == doctest::Approx(-4.0).epsilon(0.02));
    CHECK(hem.z > top.z + 0.1f);
    // Stopped, it settles under the joint again.
    sheet.frames(300);
    const std::vector<core::Vec3> settled = sheet.vertices();
    const core::Vec3 still = settled[(Sheet::Rows - 1) * Sheet::Columns + Sheet::Columns / 2];
    CHECK(std::abs(still.z + 4.0f) < 0.05f);
}

TEST_CASE("a wind across a cloth pushes it, and none leaves it hanging")
{
    Sheet calm;
    calm.frames(240);
    Sheet blown;
    // Eight metres a second along +Z, through the face of the sheet.
    physics::SoftBodyWind wind;
    wind.velocity = core::Vec3{0.0f, 0.0f, 8.0f};
    wind.drag = 1.2f;
    blown.physics->blowSoftBody(blown.world, blown.cloth, wind);
    blown.frames(240);
    const core::Vec3 still = calm.vertices().back();
    const core::Vec3 pushed = blown.vertices().back();
    CHECK(std::abs(still.z) < 0.02f);
    CHECK(pushed.z > 0.2f);
}

TEST_CASE("a cloth rests on a capsule and does not pass through it")
{
    Sheet sheet;
    // A capsule just behind the sheet's lower half, as a body is behind a
    // cape, and a wind pressing the cloth onto it.
    BodyDesc back;
    back.shape.type = ShapeType::Capsule;
    // Thirty centimetres across and ninety tall, its round ends included.
    back.shape.size = core::Vec3{0.3f, 0.9f, 0.3f};
    back.transform.position = core::DVec3{0.0, 2.3, 0.2};
    back.motion = MotionType::Kinematic;
    REQUIRE(sheet.physics->createBody(sheet.world, back).valid());
    physics::SoftBodyWind wind;
    wind.velocity = core::Vec3{0.0f, 0.0f, 12.0f};
    wind.drag = 1.2f;
    sheet.physics->blowSoftBody(sheet.world, sheet.cloth, wind);
    sheet.frames(300);
    // No vertex inside the capsule, and the cloth bent round it: something
    // of it beyond the capsule's far side, so it was pressed and held.
    u32 inside = 0;
    for (const core::Vec3& vertex : sheet.vertices()) {
        const f32 y = std::clamp(vertex.y, 2.0f, 2.6f);
        const f32 dy = vertex.y - y;
        const f32 dz = vertex.z - 0.2f;
        if (std::sqrt(vertex.x * vertex.x + dy * dy + dz * dz) < 0.15f - 1.0e-3f)
            ++inside;
    }
    CHECK(inside == 0);
}

TEST_CASE("a cloth put somewhere else is there at once, and still")
{
    Sheet sheet;
    sheet.frames(120);
    sheet.carry(core::DVec3{50.0, 3.0, -20.0}, true);
    const std::vector<core::Vec3> moved = sheet.vertices();
    CHECK(static_cast<double>(moved[0].x) == doctest::Approx(49.5).epsilon(1e-3));
    sheet.frames(1);
    const std::vector<core::Vec3> after = sheet.vertices();
    // A frame later it has not been flung by the jump.
    for (core::usize at = 0; at < moved.size(); ++at)
        CHECK(std::abs(after[at].x - moved[at].x) < 0.05f);
}
