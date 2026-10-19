#include "engine/app/travel_pass.h"

#include <algorithm>
#include <span>
#include <vector>

#include "engine/core/crypto.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/ticket.h"

namespace engine::app {
namespace {

// The version of what is signed: a pass of another is refused.
constexpr std::int64_t PassVersion = 1;
// The clocks of the manager and its worlds are one machine's.
constexpr std::int64_t PassSlack = 5;

[[nodiscard]] std::span<const core::u8> bytesOf(std::string_view text)
{
    return {reinterpret_cast<const core::u8*>(text.data()), text.size()};
}

} // namespace

std::string signPass(const TravelPass& pass, const std::array<core::u8, 64>& managerSecret)
{
    core::JsonWriter body(core::JsonLayout::Compact);
    body.beginObject();
    body.field("v", static_cast<core::i64>(PassVersion));
    body.field("player", std::string_view{pass.player});
    body.field("origin", std::string_view{pass.origin});
    body.field("target", std::string_view{pass.target});
    body.field("data", std::string_view{pass.data});
    body.field("ticket", std::string_view{pass.ticket});
    body.field("account", std::string_view{pass.account});
    body.field("reason", std::string_view{pass.reason});
    body.field("issued", static_cast<core::i64>(pass.issued));
    body.field("expires", static_cast<core::i64>(pass.expires));
    body.field("nonce", std::string_view{pass.nonce});
    body.endObject();
    const std::string& json = body.text();
    const std::array<core::u8, 64> signature = core::ed25519Sign(managerSecret, bytesOf(json));
    return core::base64UrlEncode(bytesOf(json)) + "." + core::base64UrlEncode(signature);
}

bool PassReplay::spend(const std::string& nonce, std::int64_t expires, std::int64_t now)
{
    std::erase_if(m_spent, [now](const auto& entry) { return entry.second + PassSlack < now; });
    return m_spent.emplace(nonce, expires).second;
}

std::optional<TravelPass> openPass(std::string_view text, const std::array<core::u8, 32>& managerKey,
                                   std::string_view target, std::string_view player, std::int64_t now,
                                   PassReplay& replay, PassRefusal& why)
{
    const std::size_t dot = text.find('.');
    if (text.empty() || dot == std::string_view::npos) {
        why = PassRefusal::Missing;
        return std::nullopt;
    }
    const std::optional<std::vector<core::u8>> json = core::base64UrlDecode(text.substr(0, dot));
    const std::optional<std::vector<core::u8>> signature = core::base64UrlDecode(text.substr(dot + 1));
    if (!json.has_value() || !signature.has_value() || !core::ed25519Verify(managerKey, *json, *signature)) {
        why = PassRefusal::NotTheManagers;
        return std::nullopt;
    }
    core::JsonDocument document;
    if (!document.parse(std::string_view{reinterpret_cast<const char*>(json->data()), json->size()}, "pass")) {
        why = PassRefusal::NotTheManagers;
        return std::nullopt;
    }
    const core::JsonValue root = document.root();
    if (root["v"].asInteger(0) != PassVersion) {
        why = PassRefusal::NotTheManagers;
        return std::nullopt;
    }
    TravelPass pass;
    pass.player = std::string(root["player"].asString());
    pass.origin = std::string(root["origin"].asString());
    pass.target = std::string(root["target"].asString());
    pass.data = std::string(root["data"].asString());
    pass.ticket = std::string(root["ticket"].asString());
    pass.account = std::string(root["account"].asString());
    pass.reason = std::string(root["reason"].asString());
    pass.issued = root["issued"].asInteger(0);
    pass.expires = root["expires"].asInteger(0);
    pass.nonce = std::string(root["nonce"].asString());
    if (pass.target != target) {
        why = PassRefusal::OtherWorld;
        return std::nullopt;
    }
    if (pass.player.empty() || pass.player != player) {
        why = PassRefusal::OtherPlayer;
        return std::nullopt;
    }
    if (pass.expires + PassSlack < now || pass.issued > now + PassSlack) {
        why = PassRefusal::Expired;
        return std::nullopt;
    }
    if (pass.nonce.empty() || !replay.spend(pass.nonce, pass.expires, now)) {
        why = PassRefusal::Used;
        return std::nullopt;
    }
    return pass;
}

} // namespace engine::app
