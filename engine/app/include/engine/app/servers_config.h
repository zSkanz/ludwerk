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

    // `[[world]]`: a world that runs while the manager does.
    struct World
    {
        std::string name;
        std::string scene;
        core::u32 maxPlayers = 100;
        bool entry = false;
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
