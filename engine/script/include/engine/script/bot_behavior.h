#pragma once

// **A load test's bot, as a game writes it** (ADR 0236 section 3).
//
// A light bot (`engine-host --bots`) runs no game: no renderer, no physics and
// none of the game's scripts. What it can run is a behavior -- a Luau file the
// game writes for its bots, with `require("@engine/bot")` and nothing else --
// in a VM of its own, sandboxed (R4), one thread for each bot.
//
// **What the API is.** `Bot:WalkTo(v)` walks until it gets there, or gives up
// after a time, and says which; `Bot:Wait(s)` waits; `Bot:GetPosition()`
// reads where the bot's character is; `Bot:FindTagged(tag)` reads where the
// parts with a tag are, in the bot's own copy of the world; `Bot:Say(text)`
// writes a line to the bot process's log; `Bot.Spawn` is where the bot first
// stood and `Bot.Index` which bot of its process it is. The type side is
// `runtime/engine/bot/init.luau`.
//
// **A behavior begins again in each world**: a bot the server sends on
// (`Travel`) is a new connection to a new world, where the places a behavior
// walks to are other places.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::script {

// What a bot is this tick, as the bot process sees it.
struct BotSight
{
    bool hasBody = false;
    core::DVec3 position{};
};

// What a behavior reads of the world it cannot reach: the bot's own copy, the
// one its snapshots filled.
class BotWorldView
{
public:
    virtual ~BotWorldView() = default;
    // Where the parts carrying `tag` are.
    virtual void tagged(std::string_view tag, std::vector<core::DVec3>& out) const = 0;
};

class BotBehaviors
{
public:
    // Compiles `source`; null with the reason when it does not compile.
    [[nodiscard]] static std::unique_ptr<BotBehaviors> create(std::string_view source, std::string_view chunkName,
                                                              std::string& outError);
    ~BotBehaviors();
    BotBehaviors(const BotBehaviors&) = delete;
    BotBehaviors& operator=(const BotBehaviors&) = delete;

    // A bot's run of the behavior, from its beginning: when the bot is in a
    // world (again, after it was sent on). It starts once the bot has a body.
    void restart(core::u32 bot);
    // Runs `bot`'s behavior for one tick, at `seconds` since the process
    // began. A behavior that fails is said once in the log and stops.
    void step(core::u32 bot, const BotSight& sight, const BotWorldView& view, core::f64 seconds);
    // Where `bot`'s behavior is walking it to, if anywhere.
    [[nodiscard]] std::optional<core::DVec3> walkTarget(core::u32 bot) const;
    // How many bots' behaviors failed, and how many ran to their end.
    [[nodiscard]] core::u32 failed() const noexcept;
    [[nodiscard]] core::u32 finished() const noexcept;

    struct Impl;

private:
    BotBehaviors();
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::script
