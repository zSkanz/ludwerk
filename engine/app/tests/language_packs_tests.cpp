// Language packs (ADR 0200 §7): the voices a game ships apart from itself,
// mounted after its own pack, and refused where they are not what they say.
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/language_packs.h"
#include "engine/asset/content.h"
#include "engine/asset/pack.h"
#include "engine/asset/seal.h"
#include "engine/core/content_hash.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/random.h"

using namespace engine;

namespace {

void seedRealCatalog()
{
    const auto result = core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// Bytes nothing compresses, as a recording is: stored as they are, so a byte
// changed in the file is a byte changed in the sound.
[[nodiscard]] std::vector<std::byte> recording(std::size_t size, core::u64 seed)
{
    core::Pcg32 random(seed);
    std::vector<std::byte> out(size);
    for (std::byte& byte : out)
        byte = static_cast<std::byte>(random.nextU32() & 0xFFu);
    return out;
}

void writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

[[nodiscard]] std::vector<std::byte> readBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(text.size());
    if (!text.empty())
        std::memcpy(out.data(), text.data(), text.size());
    return out;
}

struct Named
{
    std::string urn;
    std::vector<std::byte> bytes;
};

// A pack as an export leaves one: plain with its manifest beside it, or
// sealed, which takes the manifest's names in and removes it.
void writePack(const std::filesystem::path& path, const std::vector<Named>& content, bool sealed)
{
    asset::PackWriter writer;
    std::string manifest = "{\"format\":\"content-manifest\",\"version\":1,\"assets\":[";
    for (std::size_t index = 0; index < content.size(); ++index) {
        const core::ContentHash hash = writer.addContent(asset::AssetKind::Raw, content[index].bytes);
        manifest += std::string(index == 0 ? "" : ",") + "{\"urn\":\"" + content[index].urn + "\",\"hash\":\"" +
                    hash.toHex() + "\",\"kind\":\"raw\",\"bytes\":" + std::to_string(content[index].bytes.size()) + "}";
    }
    manifest += "]}";
    writeBytes(path, writer.build());
    const std::filesystem::path manifestPath = asset::packManifestPath(path);
    writeBytes(manifestPath, std::as_bytes(std::span<const char>(manifest.data(), manifest.size())));
    if (sealed) {
        const auto error = asset::sealPack(path);
        REQUIRE_MESSAGE(!error.has_value(), (error.has_value() ? error->message : ""));
        std::error_code ec;
        REQUIRE_FALSE(std::filesystem::exists(manifestPath, ec));
    }
}

// A game's folder with its own pack in it, sealed or plain, holding the
// default language's line and the index's place.
struct Game
{
    std::filesystem::path root;
    std::filesystem::path engineFolder;
    std::vector<std::byte> english = recording(4000, 1);
    std::vector<std::byte> portuguese = recording(5000, 2);
    std::vector<std::byte> japanese = recording(6000, 3);
    asset::ContentMounts mounts;

    Game(const char* name, bool sealed)
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / "engine-language-pack-tests" / name;
        std::filesystem::remove_all(root, ec);
        engineFolder = root / ".engine";
        std::filesystem::create_directories(engineFolder, ec);
        writePack(asset::gamePackPath(root), {{"asset://voice/intro_01.ogg", english}}, sealed);
        const auto error = mounts.mountPack(asset::gamePackPath(root));
        REQUIRE_MESSAGE(!error.has_value(), (error.has_value() ? error->message : ""));
    }

    ~Game()
    {
        mounts.clear();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    [[nodiscard]] bool resolvesTo(std::string_view urn, const std::vector<std::byte>& bytes) const
    {
        const asset::ResolvedContent found = mounts.resolve(urn);
        return found.found() && found.bytes.size() == bytes.size() &&
               std::memcmp(found.bytes.data(), bytes.data(), bytes.size()) == 0;
    }
};

// Every warning said while it lives.
struct Warnings
{
    std::vector<std::string> lines;

    Warnings()
    {
        (void)core::setLogSink([this](core::LogLevel level, std::string_view text) {
            if (level == core::LogLevel::Warn)
                lines.emplace_back(text);
        });
    }
    ~Warnings() { core::resetLogSink(); }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;

    [[nodiscard]] bool said(std::string_view words) const
    {
        for (const std::string& line : lines) {
            if (line.find(words) != std::string::npos)
                return true;
        }
        return false;
    }
};

} // namespace

TEST_CASE("ADR 0200: a language pack beside a sealed game's pack is mounted after it, and only one the game names")
{
    seedRealCatalog();
    Game game("sealed", true);
    const std::vector<std::string> voice{"ja", "pt-BR", "zz"};
    const std::vector<std::string> shipped{"zz"};

    // Nothing installed: nothing mounted, and nothing said.
    {
        const Warnings warnings;
        CHECK(app::mountLanguagePacks(game.mounts, game.engineFolder, voice, shipped).empty());
        CHECK(warnings.lines.empty());
        CHECK(game.mounts.mountCount() == 1);
    }

    CHECK(app::languagePackPath(game.engineFolder, "pt-BR") == game.engineFolder / "l10n-pt-BR.lpack");
    writePack(app::languagePackPath(game.engineFolder, "pt-BR"),
              {{"asset://l10n/pt-BR/voice/intro_01.ogg", game.portuguese}}, true);
    writePack(app::languagePackPath(game.engineFolder, "ja"), {{"asset://l10n/ja/voice/intro_01.ogg", game.japanese}},
              true);
    // A language whose sounds are in the game's own pack has no pack to look
    // for, and neither has one the game does not name: both are files nobody
    // opens, whatever is in them.
    writeBytes(app::languagePackPath(game.engineFolder, "zz"), game.english);
    writeBytes(app::languagePackPath(game.engineFolder, "de"), game.english);

    const Warnings warnings;
    const std::vector<std::string> mounted = app::mountLanguagePacks(game.mounts, game.engineFolder, voice, shipped);
    CHECK(mounted == std::vector<std::string>{"ja", "pt-BR"});
    CHECK(warnings.lines.empty());
    CHECK(game.mounts.mountCount() == 3);

    CHECK(game.resolvesTo("asset://l10n/pt-BR/voice/intro_01.ogg", game.portuguese));
    CHECK(game.resolvesTo("asset://l10n/ja/voice/intro_01.ogg", game.japanese));
    // The game's own names are still the game's.
    CHECK(game.resolvesTo("asset://voice/intro_01.ogg", game.english));
    CHECK_FALSE(game.mounts.contains("asset://l10n/de/voice/intro_01.ogg"));
    // Each mounted pack is whole, as the game's own check finds them.
    for (const asset::Pack* pack : game.mounts.packs())
        CHECK_FALSE(pack->verify().has_value());
}

TEST_CASE("ADR 0200: a plain language pack is mounted with its manifest, and one that names anything else is refused")
{
    seedRealCatalog();
    Game game("plain", false);
    const std::vector<std::string> voice{"ja", "pt-BR"};

    writePack(app::languagePackPath(game.engineFolder, "pt-BR"),
              {{"asset://l10n/pt-BR/voice/intro_01.ogg", game.portuguese}}, false);
    // Another language's pack under this one's name, and with it a name of
    // the game's own.
    writePack(app::languagePackPath(game.engineFolder, "ja"),
              {{"asset://l10n/ja/voice/intro_01.ogg", game.japanese}, {"asset://voice/intro_01.ogg", game.japanese}},
              false);

    const Warnings warnings;
    const std::vector<std::string> mounted = app::mountLanguagePacks(game.mounts, game.engineFolder, voice, {});
    CHECK(mounted == std::vector<std::string>{"pt-BR"});
    CHECK(game.mounts.mountCount() == 2);
    CHECK(game.resolvesTo("asset://l10n/pt-BR/voice/intro_01.ogg", game.portuguese));
    CHECK(game.resolvesTo("asset://voice/intro_01.ogg", game.english));
    CHECK_FALSE(game.mounts.contains("asset://l10n/ja/voice/intro_01.ogg"));
    REQUIRE(warnings.lines.size() == 1);
    CHECK(warnings.said("l10n-ja.lpack"));
    CHECK(warnings.said("asset://voice/intro_01.ogg"));

    // A plain pack whose manifest is gone answers to no name, and is refused.
    game.mounts.clear();
    REQUIRE_FALSE(game.mounts.mountPack(asset::gamePackPath(game.root)).has_value());
    std::error_code ec;
    std::filesystem::remove(asset::packManifestPath(app::languagePackPath(game.engineFolder, "pt-BR")), ec);
    CHECK(app::mountLanguagePacks(game.mounts, game.engineFolder, voice, {}).empty());
    CHECK(game.mounts.mountCount() == 1);
}

TEST_CASE("ADR 0200: a language pack that is damaged, a game's, or no pack at all is refused, and the game goes on")
{
    seedRealCatalog();
    Game game("refused", true);
    const std::vector<std::string> voice{"de", "ja", "pt-BR"};

    // One byte of the recording changed after the pack was made.
    const std::filesystem::path damaged = app::languagePackPath(game.engineFolder, "pt-BR");
    writePack(damaged, {{"asset://l10n/pt-BR/voice/intro_01.ogg", game.portuguese}}, true);
    std::vector<std::byte> bytes = readBytes(damaged);
    REQUIRE(bytes.size() > asset::PackHeaderBytes + 100);
    bytes[asset::PackHeaderBytes + 100] ^= std::byte{0x40};
    writeBytes(damaged, bytes);

    // A game's own pack under a language's name: sealed with a listing of the
    // game's files in it, as `sealGame` leaves one.
    const std::filesystem::path other = game.root / "other";
    writePack(asset::gamePackPath(other), {{"asset://l10n/ja/voice/intro_01.ogg", game.japanese}}, false);
    writeBytes(other / "project.toml", game.english);
    REQUIRE_FALSE(asset::sealGame(other).has_value());
    writeBytes(app::languagePackPath(game.engineFolder, "ja"), readBytes(asset::gamePackPath(other)));

    // And a file that is not a pack.
    writeBytes(app::languagePackPath(game.engineFolder, "de"), game.english);

    const Warnings warnings;
    CHECK(app::mountLanguagePacks(game.mounts, game.engineFolder, voice, {}).empty());
    CHECK(game.mounts.mountCount() == 1);
    CHECK(warnings.lines.size() == 3);
    CHECK(warnings.said("l10n-pt-BR.lpack"));
    CHECK(warnings.said("l10n-ja.lpack"));
    CHECK(warnings.said("l10n-de.lpack"));
    CHECK(warnings.said("a game's own files"));
    // What the game shipped is untouched by any of it.
    CHECK(game.resolvesTo("asset://voice/intro_01.ogg", game.english));
    CHECK_FALSE(game.mounts.contains("asset://l10n/pt-BR/voice/intro_01.ogg"));
    CHECK_FALSE(game.mounts.contains("asset://l10n/ja/voice/intro_01.ogg"));
}
