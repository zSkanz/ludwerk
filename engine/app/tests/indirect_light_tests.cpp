// Light that bounces (ADR 0203, `IrradianceVolume`): what a volume's points
// hold after its rays have been cast and lit, against scenes simple enough to
// say the answer of -- an open sky, a lit floor, a room that glows.
//
// The rays here meet shapes written out by hand, not the physics world: what
// is under test is what is made of what a ray met.

#include <atomic>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <limits>
#include <vector>

#include "engine/app/indirect_light.h"

namespace {

using namespace engine;
using app::BakeHit;
using app::BakeLight;
using app::BakeTrace;
using app::VolumeBake;
using app::VolumeLayout;
using core::Color3;
using core::DVec3;
using core::f32;
using core::Vec3;

constexpr f32 kPi = 3.14159265358979f;

// The light on a surface facing `normal` at one point, over pi: what the
// shader makes of a point's twelve numbers.
[[nodiscard]] Color3 facing(std::span<const f32> light, std::size_t sample, Vec3 normal)
{
    const f32* at = &light[sample * 12];
    Color3 out;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        (&out.r)[channel] = std::max(
            at[channel] + at[3 + channel] * normal.x + at[6 + channel] * normal.y + at[9 + channel] * normal.z, 0.0f);
    }
    return out;
}

[[nodiscard]] BakeLight whiteSky()
{
    BakeLight light;
    light.sky = [](Vec3) { return Color3{1.0f, 1.0f, 1.0f}; };
    return light;
}

// A floor at y = 0, under which everything is solid.
[[nodiscard]] BakeTrace floorOf(Color3 albedo, std::atomic<int>* asked = nullptr)
{
    return [albedo, asked](const DVec3& origin, Vec3 direction, f32) {
        if (asked != nullptr)
            asked->fetch_add(1);
        BakeHit met;
        if (origin.y < 0.0) {
            met.hit = true;
            met.inside = true;
            return met;
        }
        if (direction.y >= -1.0e-6f)
            return met;
        const double along = -origin.y / static_cast<double>(direction.y);
        met.hit = true;
        met.position = DVec3{origin.x + along * static_cast<double>(direction.x), 0.0,
                             origin.z + along * static_cast<double>(direction.z)};
        met.normal = Vec3{0.0f, 1.0f, 0.0f};
        met.albedo = albedo;
        return met;
    };
}

} // namespace

TEST_CASE("ADR 0203: a box is sampled at its spacing, and more coarsely when that is more than a volume holds")
{
    const VolumeLayout room = app::volumeLayoutOf(Vec3{8.0f, 4.0f, 6.0f}, 2.0f);
    CHECK(room.counts[0] == 4);
    CHECK(room.counts[1] == 2);
    CHECK(room.counts[2] == 3);
    CHECK_FALSE(room.coarser);
    CHECK(room.spacing == doctest::Approx(2.0));

    // A hall at half a metre would be eight million points.
    const VolumeLayout hall = app::volumeLayoutOf(Vec3{100.0f, 100.0f, 100.0f}, 0.5f);
    CHECK(hall.coarser);
    CHECK(hall.spacing > 0.5f);
    for (const core::u32 count : hall.counts)
        CHECK(count <= render::kIrradianceVolumeSide);
    CHECK(hall.counts[0] * hall.counts[1] * hall.counts[2] <= render::kIrradianceVolumeSamples);

    // And a thin one is still one point deep, not none.
    const VolumeLayout slab = app::volumeLayoutOf(Vec3{4.0f, 0.2f, 4.0f}, 2.0f);
    CHECK(slab.counts[1] == 1);
}

TEST_CASE("ADR 0203: the rays of a point look every way, and none twice")
{
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (core::u32 index = 0; index < app::kBakeRays; ++index) {
        const Vec3 ray = app::bakeDirection(index, app::kBakeRays);
        CHECK(core::dot(ray, ray) == doctest::Approx(1.0).epsilon(1.0e-4));
        sum = sum + ray;
        for (core::u32 other = 0; other < index; ++other)
            CHECK(core::dot(ray, app::bakeDirection(other, app::kBakeRays)) < 0.999f);
    }
    // Evenly: they lean nowhere.
    CHECK(std::abs(sum.x) < 1.0f);
    CHECK(std::abs(sum.y) < 1.0f);
    CHECK(std::abs(sum.z) < 1.0f);
}

TEST_CASE("ADR 0203: a point under an open sky holds the sky, the same on every side")
{
    VolumeBake bake;
    bake.begin(core::CFrameD{}, Vec3{4.0f, 4.0f, 4.0f}, app::volumeLayoutOf(Vec3{4.0f, 4.0f, 4.0f}, 2.0f), 1);
    const BakeTrace nothing = [](const DVec3&, Vec3, f32) { return BakeHit{}; };
    REQUIRE(bake.step(nothing, whiteSky(), 0));
    REQUIRE(bake.result().size() == 8 * 12);
    for (std::size_t sample = 0; sample < 8; ++sample) {
        for (const Vec3 normal : {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}) {
            const Color3 light = facing(bake.result(), sample, normal);
            CHECK(light.r == doctest::Approx(1.0).epsilon(0.05));
            CHECK(light.b == doctest::Approx(1.0).epsilon(0.05));
        }
    }
    CHECK(bake.dropped() == 0);
}

TEST_CASE("ADR 0203: a floor the sun is on lights what faces it, in the floor's colour, and nothing that faces away")
{
    // A black sky, so all the light there is came off the floor. The sun
    // straight down at pi: the floor is lit by exactly one, and gives back
    // its own colour.
    BakeLight light;
    light.sky = [](Vec3) { return Color3{0.0f, 0.0f, 0.0f}; };
    light.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
    light.sunRadiance = Color3{kPi, kPi, kPi};
    const Color3 red{0.8f, 0.1f, 0.1f};

    VolumeBake bake;
    const core::CFrameD above{.position = DVec3{0.0, 2.0, 0.0}, .rotation = core::Mat3{}};
    bake.begin(above, Vec3{2.0f, 2.0f, 2.0f}, app::volumeLayoutOf(Vec3{2.0f, 2.0f, 2.0f}, 2.0f), 1);
    REQUIRE(bake.step(floorOf(red), light, 0));
    REQUIRE(bake.result().size() == 12);

    // Facing down, the whole of what it sees is the floor.
    const Color3 down = facing(bake.result(), 0, Vec3{0.0f, -1.0f, 0.0f});
    CHECK(down.r == doctest::Approx(0.8).epsilon(0.08));
    CHECK(down.g == doctest::Approx(0.1).epsilon(0.08));
    // Facing up, none of it.
    const Color3 up = facing(bake.result(), 0, Vec3{0.0f, 1.0f, 0.0f});
    CHECK(up.r < 0.05f);
    // And sideways, half.
    const Color3 side = facing(bake.result(), 0, Vec3{1.0f, 0.0f, 0.0f});
    CHECK(side.r == doctest::Approx(0.4).epsilon(0.1));
}

TEST_CASE("ADR 0203: a point inside something is dropped, and holds its neighbour's light")
{
    BakeLight light = whiteSky();
    // Two points high, the lower one under the floor.
    VolumeBake bake;
    bake.begin(core::CFrameD{}, Vec3{2.0f, 4.0f, 2.0f}, app::volumeLayoutOf(Vec3{2.0f, 4.0f, 2.0f}, 2.0f), 1);
    REQUIRE(bake.layout().counts[1] == 2);
    REQUIRE(bake.step(floorOf(Color3{0.0f, 0.0f, 0.0f}), light, 0));
    CHECK(bake.dropped() == 1);
    // The one above sees the sky over half of everything; the one below was
    // given the same, not the nought it would have worked out.
    const std::span<const f32> result = bake.result();
    CHECK(result[12] == doctest::Approx(0.5).epsilon(0.1));
    for (std::size_t value = 0; value < 12; ++value)
        CHECK(result[value] == doctest::Approx(static_cast<double>(result[12 + value])));
}

TEST_CASE("ADR 0203: each bounce lets the light round a point light it once more")
{
    // A room that glows by one all over, half of what lands on its walls
    // given back: what a point sees is one, then one and a half, then one
    // and three quarters.
    const BakeTrace room = [](const DVec3& origin, Vec3 direction, f32) {
        BakeHit met;
        met.hit = true;
        met.position = DVec3{origin.x + static_cast<double>(direction.x), origin.y + static_cast<double>(direction.y),
                             origin.z + static_cast<double>(direction.z)};
        met.normal = Vec3{-direction.x, -direction.y, -direction.z};
        met.albedo = Color3{0.5f, 0.5f, 0.5f};
        met.emissive = Color3{1.0f, 1.0f, 1.0f};
        return met;
    };
    BakeLight dark;
    dark.sky = [](Vec3) { return Color3{0.0f, 0.0f, 0.0f}; };
    const double expected[3] = {1.5, 1.75, 1.875};
    for (core::u32 bounces = 1; bounces <= 3; ++bounces) {
        VolumeBake bake;
        bake.begin(core::CFrameD{}, Vec3{2.0f, 2.0f, 2.0f}, app::volumeLayoutOf(Vec3{2.0f, 2.0f, 2.0f}, 2.0f), bounces);
        REQUIRE(bake.step(room, dark, 0));
        CHECK(bake.result()[0] == doctest::Approx(expected[bounces - 1]).epsilon(0.02));
    }
}

TEST_CASE("ADR 0203: a volume lit again under another sun casts no ray it has cast, and is the other sun's")
{
    BakeLight light;
    light.sky = [](Vec3) { return Color3{0.0f, 0.0f, 0.0f}; };
    light.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
    light.sunRadiance = Color3{kPi, kPi, kPi};
    std::atomic<int> asked{0};
    const BakeTrace floor = floorOf(Color3{1.0f, 1.0f, 1.0f}, &asked);

    VolumeBake bake;
    const core::CFrameD above{.position = DVec3{0.0, 2.0, 0.0}, .rotation = core::Mat3{}};
    bake.begin(above, Vec3{2.0f, 2.0f, 2.0f}, app::volumeLayoutOf(Vec3{2.0f, 2.0f, 2.0f}, 2.0f), 1);
    REQUIRE(bake.step(floor, light, 0));
    const f32 noon = facing(bake.result(), 0, Vec3{0.0f, -1.0f, 0.0f}).r;
    const int first = asked.load();
    // Every ray, and one more towards the sun from each that met the floor.
    CHECK(first > static_cast<int>(app::kBakeRays));

    light.sunRadiance = Color3{kPi * 0.25f, kPi * 0.25f, kPi * 0.25f};
    asked.store(0);
    bake.relight();
    CHECK_FALSE(bake.whole());
    REQUIRE(bake.step(floor, light, 0));
    // Only the rays towards the sun were cast again.
    CHECK(asked.load() == first - static_cast<int>(app::kBakeRays));
    CHECK(facing(bake.result(), 0, Vec3{0.0f, -1.0f, 0.0f}).r ==
          doctest::Approx(static_cast<double>(noon) * 0.25).epsilon(0.01));

    // A volume that will not be lit again gives its rays back, and `relight`
    // then has nothing to light.
    bake.forgetRays();
    bake.relight();
    CHECK(bake.whole());
}

TEST_CASE("ADR 0203: a volume worked out a sliver a frame is the volume worked out at once")
{
    BakeLight light = whiteSky();
    light.sunDirection = core::normalize(Vec3{0.3f, 1.0f, 0.2f});
    light.sunRadiance = Color3{2.0f, 1.8f, 1.5f};
    const Vec3 size{8.0f, 4.0f, 8.0f};
    const core::CFrameD above{.position = DVec3{0.0, 2.0, 0.0}, .rotation = core::Mat3{}};

    VolumeBake whole;
    whole.begin(above, size, app::volumeLayoutOf(size, 1.0f), 2);
    REQUIRE(whole.step(floorOf(Color3{0.6f, 0.5f, 0.4f}), light, 0));

    VolumeBake sliced;
    sliced.begin(above, size, app::volumeLayoutOf(size, 1.0f), 2);
    int frames = 0;
    f32 before = 0.0f;
    while (!sliced.step(floorOf(Color3{0.6f, 0.5f, 0.4f}), light, 1)) {
        // And it says how far it has got, never going back.
        CHECK(sliced.progress() >= before);
        CHECK(sliced.progress() < 1.0f);
        before = sliced.progress();
        REQUIRE(++frames < 100000);
    }
    CHECK(frames > 1);
    CHECK(sliced.progress() == 1.0f);
    REQUIRE(sliced.result().size() == whole.result().size());
    for (std::size_t value = 0; value < whole.result().size(); ++value)
        CHECK(sliced.result()[value] == whole.result()[value]);
}

TEST_CASE("ADR 0203: a closed room gives back no more light than its lamp put on its walls (white furnace)")
{
    // A round room five metres in radius, every wall the same grey, one lamp
    // at its middle and no sky to be seen: the lamp puts the same light `d`
    // on every point of the wall, squarely -- behind itself as before itself,
    // which is what happened: a lamp that shines every way had a dark cone
    // behind it, and this room came out a twentieth short and leaning. What the walls give back is then
    // known exactly -- `d` times the albedo for the first bounce, times the
    // albedo again for each one after -- and whatever the bounces, less than
    // `d * albedo / (1 - albedo)`, which is all the light there is.
    const double radius = 5.0;
    const auto roomOf = [radius](f32 albedo) -> BakeTrace {
        return [radius, albedo](const DVec3& origin, Vec3 direction, f32 reach) {
            BakeHit met;
            const double along = origin.x * static_cast<double>(direction.x) +
                                 origin.y * static_cast<double>(direction.y) +
                                 origin.z * static_cast<double>(direction.z);
            const double out = origin.x * origin.x + origin.y * origin.y + origin.z * origin.z - radius * radius;
            if (out > 0.0) {
                met.hit = true;
                met.inside = true;
                return met;
            }
            const double distance = -along + std::sqrt(along * along - out);
            if (distance > static_cast<double>(reach))
                return met;
            met.hit = true;
            met.position = DVec3{origin.x + distance * static_cast<double>(direction.x),
                                 origin.y + distance * static_cast<double>(direction.y),
                                 origin.z + distance * static_cast<double>(direction.z)};
            met.normal = Vec3{static_cast<f32>(-met.position.x / radius), static_cast<f32>(-met.position.y / radius),
                              static_cast<f32>(-met.position.z / radius)};
            met.albedo = Color3{albedo, albedo, albedo};
            return met;
        };
    };
    BakeLight light;
    light.sky = [](Vec3) { return Color3{9.0f, 9.0f, 9.0f}; };
    light.lamps.push_back(app::BakeLamp{
        .position = DVec3{0.0, 0.0, 0.0},
        .direction = Vec3{0.0f, -1.0f, 0.0f},
        .radiance = Color3{2.0f, 2.0f, 2.0f},
        .range = 20.0f,
        .cosOuter = -1.0f,
        .shadows = true,
    });
    // The engine's own falloff at five metres of twenty, over pi: what the
    // shader draws the wall with, for an albedo of one.
    const double ratio = radius * radius / (20.0 * 20.0);
    const double window = 1.0 - ratio * ratio;
    const double direct = 2.0 * (window * window / (1.0 + 25.0 * ratio)) / static_cast<double>(kPi);

    const Vec3 size{2.0f, 2.0f, 2.0f};
    for (const f32 albedo : {0.5f, 0.9f}) {
        const double rho = static_cast<double>(albedo);
        double series = 0.0;
        for (core::u32 bounces = 1; bounces <= 3; ++bounces) {
            series += std::pow(rho, static_cast<double>(bounces));
            VolumeBake bake;
            bake.begin(core::CFrameD{}, size, app::volumeLayoutOf(size, 1.0f), bounces);
            REQUIRE(bake.step(roomOf(albedo), light, 0));
            for (std::size_t sample = 0; sample < bake.result().size() / 12; ++sample) {
                // The same from every side, at every point: the wall's own
                // light, by the series.
                for (const Vec3 normal : {Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}}) {
                    const double seen = static_cast<double>(facing(bake.result(), sample, normal).r);
                    CHECK(seen == doctest::Approx(direct * series).epsilon(0.02));
                    CHECK(seen < direct * rho / (1.0 - rho));
                }
            }
        }
    }
}

TEST_CASE("ADR 0203: a volume lit a share a frame is lit by the light its lighting began with, whatever came after")
{
    // A floor under a sun, lit again a sliver at a time while the sun is
    // handed in dimmer every call: what comes out is the first call's sun on
    // every point, not a different sun a slab.
    BakeLight light;
    light.sky = [](Vec3) { return Color3{0.0f, 0.0f, 0.0f}; };
    light.sunDirection = Vec3{0.0f, 1.0f, 0.0f};
    light.sunRadiance = Color3{kPi, kPi, kPi};
    light.stamp = 7;
    const BakeTrace floor = floorOf(Color3{1.0f, 1.0f, 1.0f});
    const Vec3 size{12.0f, 2.0f, 12.0f};
    const core::CFrameD above{.position = DVec3{0.0, 2.0, 0.0}, .rotation = core::Mat3{}};

    VolumeBake whole;
    whole.begin(above, size, app::volumeLayoutOf(size, 1.0f), 2);
    CHECK(whole.litBy() == 0);
    REQUIRE(whole.step(floor, light, 0));
    CHECK(whole.litBy() == 7);

    VolumeBake sliced;
    sliced.begin(above, size, app::volumeLayoutOf(size, 1.0f), 2);
    BakeLight dark = light;
    dark.sunRadiance = Color3{0.0f, 0.0f, 0.0f};
    dark.stamp = 1;
    REQUIRE(sliced.step(floor, dark, 0));
    sliced.relight();
    CHECK(sliced.litBy() == 0);
    BakeLight now = light;
    int frames = 0;
    while (!sliced.step(floor, now, 1)) {
        // The sun goes down a little every frame, and says so.
        now.sunRadiance = Color3{now.sunRadiance.r * 0.9f, now.sunRadiance.g * 0.9f, now.sunRadiance.b * 0.9f};
        now.stamp += 1;
        CHECK(sliced.litBy() == 7);
        REQUIRE(++frames < 100000);
    }
    CHECK(frames > 2);
    CHECK(sliced.litBy() == 7);
    REQUIRE(sliced.result().size() == whole.result().size());
    for (std::size_t value = 0; value < whole.result().size(); ++value)
        CHECK(sliced.result()[value] == whole.result()[value]);
}

TEST_CASE("ADR 0203: the volume lit again next is one in view that has waited longest, and the nearest of those")
{
    using app::RelightCandidate;
    // Nothing stale: nothing to do.
    std::vector<RelightCandidate> volumes{
        RelightCandidate{.stale = false, .inView = true, .distance = 1.0, .served = 0},
        RelightCandidate{.stale = false, .inView = true, .distance = 2.0, .served = 0},
    };
    CHECK(app::pickRelight(volumes) == volumes.size());

    // Never lit again, both in view: the nearest.
    volumes[0].stale = true;
    volumes[1].stale = true;
    CHECK(app::pickRelight(volumes) == 0);
    // It has had its turn: the other one, though it is further and the first
    // is stale again -- which under a light that changes every frame it
    // always is, and was served every time.
    volumes[0].served = 1;
    CHECK(app::pickRelight(volumes) == 1);
    volumes[1].served = 2;
    CHECK(app::pickRelight(volumes) == 0);

    // One out of view waits for those in view, however long it has waited.
    volumes.push_back(RelightCandidate{.stale = true, .inView = false, .distance = 0.5, .served = 0});
    volumes[0].served = 3;
    CHECK(app::pickRelight(volumes) == 1);
    // And is lit again when nothing in view is stale.
    volumes[0].stale = false;
    volumes[1].stale = false;
    CHECK(app::pickRelight(volumes) == 2);
}

TEST_CASE("ADR 0206: a box more than one volume holds is tiles on its own grid, each sharing a cell with the next")
{
    // What one volume holds is one piece, as it always was.
    const std::vector<app::VolumePiece> room = app::volumeTilesOf(core::CFrameD{}, Vec3{60.0f, 14.0f, 60.0f}, 2.42f);
    REQUIRE(room.size() == 1);
    CHECK(room[0].margin == 0.0f);
    CHECK_FALSE(room[0].layout.coarser);

    // The same box a metre between its points is fifty thousand of them.
    const Vec3 size{60.0f, 14.0f, 60.0f};
    const std::vector<app::VolumePiece> tiles = app::volumeTilesOf(core::CFrameD{}, size, 1.0f);
    REQUIRE(tiles.size() > 1);
    CHECK(tiles.size() <= app::kMaxVolumeTiles);
    double low[3] = {1.0e9, 1.0e9, 1.0e9};
    double high[3] = {-1.0e9, -1.0e9, -1.0e9};
    for (const app::VolumePiece& tile : tiles) {
        std::size_t points = 1;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const core::u32 count = tile.layout.counts[axis];
            CHECK(count <= render::kIrradianceVolumeSide);
            points *= count;
            const double length = static_cast<double>((&tile.size.x)[axis]);
            const double middle = (&tile.box.position.x)[axis];
            // A metre a cell, and its points on the box's own grid: half a
            // metre in from a whole number of metres.
            CHECK(length / static_cast<double>(count) == doctest::Approx(1.0));
            const double first = middle - length * 0.5 + 0.5;
            CHECK(first - std::floor(first) == doctest::Approx(0.5));
            low[axis] = std::min(low[axis], middle - length * 0.5);
            high[axis] = std::max(high[axis], middle + length * 0.5);
        }
        CHECK(points <= render::kIrradianceVolumeSamples);
        // Two cells over which it gives way to the tile beside it.
        CHECK(static_cast<double>(tile.margin) == doctest::Approx(2.0));
    }
    // Between them, the whole box and nothing past it.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        CHECK(low[axis] == doctest::Approx(-static_cast<double>((&size.x)[axis]) * 0.5));
        CHECK(high[axis] == doctest::Approx(static_cast<double>((&size.x)[axis]) * 0.5));
    }

    // A box that would take more tiles than a volume may have is one piece
    // at a coarser spacing, and says so.
    const std::vector<app::VolumePiece> vast = app::volumeTilesOf(core::CFrameD{}, Vec3{400.0f, 40.0f, 400.0f}, 0.5f);
    REQUIRE(vast.size() == 1);
    CHECK(vast[0].layout.coarser);
}

TEST_CASE("ADR 0206: levels round the camera double, stand on their own grids, and move a few cells at a time")
{
    const Vec3 size{40.0f, 16.0f, 40.0f};
    const std::vector<app::VolumePiece> levels = app::volumeLevelsOf(size, 2.0f, 3, DVec3{101.0, 3.0, -57.0});
    REQUIRE(levels.size() == 3);
    for (std::size_t level = 0; level < 3; ++level) {
        const app::VolumePiece& piece = levels[level];
        const double scale = static_cast<double>(1u << level);
        // Lying down: twenty by twenty by eight, its own z the world's up.
        CHECK(piece.layout.counts[0] == 20);
        CHECK(piece.layout.counts[1] == 20);
        CHECK(piece.layout.counts[2] == 8);
        CHECK(static_cast<double>(piece.size.x) == doctest::Approx(40.0 * scale));
        CHECK(static_cast<double>(piece.size.z) == doctest::Approx(16.0 * scale));
        const core::Vec3 up = core::transformDirection(piece.box, Vec3{0.0f, 0.0f, 1.0f});
        CHECK(std::abs(static_cast<double>(up.y)) == doctest::Approx(1.0));
        // On a grid of four of its own cells: eight metres, sixteen, thirty-two.
        const double step = 8.0 * scale;
        CHECK(std::abs(std::remainder(piece.box.position.x, step)) < 1.0e-6);
        CHECK(std::abs(std::remainder(piece.box.position.z, step)) < 1.0e-6);
        CHECK(std::abs(piece.box.position.x - 101.0) <= step * 0.5 + 1.0e-6);
        // It gives way to the level round it over two of its cells.
        CHECK(static_cast<double>(piece.margin) == doctest::Approx(-4.0 * scale));
    }
    // A step of the camera is not a step of a level.
    const std::vector<app::VolumePiece> stepped = app::volumeLevelsOf(size, 2.0f, 3, DVec3{102.5, 3.0, -57.0});
    CHECK(stepped[0].box.position.x == levels[0].box.position.x);
    // And more levels than there may be are three.
    CHECK(app::volumeLevelsOf(size, 2.0f, 9, DVec3{}).size() == app::kMaxVolumeLevels);
}

TEST_CASE(
    "ADR 0206: a volume carried a few cells is the volume worked out where it now stands, for a share of the rays")
{
    BakeLight light = whiteSky();
    light.sunDirection = core::normalize(Vec3{0.3f, 1.0f, 0.2f});
    light.sunRadiance = Color3{2.0f, 1.8f, 1.5f};
    light.stamp = 11;
    std::atomic<int> asked{0};
    // A floor with a wall standing on it along x = 3: what a point sees is
    // not the same from one place to the next.
    const BakeTrace scene = [&asked](const DVec3& origin, Vec3 direction, f32 reach) {
        asked.fetch_add(1);
        BakeHit nearest;
        double best = static_cast<double>(reach);
        if (origin.y < 0.0) {
            nearest.hit = true;
            nearest.inside = true;
            return nearest;
        }
        if (direction.y < -1.0e-6f) {
            const double along = -origin.y / static_cast<double>(direction.y);
            if (along < best) {
                best = along;
                nearest.hit = true;
                nearest.position = DVec3{origin.x + along * static_cast<double>(direction.x), 0.0,
                                         origin.z + along * static_cast<double>(direction.z)};
                nearest.normal = Vec3{0.0f, 1.0f, 0.0f};
                nearest.albedo = Color3{0.6f, 0.5f, 0.4f};
            }
        }
        if (std::abs(direction.x) > 1.0e-6f) {
            const double along = (3.0 - origin.x) / static_cast<double>(direction.x);
            const double height = origin.y + along * static_cast<double>(direction.y);
            if (along > 1.0e-6 && along < best && height >= 0.0 && height <= 3.0) {
                nearest.hit = true;
                nearest.position = DVec3{3.0, height, origin.z + along * static_cast<double>(direction.z)};
                nearest.normal = Vec3{direction.x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
                nearest.albedo = Color3{0.9f, 0.1f, 0.1f};
            }
        }
        return nearest;
    };
    const Vec3 size{8.0f, 4.0f, 8.0f};
    const VolumeLayout layout = app::volumeLayoutOf(size, 1.0f);
    const core::CFrameD first{.position = DVec3{0.0, 2.0, 0.0}, .rotation = core::Mat3{}};
    const core::CFrameD second{.position = DVec3{2.0, 2.0, -1.0}, .rotation = core::Mat3{}};

    VolumeBake afresh;
    afresh.begin(second, size, layout, 2);
    REQUIRE(afresh.step(scene, light, 0));
    const int whole = asked.exchange(0);

    VolumeBake carried;
    carried.begin(first, size, layout, 2);
    REQUIRE(carried.step(scene, light, 0));
    asked.store(0);
    // Not further than it is long: that is another volume, begun again.
    const core::i32 far[3] = {8, 0, 0};
    CHECK_FALSE(carried.scroll(second, far));
    const core::i32 cells[3] = {2, 0, -1};
    REQUIRE(carried.scroll(second, cells));
    CHECK_FALSE(carried.whole());
    REQUIRE(carried.step(scene, light, 0));
    // Under the same light only the points that are new were cast and lit
    // for: three eighths of them here, and well under the whole.
    CHECK(asked.load() < whole / 2);
    CHECK(asked.load() > 0);
    REQUIRE(carried.result().size() == afresh.result().size());
    for (std::size_t value = 0; value < afresh.result().size(); ++value)
        CHECK(static_cast<double>(carried.result()[value]) ==
              doctest::Approx(static_cast<double>(afresh.result()[value])).epsilon(0.001).scale(0.01));

    // Under another light every point is lit again, and none is cast again
    // but the new.
    BakeLight evening = light;
    evening.sunRadiance = Color3{0.5f, 0.3f, 0.2f};
    evening.stamp = 12;
    VolumeBake dusk;
    dusk.begin(first, size, layout, 2);
    REQUIRE(dusk.step(scene, evening, 0));
    const core::i32 back[3] = {-2, 0, 1};
    REQUIRE(carried.scroll(first, back));
    REQUIRE(carried.step(scene, evening, 0));
    for (std::size_t value = 0; value < dusk.result().size(); ++value)
        CHECK(static_cast<double>(carried.result()[value]) ==
              doctest::Approx(static_cast<double>(dusk.result()[value])).epsilon(0.001).scale(0.01));
}

// --- A volume's light, kept (ADR 0207) -------------------------------------------

TEST_CASE("ADR 0207: a kept volume is written and read back the same, with the mean of what it holds")
{
    app::BakedLight light;
    light.box.position = DVec3{12.5, 3.0, -40.25};
    light.box.rotation = core::fromEulerYxz(Vec3{0.0f, 0.5f, 0.0f});
    light.size = Vec3{8.0f, 4.0f, 6.0f};
    light.spacing = 2.0f;
    light.bounces = 2;
    light.mode = 1;
    light.room = 0x1234'5678'9ABC'DEF0ull;
    light.pieces.resize(2);
    for (std::size_t piece = 0; piece < 2; ++piece) {
        light.pieces[piece].counts[0] = 4;
        light.pieces[piece].counts[1] = 2;
        light.pieces[piece].counts[2] = 3;
        light.pieces[piece].samples.resize(4 * 2 * 3 * 12);
        for (std::size_t at = 0; at < light.pieces[piece].samples.size(); ++at)
            light.pieces[piece].samples[at] = static_cast<f32>(at % 12) * 0.01f + static_cast<f32>(piece);
    }
    const std::vector<std::byte> bytes = app::writeBakedLight(light);
    app::BakedLight read;
    REQUIRE(app::readBakedLight(bytes, read));
    CHECK(read.box.position.x == 12.5);
    CHECK(read.box.position.z == -40.25);
    CHECK(read.box.rotation.m[0][2] == light.box.rotation.m[0][2]);
    CHECK(read.size.y == 4.0f);
    CHECK(read.spacing == 2.0f);
    CHECK(read.bounces == 2);
    CHECK(read.mode == 1);
    CHECK(read.room == light.room);
    REQUIRE(read.pieces.size() == 2);
    CHECK(read.pieces[1].counts[2] == 3);
    CHECK(read.pieces[1].samples == light.pieces[1].samples);
    // The mean is of the light from every side -- the first three numbers of
    // each point -- over every piece: 0, 0.01, 0.02 in one and a whole more
    // in the other.
    CHECK(static_cast<double>(read.mean.r) == doctest::Approx(0.5));
    CHECK(static_cast<double>(read.mean.g) == doctest::Approx(0.51));
    CHECK(static_cast<double>(read.mean.b) == doctest::Approx(0.52));
    // Written again it is the same bytes.
    CHECK(app::writeBakedLight(read) == bytes);
}

TEST_CASE("ADR 0207: bytes that are not a kept volume are refused, however nearly they are one")
{
    app::BakedLight light;
    light.pieces.resize(1);
    light.pieces[0].samples.assign(12, 0.25f);
    const std::vector<std::byte> good = app::writeBakedLight(light);
    app::BakedLight read;
    REQUIRE(app::readBakedLight(good, read));

    // Nothing at all, another file's first bytes, and a version not read.
    CHECK_FALSE(app::readBakedLight({}, read));
    std::vector<std::byte> other = good;
    other[0] = std::byte{'X'};
    CHECK_FALSE(app::readBakedLight(other, read));
    std::vector<std::byte> newer = good;
    newer[8] = std::byte{9};
    CHECK_FALSE(app::readBakedLight(newer, read));
    // Cut short, and with something after its last piece.
    std::vector<std::byte> cut(good.begin(), good.end() - 5);
    CHECK_FALSE(app::readBakedLight(cut, read));
    std::vector<std::byte> longer = good;
    longer.push_back(std::byte{0});
    CHECK_FALSE(app::readBakedLight(longer, read));
    // A number that is not one: the last sample's, as a NaN's bytes.
    std::vector<std::byte> broken = good;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    std::memcpy(broken.data() + broken.size() - sizeof(f32), &nan, sizeof(f32));
    CHECK_FALSE(app::readBakedLight(broken, read));
    // More points along a side than a volume holds.
    app::BakedLight huge = light;
    huge.pieces[0].counts[0] = 64;
    huge.pieces[0].samples.assign(64 * 12, 0.0f);
    CHECK_FALSE(app::readBakedLight(app::writeBakedLight(huge), read));
}

TEST_CASE("ADR 0207: a volume given its light is whole with no ray cast, and worked out afresh by begin")
{
    const VolumeLayout layout = app::volumeLayoutOf(Vec3{4.0f, 2.0f, 2.0f}, 2.0f);
    const std::size_t points = static_cast<std::size_t>(layout.counts[0]) * layout.counts[1] * layout.counts[2];
    std::vector<f32> kept(points * 12, 0.0f);
    for (std::size_t point = 0; point < points; ++point)
        kept[point * 12] = 0.75f;

    VolumeBake bake;
    bake.give(core::CFrameD{}, Vec3{4.0f, 2.0f, 2.0f}, layout, kept);
    CHECK(bake.begun());
    CHECK(bake.whole());
    CHECK(bake.progress() == 1.0f);
    CHECK(bake.layout().counts[0] == layout.counts[0]);
    REQUIRE(bake.result().size() == kept.size());
    CHECK(bake.result()[0] == 0.75f);
    // A step has nothing to do, and asks nothing of the world.
    std::atomic<int> asked{0};
    CHECK(bake.step(floorOf(Color3{1.0f, 1.0f, 1.0f}, &asked), whiteSky(), 0));
    CHECK(asked.load() == 0);
    // It has no rays to be lit again by: lit again it is still the file's.
    bake.relight();
    CHECK(bake.whole());
    CHECK_FALSE(bake.hasRays());
    // Begun, it is worked out as any volume is.
    core::CFrameD above;
    above.position = DVec3{0.0, 2.0, 0.0};
    bake.begin(above, Vec3{4.0f, 2.0f, 2.0f}, layout, 1);
    CHECK_FALSE(bake.whole());
    CHECK(bake.step(floorOf(Color3{1.0f, 1.0f, 1.0f}, &asked), whiteSky(), 0));
    CHECK(asked.load() > 0);
}
