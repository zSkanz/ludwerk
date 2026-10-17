// **An administrator's end** (ADR 0219 §6): `engine-host --admin-connect`,
// which `ludwerk server connect` runs, and `--admin-keygen`, which `ludwerk
// server keygen` does.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

struct AdminConnect
{
    std::string host;
    core::u16 port = 0;
    std::filesystem::path keyFile;
    // The server's key, hexadecimal; empty pins the first one met.
    std::string serverKey;
    bool follow = false;
    // One command, then its outcome as the exit code; empty is a prompt.
    std::string command;
};

[[nodiscard]] int runAdminConnect(const AdminConnect& options);
// A key pair: `file` the private half, `.pub` beside it the public, printed.
[[nodiscard]] int runAdminKeygen(const std::filesystem::path& file);

} // namespace engine::app
