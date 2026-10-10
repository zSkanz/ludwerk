// A game's text by key, in the player's language (ADR 0154): which catalog a
// key is read from, and which catalog a wanted locale is.
#include <array>
#include <doctest/doctest.h>
#include <span>
#include <string>
#include <vector>

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

// --- A voice language apart from the text language (ADR 0200) -----------------

TEST_CASE("ADR 0200: a voice is narrowed to what can be played, and follows the text when nothing was chosen")
{
    const std::array<std::string, 3> playable{"en", "ja", "pt-BR"};

    // Exactly, then the bare language, then any region of it.
    CHECK(scene::narrowVoice("pt-br", playable) == "pt-BR");
    CHECK(scene::narrowVoice("pt-PT", playable) == "pt-BR");
    CHECK(scene::narrowVoice("ja-JP", playable) == "ja");
    CHECK(scene::narrowVoice("de", playable).empty());
    CHECK(scene::narrowVoice("", playable).empty());

    const std::array<std::string, 2> system{"de-DE", "ja-JP"};
    // What the player chose stands in front of everything.
    CHECK(scene::voiceLocaleFor("ja", "pt-BR", playable, system) == "ja");
    // Nothing chosen: the language being read.
    CHECK(scene::voiceLocaleFor("", "pt-BR", playable, system) == "pt-BR");
    // Read in a language the game has no voice for: the first of the
    // system's own that it has one for -- which need not be the text's.
    CHECK(scene::voiceLocaleFor("", "de", playable, system) == "ja");
    // And with none of those, the default, which is first of the list.
    CHECK(scene::voiceLocaleFor("", "de", playable, std::span<const std::string>{}) == "en");
    // **A voice chosen once whose pack has since gone** is narrowed like any
    // other, with no word said: it follows the text again.
    const std::array<std::string, 2> fewer{"en", "pt-BR"};
    CHECK(scene::voiceLocaleFor("ja", "pt-BR", fewer, system) == "pt-BR");
}

TEST_CASE("ADR 0200: the list of voices starts with the default language, and the rest are in order")
{
    const std::array<std::string, 4> others{"pt-br", "ja", "en", "ja"};
    CHECK(scene::voiceLocaleList("en", others) == std::vector<std::string>{"en", "ja", "pt-BR"});
    CHECK(scene::voiceLocaleList("", std::span<const std::string>{}) == std::vector<std::string>{"en"});
}

TEST_CASE("ADR 0200: a lines file names its lines, and what an entry does not say comes from the line's name")
{
    scene::DialogueLines lines;
    std::string why;
    REQUIRE(lines.load("act1", R"({
        "speakers": { "mara": { "name": "speaker.mara", "color": "#E8B04A" }, "tom": {} },
        "lines": {
            "intro_01": { "speaker": "mara" },
            "intro_02": { "speaker": "tom", "text": "some.other.key", "sound": "asset://voice/other.ogg",
                          "seconds": 2.5 },
            "aside": { "sound": "" }
        } })",
                       &why));
    REQUIRE(lines.size() == 3);

    const scene::DialogueLine* first = lines.find("act1.intro_01");
    REQUIRE(first != nullptr);
    // Its text key is its name, and its sound is where a recording of it goes.
    CHECK(first->text == "act1.intro_01");
    CHECK(first->sound == "asset://voice/act1/intro_01.ogg");
    CHECK(first->speaker == "speaker.mara");
    CHECK(static_cast<double>(first->color.r) == doctest::Approx(232.0 / 255.0));
    CHECK(first->seconds == 0.0f);

    const scene::DialogueLine* second = lines.find("act1.intro_02");
    REQUIRE(second != nullptr);
    CHECK(second->text == "some.other.key");
    CHECK(second->sound == "asset://voice/other.ogg");
    // A speaker with no name of its own is named after its id.
    CHECK(second->speaker == "speaker.tom");
    CHECK(static_cast<double>(second->seconds) == doctest::Approx(2.5));

    // Never voiced, and said by nobody.
    const scene::DialogueLine* aside = lines.find("act1.aside");
    REQUIRE(aside != nullptr);
    CHECK(aside->sound.empty());
    CHECK(aside->speaker.empty());
    CHECK(lines.find("intro_01") == nullptr);

    // A second file's lines are beside the first's; a file read again
    // replaces its own and no other's.
    REQUIRE(lines.load("act2", R"({ "lines": { "a": {} } })"));
    REQUIRE(lines.load("act1", R"({ "lines": { "only": {} } })"));
    CHECK(lines.ids() == std::vector<std::string>{"act1.only", "act2.a"});

    // What is not a lines file is refused whole, and what was read stays.
    CHECK_FALSE(lines.load("act2", R"({ "lines": { "a": { "speaker": "nobody" } } })", &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(lines.load("act2", "not json", &why));
    CHECK_FALSE(lines.load("act2", R"({ "lines": { "a": { "seconds": -1 } } })", &why));
    CHECK(lines.find("act2.a") != nullptr);
    lines.forget("act2");
    CHECK(lines.ids() == std::vector<std::string>{"act1.only"});
}

TEST_CASE("ADR 0200: a line with no recording lasts a reading time, counted in characters and not in bytes")
{
    // The author's number when there is one.
    CHECK(scene::captionSeconds(2.5f, "anything") == doctest::Approx(2.5));
    // A second and a half, and six hundredths a character.
    CHECK(scene::captionSeconds(0.0f, "") == doctest::Approx(1.5));
    CHECK(scene::captionSeconds(0.0f, "hello there") == doctest::Approx(1.5 + 11 * 0.06));
    // Three characters of three bytes each are three characters.
    CHECK(scene::captionSeconds(0.0f, "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB") == doctest::Approx(1.5 + 3 * 0.06));
    // And never longer than twelve seconds, whatever was written.
    CHECK(scene::captionSeconds(0.0f, std::string(1000, 'a')) == doctest::Approx(12.0));
}
