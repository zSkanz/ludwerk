// A crowd on open ground that the engine steers (ADR 0156): walking at the
// target, pushed apart, climbing the one in its way and standing on it, round
// an obstacle, thinking less often far away -- and placing its bodies.
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/scene/swarm.h"
#include "scene_fixture.h"

using namespace engine;
using namespace engine::scene;

namespace {

struct Rig
{
    testing::Fixture fixture;
    core::InstanceId holder;

    Rig()
    {
        holder = fixture.folder("Horde");
        fixture.world.swarms().add(holder, SwarmComponent{});
    }

    [[nodiscard]] SwarmComponent& swarm() { return *fixture.world.swarms().find(holder); }

    u32 agent(core::DVec3 at, SwarmAgentSettings settings = {})
    {
        const core::InstanceId body = fixture.part("Body");
        fixture.world.parts().find(body)->cframe = core::CFrameD{at, core::Mat3{}};
        return addSwarmAgent(swarm(), body, at, settings);
    }

    void ticks(int count)
    {
        for (int tick = 0; tick < count; ++tick) {
            fixture.world.engineState().tick += 1;
            stepSwarms(fixture.world, nullptr, 1.0 / 60.0);
        }
    }

    [[nodiscard]] const SwarmAgent& at(u32 agent) { return *swarmAgent(swarm(), agent); }
};

[[nodiscard]] f64 flat(const core::DVec3& a, const core::DVec3& b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST_CASE("ADR 0156: an agent walks at the target, stops short of it, and places its body facing it")
{
    Rig rig;
    rig.swarm().target = core::DVec3{10.0, 0.0, 0.0};
    const u32 walker = rig.agent(core::DVec3{0.0, 0.0, 0.0});
    rig.ticks(60);
    // Four metres a second for a second.
    CHECK(rig.at(walker).position.x == doctest::Approx(4.0).epsilon(0.01));
    rig.ticks(240);
    // Its radius and the stop distance short of it, and no further.
    CHECK(rig.at(walker).position.x == doctest::Approx(10.0 - 0.5 - 0.55).epsilon(0.02));

    const PartComponent* body = rig.fixture.world.parts().find(rig.at(walker).body);
    REQUIRE(body != nullptr);
    CHECK(body->cframe.position.x == doctest::Approx(rig.at(walker).position.x));
    // Facing the target: its look direction (-Z) along +X.
    const core::Vec3 look = body->cframe.rotation * core::Vec3{0.0f, 0.0f, -1.0f};
    CHECK(static_cast<double>(look.x) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("ADR 0156: two agents in one place are pushed apart; a floating one is not pushed")
{
    Rig rig;
    rig.swarm().target = core::DVec3{0.0, 0.0, 100.0};
    const u32 a = rig.agent(core::DVec3{0.0, 0.0, 0.0});
    const u32 b = rig.agent(core::DVec3{0.2, 0.0, 0.0});
    rig.ticks(30);
    // Apart, nearing their two radii: the push is by how far they overlap.
    CHECK(flat(rig.at(a).position, rig.at(b).position) > 0.8);

    SwarmAgentSettings floating;
    floating.floats = true;
    const u32 c = rig.agent(core::DVec3{50.0, 0.0, 0.0}, floating);
    const u32 d = rig.agent(core::DVec3{50.0, 0.0, 0.0}, floating);
    rig.ticks(30);
    CHECK(flat(rig.at(c).position, rig.at(d).position) < 0.01);
    CHECK(rig.at(c).position.y == doctest::Approx(1.0));
}

TEST_CASE("ADR 0156: blocked by one that is held, an agent climbs it and stands on it")
{
    // The target where the held one is -- the player a tower forms under --
    // so the climber, once up, has nowhere further to go.
    Rig rig;
    rig.swarm().target = core::DVec3{2.0, 0.0, 0.0};
    rig.swarm().stopDistance = 0.0f;
    SwarmAgentSettings held;
    held.speed = 0.0f;
    held.height = 1.2f;
    const u32 front = rig.agent(core::DVec3{2.0, 0.0, 0.0}, held);
    const u32 behind = rig.agent(core::DVec3{0.0, 0.0, 0.0});
    rig.ticks(240);
    // On top of the one in front, which is still on the ground.
    CHECK(rig.at(front).position.y == doctest::Approx(0.0));
    CHECK(rig.at(behind).position.y >= 1.2 - 1.0e-6);

    SUBCASE("one that does not climb stays on the ground")
    {
        Rig flat;
        flat.swarm().target = core::DVec3{20.0, 0.0, 0.0};
        (void)flat.agent(core::DVec3{2.0, 0.0, 0.0}, held);
        SwarmAgentSettings grounded;
        grounded.climbs = false;
        const u32 walker = flat.agent(core::DVec3{0.0, 0.0, 0.0}, grounded);
        flat.ticks(240);
        CHECK(flat.at(walker).position.y == doctest::Approx(0.0));
    }
}

TEST_CASE("ADR 0156: an agent goes round an obstacle rather than through it")
{
    Rig rig;
    rig.swarm().target = core::DVec3{20.0, 0.0, 0.3};
    rig.swarm().obstacles.push_back(SwarmObstacle{10.0, 0.0, 2.0f});
    const u32 walker = rig.agent(core::DVec3{0.0, 0.0, 0.0});
    f64 closest = 1.0e9;
    for (int tick = 0; tick < 600; ++tick) {
        rig.ticks(1);
        closest = std::min(closest, flat(rig.at(walker).position, core::DVec3{10.0, 0.0, 0.0}));
    }
    // Never deeper than a little of its radius into the circle.
    CHECK(closest > 2.0);
    CHECK(rig.at(walker).position.x > 15.0);
}

TEST_CASE("ADR 0156: far away it thinks every fourth tick, and covers the same ground")
{
    Rig rig;
    rig.swarm().target = core::DVec3{0.0, 0.0, 0.0};
    const u32 far = rig.agent(core::DVec3{100.0, 0.0, 0.0});
    const u32 near = rig.agent(core::DVec3{10.0, 0.0, 0.0});
    std::vector<f64> steps;
    f64 last = rig.at(far).position.x;
    int moved = 0;
    for (int tick = 0; tick < 40; ++tick) {
        rig.ticks(1);
        if (rig.at(far).position.x != last)
            ++moved;
        last = rig.at(far).position.x;
    }
    CHECK(moved == 10);
    // Forty ticks at four metres a second, either way.
    CHECK(rig.at(far).position.x == doctest::Approx(100.0 - 40.0 / 60.0 * 4.0).epsilon(0.001));
    CHECK(rig.at(near).position.x == doctest::Approx(10.0 - 40.0 / 60.0 * 4.0).epsilon(0.001));
}

TEST_CASE("ADR 0156: agents are numbered by slot, found by radius, and a removed one's number is reused")
{
    Rig rig;
    const u32 a = rig.agent(core::DVec3{0.0, 0.0, 0.0});
    const u32 b = rig.agent(core::DVec3{5.0, 0.0, 0.0});
    const u32 c = rig.agent(core::DVec3{1.0, 0.0, 0.0});
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
    std::vector<u32> found;
    querySwarmRadius(rig.swarm(), core::DVec3{0.0, 0.0, 0.0}, 2.0, false, found);
    CHECK(found == std::vector<u32>{1, 3});

    CHECK(removeSwarmAgent(rig.swarm(), b));
    CHECK(swarmAgent(rig.swarm(), b) == nullptr);
    CHECK_FALSE(removeSwarmAgent(rig.swarm(), b));
    CHECK(rig.agent(core::DVec3{9.0, 0.0, 0.0}) == 2);
}

TEST_CASE("H10: a radius query through the grid finds what a walk of every agent finds, flat or not")
{
    Rig rig;
    rig.swarm().target = core::DVec3{0.0, 0.0, 0.0};
    for (int index = 0; index < 200; ++index)
        (void)rig.agent(core::DVec3{static_cast<f64>(index % 20) * 0.9 - 9.0, static_cast<f64>(index % 3) * 1.5,
                                    static_cast<f64>(index / 20) * 0.9 - 4.5});
    rig.ticks(30);

    const auto every = [&](core::DVec3 centre, f64 radius, bool flat) {
        std::vector<u32> out;
        for (usize slot = 0; slot < rig.swarm().agents.size(); ++slot) {
            const SwarmAgent& agent = rig.swarm().agents[slot];
            const f64 dx = agent.position.x - centre.x;
            const f64 dy = flat ? 0.0 : agent.position.y - centre.y;
            const f64 dz = agent.position.z - centre.z;
            if (agent.alive && dx * dx + dy * dy + dz * dz <= radius * radius)
                out.push_back(static_cast<u32>(slot) + 1);
        }
        return out;
    };
    for (const f64 radius : {0.5, 2.0, 3.7, 40.0}) {
        for (const bool flat : {false, true}) {
            std::vector<u32> found;
            querySwarmRadius(rig.swarm(), core::DVec3{1.3, 0.0, -0.4}, radius, flat, found);
            CHECK(found == every(core::DVec3{1.3, 0.0, -0.4}, radius, flat));
        }
    }
    // A tower over the circle: flat finds the agents high up it, round does not.
    std::vector<u32> flat;
    std::vector<u32> round;
    querySwarmRadius(rig.swarm(), core::DVec3{0.0, 0.0, 0.0}, 1.0, true, flat);
    querySwarmRadius(rig.swarm(), core::DVec3{0.0, 0.0, 0.0}, 1.0, false, round);
    CHECK(flat.size() >= round.size());

    // An agent placed elsewhere since the step is found where it is now.
    const u32 moved = rig.agent(core::DVec3{50.0, 0.0, 50.0});
    std::vector<u32> there;
    querySwarmRadius(rig.swarm(), core::DVec3{50.0, 0.0, 50.0}, 0.5, false, there);
    CHECK(there == std::vector<u32>{moved});
}

TEST_CASE("ADR 0156: two runs of one crowd end in the same place")
{
    const auto run = [] {
        Rig rig;
        rig.swarm().target = core::DVec3{0.0, 0.0, 0.0};
        for (int index = 0; index < 60; ++index)
            (void)rig.agent(
                core::DVec3{static_cast<f64>(index % 10) * 0.7 + 8.0, 0.0, static_cast<f64>(index / 10) * 0.7});
        rig.ticks(300);
        std::vector<f64> out;
        for (const SwarmAgent& agent : rig.swarm().agents) {
            out.push_back(agent.position.x);
            out.push_back(agent.position.y);
            out.push_back(agent.position.z);
        }
        return out;
    };
    CHECK(run() == run());
}
