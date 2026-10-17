// **Every match encrypted, around whatever carries it** (ADR 0224).
//
// A transport that wraps another -- ENet, the relay's pipes beneath it, the
// in-memory network a test uses -- and does nothing above it differently: a
// peer is `Connected` once the Noise handshake with it is done (`noise.h`),
// every message after that is sealed, and a packet that fails to open is
// dropped. The relay carries what it cannot read.
//
// **Who checks whose key.** The side that dialled is told the other's key at
// the second message and asks `trust` whether it is the one it expected --
// pinned on a first contact, named by a redirect, set in the project -- before
// it answers; refused, the join fails as `KeyChanged`. The side that listens
// learns the joiner's key at the third, and that key IS the player
// (`remoteKey`).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "engine/core/crypto.h"
#include "engine/net/transport.h"

namespace engine::net {

struct SecureConfig
{
    // This machine's static key: a server's, or a player's.
    core::X25519Pair identity;
    // Bound into every handshake: two sides with different ones cannot finish
    // it (the protocol version and the game's id).
    std::vector<u8> prologue;
    // Asked by the side that dialled, with where it dialled and the key the
    // other side proved it holds; false refuses the connection. Absent, every
    // key is taken (a test, a listener that never dials).
    std::function<bool(std::string_view host, u16 port, const std::array<u8, 32>& key)> trust;
    // A handshake not done in this long is let go.
    u32 handshakeTimeoutMs = 10'000;
    // **What a joiner that does not encrypt is told**, in the clear, before
    // it is let go: the layers above's own refusal, so a game started without
    // encryption says why it could not join rather than that the connection
    // was lost. Empty: let go without a word.
    std::vector<u8> plainRefusal;
};

[[nodiscard]] std::unique_ptr<ITransport> createSecureTransport(std::unique_ptr<ITransport> inner, SecureConfig config);

} // namespace engine::net
