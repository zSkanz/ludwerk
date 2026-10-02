#include "engine/core/crypto.h"

#include <cstdlib>
#include <sodium.h>

namespace engine::core {

namespace {

// libsodium picks its implementations once, and says so before anything else
// is called. Idempotent and safe from any thread; the only failure it has is a
// machine with no random source, and there is nothing to do there but stop.
void ready()
{
    static const int state = sodium_init();
    if (state < 0)
        std::abort();
}

} // namespace

void secureRandom(std::span<u8> out)
{
    ready();
    if (!out.empty())
        randombytes_buf(out.data(), out.size());
}

std::string uniqueId()
{
    std::array<u8, 16> bytes{};
    secureRandom(bytes);
    // RFC 4122 section 4.4: the version in the top four bits of byte six, the
    // variant in the top two of byte eight.
    bytes[6] = static_cast<u8>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<u8>((bytes[8] & 0x3F) | 0x80);
    const std::string hex = toHex(bytes);
    std::string out;
    out.reserve(36);
    out.append(hex, 0, 8).push_back('-');
    out.append(hex, 8, 4).push_back('-');
    out.append(hex, 12, 4).push_back('-');
    out.append(hex, 16, 4).push_back('-');
    out.append(hex, 20, 12);
    return out;
}

Sha256 sha256(std::span<const u8> data)
{
    ready();
    Sha256 out{};
    (void)crypto_hash_sha256(out.data(), data.data(), data.size());
    return out;
}

Sha256 hmacSha256(std::span<const u8> key, std::span<const u8> data)
{
    ready();
    Sha256 out{};
    crypto_auth_hmacsha256_state state;
    (void)crypto_auth_hmacsha256_init(&state, key.data(), key.size());
    (void)crypto_auth_hmacsha256_update(&state, data.data(), data.size());
    (void)crypto_auth_hmacsha256_final(&state, out.data());
    return out;
}

bool secureEquals(std::span<const u8> a, std::span<const u8> b)
{
    ready();
    if (a.size() != b.size())
        return false;
    return a.empty() || sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string toHex(std::span<const u8> bytes)
{
    static constexpr char Digits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (usize i = 0; i < bytes.size(); ++i) {
        out[i * 2] = Digits[bytes[i] >> 4];
        out[i * 2 + 1] = Digits[bytes[i] & 0x0F];
    }
    return out;
}

std::string hashPassword(std::string_view password)
{
    ready();
    // "Interactive" is the library's name for a login somebody is waiting on:
    // two passes over 64 MiB.
    char out[crypto_pwhash_STRBYTES] = {};
    if (crypto_pwhash_str(out, password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
        return {};
    return std::string{out};
}

bool verifyPassword(std::string_view hash, std::string_view password)
{
    ready();
    // The library reads a terminated string of at most this many bytes; one
    // that is longer is not one of ours.
    if (hash.empty() || hash.size() >= crypto_pwhash_STRBYTES)
        return false;
    char text[crypto_pwhash_STRBYTES] = {};
    hash.copy(text, hash.size());
    return crypto_pwhash_str_verify(text, password.data(), password.size()) == 0;
}

} // namespace engine::core
