#include "engine/scene/swarm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "engine/asset/terrain.h"
#include "engine/physics/types.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/world.h"

namespace engine::scene {

namespace {

// The cells round an agent, its own first: what it looks through for
// neighbours, in a fixed order so a packed crowd's capped look is the same
// look every run.
constexpr std::array<std::array<i32, 2>, 9> Around{{
    {0, 0},
    {1, 0},
    {-1, 0},
    {0, 1},
    {0, -1},
    {1, 1},
    {-1, -1},
    {1, -1},
    {-1, 1},
}};
// The obstacles' cells, in metres: wider than the crowd's, because an obstacle
// is fixed and a tree is wider than an enemy.
constexpr f64 ObstacleCell = 4.0;
// How hard neighbours push apart, and how hard an obstacle pushes, per metre of
// overlap a second; and how fast a knock-back dies away, a second.
constexpr f64 Separation = 6.0;
constexpr f64 ObstaclePush = 2.0;
constexpr f64 KnockDecay = 6.0;
// An agent standing on another: its feet within this of the other's top.
constexpr f64 StandSlack = 0.25;
// The share of the reach within which an agent is ON another rather than
// beside it.
constexpr f64 OnTopReach = 0.75;
// A neighbour is in the way when it is this far towards the target, as a
// share of the distance between them.
constexpr f64 InTheWay = 0.3;
// How much of a step's walk an agent's remembered walk takes up (ADR 0162).
constexpr f64 WalkSmoothing = 0.15;
// How far the ground is searched for under an agent, up and down, where no
// terrain covers it.
constexpr f64 GroundReach = 64.0;

[[nodiscard]] i64 cellKey(i64 x, i64 z) noexcept
{
    return (x + (i64{1} << 30)) * (i64{1} << 31) + (z + (i64{1} << 30));
}

[[nodiscard]] i64 cellOf(f64 value, f64 size) noexcept
{
    return static_cast<i64>(std::floor(value / size));
}

// The ground's height under (x, z): the highest terrain over it, or what a
// ray down finds, or zero.
[[nodiscard]] f64 groundAt(const World& world, const PhysicsSync* physics, core::InstanceId body, f64 x, f64 z,
                           f64 from)
{
    if (const std::optional<f64> terrain = swarmTerrainAt(world, x, z); terrain.has_value())
        return *terrain;
    if (physics == nullptr)
        return 0.0;

    const std::array<u64, 1> excluded{physics->userDataOf(body)};
    physics::QueryFilter filter;
    filter.mode = physics::QueryFilter::Mode::Exclude;
    filter.userData = std::span<const u64>{excluded.data(), body.valid() ? 1u : 0u};
    const physics::RayD ray{core::DVec3{x, from + GroundReach * 0.5, z},
                            core::Vec3{0.0f, -static_cast<f32>(GroundReach * 1.5), 0.0f}};
    physics::RayHit hit;
    if (physics->backend().raycast(physics->worldHandle(), ray, filter, hit))
        return hit.position.y;
    return 0.0;
}

// **The grid**: the first agent in a cell, and the next in the same one --
// prepended, as the horde's own grid was, so a cell is walked newest slot
// first.
void buildGrid(SwarmComponent& swarm)
{
    const f64 cell = std::max(static_cast<f64>(swarm.cellSize), 0.01);
    const usize count = swarm.agents.size();
    swarm.grid.clear();
    swarm.grid.reserve(count);
    swarm.gridNext.assign(count, 0);
    for (usize slot = 0; slot < count; ++slot) {
        const SwarmAgent& agent = swarm.agents[slot];
        if (!agent.alive)
            continue;
        const i64 key = cellKey(cellOf(agent.position.x, cell), cellOf(agent.position.z, cell));
        auto [at, inserted] = swarm.grid.try_emplace(key, 0u);
        swarm.gridNext[slot] = at->second;
        at->second = static_cast<u32>(slot) + 1;
    }
    swarm.gridCell = swarm.cellSize;
    swarm.gridValid = true;
}

void stepSwarm(World& world, const PhysicsSync* physics, SwarmComponent& swarm, f64 dt, u64 tick,
               core::NameAtom cframeName)
{
    const f64 cell = std::max(static_cast<f64>(swarm.cellSize), 0.01);
    const usize count = swarm.agents.size();

    // Where the last step left everyone, unless something moved an agent since.
    if (!swarm.gridValid || swarm.gridCell != swarm.cellSize)
        buildGrid(swarm);
    const std::unordered_map<i64, u32>& head = swarm.grid;
    const std::vector<u32>& next = swarm.gridNext;

    // The obstacles, by cell -- each in every cell its circle touches, with a
    // metre to spare for the widest agent.
    std::unordered_map<i64, std::vector<u32>> obstacleCells;
    for (usize index = 0; index < swarm.obstacles.size(); ++index) {
        const SwarmObstacle& obstacle = swarm.obstacles[index];
        const f64 reach = static_cast<f64>(obstacle.radius) + 1.0;
        for (i64 x = cellOf(obstacle.x - reach, ObstacleCell); x <= cellOf(obstacle.x + reach, ObstacleCell); ++x) {
            for (i64 z = cellOf(obstacle.z - reach, ObstacleCell); z <= cellOf(obstacle.z + reach, ObstacleCell); ++z)
                obstacleCells[cellKey(x, z)].push_back(static_cast<u32>(index));
        }
    }

    for (usize slot = 0; slot < count; ++slot) {
        SwarmAgent& agent = swarm.agents[slot];
        if (!agent.alive)
            continue;
        const f64 x = agent.position.x;
        const f64 y = agent.position.y;
        const f64 z = agent.position.z;
        // **Its target: the nearest** of `targets`, the first of two as near
        // (ADR 0156, amended) -- or `target`, with none.
        f64 tx = swarm.target.x;
        f64 tz = swarm.target.z;
        if (!swarm.targets.empty()) {
            f64 best = 1.0e300;
            for (const core::DVec3& candidate : swarm.targets) {
                const f64 cx = candidate.x - x;
                const f64 cz = candidate.z - z;
                const f64 near = cx * cx + cz * cz;
                if (near < best) {
                    best = near;
                    tx = candidate.x;
                    tz = candidate.z;
                }
            }
        }
        const f64 dx = tx - x;
        const f64 dz = tz - z;
        const f64 distance = std::sqrt(dx * dx + dz * dz);

        // **Far away, it thinks less often**, with the time it skipped --
        // staggered by slot, so the far crowd's work is spread over the ticks.
        const u64 interval = distance > static_cast<f64>(swarm.farDistance)    ? 4u
                             : distance > static_cast<f64>(swarm.nearDistance) ? 2u
                                                                               : 1u;
        if ((tick + slot) % interval != 0)
            continue;
        const f64 step = dt * static_cast<f64>(interval);

        f64 dirX = 0.0;
        f64 dirZ = 0.0;
        if (distance > 0.01) {
            dirX = dx / distance;
            dirZ = dz / distance;
        }
        const f64 r = static_cast<f64>(agent.radius);
        const f64 h = static_cast<f64>(agent.height);
        f64 pushX = 0.0;
        f64 pushZ = 0.0;
        f64 support = -1.0e30;
        bool blocked = false;
        f64 climbTo = -1.0e30;

        if (!agent.floats) {
            const i64 cx = cellOf(x, cell);
            const i64 cz = cellOf(z, cell);
            u32 seen = 0;
            for (const auto& offset : Around) {
                if (seen >= swarm.maxNeighbours)
                    break;
                const auto found = head.find(cellKey(cx + offset[0], cz + offset[1]));
                u32 other = found == head.end() ? 0u : found->second;
                while (other != 0) {
                    ++seen;
                    if (seen > swarm.maxNeighbours)
                        break;
                    const usize index = other - 1;
                    const SwarmAgent& them = swarm.agents[index];
                    if (index != slot && them.alive && !them.floats) {
                        const f64 ax = x - them.position.x;
                        const f64 az = z - them.position.z;
                        const f64 reach = r + static_cast<f64>(them.radius);
                        const f64 d2 = ax * ax + az * az;
                        if (d2 < reach * reach) {
                            const f64 top = them.position.y + static_cast<f64>(them.height);
                            if (y >= top - StandSlack) {
                                // Standing on it.
                                if (d2 < (reach * OnTopReach) * (reach * OnTopReach) && top > support)
                                    support = top;
                            }
                            else if (them.position.y < y + h && y < top) {
                                // Side by side: apart.
                                const f64 d = std::sqrt(d2) + 0.0001;
                                const f64 push = (reach - d) / d;
                                pushX += ax * push;
                                pushZ += az * push;
                                // In the way, between it and the target: climb.
                                if (dirX * -ax + dirZ * -az > InTheWay * d) {
                                    blocked = true;
                                    climbTo = std::max(climbTo, top);
                                }
                            }
                        }
                    }
                    other = next[index];
                }
            }

            // Round the obstacles.
            if (const auto stones = obstacleCells.find(cellKey(cellOf(x, ObstacleCell), cellOf(z, ObstacleCell)));
                stones != obstacleCells.end()) {
                for (const u32 index : stones->second) {
                    const SwarmObstacle& stone = swarm.obstacles[index];
                    const f64 ax = x - stone.x;
                    const f64 az = z - stone.z;
                    const f64 reach = r + static_cast<f64>(stone.radius);
                    const f64 d2 = ax * ax + az * az;
                    if (d2 < reach * reach) {
                        const f64 d = std::sqrt(d2) + 0.0001;
                        pushX += ax / d * (reach - d) * ObstaclePush;
                        pushZ += az / d * (reach - d) * ObstaclePush;
                    }
                }
            }
        }

        // Close enough: it stops walking at the target, still pushed.
        const f64 stop = r + static_cast<f64>(swarm.stopDistance);
        const f64 walk = distance > stop ? static_cast<f64>(agent.speed) : 0.0;
        const f64 nx = x + (dirX * walk + pushX * Separation + static_cast<f64>(agent.pushX)) * step;
        const f64 nz = z + (dirZ * walk + pushZ * Separation + static_cast<f64>(agent.pushZ)) * step;
        const f64 decay = std::max(1.0 - KnockDecay * step, 0.0);
        agent.pushX = static_cast<f32>(static_cast<f64>(agent.pushX) * decay);
        agent.pushZ = static_cast<f32>(static_cast<f64>(agent.pushZ) * decay);

        // The ground, found again only when it has moved.
        if (std::fabs(nx - agent.groundX) + std::fabs(nz - agent.groundZ) > 0.1) {
            agent.ground = groundAt(world, physics, agent.body, nx, nz, y);
            agent.groundX = nx;
            agent.groundZ = nz;
        }
        const f64 ground = agent.ground;

        f64 ny = y;
        f64 vy = static_cast<f64>(agent.verticalSpeed);
        if (agent.floats) {
            ny = ground + static_cast<f64>(agent.floatHeight);
            vy = 0.0;
        }
        else {
            if (agent.climbs && blocked && climbTo > y && walk > 0.0)
                vy = static_cast<f64>(swarm.climbSpeed);
            else
                vy -= static_cast<f64>(swarm.gravity) * step;
            ny = y + vy * step;
            const f64 floor = std::max(ground, support);
            if (ny <= floor) {
                ny = floor;
                vy = 0.0;
            }
        }
        // **What a replica carries it forward by** (ADR 0162): the way it
        // faces, and how much of this step's move was that way. What pushed it
        // sideways is not in it; a replica that guesses wrong is told again.
        //
        // **Smoothed over a few steps**: in a packed crowd an agent is blocked
        // one tick and shoved the next, and a replica carried forward by
        // whichever of those it was last told is wrong within a tenth of a
        // second -- measured, every agent wanted telling twenty times a second.
        agent.yaw = static_cast<f32>(std::atan2(-dirX, -dirZ));
        agent.faceX = static_cast<f32>(dirX);
        agent.faceZ = static_cast<f32>(dirZ);
        const f64 walked = std::max(((nx - x) * dirX + (nz - z) * dirZ) / step, 0.0);
        agent.walk += static_cast<f32>((walked - static_cast<f64>(agent.walk)) * WalkSmoothing);
        agent.stepped = tick;
        agent.lift = static_cast<f32>(ny - ground);
        agent.position = core::DVec3{nx, ny, nz};
        agent.verticalSpeed = static_cast<f32>(vy);

        // **Placed facing the target**: the body's whole transform, written as
        // a script writes one.
        if (agent.body.valid() && world.alive(agent.body) && world.parts().find(agent.body) != nullptr) {
            (void)world.setProperty(agent.body, cframeName,
                                    Value{core::CFrameD{agent.position, core::rotationY(agent.yaw)}});
        }
    }
    // Where everyone is now: what a script asks about until the next step.
    buildGrid(swarm);
}

// The direction `yaw` faces along the ground, into an agent's row.
void faceBy(SwarmAgent& agent, f32 yaw) noexcept
{
    agent.yaw = yaw;
    agent.faceX = -std::sin(yaw);
    agent.faceZ = -std::cos(yaw);
}

// How much of a correction is left after a tick: a tenth of a second to take
// most of it up.
constexpr f32 EaseKept = 0.85f;
// How far the clock a replica draws by may trail the newest tick it was told
// before it jumps to it, and how fast it closes a smaller gap.
constexpr f64 ClockSnapTicks = 8.0;
constexpr f64 ClockSlew = 0.25;

// Where a replica's row is at the swarm's clock: what it was told, carried
// along its walk, without the height.
[[nodiscard]] core::DVec3 carried(const SwarmAgent& agent, f64 clock, f64 dt) noexcept
{
    const f64 ticks = std::clamp(clock - agent.toldTick, 0.0, SwarmCarryTicks);
    const f64 metres = static_cast<f64>(agent.walk) * ticks * dt;
    return agent.told +
           core::DVec3{static_cast<f64>(agent.faceX) * metres, 0.0, static_cast<f64>(agent.faceZ) * metres};
}

// **A replica's tick of the authority's swarm** (ADR 0162): nothing is
// simulated. The clock follows the newest tick a message said, each agent is
// where it was told carried forward to that clock, on the ground this machine
// has, and its body -- this machine's own -- is placed there.
void stepMirror(World& world, SwarmComponent& swarm, f64 dt, core::NameAtom cframeName)
{
    if (!swarm.mirrorClockSet) {
        swarm.mirrorClock = swarm.mirrorNewest;
        swarm.mirrorClockSet = true;
    }
    else {
        // **It runs on its own, a tick a tick, and is set against a message
        // only when one arrives.** Pulled towards the newest tick every tick,
        // it stood still whenever nothing arrived -- and nothing arrives for
        // an agent walking a straight line, which is the one case a replica
        // carries forward for seconds: measured, eleven metres behind.
        swarm.mirrorClock += 1.0;
        if (swarm.mirrorHeard) {
            const f64 behind = swarm.mirrorNewest - swarm.mirrorClock;
            if (std::fabs(behind) > ClockSnapTicks)
                swarm.mirrorClock = swarm.mirrorNewest;
            else
                swarm.mirrorClock += behind * ClockSlew;
        }
    }
    swarm.mirrorHeard = false;
    for (SwarmAgent& agent : swarm.agents) {
        if (!agent.alive)
            continue;
        const core::DVec3 at = carried(agent, swarm.mirrorClock, dt);
        agent.ease = agent.ease * EaseKept;
        const f64 x = at.x + static_cast<f64>(agent.ease.x);
        const f64 z = at.z + static_cast<f64>(agent.ease.z);
        // **The terrain this machine has, or the floor it was told** -- never a
        // ray: what a ray finds here is this machine's world, with the bodies
        // its own scripts made and none of what did not replicate.
        if (std::fabs(x - agent.groundX) + std::fabs(z - agent.groundZ) > 0.1) {
            const std::optional<f64> terrain = swarmTerrainAt(world, x, z);
            agent.onTerrain = terrain.has_value();
            agent.ground = terrain.value_or(agent.floor);
            agent.groundX = x;
            agent.groundZ = z;
        }
        if (!agent.onTerrain)
            agent.ground = agent.floor;
        agent.position = core::DVec3{x, agent.ground + static_cast<f64>(agent.lift + agent.ease.y), z};
        if (agent.body.valid() && world.alive(agent.body) && world.parts().find(agent.body) != nullptr) {
            (void)world.setProperty(agent.body, cframeName,
                                    Value{core::CFrameD{agent.position, core::rotationY(agent.yaw)}});
        }
    }
    buildGrid(swarm);
}

void keepEvent(SwarmComponent& swarm, const SwarmEvent& event)
{
    // A script layer that is not firing them -- a test of this module alone --
    // must not let them grow for ever.
    if (swarm.events.size() >= MaxSwarmEvents)
        swarm.events.erase(swarm.events.begin(),
                           swarm.events.begin() + static_cast<std::ptrdiff_t>(MaxSwarmEvents / 2));
    swarm.events.push_back(event);
}

} // namespace

u32 addSwarmAgent(SwarmComponent& swarm, core::InstanceId body, core::DVec3 position,
                  const SwarmAgentSettings& settings)
{
    SwarmAgent agent;
    agent.body = body;
    agent.position = position;
    agent.radius = std::max(settings.radius, 0.01f);
    agent.height = std::max(settings.height, 0.01f);
    agent.speed = std::max(settings.speed, 0.0f);
    agent.floats = settings.floats;
    agent.floatHeight = settings.floatHeight;
    agent.climbs = settings.climbs;
    agent.alive = true;
    agent.born = ++swarm.births;
    swarm.gridValid = false;
    u32 number = 0;
    if (!swarm.free.empty()) {
        // The lowest free slot, so the slots a crowd uses stay packed.
        const auto lowest = std::min_element(swarm.free.begin(), swarm.free.end());
        const u32 slot = *lowest;
        swarm.free.erase(lowest);
        swarm.agents[slot] = agent;
        number = slot + 1;
    }
    else {
        swarm.agents.push_back(agent);
        number = static_cast<u32>(swarm.agents.size());
    }
    keepEvent(swarm, SwarmEvent{.kind = SwarmEvent::Kind::Added,
                                .agent = number,
                                .radius = agent.radius,
                                .height = agent.height,
                                .position = position});
    return number;
}

bool removeSwarmAgent(SwarmComponent& swarm, u32 agent, u64 tick)
{
    SwarmAgent* row = swarmAgent(swarm, agent);
    if (row == nullptr)
        return false;
    keepEvent(swarm, SwarmEvent{.kind = SwarmEvent::Kind::Removed,
                                .reason = SwarmRemovalRemoved,
                                .tag = row->tag,
                                .agent = agent,
                                .position = row->position});
    // For the replicas, which are told by a module that looks at the rows
    // after this one is another agent's (ADR 0162). Kept only by a swarm
    // that replicates, and a tick: the replication takes them every tick.
    if (swarm.replicates) {
        std::erase_if(swarm.removed, [tick](const SwarmRemoved& gone) { return gone.tick + 600 < tick; });
        swarm.removed.push_back(SwarmRemoved{agent - 1, row->born, row->tag, tick, row->position});
    }
    *row = SwarmAgent{};
    swarm.free.push_back(agent - 1);
    swarm.gridValid = false;
    return true;
}

bool setSwarmAgentTag(SwarmComponent& swarm, u32 agent, u16 tag)
{
    SwarmAgent* row = swarmAgent(swarm, agent);
    if (row == nullptr)
        return false;
    if (row->tag != tag) {
        row->tag = tag;
        SwarmEvent changed;
        changed.kind = SwarmEvent::Kind::TagChanged;
        changed.tag = tag;
        changed.agent = agent;
        keepEvent(swarm, changed);
    }
    return true;
}

core::DVec3 swarmAgentAt(const SwarmAgent& agent, f64 tick, f64 dt) noexcept
{
    // A far agent is stepped every second or fourth tick with the time it
    // skipped: between its steps it is where its walk carries it.
    const f64 ticks = std::clamp(tick - static_cast<f64>(agent.stepped), 0.0, 4.0);
    const f64 metres = static_cast<f64>(agent.walk) * ticks * dt;
    return agent.position +
           core::DVec3{static_cast<f64>(agent.faceX) * metres, 0.0, static_cast<f64>(agent.faceZ) * metres};
}

void mirrorSwarmAgentRemoved(SwarmComponent& swarm, u32 slot, u8 reason, u16 tag, core::DVec3 position)
{
    if (slot >= swarm.agents.size() || !swarm.agents[slot].alive)
        return;
    keepEvent(
        swarm,
        SwarmEvent{
            .kind = SwarmEvent::Kind::Removed, .reason = reason, .tag = tag, .agent = slot + 1, .position = position});
    // Its body is this machine's, and stays whoever's it was: the script that
    // hears the removal decides what becomes of it.
    swarm.agents[slot] = SwarmAgent{};
    swarm.gridValid = false;
}

void mirrorSwarmAgentAdded(SwarmComponent& swarm, u32 slot, u16 tag, f32 radius, f32 height, const SwarmTold& told,
                           f64 tick)
{
    if (slot >= swarm.agents.size())
        swarm.agents.resize(static_cast<usize>(slot) + 1);
    // A number that still names another here: that one went, and the message
    // that said so was this one's own.
    if (swarm.agents[slot].alive)
        mirrorSwarmAgentRemoved(swarm, slot, SwarmRemovalRemoved, swarm.agents[slot].tag, swarm.agents[slot].position);
    SwarmAgent agent;
    agent.alive = true;
    agent.tag = tag;
    agent.radius = radius;
    agent.height = height;
    agent.born = ++swarm.births;
    agent.told = told.position;
    agent.toldTick = tick;
    agent.lift = told.lift;
    agent.floor = told.floor;
    faceBy(agent, told.yaw);
    agent.walk = told.walk;
    agent.position = told.position;
    swarm.agents[slot] = agent;
    if (tick >= swarm.mirrorNewest) {
        swarm.mirrorNewest = tick;
        swarm.mirrorHeard = true;
    }
    swarm.gridValid = false;
    keepEvent(swarm, SwarmEvent{.kind = SwarmEvent::Kind::Added,
                                .tag = tag,
                                .agent = slot + 1,
                                .radius = radius,
                                .height = height,
                                .position = told.position});
}

bool mirrorSwarmAgentTold(SwarmComponent& swarm, u32 slot, const SwarmTold& told, f64 tick, f64 dt,
                          std::optional<u16> tag)
{
    if (slot >= swarm.agents.size() || !swarm.agents[slot].alive)
        return false;
    SwarmAgent& agent = swarm.agents[slot];
    // Older than what it has: a message that arrived late, or one for the
    // agent that had this number before.
    if (tick <= agent.toldTick)
        return false;
    if (tick >= swarm.mirrorNewest) {
        swarm.mirrorNewest = tick;
        swarm.mirrorHeard = true;
    }
    const f64 clock = swarm.mirrorClockSet ? swarm.mirrorClock : tick;
    // **Taken up, not jumped to**: what it is drawn at stays where it is this
    // tick, and the difference from where the new word puts it dies away.
    const core::DVec3 before = carried(agent, clock, dt);
    const f64 heightBefore =
        (agent.onTerrain ? agent.ground : agent.floor) + static_cast<f64>(agent.lift + agent.ease.y);
    agent.told = told.position;
    agent.toldTick = tick;
    faceBy(agent, told.yaw);
    agent.walk = told.walk;
    agent.lift = told.lift;
    agent.floor = told.floor;
    // The height it is drawn at stays too: on the terrain it has, or on the
    // floor it is told -- which may have changed with the lift.
    const f64 heightAfter = (agent.onTerrain ? agent.ground : agent.floor) + static_cast<f64>(agent.lift);
    const core::DVec3 after = carried(agent, clock, dt);
    agent.ease =
        core::Vec3{agent.ease.x + static_cast<f32>(before.x - after.x), static_cast<f32>(heightBefore - heightAfter),
                   agent.ease.z + static_cast<f32>(before.z - after.z)};
    // Too far to be a correction: it was moved. There it is.
    if (agent.ease.x * agent.ease.x + agent.ease.z * agent.ease.z > 16.0f)
        agent.ease = core::Vec3{};
    if (tag.has_value() && *tag != agent.tag) {
        agent.tag = *tag;
        SwarmEvent changed;
        changed.kind = SwarmEvent::Kind::TagChanged;
        changed.tag = *tag;
        changed.agent = slot + 1;
        keepEvent(swarm, changed);
    }
    return true;
}

std::optional<f64> swarmTerrainAt(const World& world, f64 x, f64 z)
{
    std::optional<f64> best;
    world.terrains().forEach([&](core::InstanceId, const TerrainComponent& terrain) {
        const std::optional<float> height = asset::heightAt(terrain.field, x - terrain.origin.x, z - terrain.origin.z);
        if (!height.has_value())
            return;
        const f64 top = static_cast<f64>(*height) + terrain.origin.y;
        if (!best.has_value() || top > *best)
            best = top;
    });
    return best;
}

SwarmAgent* swarmAgent(SwarmComponent& swarm, u32 agent) noexcept
{
    if (agent == 0 || agent > swarm.agents.size() || !swarm.agents[agent - 1].alive)
        return nullptr;
    return &swarm.agents[agent - 1];
}

const SwarmAgent* swarmAgent(const SwarmComponent& swarm, u32 agent) noexcept
{
    if (agent == 0 || agent > swarm.agents.size() || !swarm.agents[agent - 1].alive)
        return nullptr;
    return &swarm.agents[agent - 1];
}

void querySwarmRadius(SwarmComponent& swarm, core::DVec3 centre, f64 radius, bool flat, std::vector<u32>& out)
{
    const usize first = out.size();
    const f64 reach = radius * radius;
    const auto within = [&](const SwarmAgent& agent) {
        const f64 dx = agent.position.x - centre.x;
        const f64 dy = flat ? 0.0 : agent.position.y - centre.y;
        const f64 dz = agent.position.z - centre.z;
        return dx * dx + dy * dy + dz * dz <= reach;
    };
    if (!swarm.gridValid || swarm.gridCell != swarm.cellSize)
        buildGrid(swarm);
    const f64 cell = std::max(static_cast<f64>(swarm.gridCell), 0.01);
    const i64 low[2] = {cellOf(centre.x - radius, cell), cellOf(centre.z - radius, cell)};
    const i64 high[2] = {cellOf(centre.x + radius, cell), cellOf(centre.z + radius, cell)};
    // **Through the grid** (H10), cell by cell round the centre -- unless the
    // circle covers more cells than there are agents, where the rows
    // themselves are the shorter walk.
    const f64 cells = static_cast<f64>(high[0] - low[0] + 1) * static_cast<f64>(high[1] - low[1] + 1);
    if (cells > static_cast<f64>(swarm.agents.size())) {
        for (usize slot = 0; slot < swarm.agents.size(); ++slot) {
            if (swarm.agents[slot].alive && within(swarm.agents[slot]))
                out.push_back(static_cast<u32>(slot) + 1);
        }
        return;
    }
    for (i64 x = low[0]; x <= high[0]; ++x) {
        for (i64 z = low[1]; z <= high[1]; ++z) {
            const auto found = swarm.grid.find(cellKey(x, z));
            for (u32 at = found == swarm.grid.end() ? 0u : found->second; at != 0; at = swarm.gridNext[at - 1]) {
                const SwarmAgent& agent = swarm.agents[at - 1];
                if (agent.alive && within(agent))
                    out.push_back(at);
            }
        }
    }
    // In slot order, as the rows are.
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
}

void stepSwarms(World& world, const PhysicsSync* physics, f64 dt)
{
    const core::NameAtom cframeName = world.atoms().intern("CFrame");
    const u64 tick = world.engineState().tick;
    std::vector<core::InstanceId> swarms;
    world.swarms().forEach([&](core::InstanceId id, SwarmComponent& swarm) {
        if ((swarm.enabled || swarm.mirrored) && !swarm.agents.empty())
            swarms.push_back(id);
    });
    // Collected first: a step writes parts, which can grow the world's pools.
    for (const core::InstanceId id : swarms) {
        SwarmComponent* swarm = world.swarms().find(id);
        if (swarm == nullptr)
            continue;
        if (swarm->mirrored)
            stepMirror(world, *swarm, dt, cframeName);
        else
            stepSwarm(world, physics, *swarm, dt, tick, cframeName);
    }
}

} // namespace engine::scene
