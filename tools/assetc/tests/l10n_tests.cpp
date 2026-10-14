// The localization index (ADR 0200 §2 and §7): what a content tree holds in
// each language, and how long each localized sound is in its longest.
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assetc/l10n.h"
#include "engine/audio/audio.h"
#include "engine/core/types.h"

using namespace engine::assetc;
using engine::core::u16;
using engine::core::u32;
using engine::core::u64;

namespace {

[[nodiscard]] std::filesystem::path freshDir(const char* name)
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "engine-assetc-l10n-tests" / name;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

[[nodiscard]] std::string read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put(std::string& out, u32 value, int width)
{
    for (int index = 0; index < width; ++index)
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
}

// A mono 16-bit PCM `.wav` of `frames` frames at `rate`: the smallest file a
// decoder says a length for.
[[nodiscard]] std::string wav(u32 rate, u32 frames)
{
    const u32 dataBytes = frames * 2;
    std::string out = "RIFF";
    put(out, 36 + dataBytes, 4);
    out += "WAVEfmt ";
    put(out, 16, 4);
    put(out, 1, 2); // PCM
    put(out, 1, 2); // one channel
    put(out, rate, 4);
    put(out, rate * 2, 4);
    put(out, 2, 2);
    put(out, 16, 2);
    out += "data";
    put(out, dataBytes, 4);
    for (u32 frame = 0; frame < frames; ++frame)
        put(out, static_cast<u16>((frame * 37u) & 0x7FFu), 2);
    return out;
}

// What the engine says the file is, asked of the engine.
[[nodiscard]] u64 probed(const std::string& bytes)
{
    const std::optional<u64> frames =
        engine::audio::detail::probeFrames({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
    REQUIRE(frames.has_value());
    return *frames;
}

} // namespace

TEST_CASE("ADR 0200: what is a voice is told by its ending, in any case")
{
    CHECK(isSoundFile("voice/intro_01.ogg"));
    CHECK(isSoundFile("voice/intro_01.WAV"));
    CHECK(isSoundFile("a.Mp3"));
    CHECK(isSoundFile("a.flac"));
    CHECK_FALSE(isSoundFile("ui/logo.png"));
    CHECK_FALSE(isSoundFile("voice/intro_01.ogg.txt"));
    CHECK_FALSE(isSoundFile("ogg"));
}

TEST_CASE("ADR 0200: a content tree's voice languages, its localized sounds and its lines files")
{
    const std::filesystem::path root = freshDir("scan");
    write(root / "voice/act1/intro_01.wav", wav(48000, 100));
    write(root / "voice/act1/only_default.wav", wav(48000, 100));
    write(root / "l10n/pt-BR/voice/act1/intro_01.wav", wav(48000, 100));
    write(root / "l10n/ja/voice/act1/intro_01.wav", wav(48000, 100));
    // A line the default language was never recorded in.
    write(root / "l10n/ja/voice/act1/extra.OGG", "not measured by a scan");
    // A language with pictures and no sound is not a voice language, and a
    // picture is never a voice.
    write(root / "l10n/de/ui/logo.png", "a picture");
    write(root / "l10n/pt-BR/ui/logo.png", "a picture");
    // A file at `l10n/` itself belongs to no language.
    write(root / "l10n/stray.ogg", "nobody's");
    write(root / "dialogue/act1.lines.json", "{}");
    write(root / "dialogue/act0.lines.json", "{}");
    write(root / "dialogue/notes.json", "{}");
    write(root / "dialogue/cut/act9.lines.json", "{}");
    write(root / "dialogue/.lines.json", "{}");

    L10nScan scan;
    std::string diagnostic;
    REQUIRE_MESSAGE(scanL10n(root, scan, diagnostic), diagnostic);
    CHECK(scan.voice == std::vector<std::string>{"ja", "pt-BR"});
    REQUIRE(scan.sounds.size() == 2);
    CHECK(scan.sounds[0].path == "voice/act1/extra.OGG");
    CHECK(scan.sounds[0].files == std::vector<std::string>{"l10n/ja/voice/act1/extra.OGG"});
    CHECK(scan.sounds[1].path == "voice/act1/intro_01.wav");
    CHECK(scan.sounds[1].files == std::vector<std::string>{"voice/act1/intro_01.wav", "l10n/ja/voice/act1/intro_01.wav",
                                                           "l10n/pt-BR/voice/act1/intro_01.wav"});
    CHECK(scan.lines == std::vector<std::string>{"dialogue/act0.lines.json", "dialogue/act1.lines.json"});
    CHECK_FALSE(scan.empty());

    // Nothing localized and no lines is nothing to say; no folder is an error.
    L10nScan bare;
    REQUIRE(scanL10n(freshDir("scan-bare"), bare, diagnostic));
    CHECK(bare.empty());
    CHECK_FALSE(scanL10n(root / "missing", bare, diagnostic));
    CHECK(diagnostic.find("missing") != std::string::npos);
}

TEST_CASE("ADR 0200: the index holds each localized sound's longest language, as the engine measures it")
{
    const std::filesystem::path root = freshDir("index");
    // One line, three lengths, two rates: the default at 48 kHz, one language
    // shorter, and the longest recorded at another rate.
    const std::string english = wav(48000, 9600);
    const std::string portuguese = wav(48000, 4800);
    const std::string japanese = wav(24000, 7200);
    write(root / "voice/act1/intro_01.wav", english);
    write(root / "l10n/pt-BR/voice/act1/intro_01.wav", portuguese);
    write(root / "l10n/ja/voice/act1/intro_01.wav", japanese);
    // A second, where the default is the longest.
    write(root / "voice/act1/intro_02.wav", wav(48000, 24000));
    write(root / "l10n/pt-BR/voice/act1/intro_02.wav", wav(44100, 4410));
    // A third the default language has no file for.
    write(root / "l10n/ja/voice/act1/aside.wav", wav(48000, 1234));
    // And one that is in the default language only: not localized, not listed.
    write(root / "voice/act1/only_default.wav", wav(48000, 100));
    write(root / "l10n/pt-BR/ui/logo.png", "a picture");
    write(root / "dialogue/act1.lines.json", "{}");

    // The numbers are the probe's: at the mixer's rate, whatever the file's.
    CHECK(probed(english) == 9600);
    CHECK(probed(portuguese) == 4800);
    const u64 japaneseFrames = probed(japanese);
    CHECK(japaneseFrames >= 14400);
    CHECK(japaneseFrames <= 14401);

    const L10nResult all = buildL10nIndex(root, std::nullopt);
    REQUIRE_MESSAGE(all.ok, all.diagnostic);
    const std::string expected = "{\n"
                                 "  \"version\": 1,\n"
                                 "  \"voice\": [\"ja\", \"pt-BR\"],\n"
                                 "  \"shipped\": [\"ja\", \"pt-BR\"],\n"
                                 "  \"lengths\": {\n"
                                 "    \"voice/act1/aside.wav\": 1234,\n"
                                 "    \"voice/act1/intro_01.wav\": " +
                                 std::to_string(japaneseFrames) +
                                 ",\n"
                                 "    \"voice/act1/intro_02.wav\": 24000\n"
                                 "  },\n"
                                 "  \"lines\": [\"dialogue/act1.lines.json\"]\n"
                                 "}\n";
    CHECK(all.json == expected);
    // The same tree, the same bytes.
    CHECK(buildL10nIndex(root, std::nullopt).json == all.json);

    // What ships in the game's own pack is the caller's to say: a name that
    // is no voice language -- the default's, which always ships -- is not one
    // of them, and an empty list is none.
    const L10nResult some = buildL10nIndex(root, std::vector<std::string>{"en", "pt-BR", "zz"});
    REQUIRE(some.ok);
    CHECK(some.json.find("\"voice\": [\"ja\", \"pt-BR\"]") != std::string::npos);
    CHECK(some.json.find("\"shipped\": [\"pt-BR\"]") != std::string::npos);
    // The lengths are the same whatever ships: they are the project's.
    CHECK(some.json.find("\"voice/act1/intro_01.wav\": " + std::to_string(japaneseFrames)) != std::string::npos);
    const L10nResult none = buildL10nIndex(root, std::vector<std::string>{});
    REQUIRE(none.ok);
    CHECK(none.json.find("\"shipped\": []") != std::string::npos);

    // Each file's own length, for the report.
    const L10nResult each = measureL10n(root);
    REQUIRE(each.ok);
    const std::string files = "{\n"
                              "  \"version\": 1,\n"
                              "  \"rate\": 48000,\n"
                              "  \"files\": {\n"
                              "    \"l10n/ja/voice/act1/aside.wav\": 1234,\n"
                              "    \"l10n/ja/voice/act1/intro_01.wav\": " +
                              std::to_string(japaneseFrames) +
                              ",\n"
                              "    \"l10n/pt-BR/voice/act1/intro_01.wav\": 4800,\n"
                              "    \"l10n/pt-BR/voice/act1/intro_02.wav\": " +
                              std::to_string(probed(wav(44100, 4410))) +
                              ",\n"
                              "    \"voice/act1/intro_01.wav\": 9600,\n"
                              "    \"voice/act1/intro_02.wav\": 24000\n"
                              "  },\n"
                              "  \"unmeasured\": []\n"
                              "}\n";
    CHECK(each.json == files);
}

TEST_CASE("ADR 0200: a real Ogg Vorbis line is measured as the engine measures it")
{
    const std::string tone = read(std::filesystem::path(ENG_AUDIO_TEST_DATA) / "tone.ogg");
    REQUIRE_FALSE(tone.empty());
    const u64 toneFrames = probed(tone);

    const std::filesystem::path root = freshDir("ogg");
    write(root / "voice/tone.ogg", wav(48000, 10));
    write(root / "l10n/fr/voice/tone.ogg", tone);
    const L10nResult result = buildL10nIndex(root, std::nullopt);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
    REQUIRE(toneFrames > 10);
    CHECK(result.json.find("\"voice/tone.ogg\": " + std::to_string(toneFrames)) != std::string::npos);
}

TEST_CASE("ADR 0200: a file that cannot be measured fails the index and is named; a tree with nothing gets none")
{
    const std::filesystem::path root = freshDir("unmeasured");
    write(root / "voice/intro.wav", wav(48000, 480));
    write(root / "l10n/pt-BR/voice/intro.wav", "RIFF but not a sound at all");
    const L10nResult result = buildL10nIndex(root, std::nullopt);
    CHECK_FALSE(result.ok);
    CHECK(result.json.empty());
    CHECK(result.diagnostic.find("l10n/pt-BR/voice/intro.wav") != std::string::npos);

    // The report's reading lists it and goes on.
    const L10nResult each = measureL10n(root);
    REQUIRE(each.ok);
    CHECK(each.json.find("\"voice/intro.wav\": 480") != std::string::npos);
    CHECK(each.json.find("\"unmeasured\": [\"l10n/pt-BR/voice/intro.wav\"]") != std::string::npos);

    // No language and no lines: nothing is written, and that is not a failure.
    const std::filesystem::path bare = freshDir("bare");
    write(bare / "scenes/main.scene.json", "{}");
    write(bare / "l10n/de/ui/logo.png", "a picture");
    const L10nResult nothing = buildL10nIndex(bare, std::nullopt);
    CHECK(nothing.ok);
    CHECK(nothing.json.empty());

    // Lines and no language: an index of the lines.
    write(bare / "dialogue/act1.lines.json", "{}");
    const L10nResult lines = buildL10nIndex(bare, std::nullopt);
    REQUIRE(lines.ok);
    CHECK(lines.json == "{\n  \"version\": 1,\n  \"voice\": [],\n  \"shipped\": [],\n  \"lengths\": {},\n"
                        "  \"lines\": [\"dialogue/act1.lines.json\"]\n}\n");
}
