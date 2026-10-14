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

} // namespace engine::core
