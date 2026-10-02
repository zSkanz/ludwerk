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

} // namespace engine::scene
