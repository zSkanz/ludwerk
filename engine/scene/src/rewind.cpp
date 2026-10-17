#include "engine/scene/rewind.h"

#include <algorithm>
#include <cmath>

namespace engine::scene {

namespace {

// How many ticks the cap reaches, at this world's tick.
[[nodiscard]] u64 capTicks(const EngineState& state) noexcept
{
    if (!(state.rewindCapMs > 0.0) || !(state.fixedTimestep > 0.0))
        return 0;
    return static_cast<u64>(std::llround(state.rewindCapMs / 1000.0 / state.fixedTimestep));
}

} // namespace

void recordRewind(World& world)
{
    EngineState& state = world.engineState();
    if (state.networkTopology != NetworkTopology::Host && state.networkTopology != NetworkTopology::Dedicated) {
        state.rewindFrames.clear();
        return;
    }
    // One frame past the cap: the tick being run reads the one before it.
    const u64 depth = capTicks(state) + 2;
    if (capTicks(state) == 0) {
        state.rewindFrames.clear();
        return;
    }
    // The oldest frame's storage, kept for the newest: a server at sixty
    // ticks a second allocates nothing once it has run a cap's worth.
    EngineState::RewindFrame frame;
    while (state.rewindFrames.size() >= depth) {
        frame = std::move(state.rewindFrames.front());
        state.rewindFrames.pop_front();
    }
    frame.tick = state.tick;
    frame.entries.clear();
    std::vector<core::InstanceId> below;
    const auto add = [&](core::InstanceId id) {
        if (const PartComponent* part = world.parts().find(id); part != nullptr && !world.destroyed(id))
            frame.entries.push_back(EngineState::RewindEntry{id, part->cframe});
    };
    // Every player's character, and the parts it is made of.
    world.players().forEach([&](core::InstanceId, const PlayerComponent& player) {
        if (!player.character.valid() || !world.alive(player.character))
            return;
        add(player.character);
        below.clear();
        world.collectDescendants(player.character, below);
        for (const core::InstanceId id : below)
            add(id);
    });
    world.lagCompensated().forEach([&](core::InstanceId id, const LagCompensatedComponent& compensated) {
        if (compensated.on)
            add(id);
    });
    std::sort(frame.entries.begin(), frame.entries.end(),
              [](const EngineState::RewindEntry& a, const EngineState::RewindEntry& b) {
                  return a.id.index != b.id.index ? a.id.index < b.id.index : a.id.generation < b.id.generation;
              });
    frame.entries.erase(
        std::unique(frame.entries.begin(), frame.entries.end(),
                    [](const EngineState::RewindEntry& a, const EngineState::RewindEntry& b) { return a.id == b.id; }),
        frame.entries.end());
    state.rewindFrames.push_back(std::move(frame));
}

std::optional<u64> rewindTickFor(const World& world, const PlayerComponent& player) noexcept
{
    const EngineState& state = world.engineState();
    if (!player.viewKnown || state.tick == 0)
        return std::nullopt;
    // The tick the authority heard it say was the newest it had finished: the
    // one before the tick being run.
    const u64 behind = std::min(player.viewBehind, capTicks(state));
    const u64 newest = state.tick - 1;
    return newest > behind ? newest - behind : 0;
}

const EngineState::RewindFrame* rewindFrameAt(const World& world, u64 tick) noexcept
{
    const std::deque<EngineState::RewindFrame>& frames = world.engineState().rewindFrames;
    if (frames.empty() || tick > frames.back().tick)
        return nullptr;
    if (tick <= frames.front().tick)
        return &frames.front();
    // Consecutive ticks but for a world that paused; found by search either way.
    const auto found = std::lower_bound(frames.begin(), frames.end(), tick,
                                        [](const EngineState::RewindFrame& frame, u64 at) { return frame.tick < at; });
    return found == frames.end() ? &frames.back() : &*found;
}

} // namespace engine::scene
