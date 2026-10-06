#include "engine/net/rendezvous.h"

#include <algorithm>
#include <charconv>
#include <utility>

#include "engine/core/crypto.h"

namespace engine::net::rendezvous {

namespace {

// Thirty-two characters, five bits each: the digits and capitals less 0, 1, I
// and O, which a person reads as each other.
constexpr std::string_view Alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

constexpr usize HeaderSize = Mark.size() + 1;
// The longest answers, which what asks for them is padded to.
constexpr usize PongSize = HeaderSize + 8 + 1 + 4 * 4;
constexpr usize LookupReplySize = HeaderSize + 8 + 2 + 6 + 1 + (1 + 2 + 4 * MostLocal);

// How often a host punches and a joiner knocks, and for how long.
constexpr u64 PunchEveryMs = 100;
constexpr u64 HostPunchForMs = 3'000;
constexpr u64 JoinPunchForMs = 2'500;
// A path across the internet answered: how long a local one is still given
// to answer before the first is taken.
constexpr u64 LocalGraceMs = 150;
constexpr u64 LookupEveryMs = 500;
constexpr u64 LookupForMs = 3'000;
constexpr u64 NeedRelayEveryMs = 300;
constexpr u64 NeedRelayForMs = 4'000;
// A registration not answered is said again this often, then this often once
// the relay has been silent for `UnreachableAfterMs`.
constexpr u64 RegisterRetryMs = 1'000;
constexpr u64 RegisterSlowRetryMs = 5'000;
constexpr u64 UnreachableAfterMs = 5'000;
constexpr u64 PipeOpenEveryMs = 250;
constexpr u64 PipeKeepAliveMs = 3'000;
constexpr usize MostIntroduced = 64;
constexpr usize MostCandidates = 8;

[[nodiscard]] int alphabetIndex(char character) noexcept
{
    const usize at = Alphabet.find(character);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

class Writer
{
public:
    explicit Writer(Type type)
    {
        m_bytes.reserve(64);
        m_bytes.insert(m_bytes.end(), Mark.begin(), Mark.end());
        m_bytes.push_back(static_cast<u8>(type));
    }
    Writer& byte(u8 value)
    {
        m_bytes.push_back(value);
        return *this;
    }
    Writer& flag(bool value) { return byte(value ? 1 : 0); }
    Writer& word(u16 value) { return byte(static_cast<u8>(value >> 8)).byte(static_cast<u8>(value)); }
    Writer& quad(u32 value) { return word(static_cast<u16>(value >> 16)).word(static_cast<u16>(value)); }
    Writer& wide(u64 value) { return quad(static_cast<u32>(value >> 32)).quad(static_cast<u32>(value)); }
    Writer& token(const Token& value)
    {
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return *this;
    }
    Writer& code(const Code& value)
    {
        for (const char character : value.text)
            m_bytes.push_back(static_cast<u8>(character));
        return *this;
    }
    Writer& endpoint(Endpoint value) { return quad(value.address).word(value.port); }
    Writer& locals(const Locals& value)
    {
        byte(static_cast<u8>(std::min<usize>(value.count, MostLocal)));
        word(value.port);
        for (usize index = 0; index < MostLocal; ++index)
            quad(index < value.count ? value.address[index] : 0);
        return *this;
    }
    // No shorter than `least`: what an answer of that length may be sent for.
    [[nodiscard]] std::vector<u8> take(usize least = 0)
    {
        if (m_bytes.size() < least)
            m_bytes.resize(least, 0);
        return std::move(m_bytes);
    }

private:
    std::vector<u8> m_bytes;
};

class Reader
{
public:
    Reader(std::span<const u8> datagram, Type type) : m_bytes(datagram)
    {
        m_ok = marked(datagram) && datagram.size() >= HeaderSize && datagram[Mark.size()] == static_cast<u8>(type);
        m_at = HeaderSize;
    }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    Reader& byte(u8& out)
    {
        if (m_ok && m_at < m_bytes.size())
            out = m_bytes[m_at++];
        else
            m_ok = false;
        return *this;
    }
    Reader& flag(bool& out)
    {
        u8 value = 0;
        byte(value);
        if (value > 1)
            m_ok = false;
        out = value == 1;
        return *this;
    }
    Reader& word(u16& out)
    {
        u8 high = 0;
        u8 low = 0;
        byte(high).byte(low);
        out = static_cast<u16>((static_cast<u16>(high) << 8) | low);
        return *this;
    }
    Reader& quad(u32& out)
    {
        u16 high = 0;
        u16 low = 0;
        word(high).word(low);
        out = (static_cast<u32>(high) << 16) | low;
        return *this;
    }
    Reader& wide(u64& out)
    {
        u32 high = 0;
        u32 low = 0;
        quad(high).quad(low);
        out = (static_cast<u64>(high) << 32) | low;
        return *this;
    }
    Reader& token(Token& out)
    {
        for (u8& value : out)
            byte(value);
        return *this;
    }
    Reader& code(Code& out)
    {
        for (char& character : out.text) {
            u8 value = 0;
            byte(value);
            character = static_cast<char>(value);
            if (alphabetIndex(character) < 0)
                m_ok = false;
        }
        return *this;
    }
    Reader& endpoint(Endpoint& out) { return quad(out.address).word(out.port); }
    Reader& locals(Locals& out)
    {
        byte(out.count).word(out.port);
        if (out.count > MostLocal)
            m_ok = false;
        for (u32& address : out.address)
            quad(address);
        return *this;
    }
    template <typename Enum>
    Reader& choice(Enum& out, u8 most)
    {
        u8 value = 0;
        byte(value);
        if (value > most)
            m_ok = false;
        out = static_cast<Enum>(value);
        return *this;
    }

private:
    std::span<const u8> m_bytes;
    usize m_at = 0;
    bool m_ok = false;
};

constexpr u8 MostRefusal = static_cast<u8>(Refusal::Version);

template <typename Message>
void say(const Send& send, Endpoint to, const Message& message)
{
    const std::vector<u8> bytes = encode(message);
    send(to, bytes);
}

[[nodiscard]] std::vector<Endpoint> endpointsOf(const Locals& locals)
{
    std::vector<Endpoint> out;
    for (usize index = 0; index < locals.count && index < MostLocal; ++index) {
        const Endpoint endpoint{locals.address[index], locals.port};
        if (endpoint.valid())
            out.push_back(endpoint);
    }
    return out;
}

} // namespace

// --- Addresses and codes -----------------------------------------------------------

std::string toText(Endpoint endpoint)
{
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        out += std::to_string((endpoint.address >> shift) & 0xFFu);
        out += shift != 0 ? '.' : ':';
    }
    out += std::to_string(endpoint.port);
    return out;
}

std::optional<u32> parseAddress(std::string_view dotted)
{
    u32 address = 0;
    for (int part = 0; part < 4; ++part) {
        const usize dot = part < 3 ? dotted.find('.') : dotted.size();
        if (dot == std::string_view::npos || dot == 0 || dot > 3)
            return std::nullopt;
        u32 value = 0;
        const auto [end, error] = std::from_chars(dotted.data(), dotted.data() + dot, value);
        if (error != std::errc{} || end != dotted.data() + dot || value > 255)
            return std::nullopt;
        address = (address << 8) | value;
        dotted.remove_prefix(part < 3 ? dot + 1 : dot);
    }
    return address;
}

std::optional<Endpoint> parseEndpoint(std::string_view text, u16 defaultPort)
{
    Endpoint endpoint;
    endpoint.port = defaultPort;
    if (const usize colon = text.rfind(':'); colon != std::string_view::npos) {
        const std::string_view port = text.substr(colon + 1);
        u32 value = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (port.empty() || error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
            return std::nullopt;
        endpoint.port = static_cast<u16>(value);
        text = text.substr(0, colon);
    }
    const std::optional<u32> address = parseAddress(text);
    if (!address || *address == 0 || endpoint.port == 0)
        return std::nullopt;
    endpoint.address = *address;
    return endpoint;
}

bool marked(std::span<const u8> datagram) noexcept
{
    return datagram.size() >= Mark.size() && std::equal(Mark.begin(), Mark.end(), datagram.begin());
}

Code codeOf(const Token& token)
{
    // SHA-256, and its first forty bits: the hash `core` already carries. A
    // code is not a secret -- it is what a player reads out to a friend -- so
    // forty bits are for telling sessions apart, and the token is what nobody
    // else can say.
    const core::Sha256 hash = core::sha256(token);
    u64 bits = 0;
    for (usize index = 0; index < 5; ++index)
        bits = (bits << 8) | hash[index];
    Code code;
    for (usize index = 0; index < code.text.size(); ++index)
        code.text[index] = Alphabet[(bits >> (35 - 5 * index)) & 31u];
    return code;
}

std::optional<Code> parseCode(std::string_view typed)
{
    Code code;
    usize count = 0;
    for (char character : typed) {
        if (character == ' ' || character == '-')
            continue;
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
        if (alphabetIndex(character) < 0 || count == code.text.size())
            return std::nullopt;
        code.text[count++] = character;
    }
    if (count != code.text.size())
        return std::nullopt;
    return code;
}

// --- The messages ------------------------------------------------------------------

std::vector<u8> encode(const Register& message)
{
    return Writer(Type::Register)
        .token(message.token)
        .code(message.code)
        .byte(message.version)
        .byte(message.players)
        .byte(message.maxPlayers)
        .locals(message.locals)
        .take();
}
std::vector<u8> encode(const Registered& message)
{
    return Writer(Type::Registered)
        .code(message.code)
        .endpoint(message.seen)
        .byte(message.keepAliveSeconds)
        .byte(static_cast<u8>(message.refusal))
        .take();
}
std::vector<u8> encode(const Unregister& message)
{
    return Writer(Type::Unregister).token(message.token).take();
}
std::vector<u8> encode(const Lookup& message)
{
    return Writer(Type::Lookup)
        .code(message.code)
        .wide(message.nonce)
        .byte(message.version)
        .locals(message.locals)
        .take(LookupReplySize);
}
std::vector<u8> encode(const LookupReply& message)
{
    return Writer(Type::LookupReply)
        .wide(message.nonce)
        .word(message.slot)
        .endpoint(message.host)
        .flag(message.sameNetwork)
        .locals(message.locals)
        .take();
}
std::vector<u8> encode(const LookupFailed& message)
{
    return Writer(Type::LookupFailed).wide(message.nonce).byte(static_cast<u8>(message.refusal)).take();
}
std::vector<u8> encode(const Introduce& message)
{
    return Writer(Type::Introduce)
        .wide(message.nonce)
        .word(message.slot)
        .endpoint(message.joiner)
        .flag(message.sameNetwork)
        .locals(message.locals)
        .take();
}
std::vector<u8> encode(const Punch& message)
{
    return Writer(Type::Punch).wide(message.nonce).flag(message.fromHost).take();
}
std::vector<u8> encode(const PunchAck& message)
{
    return Writer(Type::PunchAck).wide(message.nonce).flag(message.fromHost).take();
}
std::vector<u8> encode(const NeedRelay& message)
{
    return Writer(Type::NeedRelay).code(message.code).wide(message.nonce).word(message.slot).take();
}
std::vector<u8> encode(const OpenPipe& message)
{
    return Writer(Type::OpenPipe).wide(message.nonce).word(message.slot).endpoint(message.joiner).take();
}
std::vector<u8> encode(const PipeOpen& message)
{
    return Writer(Type::PipeOpen)
        .token(message.token)
        .wide(message.nonce)
        .word(message.slot)
        .endpoint(message.joiner)
        .take();
}
std::vector<u8> encode(const RelayReady& message)
{
    return Writer(Type::RelayReady).wide(message.nonce).word(message.slot).take();
}
std::vector<u8> encode(const Close& message)
{
    return Writer(Type::Close).token(message.token).word(message.slot).word(message.banSeconds).take();
}
std::vector<u8> encode(const Ping& message)
{
    return Writer(Type::Ping).wide(message.nonce).take(PongSize);
}
std::vector<u8> encode(const Pong& message)
{
    return Writer(Type::Pong)
        .wide(message.nonce)
        .byte(message.version)
        .quad(message.sessions)
        .quad(message.relayed)
        .quad(message.bytesPerSecond)
        .quad(message.uptimeSeconds)
        .take();
}

std::optional<Type> typeOf(std::span<const u8> datagram) noexcept
{
    if (!marked(datagram) || datagram.size() < HeaderSize)
        return std::nullopt;
    const u8 type = datagram[Mark.size()];
    if (type < static_cast<u8>(Type::Register) || type > static_cast<u8>(Type::Pong))
        return std::nullopt;
    return static_cast<Type>(type);
}

bool decode(std::span<const u8> datagram, Register& out)
{
    return Reader(datagram, Type::Register)
        .token(out.token)
        .code(out.code)
        .byte(out.version)
        .byte(out.players)
        .byte(out.maxPlayers)
        .locals(out.locals)
        .ok();
}
bool decode(std::span<const u8> datagram, Registered& out)
{
    return Reader(datagram, Type::Registered)
        .code(out.code)
        .endpoint(out.seen)
        .byte(out.keepAliveSeconds)
        .choice(out.refusal, MostRefusal)
        .ok();
}
bool decode(std::span<const u8> datagram, Unregister& out)
{
    return Reader(datagram, Type::Unregister).token(out.token).ok();
}
bool decode(std::span<const u8> datagram, Lookup& out)
{
    // No shorter than its answer: a lookup cut down to draw a longer reply
    // out of the relay is not one.
    return datagram.size() >= LookupReplySize &&
           Reader(datagram, Type::Lookup).code(out.code).wide(out.nonce).byte(out.version).locals(out.locals).ok();
}
bool decode(std::span<const u8> datagram, LookupReply& out)
{
    return Reader(datagram, Type::LookupReply)
        .wide(out.nonce)
        .word(out.slot)
        .endpoint(out.host)
        .flag(out.sameNetwork)
        .locals(out.locals)
        .ok();
}
bool decode(std::span<const u8> datagram, LookupFailed& out)
{
    return Reader(datagram, Type::LookupFailed).wide(out.nonce).choice(out.refusal, MostRefusal).ok();
}
bool decode(std::span<const u8> datagram, Introduce& out)
{
    return Reader(datagram, Type::Introduce)
        .wide(out.nonce)
        .word(out.slot)
        .endpoint(out.joiner)
        .flag(out.sameNetwork)
        .locals(out.locals)
        .ok();
}
bool decode(std::span<const u8> datagram, Punch& out)
{
    return Reader(datagram, Type::Punch).wide(out.nonce).flag(out.fromHost).ok();
}
bool decode(std::span<const u8> datagram, PunchAck& out)
{
    return Reader(datagram, Type::PunchAck).wide(out.nonce).flag(out.fromHost).ok();
}
bool decode(std::span<const u8> datagram, NeedRelay& out)
{
    return Reader(datagram, Type::NeedRelay).code(out.code).wide(out.nonce).word(out.slot).ok();
}
bool decode(std::span<const u8> datagram, OpenPipe& out)
{
    return Reader(datagram, Type::OpenPipe).wide(out.nonce).word(out.slot).endpoint(out.joiner).ok();
}
bool decode(std::span<const u8> datagram, PipeOpen& out)
{
    return Reader(datagram, Type::PipeOpen).token(out.token).wide(out.nonce).word(out.slot).endpoint(out.joiner).ok();
}
bool decode(std::span<const u8> datagram, RelayReady& out)
{
    return Reader(datagram, Type::RelayReady).wide(out.nonce).word(out.slot).ok();
}
bool decode(std::span<const u8> datagram, Close& out)
{
    return Reader(datagram, Type::Close).token(out.token).word(out.slot).word(out.banSeconds).ok();
}
bool decode(std::span<const u8> datagram, Ping& out)
{
    return datagram.size() >= PongSize && Reader(datagram, Type::Ping).wide(out.nonce).ok();
}
bool decode(std::span<const u8> datagram, Pong& out)
{
    return Reader(datagram, Type::Pong)
        .wide(out.nonce)
        .byte(out.version)
        .quad(out.sessions)
        .quad(out.relayed)
        .quad(out.bytesPerSecond)
        .quad(out.uptimeSeconds)
        .ok();
}

// --- The relay ---------------------------------------------------------------------

RelayServer::RelayServer(const RelayLimits& limits) : m_limits(limits)
{}

u64 RelayServer::keyOf(const Code& code) noexcept
{
    u64 key = 0;
    for (const char character : code.text)
        key = (key << 5) | static_cast<u64>(std::max(0, alphabetIndex(character)));
    return key;
}

bool RelayServer::spend(Bucket& bucket, double amount, double perSecond, double most, u64 nowMs)
{
    if (bucket.tokens < 0.0)
        bucket.tokens = most;
    else if (nowMs > bucket.atMs)
        bucket.tokens = std::min(most, bucket.tokens + static_cast<double>(nowMs - bucket.atMs) * perSecond / 1000.0);
    bucket.atMs = nowMs;
    if (bucket.tokens < amount)
        return false;
    bucket.tokens -= amount;
    return true;
}

bool RelayServer::allowed(std::unordered_map<u32, Bucket>& buckets, u32 address, u32 perSecond, u64 nowMs)
{
    return spend(buckets[address], 1.0, static_cast<double>(perSecond), static_cast<double>(perSecond) * 2.0, nowMs);
}

RelayServer::Session* RelayServer::find(const Code& code)
{
    const auto found = m_sessions.find(keyOf(code));
    return found == m_sessions.end() ? nullptr : &found->second;
}

RelayServer::Session* RelayServer::owned(const Token& token)
{
    Session* session = find(codeOf(token));
    return session != nullptr && core::secureEquals(session->token, token) ? session : nullptr;
}

RelayServer::Slot* RelayServer::slotOf(Session& session, u16 id)
{
    for (Slot& slot : session.slots)
        if (slot.id == id)
            return &slot;
    return nullptr;
}

void RelayServer::unroute(Endpoint endpoint)
{
    if (endpoint.valid())
        m_routes.erase(endpoint.key());
}

void RelayServer::removeSlot(Session& session, u16 id)
{
    const auto found =
        std::find_if(session.slots.begin(), session.slots.end(), [id](const Slot& slot) { return slot.id == id; });
    if (found == session.slots.end())
        return;
    // A route is a slot's only while it still names it: an address that has
    // since looked another code up is that session's.
    const u64 key = keyOf(session.code);
    for (const Endpoint endpoint : {found->joiner, found->pipe}) {
        const auto route = endpoint.valid() ? m_routes.find(endpoint.key()) : m_routes.end();
        if (route != m_routes.end() && route->second.session == key && route->second.slot == id)
            m_routes.erase(route);
    }
    session.slots.erase(found);
}

void RelayServer::removeSession(u64 key)
{
    const auto found = m_sessions.find(key);
    if (found == m_sessions.end())
        return;
    while (!found->second.slots.empty())
        removeSlot(found->second, found->second.slots.back().id);
    m_sessions.erase(found);
}

RelayServer::Slot* RelayServer::newSlot(Session& session, Endpoint joiner, u16 wanted, u64 nowMs)
{
    if (session.slots.size() >= m_limits.maxSlotsPerSession) {
        // Full of joiners: the one that has waited longest without being
        // carried gives its place up -- a lookup is cheap to make, and a
        // stranger making many must not close the door on a player.
        auto oldest = session.slots.end();
        for (auto slot = session.slots.begin(); slot != session.slots.end(); ++slot)
            if (!slot->pipe.valid() && (oldest == session.slots.end() || slot->seenMs < oldest->seenMs))
                oldest = slot;
        if (oldest == session.slots.end())
            return nullptr;
        removeSlot(session, oldest->id);
    }
    u16 id = wanted;
    while (id == 0 || slotOf(session, id) != nullptr) {
        id = session.nextSlot++;
        if (session.nextSlot == 0)
            session.nextSlot = 1;
    }
    Slot slot;
    slot.id = id;
    slot.joiner = joiner;
    slot.seenMs = nowMs;
    session.slots.push_back(slot);
    return &session.slots.back();
}

void RelayServer::count(usize bytes, u64 nowMs)
{
    if (nowMs - m_windowMs >= 1000) {
        const bool adjacent = nowMs - m_windowMs < 2000;
        m_lastBytes = adjacent ? m_windowBytes : 0;
        m_lastPackets = adjacent ? m_windowPackets : 0;
        m_windowBytes = 0;
        m_windowPackets = 0;
        m_windowMs = nowMs;
    }
    m_windowBytes += static_cast<u32>(bytes);
    ++m_windowPackets;
    m_bytes += bytes;
    ++m_packets;
}

void RelayServer::receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send)
{
    if (!m_started) {
        m_started = true;
        m_startMs = nowMs;
        m_windowMs = nowMs;
    }
    if (!from.valid())
        return;
    if (!marked(datagram)) {
        forward(from, datagram, nowMs, send);
        return;
    }
    const std::optional<Type> type = typeOf(datagram);
    if (!type) {
        ++m_dropped;
        return;
    }
    switch (*type) {
    case Type::Register:
        if (Register message; decode(datagram, message))
            return on(from, message, nowMs, send);
        break;
    case Type::Unregister:
        if (Unregister message; decode(datagram, message))
            return on(from, message);
        break;
    case Type::Lookup:
        if (Lookup message; decode(datagram, message))
            return on(from, message, nowMs, send);
        break;
    case Type::NeedRelay:
        if (NeedRelay message; decode(datagram, message))
            return on(from, message, nowMs, send);
        break;
    case Type::PipeOpen:
        if (PipeOpen message; decode(datagram, message))
            return on(from, message, nowMs, send);
        break;
    case Type::Close:
        if (Close message; decode(datagram, message))
            return on(message, nowMs);
        break;
    case Type::Ping:
        if (Ping message; decode(datagram, message))
            return on(from, message, nowMs, send);
        break;
    default:
        // A message a relay sends, said to one: nobody's.
        break;
    }
    ++m_dropped;
}

void RelayServer::on(Endpoint from, const Register& message, u64 nowMs, const Send& send)
{
    if (!allowed(m_registers, from.address, m_limits.registersPerSecond, nowMs)) {
        ++m_dropped;
        return;
    }
    Registered reply;
    reply.code = message.code;
    reply.seen = from;
    reply.keepAliveSeconds = static_cast<u8>(std::clamp<u32>(m_limits.sessionTimeoutMs / 3000, 1, 255));
    if (message.version != Version) {
        reply.refusal = Refusal::Version;
        return say(send, from, reply);
    }
    // The code is the token's or the registration is nobody's: this is what
    // keeps one host from taking another's code, with no memory of either.
    if (codeOf(message.token) != message.code) {
        ++m_dropped;
        return;
    }
    const u64 key = keyOf(message.code);
    auto found = m_sessions.find(key);
    if (found == m_sessions.end()) {
        if (m_sessions.size() >= m_limits.maxSessions) {
            reply.refusal = Refusal::Busy;
            return say(send, from, reply);
        }
        Session session;
        session.token = message.token;
        session.code = message.code;
        found = m_sessions.emplace(key, std::move(session)).first;
    }
    else if (!core::secureEquals(found->second.token, message.token)) {
        // Two tokens with one code, forty bits in: the one that came first
        // keeps it for as long as it lives.
        reply.refusal = Refusal::Busy;
        return say(send, from, reply);
    }
    Session& session = found->second;
    session.host = from;
    session.locals = message.locals;
    session.players = message.players;
    session.maxPlayers = message.maxPlayers;
    session.seenMs = nowMs;
    say(send, from, reply);
}

void RelayServer::on(Endpoint from, const Unregister& message)
{
    Session* session = owned(message.token);
    if (session == nullptr || session->host != from)
        return;
    removeSession(keyOf(session->code));
}

void RelayServer::on(Endpoint from, const Lookup& message, u64 nowMs, const Send& send)
{
    if (!allowed(m_lookups, from.address, m_limits.lookupsPerSecond, nowMs)) {
        ++m_dropped;
        return;
    }
    const auto refuse = [&](Refusal refusal) { say(send, from, LookupFailed{message.nonce, refusal}); };
    if (message.version != Version)
        return refuse(Refusal::Version);
    Session* session = find(message.code);
    if (session == nullptr)
        return refuse(Refusal::NoSession);
    std::erase_if(session->bans, [nowMs](const Ban& ban) { return ban.untilMs <= nowMs; });
    for (const Ban& ban : session->bans)
        if (ban.address == from.address)
            return refuse(Refusal::Refused);
    if (session->maxPlayers != 0 && session->players >= session->maxPlayers)
        return refuse(Refusal::Full);

    Slot* slot = nullptr;
    for (Slot& candidate : session->slots)
        if (candidate.joiner == from)
            slot = &candidate;
    if (slot == nullptr) {
        const auto held = std::count_if(session->slots.begin(), session->slots.end(),
                                        [&](const Slot& other) { return other.joiner.address == from.address; });
        if (static_cast<u32>(held) >= m_limits.slotsPerAddress)
            return refuse(Refusal::Busy);
        slot = newSlot(*session, from, 0, nowMs);
    }
    if (slot == nullptr)
        return refuse(Refusal::Busy);
    slot->nonce = message.nonce;
    slot->seenMs = nowMs;

    // Behind one public address they are, as far as anyone outside can tell,
    // on one network: each is given the other's own addresses to try.
    const bool sameNetwork = from.address == session->host.address;
    LookupReply reply;
    reply.nonce = message.nonce;
    reply.slot = slot->id;
    reply.host = session->host;
    reply.sameNetwork = sameNetwork;
    if (sameNetwork)
        reply.locals = session->locals;
    say(send, from, reply);

    const double introduces = static_cast<double>(m_limits.introducesPerSecond);
    if (spend(session->introduces, 1.0, introduces, introduces * 2.0, nowMs)) {
        Introduce introduce;
        introduce.nonce = message.nonce;
        introduce.slot = slot->id;
        introduce.joiner = from;
        introduce.sameNetwork = sameNetwork;
        if (sameNetwork)
            introduce.locals = message.locals;
        say(send, session->host, introduce);
    }
}

void RelayServer::on(Endpoint from, const NeedRelay& message, u64 nowMs, const Send& send)
{
    Session* session = find(message.code);
    Slot* slot = session != nullptr ? slotOf(*session, message.slot) : nullptr;
    if (slot == nullptr || slot->nonce != message.nonce || slot->joiner != from) {
        ++m_dropped;
        return;
    }
    slot->seenMs = nowMs;
    if (slot->pipe.valid())
        return say(send, from, RelayReady{slot->nonce, slot->id});
    say(send, session->host, OpenPipe{slot->nonce, slot->id, slot->joiner});
}

void RelayServer::on(Endpoint from, const PipeOpen& message, u64 nowMs, const Send& send)
{
    Session* session = owned(message.token);
    if (session == nullptr) {
        // A host that thinks it is registered, told otherwise: this relay was
        // restarted, and the host's next registration brings the code back.
        return say(send, from, LookupFailed{message.nonce, Refusal::NoSession});
    }
    Slot* slot = slotOf(*session, message.slot);
    if (slot == nullptr && message.joiner.valid()) {
        // The host holds a slot this relay does not: it was carrying the
        // joiner before a restart. The token is what makes its word enough.
        slot = newSlot(*session, message.joiner, message.slot, nowMs);
        if (slot != nullptr)
            slot->nonce = message.nonce;
    }
    if (slot == nullptr || slot->nonce != message.nonce || slot->id != message.slot) {
        ++m_dropped;
        return;
    }
    const u64 key = keyOf(session->code);
    if (slot->pipe != from) {
        unroute(slot->pipe);
        slot->pipe = from;
    }
    // An address is one joiner's at a time: a route it had to another slot is
    // that slot's no longer.
    if (const auto old = m_routes.find(slot->joiner.key());
        old != m_routes.end() && (old->second.session != key || old->second.slot != slot->id)) {
        const Route stale = old->second;
        if (const auto other = m_sessions.find(stale.session); other != m_sessions.end())
            removeSlot(other->second, stale.slot);
        slot = slotOf(*session, message.slot);
        if (slot == nullptr)
            return;
    }
    m_routes[slot->joiner.key()] = Route{key, slot->id, false};
    m_routes[slot->pipe.key()] = Route{key, slot->id, true};
    slot->seenMs = nowMs;
    const RelayReady ready{slot->nonce, slot->id};
    say(send, from, ready);
    say(send, slot->joiner, ready);
}

void RelayServer::on(const Close& message, u64 nowMs)
{
    Session* session = owned(message.token);
    if (session == nullptr)
        return;
    if (const Slot* slot = slotOf(*session, message.slot); slot != nullptr && message.banSeconds != 0) {
        constexpr usize MostBans = 256;
        if (session->bans.size() >= MostBans)
            session->bans.erase(session->bans.begin());
        session->bans.push_back(Ban{slot->joiner.address, nowMs + static_cast<u64>(message.banSeconds) * 1000});
    }
    removeSlot(*session, message.slot);
}

void RelayServer::on(Endpoint from, const Ping& message, u64 nowMs, const Send& send)
{
    const RelayStats now = stats(nowMs);
    Pong pong;
    pong.nonce = message.nonce;
    pong.sessions = now.sessions;
    pong.relayed = now.relayed;
    pong.bytesPerSecond = now.bytesPerSecond;
    pong.uptimeSeconds = now.uptimeSeconds;
    say(send, from, pong);
}

void RelayServer::forward(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send)
{
    const auto route = m_routes.find(from.key());
    if (route == m_routes.end()) {
        ++m_dropped;
        return;
    }
    const auto session = m_sessions.find(route->second.session);
    Slot* slot = session != m_sessions.end() ? slotOf(session->second, route->second.slot) : nullptr;
    if (slot == nullptr || !slot->pipe.valid()) {
        m_routes.erase(route);
        ++m_dropped;
        return;
    }
    const bool fromHost = route->second.fromHost;
    const double rate = static_cast<double>(m_limits.slotBytesPerSecond);
    // What it may carry at once is several seconds of the rate (D566): a
    // join's first second is not a match's every second.
    const double most = rate * static_cast<double>(std::max<u32>(m_limits.slotBurstSeconds, 1));
    if (!spend(fromHost ? slot->down : slot->up, static_cast<double>(datagram.size()), rate, most, nowMs)) {
        ++m_dropped;
        return;
    }
    slot->seenMs = nowMs;
    count(datagram.size(), nowMs);
    send(fromHost ? slot->joiner : slot->pipe, datagram);
}

void RelayServer::tick(u64 nowMs)
{
    std::vector<u64> gone;
    for (auto& [key, session] : m_sessions) {
        if (nowMs - session.seenMs > m_limits.sessionTimeoutMs) {
            gone.push_back(key);
            continue;
        }
        std::vector<u16> silent;
        for (const Slot& slot : session.slots)
            if (nowMs - slot.seenMs > m_limits.slotTimeoutMs)
                silent.push_back(slot.id);
        for (const u16 id : silent)
            removeSlot(session, id);
        std::erase_if(session.bans, [nowMs](const Ban& ban) { return ban.untilMs <= nowMs; });
    }
    for (const u64 key : gone)
        removeSession(key);
    // An address not heard from for ten seconds has its allowance whole
    // again, which is what having no entry means.
    constexpr u64 ForgetAfterMs = 10'000;
    const auto stale = [nowMs](const auto& entry) { return nowMs - entry.second.atMs > ForgetAfterMs; };
    std::erase_if(m_lookups, stale);
    std::erase_if(m_registers, stale);
}

RelayStats RelayServer::stats(u64 nowMs) const
{
    RelayStats out;
    out.sessions = static_cast<u32>(m_sessions.size());
    for (const auto& [key, session] : m_sessions)
        for (const Slot& slot : session.slots)
            out.relayed += slot.pipe.valid() ? 1u : 0u;
    // The last whole second: the one being counted once it is over, the one
    // before it while it is not, and nothing once both are old.
    const u64 age = nowMs - m_windowMs;
    if (age < 1000) {
        out.bytesPerSecond = m_lastBytes;
        out.packetsPerSecond = m_lastPackets;
    }
    else if (age < 2000) {
        out.bytesPerSecond = m_windowBytes;
        out.packetsPerSecond = m_windowPackets;
    }
    out.bytes = m_bytes;
    out.packets = m_packets;
    out.dropped = m_dropped;
    out.uptimeSeconds = m_started ? static_cast<u32>((nowMs - m_startMs) / 1000) : 0;
    return out;
}

// --- A host ------------------------------------------------------------------------

void HostRendezvous::start(Endpoint relay, const Token& token, const Locals& locals, u64 nowMs)
{
    *this = HostRendezvous{};
    m_relay = relay;
    m_token = token;
    m_code = codeOf(token);
    m_locals = locals;
    m_state = relay.valid() ? RelayState::Connecting : RelayState::None;
    m_nextRegisterMs = nowMs;
    m_awaiting = true;
    m_askedMs = nowMs;
}

void HostRendezvous::stop(const Send& send)
{
    if (active())
        say(send, m_relay, Unregister{m_token});
    *this = HostRendezvous{};
}

void HostRendezvous::setPlayers(u8 players, u8 maxPlayers) noexcept
{
    if (players == m_players && maxPlayers == m_maxPlayers)
        return;
    m_players = players;
    m_maxPlayers = maxPlayers;
    // Said now and not at the next keep-alive: a full host goes on being
    // looked up for ten seconds otherwise.
    m_nextRegisterMs = 0;
}

void HostRendezvous::tick(u64 nowMs, const Send& send)
{
    if (!active())
        return;
    if (nowMs >= m_nextRegisterMs) {
        if (!m_awaiting) {
            m_awaiting = true;
            m_askedMs = nowMs;
        }
        Register message;
        message.token = m_token;
        message.code = m_code;
        message.players = m_players;
        message.maxPlayers = m_maxPlayers;
        message.locals = m_locals;
        say(send, m_relay, message);
        m_nextRegisterMs = nowMs + (nowMs - m_askedMs < UnreachableAfterMs ? RegisterRetryMs : RegisterSlowRetryMs);
    }
    if (m_awaiting && nowMs - m_askedMs >= UnreachableAfterMs)
        m_state = RelayState::Unreachable;

    std::erase_if(m_introduced, [nowMs](const Introduced& introduced) { return nowMs >= introduced.untilMs; });
    for (Introduced& introduced : m_introduced) {
        if (nowMs < introduced.nextMs)
            continue;
        introduced.nextMs = nowMs + PunchEveryMs;
        for (const Endpoint target : introduced.targets)
            say(send, target, Punch{introduced.nonce, true});
    }
}

bool HostRendezvous::receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send)
{
    if (!marked(datagram))
        return false;
    const std::optional<Type> type = typeOf(datagram);
    if (!type || !active())
        return true;
    const bool fromRelay = from == m_relay;
    switch (*type) {
    case Type::Registered:
        if (Registered message; fromRelay && decode(datagram, message) && message.code == m_code) {
            if (message.refusal != Refusal::None) {
                // At a limit, or another version: asked again, slowly.
                m_state = RelayState::Unreachable;
                break;
            }
            m_seen = message.seen;
            m_keepAliveMs = static_cast<u32>(std::max<u8>(1, message.keepAliveSeconds)) * 1000u;
            m_awaiting = false;
            m_state = RelayState::Ready;
            // Unless the count of players changed since it was asked: that
            // is owed at the next tick, not a keep-alive from now.
            if (m_nextRegisterMs != 0)
                m_nextRegisterMs = nowMs + m_keepAliveMs;
        }
        break;
    case Type::Introduce:
        if (Introduce message; fromRelay && decode(datagram, message) && message.joiner.valid()) {
            auto found = std::find_if(m_introduced.begin(), m_introduced.end(),
                                      [&](const Introduced& known) { return known.nonce == message.nonce; });
            if (found == m_introduced.end()) {
                if (m_introduced.size() >= MostIntroduced)
                    m_introduced.erase(m_introduced.begin());
                m_introduced.emplace_back();
                found = m_introduced.end() - 1;
                found->nonce = message.nonce;
                found->nextMs = nowMs;
            }
            found->untilMs = nowMs + HostPunchForMs;
            found->targets.assign(1, message.joiner);
            if (message.sameNetwork)
                for (const Endpoint local : endpointsOf(message.locals))
                    found->targets.push_back(local);
        }
        break;
    case Type::Punch:
        // A knock is answered only when the relay said to expect it: a host
        // answers nobody it was not introduced to. And to where it came from,
        // which behind some NATs is not where the relay saw the joiner.
        if (Punch message; decode(datagram, message) && !message.fromHost) {
            const bool known = std::any_of(m_introduced.begin(), m_introduced.end(), [&](const Introduced& introduced) {
                return introduced.nonce == message.nonce;
            });
            if (known)
                say(send, from, PunchAck{message.nonce, true});
        }
        break;
    case Type::OpenPipe:
        if (OpenPipe message; fromRelay && decode(datagram, message)) {
            const bool asked = std::any_of(m_pipes.begin(), m_pipes.end(),
                                           [&](const PipeRequest& pipe) { return pipe.slot == message.slot; });
            if (!asked && m_pipes.size() < MostIntroduced)
                m_pipes.push_back(PipeRequest{message.slot, message.nonce, message.joiner});
        }
        break;
    default:
        break;
    }
    return true;
}

std::vector<HostRendezvous::PipeRequest> HostRendezvous::takePipeRequests()
{
    return std::exchange(m_pipes, {});
}

void HostRendezvous::close(u16 slot, u16 banSeconds, const Send& send)
{
    if (!active())
        return;
    // Twice: it is not answered, and one that is lost leaves a refused player
    // free to look the code up again.
    const Close message{m_token, slot, banSeconds};
    say(send, m_relay, message);
    say(send, m_relay, message);
}

// --- A host's socket for a relayed joiner ----------------------------------------------

void HostPipe::start(Endpoint relay, const Token& token, const HostRendezvous::PipeRequest& request, u64 nowMs)
{
    *this = HostPipe{};
    m_relay = relay;
    m_token = token;
    m_request = request;
    m_nextMs = nowMs;
}

bool HostPipe::receive(Endpoint from, std::span<const u8> datagram, u64 nowMs)
{
    if (!marked(datagram))
        return false;
    if (from != m_relay)
        return true;
    if (RelayReady ready; decode(datagram, ready) && ready.slot == m_request.slot && ready.nonce == m_request.nonce) {
        m_ready = true;
        m_nextMs = nowMs + PipeKeepAliveMs;
    }
    else if (LookupFailed failed; decode(datagram, failed) && failed.nonce == m_request.nonce) {
        m_lost = true;
        m_ready = false;
        m_nextMs = nowMs + PipeOpenEveryMs;
    }
    return true;
}

void HostPipe::tick(u64 nowMs, const Send& send)
{
    if (!m_relay.valid() || nowMs < m_nextMs)
        return;
    m_nextMs = nowMs + (m_ready ? PipeKeepAliveMs : PipeOpenEveryMs);
    say(send, m_relay, PipeOpen{m_token, m_request.nonce, m_request.slot, m_request.joiner});
}

bool HostPipe::takeLost() noexcept
{
    return std::exchange(m_lost, false);
}

// --- A joiner ----------------------------------------------------------------------

void JoinRendezvous::start(Endpoint relay, const Code& code, const Locals& locals, u64 nonce, u64 nowMs,
                           const Options& options)
{
    *this = JoinRendezvous{};
    m_relay = relay;
    m_code = code;
    m_locals = locals;
    m_options = options;
    m_nonce = nonce;
    m_stage = Stage::Lookup;
    m_stageMs = nowMs;
    m_nextMs = nowMs;
}

void JoinRendezvous::fail(JoinFailure failure) noexcept
{
    m_stage = Stage::Failed;
    m_failure = failure;
}

void JoinRendezvous::find(Endpoint target, Path path) noexcept
{
    m_stage = Stage::Found;
    m_target = target;
    m_path = path;
}

bool JoinRendezvous::local(Endpoint endpoint) const noexcept
{
    return std::find(m_localCandidates.begin(), m_localCandidates.end(), endpoint) != m_localCandidates.end();
}

void JoinRendezvous::tick(u64 nowMs, const Send& send)
{
    switch (m_stage) {
    case Stage::Lookup:
        if (nowMs - m_stageMs >= LookupForMs)
            return fail(JoinFailure::RelayUnreachable);
        if (nowMs >= m_nextMs) {
            m_nextMs = nowMs + LookupEveryMs;
            Lookup message;
            message.code = m_code;
            message.nonce = m_nonce;
            message.locals = m_locals;
            say(send, m_relay, message);
        }
        break;
    case Stage::Punch:
        if (m_answered.valid() && nowMs - m_answeredMs >= LocalGraceMs)
            return find(m_answered, Path::Direct);
        if (nowMs - m_stageMs >= JoinPunchForMs) {
            if (m_answered.valid())
                return find(m_answered, Path::Direct);
            m_stage = Stage::Relay;
            m_stageMs = nowMs;
            m_nextMs = nowMs;
            return tick(nowMs, send);
        }
        if (nowMs >= m_nextMs) {
            m_nextMs = nowMs + PunchEveryMs;
            for (const Endpoint candidate : m_candidates)
                say(send, candidate, Punch{m_nonce, false});
            for (const Endpoint candidate : m_localCandidates)
                say(send, candidate, Punch{m_nonce, false});
        }
        break;
    case Stage::Relay:
        if (nowMs - m_stageMs >= NeedRelayForMs)
            return fail(JoinFailure::TimedOut);
        if (nowMs >= m_nextMs) {
            m_nextMs = nowMs + NeedRelayEveryMs;
            say(send, m_relay, NeedRelay{m_code, m_nonce, m_slot});
        }
        break;
    case Stage::Idle:
    case Stage::Found:
    case Stage::Failed:
        break;
    }
}

bool JoinRendezvous::receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send)
{
    if (!marked(datagram))
        return false;
    const std::optional<Type> type = typeOf(datagram);
    if (!type)
        return true;
    const bool fromRelay = from == m_relay;
    switch (*type) {
    case Type::LookupReply:
        if (LookupReply message;
            m_stage == Stage::Lookup && fromRelay && decode(datagram, message) && message.nonce == m_nonce) {
            m_slot = message.slot;
            m_host = message.host;
            m_candidates.clear();
            m_localCandidates.clear();
            if (message.host.valid())
                m_candidates.push_back(message.host);
            if (message.sameNetwork)
                for (const Endpoint candidate : endpointsOf(message.locals))
                    if (candidate != message.host)
                        m_localCandidates.push_back(candidate);
            const bool anywhere = !m_candidates.empty() || !m_localCandidates.empty();
            m_stage = m_options.direct && anywhere ? Stage::Punch : Stage::Relay;
            m_stageMs = nowMs;
            m_nextMs = nowMs;
            tick(nowMs, send);
        }
        break;
    case Type::LookupFailed:
        if (LookupFailed message;
            m_stage == Stage::Lookup && fromRelay && decode(datagram, message) && message.nonce == m_nonce) {
            switch (message.refusal) {
            case Refusal::Full:
                fail(JoinFailure::Full);
                break;
            case Refusal::Refused:
                fail(JoinFailure::Refused);
                break;
            case Refusal::Busy:
            case Refusal::Version:
                fail(JoinFailure::Busy);
                break;
            case Refusal::None:
            case Refusal::NoSession:
                fail(JoinFailure::NoSession);
                break;
            }
        }
        break;
    case Type::Punch:
        // The host's knock, from where it really comes: behind some NATs that
        // is not where the relay saw it, and it is where to knock back.
        if (Punch message; m_stage == Stage::Punch && decode(datagram, message) && message.fromHost &&
                           message.nonce == m_nonce && m_options.direct) {
            if (!local(from) && std::find(m_candidates.begin(), m_candidates.end(), from) == m_candidates.end() &&
                m_candidates.size() < MostCandidates) {
                m_candidates.push_back(from);
                say(send, from, Punch{m_nonce, false});
            }
        }
        break;
    case Type::PunchAck:
        // The host answered a knock: the path it came back along is open both
        // ways. Late is as good -- one that arrives while the relay is being
        // asked is still the better path.
        if (PunchAck message; (m_stage == Stage::Punch || m_stage == Stage::Relay) && m_options.direct &&
                              decode(datagram, message) && message.fromHost && message.nonce == m_nonce) {
            if (local(from))
                find(from, Path::Lan);
            else if (m_stage == Stage::Punch && !m_localCandidates.empty()) {
                // A local address may still answer, and is the shorter way.
                if (!m_answered.valid()) {
                    m_answered = from;
                    m_answeredMs = nowMs;
                }
            }
            else
                find(from, Path::Direct);
        }
        break;
    case Type::RelayReady:
        if (RelayReady message; m_stage == Stage::Relay && fromRelay && decode(datagram, message) &&
                                message.nonce == m_nonce && message.slot == m_slot)
            find(m_relay, Path::Relayed);
        break;
    default:
        break;
    }
    return true;
}

} // namespace engine::net::rendezvous
