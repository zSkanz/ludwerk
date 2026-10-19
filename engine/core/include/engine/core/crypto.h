// What a game needs to keep a secret and to tell one player from another
// (ADR 0151): chance that is not the simulation's, two hashes, and one hash
// meant for passwords.
//
// **Nothing here is written here.** Every function is libsodium's -- a
// primitive nobody in this repository wrote is one nobody in it has to defend
// -- and this header is the small face of it the rest of the engine sees, so
// no other module includes a vendored header for it.
//
// **Not the simulation's** (R10). `Pcg32` is the simulation's chance: seeded
// by the world, the same on every run, which is exactly what a session token
// must not be. Nothing a tick computes may come from here.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::core {

// Bytes from the operating system's generator. Never fails: a machine whose
// generator cannot be opened is one the process does not start on.
void secureRandom(std::span<u8> out);

// A version-4 UUID -- 122 random bits -- in its usual spelling, lower case:
// `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`.
[[nodiscard]] std::string uniqueId();

using Sha256 = std::array<u8, 32>;

[[nodiscard]] Sha256 sha256(std::span<const u8> data);

// HMAC-SHA-256 (RFC 2104), a key of any length.
[[nodiscard]] Sha256 hmacSha256(std::span<const u8> key, std::span<const u8> data);

// Whether two byte strings are the same, in a time that depends on their
// LENGTH and not on where they first differ -- what a token or a signature is
// compared with, so that how long the comparison took says nothing about how
// much of a guess was right.
[[nodiscard]] bool secureEquals(std::span<const u8> a, std::span<const u8> b);

// Lower-case hexadecimal, two characters a byte.
[[nodiscard]] std::string toHex(std::span<const u8> bytes);

// **A hash meant for a password**: Argon2id, salted with sixteen bytes of its
// own choosing, slow and memory-hungry on purpose (about 64 MiB, a fraction of
// a second) so that a stolen table cannot be guessed at a billion tries a
// second, which is what a plain SHA-256 of a password allows.
//
// What comes back is a line of text that carries the algorithm, its cost and
// the salt beside the hash -- `$argon2id$v=19$m=65536,t=2,p=1$...` -- so the
// cost can be raised one day without a stored hash becoming unreadable. Empty
// when the memory could not be had.
//
// BLOCKS for as long as that takes. A caller with a frame to keep runs it on
// another thread.
[[nodiscard]] std::string hashPassword(std::string_view password);

// Whether `password` is the one `hash` was made from. False for a hash that is
// not one of `hashPassword`'s. Costs what the hash was made at.
[[nodiscard]] bool verifyPassword(std::string_view hash, std::string_view password);

// **A secret overwritten before it is let go**: a password once its hash is
// made or checked, where an ordinary clear would leave it in the memory the
// string gives back. Empty afterwards.
void wipe(std::string& secret);

// --- What a match's encryption and a signed pass are made of (ADR 0224) ----
//
// Each of these is libsodium's, under the names the Noise specification and
// RFC 8032 use, so the code that builds on them reads as those documents do.

// **X25519**: a key pair, and the shared secret two halves of two pairs make.
struct X25519Pair
{
    std::array<u8, 32> secret{};
    std::array<u8, 32> pub{};
};
[[nodiscard]] X25519Pair x25519Generate();
// The public half of a secret.
[[nodiscard]] std::array<u8, 32> x25519Public(const std::array<u8, 32>& secret);
// False for a public key that makes an all-zero secret (a low-order point):
// a peer that sends one is refused.
[[nodiscard]] bool x25519(const std::array<u8, 32>& secret, const std::array<u8, 32>& peerPublic,
                          std::array<u8, 32>& shared);

// **ChaCha20-Poly1305 (IETF)**: sealed with a 32-byte key, a 64-bit nonce
// (in the 96-bit nonce's last eight bytes, little-endian, as Noise has it)
// and data that is authenticated but not hidden. A seal is 16 bytes longer.
inline constexpr usize AeadTagBytes = 16;
void aeadSeal(const std::array<u8, 32>& key, u64 nonce, std::span<const u8> associated, std::span<const u8> plain,
              std::vector<u8>& out);
// False, with `out` untouched, for anything forged, damaged or sealed with
// another key, nonce or associated data.
[[nodiscard]] bool aeadOpen(const std::array<u8, 32>& key, u64 nonce, std::span<const u8> associated,
                            std::span<const u8> sealed, std::vector<u8>& out);

// **BLAKE2b-512**, and HMAC over it (RFC 2104, the 128-byte block BLAKE2b
// has): what Noise's HKDF is built from.
using Blake2b = std::array<u8, 64>;
[[nodiscard]] Blake2b blake2b(std::span<const u8> data);
[[nodiscard]] Blake2b hmacBlake2b(std::span<const u8> key, std::span<const u8> data);

// **Ed25519** (RFC 8032): a signing pair, a signature and its check.
struct Ed25519Pair
{
    std::array<u8, 64> secret{};
    std::array<u8, 32> pub{};
};
[[nodiscard]] Ed25519Pair ed25519Generate();
// The pair a 32-byte seed makes: what a PKCS #8 private key file holds.
[[nodiscard]] Ed25519Pair ed25519FromSeed(const std::array<u8, 32>& seed);
[[nodiscard]] std::array<u8, 64> ed25519Sign(const std::array<u8, 64>& secret, std::span<const u8> message);
[[nodiscard]] bool ed25519Verify(const std::array<u8, 32>& pub, std::span<const u8> message,
                                 std::span<const u8> signature);

} // namespace engine::core
