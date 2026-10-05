// Reaching a host behind a NAT (ADR 0178): the three machines of
// `rendezvous.h` on a network this file makes up. What a router does to a
// knock is the whole problem, and no loopback socket will ever do it -- so the
// routers here are written down: one that lets anything in to a port it has
// opened, one that lets in only who that port has written to, and one that
// opens a different port for every destination.

#include <deque>
#include <doctest/doctest.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engine/net/rendezvous.h"

namespace {

using namespace engine::net::rendezvous;
namespace core = engine::core;

constexpr u32 address(u32 a, u32 b, u32 c, u32 d)
{
    return (a << 24) | (b << 16) | (c << 8) | d;
}

enum class Nat
{
    // Lets anything in to a port a machine behind it has opened.
    FullCone,
    // Lets in, to a port, only an address and port it has written to.
    PortRestricted,
    // Opens another port for every destination, and lets in only that one.
    Symmetric,
};

class Network
{
public:
    using Handler = std::function<void(Endpoint from, std::span<const u8> datagram)>;

    int addRouter(u32 publicAddress, Nat kind)
    {
        m_routers.push_back(Router{publicAddress, kind, {}, {}, 40000});
        return static_cast<int>(m_routers.size()) - 1;
    }
    // `router` under zero: a machine with an address of its own.
    int addMachine(Endpoint at, int router, Handler handler)
    {
        m_machines.push_back(Machine{at, router, std::move(handler), true});
        return static_cast<int>(m_machines.size()) - 1;
    }
    void setHandler(int machine, Handler handler)
    {
        m_machines[static_cast<usize>(machine)].handler = std::move(handler);
    }
    // A machine that is switched off hears nothing and says nothing.
    void setUp(int machine, bool up) { m_machines[static_cast<usize>(machine)].up = up; }

    void send(int machine, Endpoint to, std::span<const u8> datagram, u64 nowMs)
    {
        const Machine& from = m_machines[static_cast<usize>(machine)];
        if (!from.up)
            return;
        ++sent;
        if (from.router < 0)
            return route(from.at, to, datagram, nowMs);
        Router& router = m_routers[static_cast<usize>(from.router)];
        if (isPrivate(to.address)) {
            // Its own network, and nowhere else: a private address means
            // nothing past the router.
            for (usize index = 0; index < m_machines.size(); ++index)
                if (m_machines[index].router == from.router && m_machines[index].at == to)
                    m_queue.push_back(Packet{nowMs + LatencyMs, static_cast<int>(index), from.at, copy(datagram)});
            return;
        }
        // No hairpin: what most home routers do with their own address.
        if (to.address == router.publicAddress)
            return;
        const Mapping mapping{from.at, router.kind == Nat::Symmetric ? to : Endpoint{}};
        auto found = router.ports.find(mapping);
        if (found == router.ports.end())
            found = router.ports.emplace(mapping, router.nextPort++).first;
        router.wrote.insert({found->second, to.key()});
        route(Endpoint{router.publicAddress, found->second}, to, datagram, nowMs);
    }

    void step(u64 nowMs)
    {
        // What a delivery sends is delivered on a later step: nothing here
        // arrives the instant it leaves.
        std::vector<Packet> due;
        for (auto packet = m_queue.begin(); packet != m_queue.end();) {
            if (packet->atMs <= nowMs) {
                due.push_back(std::move(*packet));
                packet = m_queue.erase(packet);
            }
            else
                ++packet;
        }
        for (const Packet& packet : due) {
            const Machine& to = m_machines[static_cast<usize>(packet.to)];
            if (to.up && to.handler)
                to.handler(packet.from, packet.bytes);
        }
    }

    // The address a machine's datagrams to `to` are seen to come from.
    [[nodiscard]] Endpoint seenFrom(int machine, Endpoint to) const
    {
        const Machine& from = m_machines[static_cast<usize>(machine)];
        if (from.router < 0)
            return from.at;
        const Router& router = m_routers[static_cast<usize>(from.router)];
        const auto found = router.ports.find(Mapping{from.at, router.kind == Nat::Symmetric ? to : Endpoint{}});
        return found == router.ports.end() ? Endpoint{} : Endpoint{router.publicAddress, found->second};
    }

    u64 sent = 0;
    static constexpr u64 LatencyMs = 20;

private:
    struct Mapping
    {
        Endpoint inside{};
        Endpoint to{};
        bool operator<(const Mapping& other) const
        {
            return std::pair{inside.key(), to.key()} < std::pair{other.inside.key(), other.to.key()};
        }
    };
    struct Router
    {
        u32 publicAddress = 0;
        Nat kind = Nat::FullCone;
        std::map<Mapping, u16> ports;
        // A port, and where it has written to.
        std::set<std::pair<u16, u64>> wrote;
        u16 nextPort = 40000;
    };
    struct Machine
    {
        Endpoint at{};
        int router = -1;
        Handler handler;
        bool up = true;
    };
    struct Packet
    {
        u64 atMs = 0;
        int to = 0;
        Endpoint from{};
        std::vector<u8> bytes;
    };

    static bool isPrivate(u32 value) { return (value >> 24) == 10 || (value >> 16) == ((192u << 8) | 168u); }
    static std::vector<u8> copy(std::span<const u8> datagram) { return {datagram.begin(), datagram.end()}; }

    void route(Endpoint from, Endpoint to, std::span<const u8> datagram, u64 nowMs)
    {
        for (usize index = 0; index < m_machines.size(); ++index)
            if (m_machines[index].router < 0 && m_machines[index].at == to) {
                m_queue.push_back(Packet{nowMs + LatencyMs, static_cast<int>(index), from, copy(datagram)});
                return;
            }
        for (usize index = 0; index < m_routers.size(); ++index) {
            const Router& router = m_routers[index];
            if (router.publicAddress != to.address)
                continue;
            for (const auto& [mapping, port] : router.ports) {
                if (port != to.port)
                    continue;
                const bool let = router.kind == Nat::FullCone ||
                                 (router.kind == Nat::PortRestricted && router.wrote.contains({port, from.key()})) ||
                                 (router.kind == Nat::Symmetric && mapping.to == from);
                if (!let)
                    return;
                for (usize machine = 0; machine < m_machines.size(); ++machine)
                    if (m_machines[machine].router == static_cast<int>(index) &&
                        m_machines[machine].at == mapping.inside)
                        m_queue.push_back(Packet{nowMs + LatencyMs, static_cast<int>(machine), from, copy(datagram)});
                return;
            }
        }
    }

    std::vector<Router> m_routers;
    std::vector<Machine> m_machines;
    std::vector<Packet> m_queue;
};

Token tokenOf(u8 seed)
{
    Token token;
    for (usize index = 0; index < token.size(); ++index)
        token[index] = static_cast<u8>(seed * 31u + index * 7u + 1u);
    return token;
}

struct World;

// A host: its rendezvous, and a socket more for each joiner the relay carries.
struct HostSide
{
    World& world;
    int machine = -1;
    int router = -1;
    Endpoint at{};
    Token token{};
    HostRendezvous rendezvous;
    struct Pipe
    {
        HostPipe pipe;
        int machine = -1;
        // What the relay carried in through it: the joiner's own datagrams.
        std::vector<std::vector<u8>> in;
    };
    std::deque<Pipe> pipes;
    // Datagrams that were not the rendezvous's, and where each came from.
    std::vector<std::pair<Endpoint, std::vector<u8>>> match;

    HostSide(World& owner, Endpoint where, int behind, u8 seed);
    void start();
    void tick();
    [[nodiscard]] Send send();
    [[nodiscard]] Pipe* pipeOf(u16 slot);
};

struct JoinSide
{
    World& world;
    int machine = -1;
    Endpoint at{};
    JoinRendezvous rendezvous;
    std::vector<std::pair<Endpoint, std::vector<u8>>> match;

    JoinSide(World& owner, Endpoint where, int behind);
    void start(const Code& code, u64 nonce, bool direct = true);
    void tick();
    [[nodiscard]] Send send();
    void say(std::span<const u8> datagram);
};

struct World
{
    Network net;
    Endpoint relayAt{address(203, 0, 113, 1), 7790};
    RelayLimits limits{};
    std::unique_ptr<RelayServer> relay;
    int relayMachine = -1;
    u64 now = 1'000;
    std::deque<HostSide> hosts;
    std::deque<JoinSide> joiners;

    explicit World(const RelayLimits& with = {}) : limits(with), relay(std::make_unique<RelayServer>(with))
    {
        relayMachine = net.addMachine(relayAt, -1, [this](Endpoint from, std::span<const u8> datagram) {
            relay->receive(from, datagram, now,
                           [this](Endpoint to, std::span<const u8> reply) { net.send(relayMachine, to, reply, now); });
        });
    }
    // The relay's program is stopped and started: it remembers nothing.
    void restartRelay() { relay = std::make_unique<RelayServer>(limits); }

    HostSide& host(Endpoint at, int router, u8 seed = 1) { return hosts.emplace_back(*this, at, router, seed); }
    JoinSide& joiner(Endpoint at, int router) { return joiners.emplace_back(*this, at, router); }

    void run(u64 milliseconds)
    {
        const u64 until = now + milliseconds;
        while (now < until) {
            now += 10;
            net.step(now);
            for (HostSide& side : hosts)
                side.tick();
            for (JoinSide& side : joiners)
                side.tick();
            if (now % 250 == 0)
                relay->tick(now);
        }
    }
};

HostSide::HostSide(World& owner, Endpoint where, int behind, u8 seed)
    : world(owner), router(behind), at(where), token(tokenOf(seed))
{
    machine = world.net.addMachine(at, router, [this](Endpoint from, std::span<const u8> datagram) {
        if (!rendezvous.receive(from, datagram, world.now, send()))
            match.emplace_back(from, std::vector<u8>(datagram.begin(), datagram.end()));
    });
}

Send HostSide::send()
{
    return [this](Endpoint to, std::span<const u8> datagram) { world.net.send(machine, to, datagram, world.now); };
}

void HostSide::start()
{
    Locals locals;
    locals.address[0] = at.address;
    locals.count = 1;
    locals.port = at.port;
    rendezvous.start(world.relayAt, token, locals, world.now);
}

HostSide::Pipe* HostSide::pipeOf(u16 slot)
{
    for (Pipe& pipe : pipes)
        if (pipe.pipe.slot() == slot)
            return &pipe;
    return nullptr;
}

void HostSide::tick()
{
    rendezvous.tick(world.now, send());
    for (const HostRendezvous::PipeRequest& request : rendezvous.takePipeRequests()) {
        if (pipeOf(request.slot) != nullptr)
            continue;
        Pipe& pipe = pipes.emplace_back();
        // Another socket of the same machine: its address, another port.
        const Endpoint where{at.address, static_cast<u16>(at.port + 100 + pipes.size())};
        pipe.machine = world.net.addMachine(where, router, [this, &pipe](Endpoint from, std::span<const u8> datagram) {
            if (!pipe.pipe.receive(from, datagram, world.now) && from == world.relayAt)
                pipe.in.emplace_back(datagram.begin(), datagram.end());
        });
        pipe.pipe.start(world.relayAt, token, request, world.now);
    }
    for (Pipe& pipe : pipes) {
        pipe.pipe.tick(world.now, [this, &pipe](Endpoint to, std::span<const u8> datagram) {
            world.net.send(pipe.machine, to, datagram, world.now);
        });
        if (pipe.pipe.takeLost())
            rendezvous.refresh();
    }
}

JoinSide::JoinSide(World& owner, Endpoint where, int behind) : world(owner), at(where)
{
    machine = world.net.addMachine(at, behind, [this](Endpoint from, std::span<const u8> datagram) {
        if (!rendezvous.receive(from, datagram, world.now, send()))
            match.emplace_back(from, std::vector<u8>(datagram.begin(), datagram.end()));
    });
}

Send JoinSide::send()
{
    return [this](Endpoint to, std::span<const u8> datagram) { world.net.send(machine, to, datagram, world.now); };
}

void JoinSide::start(const Code& code, u64 nonce, bool direct)
{
    Locals locals;
    locals.address[0] = at.address;
    locals.count = 1;
    locals.port = at.port;
    rendezvous.start(world.relayAt, code, locals, nonce, world.now, JoinRendezvous::Options{.direct = direct});
}

void JoinSide::tick()
{
    rendezvous.tick(world.now, send());
}

void JoinSide::say(std::span<const u8> datagram)
{
    world.net.send(machine, rendezvous.target(), datagram, world.now);
}

// A datagram of a match, as ENet would make one: anything that is not marked.
std::vector<u8> matchBytes(u8 seed, usize size = 48)
{
    std::vector<u8> bytes(size);
    for (usize index = 0; index < size; ++index)
        bytes[index] = static_cast<u8>(seed + index * 3u);
    bytes[0] = 0x80;
    return bytes;
}

} // namespace

TEST_CASE("a join code is its token's: eight characters nobody mistakes, read back however they are typed")
{
    const Token token = tokenOf(1);
    const Code code = codeOf(token);
    CHECK(code == codeOf(token));
    CHECK(code != codeOf(tokenOf(2)));
    for (const char character : code.text) {
        CHECK(std::string_view("23456789ABCDEFGHJKLMNPQRSTUVWXYZ").find(character) != std::string_view::npos);
    }
    std::string typed = code.str();
    CHECK(parseCode(typed) == code);
    // As a person types one: lower case, in two halves.
    std::string loose;
    for (usize index = 0; index < typed.size(); ++index) {
        if (index == 4)
            loose += " - ";
        const char character = typed[index];
        loose += character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }
    CHECK(parseCode(loose) == code);
    CHECK_FALSE(parseCode("ABCD").has_value());
    CHECK_FALSE(parseCode("ABCDEFGHJ").has_value());
    // No 0, 1, I or O in the alphabet: one of them typed is a mistake, said.
    CHECK_FALSE(parseCode("ABCDEFG0").has_value());
    CHECK_FALSE(parseCode("ABCDEFGI").has_value());
}

TEST_CASE("an address is read and written as people write one")
{
    CHECK(parseAddress("192.168.0.7") == address(192, 168, 0, 7));
    CHECK_FALSE(parseAddress("192.168.0").has_value());
    CHECK_FALSE(parseAddress("192.168.0.256").has_value());
    CHECK_FALSE(parseAddress("relay.example").has_value());
    const auto endpoint = parseEndpoint("203.0.113.9:7790", 1);
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->address == address(203, 0, 113, 9));
    CHECK(endpoint->port == 7790);
    CHECK(toText(*endpoint) == "203.0.113.9:7790");
    CHECK(parseEndpoint("203.0.113.9", 7790) == endpoint);
    CHECK_FALSE(parseEndpoint("203.0.113.9:0", 7790).has_value());
    CHECK_FALSE(parseEndpoint("203.0.113.9:70000", 7790).has_value());
}

TEST_CASE("every message survives its datagram, and nothing else is taken for one")
{
    Locals locals;
    locals.address = {address(192, 168, 1, 20), address(10, 0, 0, 4), 0, 0};
    locals.count = 2;
    locals.port = 7777;
    const Token token = tokenOf(9);
    const Endpoint endpoint{address(198, 51, 100, 7), 40123};

    {
        Register out;
        REQUIRE(decode(encode(Register{token, codeOf(token), Version, 3, 8, locals}), out));
        CHECK(out.token == token);
        CHECK(out.code == codeOf(token));
        CHECK(out.players == 3);
        CHECK(out.maxPlayers == 8);
        CHECK(out.locals.count == 2);
        CHECK(out.locals.address[1] == address(10, 0, 0, 4));
        CHECK(out.locals.port == 7777);
    }
    {
        Registered out;
        REQUIRE(decode(encode(Registered{codeOf(token), endpoint, 10, Refusal::Busy}), out));
        CHECK(out.seen == endpoint);
        CHECK(out.keepAliveSeconds == 10);
        CHECK(out.refusal == Refusal::Busy);
    }
    {
        Lookup out;
        REQUIRE(decode(encode(Lookup{codeOf(token), 0x0123456789ABCDEFull, Version, locals}), out));
        CHECK(out.code == codeOf(token));
        CHECK(out.nonce == 0x0123456789ABCDEFull);
        CHECK(out.locals.address[0] == address(192, 168, 1, 20));
    }
    {
        LookupReply out;
        REQUIRE(decode(encode(LookupReply{77, 5, endpoint, true, locals}), out));
        CHECK(out.nonce == 77);
        CHECK(out.slot == 5);
        CHECK(out.host == endpoint);
        CHECK(out.sameNetwork);
        CHECK(out.locals.count == 2);
    }
    {
        Introduce out;
        REQUIRE(decode(encode(Introduce{77, 5, endpoint, false, {}}), out));
        CHECK(out.joiner == endpoint);
        CHECK_FALSE(out.sameNetwork);
    }
    {
        PipeOpen out;
        REQUIRE(decode(encode(PipeOpen{token, 77, 5, endpoint}), out));
        CHECK(out.token == token);
        CHECK(out.slot == 5);
        CHECK(out.joiner == endpoint);
        Close closed;
        REQUIRE(decode(encode(Close{token, 5, 600}), closed));
        CHECK(closed.banSeconds == 600);
        Pong pong;
        REQUIRE(decode(encode(Pong{9, Version, 12, 3, 51200, 86400}), pong));
        CHECK(pong.sessions == 12);
        CHECK(pong.relayed == 3);
        CHECK(pong.bytesPerSecond == 51200);
        CHECK(pong.uptimeSeconds == 86400);
    }

    // One type is not read as another, a cut one not at all, and a match's
    // datagram is not a message.
    const std::vector<u8> punch = encode(Punch{77, true});
    PunchAck ack;
    CHECK_FALSE(decode(punch, ack));
    Punch cut;
    CHECK_FALSE(decode(std::span<const u8>(punch).first(punch.size() - 1), cut));
    CHECK(marked(punch));
    CHECK(typeOf(punch) == Type::Punch);
    const std::vector<u8> game = matchBytes(1);
    CHECK_FALSE(marked(game));
    CHECK_FALSE(typeOf(game).has_value());
    std::vector<u8> unknown = punch;
    unknown[4] = 200;
    CHECK_FALSE(typeOf(unknown).has_value());
    // A code with a character outside the alphabet is not a registration.
    std::vector<u8> badCode = encode(Register{token, codeOf(token), Version, 0, 0, locals});
    badCode[5 + 16] = '0';
    Register refused;
    CHECK_FALSE(decode(badCode, refused));
}

TEST_CASE("a relay answers with no more than it was sent: it amplifies nothing")
{
    const Token token = tokenOf(3);
    Locals locals;
    locals.count = 4;
    CHECK(encode(Registered{}).size() <= encode(Register{}).size());
    CHECK(encode(LookupReply{0, 0, {}, true, locals}).size() <= encode(Lookup{}).size());
    CHECK(encode(LookupFailed{}).size() <= encode(Lookup{}).size());
    CHECK(encode(RelayReady{}).size() <= encode(NeedRelay{}).size());
    CHECK(encode(RelayReady{}).size() * 2 <= encode(PipeOpen{}).size());
    CHECK(encode(LookupFailed{}).size() <= encode(PipeOpen{}).size());
    CHECK(encode(Pong{}).size() <= encode(Ping{}).size());

    // And a lookup or a ping cut short to draw a longer answer is not answered.
    World world;
    HostSide& host = world.host({address(198, 51, 100, 2), 7777}, -1);
    host.start();
    world.run(200);
    REQUIRE(host.rendezvous.state() == RelayState::Ready);
    std::vector<std::vector<u8>> answers;
    const int stranger =
        world.net.addMachine({address(198, 51, 100, 66), 5000}, -1, [&](Endpoint, std::span<const u8> datagram) {
            answers.emplace_back(datagram.begin(), datagram.end());
        });
    std::vector<u8> lookup = encode(Lookup{codeOf(token), 1, Version, {}});
    lookup.resize(30);
    world.net.send(stranger, world.relayAt, lookup, world.now);
    std::vector<u8> ping = encode(Ping{1});
    ping.resize(13);
    world.net.send(stranger, world.relayAt, ping, world.now);
    world.run(200);
    CHECK(answers.empty());
    world.net.send(stranger, world.relayAt, encode(Ping{42}), world.now);
    world.run(200);
    REQUIRE(answers.size() == 1);
    Pong pong;
    REQUIRE(decode(answers[0], pong));
    CHECK(pong.nonce == 42);
    CHECK(pong.sessions == 1);
    CHECK(pong.version == Version);
}

TEST_CASE("a host registers and its code resolves; a code is nobody's without its token")
{
    World world;
    HostSide& host = world.host({address(198, 51, 100, 2), 7777}, -1);
    CHECK(host.rendezvous.state() == RelayState::None);
    host.start();
    CHECK(host.rendezvous.state() == RelayState::Connecting);
    world.run(200);
    CHECK(host.rendezvous.state() == RelayState::Ready);
    CHECK(host.rendezvous.code() == codeOf(host.token));
    CHECK(host.rendezvous.seen() == host.at);
    CHECK(world.relay->stats(world.now).sessions == 1);

    // Somebody else says the same code with a token of their own: not theirs.
    std::vector<std::vector<u8>> answers;
    const int thief =
        world.net.addMachine({address(198, 51, 100, 66), 5000}, -1, [&](Endpoint, std::span<const u8> datagram) {
            answers.emplace_back(datagram.begin(), datagram.end());
        });
    world.net.send(thief, world.relayAt, encode(Register{tokenOf(50), host.rendezvous.code(), Version, 0, 0, {}}),
                   world.now);
    world.run(200);
    CHECK(answers.empty());
    CHECK(world.relay->stats(world.now).sessions == 1);

    // The code leads to the host and to nobody else.
    JoinSide& joiner = world.joiner({address(198, 51, 100, 3), 6000}, -1);
    joiner.start(host.rendezvous.code(), 11);
    world.run(1'000);
    REQUIRE(joiner.rendezvous.found());
    CHECK(joiner.rendezvous.target() == host.at);

    // A host that stops says so, and the code is nobody's at once.
    host.rendezvous.stop(host.send());
    world.run(200);
    CHECK(world.relay->stats(world.now).sessions == 0);
    JoinSide& late = world.joiner({address(198, 51, 100, 4), 6000}, -1);
    late.start(codeOf(host.token), 12);
    world.run(500);
    CHECK(late.rendezvous.failed());
    CHECK(late.rendezvous.failure() == JoinFailure::NoSession);
}

TEST_CASE("two machines the internet reaches connect to each other, and the relay carries nothing")
{
    World world;
    HostSide& host = world.host({address(198, 51, 100, 2), 7777}, -1);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(198, 51, 100, 3), 6000}, -1);
    joiner.start(host.rendezvous.code(), 21);
    world.run(1'000);
    REQUIRE(joiner.rendezvous.found());
    CHECK(joiner.rendezvous.path() == Path::Direct);
    CHECK(joiner.rendezvous.target() == host.at);
    CHECK(world.relay->stats(world.now).relayed == 0);
    CHECK(host.pipes.empty());
}

TEST_CASE("behind two home routers, the knock from both sides opens a path each to the other")
{
    for (const Nat kind : {Nat::FullCone, Nat::PortRestricted}) {
        World world;
        const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), kind);
        const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), kind);
        HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
        host.start();
        world.run(200);
        REQUIRE(host.rendezvous.state() == RelayState::Ready);
        // The host as the world sees it is its router, not itself.
        CHECK(host.rendezvous.seen().address == address(198, 51, 100, 10));

        JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
        joiner.start(host.rendezvous.code(), 31);
        world.run(1'500);
        REQUIRE(joiner.rendezvous.found());
        CHECK(joiner.rendezvous.path() == Path::Direct);
        CHECK(joiner.rendezvous.target() == host.rendezvous.seen());
        CHECK(world.relay->stats(world.now).relayed == 0);

        // And the match's own datagrams cross that path both ways.
        joiner.say(matchBytes(1));
        world.run(100);
        REQUIRE(host.match.size() == 1);
        CHECK(host.match[0].second == matchBytes(1));
        world.net.send(host.machine, host.match[0].first, matchBytes(2), world.now);
        world.run(100);
        REQUIRE(joiner.match.size() == 1);
        CHECK(joiner.match[0].second == matchBytes(2));
    }
}

TEST_CASE("two machines behind one router reach each other by their own addresses")
{
    World world;
    const int router = world.net.addRouter(address(198, 51, 100, 10), Nat::PortRestricted);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, router);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, router);
    joiner.start(host.rendezvous.code(), 41);
    world.run(1'500);
    REQUIRE(joiner.rendezvous.found());
    CHECK(joiner.rendezvous.path() == Path::Lan);
    CHECK(joiner.rendezvous.target() == host.at);
    CHECK(world.relay->stats(world.now).relayed == 0);
}

TEST_CASE("a router that opens a port per destination: direct where the other side lets anyone in, the relay where not")
{
    SUBCASE("the joiner's is such a router, the host's lets anything in: the host answers where the knock came from")
    {
        World world;
        const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::FullCone);
        const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
        HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
        host.start();
        world.run(200);
        JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
        joiner.start(host.rendezvous.code(), 51);
        world.run(1'500);
        REQUIRE(joiner.rendezvous.found());
        CHECK(joiner.rendezvous.path() == Path::Direct);
    }
    SUBCASE("the host's is such a router, the joiner's lets anything in: the joiner knocks where the host's came from")
    {
        World world;
        const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
        const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::FullCone);
        HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
        host.start();
        world.run(200);
        JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
        joiner.start(host.rendezvous.code(), 52);
        world.run(1'500);
        REQUIRE(joiner.rendezvous.found());
        CHECK(joiner.rendezvous.path() == Path::Direct);
        // Not where the relay saw the host: the port its router opened for
        // the joiner.
        CHECK(joiner.rendezvous.target() != host.rendezvous.seen());
        joiner.say(matchBytes(5));
        world.run(100);
        REQUIRE(host.match.size() == 1);
    }
    SUBCASE("against a router that lets in only whom it wrote to, and between two such: the relay")
    {
        for (const Nat other : {Nat::PortRestricted, Nat::Symmetric}) {
            World world;
            const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), other);
            const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
            HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
            host.start();
            world.run(200);
            JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
            joiner.start(host.rendezvous.code(), 53);
            // Two and a half seconds of knocking first.
            world.run(2'000);
            CHECK_FALSE(joiner.rendezvous.found());
            world.run(1'500);
            REQUIRE(joiner.rendezvous.found());
            CHECK(joiner.rendezvous.path() == Path::Relayed);
            CHECK(joiner.rendezvous.target() == world.relayAt);
            CHECK(world.relay->stats(world.now).relayed == 1);
        }
    }
}

TEST_CASE("through the relay a match's datagrams cross as they are, both ways, and are counted")
{
    World world;
    const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
    const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
    // Asked not to knock at all: straight to the relay.
    joiner.start(host.rendezvous.code(), 61, false);
    world.run(600);
    REQUIRE(joiner.rendezvous.found());
    CHECK(joiner.rendezvous.path() == Path::Relayed);
    REQUIRE(host.pipes.size() == 1);
    HostSide::Pipe& pipe = host.pipes.front();
    CHECK(pipe.pipe.ready());

    const u64 before = world.relay->stats(world.now).bytes;
    for (u8 index = 0; index < 5; ++index)
        joiner.say(matchBytes(index, 100));
    world.run(100);
    REQUIRE(pipe.in.size() == 5);
    for (u8 index = 0; index < 5; ++index)
        CHECK(pipe.in[index] == matchBytes(index, 100));
    // Back: from the host's socket for that joiner, to the relay.
    for (u8 index = 0; index < 3; ++index)
        world.net.send(pipe.machine, world.relayAt, matchBytes(static_cast<u8>(100 + index), 60), world.now);
    world.run(100);
    REQUIRE(joiner.match.size() == 3);
    CHECK(joiner.match[0].first == world.relayAt);
    CHECK(joiner.match[2].second == matchBytes(102, 60));
    // Not a byte added to any of them.
    CHECK(world.relay->stats(world.now).bytes - before == 5 * 100 + 3 * 60);
    CHECK(world.relay->stats(world.now).packets == 8);

    // A second joiner is another slot and another socket, and neither hears
    // the other's.
    JoinSide& second =
        world.joiner({address(192, 168, 0, 9), 6000}, world.net.addRouter(address(198, 51, 100, 30), Nat::Symmetric));
    second.start(host.rendezvous.code(), 62, false);
    world.run(600);
    REQUIRE(second.rendezvous.found());
    REQUIRE(host.pipes.size() == 2);
    second.say(matchBytes(200));
    world.run(100);
    CHECK(host.pipes[0].in.size() == 5);
    REQUIRE(host.pipes[1].in.size() == 1);
    CHECK(host.pipes[1].in[0] == matchBytes(200));
    CHECK(world.relay->stats(world.now).relayed == 2);

    // A stranger's datagram to the relay goes nowhere.
    const u64 dropped = world.relay->stats(world.now).dropped;
    const int stranger = world.net.addMachine({address(198, 51, 100, 66), 5000}, -1, {});
    world.net.send(stranger, world.relayAt, matchBytes(9), world.now);
    world.run(100);
    CHECK(world.relay->stats(world.now).dropped == dropped + 1);
    CHECK(host.pipes[0].in.size() == 5);
}

TEST_CASE("a joiner a host lets go is closed at the relay, and may not look the code up while it is kept away")
{
    World world;
    const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
    const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
    joiner.start(host.rendezvous.code(), 71, false);
    world.run(600);
    REQUIRE(joiner.rendezvous.found());
    REQUIRE(host.pipes.size() == 1);

    host.rendezvous.close(host.pipes[0].pipe.slot(), 60, host.send());
    world.run(100);
    CHECK(world.relay->stats(world.now).relayed == 0);
    // What it sends now goes nowhere.
    joiner.say(matchBytes(1));
    world.run(100);
    CHECK(host.pipes[0].in.empty());

    joiner.start(host.rendezvous.code(), 72, false);
    world.run(500);
    REQUIRE(joiner.rendezvous.failed());
    CHECK(joiner.rendezvous.failure() == JoinFailure::Refused);
    // Somebody else is not kept away by it.
    JoinSide& other =
        world.joiner({address(192, 168, 0, 9), 6000}, world.net.addRouter(address(198, 51, 100, 30), Nat::Symmetric));
    other.start(host.rendezvous.code(), 73, false);
    world.run(600);
    CHECK(other.rendezvous.found());
    // And the minute over, the first may come back.
    world.run(60'000);
    joiner.start(host.rendezvous.code(), 74, false);
    world.run(600);
    CHECK(joiner.rendezvous.found());
}

TEST_CASE("a join that cannot be: no such code, a host with no room, a relay that does not answer")
{
    World world;
    HostSide& host = world.host({address(198, 51, 100, 2), 7777}, -1);
    host.start();
    host.rendezvous.setPlayers(4, 4);
    world.run(200);
    REQUIRE(host.rendezvous.state() == RelayState::Ready);

    JoinSide& joiner = world.joiner({address(198, 51, 100, 3), 6000}, -1);
    joiner.start(*parseCode("22222222"), 81);
    world.run(300);
    REQUIRE(joiner.rendezvous.failed());
    CHECK(joiner.rendezvous.failure() == JoinFailure::NoSession);

    joiner.start(host.rendezvous.code(), 82);
    world.run(300);
    REQUIRE(joiner.rendezvous.failed());
    CHECK(joiner.rendezvous.failure() == JoinFailure::Full);
    // Nobody was introduced: a full host is not knocked on.
    const u64 sent = world.net.sent;
    world.run(500);
    CHECK(world.net.sent == sent);

    // A place frees: said to the relay at once, not at the next keep-alive.
    host.rendezvous.setPlayers(3, 4);
    world.run(100);
    joiner.start(host.rendezvous.code(), 83);
    world.run(1'000);
    CHECK(joiner.rendezvous.found());

    // The relay's machine is off.
    world.net.setUp(world.relayMachine, false);
    joiner.start(host.rendezvous.code(), 84);
    world.run(2'500);
    CHECK_FALSE(joiner.rendezvous.failed());
    world.run(1'000);
    REQUIRE(joiner.rendezvous.failed());
    CHECK(joiner.rendezvous.failure() == JoinFailure::RelayUnreachable);
    // And the host says so too, without ceasing to be a host.
    world.run(16'000);
    CHECK(host.rendezvous.state() == RelayState::Unreachable);
    world.net.setUp(world.relayMachine, true);
    world.run(6'000);
    CHECK(host.rendezvous.state() == RelayState::Ready);
}

TEST_CASE("a relay that is restarted: the same code resolves again, and a joiner it carried is carried again")
{
    World world;
    const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
    const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
    host.start();
    world.run(200);
    const Code code = host.rendezvous.code();
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
    joiner.start(code, 91, false);
    world.run(600);
    REQUIRE(joiner.rendezvous.found());
    REQUIRE(host.pipes.size() == 1);

    world.restartRelay();
    CHECK(world.relay->stats(world.now).sessions == 0);
    joiner.say(matchBytes(1));
    world.run(100);
    CHECK(host.pipes[0].in.empty());

    // The host's socket for the joiner says its slot every three seconds; the
    // relay answers that it knows no such session; the host registers again
    // and the slot is the relay's once more -- with nothing asked of the
    // joiner, who never knew.
    world.run(4'000);
    CHECK(world.relay->stats(world.now).sessions == 1);
    CHECK(world.relay->stats(world.now).relayed == 1);
    CHECK(host.rendezvous.code() == code);
    joiner.say(matchBytes(2));
    world.net.send(host.pipes[0].machine, world.relayAt, matchBytes(3), world.now);
    world.run(100);
    REQUIRE(host.pipes[0].in.size() == 1);
    CHECK(host.pipes[0].in[0] == matchBytes(2));
    REQUIRE(joiner.match.size() == 1);
    CHECK(joiner.match[0].second == matchBytes(3));

    // With nobody carried, it is the keep-alive that brings the code back:
    // ten seconds at most.
    World quiet;
    HostSide& alone = quiet.host({address(198, 51, 100, 2), 7777}, -1);
    alone.start();
    quiet.run(200);
    quiet.restartRelay();
    quiet.run(10'500);
    CHECK(quiet.relay->stats(quiet.now).sessions == 1);
    JoinSide& late = quiet.joiner({address(198, 51, 100, 3), 6000}, -1);
    late.start(alone.rendezvous.code(), 92);
    quiet.run(1'000);
    CHECK(late.rendezvous.found());
}

TEST_CASE("a host that vanishes: its code stops resolving, and what its joiners send goes nowhere")
{
    World world;
    const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
    const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
    joiner.start(host.rendezvous.code(), 101, false);
    world.run(600);
    REQUIRE(joiner.rendezvous.found());

    // The power goes: no goodbye.
    world.net.setUp(host.machine, false);
    world.net.setUp(host.pipes[0].machine, false);
    world.run(29'000);
    CHECK(world.relay->stats(world.now).sessions == 1);
    world.run(2'500);
    CHECK(world.relay->stats(world.now).sessions == 0);
    CHECK(world.relay->stats(world.now).relayed == 0);
    const u64 dropped = world.relay->stats(world.now).dropped;
    joiner.say(matchBytes(1));
    world.run(100);
    CHECK(world.relay->stats(world.now).dropped == dropped + 1);

    JoinSide& late = world.joiner({address(198, 51, 100, 3), 6000}, -1);
    late.start(host.rendezvous.code(), 102);
    world.run(500);
    REQUIRE(late.rendezvous.failed());
    CHECK(late.rendezvous.failure() == JoinFailure::NoSession);
}

TEST_CASE("a relay is bounded: bytes a second for a joiner, lookups from an address, sessions in all")
{
    RelayLimits limits;
    limits.slotBytesPerSecond = 1'000;
    limits.lookupsPerSecond = 2;
    limits.maxSessions = 2;
    World world(limits);
    const int hostRouter = world.net.addRouter(address(198, 51, 100, 10), Nat::Symmetric);
    const int joinRouter = world.net.addRouter(address(198, 51, 100, 20), Nat::Symmetric);
    HostSide& host = world.host({address(192, 168, 0, 5), 7777}, hostRouter);
    host.start();
    world.run(200);
    JoinSide& joiner = world.joiner({address(192, 168, 0, 9), 6000}, joinRouter);
    joiner.start(host.rendezvous.code(), 111, false);
    world.run(600);
    REQUIRE(joiner.rendezvous.found());

    // Thirty datagrams of a hundred bytes at once, where a thousand a second
    // are allowed: ten cross and the rest fall, as on a full link.
    for (u8 index = 0; index < 30; ++index)
        joiner.say(matchBytes(index, 100));
    world.run(100);
    CHECK(host.pipes[0].in.size() == 10);
    // A second later there is room for ten more, and the other way was never
    // short.
    world.run(1'000);
    for (u8 index = 0; index < 30; ++index)
        joiner.say(matchBytes(index, 100));
    world.net.send(host.pipes[0].machine, world.relayAt, matchBytes(7, 100), world.now);
    world.run(100);
    CHECK(host.pipes[0].in.size() == 20);
    CHECK(joiner.match.size() == 1);

    // Lookups: four at once from one address are answered, the fifth is not.
    int answers = 0;
    const int asker =
        world.net.addMachine({address(198, 51, 100, 66), 5000}, -1, [&](Endpoint, std::span<const u8>) { ++answers; });
    for (u64 nonce = 0; nonce < 9; ++nonce)
        world.net.send(asker, world.relayAt, encode(Lookup{*parseCode("22222222"), nonce, Version, {}}), world.now);
    world.run(100);
    CHECK(answers == 4);

    // Sessions: the third host is told the relay is at its limit, and says
    // so as a relay it cannot use.
    HostSide& second = world.host({address(198, 51, 100, 40), 7777}, -1, 2);
    second.start();
    HostSide& third = world.host({address(198, 51, 100, 41), 7777}, -1, 3);
    third.start();
    world.run(300);
    CHECK(second.rendezvous.state() == RelayState::Ready);
    CHECK(third.rendezvous.state() == RelayState::Unreachable);
    CHECK(world.relay->stats(world.now).sessions == 2);
}

TEST_CASE("a host answers a knock only from whom the relay introduced")
{
    World world;
    HostSide& host = world.host({address(198, 51, 100, 2), 7777}, -1);
    host.start();
    world.run(200);
    int answers = 0;
    const int stranger =
        world.net.addMachine({address(198, 51, 100, 66), 5000}, -1, [&](Endpoint, std::span<const u8>) { ++answers; });
    world.net.send(stranger, host.at, encode(Punch{12345, false}), world.now);
    world.run(200);
    CHECK(answers == 0);
    // And a knock is never the match's: the host's own code does not see it.
    CHECK(host.match.empty());
}
