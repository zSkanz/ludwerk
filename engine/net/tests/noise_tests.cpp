// A match's handshake and its sealed packets (ADR 0224): the primitives
// against their published answers, and the Noise XX state machine and the
// packet window against what each must refuse.
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/crypto.h"
#include "engine/net/noise.h"

using namespace engine;
using net::NoiseHandshake;

namespace {

template <core::usize N>
[[nodiscard]] std::array<core::u8, N> fromHex(std::string_view hex)
{
    REQUIRE(hex.size() == N * 2);
    std::array<core::u8, N> out{};
    for (core::usize at = 0; at < N; ++at)
        out[at] = static_cast<core::u8>(std::stoi(std::string(hex.substr(at * 2, 2)), nullptr, 16));
    return out;
}

[[nodiscard]] std::span<const core::u8> bytes(std::string_view text)
{
    return {reinterpret_cast<const core::u8*>(text.data()), text.size()};
}

struct Pair
{
    NoiseHandshake initiator;
    NoiseHandshake responder;
};

// Runs the three messages; false at the first that is refused.
bool shake(NoiseHandshake& initiator, NoiseHandshake& responder)
{
    for (int message = 0; message < 3; ++message) {
        NoiseHandshake& writer = initiator.myTurn() ? initiator : responder;
        NoiseHandshake& reader = &writer == &initiator ? responder : initiator;
        const std::optional<std::vector<core::u8>> sent = writer.write();
        if (!sent.has_value() || !reader.read(*sent))
            return false;
    }
    return initiator.done() && responder.done();
}

} // namespace

TEST_CASE("the primitives give their published answers")
{
    // BLAKE2b-512 of "abc" (RFC 7693, appendix A).
    CHECK(core::toHex(core::blake2b(bytes("abc"))) ==
          "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
          "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");

    // X25519 (RFC 7748 §6.1): Alice's and Bob's public keys and their secret.
    const auto alice = fromHex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob = fromHex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    CHECK(core::toHex(core::x25519Public(alice)) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    std::array<core::u8, 32> shared{};
    REQUIRE(core::x25519(alice, core::x25519Public(bob), shared));
    CHECK(core::toHex(shared) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    // A low-order point makes no secret, and is refused.
    REQUIRE_FALSE(core::x25519(alice, std::array<core::u8, 32>{}, shared));

    // Ed25519 (RFC 8032 §7.1, test 1): the empty message.
    std::array<core::u8, 64> secret{};
    const auto seed = fromHex<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const auto pub = fromHex<32>("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    std::copy(seed.begin(), seed.end(), secret.begin());
    std::copy(pub.begin(), pub.end(), secret.begin() + 32);
    const auto signature = core::ed25519Sign(secret, {});
    CHECK(core::toHex(signature) == "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
                                    "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(core::ed25519Verify(pub, {}, signature));
    auto forged = signature;
    forged[0] ^= 1u;
    CHECK_FALSE(core::ed25519Verify(pub, {}, forged));

    // A seal opens with what sealed it and nothing else.
    const std::array<core::u8, 32> key =
        fromHex<32>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    std::vector<core::u8> sealed;
    core::aeadSeal(key, 7, bytes("ad"), bytes("hello"), sealed);
    CHECK(sealed.size() == 5 + core::AeadTagBytes);
    std::vector<core::u8> opened;
    REQUIRE(core::aeadOpen(key, 7, bytes("ad"), sealed, opened));
    CHECK(std::string(opened.begin(), opened.end()) == "hello");
    CHECK_FALSE(core::aeadOpen(key, 8, bytes("ad"), sealed, opened));
    CHECK_FALSE(core::aeadOpen(key, 7, bytes("da"), sealed, opened));
}

TEST_CASE("two sides that shake hands share keys and know each other's static key")
{
    const core::X25519Pair client = core::x25519Generate();
    const core::X25519Pair server = core::x25519Generate();
    const auto prologue = bytes("match 53 com.example.game");
    NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, prologue);
    NoiseHandshake responder(NoiseHandshake::Role::Responder, server, prologue);
    REQUIRE(shake(initiator, responder));
    CHECK(initiator.remoteStatic() == server.pub);
    CHECK(responder.remoteStatic() == client.pub);
    const NoiseHandshake::Keys mine = initiator.split();
    const NoiseHandshake::Keys theirs = responder.split();
    CHECK(mine.send == theirs.receive);
    CHECK(mine.receive == theirs.send);
    CHECK(mine.send != mine.receive);

    // And the packets after it go both ways.
    net::NoiseSender toServer(mine.send);
    net::NoiseReceiver atServer(theirs.receive);
    std::vector<core::u8> packet;
    toServer.seal(bytes("intent"), packet);
    CHECK(packet.size() == 6 + net::NoiseOverheadBytes);
    std::vector<core::u8> plain;
    REQUIRE(atServer.open(packet, plain));
    CHECK(std::string(plain.begin(), plain.end()) == "intent");
}

TEST_CASE("a handshake refuses another game, a changed message and a message out of turn")
{
    const core::X25519Pair client = core::x25519Generate();
    const core::X25519Pair server = core::x25519Generate();

    SUBCASE("another prologue: another protocol version or another game")
    {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, bytes("match 53 game.one"));
        NoiseHandshake responder(NoiseHandshake::Role::Responder, server, bytes("match 53 game.two"));
        CHECK_FALSE(shake(initiator, responder));
    }
    SUBCASE("a byte changed on the way")
    {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, bytes("p"));
        NoiseHandshake responder(NoiseHandshake::Role::Responder, server, bytes("p"));
        REQUIRE(responder.read(*initiator.write()));
        std::vector<core::u8> second = *responder.write();
        second[40] ^= 0x10u;
        CHECK_FALSE(initiator.read(second));
        CHECK(initiator.failed());
        CHECK_FALSE(initiator.write().has_value());
    }
    SUBCASE("a message out of turn")
    {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, bytes("p"));
        NoiseHandshake responder(NoiseHandshake::Role::Responder, server, bytes("p"));
        CHECK_FALSE(responder.write().has_value());
        const std::vector<core::u8> first = *initiator.write();
        CHECK_FALSE(initiator.read(first));
    }
}

TEST_CASE("a sealed packet is refused played back, too old, or forged, and taken out of order")
{
    const std::array<core::u8, 32> key = core::x25519Generate().secret;
    net::NoiseSender sender(key);
    net::NoiseReceiver receiver(key);
    std::vector<std::vector<core::u8>> sent(2000);
    for (std::vector<core::u8>& packet : sent)
        sender.seal(bytes("x"), packet);
    std::vector<core::u8> plain;

    // In another order, within the window: taken.
    CHECK(receiver.open(sent[5], plain));
    CHECK(receiver.open(sent[3], plain));
    CHECK(receiver.open(sent[4], plain));
    // Played back: refused.
    CHECK_FALSE(receiver.open(sent[3], plain));
    CHECK_FALSE(receiver.open(sent[5], plain));
    // Far ahead, then one that has fallen out of the window: refused.
    CHECK(receiver.open(sent[1500], plain));
    CHECK_FALSE(receiver.open(sent[100], plain));
    CHECK(receiver.open(sent[1499], plain));
    // Forged: refused, and the real one after it still taken.
    std::vector<core::u8> forged = sent[1600];
    forged.back() ^= 1u;
    CHECK_FALSE(receiver.open(forged, plain));
    CHECK(receiver.open(sent[1600], plain));
}

TEST_CASE("what a packet and a handshake cost, measured and said (ADR 0224 section 5)")
{
    // **Said, not bounded**: a number for the record, read by whoever
    // measures on the machine the record names. A bound on a clock is a test
    // that fails when the machine is busy.
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto micros = [](auto from, auto to) { return std::chrono::duration<double, std::micro>(to - from).count(); };
    const core::X25519Pair client = core::x25519Generate();
    const core::X25519Pair server = core::x25519Generate();
    const auto prologue = bytes("match 54 com.example.game");

    constexpr int Handshakes = 200;
    const auto shakeBegan = now();
    for (int at = 0; at < Handshakes; ++at) {
        NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, prologue);
        NoiseHandshake responder(NoiseHandshake::Role::Responder, server, prologue);
        REQUIRE(shake(initiator, responder));
    }
    const double perHandshake = micros(shakeBegan, now()) / Handshakes;

    NoiseHandshake initiator(NoiseHandshake::Role::Initiator, client, prologue);
    NoiseHandshake responder(NoiseHandshake::Role::Responder, server, prologue);
    REQUIRE(shake(initiator, responder));
    net::NoiseSender sender(initiator.split().send);
    net::NoiseReceiver receiver(responder.split().receive);
    // A snapshot's size and an intent's: what a server sends most of, and
    // what it takes in most of.
    for (const core::usize size : {core::usize{600}, core::usize{60}}) {
        const std::vector<core::u8> payload(size, 0x5a);
        std::vector<core::u8> packet;
        std::vector<core::u8> plain;
        constexpr int Packets = 20000;
        double sealing = 0.0;
        double opening = 0.0;
        int refused = 0;
        for (int at = 0; at < Packets; ++at) {
            const auto began = now();
            sender.seal(payload, packet);
            const auto sealed = now();
            const bool opened = receiver.open(packet, plain);
            const auto ended = now();
            sealing += micros(began, sealed);
            opening += micros(sealed, ended);
            if (!opened)
                ++refused;
        }
        CHECK(refused == 0);
        MESSAGE("MEASURE packet bytes=" << size << " seal_us=" << sealing / Packets
                                        << " open_us=" << opening / Packets);
    }
    MESSAGE("MEASURE handshake both_sides_us=" << perHandshake);
}
