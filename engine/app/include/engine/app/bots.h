// **Light bots** (ADR 0236): many players from one process, for a load test.
//
// A bot is a connection, not a game. It joins as a player does -- the same
// handshake, encrypted, the same Luau flags -- keeps the world it is sent in a
// world of its own, and answers with input; it runs no renderer, no physics
// and none of the game's scripts. What it does is a behavior: walk about,
// crowd one place, walk to the pads the game sends players on from, stand --
// or a game's own, a Luau file (`script::BotBehaviors`). Wherever the server
// sends a bot (`Travel`), it follows.
//
// `engine-host <project> --join=host:port --bots=N` runs them; `ludwerk
// server loadtest` starts as many of those processes as it needs and reads
// what each prints.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

struct BotOptions
{
    std::string address;
    core::u16 port = 0;
    core::u32 count = 0;
    // `wander`, `crowd`, `travel` or `idle`; or, with `script`, the file the
    // game's own behavior came from.
    std::string behavior = "wander";
    // A game's behavior: the file's text, empty for the built-in ones.
    std::string script;
    // How long to run; 0 until the process is stopped.
    core::f64 seconds = 0.0;
    // The intent a bot walks with: a `Direction2D` the game reads with
    // `Player:GetIntent`.
    std::string intent = "Move";
    // What every match's handshake binds, and whether it is encrypted.
    std::vector<core::u8> prologue;
    bool encrypt = true;
};

// The most bots one process holds.
inline constexpr core::u32 MostBotsAProcess = 64;
// What `travel` walks to: the parts carrying this tag, as a game tags the
// pads its server sends players on from.
inline constexpr std::string_view BotPortalTag = "Portal";

// Runs them; the process's exit code. Prints one line a second, a JSON
// object after the record separator (0x1E), for whoever started it.
[[nodiscard]] int runBots(const BotOptions& options);

} // namespace engine::app
