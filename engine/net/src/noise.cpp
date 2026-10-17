#include "engine/net/noise.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace engine::net {
namespace {

constexpr std::string_view ProtocolName = "Noise_XX_25519_ChaChaPoly_BLAKE2b";
constexpr usize KeyBytes = 32;

// Noise's HKDF over HMAC-BLAKE2b, two outputs.
void hkdf2(const core::Blake2b& chainingKey, std::span<const u8> ikm, core::Blake2b& first, core::Blake2b& second)
{
    const core::Blake2b temp = core::hmacBlake2b(chainingKey, ikm);
    const u8 one = 0x01;
    first = core::hmacBlake2b(temp, std::span<const u8>(&one, 1));
    std::array<u8, 65> next{};
    std::copy(first.begin(), first.end(), next.begin());
    next[64] = 0x02;
    second = core::hmacBlake2b(temp, next);
}

[[nodiscard]] std::array<u8, 32> truncated(const core::Blake2b& hash)
{
    std::array<u8, 32> key{};
    std::copy_n(hash.begin(), key.size(), key.begin());
    return key;
}

} // namespace

NoiseHandshake::NoiseHandshake(Role role, const core::X25519Pair& local, std::span<const u8> prologue)
    : m_role(role), m_s(local)
{
    // The name fits HASHLEN, so `h` is the name padded with zeros.
    std::copy(ProtocolName.begin(), ProtocolName.end(), m_h.begin());
    m_ck = m_h;
    mixHash(prologue);
}

bool NoiseHandshake::myTurn() const noexcept
{
    if (m_failed || m_step >= 3)
        return false;
    // Messages 1 and 3 are the initiator's, 2 the responder's.
    return (m_step % 2 == 0) == (m_role == Role::Initiator);
}

void NoiseHandshake::mixHash(std::span<const u8> data)
{
    std::vector<u8> joined(m_h.begin(), m_h.end());
    joined.insert(joined.end(), data.begin(), data.end());
    m_h = core::blake2b(joined);
}

void NoiseHandshake::mixKey(std::span<const u8> ikm)
{
    core::Blake2b key{};
    hkdf2(m_ck, ikm, m_ck, key);
    m_k = truncated(key);
    m_hasKey = true;
    m_n = 0;
}

void NoiseHandshake::encryptAndHash(std::span<const u8> plain, std::vector<u8>& out)
{
    std::vector<u8> sealed;
    if (m_hasKey)
        core::aeadSeal(m_k, m_n++, m_h, plain, sealed);
    else
        sealed.assign(plain.begin(), plain.end());
    mixHash(sealed);
    out.insert(out.end(), sealed.begin(), sealed.end());
}

bool NoiseHandshake::decryptAndHash(std::span<const u8> sealed, std::vector<u8>& out)
{
    if (m_hasKey) {
        if (!core::aeadOpen(m_k, m_n, m_h, sealed, out))
            return false;
        ++m_n;
    }
    else {
        out.assign(sealed.begin(), sealed.end());
    }
    mixHash(sealed);
    return true;
}

bool NoiseHandshake::dh(const std::array<u8, 32>& secret, const std::array<u8, 32>& pub)
{
    std::array<u8, 32> shared{};
    if (!core::x25519(secret, pub, shared))
        return false;
    mixKey(shared);
    return true;
}

std::optional<std::vector<u8>> NoiseHandshake::write()
{
    if (!myTurn())
        return std::nullopt;
    std::vector<u8> out;
    const std::span<const u8> empty;
    if (m_step == 0) {
        // -> e
        m_e = core::x25519Generate();
        out.insert(out.end(), m_e.pub.begin(), m_e.pub.end());
        mixHash(m_e.pub);
        encryptAndHash(empty, out);
    }
    else if (m_step == 1) {
        // <- e, ee, s, es
        m_e = core::x25519Generate();
        out.insert(out.end(), m_e.pub.begin(), m_e.pub.end());
        mixHash(m_e.pub);
        if (!dh(m_e.secret, m_re)) {
            m_failed = true;
            return std::nullopt;
        }
        encryptAndHash(m_s.pub, out);
        if (!dh(m_s.secret, m_re)) {
            m_failed = true;
            return std::nullopt;
        }
        encryptAndHash(empty, out);
    }
    else {
        // -> s, se
        encryptAndHash(m_s.pub, out);
        if (!dh(m_s.secret, m_re)) {
            m_failed = true;
            return std::nullopt;
        }
        encryptAndHash(empty, out);
    }
    ++m_step;
    return out;
}

bool NoiseHandshake::read(std::span<const u8> message)
{
    if (m_failed || m_step >= 3 || myTurn()) {
        m_failed = true;
        return false;
    }
    const auto fail = [this] {
        m_failed = true;
        return false;
    };
    std::vector<u8> plain;
    if (m_step == 0) {
        // -> e, read by the responder
        if (message.size() != KeyBytes)
            return fail();
        std::copy_n(message.begin(), KeyBytes, m_re.begin());
        mixHash(m_re);
        if (!decryptAndHash(message.subspan(KeyBytes), plain))
            return fail();
    }
    else if (m_step == 1) {
        // <- e, ee, s, es, read by the initiator
        if (message.size() != KeyBytes + KeyBytes + core::AeadTagBytes + core::AeadTagBytes)
            return fail();
        std::copy_n(message.begin(), KeyBytes, m_re.begin());
        mixHash(m_re);
        if (!dh(m_e.secret, m_re))
            return fail();
        if (!decryptAndHash(message.subspan(KeyBytes, KeyBytes + core::AeadTagBytes), plain) ||
            plain.size() != KeyBytes)
            return fail();
        std::copy_n(plain.begin(), KeyBytes, m_rs.begin());
        if (!dh(m_e.secret, m_rs))
            return fail();
        if (!decryptAndHash(message.subspan(KeyBytes + KeyBytes + core::AeadTagBytes), plain))
            return fail();
    }
    else {
        // -> s, se, read by the responder
        if (message.size() != KeyBytes + core::AeadTagBytes + core::AeadTagBytes)
            return fail();
        if (!decryptAndHash(message.first(KeyBytes + core::AeadTagBytes), plain) || plain.size() != KeyBytes)
            return fail();
        std::copy_n(plain.begin(), KeyBytes, m_rs.begin());
        if (!dh(m_e.secret, m_rs))
            return fail();
        if (!decryptAndHash(message.subspan(KeyBytes + core::AeadTagBytes), plain))
            return fail();
    }
    ++m_step;
    return true;
}

NoiseHandshake::Keys NoiseHandshake::split() const
{
    core::Blake2b first{};
    core::Blake2b second{};
    hkdf2(m_ck, {}, first, second);
    Keys keys;
    // The first is the initiator's sending key.
    if (m_role == Role::Initiator) {
        keys.send = truncated(first);
        keys.receive = truncated(second);
    }
    else {
        keys.send = truncated(second);
        keys.receive = truncated(first);
    }
    return keys;
}

void NoiseSender::seal(std::span<const u8> plain, std::vector<u8>& out)
{
    const u64 counter = m_next++;
    std::vector<u8> sealed;
    core::aeadSeal(m_key, counter, {}, plain, sealed);
    out.resize(8 + sealed.size());
    for (usize at = 0; at < 8; ++at)
        out[at] = static_cast<u8>(counter >> (8 * at));
    std::copy(sealed.begin(), sealed.end(), out.begin() + 8);
}

bool NoiseReceiver::open(std::span<const u8> packet, std::vector<u8>& out)
{
    if (packet.size() < NoiseOverheadBytes)
        return false;
    u64 counter = 0;
    for (usize at = 0; at < 8; ++at)
        counter |= static_cast<u64>(packet[at]) << (8 * at);
    // Placed before it is opened, refused before any work for one that
    // cannot be new.
    if (m_any && counter <= m_highest) {
        const u64 behind = m_highest - counter;
        if (behind >= Window || m_seen.test(static_cast<usize>(behind)))
            return false;
    }
    if (!core::aeadOpen(m_key, counter, {}, packet.subspan(8), out))
        return false;
    if (!m_any || counter > m_highest) {
        const u64 ahead = m_any ? counter - m_highest : 0;
        m_seen = ahead >= Window ? std::bitset<Window>{} : (m_seen << static_cast<usize>(ahead));
        m_highest = counter;
        m_any = true;
        m_seen.set(0);
    }
    else {
        m_seen.set(static_cast<usize>(m_highest - counter));
    }
    return true;
}

} // namespace engine::net
