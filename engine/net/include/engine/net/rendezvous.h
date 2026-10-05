// Reaching a host behind a NAT (ADR 0178, bringing ADR 0120 §4 forward): the
// messages a host, a joiner and a relay say to each other, and the three of
// them as machines that are told what arrived and when, and answer what to
// send.
//
// **No socket and no clock in here.** Everything is "this datagram came from
// there at this time" in, "send this there" out -- so the same code is the
// relay program's, the transport's, and a test's with a network it makes up:
// a NAT that maps by destination, one that filters by port, a relay that is
// restarted. What a NAT does to a punch is the whole problem, and it is not a
// thing a loopback socket will ever do.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/types.h"

namespace engine::net::rendezvous {

using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// An IPv4 address and a port. The address is `a.b.c.d` as `0xAABBCCDD`,
// whatever the machine's byte order: it is compared and printed, never handed
// to a socket as it is.
struct Endpoint
{
    u32 address = 0;
    u16 port = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return address != 0 && port != 0; }
    [[nodiscard]] constexpr u64 key() const noexcept { return (static_cast<u64>(address) << 16) | port; }
    friend constexpr bool operator==(Endpoint, Endpoint) noexcept = default;
};
// `a.b.c.d:port`.
[[nodiscard]] std::string toText(Endpoint endpoint);
// `a.b.c.d` or `a.b.c.d:port`; a name is not looked up here.
[[nodiscard]] std::optional<Endpoint> parseEndpoint(std::string_view text, u16 defaultPort);
[[nodiscard]] std::optional<u32> parseAddress(std::string_view dotted);

// What every rendezvous datagram begins with. ENet never begins one so: these
// bytes read as its header are a compressed packet, and the transport
// compresses nothing.
inline constexpr std::array<u8, 4> Mark{'L', 'W', 'R', '1'};
[[nodiscard]] bool marked(std::span<const u8> datagram) noexcept;

// The rendezvous's own version, apart from the match's protocol: a relay
// forwards a match's bytes without reading them.
inline constexpr u8 Version = 1;

// **A host's secret for a session**, drawn once when it starts hosting; and
// the code its joiners type, which is that secret's hash -- so the relay can
// check a registration's code is its own token's and needs no memory of who
// holds which: after a restart the next keep-alive registers the same code.
using Token = std::array<u8, 16>;
struct Code
{
    // Eight characters of an alphabet with no look-alikes: no 0, 1, I or O.
    std::array<char, 8> text{};

    [[nodiscard]] std::string str() const { return std::string(text.data(), text.size()); }
    friend bool operator==(const Code&, const Code&) noexcept = default;
};
[[nodiscard]] Code codeOf(const Token& token);
// As a person types it: either case, with spaces or dashes between.
[[nodiscard]] std::optional<Code> parseCode(std::string_view typed);

enum class Type : u8
{
    Register = 1,
    Registered = 2,
    Unregister = 3,
    Lookup = 4,
    LookupReply = 5,
    LookupFailed = 6,
    Introduce = 7,
    Punch = 8,
    PunchAck = 9,
    NeedRelay = 10,
    OpenPipe = 11,
    PipeOpen = 12,
    RelayReady = 13,
    Close = 14,
    Ping = 15,
    Pong = 16,
};

// Why a lookup was refused. The numbers are on the wire.
enum class Refusal : u8
{
    None = 0,
    // No host has registered that code, or its host stopped answering.
    NoSession = 1,
    // The host says it has no room.
    Full = 2,
    // The host let this address go and asked that it not come back yet.
    Refused = 3,
    // The relay is at one of its limits.
    Busy = 4,
    // The two speak different versions of this.
    Version = 5,
};

// A machine's own addresses on its networks, at most four: what two machines
// behind one public address reach each other by.
inline constexpr usize MostLocal = 4;
struct Locals
{
    std::array<u32, MostLocal> address{};
    u8 count = 0;
    // The port the machine's socket is bound to on them.
    u16 port = 0;
};

struct Register
{
    Token token{};
    Code code{};
    u8 version = Version;
    u8 players = 0;
    u8 maxPlayers = 0;
    Locals locals{};
};
struct Registered
{
    Code code{};
    // The host as the relay sees it.
    Endpoint seen{};
    // How often it wants to hear from the host, in seconds.
    u8 keepAliveSeconds = 10;
    Refusal refusal = Refusal::None;
};
struct Unregister
{
    Token token{};
};
struct Lookup
{
    Code code{};
    u64 nonce = 0;
    u8 version = Version;
    Locals locals{};
};
struct LookupReply
{
    u64 nonce = 0;
    u16 slot = 0;
    Endpoint host{};
    // Both are behind one public address: `locals` is the host's.
    bool sameNetwork = false;
    Locals locals{};
};
struct LookupFailed
{
    u64 nonce = 0;
    Refusal refusal = Refusal::NoSession;
};
struct Introduce
{
    u64 nonce = 0;
    u16 slot = 0;
    Endpoint joiner{};
    bool sameNetwork = false;
    Locals locals{};
};
struct Punch
{
    u64 nonce = 0;
    bool fromHost = false;
};
struct PunchAck
{
    u64 nonce = 0;
    bool fromHost = false;
};
struct NeedRelay
{
    Code code{};
    u64 nonce = 0;
    u16 slot = 0;
};
struct OpenPipe
{
    u64 nonce = 0;
    u16 slot = 0;
    // The joiner as the relay sees it: the host says it back with every
    // `PipeOpen`, so a relay that was restarted learns the slot again from the
    // host that holds it.
    Endpoint joiner{};
};
struct PipeOpen
{
    Token token{};
    u64 nonce = 0;
    u16 slot = 0;
    Endpoint joiner{};
};
struct RelayReady
{
    u64 nonce = 0;
    u16 slot = 0;
};
struct Close
{
    Token token{};
    u16 slot = 0;
    // How long the joiner's address may not look the code up again.
    u16 banSeconds = 0;
};
struct Ping
{
    u64 nonce = 0;
};
struct Pong
{
    u64 nonce = 0;
    u8 version = Version;
    u32 sessions = 0;
    u32 relayed = 0;
    u32 bytesPerSecond = 0;
    u32 uptimeSeconds = 0;
};

// Each message as a datagram. The ones a relay answers are padded so that no
// answer is longer than what asked for it: a relay amplifies nothing.
[[nodiscard]] std::vector<u8> encode(const Register& message);
[[nodiscard]] std::vector<u8> encode(const Registered& message);
[[nodiscard]] std::vector<u8> encode(const Unregister& message);
[[nodiscard]] std::vector<u8> encode(const Lookup& message);
[[nodiscard]] std::vector<u8> encode(const LookupReply& message);
[[nodiscard]] std::vector<u8> encode(const LookupFailed& message);
[[nodiscard]] std::vector<u8> encode(const Introduce& message);
[[nodiscard]] std::vector<u8> encode(const Punch& message);
[[nodiscard]] std::vector<u8> encode(const PunchAck& message);
[[nodiscard]] std::vector<u8> encode(const NeedRelay& message);
[[nodiscard]] std::vector<u8> encode(const OpenPipe& message);
[[nodiscard]] std::vector<u8> encode(const PipeOpen& message);
[[nodiscard]] std::vector<u8> encode(const RelayReady& message);
[[nodiscard]] std::vector<u8> encode(const Close& message);
[[nodiscard]] std::vector<u8> encode(const Ping& message);
[[nodiscard]] std::vector<u8> encode(const Pong& message);

// What a marked datagram is, or nothing for one that is not a message.
[[nodiscard]] std::optional<Type> typeOf(std::span<const u8> datagram) noexcept;
// False for a datagram of another type, too short, or with a field out of
// range; `out` is then unspecified.
[[nodiscard]] bool decode(std::span<const u8> datagram, Register& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Registered& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Unregister& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Lookup& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, LookupReply& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, LookupFailed& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Introduce& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Punch& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, PunchAck& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, NeedRelay& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, OpenPipe& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, PipeOpen& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, RelayReady& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Close& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Ping& out);
[[nodiscard]] bool decode(std::span<const u8> datagram, Pong& out);

// Where a machine sends: handed to each of the three below with every call
// that may answer.
using Send = std::function<void(Endpoint to, std::span<const u8> datagram)>;

// --- The relay -----------------------------------------------------------------

struct RelayLimits
{
    u32 maxSessions = 4096;
    u32 maxSlotsPerSession = 64;
    // And of those, how many one address may hold: what a host's own limit of
    // connections an address is for a joiner the host never sees the address
    // of.
    u32 slotsPerAddress = 8;
    // What one relayed joiner may carry each way, in bytes a second; what is
    // over it is dropped, as a full link drops it.
    u32 slotBytesPerSecond = 512u * 1024u;
    // A host not heard from for this long is gone, and its code with it.
    u32 sessionTimeoutMs = 30'000;
    // A relayed joiner nothing has crossed for this long is let go.
    u32 slotTimeoutMs = 30'000;
    // Lookups and registrations a second from one address, with twice that
    // allowed at once.
    u32 lookupsPerSecond = 5;
    u32 registersPerSecond = 4;
    // Introductions a second to one host: what a stranger who knows a code can
    // make the relay send it.
    u32 introducesPerSecond = 10;
};

struct RelayStats
{
    u32 sessions = 0;
    // Joiners the relay is forwarding for.
    u32 relayed = 0;
    // Forwarded in the last whole second, both ways together, and since start.
    u32 bytesPerSecond = 0;
    u32 packetsPerSecond = 0;
    u64 bytes = 0;
    u64 packets = 0;
    // Datagrams left where they fell: over a limit, from nobody known.
    u64 dropped = 0;
    u32 uptimeSeconds = 0;
};

// **The relay**: rendezvous, forwarding and the ping, as ADR 0178 has them.
// One of these is the whole program but for its socket.
class RelayServer
{
public:
    explicit RelayServer(const RelayLimits& limits = {});

    // One datagram that arrived, and when, in milliseconds on any clock that
    // only goes forward.
    void receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send);
    // Lets go of what has gone silent. A few times a second is plenty.
    void tick(u64 nowMs);
    [[nodiscard]] RelayStats stats(u64 nowMs) const;

private:
    struct Bucket
    {
        // Under zero: not drawn from yet, and full at its first use.
        double tokens = -1.0;
        u64 atMs = 0;
    };
    struct Slot
    {
        u16 id = 0;
        u64 nonce = 0;
        Endpoint joiner{};
        // The host's socket for this joiner, once it has said which.
        Endpoint pipe{};
        u64 seenMs = 0;
        Bucket up{};
        Bucket down{};
    };
    struct Ban
    {
        u32 address = 0;
        u64 untilMs = 0;
    };
    struct Session
    {
        Token token{};
        Code code{};
        Endpoint host{};
        Locals locals{};
        u8 players = 0;
        u8 maxPlayers = 0;
        u64 seenMs = 0;
        u16 nextSlot = 1;
        std::vector<Slot> slots;
        std::vector<Ban> bans;
        Bucket introduces{};
    };
    struct Route
    {
        u64 session = 0;
        u16 slot = 0;
        bool fromHost = false;
    };

    [[nodiscard]] static u64 keyOf(const Code& code) noexcept;
    [[nodiscard]] bool allowed(std::unordered_map<u32, Bucket>& buckets, u32 address, u32 perSecond, u64 nowMs);
    [[nodiscard]] static bool spend(Bucket& bucket, double amount, double perSecond, double most, u64 nowMs);
    [[nodiscard]] Session* find(const Code& code);
    // The session a token is the secret of, or none.
    [[nodiscard]] Session* owned(const Token& token);
    [[nodiscard]] static Slot* slotOf(Session& session, u16 id);
    [[nodiscard]] Slot* newSlot(Session& session, Endpoint joiner, u16 wanted, u64 nowMs);
    void removeSlot(Session& session, u16 id);
    void unroute(Endpoint endpoint);
    void removeSession(u64 key);
    void forward(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send);
    void count(usize bytes, u64 nowMs);

    void on(Endpoint from, const Register& message, u64 nowMs, const Send& send);
    void on(Endpoint from, const Unregister& message);
    void on(Endpoint from, const Lookup& message, u64 nowMs, const Send& send);
    void on(Endpoint from, const NeedRelay& message, u64 nowMs, const Send& send);
    void on(Endpoint from, const PipeOpen& message, u64 nowMs, const Send& send);
    void on(const Close& message, u64 nowMs);
    void on(Endpoint from, const Ping& message, u64 nowMs, const Send& send);

    RelayLimits m_limits;
    std::unordered_map<u64, Session> m_sessions;
    // Who a datagram that is not a message is from: a relayed joiner, or a
    // host's socket for one.
    std::unordered_map<u64, Route> m_routes;
    std::unordered_map<u32, Bucket> m_lookups;
    std::unordered_map<u32, Bucket> m_registers;
    u64 m_startMs = 0;
    bool m_started = false;
    // The second being counted, and the one before it.
    u64 m_windowMs = 0;
    u32 m_windowBytes = 0;
    u32 m_windowPackets = 0;
    u32 m_lastBytes = 0;
    u32 m_lastPackets = 0;
    u64 m_bytes = 0;
    u64 m_packets = 0;
    u64 m_dropped = 0;
};

// --- A host --------------------------------------------------------------------

enum class RelayState : u8
{
    // No relay asked for.
    None,
    // Asked, and not answered yet.
    Connecting,
    // Registered: the code resolves.
    Ready,
    // The relay has not answered for too long. Still asked.
    Unreachable,
};

// **A host's side**: registers and keeps registered, answers introductions
// with punches, and says which joiners the relay must carry.
class HostRendezvous
{
public:
    // `token` is drawn by the caller, once a session.
    void start(Endpoint relay, const Token& token, const Locals& locals, u64 nowMs);
    // Tells the relay the code is no longer anybody's.
    void stop(const Send& send);
    [[nodiscard]] bool active() const noexcept { return m_relay.valid(); }

    void setPlayers(u8 players, u8 maxPlayers) noexcept;

    // True when the datagram was this machine's to take; false for anything
    // else, which is the match's.
    [[nodiscard]] bool receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send);
    void tick(u64 nowMs, const Send& send);

    [[nodiscard]] RelayState state() const noexcept { return m_state; }
    [[nodiscard]] Code code() const noexcept { return m_code; }
    // The host as the relay sees it, once registered.
    [[nodiscard]] Endpoint seen() const noexcept { return m_seen; }
    [[nodiscard]] Endpoint relay() const noexcept { return m_relay; }
    [[nodiscard]] const Token& token() const noexcept { return m_token; }

    // The joiners the relay asked a pipe for since this was last asked.
    struct PipeRequest
    {
        u16 slot = 0;
        u64 nonce = 0;
        Endpoint joiner{};
    };
    [[nodiscard]] std::vector<PipeRequest> takePipeRequests();
    // Registers again at the next tick: a pipe was told the relay does not
    // know the session, which is a relay that was restarted.
    void refresh() noexcept { m_nextRegisterMs = 0; }
    // A relayed joiner is gone: the relay stops carrying it, and keeps its
    // address away for `banSeconds`.
    void close(u16 slot, u16 banSeconds, const Send& send);

private:
    struct Introduced
    {
        u64 nonce = 0;
        u64 untilMs = 0;
        u64 nextMs = 0;
        std::vector<Endpoint> targets;
    };

    Endpoint m_relay{};
    Token m_token{};
    Code m_code{};
    Locals m_locals{};
    Endpoint m_seen{};
    RelayState m_state = RelayState::None;
    u8 m_players = 0;
    u8 m_maxPlayers = 0;
    u64 m_nextRegisterMs = 0;
    // A registration is out and not answered, and since when.
    bool m_awaiting = false;
    u64 m_askedMs = 0;
    u32 m_keepAliveMs = 10'000;
    std::vector<Introduced> m_introduced;
    std::vector<PipeRequest> m_pipes;
};

// **A host's socket for one relayed joiner**, as far as the relay is
// concerned: it says which slot it is until the relay has heard, and again
// every few seconds for as long as the joiner is there -- which is also what
// tells a restarted relay the slot again.
class HostPipe
{
public:
    void start(Endpoint relay, const Token& token, const HostRendezvous::PipeRequest& request, u64 nowMs);
    // True when the datagram was a message of the relay's, and so not the
    // match's to carry inward.
    [[nodiscard]] bool receive(Endpoint from, std::span<const u8> datagram, u64 nowMs);
    void tick(u64 nowMs, const Send& send);

    // The relay has said it is carrying the joiner.
    [[nodiscard]] bool ready() const noexcept { return m_ready; }
    [[nodiscard]] u16 slot() const noexcept { return m_request.slot; }
    // True once after the relay answered that it knows no such session: the
    // host registers again (`HostRendezvous::refresh`).
    [[nodiscard]] bool takeLost() noexcept;

private:
    Endpoint m_relay{};
    Token m_token{};
    HostRendezvous::PipeRequest m_request{};
    bool m_ready = false;
    bool m_lost = false;
    u64 m_nextMs = 0;
};

// --- A joiner ------------------------------------------------------------------

// How a joiner reached its host.
enum class Path : u8
{
    None,
    // On the same network, by a local address.
    Lan,
    // Across the internet, each to the other.
    Direct,
    // Through the relay.
    Relayed,
};

enum class JoinFailure : u8
{
    None,
    // The relay did not answer.
    RelayUnreachable,
    // No host has that code.
    NoSession,
    // The host has no room.
    Full,
    // The host let this machine go and it may not come back yet.
    Refused,
    // The relay is at a limit, or speaks another version.
    Busy,
    // No path opened in time, the relay's included.
    TimedOut,
};

// **A joiner's side**: looks a code up, tries the host's own network, punches,
// and falls back to the relay -- and says where to connect, or why not.
class JoinRendezvous
{
public:
    struct Options
    {
        // Off: straight to the relay, for a test of that path and for a
        // network a player knows will not punch.
        bool direct = true;
    };
    void start(Endpoint relay, const Code& code, const Locals& locals, u64 nonce, u64 nowMs, const Options& options);
    void start(Endpoint relay, const Code& code, const Locals& locals, u64 nonce, u64 nowMs)
    {
        start(relay, code, locals, nonce, nowMs, Options{});
    }
    [[nodiscard]] bool active() const noexcept { return m_stage != Stage::Idle; }

    [[nodiscard]] bool receive(Endpoint from, std::span<const u8> datagram, u64 nowMs, const Send& send);
    void tick(u64 nowMs, const Send& send);

    // Decided: where the match's connection goes, and by what path.
    [[nodiscard]] bool found() const noexcept { return m_stage == Stage::Found; }
    [[nodiscard]] Endpoint target() const noexcept { return m_target; }
    [[nodiscard]] Path path() const noexcept { return m_path; }
    [[nodiscard]] bool failed() const noexcept { return m_stage == Stage::Failed; }
    [[nodiscard]] JoinFailure failure() const noexcept { return m_failure; }

private:
    enum class Stage : u8
    {
        Idle,
        Lookup,
        Punch,
        Relay,
        Found,
        Failed,
    };
    void fail(JoinFailure failure) noexcept;
    void find(Endpoint target, Path path) noexcept;
    [[nodiscard]] bool local(Endpoint endpoint) const noexcept;

    Endpoint m_relay{};
    Code m_code{};
    Locals m_locals{};
    Options m_options{};
    u64 m_nonce = 0;
    Stage m_stage = Stage::Idle;
    u64 m_stageMs = 0;
    u64 m_nextMs = 0;
    u16 m_slot = 0;
    Endpoint m_host{};
    // Where the host may be across the internet -- as the relay sees it, and
    // wherever its own knocks have come from -- and its own addresses on a
    // network the two share.
    std::vector<Endpoint> m_candidates;
    std::vector<Endpoint> m_localCandidates;
    // A path across the internet answered, and a local one may yet: how long
    // it is given.
    Endpoint m_answered{};
    u64 m_answeredMs = 0;
    Endpoint m_target{};
    Path m_path = Path::None;
    JoinFailure m_failure = JoinFailure::None;
};

} // namespace engine::net::rendezvous
