// Fluids in the block world (V1): the rules, and that they are deterministic.
#include <chrono>
#include <doctest/doctest.h>
#include <utility>

#include "engine/scene/voxel_fluid.h"

using namespace engine;
using asset::BlockId;

namespace {

constexpr BlockId Stone = 1;
constexpr BlockId Water = 2;

// A stone floor at y = -1, forty blocks square, and water that reaches four
// and moves every tick.
struct Pond
{
    scene::VoxelComponent voxels;
    core::u64 tick = 0;

    Pond()
    {
        voxels.types.push_back(scene::VoxelBlockType{});
        scene::VoxelBlockType water;
        water.fluidReach = 4;
        water.fluidTicks = 1;
        voxels.types.push_back(water);
        (void)voxels.grid.fill(-20, -1, -20, 20, -1, 20, Stone);
    }

    void place(core::i32 x, core::i32 y, core::i32 z, BlockId id)
    {
        (void)voxels.grid.set(x, y, z, id);
        scene::wakeFluids(voxels, x, y, z);
    }

    void run(int ticks)
    {
        for (int at = 0; at < ticks; ++at)
            (void)scene::stepFluids(voxels, ++tick);
    }

    [[nodiscard]] BlockId at(core::i32 x, core::i32 y, core::i32 z) const { return voxels.grid.get(x, y, z); }

    [[nodiscard]] int waterBlocks() const
    {
        int count = 0;
        for (core::i32 y = -1; y <= 8; ++y) {
            for (core::i32 z = -12; z <= 12; ++z) {
                for (core::i32 x = -12; x <= 12; ++x)
                    count += asset::blockTypeOf(at(x, y, z)) == Water ? 1 : 0;
            }
        }
        return count;
    }
};

} // namespace

TEST_CASE("a source on a floor spreads its reach and no further, one level shallower per block")
{
    Pond pond;
    pond.place(0, 0, 0, Water);
    pond.run(20);

    CHECK(pond.at(0, 0, 0) == Water);
    for (core::i32 out = 1; out <= 4; ++out) {
        CHECK(pond.at(out, 0, 0) == asset::blockWithState(Water, static_cast<core::u32>(out)));
        CHECK(pond.at(0, 0, -out) == asset::blockWithState(Water, static_cast<core::u32>(out)));
    }
    CHECK(pond.at(5, 0, 0) == asset::AirBlock);
    // Distance is counted in steps, so a diagonal block is as far as its two
    // sides added.
    CHECK(pond.at(2, 0, 2) == asset::blockWithState(Water, 4));
    CHECK(pond.at(3, 0, 2) == asset::AirBlock);
    // **Settled water costs nothing**: nothing is left due.
    CHECK(pond.voxels.fluidWakes.empty());
    // A fluid never rises: nothing appeared above the source.
    CHECK(pond.at(0, 1, 0) == asset::AirBlock);
}

TEST_CASE("a fluid falls before it spreads, and lands as if it were a source")
{
    Pond pond;
    // A pillar five blocks tall with a source on top.
    (void)pond.voxels.grid.fill(0, 0, 0, 0, 4, 0, Stone);
    pond.place(0, 5, 0, Water);
    pond.run(40);

    // Off the pillar's edge, the first block out has nothing under it: it is
    // there, and it pours rather than spreading.
    CHECK(pond.at(1, 5, 0) == asset::blockWithState(Water, 1));
    CHECK(pond.at(2, 5, 0) == asset::AirBlock);
    for (core::i32 y = 0; y <= 4; ++y)
        CHECK(pond.at(1, y, 0) == asset::blockWithState(Water, asset::FluidFalling));
    // Landed on the floor, the fall spreads its whole reach again.
    CHECK(pond.at(5, 0, 0) == asset::blockWithState(Water, 4));
    CHECK(pond.at(6, 0, 0) == asset::AirBlock);
}

TEST_CASE("a wall holds water back until it is broken")
{
    Pond pond;
    (void)pond.voxels.grid.fill(2, 0, -6, 2, 2, 6, Stone);
    pond.place(0, 0, 0, Water);
    pond.run(20);
    CHECK(pond.at(3, 0, 0) == asset::AirBlock);

    pond.place(2, 0, 0, asset::AirBlock);
    pond.run(20);
    CHECK(pond.at(2, 0, 0) == asset::blockWithState(Water, 2));
    CHECK(pond.at(3, 0, 0) == asset::blockWithState(Water, 3));
}

TEST_CASE("take the source away and what it fed drains")
{
    Pond pond;
    (void)pond.voxels.grid.fill(0, 0, 0, 0, 2, 0, Stone);
    pond.place(0, 3, 0, Water);
    pond.run(30);
    REQUIRE(pond.waterBlocks() > 20);

    pond.place(0, 3, 0, asset::AirBlock);
    pond.run(60);
    CHECK(pond.waterBlocks() == 0);
    CHECK(pond.voxels.fluidWakes.empty());
}

TEST_CASE("the same world and the same ticks make the same water, tick for tick")
{
    Pond first;
    Pond second;
    for (Pond* pond : {&first, &second}) {
        (void)pond->voxels.grid.fill(-3, 0, 4, 3, 1, 4, Stone);
        pond->place(0, 2, 0, Water);
        pond->place(-4, 0, -4, Water);
    }
    for (int tick = 0; tick < 40; ++tick) {
        first.run(1);
        second.run(1);
        REQUIRE(first.voxels.grid.digest() == second.voxels.grid.digest());
        REQUIRE(first.voxels.fluidWakes == second.voxels.fluidWakes);
    }
}

TEST_CASE("a slower fluid moves at its own pace, and one that is not a fluid never moves")
{
    Pond pond;
    pond.voxels.types[Water - 1u].fluidTicks = 10;
    pond.place(0, 0, 0, Water);
    pond.run(1);
    CHECK(pond.at(1, 0, 0) == asset::blockWithState(Water, 1)); // woken: due at once
    pond.run(5);
    CHECK(pond.at(2, 0, 0) == asset::AirBlock); // the next step is ten ticks on
    pond.run(10);
    CHECK(pond.at(2, 0, 0) == asset::blockWithState(Water, 2));

    Pond still;
    still.voxels.types[Water - 1u].fluidReach = 0;
    still.place(0, 0, 0, Water);
    still.run(20);
    CHECK(still.at(1, 0, 0) == asset::AirBlock);
    CHECK(still.voxels.fluidWakes.empty());
}

TEST_CASE("water that arrives in a streamed cell is woken, and nothing else is")
{
    Pond pond;
    // Already in the world, and still: nothing wakes it.
    (void)pond.voxels.grid.set(-10, 0, -10, Water);
    // What a streamed cell brings: a source with room to spread.
    asset::VoxelGrid arrived;
    (void)arrived.set(10, 0, 10, Water);
    (void)pond.voxels.grid.set(10, 0, 10, Water);

    scene::wakeFluidsIn(pond.voxels, arrived);
    pond.run(10);
    CHECK(pond.at(11, 0, 10) == asset::blockWithState(Water, 1));
    CHECK(pond.at(-9, 0, -10) == asset::AirBlock);
}

TEST_CASE("lava that reaches water sets as stone where they meet, and the water stays water")
{
    Pond pond;
    constexpr BlockId Lava = 3;
    scene::VoxelBlockType lava;
    lava.fluidReach = 3;
    lava.fluidTicks = 1;
    pond.voxels.types.push_back(lava);
    scene::setFluidReaction(pond.voxels, Lava, Water, Stone);
    REQUIRE(pond.voxels.fluidReactions.size() == 1);

    // Water spreading from the left, lava from the right: they meet between.
    pond.place(-4, 0, 0, Water);
    pond.place(4, 0, 0, Lava);
    pond.run(20);

    // Where lava would have touched water, there is stone instead, and the
    // water around it is still water.
    bool setStone = false;
    for (core::i32 x = -3; x <= 3; ++x) {
        if (pond.at(x, 0, 0) == Stone)
            setStone = true;
    }
    CHECK(setStone);
    CHECK(asset::blockTypeOf(pond.at(-3, 0, 0)) == Water);
    CHECK(pond.at(4, 0, 0) == Lava);
    // No lava block anywhere touches water.
    for (core::i32 x = -8; x <= 8; ++x) {
        for (core::i32 z = -8; z <= 8; ++z) {
            if (asset::blockTypeOf(pond.at(x, 0, z)) != Lava)
                continue;
            for (const auto& [dx, dz] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                CHECK(asset::blockTypeOf(pond.at(x + dx, 0, z + dz)) != Water);
        }
    }

    // Removed, it is gone.
    scene::setFluidReaction(pond.voxels, Lava, Water, asset::AirBlock);
    CHECK(pond.voxels.fluidReactions.empty());
}

TEST_CASE("D448: building dry land beside a registered fluid costs no fluid work")
{
    // A block game's world, built a column at a time with water registered:
    // every stone block of every column's surface was queued -- seventy
    // thousand a column, twenty times the cost of the build -- and the step
    // then read each of them to find nothing to do.
    Pond pond;
    for (core::i32 column = 0; column < 100; ++column) {
        const core::i32 x = (column % 10) * 16;
        const core::i32 z = (column / 10) * 16;
        (void)pond.voxels.grid.fill(x, 0, z, x + 15, 39, z + 15, Stone);
        scene::wakeFluidsInBox(pond.voxels, x, 0, z, x + 15, 39, z + 15);
    }
    CHECK(pond.voxels.fluidWakes.size() == 0);
    // One block at a time is the same.
    pond.place(3, 40, 3, Stone);
    pond.place(3, 40, 3, asset::AirBlock);
    CHECK(pond.voxels.fluidWakes.size() == 0);
    CHECK(scene::stepFluids(pond.voxels, ++pond.tick) == 0);
}

TEST_CASE("D448: a sea filled into a closed basin is still, and one with an open side flows")
{
    Pond pond;
    // Stone, twelve deep, and a basin cut into it: walls all round, open to
    // the sky.
    (void)pond.voxels.grid.fill(-20, 0, -20, 20, 11, 20, Stone);
    (void)pond.voxels.grid.fill(-10, 2, -10, 10, 11, 10, asset::AirBlock);
    scene::wakeFluidsInBox(pond.voxels, -10, 2, -10, 10, 11, 10);
    CHECK(pond.voxels.fluidWakes.size() == 0);

    // The sea: sources to the brim. Nothing is beside it but stone, more sea
    // and the air over it, where water does not climb.
    (void)pond.voxels.grid.fill(-10, 2, -10, 10, 11, 10, Water);
    scene::wakeFluidsInBox(pond.voxels, -10, 2, -10, 10, 11, 10);
    CHECK(pond.voxels.fluidWakes.size() == 0);
    CHECK(scene::stepFluids(pond.voxels, ++pond.tick) == 0);
    CHECK(pond.voxels.fluidWakes.empty());

    // **And it still flows**: break the wall beside it, and the water comes
    // through.
    pond.place(11, 11, 0, asset::AirBlock);
    pond.place(12, 11, 0, asset::AirBlock);
    CHECK_FALSE(pond.voxels.fluidWakes.empty());
    pond.run(12);
    CHECK(pond.at(11, 11, 0) == asset::blockWithState(Water, 1));
    CHECK(pond.at(12, 11, 0) == asset::blockWithState(Water, 2));
    // And is still again, with nothing left to look at.
    CHECK(pond.voxels.fluidWakes.empty());
}

TEST_CASE("D448: a floor put under a pouring source makes it spread")
{
    // A block's decision reads what is under each of its sides -- a fluid
    // standing on something spreads, one pouring past does not -- and the
    // write that changed that woke nobody who read it.
    Pond pond;
    pond.place(0, 6, 0, Water);
    pond.run(20);
    // Pouring: a column under it, and nothing beside the source.
    REQUIRE(asset::blockTypeOf(pond.at(0, 5, 0)) == Water);
    REQUIRE(pond.at(1, 6, 0) == asset::AirBlock);

    pond.place(0, 5, 0, Stone);
    pond.run(20);
    CHECK(asset::blockTypeOf(pond.at(1, 6, 0)) == Water);
    CHECK(asset::blockTypeOf(pond.at(-1, 6, 0)) == Water);
    CHECK(pond.voxels.fluidWakes.empty());
}

TEST_CASE("D448: taking what is due costs the budget, not the queue")
{
    // Two hundred thousand positions waiting, and the step takes 8192 a tick:
    // each take copied and sorted all of them.
    scene::FluidWakes wakes;
    constexpr core::i32 Side = 450;
    for (core::i32 z = 0; z < Side; ++z) {
        for (core::i32 x = 0; x < Side; ++x)
            wakes.schedule(scene::FluidWakes::Position{x, 0, z}, (x + z) % 3 == 0 ? 5u : 0u);
    }
    const std::size_t total = wakes.size();
    REQUIRE(total == static_cast<std::size_t>(Side) * Side);

    // The oldest first, in position order among themselves, and no more than
    // asked for; what was not taken is still waiting.
    const std::vector<scene::FluidWakes::Position> first = wakes.takeDue(1, 1000);
    REQUIRE(first.size() == 1000);
    CHECK(std::is_sorted(first.begin(), first.end()));
    CHECK(wakes.size() == total - 1000);
    // Nothing due at 5 comes out at tick 1.
    for (const scene::FluidWakes::Position& at : first)
        CHECK((at[0] + at[2]) % 3 != 0);

    std::size_t taken = first.size();
    for (int tick = 0; tick < 20; ++tick)
        taken += wakes.takeDue(10, scene::MaxFluidUpdatesPerTick).size();
    CHECK(taken == 1000 + 20u * scene::MaxFluidUpdatesPerTick);
    CHECK(wakes.size() == total - taken);

    // **Twenty ticks' worth costs the same out of four times the queue.**
    // Copying and sorting the backlog each time, it cost four times as much;
    // taking the budget, about the same. A ratio and not a bound in
    // milliseconds: this ran in a tenth of a second alone and in three tenths
    // beside two other builds, and a clock on a busy machine says how busy it
    // is. The least of three runs each, since one can still be interrupted.
    const auto twentyTicks = [](core::i32 side) {
        double least = 1.0e9;
        for (int run = 0; run < 3; ++run) {
            scene::FluidWakes queue;
            for (core::i32 z = 0; z < side; ++z) {
                for (core::i32 x = 0; x < side; ++x)
                    queue.schedule(scene::FluidWakes::Position{x, 0, z}, 0u);
            }
            const auto began = std::chrono::steady_clock::now();
            std::size_t out = 0;
            for (int tick = 0; tick < 20; ++tick)
                out += queue.takeDue(10, scene::MaxFluidUpdatesPerTick).size();
            const double took =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            REQUIRE(out == 20u * scene::MaxFluidUpdatesPerTick);
            least = std::min(least, took);
        }
        return least;
    };
    const double ofTheQueue = twentyTicks(Side);
    const double ofFourTimesIt = twentyTicks(Side * 2);
    CAPTURE(ofTheQueue);
    CAPTURE(ofFourTimesIt);
    CHECK(ofFourTimesIt < 2.0 * ofTheQueue + 5.0);
}
