// The player's own graphics choices, in their folder (ADR 0147 section 4).
//
// `settings.json`: a setting's name to its value, holding only what the player
// changed -- so a game that later ships a better default gives it to everybody
// who never touched that setting.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/app/project_config.h"
#include "engine/core/json.h"
#include "engine/platform/file.h"
#include "engine/scene/localization.h"

namespace engine::app {
namespace {

using core::f64;
using core::usize;

// A number as the file holds it: whole where it is whole, and otherwise with
// the digits a float has.
[[nodiscard]] std::string numberText(f64 value)
{
    if (value == std::floor(value) && std::fabs(value) < 1.0e15)
        return std::to_string(static_cast<long long>(value));
    char text[32];
    (void)std::snprintf(text, sizeof(text), "%.9g", value);
    return text;
}

} // namespace

bool writePlayerGraphics(const std::filesystem::path& file, const scene::GraphicsLayer& choices,
                         std::string_view locale, const PlayerHearing* hearing)
{
    std::string text = "{\n  \"version\": 1,\n";
    // A locale is letters, digits and dashes (`canonicalLocale`): nothing in it
    // needs escaping.
    if (!locale.empty()) {
        text += "  \"locale\": \"";
        text += scene::canonicalLocale(locale);
        text += "\",\n";
    }
    if (hearing != nullptr) {
        if (!hearing->voiceLocale.empty()) {
            text += "  \"voice_locale\": \"";
            text += scene::canonicalLocale(hearing->voiceLocale);
            text += "\",\n";
        }
        // Only what is not as it comes: a file is what the player changed.
        const scene::PlayerSound& sound = hearing->sound;
        const auto group = [&text](std::string_view name,
                                   std::initializer_list<std::pair<std::string_view, std::string>> entries) {
            bool any = false;
            for (const auto& [key, value] : entries) {
                if (value.empty())
                    continue;
                text += any ? ", " : "  \"" + std::string(name) + "\": { ";
                any = true;
                text += "\"" + std::string(key) + "\": " + value;
            }
            if (any)
                text += " },\n";
        };
        const auto level = [](f32 value) {
            return value == 1.0f ? std::string{} : numberText(static_cast<f64>(value));
        };
        group("audio", {{"player", level(sound.playerVolume)},
                        {"music", level(sound.musicVolume)},
                        {"effects", level(sound.effectsVolume)},
                        {"voice", level(sound.voiceVolume)}});
        constexpr std::string_view Modes[3] = {"\"auto\"", "\"on\"", "\"off\""};
        group("subtitles",
              {{"mode", sound.subtitlesSaid && sound.subtitles >= 0 && sound.subtitles <= 2
                            ? std::string(Modes[sound.subtitles])
                            : std::string{}},
               {"scale", sound.subtitleScaleSaid ? numberText(static_cast<f64>(sound.subtitleScale)) : std::string{}},
               {"background", sound.subtitleBackgroundSaid ? numberText(static_cast<f64>(sound.subtitleBackground))
                                                           : std::string{}}});
    }
    text += "  \"settings\": {";
    bool first = true;
    for (usize index = 0; index < scene::kGraphicsSettingCount; ++index) {
        const auto setting = static_cast<scene::GraphicsSetting>(index);
        if (!choices.says(setting))
            continue;
        const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(setting);
        text += first ? "\n" : ",\n";
        first = false;
        text += "    \"";
        text += info.name;
        text += "\": ";
        if (info.kind == scene::GraphicsValueKind::Flag)
            text += choices.at(setting) != 0.0 ? "true" : "false";
        else
            text += numberText(choices.at(setting));
    }
    text += first ? "}\n}\n" : "\n  }\n}\n";

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Durable: a game closed by a power cut must not come back with half a
    // file where the player's window mode was.
    return platform::writeFileDurable(file, std::as_bytes(std::span{text.data(), text.size()}));
}

bool readPlayerGraphics(const std::filesystem::path& file, scene::GraphicsLayer& choices,
                        std::vector<std::string>* refused, std::string* locale, PlayerHearing* hearing)
{
    choices = scene::GraphicsLayer{};
    if (locale != nullptr)
        locale->clear();
    if (hearing != nullptr)
        *hearing = PlayerHearing{};
    std::vector<std::byte> bytes;
    if (file.empty() || !platform::readFile(file, bytes))
        return false;
    core::JsonDocument document;
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (!document.parse(text, file.string()))
        return false;
    if (locale != nullptr) {
        if (const core::JsonValue chosen = document.root()["locale"]; chosen.type() == core::JsonType::String)
            *locale = scene::canonicalLocale(chosen.asString());
    }
    if (hearing != nullptr) {
        const core::JsonValue root = document.root();
        if (const core::JsonValue voice = root["voice_locale"]; voice.type() == core::JsonType::String)
            hearing->voiceLocale = scene::canonicalLocale(voice.asString());
        // A number out of its range is brought into it, as a script's write
        // is refused: a file cannot say what a script could not.
        const auto level = [](const core::JsonValue& value, f32 low, f32 high, f32& out) {
            if (value.type() != core::JsonType::Number || !std::isfinite(value.asNumber()))
                return false;
            out = std::clamp(static_cast<f32>(value.asNumber()), low, high);
            return true;
        };
        scene::PlayerSound& sound = hearing->sound;
        const core::JsonValue audio = root["audio"];
        (void)level(audio["player"], 0.0f, 1.0f, sound.playerVolume);
        (void)level(audio["music"], 0.0f, 1.0f, sound.musicVolume);
        (void)level(audio["effects"], 0.0f, 1.0f, sound.effectsVolume);
        (void)level(audio["voice"], 0.0f, 1.0f, sound.voiceVolume);
        const core::JsonValue subtitles = root["subtitles"];
        const std::string_view mode = subtitles["mode"].asString();
        if (mode == "auto" || mode == "on" || mode == "off") {
            sound.subtitles = mode == "on" ? 1 : (mode == "off" ? 2 : 0);
            sound.subtitlesSaid = true;
        }
        sound.subtitleScaleSaid = level(subtitles["scale"], 0.75f, 2.0f, sound.subtitleScale);
        sound.subtitleBackgroundSaid = level(subtitles["background"], 0.0f, 1.0f, sound.subtitleBackground);
    }
    const core::JsonValue settings = document.root()["settings"];
    if (settings.type() != core::JsonType::Object)
        return false;

    const auto refuse = [refused](std::string_view name) {
        if (refused != nullptr)
            refused->emplace_back(name);
    };
    for (usize index = 0; index < settings.size(); ++index) {
        const std::string_view name = settings.keyAt(index);
        const core::JsonValue value = settings[name];
        const std::optional<scene::GraphicsSetting> setting = scene::graphicsSettingNamed(name);
        if (!setting.has_value()) {
            // A setting of a newer build, or a hand-edited name: left out.
            refuse(name);
            continue;
        }
        const scene::GraphicsSettingInfo& info = scene::graphicsSettingInfo(*setting);
        f64 number = 0.0;
        if (info.kind == scene::GraphicsValueKind::Flag && value.type() == core::JsonType::Boolean) {
            number = value.asBool() ? 1.0 : 0.0;
        }
        else if (info.kind != scene::GraphicsValueKind::Flag && value.type() == core::JsonType::Number) {
            number = value.asNumber();
        }
        else {
            refuse(name);
            continue;
        }
        // Checked as a script's write is, into a scratch model: the same kinds
        // and the same ranges, so a file cannot say what a script could not.
        const bool whole =
            info.kind == scene::GraphicsValueKind::Whole || info.kind == scene::GraphicsValueKind::Choice;
        if (!std::isfinite(number) || (whole && number != std::floor(number)) ||
            (info.kind == scene::GraphicsValueKind::Choice && (number < info.lowest || number > info.highest)) ||
            (*setting == scene::GraphicsSetting::QualityLevel &&
             static_cast<core::i32>(number) == scene::kQualityCustom)) {
            refuse(name);
            continue;
        }
        choices.put(*setting, std::fmin(std::fmax(number, info.lowest), info.highest));
    }
    return true;
}

} // namespace engine::app
