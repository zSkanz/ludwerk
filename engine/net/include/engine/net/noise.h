// **A match's handshake and its sealed packets** (ADR 0224): Noise
// `XX_25519_ChaChaPoly_BLAKE2b`, from the Noise Protocol Framework
// (revision 34), built on libsodium's primitives (`core/crypto.h`).
//
// **XX, not IK.** IK needs the side that dials to know the other's key before
// it says a word, and a first contact by address or code -- a game on a LAN,
// a friend's host -- has no key to start from. XX trades both keys in three
// messages; the dialling side then checks the one it was given against what
// it pinned or was told (ADR 0224 §3). One handshake for every case: a match,
// a redirect, an administrator.
//
// **The packets after it** carry their own counter, because a match's
// packets may be lost or come in another order: each is sealed under that
// counter, and a receiver keeps a window of the last 1,024 it has seen, so a
// packet played back, or one too old to place, is refused.
#pragma once

#include <array>
#include <bitset>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/crypto.h"
#include "engine/core/types.h"

namespace engine::net {

using core::u64;
using core::u8;
using core::usize;

class NoiseHandshake
{
public:
    enum class Role : u8
    {
        // The side that dialled.
        Initiator,
        Responder,
    };

    // `prologue` is bound into the handshake: two sides with different ones
    // cannot finish it -- another protocol version, another game.
    NoiseHandshake(Role role, const core::X25519Pair& local, std::span<const u8> prologue);

    // Whether the next message is this side's to write.
    [[nodiscard]] bool myTurn() const noexcept;
    // This side's next message; nothing when it is not this side's turn or
    // the handshake is over.
    [[nodiscard]] std::optional<std::vector<u8>> write();
    // The other side's message. False -- and the handshake is dead -- for one
    // that is malformed, forged, out of turn, or carries a key that makes no
    // secret.
    [[nodiscard]] bool read(std::span<const u8> message);

    [[nodiscard]] bool done() const noexcept { return m_step == 3 && !m_failed; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    // The other side's static key, once it has been read (after the second
    // message for the initiator, the third for the responder).
    [[nodiscard]] const std::array<u8, 32>& remoteStatic() const noexcept { return m_rs; }

    struct Keys
    {
        std::array<u8, 32> send{};
        std::array<u8, 32> receive{};
    };
    // This side's two keys, once `done()`.
    [[nodiscard]] Keys split() const;

private:
    void mixHash(std::span<const u8> data);
    void mixKey(std::span<const u8> ikm);
    void encryptAndHash(std::span<const u8> plain, std::vector<u8>& out);
    [[nodiscard]] bool decryptAndHash(std::span<const u8> sealed, std::vector<u8>& out);
    [[nodiscard]] bool dh(const std::array<u8, 32>& secret, const std::array<u8, 32>& pub);

    Role m_role;
    core::X25519Pair m_s;
    core::X25519Pair m_e;
    std::array<u8, 32> m_re{};
    std::array<u8, 32> m_rs{};
    core::Blake2b m_h{};
    core::Blake2b m_ck{};
    std::array<u8, 32> m_k{};
    bool m_hasKey = false;
    u64 m_n = 0;
    // Messages done: 0 to 3.
    int m_step = 0;
    bool m_failed = false;
};

// One direction of a match's packets, sending.
class NoiseSender
{
public:
    explicit NoiseSender(const std::array<u8, 32>& key) noexcept : m_key(key) {}
    // `[counter, 8 bytes little-endian][sealed]`: 24 bytes more than `plain`.
    void seal(std::span<const u8> plain, std::vector<u8>& out);

private:
    std::array<u8, 32> m_key;
    u64 m_next = 0;
};

// One direction of a match's packets, receiving.
class NoiseReceiver
{
public:
    static constexpr usize Window = 1024;
    explicit NoiseReceiver(const std::array<u8, 32>& key) noexcept : m_key(key) {}
    // False for a packet forged, damaged, played back, or older than the
    // window; `out` is untouched then.
    [[nodiscard]] bool open(std::span<const u8> packet, std::vector<u8>& out);

private:
    std::array<u8, 32> m_key;
    bool m_any = false;
    u64 m_highest = 0;
    // Bit `i` is counter `m_highest - i`, seen.
    std::bitset<Window> m_seen;
};

// What a sealed packet costs over its plain one.
inline constexpr usize NoiseOverheadBytes = 8 + core::AeadTagBytes;

} // namespace engine::net
