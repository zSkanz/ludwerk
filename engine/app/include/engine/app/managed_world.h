// **A world a manager started** (ADR 0220 §2, ADR 0222): what it tells the
// manager -- ready, a heartbeat a second, who came and went, who it sends
// elsewhere -- and what it takes from it: the other worlds' names, the passes
// for its travellers, a drain and a stop.
//
// It takes nobody without a pass the manager signed for it. Its input closing
// is its manager gone, and it stops.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/manager_link.h"
#include "engine/app/travel_pass.h"
#include "engine/core/types.h"
#include "engine/replication/types.h"

namespace engine::scene {
class World;
struct EngineState;
} // namespace engine::scene

namespace engine::replication {
class IReplication;
} // namespace engine::replication

namespace engine::app {

// ADR 0223 §1: an event between worlds older than this is not news, and a
// world holds at most so many, so large, waiting.
inline constexpr std::int64_t CrossWorldOldSeconds = 10;
inline constexpr std::size_t CrossWorldMostHeld = 1000;
inline constexpr std::size_t CrossWorldMostBytes = 4u * 1024u * 1024u;

// **Which channel of its world a managed world is** (ADR 0230): `town:3`
// is 3, and a name with no channel is 1.
[[nodiscard]] core::u32 channelOf(std::string_view world) noexcept;

class ManagedWorld
{
public:
    // How a line reaches the manager: the process's standard output, or a
    // test's list.
    using Send = std::function<void(const std::string& line)>;

    ManagedWorld(std::string name, std::array<core::u8, 32> managerKey, Send send);

    // The wall clock, for an event between worlds too old to be news.
    void setUnixClock(std::function<std::int64_t()> clock) { m_unixClock = std::move(clock); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    // This world's own public key, hexadecimal: what the manager names it by
    // in each redirect to it (ADR 0224 §3).
    void setKey(std::string key) { m_key = std::move(key); }

    // A message of the manager's.
    void take(const ManagerMessage& message, scene::World& world, replication::IReplication* replication);

    // **The door's word on a joiner** (the session's gate): a refusal, or
    // nothing -- the pass was this world's, its ticket is now the joiner's,
    // and their arrival waits for their player.
    [[nodiscard]] std::optional<replication::JoinAnswer> admit(replication::JoinAsk& ask, scene::World& world,
                                                               std::int64_t nowUnix);

    // Once a frame: ready once the match listens, a heartbeat a second, who
    // came and went, and who a script sends elsewhere.
    void frame(scene::World& world, core::InstanceId dataModel, replication::IReplication* replication, core::u16 port,
               core::u64 nowNs, core::f64 tickMs);

    // The manager said stop, or is gone.
    [[nodiscard]] bool stopping() const noexcept { return m_stopping; }
    void managerGone() noexcept { m_stopping = true; }
    // Draining (ADR 0220 §5): nobody new.
    [[nodiscard]] bool draining() const noexcept { return m_draining; }

private:
    void send(const core::JsonWriter& message) const;
    void dropCrossWorld(scene::EngineState& state);

    std::string m_name;
    std::string m_key;
    std::function<std::int64_t()> m_unixClock;
    std::int64_t m_crossWorldSaidAt = 0;
    core::InstanceId m_dataModel;
    std::array<core::u8, 32> m_managerKey;
    Send m_send;
    PassReplay m_replay;
    bool m_ready = false;
    bool m_stopping = false;
    bool m_draining = false;
    core::u64 m_nextHeartbeatNs = 0;
    core::f64 m_tickSum = 0.0;
    core::u32 m_ticks = 0;
    core::f64 m_tickWorst = 0.0;
    // Who is in, by key, as last told.
    std::map<std::string, std::string> m_present;
    // Travels asked for and not yet answered: who, by key, and their number.
    std::map<core::u32, std::map<std::string, core::u32>> m_travels;
};

} // namespace engine::app
