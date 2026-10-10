// An asset in the player's language (ADR 0200): the names a file may be found
// under, the index an export carries, and what a loose folder says of itself.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/asset/content.h"
#include "engine/asset/localized.h"

using namespace engine::asset;

namespace {

struct Folder
{
    std::filesystem::path root;

    Folder()
    {
        root = std::filesystem::temp_directory_path() / "engine-localized-tests";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
    }

    ~Folder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void file(const std::string& relative) const
    {
        const std::filesystem::path path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << "x";
    }
};

[[nodiscard]] std::vector<std::string> candidates(std::string_view urn, std::string_view locale)
{
    std::vector<std::string> out;
    localizedCandidates(urn, locale, out);
    return out;
}

} // namespace

TEST_CASE("ADR 0200: a name is looked for under its locale, then its language, then as written")
{
    CHECK(candidates("asset://voice/intro_01.ogg", "pt-BR") ==
          std::vector<std::string>{"asset://l10n/pt-BR/voice/intro_01.ogg", "asset://l10n/pt/voice/intro_01.ogg",
                                   "asset://voice/intro_01.ogg"});
    // A locale that is a language alone has nothing wider to try.
    CHECK(candidates("asset://voice/intro_01.ogg", "ja") ==
          std::vector<std::string>{"asset://l10n/ja/voice/intro_01.ogg", "asset://voice/intro_01.ogg"});
    // No language asked for is the name itself, and so is a name that is
    // already a language's: it is not localized twice.
    CHECK(candidates("asset://voice/intro_01.ogg", "") == std::vector<std::string>{"asset://voice/intro_01.ogg"});
    CHECK(candidates("asset://l10n/ja/voice/intro_01.ogg", "pt-BR") ==
          std::vector<std::string>{"asset://l10n/ja/voice/intro_01.ogg"});
    // And what is not content is left alone.
    CHECK(candidates("view://minimap", "ja") == std::vector<std::string>{"view://minimap"});
    // A folder that only begins like the localized one is an ordinary folder.
    CHECK(candidates("asset://l10nx/a.ogg", "ja") ==
          std::vector<std::string>{"asset://l10n/ja/l10nx/a.ogg", "asset://l10nx/a.ogg"});
}

TEST_CASE("ADR 0200: a file missing in a language is the default language's, never nothing")
{
    const Folder folder;
    folder.file("voice/both.ogg");
    folder.file("l10n/pt-BR/voice/both.ogg");
    folder.file("voice/default_only.ogg");
    folder.file("l10n/pt/voice/language_only.ogg");
    folder.file("voice/language_only.ogg");

    ContentMounts mounts;
    mounts.mountDirectory(folder.root);

    CHECK(resolveLocalized(mounts, "asset://voice/both.ogg", "pt-BR") == "asset://l10n/pt-BR/voice/both.ogg");
    CHECK(resolveLocalized(mounts, "asset://voice/default_only.ogg", "pt-BR") == "asset://voice/default_only.ogg");
    // Recorded once for the language, heard in every region of it.
    CHECK(resolveLocalized(mounts, "asset://voice/language_only.ogg", "pt-BR") ==
          "asset://l10n/pt/voice/language_only.ogg");
    CHECK(resolveLocalized(mounts, "asset://voice/both.ogg", "ja") == "asset://voice/both.ogg");
    // A name nobody has comes back as written, for whoever reports it missing.
    CHECK(resolveLocalized(mounts, "asset://voice/nobody.ogg", "pt-BR") == "asset://voice/nobody.ogg");

    CHECK(localeOfResolved("asset://l10n/pt-BR/voice/both.ogg") == "pt-BR");
    CHECK(localeOfResolved("asset://voice/both.ogg").empty());
    CHECK(localeOfResolved("asset://l10n/").empty());
}

TEST_CASE("ADR 0200: the index an export carries is read, and one that is not an index is refused")
{
    LocalizationIndex index;
    std::string what;
    REQUIRE(readLocalizationIndex(
        R"({ "version": 1, "voice": ["pt-BR", "ja"], "shipped": ["pt-BR", "de"],
             "lengths": { "voice/b.ogg": 96000, "voice/a.ogg": 134400 },
             "lines": ["dialogue/act1.lines.json"] })",
        index, &what));
    // Sorted whatever order the file had them in.
    CHECK(index.voice == std::vector<std::string>{"ja", "pt-BR"});
    // A language that has no voice cannot have shipped.
    CHECK(index.shipped == std::vector<std::string>{"pt-BR"});
    CHECK(index.lines == std::vector<std::string>{"dialogue/act1.lines.json"});
    CHECK(index.measured);
    REQUIRE(index.longest("voice/a.ogg").has_value());
    CHECK(*index.longest("voice/a.ogg") == 134400u);
    CHECK(*index.longest("voice/b.ogg") == 96000u);
    CHECK_FALSE(index.longest("voice/c.ogg").has_value());

    // Its lengths decide the tick a sound ends on: nothing is guessed.
    CHECK_FALSE(readLocalizationIndex("not json", index, &what));
    CHECK_FALSE(what.empty());
    CHECK_FALSE(readLocalizationIndex(R"({ "version": 2, "voice": [] })", index, &what));
    CHECK(what == "version");
    CHECK_FALSE(readLocalizationIndex(R"({ "version": 1, "lengths": { "voice/a.ogg": -4 } })", index, &what));
    CHECK(what == "lengths");
    CHECK_FALSE(readLocalizationIndex(R"([1, 2])", index, &what));
    CHECK(index.empty());
}

TEST_CASE("ADR 0200: a loose folder's languages are the ones with a sound in them, and its lines files are found")
{
    const Folder folder;
    folder.file("voice/act1/intro_01.ogg");
    folder.file("l10n/pt-BR/voice/act1/intro_01.OGG");
    folder.file("l10n/ja/voice/act1/intro_01.wav");
    // Translated pictures alone are not a voice.
    folder.file("l10n/de/ui/logo.png");
    folder.file("dialogue/act2.lines.json");
    folder.file("dialogue/act1.lines.json");
    folder.file("dialogue/notes.txt");
    folder.file("dialogue/deeper/act3.lines.json");

    LocalizationIndex index;
    scanLocalizedContent(folder.root, index);
    CHECK(index.voice == std::vector<std::string>{"ja", "pt-BR"});
    CHECK(index.shipped == index.voice);
    CHECK(index.lines == std::vector<std::string>{"dialogue/act1.lines.json", "dialogue/act2.lines.json"});
    // Nothing was measured: a development run reads a header when it is asked.
    CHECK_FALSE(index.measured);
    CHECK(index.lengths.empty());

    // A project with none of it says nothing.
    const std::filesystem::path bare = folder.root / "voice";
    scanLocalizedContent(bare, index);
    CHECK(index.empty());
}

TEST_CASE("ADR 0200: a sound is told by its extension, in any case")
{
    CHECK(isSoundPath("voice/a.ogg"));
    CHECK(isSoundPath("voice/a.WAV"));
    CHECK(isSoundPath("a.Mp3"));
    CHECK(isSoundPath("a.flac"));
    CHECK_FALSE(isSoundPath("a.png"));
    CHECK_FALSE(isSoundPath("ogg"));
    CHECK_FALSE(isSoundPath(""));
}
