// The relay program (ADR 0178): `net::RelayService` on a port, until it is
// told to stop, saying now and then what it carries.
//
//   engine-relay [--port=N] [--max-matches=N] [--max-relayed=N] [--rate=BYTES]
//                [--report=SECONDS]
//
// It keeps no file, knows no other relay, and reads nothing it forwards. What
// it is for, how a game uses it and what to open on the machine it runs on are
// in the manual, under "Playing over the internet".
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <string_view>
#include <thread>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/net/relay_service.h"
#include "engine/net/transport.h"
#include "relay_catalog.gen.h"

namespace {

using engine::core::I18nArg;
using engine::core::LogLevel;
using engine::core::u32;
using engine::core::u64;

constexpr int ExitOk = 0;
constexpr int ExitFailed = 1;
constexpr int ExitUsage = 2;

// Set by a signal, read by the loop: the one thing a handler may touch.
volatile std::sig_atomic_t g_stop = 0;

void onSignal(int)
{
    g_stop = 1;
}

[[nodiscard]] bool numberOf(std::string_view text, u64 lowest, u64 highest, u64& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return !text.empty() && error == std::errc{} && end == text.data() + text.size() && out >= lowest && out <= highest;
}

void sayUsage()
{
    const std::array<I18nArg, 1> args{I18nArg{"port", static_cast<engine::core::i64>(engine::net::DefaultRelayPort)}};
    engine::core::log(LogLevel::Info, ENG_TR("relay.info.usage"), args);
}

} // namespace

int main(int argc, char** argv)
{
    const auto catalog = engine::core::engineCatalog().loadFromJson(
        std::string_view(reinterpret_cast<const char*>(RelayCatalog), sizeof(RelayCatalog)), "en.json");
    if (!catalog.ok) {
        // The one line that cannot come from the catalog: it is about it.
        engine::core::logText(LogLevel::Error, catalog.diagnostic);
        return ExitFailed;
    }

    u64 port = engine::net::DefaultRelayPort;
    u64 reportSeconds = 60;
    engine::net::rendezvous::RelayLimits limits;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&arg](std::string_view name) { return arg.substr(name.size()); };
        u64 number = 0;
        if (arg == "--help" || arg == "-h") {
            sayUsage();
            return ExitOk;
        }
        if (arg.starts_with("--port=") && numberOf(value("--port="), 1, 65535, number)) {
            port = number;
        }
        else if (arg.starts_with("--max-matches=") && numberOf(value("--max-matches="), 1, 1'000'000, number)) {
            limits.maxSessions = static_cast<u32>(number);
        }
        else if (arg.starts_with("--max-relayed=") && numberOf(value("--max-relayed="), 1, 4095, number)) {
            limits.maxSlotsPerSession = static_cast<u32>(number);
        }
        else if (arg.starts_with("--rate=") && numberOf(value("--rate="), 1024, 1'000'000'000, number)) {
            limits.slotBytesPerSecond = static_cast<u32>(number);
        }
        else if (arg.starts_with("--burst=") && numberOf(value("--burst="), 1, 600, number)) {
            limits.slotBurstSeconds = static_cast<u32>(number);
        }
        else if (arg.starts_with("--report=") && numberOf(value("--report="), 0, 86'400, number)) {
            reportSeconds = number;
        }
        else {
            const std::array<I18nArg, 1> args{I18nArg{"option", arg}};
            engine::core::log(LogLevel::Error, ENG_TR("relay.err.bad_option"), args);
            sayUsage();
            return ExitUsage;
        }
    }

    engine::net::RelayService relay;
    if (const auto error = relay.start(static_cast<engine::core::u16>(port), limits); error.has_value()) {
        engine::core::logText(LogLevel::Error, error->message);
        return ExitFailed;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    {
        const std::array<I18nArg, 5> args{I18nArg{"port", static_cast<engine::core::i64>(relay.port())},
                                          I18nArg{"matches", static_cast<engine::core::i64>(limits.maxSessions)},
                                          I18nArg{"relayed", static_cast<engine::core::i64>(limits.maxSlotsPerSession)},
                                          I18nArg{"rate", static_cast<engine::core::i64>(limits.slotBytesPerSecond)},
                                          I18nArg{"burst", static_cast<engine::core::i64>(limits.slotBurstSeconds)}};
        engine::core::log(LogLevel::Info, ENG_TR("relay.info.listening"), args);
    }

    // A report when the interval is over and something is being carried or
    // has changed: a relay nobody uses says so once, not every minute.
    auto reportedAt = std::chrono::steady_clock::now();
    engine::net::rendezvous::RelayStats said{};
    bool saidOnce = false;
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (reportSeconds == 0)
            continue;
        const auto now = std::chrono::steady_clock::now();
        if (now - reportedAt < std::chrono::seconds(reportSeconds))
            continue;
        reportedAt = now;
        const engine::net::rendezvous::RelayStats stats = relay.stats();
        const bool same = saidOnce && stats.sessions == said.sessions && stats.relayed == said.relayed &&
                          stats.packets == said.packets && stats.dropped == said.dropped;
        if (same)
            continue;
        said = stats;
        saidOnce = true;
        const std::array<I18nArg, 7> args{I18nArg{"matches", static_cast<engine::core::i64>(stats.sessions)},
                                          I18nArg{"relayed", static_cast<engine::core::i64>(stats.relayed)},
                                          I18nArg{"bytes", static_cast<engine::core::i64>(stats.bytesPerSecond)},
                                          I18nArg{"packets", static_cast<engine::core::i64>(stats.packetsPerSecond)},
                                          I18nArg{"total", static_cast<engine::core::i64>(stats.bytes)},
                                          I18nArg{"dropped", static_cast<engine::core::i64>(stats.dropped)},
                                          I18nArg{"uptime", static_cast<engine::core::i64>(stats.uptimeSeconds)}};
        engine::core::log(LogLevel::Info, ENG_TR("relay.info.report"), args);
    }
    engine::core::log(LogLevel::Info, ENG_TR("relay.info.stopping"));
    relay.stop();
    return ExitOk;
}
