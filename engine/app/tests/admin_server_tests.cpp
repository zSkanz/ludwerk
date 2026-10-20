// **A server administered from another machine** (ADR 0219): who is let in,
// from where, how often a stranger may try, and a command's round trip -- a
// server and its administrators in one process, over loopback TCP.
#include <chrono>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/admin_channel.h"
#include "engine/app/admin_server.h"
#include "engine/core/crypto.h"
#include "engine/core/log.h"
#include "engine_test_ports.h"

using namespace engine;

namespace {

// Test ports are 47xxx, never a game's.
constexpr core::u16 AdminTestPort = engine::testing::port(47811);

struct Admin
{
    core::X25519Pair key = core::x25519Generate();
};

app::AdminServer::Options serverWith(core::u16 port, const std::vector<app::AdminEntry>& admins,
                                     const core::X25519Pair& serverKey)
{
    app::AdminServer::Options options;
    options.port = port;
    options.serverKey = serverKey;
    options.prologue = app::adminPrologue();
    options.admins = admins;
    return options;
}

std::optional<app::AdminChannel> connectAs(core::u16 port, const Admin& admin, std::array<core::u8, 32>& serverKey)
{
    std::optional<core::EngineError> error;
    return app::AdminChannel::connect("127.0.0.1", port, admin.key, app::adminPrologue(), serverKey, error);
}

// The server's next lines, waiting a while for them.
std::vector<app::AdminServer::Line> linesFrom(app::AdminServer& server)
{
    std::vector<app::AdminServer::Line> lines;
    for (int at = 0; at < 200 && lines.empty(); ++at) {
        server.take(lines);
        if (lines.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return lines;
}

// Whether the channel is still answered: a message sent, and none of the
// server's refusal coming back.
bool refused(app::AdminChannel& channel)
{
    (void)channel.send(app::AdminKind::Line, "status");
    for (int at = 0; at < 40; ++at) {
        app::AdminKind kind = app::AdminKind::Answer;
        std::string text;
        bool got = false;
        if (channel.receive(kind, text, got, 50).has_value())
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("admins.toml names each administrator by key, with a level")
{
    const Admin ana;
    const std::string text = "[[admin]]\nname = \"ana\"\nkey = \"" + core::toHex(ana.key.pub) +
                             "\"\nlevel = 2\n\n[[admin]]\nname = \"bo\"\nkey = \"" + core::toHex(ana.key.pub) + "\"\n";
    std::optional<core::EngineError> error;
    const std::vector<app::AdminEntry> admins = app::readAdmins(text, "admins.toml", error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(admins.size() == 2);
    CHECK(admins[0].name == "ana");
    CHECK(admins[0].key == ana.key.pub);
    CHECK(admins[0].level == 2);
    // A level not given is the whole console's.
    CHECK(admins[1].level == 3);

    // An entry that is wrong stops the list, rather than letting in part of it.
    const std::vector<app::AdminEntry> wrong =
        app::readAdmins("[[admin]]\nname = \"ana\"\nkey = \"abc\"\n", "admins.toml", error);
    CHECK(wrong.empty());
    REQUIRE(error.has_value());
    CHECK(error->message.find("admins.toml") != std::string::npos);
}

TEST_CASE("the machine itself may always reach administration, others only where allowed")
{
    CHECK(app::addressAllowed("127.0.0.1", {}));
    CHECK(app::addressAllowed("127.4.5.6", {}));
    CHECK_FALSE(app::addressAllowed("203.0.113.4", {}));
    CHECK(app::addressAllowed("203.0.113.4", {"203.0.113.4"}));
    CHECK_FALSE(app::addressAllowed("203.0.113.5", {"203.0.113.4"}));
    CHECK(app::addressAllowed("10.20.30.40", {"10.0.0.0/8"}));
    CHECK_FALSE(app::addressAllowed("11.0.0.1", {"10.0.0.0/8"}));
    CHECK(app::addressAllowed("192.168.1.77", {"192.168.1.0/24"}));
    // What is not an address is never allowed, and a range that is wrong
    // allows nobody.
    CHECK_FALSE(app::addressAllowed("::1", {"10.0.0.0/8"}));
    CHECK_FALSE(app::addressAllowed("10.0.0.1", {"10.0.0.0/40", "nonsense"}));
}

TEST_CASE("an administrator's line reaches the console with their name, and the answer reaches them alone")
{
    const Admin ana;
    const Admin bo;
    const core::X25519Pair serverKey = core::x25519Generate();
    app::AdminServer server;
    REQUIRE_FALSE(server
                      .start(serverWith(AdminTestPort,
                                        {app::AdminEntry{"ana", ana.key.pub, 1}, app::AdminEntry{"bo", bo.key.pub, 3}},
                                        serverKey))
                      .has_value());

    std::array<core::u8, 32> seenByAna{};
    std::optional<app::AdminChannel> anaChannel = connectAs(AdminTestPort, ana, seenByAna);
    REQUIRE(anaChannel.has_value());
    // **The administrator learns the server's key**, to check before sending.
    CHECK(seenByAna == serverKey.pub);
    std::array<core::u8, 32> seenByBo{};
    std::optional<app::AdminChannel> boChannel = connectAs(AdminTestPort, bo, seenByBo);
    REQUIRE(boChannel.has_value());

    REQUIRE_FALSE(anaChannel->send(app::AdminKind::Line, "players").has_value());
    const std::vector<app::AdminServer::Line> lines = linesFrom(server);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].line == "players");
    CHECK_FALSE(lines[0].caller.local);
    CHECK(lines[0].caller.name == "ana");
    CHECK(lines[0].caller.permission == 1);
    CHECK(lines[0].caller.key == core::toHex(ana.key.pub).substr(0, 16));

    server.answer(lines[0].session, "nobody is here", true);
    app::AdminKind kind = app::AdminKind::Answer;
    std::string text;
    bool got = false;
    for (int at = 0; at < 40 && !got; ++at)
        REQUIRE_FALSE(anaChannel->receive(kind, text, got, 50).has_value());
    REQUIRE(got);
    CHECK(kind == app::AdminKind::Failed);
    CHECK(text == "nobody is here");

    // Bo was not asked anything, and hears nothing.
    got = false;
    REQUIRE_FALSE(boChannel->receive(kind, text, got, 200).has_value());
    CHECK_FALSE(got);
    CHECK(server.sessions() == 2);
    server.stop();
}

TEST_CASE("a key not in admins.toml is refused, and an address that keeps failing is not answered")
{
    const Admin ana;
    const Admin stranger;
    app::AdminServer server;
    REQUIRE_FALSE(
        server.start(serverWith(AdminTestPort + 1, {app::AdminEntry{"ana", ana.key.pub, 3}}, core::x25519Generate()))
            .has_value());

    // The handshake itself completes on the stranger's side; the server ends
    // it there, and nothing they send reaches the console.
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::array<core::u8, 32> seen{};
        std::optional<app::AdminChannel> channel = connectAs(AdminTestPort + 1, stranger, seen);
        if (channel.has_value())
            CHECK(refused(*channel));
    }
    std::vector<app::AdminServer::Line> lines;
    server.take(lines);
    CHECK(lines.empty());

    // **Five in a minute, and the address is not answered for ten**: even a
    // key that is in the list, from there.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::array<core::u8, 32> seen{};
    std::optional<app::AdminChannel> blocked = connectAs(AdminTestPort + 1, ana, seen);
    if (blocked.has_value())
        CHECK(refused(*blocked));
    server.take(lines);
    CHECK(lines.empty());
    server.stop();
}

TEST_CASE("an administrator who follows the log is sent its lines")
{
    const Admin ana;
    app::AdminServer server;
    REQUIRE_FALSE(
        server.start(serverWith(AdminTestPort + 2, {app::AdminEntry{"ana", ana.key.pub, 3}}, core::x25519Generate()))
            .has_value());
    std::array<core::u8, 32> seen{};
    std::optional<app::AdminChannel> channel = connectAs(AdminTestPort + 2, ana, seen);
    REQUIRE(channel.has_value());
    REQUIRE_FALSE(channel->send(app::AdminKind::Follow, {}).has_value());

    bool heard = false;
    for (int at = 0; at < 100 && !heard; ++at) {
        core::logText(core::LogLevel::Info, "a line for the administrator");
        app::AdminKind kind = app::AdminKind::Answer;
        std::string text;
        bool got = false;
        REQUIRE_FALSE(channel->receive(kind, text, got, 30).has_value());
        heard =
            got && kind == app::AdminKind::LogLine && text.find("a line for the administrator") != std::string::npos;
    }
    CHECK(heard);
    server.stop();
}
