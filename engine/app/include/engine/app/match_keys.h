// **The keys a machine plays by** (ADR 0224): its own, and the hosts it has
// met.
//
// A machine's key is made the first time it is wanted and kept in the game's
// user data folder, readable by its owner only: the same file is the same
// player next week, and a server's is the same server after a restart. A
// host's key is pinned the first time this machine joins it -- **silently**:
// a game on a LAN must not frighten a player with a question about a key --
// and a later join to the same address with another key is refused as
// `KeyChanged`, which the player is told how to deal with.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/crypto.h"
#include "engine/platform/file.h"

namespace engine::app {

// The key pair at `file`, made and written there when there is none. A file
// that cannot be read or written leaves a key that lasts this run only, and
// says so in the log. An empty path is a key for this run.
//
// **One process at a time is that player**: `held` takes a lock beside the
// file for as long as it lives. A second copy of the game started on the same
// machine -- how a match is tried on one computer -- finds it taken and plays
// with a key of its own for the run: given the same key, the server took it
// for the first player reconnecting and let the first one go.
[[nodiscard]] core::X25519Pair loadOrMakeMatchKey(const std::filesystem::path& file, platform::FileLock& held);

// A key written in hexadecimal, as a host's log prints it: 64 digits, or
// nothing.
[[nodiscard]] std::optional<std::array<core::u8, 32>> matchKeyFromHex(std::string_view hex);

// Hosts this machine has met, by where it joined them.
class KeyPins
{
public:
    explicit KeyPins(std::filesystem::path file);

    // **Whether to go on with a host**: a host never met is pinned and taken;
    // one met before is taken when its key is the pinned one. `where` is the
    // address and port joined, or a join code.
    [[nodiscard]] bool trust(std::string_view where, const std::array<core::u8, 32>& key);
    // Forgets one: what a player does about a host that reinstalled its game.
    void forget(std::string_view where);

private:
    void save() const;

    std::filesystem::path m_file;
    std::map<std::string, std::string, std::less<>> m_pins;
};

} // namespace engine::app
