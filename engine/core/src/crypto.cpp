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

void wipe(std::string& secret)
{
    if (!secret.empty())
        sodium_memzero(secret.data(), secret.size());
    secret.clear();
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

std::string passwordCost(std::string_view hash)
{
    // `$argon2id$v=19$m=65536,t=2,p=1$<salt>$<digest>`: what comes before
    // the salt, under the length `verifyPassword` takes.
    constexpr std::string_view Algorithm = "$argon2id$";
    if (hash.size() >= crypto_pwhash_STRBYTES || !hash.starts_with(Algorithm))
        return {};
    std::size_t at = Algorithm.size() - 1;
    for (int field = 0; field < 2; ++field) {
        at = hash.find('$', at + 1);
        if (at == std::string_view::npos)
            return {};
    }
    // A salt and a digest after it, or it is not a hash.
    const std::size_t salt = hash.find('$', at + 1);
    if (salt == std::string_view::npos || salt == at + 1 || salt + 1 == hash.size())
        return {};
    return std::string(hash.substr(1, at - 1));
}

X25519Pair x25519Generate()
{
    X25519Pair pair;
    secureRandom(pair.secret);
    pair.pub = x25519Public(pair.secret);
    return pair;
}

std::array<u8, 32> x25519Public(const std::array<u8, 32>& secret)
{
    std::array<u8, 32> pub{};
    crypto_scalarmult_base(pub.data(), secret.data());
    return pub;
}

bool x25519(const std::array<u8, 32>& secret, const std::array<u8, 32>& peerPublic, std::array<u8, 32>& shared)
{
    return crypto_scalarmult(shared.data(), secret.data(), peerPublic.data()) == 0;
}

namespace {

[[nodiscard]] std::array<u8, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> nonceOf(u64 counter)
{
    std::array<u8, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> nonce{};
    for (usize at = 0; at < 8; ++at)
        nonce[4 + at] = static_cast<u8>(counter >> (8 * at));
    return nonce;
}

} // namespace

void aeadSeal(const std::array<u8, 32>& key, u64 nonce, std::span<const u8> associated, std::span<const u8> plain,
              std::vector<u8>& out)
{
    out.resize(plain.size() + AeadTagBytes);
    unsigned long long written = 0;
    const auto n = nonceOf(nonce);
    crypto_aead_chacha20poly1305_ietf_encrypt(out.data(), &written, plain.data(), plain.size(), associated.data(),
                                              associated.size(), nullptr, n.data(), key.data());
    out.resize(static_cast<usize>(written));
}

bool aeadOpen(const std::array<u8, 32>& key, u64 nonce, std::span<const u8> associated, std::span<const u8> sealed,
              std::vector<u8>& out)
{
    if (sealed.size() < AeadTagBytes)
        return false;
    std::vector<u8> plain(sealed.size() - AeadTagBytes);
    unsigned long long written = 0;
    const auto n = nonceOf(nonce);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(plain.data(), &written, nullptr, sealed.data(), sealed.size(),
                                                  associated.data(), associated.size(), n.data(), key.data()) != 0)
        return false;
    plain.resize(static_cast<usize>(written));
    out = std::move(plain);
    return true;
}

Blake2b blake2b(std::span<const u8> data)
{
    Blake2b out{};
    crypto_generichash_blake2b(out.data(), out.size(), data.data(), data.size(), nullptr, 0);
    return out;
}

Blake2b hmacBlake2b(std::span<const u8> key, std::span<const u8> data)
{
    // RFC 2104 with BLAKE2b's 128-byte block: a longer key is hashed first.
    constexpr usize Block = 128;
    std::array<u8, Block> padded{};
    if (key.size() > Block) {
        const Blake2b hashed = blake2b(key);
        std::copy(hashed.begin(), hashed.end(), padded.begin());
    }
    else {
        std::copy(key.begin(), key.end(), padded.begin());
    }
    std::array<u8, Block> inner{};
    std::array<u8, Block> outer{};
    for (usize at = 0; at < Block; ++at) {
        inner[at] = static_cast<u8>(padded[at] ^ 0x36u);
        outer[at] = static_cast<u8>(padded[at] ^ 0x5cu);
    }
    crypto_generichash_blake2b_state state;
    Blake2b innerHash{};
    crypto_generichash_blake2b_init(&state, nullptr, 0, innerHash.size());
    crypto_generichash_blake2b_update(&state, inner.data(), inner.size());
    crypto_generichash_blake2b_update(&state, data.data(), data.size());
    crypto_generichash_blake2b_final(&state, innerHash.data(), innerHash.size());
    Blake2b out{};
    crypto_generichash_blake2b_init(&state, nullptr, 0, out.size());
    crypto_generichash_blake2b_update(&state, outer.data(), outer.size());
    crypto_generichash_blake2b_update(&state, innerHash.data(), innerHash.size());
    crypto_generichash_blake2b_final(&state, out.data(), out.size());
    sodium_memzero(padded.data(), padded.size());
    return out;
}

Ed25519Pair ed25519Generate()
{
    Ed25519Pair pair;
    crypto_sign_ed25519_keypair(pair.pub.data(), pair.secret.data());
    return pair;
}

Ed25519Pair ed25519FromSeed(const std::array<u8, 32>& seed)
{
    Ed25519Pair pair;
    crypto_sign_ed25519_seed_keypair(pair.pub.data(), pair.secret.data(), seed.data());
    return pair;
}

std::array<u8, 64> ed25519Sign(const std::array<u8, 64>& secret, std::span<const u8> message)
{
    std::array<u8, 64> signature{};
    crypto_sign_ed25519_detached(signature.data(), nullptr, message.data(), message.size(), secret.data());
    return signature;
}

bool ed25519Verify(const std::array<u8, 32>& pub, std::span<const u8> message, std::span<const u8> signature)
{
    return signature.size() == crypto_sign_ed25519_BYTES &&
           crypto_sign_ed25519_verify_detached(signature.data(), message.data(), message.size(), pub.data()) == 0;
}

} // namespace engine::core
