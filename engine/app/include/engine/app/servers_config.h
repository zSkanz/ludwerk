// **`servers.toml`** (ADR 0220 §3): the worlds of an online game and the
// manager that runs them.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::app {

struct ServersConfig
{
    // `[manager]`
    std::string publicAddress = "127.0.0.1";
    core::u16 port = 7777;
    core::u16 firstWorldPort = 7800;
    core::u16 lastWorldPort = 7899;
    std::string logs = "logs";
    core::u32 logMegabytes = 10;
    core::u32 logFiles = 5;
    core::u32 restarts = 5;
    core::u32 restartMinutes = 10;
    core::u32 minFreeMegabytes = 512;
    // `health = true`: each world answers `/health` and `/metrics` on its own
    // port number, over TCP.
    bool health = false;
    // `admin = true`, `admin_port` (the front door's plus one when not given)
    // and `admin_allow` (ADR 0219): the manager's console from another
    // machine, by the keys in `admins.toml` beside this file.
    bool admin = false;
    core::u16 adminPort = 0;
    std::vector<std::string> adminAllow;
    // **The line at the front door** (ADR 0232): the whole game's players at
    // most, 0 for no limit beyond each world's; the most who may wait; and
    // how long one who dropped while waiting -- or from a world -- keeps
    // their place.
    core::u32 maxPlayers = 0;
    core::u32 queueMax = 10000;
    core::u32 queueGraceSeconds = 60;

    // `[data]` (ADR 0238 §3, §7): an online backup of the game's database
    // every `backup_every_hours` into `data/backups/` (0, never), the newest
    // `backup_keep` kept; and the move log's lines kept `moves_keep_days`
    // (0, for good).
    core::f64 backupEveryHours = 0.0;
    core::u32 backupKeep = 7;
    core::u32 movesKeepDays = 90;

    // `[accounts]` (ADR 0237): the front door signs players in, and lets in
    // only those it signed in; whether a player may make an account
    // (`register`); and a pattern a login name must match too, inside the
    // engine's own set (amendment F).
    bool accounts = false;
    bool accountsRegister = true;
    std::string accountsNamePattern;

    // `[parties] size` (ADR 0240 §1): the most members a party has.
    core::u32 partySize = 8;
    // `[manager] region` (ADR 0243 §1): the region its players are matched
    // in, where a script names none.
    std::string region;

    // `[[world]]`: a world that runs while the manager does.
    struct World
    {
        std::string name;
        std::string scene;
        core::u32 maxPlayers = 100;
        bool entry = false;
        // `time_dilation` (ADR 0233): this world slows its clock when it is
        // overloaded, whatever the game's `[server]` says.
        bool timeDilation = false;
        // **Channels** (ADR 0230): copies of this world, opened as every
        // one open holds `channelOpenAt` of `maxPlayers`, up to
        // `maxChannels`; one other than the first stopped after
        // `channelCloseSeconds` empty, and merged into another after
        // `mergeMinutes` under a quarter full (0 never).
        bool channels = false;
        core::u32 maxChannels = 8;
        core::f64 channelOpenAt = 0.9;
        core::u32 channelCloseSeconds = 120;
        core::f64 mergeMinutes = 10.0;
    };
    std::vector<World> worlds;

    // `[[instance]]`: a template a world asks copies of (ADR 0221).
    struct Instance
    {
        std::string name;
        std::string scene;
        core::u32 maxPlayers = 10;
        core::u32 maxInstances = 20;
        core::u32 warm = 0;
        core::u32 idleCloseSeconds = 60;
    };
    std::vector<Instance> instances;

    [[nodiscard]] const World* entry() const;
    [[nodiscard]] const World* world(std::string_view name) const;
    [[nodiscard]] const Instance* instance(std::string_view name) const;
};

// The file read and checked, or what is wrong with it.
[[nodiscard]] std::optional<ServersConfig> readServersConfig(std::string_view text, std::string_view source,
                                                             std::optional<core::EngineError>& error);

} // namespace engine::app
