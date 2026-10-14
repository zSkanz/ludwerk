// A game's text by key, in the player's language (ADR 0154).
//
// A catalog a locale: the project's own, from `i18n/<locale>.json`, and under
// them the engine's -- so a module the engine ships asks the same question a
// game's script does. `LocalizationService` is this, asked by a script; the
// host fills it and gives the world a pointer to it.
//
// **Nothing here is the simulation's.** Which language a player reads is a
// fact about a person, not about the world: it is not hashed, not saved with a
// scene and not sent.
#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::scene {

class Localization
{
public:
    // One of the project's catalogs, as its file's text: a flat object, key to
    // text. Replaces the locale's catalog when it has one. False, with why,
    // for a file that is not a catalog -- and the one it had stays.
    bool load(std::string_view locale, std::string_view json, std::string* diagnostic = nullptr);
    // The same for the engine's own text in a locale other than its first.
    bool loadEngine(std::string_view locale, std::string_view json, std::string* diagnostic = nullptr);
    void clear();

    // The locale a key falls back to when the player's has no word for it:
    // the project's `[project] default_locale`, "en" until it says.
    void setDefaultLocale(std::string_view locale);
    [[nodiscard]] const std::string& defaultLocale() const noexcept { return defaultLocale_; }

    // The locales the project has a catalog for, in order; the default alone
    // when it has none.
    [[nodiscard]] std::vector<std::string> locales() const;

    // **The catalog a wanted locale is read from**: itself where there is one,
    // else one of the same language (`pt` for `pt-PT`, `pt-BR` for `pt`), else
    // nothing. Case and `_` for `-` do not matter.
    [[nodiscard]] std::string narrow(std::string_view wanted) const;

    // The text for `key` in `locale`: the project's catalog for it, then the
    // project's default locale, then the engine's catalog in that locale and
    // in its own. Nothing when nobody has the key.
    [[nodiscard]] std::optional<std::string> translate(std::string_view locale, std::string_view key,
                                                       std::span<const core::I18nArg> arguments = {}) const;

    // Rises whenever a catalog is loaded or cleared: what a host watches to
    // say the words changed under a locale that did not.
    [[nodiscard]] core::u64 revision() const noexcept { return revision_; }

private:
    std::map<std::string, core::Catalog, std::less<>> project_;
    std::map<std::string, core::Catalog, std::less<>> engine_;
    std::string defaultLocale_ = "en";
    core::u64 revision_ = 0;
};

// A locale's name as catalogs are kept: `pt-BR` for `PT_br`. Language in lower
// case, region in upper, a script with its first letter capital.
[[nodiscard]] std::string canonicalLocale(std::string_view locale);

// --- The voice language (ADR 0200) ---------------------------------------------
//
// A game is heard in a language that need not be the one it is read in. What a
// machine can PLAY is a list the host makes -- the project's default first,
// then each language it has sounds for -- and these two pick from it.

// **One of `playable` for a wanted locale**: itself, else the language alone,
// else any of that language, in the list's order; nothing when the list has
// none of that language. Case and `_` for `-` do not matter.
[[nodiscard]] std::string narrowVoice(std::string_view wanted, std::span<const std::string> playable);

// **The voice language in force.** What the player chose when this machine can
// play it or one of its language; else the voice FOLLOWS the text: the text
// locale narrowed, else the first of the system's languages this machine has
// voices for, else the first of `playable` (the default). Never empty unless
// `playable` is.
[[nodiscard]] std::string voiceLocaleFor(std::string_view chosen, std::string_view textLocale,
                                         std::span<const std::string> playable, std::span<const std::string> system);

// The languages a machine can play, as `GetVoiceLocales` gives them: the
// default first, then `others` sorted, each once, canonical.
[[nodiscard]] std::vector<std::string> voiceLocaleList(std::string_view defaultLocale,
                                                       std::span<const std::string> others);

// --- Lines of dialogue (ADR 0200 section 5) --------------------------------------
//
// `content/dialogue/<name>.lines.json`: who speaks, and the lines by id.
//
//     { "speakers": { "mara": { "name": "speaker.mara", "color": "#E8B04A" } },
//       "lines":    { "intro_01": { "speaker": "mara" },
//                     "intro_02": { "speaker": "tom", "text": "some.other.key",
//                                   "sound": "asset://voice/other.ogg", "seconds": 2.5 } } }
//
// A line's id in the game is `<name>.<id>`. Its text key is that id and its
// sound `asset://voice/<name>/<id>.ogg` unless the entry says otherwise;
// `"sound": ""` is a line that is never voiced.
struct DialogueLine
{
    // The catalog key of the words, and of the speaker's name (or empty).
    std::string text;
    std::string speaker;
    core::Color3 color{1.0f, 1.0f, 1.0f};
    // The sound's content name; empty for a line that is never voiced.
    std::string sound;
    // What `Caption.Seconds` is given; 0 is a reading time.
    core::f32 seconds = 0.0f;
};

class DialogueLines
{
public:
    // One file's lines, as its text, under its `name` (`act1` for
    // `act1.lines.json`). Replaces what that name had. False, with why, for a
    // file that is not a lines file -- and the lines it had stay.
    bool load(std::string_view name, std::string_view json, std::string* diagnostic = nullptr);
    // Forgets one file's lines, or every file's.
    void forget(std::string_view name);
    void clear();

    // The line `<name>.<id>`, or null.
    [[nodiscard]] const DialogueLine* find(std::string_view line) const;
    [[nodiscard]] core::usize size() const noexcept { return lines_.size(); }
    // Every line's id, sorted: what a report walks.
    [[nodiscard]] std::vector<std::string> ids() const;

private:
    std::map<std::string, DialogueLine, std::less<>> lines_;
};

// **How long a caption with no recording shows** (ADR 0200), in seconds: its
// own `seconds`, or when that is zero a reading time for `text` -- a second
// and a half, and six hundredths of a second a character (a code point),
// twelve seconds at most. A pure function of its two inputs, which is what
// lets a tick end a sound by it (R10).
[[nodiscard]] core::f64 captionSeconds(core::f32 seconds, std::string_view text) noexcept;

} // namespace engine::scene
