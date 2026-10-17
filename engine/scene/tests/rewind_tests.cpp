// Where what can be shot was (ADR 0235): what an authority keeps a tick at a
// time, how deep, and the tick a player's shot is rewound to.
#include <doctest/doctest.h>

#include "engine/scene/rewind.h"
#include "scene_fixture.h"

using namespace engine;
using namespace engine::scene;

namespace {

void place(testing::Fixture& fixture, core::InstanceId id, f64 x)
{
    fixture.world.parts().find(id)->cframe = core::CFrameD{core::DVec3{x, 0.0, 0.0}, core::Mat3{}};
}

} // namespace

TEST_CASE("ADR 0235: an authority keeps each tick's characters and compensated parts, as deep as the cap")
{
    testing::Fixture fixture;
    World& world = fixture.world;
    EngineState& state = world.engineState();
    const core::InstanceId seat = fixture.folder("Player");
    const core::InstanceId body = fixture.part("Body");
    const core::InstanceId arm = fixture.part("Arm");
    REQUIRE_FALSE(world.setParent(arm, body).has_value());
    const core::InstanceId dummy = fixture.part("Dummy");
    const core::InstanceId crate = fixture.part("Crate");
    PlayerComponent& player = world.players().add(seat, PlayerComponent{});
    player.character = body;
    (void)world.lagCompensated().add(dummy, LagCompensatedComponent{});

    // Alone, nothing is kept: nobody's shot is late.
    for (int tick = 0; tick < 3; ++tick) {
        state.tick += 1;
        recordRewind(world);
    }
    CHECK(state.rewindFrames.empty());

    // Hosting at sixty ticks a second with the cap at 200 ms: twelve ticks
    // back, and the one before the tick being run.
    state.networkTopology = NetworkTopology::Dedicated;
    for (int tick = 0; tick < 40; ++tick) {
        state.tick += 1;
        place(fixture, body, static_cast<f64>(state.tick));
        place(fixture, dummy, -static_cast<f64>(state.tick));
        recordRewind(world);
    }
    REQUIRE(state.rewindFrames.size() == 14);
    CHECK(state.rewindFrames.back().tick == state.tick);
    // The character, its arm and the dummy; not the crate.
    const EngineState::RewindFrame& newest = state.rewindFrames.back();
    REQUIRE(newest.entries.size() == 3);
    bool sawCrate = false;
    for (const EngineState::RewindEntry& entry : newest.entries)
        sawCrate = sawCrate || entry.id == crate;
    CHECK_FALSE(sawCrate);

    // A machine that has said nothing is not rewound.
    CHECK_FALSE(rewindTickFor(world, player).has_value());
    // One that draws five ticks behind is shown the world five ticks before
    // the newest, where the character was then.
    player.viewKnown = true;
    player.viewBehind = 5;
    state.tick += 1;
    const std::optional<u64> five = rewindTickFor(world, player);
    REQUIRE(five.has_value());
    CHECK(*five == state.tick - 1 - 5);
    const EngineState::RewindFrame* then = rewindFrameAt(world, *five);
    REQUIRE(then != nullptr);
    CHECK(then->tick == *five);
    for (const EngineState::RewindEntry& entry : then->entries) {
        if (entry.id == body)
            CHECK(entry.cframe.position.x == static_cast<f64>(*five));
    }
    // Forty ticks behind is rewound to the cap, twelve.
    player.viewBehind = 40;
    CHECK(*rewindTickFor(world, player) == state.tick - 1 - 12);
    // A cap of nought keeps nothing and rewinds nothing.
    state.rewindCapMs = 0.0;
    recordRewind(world);
    CHECK(state.rewindFrames.empty());
    CHECK(rewindFrameAt(world, state.tick - 1) == nullptr);
}
