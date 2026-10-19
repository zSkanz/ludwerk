#include "engine/app/server_console.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>

#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/platform/console.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/replication/replication.h"
#include "engine/scene/players.h"

namespace engine::app {
namespace {

using Parameter = scene::EngineState::ConsoleParameter;

constexpr core::u64 NsPerSecond = 1'000'000'000ull;
constexpr core::u64 NsPerMinute = 60ull * NsPerSecond;
// A minute of ticks at sixty, for `status`.
constexpr std::size_t TicksKept = 3600;
// What a key is shown as in a list: enough to tell two apart and to type.
constexpr std::size_t FingerprintDigits = 16;
// The most minutes `stop` and `ban` take: a week, and a year.
constexpr core::f64 MostStopMinutes = 7.0 * 24.0 * 60.0;
constexpr core::f64 MostBanMinutes = 365.0 * 24.0 * 60.0;

struct EngineCommand
{
    std::string_view name;
    core::u8 permission;
    core::TextKey usage;
};

// ADR 0218 §2 and §3: who may run each.
const std::array<EngineCommand, 15> EngineCommands{{
    {"help", 0, ENG_TR("console.usage.help")},
    {"status", 0, ENG_TR("console.usage.status")},
    {"players", 0, ENG_TR("console.usage.players")},
    {"kick", 1, ENG_TR("console.usage.kick")},
    {"say", 1, ENG_TR("console.usage.say")},
    {"ban", 2, ENG_TR("console.usage.ban")},
    {"unban", 2, ENG_TR("console.usage.unban")},
    {"mute", 1, ENG_TR("console.usage.mute")},
    {"unmute", 1, ENG_TR("console.usage.unmute")},
    {"suspects", 1, ENG_TR("console.usage.suspects")},
    {"suspect", 1, ENG_TR("console.usage.suspect")},
    {"save", 2, ENG_TR("console.usage.save")},
    {"data", 3, ENG_TR("console.usage.data")},
    {"moves", 3, ENG_TR("console.usage.moves")},
    {"stop", 3, ENG_TR("console.usage.stop")},
}};

[[nodiscard]] std::string say(core::TextKey key, std::span<const core::I18nArg> args = {})
{
    return core::engineCatalog().format(key, args);
}

[[nodiscard]] std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

// The first word of `text`, and what is after it. A word in double quotes
// may hold spaces.
[[nodiscard]] std::pair<std::string, std::string_view> firstWord(std::string_view text)
{
    text = trim(text);
    if (text.empty())
        return {{}, {}};
    if (text.front() == '"') {
        const std::size_t close = text.find('"', 1);
        if (close != std::string_view::npos)
            return {std::string(text.substr(1, close - 1)), trim(text.substr(close + 1))};
    }
    std::size_t end = 0;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
        ++end;
    return {std::string(text.substr(0, end)), trim(text.substr(end))};
}

// `strtod` rather than `from_chars`: the floating-point overloads of
// `from_chars` are missing from one of the standard libraries the engine builds
// against (`core/json.cpp` has the same). The whole word has to convert.
[[nodiscard]] std::optional<core::f64> numberOf(std::string_view word)
{
    const std::string buffer(word);
    if (buffer.empty())
        return std::nullopt;
    char* end = nullptr;
    const core::f64 value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size())
        return std::nullopt;
    return value;
}

[[nodiscard]] bool hexWord(std::string_view word)
{
    return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

[[nodiscard]] std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A player of the match, as the console names them.
struct Seen
{
    std::string name;
    core::u32 userId = 0;
    std::string key;
    std::string account;
};

[[nodiscard]] std::vector<Seen> playersOf(const ServerConsole::Host& host)
{
    std::vector<Seen> out;
    const scene::World& world = *host.world;
    const core::InstanceId network = scene::networkServiceOf(world, host.dataModel);
    for (core::InstanceId child = network.valid() ? world.firstChild(network) : core::InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        const scene::PlayerComponent* player = world.players().find(child);
        if (player == nullptr || world.destroyed(child))
            continue;
        out.push_back(Seen{std::string(world.atoms().text(world.name(child))), player->userId, player->publicKey,
                           player->accountId});
    }
    return out;
}

// **`<player>`** (ADR 0218 §2): a name, a `UserId`, or the start of a key
// fingerprint. One that names two is refused with both.
[[nodiscard]] std::optional<Seen> findPlayer(const ServerConsole::Host& host, std::string_view word, std::string& error)
{
    std::vector<Seen> found;
    const std::string wanted = lower(word);
    const std::optional<core::f64> number = numberOf(word);
    for (const Seen& player : playersOf(host)) {
        const bool byName = lower(player.name) == wanted;
        const bool byNumber = number.has_value() && static_cast<core::f64>(player.userId) == *number;
        const bool byKey = word.size() >= 4 && hexWord(word) && player.key.starts_with(wanted);
        if (byName || byNumber || byKey)
            found.push_back(player);
    }
    if (found.size() == 1)
        return found.front();
    if (found.empty()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"player", word}};
        error = say(ENG_TR("console.err.no_player"), args);
        return std::nullopt;
    }
    std::string names;
    for (const Seen& player : found)
        names += (names.empty() ? "" : ", ") + player.name + " (" + std::to_string(player.userId) + ")";
    const std::array<core::I18nArg, 2> args{core::I18nArg{"player", word}, core::I18nArg{"players", names}};
    error = say(ENG_TR("console.err.ambiguous_player"), args);
    return std::nullopt;
}

[[nodiscard]] std::string fingerprint(std::string_view key)
{
    return key.empty() ? std::string("-") : std::string(key.substr(0, FingerprintDigits));
}

[[nodiscard]] std::string duration(core::u64 seconds)
{
    char text[32];
    if (seconds >= 3600)
        (void)std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                            static_cast<unsigned long long>(seconds / 60 % 60),
                            static_cast<unsigned long long>(seconds % 60));
    else
        (void)std::snprintf(text, sizeof(text), "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
                            static_cast<unsigned long long>(seconds % 60));
    return text;
}

[[nodiscard]] std::string typeName(Parameter::Type type)
{
    switch (type) {
    case Parameter::Type::Number:
        return "number";
    case Parameter::Type::Boolean:
        return "boolean";
    case Parameter::Type::Player:
        return "player";
    case Parameter::Type::String:
        break;
    }
    return "text";
}

[[nodiscard]] std::string usageOf(const scene::EngineState::ConsoleCommand& command)
{
    std::string usage = command.name;
    for (const Parameter& parameter : command.parameters)
        usage += parameter.optional ? " [" + parameter.name + "]" : " <" + parameter.name + ">";
    return usage;
}

} // namespace

// --- ConsoleInput --------------------------------------------------------------

// **Never what holds a process open**: the reader looks up from its wait
// every tenth of a second, and an ending process waits for it at most a
// second -- past that it is let go, which is safe because the read it may be
// in holds no lock anything else wants (see `readStandardInput`).
ConsoleInput::~ConsoleInput()
{
    if (!m_thread.joinable())
        return;
    m_shared->stopping = true;
    for (int waited = 0; waited < 100 && !m_shared->stopped; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (m_shared->stopped)
        m_thread.join();
    else
        m_thread.detach();
}

void ConsoleInput::start()
{
    if (m_started)
        return;
    m_started = true;
    m_thread = std::thread([shared = m_shared] {
        std::string pending;
        while (!shared->stopping) {
            const platform::InputRead read = platform::readStandardInput(pending, 100);
            if (read == platform::InputRead::Ended) {
                // A last line with no newline is still a line.
                if (!pending.empty()) {
                    const std::lock_guard lock(shared->mutex);
                    shared->lines.push_back(std::move(pending));
                }
                shared->ended = true;
                break;
            }
            for (std::size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const std::lock_guard lock(shared->mutex);
                shared->lines.push_back(std::move(line));
            }
        }
        shared->stopped = true;
    });
}

bool ConsoleInput::ended() const
{
    if (!m_started || !m_shared->ended)
        return false;
    const std::lock_guard lock(m_shared->mutex);
    return m_shared->lines.empty();
}

void ConsoleInput::take(std::vector<std::string>& out)
{
    const std::lock_guard lock(m_shared->mutex);
    while (!m_shared->lines.empty()) {
        out.push_back(std::move(m_shared->lines.front()));
        m_shared->lines.pop_front();
    }
}

// --- BanList -------------------------------------------------------------------

BanList::BanList(std::filesystem::path file) : m_file(std::move(file))
{
    load();
}

void BanList::refresh() const
{
    if (m_file.empty())
        return;
    std::error_code ec;
    const std::filesystem::file_time_type at = std::filesystem::last_write_time(m_file, ec);
    const std::uintmax_t size = ec ? 0 : std::filesystem::file_size(m_file, ec);
    if (ec || (at == m_readAt && size == m_readSize))
        return;
    load();
}

void BanList::load() const
{
    m_bans.clear();
    std::error_code ec;
    m_readAt = std::filesystem::last_write_time(m_file, ec);
    m_readSize = ec ? 0 : std::filesystem::file_size(m_file, ec);
    std::string text;
    if (m_file.empty() || !platform::readTextFile(m_file, text))
        return;
    core::JsonDocument document;
    if (!document.parse(text, m_file.string()))
        return;
    const core::JsonValue list = document.root()["bans"];
    for (core::usize index = 0; index < list.size(); ++index) {
        const core::JsonValue entry = list.at(index);
        Ban ban;
        ban.who = std::string(entry["who"].asString());
        ban.reason = std::string(entry["reason"].asString());
        ban.until = static_cast<std::int64_t>(entry["until"].asNumber());
        ban.by = std::string(entry["by"].asString());
        ban.at = static_cast<std::int64_t>(entry["at"].asNumber());
        if (!ban.who.empty())
            m_bans.push_back(std::move(ban));
    }
}

const BanList::Ban* BanList::find(std::string_view who, std::int64_t now) const
{
    refresh();
    for (const Ban& ban : m_bans) {
        if (ban.who == who && (ban.until == 0 || ban.until > now))
            return &ban;
    }
    return nullptr;
}

const std::vector<BanList::Ban>& BanList::all() const
{
    refresh();
    return m_bans;
}

namespace {

// **A change to the list is made on the file as it is now**, by one world at a
// time: held for the read, the change and the write.
void lockBans(const std::filesystem::path& file, platform::FileLock& lock)
{
    if (file.empty() || !platform::createDirectories(file.parent_path()))
        return;
    std::filesystem::path lockFile = file;
    lockFile += ".lock";
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (lock.tryLock(lockFile) != platform::FileLock::Taken::Busy)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

void BanList::add(Ban ban)
{
    platform::FileLock held;
    lockBans(m_file, held);
    load();
    std::erase_if(m_bans, [&](const Ban& other) { return other.who == ban.who; });
    m_bans.push_back(std::move(ban));
    save();
}

bool BanList::remove(std::string_view who)
{
    platform::FileLock held;
    lockBans(m_file, held);
    load();
    const std::size_t before = m_bans.size();
    std::erase_if(m_bans, [&](const Ban& ban) { return ban.who == who; });
    if (m_bans.size() == before)
        return false;
    save();
    return true;
}

void BanList::save() const
{
    if (m_file.empty())
        return;
    core::JsonWriter writer(core::JsonLayout::Indented);
    writer.beginObject();
    writer.key("bans");
    writer.beginArray();
    for (const Ban& ban : m_bans) {
        writer.beginObject();
        writer.field("who", std::string_view{ban.who});
        writer.field("reason", std::string_view{ban.reason});
        writer.field("until", static_cast<core::f64>(ban.until));
        writer.field("by", std::string_view{ban.by});
        writer.field("at", static_cast<core::f64>(ban.at));
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
    if (platform::createDirectories(m_file.parent_path()) && platform::writeTextFile(m_file, writer.text())) {
        std::error_code ec;
        m_readAt = std::filesystem::last_write_time(m_file, ec);
        m_readSize = ec ? 0 : std::filesystem::file_size(m_file, ec);
    }
}

namespace {

// Who a `mute`, `unmute` or `suspect` names, as a ban does: a key or an
// account written out, else an online player's account or key. Empty, with
// `error` said, when nobody.
[[nodiscard]] std::string whoOf(std::string_view target, const ServerConsole::Host& host, std::string& error)
{
    if (target.starts_with("key:") || target.starts_with("account:"))
        return std::string(target);
    if (target.size() == 64 && hexWord(target))
        return "key:" + lower(target);
    const std::optional<Seen> online = findPlayer(host, target, error);
    if (!online.has_value())
        return {};
    if (!online->account.empty())
        return "account:" + online->account;
    if (!online->key.empty())
        return "key:" + online->key;
    error = say(ENG_TR("console.err.ban_no_key"));
    return {};
}

} // namespace

// --- ServerConsole -------------------------------------------------------------

ServerConsole::ServerConsole(std::filesystem::path bansFile)
    : m_bans(bansFile), m_mutes(bansFile.empty() ? std::filesystem::path{} : bansFile.parent_path() / "mutes.json")
{}

ServerConsole::Health ServerConsole::health(const Host& host) const
{
    Health out;
    for (const core::f64 ms : m_ticks) {
        out.tickMeanMs += ms;
        out.tickWorstMs = std::max(out.tickWorstMs, ms);
    }
    out.tickMeanMs = m_ticks.empty() ? 0.0 : out.tickMeanMs / static_cast<core::f64>(m_ticks.size());
    out.ticksTotal = m_ticksTotal;
    out.tickMsTotal = m_tickMsTotal;
    out.uptimeSeconds = host.nowNs > m_startedNs ? (host.nowNs - m_startedNs) / NsPerSecond : 0;
    out.memoryBytes = platform::residentBytes();
    out.stopping = m_phase != Phase::Running;
    if (host.world != nullptr) {
        const scene::EngineState& state = host.world->engineState();
        out.players = playersOf(host).size();
        out.maxPlayers = state.networkMaxPlayers;
        out.scene = state.currentScene;
    }
    return out;
}

void ServerConsole::noteTick(core::f64 ms)
{
    m_ticksTotal += 1;
    m_tickMsTotal += ms;
    m_ticks.push_back(ms);
    while (m_ticks.size() > TicksKept)
        m_ticks.pop_front();
}

std::optional<replication::JoinAnswer> ServerConsole::refusal(const replication::JoinAsk& ask,
                                                              std::int64_t nowUnix) const
{
    if (m_phase == Phase::Closing || m_phase == Phase::Finished)
        return replication::JoinAnswer{
            .admit = false, .reason = say(ENG_TR("console.stopping_reason")), .accountId = {}};
    if (!ask.publicKey.empty()) {
        if (const BanList::Ban* ban = m_bans.find("key:" + ask.publicKey, nowUnix); ban != nullptr) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"reason", std::string_view{ban->reason}}};
            return replication::JoinAnswer{.admit = false,
                                           .reason = ban->reason.empty() ? say(ENG_TR("console.banned"))
                                                                         : say(ENG_TR("console.banned_for"), args),
                                           .accountId = {}};
        }
    }
    return std::nullopt;
}

std::optional<replication::JoinAnswer> ServerConsole::accountRefusal(const std::string& account,
                                                                     std::int64_t nowUnix) const
{
    const BanList::Ban* ban = m_bans.find("account:" + account, nowUnix);
    if (ban == nullptr)
        return std::nullopt;
    const std::array<core::I18nArg, 1> args{core::I18nArg{"reason", std::string_view{ban->reason}}};
    return replication::JoinAnswer{.admit = false,
                                   .reason = ban->reason.empty() ? say(ENG_TR("console.banned"))
                                                                 : say(ENG_TR("console.banned_for"), args),
                                   .accountId = {}};
}

void ServerConsole::answer(const Reply& reply, std::string_view line, const Caller& caller, const std::string& text,
                           bool failed) const
{
    // **Said in the log, with who ran it** (ADR 0218 §4).
    const std::string who = caller.local ? caller.name : caller.name + " (" + fingerprint(caller.key) + ")";
    const std::array<core::I18nArg, 3> args{core::I18nArg{"who", std::string_view{who}}, core::I18nArg{"line", line},
                                            core::I18nArg{"answer", std::string_view{text}}};
    core::log(failed ? core::LogLevel::Warn : core::LogLevel::Info, ENG_TR("console.info.ran"), args);
    if (reply)
        reply(text, failed);
}

void ServerConsole::announce(const Host& host, const std::string& text) const
{
    if (host.replication != nullptr)
        host.replication->announce(text);
    // And this machine's own scripts -- a host's players are here too.
    host.world->engineState().serverMessages.push_back(text);
}

void ServerConsole::beginClosing(const Host& host)
{
    m_phase = Phase::Closing;
    m_closingFrames = 0;
    if (host.flushSaves)
        host.flushSaves();
    const std::string reason = say(ENG_TR("console.stopping_reason"));
    for (const Seen& player : playersOf(host))
        host.world->engineState().pendingRemovals.emplace_back(player.userId, reason);
    core::log(core::LogLevel::Info, ENG_TR("console.info.stopping"));
}

void ServerConsole::run(std::string_view line, const Caller& caller, Reply reply, const Host& host)
{
    if (m_startedNs == 0)
        m_startedNs = host.nowNs;
    const std::string_view whole = trim(line);
    if (whole.empty())
        return;
    const auto [word, rest] = firstWord(whole);
    const std::string name = lower(word);

    if (const auto engine = std::find_if(EngineCommands.begin(), EngineCommands.end(),
                                         [&](const EngineCommand& c) { return c.name == name; });
        engine != EngineCommands.end()) {
        if (caller.permission < engine->permission) {
            answer(reply, whole, caller, say(ENG_TR("console.err.permission")), true);
            return;
        }
        runEngineCommand(name, rest, caller, reply, whole, host);
        return;
    }

    std::vector<scene::EngineState::ConsoleCommand>& commands = host.world->engineState().consoleCommands;
    const auto game = std::find_if(commands.begin(), commands.end(),
                                   [&](const scene::EngineState::ConsoleCommand& c) { return c.name == name; });
    if (game == commands.end()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{name}}};
        answer(reply, whole, caller, say(ENG_TR("console.err.unknown_command"), args), true);
        return;
    }
    if (caller.permission < game->permission) {
        answer(reply, whole, caller, say(ENG_TR("console.err.permission")), true);
        return;
    }
    // **Parsed by what the game said it takes**: a line that does not fit
    // is answered with the usage, and the function is not called.
    scene::EngineState::ConsoleCall call;
    call.name = name;
    call.caller = caller;
    std::string_view remaining = rest;
    std::string problem;
    for (std::size_t at = 0; at < game->parameters.size() && problem.empty(); ++at) {
        const Parameter& parameter = game->parameters[at];
        scene::EngineState::ConsoleArgument argument;
        // The last text parameter takes what is left of the line.
        const bool last = at + 1 == game->parameters.size();
        if (remaining.empty()) {
            if (!parameter.optional)
                problem = "missing";
            call.arguments.push_back(argument);
            continue;
        }
        std::string text;
        if (last && parameter.type == Parameter::Type::String && !remaining.starts_with('"')) {
            text = std::string(remaining);
            remaining = {};
        }
        else {
            auto [next, after] = firstWord(remaining);
            text = std::move(next);
            remaining = after;
        }
        argument.present = true;
        switch (parameter.type) {
        case Parameter::Type::String:
            argument.text = std::move(text);
            break;
        case Parameter::Type::Number:
            if (const std::optional<core::f64> number = numberOf(text); number.has_value())
                argument.number = *number;
            else
                problem = "number";
            break;
        case Parameter::Type::Boolean: {
            const std::string flag = lower(text);
            if (flag == "true" || flag == "yes" || flag == "on" || flag == "1")
                argument.flag = true;
            else if (!(flag == "false" || flag == "no" || flag == "off" || flag == "0"))
                problem = "boolean";
            break;
        }
        case Parameter::Type::Player: {
            std::string error;
            if (const std::optional<Seen> player = findPlayer(host, text, error); player.has_value())
                argument.userId = player->userId;
            else
                problem = std::move(error);
            break;
        }
        }
        call.arguments.push_back(std::move(argument));
    }
    if (problem.empty() && !remaining.empty())
        problem = "extra";
    if (!problem.empty()) {
        const std::string usage = usageOf(*game);
        const std::array<core::I18nArg, 1> args{core::I18nArg{"usage", std::string_view{usage}}};
        std::string text = say(ENG_TR("console.err.usage"), args);
        // A player who could not be found says why, before the usage.
        if (problem != "missing" && problem != "number" && problem != "boolean" && problem != "extra")
            text = problem + "\n" + text;
        answer(reply, whole, caller, text, true);
        return;
    }
    call.id = m_nextCall++;
    if (m_nextCall >= 0x80000000u)
        m_nextCall = 1;
    m_pending[call.id] = Pending{std::move(reply), std::string(whole), caller};
    host.world->engineState().consoleCalls.push_back(std::move(call));
}

void ServerConsole::runEngineCommand(std::string_view name, std::string_view rest, const Caller& caller,
                                     const Reply& reply, std::string_view line, const Host& host)
{
    scene::EngineState& state = host.world->engineState();
    if (name == "help") {
        const std::string wanted = lower(firstWord(rest).first);
        std::string text;
        for (const EngineCommand& command : EngineCommands) {
            if (!wanted.empty() && wanted != command.name)
                continue;
            text += (text.empty() ? "" : "\n") + say(command.usage);
        }
        for (const scene::EngineState::ConsoleCommand& command : state.consoleCommands) {
            if (!wanted.empty() && wanted != command.name)
                continue;
            std::string row = usageOf(command);
            if (!command.description.empty())
                row += " -- " + command.description;
            if (!wanted.empty()) {
                for (const Parameter& parameter : command.parameters)
                    row += "\n  " + parameter.name + ": " + typeName(parameter.type);
            }
            text += (text.empty() ? "" : "\n") + row;
        }
        if (text.empty()) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{wanted}}};
            answer(reply, line, caller, say(ENG_TR("console.err.unknown_command"), args), true);
            return;
        }
        answer(reply, line, caller, text, false);
        return;
    }
    if (name == "status") {
        const Health now = health(host);
        const std::string phase =
            now.stopping ? say(ENG_TR("console.state.stopping")) : say(ENG_TR("console.state.running"));
        const std::string uptime = duration(now.uptimeSeconds);
        // A game whose world is built by its scripts has no scene file.
        const std::string scene = now.scene.empty() ? say(ENG_TR("console.status.no_scene")) : now.scene;
        const std::array<core::I18nArg, 8> args{
            core::I18nArg{"uptime", std::string_view{uptime}},
            core::I18nArg{"mean", now.tickMeanMs},
            core::I18nArg{"worst", now.tickWorstMs},
            core::I18nArg{"players", static_cast<core::i64>(now.players)},
            core::I18nArg{"limit", static_cast<core::i64>(now.maxPlayers)},
            core::I18nArg{"memory", static_cast<core::i64>(now.memoryBytes / (1024u * 1024u))},
            core::I18nArg{"scene", std::string_view{scene}},
            core::I18nArg{"state", std::string_view{phase}}};
        answer(reply, line, caller, say(ENG_TR("console.status"), args), false);
        return;
    }
    if (name == "players") {
        const std::vector<Seen> seen = playersOf(host);
        if (seen.empty()) {
            answer(reply, line, caller, say(ENG_TR("console.players.none")), false);
            return;
        }
        const std::vector<replication::PeerSummary> links =
            host.replication != nullptr ? host.replication->players() : std::vector<replication::PeerSummary>{};
        const core::u64 tick = state.tick;
        std::string text;
        for (const Seen& player : seen) {
            const auto link = std::find_if(links.begin(), links.end(), [&](const replication::PeerSummary& p) {
                return p.userId == player.userId;
            });
            const std::string key = fingerprint(player.key);
            const std::string address = link != links.end() && !link->address.empty() ? link->address : "-";
            const std::string on =
                link != links.end() && tick > link->joinedTick ? duration((tick - link->joinedTick) / 60) : "-";
            const std::array<core::I18nArg, 6> args{
                core::I18nArg{"name", std::string_view{player.name}},
                core::I18nArg{"userId", static_cast<core::i64>(player.userId)},
                core::I18nArg{"key", std::string_view{key}},
                core::I18nArg{"address", std::string_view{address}},
                core::I18nArg{"ping", static_cast<core::i64>(link != links.end() ? link->pingMs : 0)},
                core::I18nArg{"time", std::string_view{on}}};
            text += (text.empty() ? "" : "\n") + say(ENG_TR("console.players.row"), args);
        }
        answer(reply, line, caller, text, false);
        return;
    }
    if (name == "kick") {
        const auto [target, reason] = firstWord(rest);
        std::string error;
        const std::optional<Seen> player = target.empty() ? std::nullopt : findPlayer(host, target, error);
        if (!player.has_value()) {
            answer(reply, line, caller, error.empty() ? say(ENG_TR("console.usage.kick")) : error, true);
            return;
        }
        state.pendingRemovals.emplace_back(player->userId, std::string(reason));
        const std::array<core::I18nArg, 1> args{core::I18nArg{"name", std::string_view{player->name}}};
        answer(reply, line, caller, say(ENG_TR("console.done.kicked"), args), false);
        return;
    }
    if (name == "ban") {
        auto [target, after] = firstWord(rest);
        if (target.empty()) {
            answer(reply, line, caller, say(ENG_TR("console.usage.ban")), true);
            return;
        }
        // **By key** (ADR 0218 §2): an online player is their key; a key
        // written whole -- or as `key:` and it -- is one who is not online.
        std::string who;
        std::optional<Seen> online;
        std::string error;
        if (target.starts_with("key:") || target.starts_with("account:")) {
            who = target;
        }
        else if (target.size() == 64 && hexWord(target)) {
            who = "key:" + lower(target);
        }
        else if (online = findPlayer(host, target, error); online.has_value()) {
            // **An account where the game named one** (ADR 0226 §5): the ban
            // that holds, whatever key the account comes back with.
            if (!online->account.empty()) {
                who = "account:" + online->account;
            }
            else if (online->key.empty()) {
                answer(reply, line, caller, say(ENG_TR("console.err.ban_no_key")), true);
                return;
            }
            else {
                who = "key:" + online->key;
            }
        }
        else {
            answer(reply, line, caller, error, true);
            return;
        }
        std::int64_t until = 0;
        const auto [maybeMinutes, afterMinutes] = firstWord(after);
        std::string_view reason = after;
        if (const std::optional<core::f64> minutes = numberOf(maybeMinutes); minutes.has_value()) {
            if (*minutes <= 0.0 || *minutes > MostBanMinutes) {
                answer(reply, line, caller, say(ENG_TR("console.usage.ban")), true);
                return;
            }
            until = host.nowUnix + static_cast<std::int64_t>(*minutes * 60.0);
            reason = afterMinutes;
        }
        const std::string by = caller.local ? caller.name : caller.name + " " + fingerprint(caller.key);
        m_bans.add(BanList::Ban{who, std::string(reason), until, by, host.nowUnix});
        // And out, if they are on.
        if (!online.has_value()) {
            for (const Seen& player : playersOf(host)) {
                if ((!player.key.empty() && who == "key:" + player.key) ||
                    (!player.account.empty() && who == "account:" + player.account))
                    online = player;
            }
        }
        if (online.has_value()) {
            const std::array<core::I18nArg, 1> reasonArgs{core::I18nArg{"reason", reason}};
            state.pendingRemovals.emplace_back(online->userId, reason.empty()
                                                                   ? say(ENG_TR("console.banned"))
                                                                   : say(ENG_TR("console.banned_for"), reasonArgs));
        }
        const std::array<core::I18nArg, 2> args{core::I18nArg{"who", std::string_view{who}},
                                                core::I18nArg{"by", std::string_view{by}}};
        core::log(core::LogLevel::Info, ENG_TR("console.info.banned"), args);
        answer(reply, line, caller,
               say(until == 0 ? ENG_TR("console.done.banned") : ENG_TR("console.done.banned_for_now"), args), false);
        return;
    }
    if (name == "unban") {
        std::string target = firstWord(rest).first;
        if (target.size() == 64 && hexWord(target))
            target = "key:" + lower(target);
        const std::array<core::I18nArg, 1> args{core::I18nArg{"who", std::string_view{target}}};
        if (target.empty() || !m_bans.remove(target)) {
            answer(reply, line, caller, say(ENG_TR("console.err.not_banned"), args), true);
            return;
        }
        answer(reply, line, caller, say(ENG_TR("console.done.unbanned"), args), false);
        return;
    }
    // **Mute and unmute** (ADR 0241 §4): named as a ban names a player, for
    // as long as it says; what a mute stops is the game's (`IsMuted`).
    if (name == "mute") {
        const auto [target, after] = firstWord(rest);
        std::string error;
        const std::string who = target.empty() ? std::string{} : whoOf(target, host, error);
        if (who.empty()) {
            answer(reply, line, caller, error.empty() ? say(ENG_TR("console.usage.mute")) : error, true);
            return;
        }
        std::int64_t until = 0;
        const auto [maybeMinutes, afterMinutes] = firstWord(after);
        std::string_view reason = after;
        if (const std::optional<core::f64> minutes = numberOf(maybeMinutes); minutes.has_value()) {
            if (*minutes <= 0.0 || *minutes > MostBanMinutes) {
                answer(reply, line, caller, say(ENG_TR("console.usage.mute")), true);
                return;
            }
            until = host.nowUnix + static_cast<std::int64_t>(*minutes * 60.0);
            reason = afterMinutes;
        }
        const std::string by = caller.local ? caller.name : caller.name + " " + fingerprint(caller.key);
        m_mutes.add(BanList::Ban{who, std::string(reason), until, by, host.nowUnix});
        m_mutesReadNs = 0;
        const std::array<core::I18nArg, 1> args{core::I18nArg{"who", std::string_view{who}}};
        answer(reply, line, caller, say(ENG_TR("console.done.muted"), args), false);
        return;
    }
    if (name == "unmute") {
        const std::string target(firstWord(rest).first);
        std::string error;
        const std::string who = target.empty() ? std::string{} : whoOf(target, host, error);
        const std::array<core::I18nArg, 1> args{core::I18nArg{"who", std::string_view{who.empty() ? target : who}}};
        if (who.empty() || !m_mutes.remove(who)) {
            answer(reply, line, caller, say(ENG_TR("console.err.not_muted"), args), true);
            return;
        }
        m_mutesReadNs = 0;
        answer(reply, line, caller, say(ENG_TR("console.done.unmuted"), args), false);
        return;
    }
    // **Who went past a limit** (ADR 0241 §3): the ten players with the most
    // suspicions in the last hour, or one player's, line by line.
    if (name == "suspects" || name == "suspect") {
        const core::f64 step = std::max(state.fixedTimestep, 1.0e-3);
        const core::u64 hour = static_cast<core::u64>(3600.0 / step);
        const core::u64 since = state.tick > hour ? state.tick - hour : 0;
        std::string text;
        if (name == "suspects") {
            std::map<std::string, core::u64> counts;
            for (const scene::EngineState::Suspicion& each : state.suspicions) {
                if (each.tick >= since && !each.who.empty())
                    counts[each.who] += 1;
            }
            std::vector<std::pair<core::u64, std::string>> ranked;
            for (const auto& [someone, count] : counts)
                ranked.emplace_back(count, someone);
            std::sort(ranked.begin(), ranked.end(), std::greater<>());
            for (core::usize at = 0; at < ranked.size() && at < 10; ++at) {
                const std::array<core::I18nArg, 2> args{
                    core::I18nArg{"who", std::string_view{ranked[at].second}},
                    core::I18nArg{"count", static_cast<core::i64>(ranked[at].first)}};
                text += say(ENG_TR("console.say.suspect_count"), args) + "\n";
            }
        }
        else {
            const std::string target(firstWord(rest).first);
            std::string error;
            const std::string who = target.empty() ? std::string{} : whoOf(target, host, error);
            if (who.empty()) {
                answer(reply, line, caller, error.empty() ? say(ENG_TR("console.usage.suspect")) : error, true);
                return;
            }
            for (const scene::EngineState::Suspicion& each : state.suspicions) {
                if (each.tick < since || each.who != who)
                    continue;
                const std::array<core::I18nArg, 3> args{core::I18nArg{"tick", static_cast<core::i64>(each.tick)},
                                                        core::I18nArg{"kind", std::string_view{each.kind}},
                                                        core::I18nArg{"detail", std::string_view{each.detail}}};
                text += say(ENG_TR("console.say.suspect_line"), args) + "\n";
            }
        }
        if (text.empty())
            text = say(ENG_TR("console.say.no_suspects"));
        else
            text.pop_back();
        answer(reply, line, caller, text, false);
        return;
    }
    if (name == "say") {
        if (rest.empty()) {
            answer(reply, line, caller, say(ENG_TR("console.usage.say")), true);
            return;
        }
        announce(host, std::string(rest));
        answer(reply, line, caller, say(ENG_TR("console.done.said")), false);
        return;
    }
    if (name == "data" || name == "moves") {
        if (!host.data) {
            answer(reply, line, caller, say(ENG_TR("console.err.data_at_manager")), true);
            return;
        }
        const auto [text, failed] = host.data(line);
        answer(reply, line, caller, text, failed);
        return;
    }
    if (name == "save") {
        if (host.flushSaves)
            host.flushSaves();
        answer(reply, line, caller, say(ENG_TR("console.done.saved")), false);
        return;
    }
    if (name == "stop") {
        // **A second `stop` cancels a countdown.**
        if (m_phase == Phase::Counting && rest.empty()) {
            m_phase = Phase::Running;
            announce(host, say(ENG_TR("console.stop_cancelled")));
            answer(reply, line, caller, say(ENG_TR("console.done.stop_cancelled")), false);
            return;
        }
        if (m_phase == Phase::Closing || m_phase == Phase::Finished) {
            answer(reply, line, caller, say(ENG_TR("console.done.stopping")), false);
            return;
        }
        core::f64 minutes = 0.0;
        if (!rest.empty()) {
            const std::optional<core::f64> asked = numberOf(firstWord(rest).first);
            if (!asked.has_value() || *asked < 0.0 || *asked > MostStopMinutes) {
                answer(reply, line, caller, say(ENG_TR("console.usage.stop")), true);
                return;
            }
            minutes = *asked;
        }
        if (minutes <= 0.0) {
            announce(host, say(ENG_TR("console.stopping_now")));
            beginClosing(host);
            answer(reply, line, caller, say(ENG_TR("console.done.stopping")), false);
            return;
        }
        m_phase = Phase::Counting;
        m_stopAtNs = host.nowNs + static_cast<core::u64>(minutes * static_cast<core::f64>(NsPerMinute));
        m_nextWarningNs = host.nowNs;
        const std::array<core::I18nArg, 1> args{core::I18nArg{"minutes", minutes}};
        answer(reply, line, caller, say(ENG_TR("console.done.stop_counting"), args), false);
        return;
    }
}

void ServerConsole::frame(const Host& host)
{
    if (m_startedNs == 0)
        m_startedNs = host.nowNs;
    scene::EngineState& state = host.world->engineState();
    // **Who is muted now**, for `NetworkService:IsMuted`: a mute another
    // world of the manager wrote reaches this one within a second.
    constexpr core::u64 MutesEveryNs = 1'000'000'000;
    if (m_mutesWorld != host.world || m_mutesReadNs == 0 || host.nowNs - m_mutesReadNs >= MutesEveryNs) {
        m_mutesWorld = host.world;
        m_mutesReadNs = std::max<core::u64>(host.nowNs, 1);
        state.muted.clear();
        for (const BanList::Ban& mute : m_mutes.all()) {
            if (mute.until == 0 || mute.until > host.nowUnix)
                state.muted.push_back(mute.who);
        }
    }
    // What the game's commands answered.
    std::vector<scene::EngineState::ConsoleAnswer> answers;
    answers.swap(state.consoleAnswers);
    for (const scene::EngineState::ConsoleAnswer& said : answers) {
        const auto pending = m_pending.find(said.id);
        if (pending == m_pending.end())
            continue;
        const Pending call = std::move(pending->second);
        m_pending.erase(pending);
        answer(call.reply, call.line, call.caller, said.text, said.failed);
    }
    // A script's own lines, run as the local console with the script as
    // caller.
    std::vector<scene::EngineState::ConsoleLine> lines;
    lines.swap(state.consoleLines);
    for (const scene::EngineState::ConsoleLine& asked : lines) {
        const core::u32 id = asked.id;
        scene::World* world = host.world;
        const Caller caller{.local = true, .name = "script", .key = {}, .permission = 3};
        run(
            asked.line, caller,
            [world, id](const std::string& text, bool failed) {
                world->engineState().consoleLineAnswers.push_back(
                    scene::EngineState::ConsoleAnswer{.id = id, .text = text, .failed = failed});
            },
            host);
    }
    // **`stop` counting down**: said at the start and each minute after.
    if (m_phase == Phase::Counting) {
        if (host.nowNs >= m_stopAtNs) {
            announce(host, say(ENG_TR("console.stopping_now")));
            beginClosing(host);
        }
        else if (host.nowNs >= m_nextWarningNs) {
            const core::u64 left = (m_stopAtNs - host.nowNs + NsPerMinute - 1) / NsPerMinute;
            const std::array<core::I18nArg, 1> args{core::I18nArg{"minutes", static_cast<core::i64>(left)}};
            announce(host, say(ENG_TR("console.stopping_in"), args));
            m_nextWarningNs += NsPerMinute;
        }
    }
    else if (m_phase == Phase::Closing) {
        // The removals go out with the next frames' sends, then the process
        // ends.
        if (++m_closingFrames >= 3)
            m_phase = Phase::Finished;
    }
}

} // namespace engine::app
