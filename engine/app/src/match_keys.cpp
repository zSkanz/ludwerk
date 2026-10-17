#include "engine/app/match_keys.h"

#include <cctype>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"

namespace engine::app {
namespace {

[[nodiscard]] std::optional<std::array<core::u8, 32>> keyFromHex(std::string_view hex)
{
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.back())))
        hex.remove_suffix(1);
    if (hex.size() != 64)
        return std::nullopt;
    std::array<core::u8, 32> out{};
    for (std::size_t at = 0; at < 32; ++at) {
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const int high = digit(hex[at * 2]);
        const int low = digit(hex[at * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out[at] = static_cast<core::u8>(high * 16 + low);
    }
    return out;
}

} // namespace

std::optional<std::array<core::u8, 32>> matchKeyFromHex(std::string_view hex)
{
    return keyFromHex(hex);
}

core::X25519Pair loadOrMakeMatchKey(const std::filesystem::path& file, platform::FileLock& held)
{
    if (!file.empty() && platform::createDirectories(file.parent_path())) {
        std::filesystem::path lock = file;
        lock += ".lock";
        if (held.tryLock(lock) == platform::FileLock::Taken::Busy) {
            core::log(core::LogLevel::Info, ENG_TR("net.info.match_key_in_use"));
            return core::x25519Generate();
        }
    }
    if (!file.empty()) {
        if (std::string text; platform::readTextFile(file, text)) {
            if (const std::optional<std::array<core::u8, 32>> secret = keyFromHex(text); secret.has_value())
                return core::X25519Pair{*secret, core::x25519Public(*secret)};
        }
    }
    const core::X25519Pair made = core::x25519Generate();
    if (file.empty())
        return made;
    if (!platform::writeTextFile(file, core::toHex(made.secret)) || !platform::restrictToOwner(file)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", file.string()}};
        core::log(core::LogLevel::Warn, ENG_TR("net.warn.match_key_not_kept"), args);
    }
    return made;
}

KeyPins::KeyPins(std::filesystem::path file) : m_file(std::move(file))
{
    std::string text;
    if (m_file.empty() || !platform::readTextFile(m_file, text))
        return;
    core::JsonDocument document;
    if (!document.parse(text, m_file.string()))
        return;
    const core::JsonValue root = document.root();
    for (core::usize index = 0; index < root.size(); ++index) {
        const std::string_view where = root.keyAt(index);
        m_pins.emplace(std::string(where), std::string(root[where].asString()));
    }
}

bool KeyPins::trust(std::string_view where, const std::array<core::u8, 32>& key)
{
    const std::string hex = core::toHex(key);
    const auto found = m_pins.find(where);
    if (found == m_pins.end()) {
        // Met for the first time: pinned without a word.
        m_pins.emplace(std::string(where), hex);
        save();
        return true;
    }
    return found->second == hex;
}

void KeyPins::forget(std::string_view where)
{
    if (const auto found = m_pins.find(where); found != m_pins.end()) {
        m_pins.erase(found);
        save();
    }
}

void KeyPins::save() const
{
    if (m_file.empty())
        return;
    core::JsonWriter writer(core::JsonLayout::Indented);
    writer.beginObject();
    for (const auto& [where, hex] : m_pins)
        writer.field(where, std::string_view(hex));
    writer.endObject();
    if (platform::createDirectories(m_file.parent_path()))
        (void)platform::writeTextFile(m_file, writer.text());
}

} // namespace engine::app
