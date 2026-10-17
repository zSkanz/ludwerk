#include "engine/core/ticket.h"

#include <algorithm>
#include <cmath>

#include "engine/core/base64.h"
#include "engine/core/crypto.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"

namespace engine::core {
namespace {

// What a PKCS #8 file wraps an Ed25519 seed in (RFC 8410): the DER of the
// key's algorithm and an octet string, then the 32 bytes.
constexpr std::array<u8, 16> Pkcs8Prefix{0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
                                         0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20};
constexpr std::string_view PemBegin = "-----BEGIN PRIVATE KEY-----";
constexpr std::string_view PemEnd = "-----END PRIVATE KEY-----";
// Where the spent ids are let go of, so a flood of tickets cannot grow the
// list without end: a server takes far fewer joins in their lifetime.
constexpr std::size_t MostSpent = 1'000'000;

[[nodiscard]] std::string_view textOf(std::span<const u8> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

std::string base64UrlEncode(std::span<const u8> bytes)
{
    std::string text = base64Encode(bytes);
    for (char& c : text) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    while (!text.empty() && text.back() == '=')
        text.pop_back();
    return text;
}

std::optional<std::vector<u8>> base64UrlDecode(std::string_view text)
{
    // Its own alphabet only: a `+`, a `/` or padding is not base64url.
    std::string standard;
    standard.reserve(text.size() + 3);
    for (const char c : text) {
        if (c == '+' || c == '/' || c == '=')
            return std::nullopt;
        standard.push_back(c == '-' ? '+' : c == '_' ? '/' : c);
    }
    while (standard.size() % 4 != 0)
        standard.push_back('=');
    return base64Decode(standard);
}

bool TicketReplay::spend(const std::string& id, std::int64_t expiresAt, std::int64_t now)
{
    std::erase_if(m_spent, [now](const auto& entry) { return entry.second + TicketClockSlack < now; });
    if (m_spent.contains(id))
        return false;
    if (m_spent.size() >= MostSpent)
        m_spent.erase(std::min_element(m_spent.begin(), m_spent.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; }));
    m_spent.emplace(id, expiresAt);
    return true;
}

std::optional<TicketClaims> verifyTicket(std::string_view ticket, const std::array<u8, 32>& publicKey,
                                         std::string_view audience, std::int64_t now, TicketReplay* replay,
                                         std::string& why)
{
    const std::size_t first = ticket.find('.');
    const std::size_t second = first == std::string_view::npos ? first : ticket.find('.', first + 1);
    if (ticket.empty() || second == std::string_view::npos || ticket.find('.', second + 1) != std::string_view::npos) {
        why = "not a JWT: three parts joined by dots";
        return std::nullopt;
    }
    const std::optional<std::vector<u8>> header = base64UrlDecode(ticket.substr(0, first));
    const std::optional<std::vector<u8>> payload = base64UrlDecode(ticket.substr(first + 1, second - first - 1));
    const std::optional<std::vector<u8>> signature = base64UrlDecode(ticket.substr(second + 1));
    if (!header.has_value() || !payload.has_value() || !signature.has_value()) {
        why = "a part is not base64url";
        return std::nullopt;
    }
    // **The algorithm is not the ticket's to choose**: EdDSA, or refused
    // before the signature is looked at.
    JsonDocument headerDocument;
    if (!headerDocument.parse(textOf(*header), "ticket header")) {
        why = "the header is not JSON";
        return std::nullopt;
    }
    const JsonValue head = headerDocument.root();
    if (head.type() != JsonType::Object || head["alg"].type() != JsonType::String ||
        head["alg"].asString() != "EdDSA") {
        why = "the algorithm is not EdDSA";
        return std::nullopt;
    }
    if (head.has("typ") && head["typ"].asString() != "JWT") {
        why = "the type is not JWT";
        return std::nullopt;
    }
    const std::string_view signedPart = ticket.substr(0, second);
    if (!ed25519Verify(publicKey,
                       std::span<const u8>(reinterpret_cast<const u8*>(signedPart.data()), signedPart.size()),
                       *signature)) {
        why = "the signature does not check against [network] ticket_public_key";
        return std::nullopt;
    }
    JsonDocument payloadDocument;
    if (!payloadDocument.parse(textOf(*payload), "ticket payload") ||
        payloadDocument.root().type() != JsonType::Object) {
        why = "the payload is not a JSON object";
        return std::nullopt;
    }
    const JsonValue body = payloadDocument.root();
    TicketClaims claims;
    claims.subject = std::string(body["sub"].asString());
    claims.id = std::string(body["jti"].asString());
    // `aud` is a string, or a list one of which is this game.
    bool forUs = false;
    if (body["aud"].type() == JsonType::String) {
        claims.audience = std::string(body["aud"].asString());
        forUs = claims.audience == audience;
    }
    else if (body["aud"].type() == JsonType::Array) {
        for (usize at = 0; at < body["aud"].size(); ++at) {
            if (body["aud"].at(at).asString() == audience) {
                forUs = true;
                claims.audience = std::string(audience);
            }
        }
    }
    if (claims.subject.empty()) {
        why = "no sub: the ticket names no account";
        return std::nullopt;
    }
    if (!forUs || audience.empty()) {
        why = "aud is not this game's [project] id";
        return std::nullopt;
    }
    const auto whole = [](const JsonValue& value) -> std::optional<std::int64_t> {
        if (value.type() != JsonType::Number)
            return std::nullopt;
        const f64 number = value.asNumber();
        if (!std::isfinite(number) || number != std::floor(number))
            return std::nullopt;
        return static_cast<std::int64_t>(number);
    };
    const std::optional<std::int64_t> issued = whole(body["iat"]);
    const std::optional<std::int64_t> expires = whole(body["exp"]);
    if (!issued.has_value() || !expires.has_value()) {
        why = "iat and exp must be whole seconds";
        return std::nullopt;
    }
    claims.issuedAt = *issued;
    claims.expiresAt = *expires;
    for (usize at = 0; at < body.size(); ++at) {
        const std::string_view name = body.keyAt(at);
        const JsonValue value = body[name];
        if (value.type() == JsonType::String)
            claims.all.emplace_back(std::string(name), std::string(value.asString()));
        else if (value.type() == JsonType::Number)
            claims.all.emplace_back(std::string(name), value.asNumber());
        else if (value.type() == JsonType::Boolean)
            claims.all.emplace_back(std::string(name), value.asBool());
    }
    if (replay == nullptr)
        return claims;
    // **Short-lived, and spent once.**
    if (claims.expiresAt < claims.issuedAt || claims.expiresAt - claims.issuedAt > TicketMostSeconds) {
        why = "the ticket lives longer than 90 seconds";
        return std::nullopt;
    }
    if (claims.issuedAt > now + TicketClockSlack) {
        why = "iat is in the future";
        return std::nullopt;
    }
    if (claims.expiresAt + TicketClockSlack < now) {
        why = "the ticket has expired";
        return std::nullopt;
    }
    if (claims.id.empty()) {
        why = "no jti: a ticket must be good for one join";
        return std::nullopt;
    }
    if (!replay->spend(claims.id, claims.expiresAt, now)) {
        why = "this ticket was used before";
        return std::nullopt;
    }
    return claims;
}

std::string signTicketParts(const std::array<u8, 64>& secret, std::string_view header, std::string_view payload)
{
    const std::string signedPart =
        base64UrlEncode(std::span<const u8>(reinterpret_cast<const u8*>(header.data()), header.size())) + "." +
        base64UrlEncode(std::span<const u8>(reinterpret_cast<const u8*>(payload.data()), payload.size()));
    const std::array<u8, 64> signature =
        ed25519Sign(secret, std::span<const u8>(reinterpret_cast<const u8*>(signedPart.data()), signedPart.size()));
    return signedPart + "." + base64UrlEncode(signature);
}

std::string signTicket(const std::array<u8, 64>& secret, std::string_view subject, std::string_view audience,
                       std::int64_t issuedAt, std::int64_t seconds, std::string_view id)
{
    JsonWriter payload(JsonLayout::Compact);
    payload.beginObject();
    payload.field("sub", subject);
    payload.field("aud", audience);
    payload.field("iat", static_cast<i64>(issuedAt));
    payload.field("exp", static_cast<i64>(issuedAt + seconds));
    payload.field("jti", id);
    payload.endObject();
    return signTicketParts(secret, R"({"alg":"EdDSA","typ":"JWT"})", payload.text());
}

std::string ed25519PrivatePem(const std::array<u8, 32>& seed)
{
    std::array<u8, Pkcs8Prefix.size() + 32> der{};
    std::copy(Pkcs8Prefix.begin(), Pkcs8Prefix.end(), der.begin());
    std::copy(seed.begin(), seed.end(), der.begin() + Pkcs8Prefix.size());
    return std::string(PemBegin) + "\n" + base64Encode(der) + "\n" + std::string(PemEnd) + "\n";
}

std::optional<std::array<u8, 32>> ed25519SeedFromPem(std::string_view pem)
{
    const std::size_t begin = pem.find(PemBegin);
    const std::size_t end = pem.find(PemEnd);
    if (begin == std::string_view::npos || end == std::string_view::npos || end < begin)
        return std::nullopt;
    std::string body;
    for (const char c : pem.substr(begin + PemBegin.size(), end - begin - PemBegin.size())) {
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            body.push_back(c);
    }
    const std::optional<std::vector<u8>> der = base64Decode(body);
    if (!der.has_value() || der->size() != Pkcs8Prefix.size() + 32 ||
        !std::equal(Pkcs8Prefix.begin(), Pkcs8Prefix.end(), der->begin()))
        return std::nullopt;
    std::array<u8, 32> seed{};
    std::copy(der->begin() + Pkcs8Prefix.size(), der->end(), seed.begin());
    return seed;
}

} // namespace engine::core
