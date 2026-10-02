// A game's text by key, in the player's language (ADR 0154): which catalog a
// key is read from, and which catalog a wanted locale is.
#include <array>
#include <doctest/doctest.h>
#include <string>

#include "engine/scene/localization.h"

namespace {

namespace core = engine::core;
namespace scene = engine::scene;

constexpr const char* English = R"({
  "menu.play": "Play",
  "menu.quit": "Quit",
  "hud.score": "Score: {points}",
  "hud.greeting": "Hello, {name}!"
})";

constexpr const char* Portuguese = R"({
  "menu.play": "Jogar",
  "hud.score": "Pontos: {points}",
  "hud.greeting": "Olá, {name}!"
})";

[[nodiscard]] scene::Localization game()
{
    scene::Localization localization;
    REQUIRE(localization.load("en", English));
    REQUIRE(localization.load("pt-BR", Portuguese));
    return localization;
}

} // namespace

TEST_CASE("a locale is written one way, however it was typed")
{
    CHECK(scene::canonicalLocale("pt-BR") == "pt-BR");
    CHECK(scene::canonicalLocale("PT_br") == "pt-BR");
    CHECK(scene::canonicalLocale("EN") == "en");
    CHECK(scene::canonicalLocale("zh_hans") == "zh-Hans");
    CHECK(scene::canonicalLocale("zh-hant-tw") == "zh-Hant-TW");
    // Not a locale at all.
    CHECK(scene::canonicalLocale("").empty());
    CHECK(scene::canonicalLocale("pt BR").empty());
    CHECK(scene::canonicalLocale("-BR").empty());
    CHECK(scene::canonicalLocale("../secrets").empty());
}

TEST_CASE("a key in two catalogs returns each by the locale")
{
    const scene::Localization localization = game();
    CHECK(localization.translate("en", "menu.play") == "Play");
    CHECK(localization.translate("pt-BR", "menu.play") == "Jogar");
    // However the locale was typed.
    CHECK(localization.translate("pt_br", "menu.play") == "Jogar");
}

TEST_CASE("a key the player's language lacks is read in the project's default")
{
    scene::Localization localization = game();
    CHECK(localization.translate("pt-BR", "menu.quit") == "Quit");

    // The default is the project's to name.
    localization.setDefaultLocale("pt-BR");
    CHECK(localization.translate("fr", "menu.play") == "Jogar");
    CHECK(localization.defaultLocale() == "pt-BR");
}

TEST_CASE("placeholders are filled from the arguments")
{
    const scene::Localization localization = game();
    const std::array<core::I18nArg, 1> points{core::I18nArg{"points", core::i64{1200}}};
    CHECK(localization.translate("en", "hud.score", points) == "Score: 1200");
    CHECK(localization.translate("pt-BR", "hud.score", points) == "Pontos: 1200");
    const std::array<core::I18nArg, 1> name{core::I18nArg{"name", std::string_view{"Ana"}}};
    CHECK(localization.translate("pt-BR", "hud.greeting", name) == "Olá, Ana!");
}

TEST_CASE("a key nobody has is nothing, which is the caller's to show as the key")
{
    const scene::Localization localization = game();
    CHECK_FALSE(localization.translate("en", "menu.options").has_value());
    CHECK_FALSE(localization.translate("pt-BR", "").has_value());
}

TEST_CASE("the engine's own text is under the project's, and a project's key of the same name wins")
{
    scene::Localization localization = game();
    // What the engine says in another language, where it has it.
    REQUIRE(localization.loadEngine("pt-BR", R"({"engine.settings.apply": "Aplicar"})"));
    REQUIRE(localization.loadEngine("en", R"({"engine.settings.apply": "Apply"})"));
    CHECK(localization.translate("pt-BR", "engine.settings.apply") == "Aplicar");
    CHECK(localization.translate("en", "engine.settings.apply") == "Apply");
    // A language the engine has no catalog for reads its plain language's.
    REQUIRE(localization.loadEngine("pt", R"({"engine.settings.revert": "Reverter"})"));
    CHECK(localization.translate("pt-BR", "engine.settings.revert") == "Reverter");

    REQUIRE(localization.load("en", R"({"engine.settings.apply": "OK"})"));
    CHECK(localization.translate("en", "engine.settings.apply") == "OK");
}

TEST_CASE("a wanted locale is narrowed to a catalog the project has")
{
    const scene::Localization localization = game();
    CHECK(localization.narrow("en") == "en");
    CHECK(localization.narrow("pt-BR") == "pt-BR");
    // The same language: a Portuguese reader is shown the Portuguese there is.
    CHECK(localization.narrow("pt") == "pt-BR");
    CHECK(localization.narrow("pt-PT") == "pt-BR");
    CHECK(localization.narrow("en-US") == "en");
    // No catalog of that language: nothing, and the caller falls back.
    CHECK(localization.narrow("fr").empty());
    CHECK(localization.narrow("").empty());

    const std::vector<std::string> locales = localization.locales();
    REQUIRE(locales.size() == 2);
    CHECK(locales[0] == "en");
    CHECK(locales[1] == "pt-BR");
}

TEST_CASE("a project with no catalog has one locale, its default")
{
    scene::Localization localization;
    REQUIRE(localization.locales().size() == 1);
    CHECK(localization.locales().front() == "en");
    CHECK(localization.narrow("en-GB") == "en");
    CHECK(localization.narrow("pt").empty());
    localization.setDefaultLocale("pt-BR");
    CHECK(localization.locales().front() == "pt-BR");
    CHECK(localization.narrow("pt") == "pt-BR");
}

TEST_CASE("a file that is not a catalog leaves the words its locale had")
{
    scene::Localization localization = game();
    const core::u64 before = localization.revision();
    std::string diagnostic;
    CHECK_FALSE(localization.load("en", "{ not json", &diagnostic));
    CHECK_FALSE(diagnostic.empty());
    CHECK(localization.revision() == before);
    CHECK(localization.translate("en", "menu.play") == "Play");
    // A file named for nothing is no locale's.
    CHECK_FALSE(localization.load("", English));

    // A catalog read again replaces its locale's, and says so.
    REQUIRE(localization.load("en", R"({"menu.play": "Start"})"));
    CHECK(localization.revision() != before);
    CHECK(localization.translate("en", "menu.play") == "Start");
    CHECK_FALSE(localization.translate("en", "menu.quit").has_value());
}
