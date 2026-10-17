// **Administering a server from another machine** (ADR 0219): the listener,
// who may come in, and their lines handed to the console between ticks.
//
// Off unless asked for, on the machine's own address unless `admin_allow`
// names others, and a key not in `admins.toml` fails the handshake. At most
// four administrators at once; an address that fails five handshakes in a
// minute is not answered for ten; a session quiet for ten minutes is closed.
// Every session, command and refusal is said in the log.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/server_console.h"
#include "engine/core/crypto.h"
#include "engine/core/error.h"

namespace engine::app {

struct AdminEntry
{
    std::string name;
    std::array<core::u8, 32> key{};
    core::u8 level = 3;
};

// `admins.toml`: `[[admin]]` with `name`, `key` (hexadecimal, what `ludwerk
// server keygen` printed) and `level` (0 to 3). Nothing, and `error`, for a
// file that is wrong.
[[nodiscard]] std::vector<AdminEntry> readAdmins(std::string_view text, std::string_view source,
                                                 std::optional<core::EngineError>& error);

// Whether `address` (IPv4) is one `allow` names: an address, or a range
// written `10.0.0.0/8`.
[[nodiscard]] bool addressAllowed(std::string_view address, const std::vector<std::string>& allow);

struct AdminListener;

class AdminServer
{
public:
    struct Options
    {
        core::u16 port = 0;
        // Listens beyond 127.0.0.1 only where `allow` names somebody.
        std::vector<std::string> allow;
        core::X25519Pair serverKey;
        std::vector<core::u8> prologue;
        std::vector<AdminEntry> admins;
        // Where they were read from, for what the log says.
        std::string adminsSource = "admins.toml";
    };

    AdminServer();
    ~AdminServer();
    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    [[nodiscard]] std::optional<core::EngineError> start(Options options);
    void stop();

    // A line an administrator sent, for the console.
    struct Line
    {
        core::u32 session = 0;
        std::string line;
        ServerConsole::Caller caller;
    };
    void take(std::vector<Line>& out);
    // What the console answered it.
    void answer(core::u32 session, const std::string& text, bool failed);

    // How many administrators are in now.
    [[nodiscard]] core::usize sessions() const;

private:
    struct Session;
    void acceptLoop();
    void serve(std::shared_ptr<Session> session);
    void followed(std::string_view line);

    Options m_options;
    std::unique_ptr<AdminListener> m_listener;
    std::thread m_acceptor;
    std::atomic<bool> m_stopping{false};
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Session>> m_sessions;
    std::vector<Line> m_lines;
    core::u32 m_nextSession = 1;
    // Failed handshakes by address, and until when one is not answered.
    std::map<std::string, std::vector<std::int64_t>> m_failures;
    std::map<std::string, std::int64_t> m_blockedUntil;
};

} // namespace engine::app
