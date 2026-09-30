// Rules paint a terrain by slope and height (ADR 0113 §2), and the CPU's answer
// to "what is drawn here" is the shader's.
#include <cmath>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/asset/terrain_rules.h"

using namespace engine;
using namespace engine::asset;

namespace {

constexpr core::u8 Grass = 1;
constexpr core::u8 Rock = 3;
constexpr core::u8 Snow = 4;

// A surface this many degrees from level.
[[nodiscard]] core::Vec3 tilted(double degrees)
{
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    return core::Vec3{static_cast<float>(std::sin(radians)), static_cast<float>(std::cos(radians)), 0.0f};
}

} // namespace

TEST_CASE("a new terrain's one rule is the slope rock it always had")
{
    const std::vector<TerrainRule> rules = defaultTerrainRules();
    REQUIRE(rules.size() == 1);
    CHECK(rules[0].material == Rock);
    // The old `smoothstep(0.24, 0.36, 1 - normal.y)`, to the cosine.
    const TerrainRuleShape shape = shapeOf(rules[0]);
    CHECK(static_cast<double>(shape.slope[0]) == doctest::Approx(0.24).epsilon(0.01));
    CHECK(static_cast<double>(shape.slope[1]) == doctest::Approx(0.36).epsilon(0.01));
    // Rock and basalt are not painted over; every other engine layer is.
    CHECK_FALSE(ruleCovers(shape, Rock));
    CHECK_FALSE(ruleCovers(shape, 7));
    CHECK(ruleCovers(shape, Grass));
    CHECK_FALSE(ruleCovers(shape, 0));
}

TEST_CASE("steep grass is drawn as rock, and with the rule off it stays grass")
{
    const std::vector<TerrainRule> rules = defaultTerrainRules();
    const core::Vec3 ground{10.0f, 2.0f, 10.0f};
    CHECK(drawnMaterial(rules, Grass, tilted(70.0), ground, 2.0f) == Rock);
    CHECK(drawnMaterial(rules, Grass, tilted(10.0), ground, 2.0f) == Grass);

    std::vector<TerrainRule> off = rules;
    off[0].enabled = false;
    CHECK(drawnMaterial(off, Grass, tilted(70.0), ground, 2.0f) == Grass);
    CHECK(drawnMaterial({}, Grass, tilted(70.0), ground, 2.0f) == Grass);
}

TEST_CASE("a height rule puts snow above a level")
{
    TerrainRule snow;
    snow.material = Snow;
    snow.heightMin = 40.0f;
    snow.blend = 2.0f;
    snow.noise = 0.0f;
    const std::vector<TerrainRule> rules{snow};
    const core::Vec3 ground{0.0f, 0.0f, 0.0f};
    CHECK(drawnMaterial(rules, Grass, tilted(5.0), ground, 60.0f) == Snow);
    CHECK(drawnMaterial(rules, Grass, tilted(5.0), ground, 20.0f) == Grass);
    // A snow rule does not cover snow with itself.
    CHECK(drawnMaterial(rules, Snow, tilted(5.0), ground, 20.0f) == Snow);
}

TEST_CASE("the rules' noise is a pure function of the point, between 0 and 1, and smooth")
{
    for (float x = -50.0f; x < 50.0f; x += 3.7f) {
        for (float z = -50.0f; z < 50.0f; z += 2.9f) {
            const float value = terrainRuleNoise(x, z);
            CHECK(value >= 0.0f);
            CHECK(value <= 1.0f);
            CHECK(value == terrainRuleNoise(x, z));
            // A centimetre away is nearly the same.
            CHECK(std::abs(static_cast<double>(terrainRuleNoise(x + 0.01f, z) - value)) < 0.02);
        }
    }
}

TEST_CASE("applying the rules writes what they draw, so the rule can then be off")
{
    // A steep bank of grass: a slab, and a slope cut into its side.
    TerrainField field(FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 4.0f, Grass);
    (void)fillBall(field, core::DVec3{0.0, 4.0, 0.0}, 10.0, 0);
    const std::vector<TerrainRule> rules = defaultTerrainRules();

    const EditReport written =
        applyRules(field, rules, core::DVec3{-20.0, -10.0, -20.0}, core::DVec3{20.0, 10.0, 20.0}, 0.0);
    CHECK(written.touched > 0);

    // Somewhere on the crater's wall is rock now, in the voxels themselves.
    bool rock = false;
    for (core::i32 x = -12; x <= 12 && !rock; ++x) {
        for (core::i32 y = -8; y <= 6 && !rock; ++y)
            rock = field.voxel(x, y, 0).material == Rock && field.voxel(x, y, 0).occupancy > 0;
    }
    CHECK(rock);
    // And the flat ground far from it kept its grass.
    CHECK(field.voxel(25, 3, 25).material == Grass);

    // Applying again changes nothing: the voxels already are what is drawn.
    const EditReport again =
        applyRules(field, rules, core::DVec3{-20.0, -10.0, -20.0}, core::DVec3{20.0, 10.0, 20.0}, 0.0);
    CHECK(again.touched == 0);
}

TEST_CASE("paint is drawn over the ground, and a rule over the paint, by the ground's own layer")
{
    // The terrain audit's CPU and GPU agreement: the shader draws the paint
    // over the voxel's layer from half its cover, and a rule over both by the
    // voxel's layer. What the game is told the ground is must be that.
    constexpr core::u8 Sand = 2;
    const std::vector<TerrainRule> rules = defaultTerrainRules();
    const core::Vec3 ground{10.0f, 2.0f, 10.0f};
    const Voxel painted{255, Grass, Sand, 200};
    CHECK(drawnMaterial(rules, painted, tilted(10.0), ground, 2.0f) == Sand);
    // Under half its cover, the ground's own layer shows.
    CHECK(drawnMaterial(rules, Voxel{255, Grass, Sand, 100}, tilted(10.0), ground, 2.0f) == Grass);
    // Steep, the slope rock covers the grass under the paint.
    CHECK(drawnMaterial(rules, painted, tilted(70.0), ground, 2.0f) == Rock);
    // Rock painted with sand: the rule does not cover rock, so the sand shows.
    CHECK(drawnMaterial(rules, Voxel{255, Rock, Sand, 200}, tilted(70.0), ground, 2.0f) == Sand);
}

TEST_CASE("a ray at painted ground says what is painted there")
{
    constexpr core::u8 Sand = 2;
    TerrainField field(FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)fillBlock(field, {0.0, 2.0, 0.0}, {32.0f, 4.0f, 32.0f}, Grass);
    PaintOptions partly;
    partly.mode = PaintMode::Blend;
    partly.strength = 0.6f;
    (void)paintBall(field, {0.0, 4.0, 0.0}, 4.0, Sand, partly);
    const std::optional<TerrainHit> hit = raycastField(field, {0.5, 20.0, 0.5}, {0.0f, -1.0f, 0.0f}, 40.0);
    REQUIRE(hit.has_value());
    CHECK(hit->material == Grass);
    CHECK(hit->top == Sand);
    CHECK(hit->cover >= 128);
    const core::Vec3 at{0.5f, 4.0f, 0.5f};
    CHECK(drawnMaterial({}, Voxel{255, hit->material, hit->top, hit->cover}, hit->normal, at, 4.0f) == Sand);
}

TEST_CASE("applying the rules writes what a rule draws, and leaves paint no rule covers as paint")
{
    constexpr core::u8 Sand = 2;
    TerrainField field(FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)fillBlock(field, {0.0, 2.0, 0.0}, {32.0f, 4.0f, 32.0f}, Grass);
    PaintOptions partly;
    partly.mode = PaintMode::Blend;
    partly.strength = 0.6f;
    (void)paintBall(field, {0.0, 4.0, 0.0}, 3.0, Sand, partly);
    const Voxel before = field.voxel(0, 3, 0);
    REQUIRE(before.top == Sand);
    (void)applyRules(field, defaultTerrainRules(), {-20.0, -4.0, -20.0}, {20.0, 12.0, 20.0}, 0.0);
    // Flat ground: no rule covers it, and its paint is kept.
    CHECK(field.voxel(0, 3, 0) == before);
}
