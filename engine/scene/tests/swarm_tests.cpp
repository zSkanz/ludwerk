// A crowd on open ground that the engine steers (ADR 0156): walking at the
// target, pushed apart, climbing the one in its way and standing on it, round
// an obstacle, thinking less often far away -- and placing its bodies.
#include <cmath>
#include <doctest/doctest.h>
#include <utility>
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

TEST_CASE("Swarm.PileHeight: a crowd round what stands still piles as high as it is let, and no higher")
{
    // A hundred and fifty agents closing on one point from all round it: what
    // a horde does to a hero who stands still. With no limit they climb each
    // other for as long as there is one in the way -- a tower over the point.
    const auto pile = [](f32 pileHeight) {
        Rig rig;
        rig.swarm().target = core::DVec3{0.0, 0.0, 0.0};
        rig.swarm().stopDistance = 0.0f;
        rig.swarm().pileHeight = pileHeight;
        SwarmAgentSettings held;
        held.speed = 0.0f;
        (void)rig.agent(core::DVec3{0.0, 0.0, 0.0}, held);
        for (int index = 0; index < 150; ++index) {
            const f64 turn = static_cast<f64>(index) * 2.399963;
            const f64 far = 3.0 + static_cast<f64>(index) * 0.06;
            (void)rig.agent(core::DVec3{std::cos(turn) * far, 0.0, std::sin(turn) * far});
        }
        rig.ticks(900);
        // Over the fifteen seconds' last second -- a limit that is held, not
        // passed through: the highest any of them STANDS (at rest on the
        // ground or on another), and the highest any is at all, which is
        // more by the hop an agent makes as it crests the one it climbed.
        std::pair<f64, f64> highest{0.0, 0.0};
        std::vector<f64> before;
        for (const SwarmAgent& agent : rig.swarm().agents)
            before.push_back(agent.position.y);
        std::vector<int> still(before.size(), 0);
        for (int tick = 0; tick < 60; ++tick) {
            rig.ticks(1);
            for (usize slot = 0; slot < rig.swarm().agents.size(); ++slot) {
                const SwarmAgent& agent = rig.swarm().agents[slot];
                if (!agent.alive)
                    continue;
                // At rest: where it was for three ticks running. One tick is
                // also the top of a hop, where it hangs for an instant.
                still[slot] = std::fabs(agent.position.y - before[slot]) < 1.0e-3 ? still[slot] + 1 : 0;
                if (still[slot] >= 3)
                    highest.first = std::max(highest.first, agent.position.y);
                highest.second = std::max(highest.second, agent.position.y);
                before[slot] = agent.position.y;
            }
        }
        return highest;
    };

    const std::pair<f64, f64> unlimited = pile(0.0f);
    const std::pair<f64, f64> limited = pile(2.0f);
    CAPTURE(unlimited.first);
    CAPTURE(limited.first);
    CAPTURE(limited.second);
    // No limit: a tower -- agents a metre tall, standing five and more deep.
    CHECK(unlimited.first > 5.0);
    // Two metres: nobody stands on a top past two metres over the ground,
    // so the highest feet at rest are at two, on the second one up.
    CHECK(limited.first <= 2.0 + 1.0e-3);
    // Cresting, an agent hops as it always did: a little over half a metre.
    CHECK(limited.second <= 2.0 + 0.7);
    // And it is still a pile: they do climb what is under the limit.
    CHECK(limited.first > 0.9);
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

TEST_CASE("ADR 0156 amended: with several targets each agent walks at the nearest, and thinks as often as it is near")
{
    Rig rig;
    rig.swarm().target = core::DVec3{0.0, 0.0, 100.0};
    rig.swarm().targets = {core::DVec3{-20.0, 0.0, 0.0}, core::DVec3{20.0, 0.0, 0.0}};
    const u32 west = rig.agent(core::DVec3{-5.0, 0.0, 0.0});
    const u32 east = rig.agent(core::DVec3{5.0, 0.0, 0.0});
    rig.ticks(60);
    // Each a metre a quarter-second towards its own -- not at `target`.
    CHECK(rig.at(west).position.x == doctest::Approx(-9.0).epsilon(0.01));
    CHECK(rig.at(east).position.x == doctest::Approx(9.0).epsilon(0.01));
    CHECK(std::abs(rig.at(west).position.z) < 1e-9);

    // Near one of them, an agent far from the other thinks every tick.
    Rig far;
    far.swarm().target = core::DVec3{0.0, 0.0, 0.0};
    far.swarm().targets = {core::DVec3{0.0, 0.0, 0.0}, core::DVec3{200.0, 0.0, 0.0}};
    const u32 walker = far.agent(core::DVec3{190.0, 0.0, 0.0});
    far.ticks(1);
    // One tick's walk, not none and not four ticks' at once.
    CHECK(far.at(walker).position.x == doctest::Approx(190.0 + 4.0 / 60.0).epsilon(1e-6));

    // None given again: `target` alone.
    rig.swarm().targets.clear();
    const core::DVec3 before = rig.at(east).position;
    rig.ticks(30);
    CHECK(rig.at(east).position.z > before.z);
}

TEST_CASE("ADR 0156 amended: an agent with no body is stepped like any other, and found where it went")
{
    // The same walk from the same place, once with a body and once without.
    Rig rig;
    rig.swarm().target = core::DVec3{10.0, 0.0, 0.0};
    const u32 bare = addSwarmAgent(rig.swarm(), core::InstanceId{}, core::DVec3{0.0, 0.0, 0.0}, {});
    Rig other;
    other.swarm().target = core::DVec3{10.0, 0.0, 0.0};
    const u32 bodied = other.agent(core::DVec3{0.0, 0.0, 0.0});
    rig.ticks(60);
    other.ticks(60);
    CHECK(rig.at(bare).position.x == doctest::Approx(4.0).epsilon(0.01));
    CHECK(rig.at(bare).position.x == other.at(bodied).position.x);
    CHECK(rig.at(bare).position.z == other.at(bodied).position.z);
    CHECK_FALSE(rig.at(bare).body.valid());
}

TEST_CASE("ADR 0162: a swarm keeps what became of its agents -- added, tagged, removed with its last tag")
{
    Rig rig;
    SwarmAgentSettings settings;
    settings.radius = 0.75f;
    settings.height = 2.0f;
    const u32 first = addSwarmAgent(rig.swarm(), core::InstanceId{}, core::DVec3{1.0, 0.0, 2.0}, settings);
    REQUIRE(setSwarmAgentTag(rig.swarm(), first, 7));
    // The same tag again is nothing new.
    REQUIRE(setSwarmAgentTag(rig.swarm(), first, 7));
    REQUIRE(setSwarmAgentTag(rig.swarm(), first, 0x8007));
    REQUIRE(removeSwarmAgent(rig.swarm(), first, 5));
    CHECK_FALSE(setSwarmAgentTag(rig.swarm(), first, 1));

    const std::vector<SwarmEvent>& events = rig.swarm().events;
    REQUIRE(events.size() == 4);
    CHECK(events[0].kind == SwarmEvent::Kind::Added);
    CHECK(events[0].agent == first);
    CHECK(events[0].radius == 0.75f);
    CHECK(events[0].height == 2.0f);
    CHECK(events[1].kind == SwarmEvent::Kind::TagChanged);
    CHECK(events[1].tag == 7);
    CHECK(events[2].tag == 0x8007);
    CHECK(events[3].kind == SwarmEvent::Kind::Removed);
    CHECK(events[3].reason == SwarmRemovalRemoved);
    CHECK(events[3].tag == 0x8007);
    CHECK(events[3].position.x == 1.0);
    // Only a swarm that replicates keeps the going for the replicas.
    CHECK(rig.swarm().removed.empty());

    rig.swarm().replicates = true;
    const u32 second = addSwarmAgent(rig.swarm(), core::InstanceId{}, core::DVec3{}, settings);
    // The number is used again, and the row is another agent.
    CHECK(second == first);
    const u32 born = rig.at(second).born;
    REQUIRE(removeSwarmAgent(rig.swarm(), second, 6));
    REQUIRE(rig.swarm().removed.size() == 1);
    CHECK(rig.swarm().removed[0].slot == second - 1);
    CHECK(rig.swarm().removed[0].born == born);
}

TEST_CASE("ADR 0162: a replica's row is carried along its walk on a clock of its own, and a correction is taken up")
{
    Rig rig;
    SwarmComponent& swarm = rig.swarm();
    swarm.mirrored = true;
    // Told at the authority's tick 100: at the origin, walking four metres a
    // second along -z, which is what a yaw of nothing faces.
    SwarmTold told;
    told.position = core::DVec3{0.0, 0.0, 0.0};
    told.walk = 4.0f;
    mirrorSwarmAgentAdded(swarm, 3, 9, 0.5f, 1.5f, told, 100.0);
    REQUIRE(swarmAgent(swarm, 4) != nullptr);
    CHECK(rig.at(4).tag == 9);
    REQUIRE(swarm.events.size() == 1);
    CHECK(swarm.events[0].kind == SwarmEvent::Kind::Added);

    // **Sixty ticks with nothing said**: it has walked four metres. The clock
    // runs on with no message to set it against.
    rig.ticks(61);
    CHECK(rig.at(4).position.z == doctest::Approx(-4.0).epsilon(0.02));
    CHECK(rig.at(4).position.x == doctest::Approx(0.0));

    // Told again, a metre to the side of where it is drawn: it does not jump.
    const core::DVec3 before = rig.at(4).position;
    SwarmTold moved;
    moved.position = core::DVec3{1.0, 0.0, before.z};
    moved.walk = 4.0f;
    REQUIRE(mirrorSwarmAgentTold(swarm, 3, moved, 160.0, 1.0 / 60.0, std::optional<u16>{12}));
    rig.ticks(1);
    CHECK(rig.at(4).position.x < 0.3);
    CHECK(rig.at(4).position.x > 0.0);
    // And a second later it is where it was told, carried on.
    rig.ticks(60);
    CHECK(rig.at(4).position.x == doctest::Approx(1.0).epsilon(0.01));
    CHECK(rig.at(4).tag == 12);
    CHECK(swarm.events.back().kind == SwarmEvent::Kind::TagChanged);

    // An older word -- a message that arrived late -- changes nothing.
    SwarmTold stale;
    stale.position = core::DVec3{50.0, 0.0, 50.0};
    CHECK_FALSE(mirrorSwarmAgentTold(swarm, 3, stale, 120.0, 1.0 / 60.0, std::nullopt));
    // Nor one for a number nobody has.
    CHECK_FALSE(mirrorSwarmAgentTold(swarm, 9, stale, 500.0, 1.0 / 60.0, std::nullopt));

    // Gone, with what it was: the script that draws it hears where and why.
    mirrorSwarmAgentRemoved(swarm, 3, SwarmRemovalOutOfReach, 12, core::DVec3{1.0, 0.0, -9.0});
    CHECK(swarmAgent(swarm, 4) == nullptr);
    CHECK(swarm.events.back().kind == SwarmEvent::Kind::Removed);
    CHECK(swarm.events.back().reason == SwarmRemovalOutOfReach);
    CHECK(swarm.events.back().tag == 12);
}

TEST_CASE("ADR 0162: a replica stands an agent on the floor it was told, a lift above it, and moves its body")
{
    Rig rig;
    SwarmComponent& swarm = rig.swarm();
    swarm.mirrored = true;
    const core::InstanceId body = rig.fixture.part("Body");
    SwarmTold told;
    told.position = core::DVec3{2.0, 21.6, 3.0};
    told.floor = 20.0;
    told.lift = 1.6f;
    told.yaw = 1.0f;
    mirrorSwarmAgentAdded(swarm, 0, 0, 0.5f, 1.6f, told, 10.0);
    swarm.agents[0].body = body;
    rig.ticks(2);
    // No terrain here and no ray: the floor, and what it stands on.
    CHECK(rig.at(1).position.y == doctest::Approx(21.6).epsilon(0.001));
    const PartComponent* part = rig.fixture.world.parts().find(body);
    CHECK(part->cframe.position.y == doctest::Approx(21.6).epsilon(0.001));
    CHECK(part->cframe.position.x == doctest::Approx(2.0));

    // It came down: the height is taken up over a few ticks, not at once.
    SwarmTold lower;
    lower.position = core::DVec3{2.0, 0.0, 3.0};
    lower.floor = 20.0;
    lower.lift = 0.0f;
    lower.yaw = 1.0f;
    REQUIRE(mirrorSwarmAgentTold(swarm, 0, lower, 20.0, 1.0 / 60.0, std::nullopt));
    rig.ticks(1);
    CHECK(rig.at(1).position.y < 21.6);
    CHECK(rig.at(1).position.y > 20.5);
    rig.ticks(60);
    CHECK(rig.at(1).position.y == doctest::Approx(20.0).epsilon(0.001));
}
