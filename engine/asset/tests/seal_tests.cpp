// A game sealed into its pack (ADR 0183): the format's two additions -- an
// entry stored deflated, and the names a pack answers to, hashed -- and the
// folder a sealed game is.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/content.h"
#include "engine/asset/pack.h"
#include "engine/asset/seal.h"
#include "engine/core/content_hash.h"
#include "engine/core/i18n.h"
#include "engine/core/random.h"

using namespace engine::asset;
using engine::core::ContentHash;
using engine::core::engineCatalog;
using engine::core::hashText;

namespace {

void seedRealCatalog()
{
    const auto result = engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    if (!text.empty())
        std::memcpy(out.data(), text.data(), text.size());
    return out;
}

[[nodiscard]] std::string textOf(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool holds(std::span<const std::byte> haystack, std::string_view needle)
{
    const std::string_view text(reinterpret_cast<const char*>(haystack.data()), haystack.size());
    return text.find(needle) != std::string_view::npos;
}

// Bytes nothing compresses: what a texture or a mesh already is.
[[nodiscard]] std::vector<std::byte> noise(std::size_t size, engine::core::u64 seed)
{
    engine::core::Pcg32 random(seed);
    std::vector<std::byte> out(size);
    for (std::byte& byte : out)
        byte = static_cast<std::byte>(random.nextU32() & 0xFFu);
    return out;
}

// A scene as a pack holds one: text, with the names of things in it.
[[nodiscard]] std::string sceneText()
{
    std::string scene = "{\"format\":\"scene\",\"instances\":[";
    for (int index = 0; index < 60; ++index) {
        scene += "{\"class\":\"Part\",\"name\":\"SENTINEL-SCENE-Crate" + std::to_string(index) +
                 "\",\"material\":\"asset://materials/wood.material.json\"},";
    }
    scene += "{}]}";
    return scene;
}

void write(const std::filesystem::path& path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::filesystem::path freshDir(const char* name)
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "engine-seal-tests" / name;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("ADR 0183: a sealed pack stores what is text deflated, and hands it back as it was")
{
    seedRealCatalog();
    const std::string scene = sceneText();
    const std::vector<std::byte> texture = noise(40000, 7);
    PackWriter writer;
    const ContentHash sceneHash = writer.addContent(AssetKind::Raw, bytesOf(scene));
    const ContentHash textureHash = writer.addContent(AssetKind::Texture, texture);
    const ContentHash fontHash = writer.addContent(AssetKind::Raw, noise(100000, 11));
    const ContentHash emptyHash = writer.addContent(AssetKind::Raw, bytesOf(""));

    // Unsealed it is the format it always was, and the scene is there to read.
    const std::vector<std::byte> plain = writer.build();
    CHECK(static_cast<unsigned>(plain[4]) == PackFormatPlain);
    CHECK(holds(plain, "SENTINEL-SCENE-Crate7"));

    const std::vector<std::byte> sealed = writer.buildSealed();
    CHECK(static_cast<unsigned>(sealed[4]) == PackFormatVersion);
    CHECK(sealed.size() < plain.size());
    CHECK_FALSE(holds(sealed, "SENTINEL-SCENE"));
    CHECK_FALSE(holds(sealed, "asset://materials"));
    // The same content sealed twice is the same file.
    CHECK(writer.buildSealed() == sealed);

    Pack pack;
    REQUIRE_FALSE(Pack::openVerified(sealed, pack).has_value());
    const PackEntry* const sceneEntry = pack.find(sceneHash);
    REQUIRE(sceneEntry != nullptr);
    CHECK(sceneEntry->codec == BlobCodec::Deflate);
    CHECK(sceneEntry->originalSize == scene.size());
    CHECK(sceneEntry->storedSize < scene.size() / 4);
    // What does not shrink is stored as it is: a texture by its kind, and a
    // raw file -- a font, a sound -- by what deflating it gave.
    CHECK(pack.find(textureHash)->codec == BlobCodec::None);
    CHECK(pack.find(fontHash)->codec == BlobCodec::None);
    CHECK(pack.find(emptyHash)->codec == BlobCodec::None);

    CHECK(textOf(pack.blob(sceneHash)) == scene);
    // Asked again, the same bytes at the same place: it was inflated once.
    CHECK(pack.blob(sceneHash).data() == pack.blob(sceneHash).data());
    CHECK(pack.blob(textureHash).size() == texture.size());
    CHECK(pack.blob(emptyHash).empty());
    CHECK_FALSE(pack.damaged());
    CHECK_FALSE(pack.verify().has_value());
    CHECK(pack.names() == nullptr);

    // A build that reads format 1 alone is not handed a deflated entry by
    // mistake: a format 1 header over these entries is refused.
    std::vector<std::byte> downgraded = sealed;
    downgraded[4] = static_cast<std::byte>(PackFormatPlain);
    Pack refused;
    CHECK(Pack::open(std::move(downgraded), refused).has_value());
}

TEST_CASE("ADR 0183: a deflated entry that is not what its name says is damage that is said, never read")
{
    seedRealCatalog();
    const std::string scene = sceneText();
    PackWriter writer;
    const ContentHash sceneHash = writer.addContent(AssetKind::Raw, bytesOf(scene));
    (void)writer.addContent(AssetKind::Texture, noise(5000, 3));
    const std::vector<std::byte> sealed = writer.buildSealed();

    Pack whole;
    REQUIRE_FALSE(Pack::open(sealed, whole).has_value());
    const PackEntry entry = *whole.find(sceneHash);

    // Every byte of the stored stream in turn, a few at a time: the pack
    // opens -- its table is whole -- and the entry is empty or it is exactly
    // the scene, never anything else.
    engine::core::Pcg32 random(99);
    int refusedByVerify = 0;
    for (int round = 0; round < 200; ++round) {
        std::vector<std::byte> changed = sealed;
        const std::size_t at = static_cast<std::size_t>(entry.offset) + random.nextU32() % entry.storedSize;
        changed[at] ^= static_cast<std::byte>(1u + random.nextU32() % 255u);
        Pack pack;
        REQUIRE_FALSE(Pack::open(changed, pack).has_value());
        const std::span<const std::byte> blob = pack.blob(sceneHash);
        const bool same = textOf(blob) == scene;
        CHECK((blob.empty() || same));
        CHECK(pack.damaged() == blob.empty());
        if (pack.verify().has_value())
            ++refusedByVerify;
        Pack verified;
        CHECK(Pack::openVerified(changed, verified).has_value() == blob.empty());
    }
    CHECK(refusedByVerify > 150);
}

TEST_CASE("ADR 0183: a pack's names are hashes of them, found by name and listed by nobody")
{
    std::vector<PackName> rows;
    const ContentHash wood = hashText("the wood material's bytes");
    const ContentHash scene = hashText("the scene's bytes");
    rows.push_back(PackName{hashText("asset://scenes/main.scene.json"), scene, AssetKind::Raw});
    rows.push_back(PackName{hashText("asset://materials/wood.material.json"), wood, AssetKind::Material});
    rows.push_back(PackName{hashText("game://src/client/Main.luauc"), scene, AssetKind::Raw});
    // The same name twice is one name.
    rows.push_back(PackName{hashText("asset://scenes/main.scene.json"), scene, AssetKind::Raw});

    const std::vector<std::byte> encoded = encodePackNames(rows);
    CHECK_FALSE(holds(encoded, "asset://"));
    CHECK_FALSE(holds(encoded, "main.scene"));
    std::vector<PackName> decoded;
    REQUIRE(decodePackNames(encoded, decoded));
    CHECK(decoded.size() == 3);
    const PackName* const found = findPackName(decoded, "asset://materials/wood.material.json");
    REQUIRE(found != nullptr);
    CHECK(found->content == wood);
    CHECK(found->kind == AssetKind::Material);
    CHECK(findPackName(decoded, "asset://materials/Wood.material.json") == nullptr);
    CHECK(findPackName(decoded, "game://src/client/Main.luauc") != nullptr);

    // Not a table: too short, another magic, a count that does not match what
    // follows, rows out of order.
    std::vector<PackName> none;
    CHECK_FALSE(decodePackNames(std::span<const std::byte>(encoded).first(8), none));
    std::vector<std::byte> other = encoded;
    other[0] = std::byte{'X'};
    CHECK_FALSE(decodePackNames(other, none));
    CHECK_FALSE(decodePackNames(std::span<const std::byte>(encoded).first(encoded.size() - 1), none));
    std::vector<std::byte> swapped = encoded;
    std::swap_ranges(swapped.begin() + 12, swapped.begin() + 52, swapped.begin() + 52);
    CHECK_FALSE(decodePackNames(swapped, none));
    CHECK(none.empty());
}

TEST_CASE("ADR 0183: a game is sealed into one pack, and read out of it as it was read off the disk")
{
    seedRealCatalog();
    const std::filesystem::path game = freshDir("game");
    const std::string scene = sceneText();
    const std::string material = "{\"format\":\"material\",\"color_map\":\"asset://textures/SENTINEL-base.png\"}";
    const std::vector<std::byte> texture = noise(30000, 21);

    // The folder `ludwerk build` leaves before it seals: a pack and its
    // manifest, the scripts compiled under their own names, the catalogues,
    // the settings.
    PackWriter writer;
    const ContentHash sceneHash = writer.addContent(AssetKind::Raw, bytesOf(scene));
    const ContentHash materialHash = writer.addContent(AssetKind::Material, bytesOf(material));
    const ContentHash textureHash = writer.addContent(AssetKind::Texture, texture);
    const std::vector<std::byte> built = writer.build();
    write(gamePackPath(game), textOf(built));
    write(gameManifestPath(game), "{\"format\":\"content-manifest\",\"version\":1,\"assets\":["
                                  "{\"urn\":\"asset://scenes/main.scene.json\",\"hash\":\"" +
                                      sceneHash.toHex() +
                                      "\",\"kind\":\"raw\",\"bytes\":1},"
                                      "{\"urn\":\"asset://materials/wood.material.json\",\"hash\":\"" +
                                      materialHash.toHex() +
                                      "\",\"kind\":\"material\",\"bytes\":1},"
                                      "{\"urn\":\"asset://textures/base.png\",\"hash\":\"" +
                                      textureHash.toHex() + "\",\"kind\":\"texture\",\"bytes\":1}]}");
    const std::string mainScript = std::string("\x06\x03", 2) + std::string(300, 'b') + "SENTINEL-CLIENT-STRING";
    const std::string serverScript = std::string("\x06\x03", 2) + std::string(200, 'c') + "SENTINEL-SERVER-STRING";
    const std::string catalog = "{\"hello\":\"SENTINEL-CATALOG Hello\",\"bye\":\"Goodbye, goodbye, goodbye, goodbye\"}";
    const std::string config = "[project]\nname = \"SENTINEL-PROJECT\"\n\n[window]\ntitle = \"Sealed\"\n";
    const std::string aliases = "{\"aliases\":{\"shared\":\"src/shared\"}}";
    write(game / "src/client/Main.luauc", mainScript);
    write(game / "src/client/Hud/init.luauc", mainScript + "2");
    write(game / "src/server/Rules.luauc", serverScript);
    write(game / "src/shared/Empty.luauc", "");
    write(game / "i18n/en.json", catalog);
    write(game / "i18n/pt-BR.json", catalog + " ");
    write(game / "project.toml", config);
    write(game / ".luaurc", aliases);
    // And what is not the game's own stays where it is: a streamed terrain's
    // cell, beside the pack.
    write(game / "content/terrain/main/cell_0_0.lterrain", "a cell");

    CHECK(SealedGame::open(game) == nullptr);

    SealReport report;
    REQUIRE_FALSE(sealGame(game, &report).has_value());
    CHECK(report.assets == 3);
    CHECK(report.files == 8);
    CHECK(report.bytesAfter > 0);

    // One pack, and none of what it took in.
    std::error_code ec;
    CHECK(std::filesystem::is_regular_file(gamePackPath(game), ec));
    CHECK_FALSE(std::filesystem::exists(gameManifestPath(game), ec));
    CHECK_FALSE(std::filesystem::exists(game / "src", ec));
    CHECK_FALSE(std::filesystem::exists(game / "i18n", ec));
    CHECK_FALSE(std::filesystem::exists(game / "project.toml", ec));
    CHECK_FALSE(std::filesystem::exists(game / ".luaurc", ec));
    CHECK(read(game / "content/terrain/main/cell_0_0.lterrain") == "a cell");

    // Nothing of the project is there to be read in it: not a path, not a
    // name, not a string of a script, a scene, a catalogue or the settings.
    const std::string sealed = read(gamePackPath(game));
    for (const std::string_view word : {"SENTINEL", "src/", "Main", "Rules", "i18n", "pt-BR", "project.toml", "luaurc",
                                        "asset://", "scenes/main", "wood.material", "base.png", "game://"}) {
        CAPTURE(word);
        CHECK(sealed.find(word) == std::string::npos);
    }

    // The game's own files, as the host asks for them.
    const std::shared_ptr<const SealedGame> opened = SealedGame::open(game);
    REQUIRE(opened != nullptr);
    CHECK(SealedGame::open(game) == opened);
    const std::vector<std::string> expected{".luaurc",
                                            "i18n/en.json",
                                            "i18n/pt-BR.json",
                                            "project.toml",
                                            "src/client/Hud/init.luauc",
                                            "src/client/Main.luauc",
                                            "src/server/Rules.luauc",
                                            "src/shared/Empty.luauc"};
    CHECK(std::vector<std::string>(opened->files().begin(), opened->files().end()) == expected);
    CHECK(opened->has("src/client/Main.luauc"));
    CHECK_FALSE(opened->has("src/client/Main.luau"));
    CHECK_FALSE(opened->has("src/client"));
    CHECK(opened->hasUnder("src/client"));
    CHECK(opened->hasUnder("src"));
    CHECK_FALSE(opened->hasUnder("src/scripts"));
    CHECK_FALSE(opened->hasUnder("src/cli"));
    CHECK(opened->filesUnder("src/client") ==
          std::vector<std::string>{"src/client/Hud/init.luauc", "src/client/Main.luauc"});
    CHECK(opened->filesUnder("i18n").size() == 2);
    std::string text;
    REQUIRE(opened->readText("src/client/Main.luauc", text));
    CHECK(text == mainScript);
    REQUIRE(opened->readText("project.toml", text));
    CHECK(text == config);
    REQUIRE(opened->readText("i18n/pt-BR.json", text));
    CHECK(text == catalog + " ");
    REQUIRE(opened->readText("src/shared/Empty.luauc", text));
    CHECK(text.empty());
    CHECK_FALSE(opened->readText("src/client/Missing.luauc", text));
    CHECK_FALSE(opened->pack().verify().has_value());

    // Its content, by the names the game has always used -- and no manifest.
    ContentMounts mounts;
    REQUIRE_FALSE(mounts.mountPack(gamePackPath(game)).has_value());
    const ResolvedContent resolvedScene = mounts.resolve("asset://scenes/main.scene.json");
    REQUIRE(resolvedScene.source == ResolvedContent::Source::Pack);
    CHECK(resolvedScene.kind == AssetKind::Raw);
    CHECK(textOf(resolvedScene.bytes) == scene);
    const ResolvedContent resolvedMaterial = mounts.resolve("asset://materials/wood.material.json");
    CHECK(resolvedMaterial.kind == AssetKind::Material);
    CHECK(textOf(resolvedMaterial.bytes) == material);
    const ResolvedContent resolvedTexture = mounts.resolve("asset://textures/base.png");
    CHECK(resolvedTexture.kind == AssetKind::Texture);
    CHECK(resolvedTexture.hash == textureHash);
    CHECK(resolvedTexture.bytes.size() == texture.size());
    CHECK_FALSE(mounts.resolve("asset://textures/other.png").found());
    CHECK(textOf(mounts.blob(materialHash)) == material);
    CHECK(textOf(mounts.named("game://project.toml")) == config);
    CHECK(mounts.named("game://nothing").empty());
    CHECK(mounts.packedUrns().empty());
    REQUIRE(mounts.packs().size() == 1);
    CHECK_FALSE(mounts.packs().front()->verify().has_value());
    mounts.clear();

    // Sealed once.
    const auto again = sealGame(game);
    REQUIRE(again.has_value());
    CHECK(again->message.find("sealed already") != std::string::npos);

    // And the way back gives every file as it was, and what else was text.
    const std::filesystem::path out = freshDir("unsealed");
    REQUIRE_FALSE(unsealGame(game, out).has_value());
    CHECK(read(out / "src/client/Main.luauc") == mainScript);
    CHECK(read(out / "src/server/Rules.luauc") == serverScript);
    CHECK(read(out / "i18n/en.json") == catalog);
    CHECK(read(out / "project.toml") == config);
    CHECK(read(out / ".luaurc") == aliases);
    CHECK(read(out / ".blobs" / sceneHash.toHex()) == scene);
    CHECK(read(out / ".blobs" / materialHash.toHex()) == material);
    CHECK_FALSE(std::filesystem::exists(out / ".blobs" / textureHash.toHex(), ec));
}

TEST_CASE("ADR 0183: a game with no content is sealed too, and one that is not sealed is read off the disk")
{
    seedRealCatalog();
    const std::filesystem::path game = freshDir("bare");
    write(game / "src/client/Main.luauc", "only a script");
    write(game / "project.toml", "[project]\nname = \"Bare\"\n");
    REQUIRE_FALSE(sealGame(game).has_value());

    const std::shared_ptr<const SealedGame> opened = SealedGame::open(game);
    REQUIRE(opened != nullptr);
    CHECK(opened->files().size() == 2);
    std::string text;
    REQUIRE(opened->readText("src/client/Main.luauc", text));
    CHECK(text == "only a script");

    // A pack that is not sealed is not a sealed game, and unsealing it says so.
    const std::filesystem::path loose = freshDir("loose");
    PackWriter writer;
    (void)writer.addContent(AssetKind::Raw, bytesOf("a scene"));
    write(gamePackPath(loose), textOf(writer.build()));
    CHECK(SealedGame::open(loose) == nullptr);
    CHECK(unsealGame(loose, freshDir("loose-out")).has_value());
    // And a folder with nothing in it at all is none.
    CHECK(SealedGame::open(freshDir("empty")) == nullptr);
}
