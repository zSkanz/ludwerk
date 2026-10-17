#include "engine/app/admin_server.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>

#include "engine/app/admin_channel.h"
#include "engine/app/match_keys.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/toml.h"
#include "engine/net/tcp.h"

namespace engine::app {

struct AdminListener
{
    net::TcpListener listener;
};

namespace {

// ADR 0219 §4.
constexpr core::usize MostSessions = 4;
constexpr std::size_t FailuresAllowed = 5;
constexpr std::int64_t FailureWindowSeconds = 60;
constexpr std::int64_t BlockSeconds = 600;
constexpr std::int64_t IdleSeconds = 600;

[[nodiscard]] std::int64_t secondsNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::optional<std::uint32_t> ipv4(std::string_view text)
{
    std::uint32_t value = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t dot = part < 3 ? text.find('.') : text.size();
        if (dot == std::string_view::npos)
            return std::nullopt;
        unsigned octet = 0;
        const std::string_view piece = text.substr(0, dot);
        const auto [end, failed] = std::from_chars(piece.data(), piece.data() + piece.size(), octet);
        if (failed != std::errc{} || end != piece.data() + piece.size() || octet > 255)
            return std::nullopt;
        value = value << 8 | octet;
        text.remove_prefix(std::min(text.size(), dot + 1));
    }
    return value;
}

} // namespace

struct AdminServer::Session
{
    core::u32 id = 0;
    std::string address;
    std::string name;
    std::string fingerprint;
    core::u8 level = 0;
    std::optional<AdminChannel> channel;
    std::mutex mutex;
    std::deque<std::pair<AdminKind, std::string>> outbox;
    bool follow = false;
    std::atomic<bool> done{false};
    std::thread thread;
};

std::vector<AdminEntry> readAdmins(std::string_view text, std::string_view source,
                                   std::optional<core::EngineError>& error)
{
    core::TomlDocument document;
    if (const core::TomlDocument::ParseResult parsed = document.parse(text, source); !parsed) {
        const std::array<core::I18nArg, 2> args{core::I18nArg{"path", source},
                                                core::I18nArg{"detail", std::string_view{parsed.diagnostic}}};
        error = core::makeError(ENG_TR("admin.err.admins_unreadable"), args);
        return {};
    }
    std::vector<AdminEntry> admins;
    for (core::usize at = 0; at < document.count("admin"); ++at) {
        const std::string prefix = "admin." + std::to_string(at) + ".";
        AdminEntry entry;
        entry.name = std::string(document.string(prefix + "name").value_or(""));
        const std::optional<std::array<core::u8, 32>> key =
            matchKeyFromHex(document.string(prefix + "key").value_or(""));
        const core::f64 level = document.number(prefix + "level").value_or(3.0);
        if (entry.name.empty() || !key.has_value() || level < 0.0 || level > 3.0) {
            const std::array<core::I18nArg, 2> args{core::I18nArg{"path", source},
                                                    core::I18nArg{"detail", std::string_view{entry.name}}};
            error = core::makeError(ENG_TR("admin.err.admins_entry"), args);
            return {};
        }
        entry.key = *key;
        entry.level = static_cast<core::u8>(level);
        admins.push_back(std::move(entry));
    }
    return admins;
}

bool addressAllowed(std::string_view address, const std::vector<std::string>& allow)
{
    const std::optional<std::uint32_t> from = ipv4(address);
    if (!from.has_value())
        return false;
    if (*from >> 24 == 127)
        return true;
    for (const std::string& entry : allow) {
        const std::size_t slash = entry.find('/');
        const std::optional<std::uint32_t> base = ipv4(std::string_view{entry}.substr(0, slash));
        unsigned bits = 32;
        if (slash != std::string::npos) {
            const std::string_view count = std::string_view{entry}.substr(slash + 1);
            if (std::from_chars(count.data(), count.data() + count.size(), bits).ec != std::errc{} || bits > 32)
                continue;
        }
        if (!base.has_value())
            continue;
        const std::uint32_t mask = bits == 0 ? 0u : ~0u << (32 - bits);
        if ((*from & mask) == (*base & mask))
            return true;
    }
    return false;
}

AdminServer::AdminServer() = default;

AdminServer::~AdminServer()
{
    stop();
}

std::optional<core::EngineError> AdminServer::start(Options options)
{
    m_options = std::move(options);
    m_listener = std::make_unique<AdminListener>();
    if (auto error = m_listener->listener.open(m_options.port, !m_options.allow.empty()); error.has_value())
        return error;
    if (m_options.admins.empty()) {
        const std::array<core::I18nArg, 1> nobody{core::I18nArg{"path", std::string_view{m_options.adminsSource}}};
        core::log(core::LogLevel::Warn, ENG_TR("admin.warn.nobody"), nobody);
    }
    const std::array<core::I18nArg, 2> args{core::I18nArg{"port", static_cast<core::i64>(m_options.port)},
                                            core::I18nArg{"key", core::toHex(m_options.serverKey.pub)}};
    core::log(core::LogLevel::Info, ENG_TR("admin.info.listening"), args);
    // **Following the log**: each line to every administrator who asked.
    core::setLogTap(
        [this](core::LogLevel level, std::string_view text) { followed(core::formatLogLine(level, text)); });
    m_acceptor = std::thread([this] { acceptLoop(); });
    return std::nullopt;
}

void AdminServer::stop()
{
    if (m_stopping.exchange(true))
        return;
    core::setLogTap(nullptr);
    if (m_acceptor.joinable())
        m_acceptor.join();
    std::vector<std::shared_ptr<Session>> sessions;
    {
        const std::lock_guard lock(m_mutex);
        sessions = m_sessions;
    }
    for (const std::shared_ptr<Session>& session : sessions) {
        session->done = true;
        if (session->thread.joinable())
            session->thread.join();
    }
    if (m_listener != nullptr)
        m_listener->listener.close();
}

core::usize AdminServer::sessions() const
{
    const std::lock_guard lock(m_mutex);
    return static_cast<core::usize>(
        std::count_if(m_sessions.begin(), m_sessions.end(), [](const auto& s) { return !s->done; }));
}

void AdminServer::followed(std::string_view line)
{
    const std::lock_guard lock(m_mutex);
    for (const std::shared_ptr<Session>& session : m_sessions) {
        const std::lock_guard sessionLock(session->mutex);
        if (session->follow && !session->done)
            session->outbox.emplace_back(AdminKind::LogLine, std::string(line));
    }
}

void AdminServer::acceptLoop()
{
    while (!m_stopping) {
        std::string address;
        std::optional<net::TcpStream> stream = m_listener->listener.accept(200, address);
        {
            // Sessions that ended are let go.
            const std::lock_guard lock(m_mutex);
            for (auto at = m_sessions.begin(); at != m_sessions.end();) {
                if ((*at)->done && (*at)->thread.joinable()) {
                    (*at)->thread.join();
                    at = m_sessions.erase(at);
                }
                else {
                    ++at;
                }
            }
        }
        if (!stream.has_value())
            continue;
        const std::int64_t now = secondsNow();
        bool refuse = !addressAllowed(address, m_options.allow);
        {
            const std::lock_guard lock(m_mutex);
            if (const auto blocked = m_blockedUntil.find(address);
                blocked != m_blockedUntil.end() && blocked->second > now)
                refuse = true;
            if (m_sessions.size() >= MostSessions)
                refuse = true;
        }
        // **Closed before a byte of it is read.**
        if (refuse) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{address}}};
            core::log(core::LogLevel::Info, ENG_TR("admin.info.refused_address"), args);
            stream->close();
            continue;
        }
        auto session = std::make_shared<Session>();
        session->address = address;
        {
            const std::lock_guard lock(m_mutex);
            session->id = m_nextSession++;
            m_sessions.push_back(session);
        }
        session->thread = std::thread([this, session, stream = std::move(*stream)]() mutable {
            std::array<core::u8, 32> key{};
            std::optional<core::EngineError> error;
            session->channel =
                AdminChannel::accept(std::move(stream), m_options.serverKey, m_options.prologue, key, error);
            const auto admin = std::find_if(m_options.admins.begin(), m_options.admins.end(),
                                            [&](const AdminEntry& entry) { return entry.key == key; });
            if (!session->channel.has_value() || admin == m_options.admins.end()) {
                // **A failed handshake counts against its address.**
                const std::int64_t now = secondsNow();
                {
                    const std::lock_guard lock(m_mutex);
                    std::vector<std::int64_t>& failures = m_failures[session->address];
                    std::erase_if(failures, [now](std::int64_t at) { return at + FailureWindowSeconds < now; });
                    failures.push_back(now);
                    if (failures.size() >= FailuresAllowed)
                        m_blockedUntil[session->address] = now + BlockSeconds;
                }
                const std::array<core::I18nArg, 1> args{core::I18nArg{"address", std::string_view{session->address}}};
                core::log(core::LogLevel::Warn, ENG_TR("admin.warn.refused_key"), args);
                session->done = true;
                return;
            }
            session->name = admin->name;
            session->fingerprint = core::toHex(key).substr(0, 16);
            session->level = admin->level;
            serve(session);
        });
    }
}

void AdminServer::serve(std::shared_ptr<Session> session)
{
    const std::array<core::I18nArg, 3> args{core::I18nArg{"name", std::string_view{session->name}},
                                            core::I18nArg{"key", std::string_view{session->fingerprint}},
                                            core::I18nArg{"address", std::string_view{session->address}}};
    core::log(core::LogLevel::Info, ENG_TR("admin.info.session_started"), args);
    std::int64_t lastHeard = secondsNow();
    while (!session->done && !m_stopping) {
        // What is waiting for it.
        std::deque<std::pair<AdminKind, std::string>> sending;
        {
            const std::lock_guard lock(session->mutex);
            sending.swap(session->outbox);
        }
        bool broken = false;
        for (const auto& [kind, text] : sending) {
            if (session->channel->send(kind, text).has_value()) {
                broken = true;
                break;
            }
        }
        if (broken)
            break;
        AdminKind kind = AdminKind::Line;
        std::string text;
        bool got = false;
        if (session->channel->receive(kind, text, got, 50).has_value())
            break;
        if (!got) {
            if (secondsNow() > lastHeard + IdleSeconds)
                break;
            continue;
        }
        lastHeard = secondsNow();
        if (kind == AdminKind::Follow) {
            const std::lock_guard lock(session->mutex);
            session->follow = true;
            continue;
        }
        if (kind != AdminKind::Line)
            continue;
        if (text.size() > MaxAdminLine)
            text.resize(MaxAdminLine);
        const std::lock_guard lock(m_mutex);
        m_lines.push_back(Line{
            session->id, std::move(text),
            ServerConsole::Caller{
                .local = false, .name = session->name, .key = session->fingerprint, .permission = session->level}});
    }
    // Said before it is marked done: whoever lets it go joins it holding the
    // lock a log line's followers are found under.
    core::log(core::LogLevel::Info, ENG_TR("admin.info.session_ended"), args);
    session->done = true;
}

void AdminServer::take(std::vector<Line>& out)
{
    const std::lock_guard lock(m_mutex);
    for (Line& line : m_lines)
        out.push_back(std::move(line));
    m_lines.clear();
}

void AdminServer::answer(core::u32 session, const std::string& text, bool failed)
{
    const std::lock_guard lock(m_mutex);
    for (const std::shared_ptr<Session>& one : m_sessions) {
        if (one->id != session)
            continue;
        const std::lock_guard sessionLock(one->mutex);
        // **An answer is cut at its limit**, with a note.
        std::string said = text;
        if (said.size() > MaxAdminMessage - 64)
            said =
                said.substr(0, MaxAdminMessage - 64) + "\n" + core::engineCatalog().format(ENG_TR("admin.answer_cut"));
        one->outbox.emplace_back(failed ? AdminKind::Failed : AdminKind::Answer, std::move(said));
    }
}

} // namespace engine::app
