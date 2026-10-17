// The parallel character step (ADR 0228): the characters of a crowd that
// nothing they could change is near -- no dynamic body, no other character
// stepping that could reach the same space -- step together on several
// threads; the rest step alone, in order, as one character always did.
//
// What is held here is the step's contract: the same world, bit for bit,
// whatever the number of threads and whatever the order the steps are handed
// in, and no two characters ever in one another.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <vector>

#include "engine/physics/backends.h"
#include "engine/physics/physics.h"
#include "engine/physics/types.h"

using namespace engine;
using namespace engine::physics;

namespace {

constexpr f32 kFixedDt = 1.0f / 60.0f;
constexpr int kWalkers = 48;
constexpr f64 kRadius = 0.5;
// Half the length of a capsule's axis: half its height less its radius.
constexpr f64 kHalfAxis = 1.0;
// Ticks the crowd walks for, then stands still for.
constexpr int kWalking = 180;
constexpr int kStanding = 60;

// **How the crowd walks.** Converging: from a ring onto its middle, where it
// packs and pushes -- whoever is in the press steps alone. In lanes: rows
// passing one another the other way, never near enough to meet -- every one
// of them steps together.
enum class Layout
{
    Converging,
    Lanes,
};

struct CrowdRun
{
    std::vector<u8> state;
    // The most of the crowd that stepped together, and alone, in any tick;
    // and of those together, the most that touched a dynamic body.
    u32 together = 0;
    u32 alone = 0;
    u32 metDynamic = 0;
    // The deepest two walkers' capsules were ever in one another.
    f64 deepest = 0.0;
};

[[nodiscard]] BodyDesc box(core::DVec3 at, core::Vec3 size, MotionType motion, u64 userData)
{
    BodyDesc desc;
    desc.shape.type = ShapeType::Box;
    desc.shape.size = size;
    desc.transform.position = at;
    desc.motion = motion;
    desc.userData = userData;
    return desc;
}

[[nodiscard]] CharacterHandle character(IPhysics3D& physics, WorldHandle world, core::DVec3 at, u64 userData)
{
    CharacterDesc desc;
    desc.transform.position = at;
    desc.height = 3.0f;
    desc.diameter = static_cast<f32>(2.0 * kRadius);
    desc.userData = userData;
    return physics.createCharacter(world, desc);
}

// The 48 walkers, laid out and walking as `layout` says; four more, far out,
// each walking into a crate of its own; and one walking across a dynamic
// platform. `together` is `moveCharacters`, or each character moved on its own
// (`moveCharacter`) -- the step as it was.
[[nodiscard]] CrowdRun runCrowd(u32 threads, bool reversed, Layout layout, f32 pace, bool together = true)
{
    CrowdRun run;
    PhysicsResult physics = createJoltPhysics();
    REQUIRE(physics != nullptr);
    const WorldHandle world = physics->createWorld(WorldDesc{});
    REQUIRE(world.valid());
    physics->setPhysicsThreads(world, threads);
    REQUIRE(physics->createBody(world, box({0.0, -1.0, 0.0}, {120.0f, 2.0f, 120.0f}, MotionType::Static, 1)).valid());

    std::vector<CharacterHandle> walkers;
    std::vector<core::Vec3> heading;
    for (int i = 0; i < kWalkers; ++i) {
        core::DVec3 at;
        core::Vec3 towards;
        if (layout == Layout::Converging) {
            const f64 angle = 6.283185307179586 * static_cast<f64>(i) / kWalkers;
            const f64 ring = 7.0 + static_cast<f64>(i % 3) * 1.2;
            at = core::DVec3{ring * std::cos(angle), 1.6, ring * std::sin(angle)};
            towards = core::Vec3{static_cast<f32>(-std::cos(angle)), 0.0f, static_cast<f32>(-std::sin(angle))};
        }
        else {
            // Six rows of eight, 1.6 metres apart -- 60 centimetres between
            // two capsules -- each row the other way to the last.
            const int row = i / 8;
            at = core::DVec3{-10.5 + static_cast<f64>(i % 8) * 3.0, 1.6, -4.0 + static_cast<f64>(row) * 1.6};
            towards = core::Vec3{row % 2 == 0 ? 1.0f : -1.0f, 0.0f, 0.0f};
        }
        walkers.push_back(character(*physics, world, at, 100 + static_cast<u64>(i)));
        heading.push_back(towards);
    }
    std::vector<CharacterHandle> others;
    std::vector<core::Vec3> otherHeading;
    for (int i = 0; i < 4; ++i) {
        const f64 x = i == 0 ? 30.0 : i == 1 ? -30.0 : 0.0;
        const f64 z = i == 2 ? 30.0 : i == 3 ? -30.0 : 0.0;
        const core::Vec3 out{static_cast<f32>(x / 30.0), 0.0f, static_cast<f32>(z / 30.0)};
        others.push_back(character(*physics, world, {x, 1.6, z}, 200 + static_cast<u64>(i)));
        otherHeading.push_back(out);
        REQUIRE(physics
                    ->createBody(world, box({x + 2.5 * static_cast<f64>(out.x), 0.5, z + 2.5 * static_cast<f64>(out.z)},
                                            {1.0f, 1.0f, 1.0f}, MotionType::Dynamic, 300 + static_cast<u64>(i)))
                    .valid());
    }
    REQUIRE(physics->createBody(world, box({20.0, 0.25, 20.0}, {4.0f, 0.5f, 4.0f}, MotionType::Dynamic, 400)).valid());
    others.push_back(character(*physics, world, {19.0, 2.1, 20.0}, 204));
    otherHeading.push_back(core::Vec3{0.5f, 0.0f, 0.0f});

    std::vector<CharacterMove> moves;
    for (int tick = 0; tick < kWalking + kStanding; ++tick) {
        const f32 speed = tick < kWalking ? pace : 0.0f;
        moves.clear();
        for (std::size_t i = 0; i < walkers.size(); ++i)
            moves.push_back(CharacterMove{walkers[i], heading[i] * speed + core::Vec3{0.0f, -2.0f, 0.0f}, kFixedDt});
        for (std::size_t i = 0; i < others.size(); ++i)
            moves.push_back(CharacterMove{others[i], otherHeading[i] * 2.0f + core::Vec3{0.0f, -2.0f, 0.0f}, kFixedDt});
        if (reversed)
            std::reverse(moves.begin(), moves.end());
        if (together) {
            physics->moveCharacters(world, moves);
            const CharacterStepCounts counts = physics->lastCharacterStep(world);
            run.together = std::max(run.together, counts.together);
            run.alone = std::max(run.alone, counts.alone);
            run.metDynamic = std::max(run.metDynamic, counts.togetherMetDynamic);
        }
        else {
            for (const CharacterMove& move : moves)
                physics->moveCharacter(world, move.character, move.velocity, move.fixedDt, move.walking);
        }
        physics->step(world, kFixedDt);

        for (std::size_t a = 0; a < walkers.size(); ++a) {
            const core::DVec3 p = physics->characterState(world, walkers[a]).transform.position;
            for (std::size_t b = a + 1; b < walkers.size(); ++b) {
                const core::DVec3 q = physics->characterState(world, walkers[b]).transform.position;
                // Upright capsules: the nearest points of their two axes.
                const f64 above = std::max(0.0, std::abs(p.y - q.y) - 2.0 * kHalfAxis);
                const f64 apart = std::hypot(std::hypot(p.x - q.x, p.z - q.z), above);
                run.deepest = std::max(run.deepest, 2.0 * kRadius - apart);
            }
        }
    }
    REQUIRE(physics->saveState(world, run.state));
    physics->destroyWorld(world);
    return run;
}

} // namespace

TEST_CASE("ADR 0228: a crowd steps the same on any number of threads, bit for bit")
{
    // Running into one another, so the press steps alone and the rest
    // together, in the same tick.
    const CrowdRun none = runCrowd(0, false, Layout::Converging, 6.0f);
    const CrowdRun one = runCrowd(1, false, Layout::Converging, 6.0f);
    const CrowdRun four = runCrowd(4, false, Layout::Converging, 6.0f);

    CHECK(four.together > 0);
    CHECK(four.alone > 5);
    // None of those together touched a dynamic body, which would mean the
    // reach is wrong.
    CHECK(four.metDynamic == 0);

    REQUIRE(!none.state.empty());
    CHECK(none.state == one.state);
    CHECK(none.state == four.state);
}

TEST_CASE("ADR 0228: a crowd steps the same whatever order its steps are handed in")
{
    // Every walker together, and the five near a crate or on the platform
    // alone -- which do not meet one another, so their order decides nothing
    // either. (Characters stepping alone that meet do so in the order they are
    // handed in, as they always did.)
    const CrowdRun forward = runCrowd(4, false, Layout::Lanes, 3.0f);
    const CrowdRun backward = runCrowd(4, true, Layout::Lanes, 3.0f);
    CHECK(forward.together == kWalkers);
    CHECK(forward.alone == 5);
    CHECK(forward.metDynamic == 0);
    REQUIRE(!forward.state.empty());
    CHECK(forward.state == backward.state);
}

TEST_CASE("ADR 0228: no two characters stepped together end up in one another")
{
    // A character steps together only when no other character stepping could
    // reach the same space, so two can never both step into it: a crowd
    // running into itself is as apart as one stepped one character after
    // another.
    for (const f32 pace : {3.0f, 6.0f, 12.0f, 20.0f}) {
        const CrowdRun together = runCrowd(4, false, Layout::Converging, pace);
        const CrowdRun serial = runCrowd(4, false, Layout::Converging, pace, false);
        MESSAGE("at " << pace << " m/s, the deepest two characters were in one another: together " << together.deepest
                      << " m, one after another " << serial.deepest << " m; " << together.together
                      << " stepped together and " << together.alone << " alone at most");
        CHECK(together.deepest < 1.0e-3);
        CHECK(together.metDynamic == 0);
    }
}
