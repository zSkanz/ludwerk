// The faces a character falls back on, and where a line of text with no
// spaces may end (ADR 0259).
//
// The Latin face is the vendored Inter, which has Latin and Cyrillic and no
// ideographs. The other is made here: a real CJK face is tens of megabytes,
// and what these cases ask of one is that it has the characters and metrics
// unlike the Latin face's.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/ui/ui.h"

using engine::core::f32;
using engine::core::Rect;
using engine::core::Vec2;
using engine::ui::DrawQuad;
using engine::ui::glyphCacheStats;
using engine::ui::measureText;
using engine::ui::resetGlyphCache;
using engine::ui::TextLine;
using engine::ui::textLines;
using engine::ui::textWidth;

namespace {

using Bytes = std::vector<engine::core::u8>;

constexpr std::string_view Latin = "asset://fonts/latin.ttf";
constexpr std::string_view Squares = "asset://fonts/squares.ttf";

void put16(Bytes& out, int value)
{
    const auto bits = static_cast<std::uint16_t>(value);
    out.push_back(static_cast<engine::core::u8>(bits >> 8));
    out.push_back(static_cast<engine::core::u8>(bits & 0xFFu));
}

void put32(Bytes& out, std::uint32_t value)
{
    put16(out, static_cast<int>(value >> 16));
    put16(out, static_cast<int>(value & 0xFFFFu));
}

// **A TrueType face of one glyph**: a square, a full em wide, for every CJK
// punctuation mark, kana, unified ideograph and full-width form. 950 units of
// its em above the baseline and 50 below, where Inter has about 80 and 20 in a
// hundred: a line holding both reaches higher than either face alone.
Bytes makeSquareFace()
{
    struct Table
    {
        const char* tag;
        Bytes bytes;
    };
    Table cmap{"cmap", {}};
    Table glyf{"glyf", {}};
    Table head{"head", {}};
    Table hhea{"hhea", {}};
    Table hmtx{"hmtx", {}};
    Table loca{"loca", {}};
    Table maxp{"maxp", {}};

    // Format 13: every character of a group is the one glyph.
    static constexpr std::uint32_t Groups[][2] = {{0x3000, 0x30FF}, {0x4E00, 0x9FFF}, {0xFF01, 0xFF65}};
    put16(cmap.bytes, 0);
    put16(cmap.bytes, 1);
    put16(cmap.bytes, 3);
    put16(cmap.bytes, 10);
    put32(cmap.bytes, 12);
    put16(cmap.bytes, 13);
    put16(cmap.bytes, 0);
    put32(cmap.bytes, 16 + 12 * static_cast<std::uint32_t>(std::size(Groups)));
    put32(cmap.bytes, 0);
    put32(cmap.bytes, static_cast<std::uint32_t>(std::size(Groups)));
    for (const auto& group : Groups) {
        put32(cmap.bytes, group[0]);
        put32(cmap.bytes, group[1]);
        put32(cmap.bytes, 1);
    }

    // Glyph 1, one contour of four points on the curve, each a 16-bit step.
    put16(glyf.bytes, 1);
    for (const int bound : {100, 0, 900, 800})
        put16(glyf.bytes, bound);
    put16(glyf.bytes, 3);
    put16(glyf.bytes, 0);
    for (int point = 0; point < 4; ++point)
        glyf.bytes.push_back(0x01);
    for (const int x : {100, 800, 0, -800})
        put16(glyf.bytes, x);
    for (const int y : {0, 0, 800, 0})
        put16(glyf.bytes, y);
    while (glyf.bytes.size() % 4 != 0)
        glyf.bytes.push_back(0);
    put32(loca.bytes, 0);
    put32(loca.bytes, 0);
    put32(loca.bytes, static_cast<std::uint32_t>(glyf.bytes.size()));

    put32(head.bytes, 0x00010000);
    put32(head.bytes, 0x00010000);
    put32(head.bytes, 0);
    put32(head.bytes, 0x5F0F3CF5);
    put16(head.bytes, 0);
    put16(head.bytes, 1000);
    for (int word = 0; word < 8; ++word)
        put16(head.bytes, 0);
    for (const int bound : {0, 0, 1000, 950})
        put16(head.bytes, bound);
    put16(head.bytes, 0);
    put16(head.bytes, 8);
    put16(head.bytes, 2);
    put16(head.bytes, 1);
    put16(head.bytes, 0);

    put32(hhea.bytes, 0x00010000);
    for (const int value : {950, -50, 0, 1000, 0, 0, 1000, 1, 0, 0, 0, 0, 0, 0, 0, 2})
        put16(hhea.bytes, value);

    for (const int value : {500, 0, 1000, 100})
        put16(hmtx.bytes, value);
    put32(maxp.bytes, 0x00005000);
    put16(maxp.bytes, 2);

    // Tag order, each table on a four-byte boundary.
    const Table* tables[] = {&cmap, &glyf, &head, &hhea, &hmtx, &loca, &maxp};
    Bytes out;
    put32(out, 0x00010000);
    put16(out, static_cast<int>(std::size(tables)));
    for (int word = 0; word < 3; ++word)
        put16(out, 0);
    std::uint32_t offset = 12 + 16 * static_cast<std::uint32_t>(std::size(tables));
    for (const Table* table : tables) {
        for (int at = 0; at < 4; ++at)
            out.push_back(static_cast<engine::core::u8>(table->tag[at]));
        put32(out, 0);
        put32(out, offset);
        put32(out, static_cast<std::uint32_t>(table->bytes.size()));
        offset += static_cast<std::uint32_t>((table->bytes.size() + 3) / 4 * 4);
    }
    for (const Table* table : tables) {
        out.insert(out.end(), table->bytes.begin(), table->bytes.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

bool provide(void* user, std::string_view name, Bytes& out)
{
    (void)user;
    if (name == Squares) {
        out = makeSquareFace();
        return true;
    }
    if (name != Latin)
        return false;
    std::ifstream file(ENG_TEST_FONT, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    out.resize(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return !out.empty();
}

// The two faces, and the project's fallbacks a case names.
struct Faces
{
    explicit Faces(std::span<const std::string> fallbacks = {})
    {
        engine::ui::setFaceProvider(&provide, nullptr);
        engine::ui::setFontFallbacks(fallbacks);
    }
    ~Faces()
    {
        engine::ui::setFontFallbacks({});
        engine::ui::setFaceProvider(nullptr, nullptr);
    }
};

const std::vector<std::string> SquaresAfter{std::string(Squares)};

// The first and last character of a line.
[[nodiscard]] char32_t decodeAt(std::string_view text, std::size_t at)
{
    const auto byte = static_cast<unsigned char>(text[at]);
    if (byte < 0x80)
        return byte;
    const int length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
    char32_t codepoint = byte & (0x3Fu >> (length - 1));
    for (int index = 1; index < length; ++index)
        codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[at + static_cast<std::size_t>(index)]) & 0x3Fu);
    return codepoint;
}

[[nodiscard]] char32_t lastOf(std::string_view text)
{
    std::size_t at = text.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0u) == 0x80u)
        --at;
    return decodeAt(text, at);
}

// A paragraph wrapped at `width`: every line fits, and none begins or ends
// where the typesetting rules say it may not.
void checkWrapped(std::string_view text, f32 width)
{
    const std::vector<TextLine> lines = textLines(text, Latin, 16.0f, width);
    CHECK(lines.size() > 2);
    static constexpr std::u32string_view StartsNone = U"、。，．！？）」』ぁぃぅぇぉっゃゅょァィゥェォッャュョー";
    static constexpr std::u32string_view EndsNone = U"（「『";
    for (const TextLine& line : lines) {
        CHECK(line.width <= width * 1.0001f + 0.01f);
        REQUIRE(line.end > line.begin);
        const std::string_view run = text.substr(line.begin, line.end - line.begin);
        CHECK(StartsNone.find(decodeAt(run, 0)) == std::u32string_view::npos);
        CHECK(EndsNone.find(lastOf(run)) == std::u32string_view::npos);
    }
    // Nothing lost or doubled: the lines are the text, end to end.
    CHECK(lines.front().begin == 0);
    CHECK(lines.back().end == text.size());
    for (std::size_t at = 1; at < lines.size(); ++at)
        CHECK(lines[at].begin == lines[at - 1].end);
}

// Public domain (1905).
constexpr std::string_view Japanese =
    "吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。何でも薄暗いじめじめした所でニャーニャー泣いて"
    "いた事だけは記憶している。吾輩はここで始めて人間というものを見た。";
constexpr std::string_view Chinese =
    "我们在这里测试中文的换行。每一行都不应该超出边框，也不应该以标点符号开头！这样的排版才符合规范（也就是禁则）。";

} // namespace

TEST_CASE("ADR 0259: Latin, Cyrillic and CJK in one label draw no box")
{
    {
        // Without a fallback, the Latin face has no ideographs: a box each.
        Faces faces;
        resetGlyphCache();
        (void)measureText("Hello Привет 日本語", Latin, 16.0f, 0.0f);
        CHECK(glyphCacheStats().missingGlyphs == 3);
    }
    Faces faces(SquaresAfter);
    resetGlyphCache();
    const engine::ui::TextRunMetrics latin = measureText("Hello Привет", Latin, 16.0f, 0.0f);
    const engine::ui::TextRunMetrics mixed = measureText("Hello Привет 日本語", Latin, 16.0f, 0.0f);
    CHECK(glyphCacheStats().missingGlyphs == 0);
    // Each ideograph a full em: three more at 16 px, and a space.
    CHECK(mixed.size.x > latin.size.x + 3.0f * 15.0f);
    // The line reaches as high as the squares' face and as deep as Inter.
    CHECK(mixed.ascent > latin.ascent);
    CHECK(mixed.size.y > latin.size.y);
}

TEST_CASE("ADR 0259: a label in the label's own face alone measures what it did with no fallbacks")
{
    engine::ui::TextRunMetrics before;
    {
        Faces faces;
        before = measureText("Hamburgefonstiv", Latin, 18.0f, 0.0f);
    }
    Faces faces(SquaresAfter);
    const engine::ui::TextRunMetrics after = measureText("Hamburgefonstiv", Latin, 18.0f, 0.0f);
    CHECK(after.size.x == before.size.x);
    CHECK(after.size.y == before.size.y);
    CHECK(after.ascent == before.ascent);
}

TEST_CASE("ADR 0259: a label's own FallbackFont is tried with no project fallbacks")
{
    Faces faces;
    resetGlyphCache();
    const std::string_view font = engine::ui::labelFont(Latin, Squares);
    // One view for one pair, whoever asks.
    CHECK(engine::ui::labelFont(Latin, Squares).data() == font.data());
    CHECK(engine::ui::labelFont(Latin, {}) == Latin);
    (void)measureText("Привет 日本語", font, 16.0f, 0.0f);
    CHECK(glyphCacheStats().missingGlyphs == 0);
}

TEST_CASE("ADR 0259: a glyph from a fallback face stands on the same baseline as the label's own")
{
    Faces faces(SquaresAfter);
    resetGlyphCache();
    std::vector<DrawQuad> quads;
    engine::ui::buildTextGeometry("H日", Latin, 32.0f, 0.0f, Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 80.0f}}, 0, 0,
                                  engine::core::Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, quads);
    REQUIRE(quads.size() == 2);
    // The H and the square both sit on the baseline: their bottoms meet, to
    // the texel of padding either has.
    CHECK(std::fabs(quads[0].max.y - quads[1].max.y) <= 1.5f);
    // And the square's top is 800 of its 1000 units above it: the face that
    // drew it sized it, not the label's.
    CHECK((quads[1].max.y - quads[1].min.y) == doctest::Approx(32.0 * 0.8).epsilon(0.1));
}

TEST_CASE("ADR 0259: Japanese and Chinese paragraphs wrap inside the box, by the kinsoku rules")
{
    Faces faces(SquaresAfter);
    for (const f32 width : {100.0f, 133.0f, 161.0f, 240.0f}) {
        CAPTURE(width);
        checkWrapped(Japanese, width);
        checkWrapped(Chinese, width);
    }
}

TEST_CASE("ADR 0259: a full stop is not left to begin a line, and an opening bracket not to end one")
{
    Faces faces(SquaresAfter);
    // Five squares fit at 16 px.
    const f32 width = 5.0f * 16.0f + 1.0f;
    // The sixth character is the full stop: the ideograph before it goes
    // down with it.
    std::vector<TextLine> lines = textLines("一二三四五。六", Latin, 16.0f, width);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end == 12);
    // The fifth is the bracket: it goes down with what it opens.
    lines = textLines("一二三四「五六」", Latin, 16.0f, width);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end == 12);
    CHECK(lines[1].begin == 12);
}

TEST_CASE("ADR 0259: a zero-width space is a place to break, and a no-break space is not")
{
    Faces faces;
    resetGlyphCache();
    // Room for the first word and two letters of the second.
    std::vector<TextLine> lines = textLines("aaaa bbbb", Latin, 16.0f, textWidth("aaaa bb", Latin, 16.0f));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end == 4);

    const std::string_view held = "aaaa\xC2\xA0"
                                  "bbbb";
    lines = textLines(held, Latin, 16.0f,
                      textWidth("aaaa\xC2\xA0"
                                "bb",
                                Latin, 16.0f));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end > 6);

    const std::string_view broken = "aaaa\xE2\x80\x8B"
                                    "bbbb";
    lines = textLines(broken, Latin, 16.0f,
                      textWidth("aaaa\xE2\x80\x8B"
                                "bb",
                                Latin, 16.0f));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end == 7);
    CHECK(lines[1].begin == 7);
    // And it draws nothing, though the face has no glyph for it.
    CHECK(glyphCacheStats().missingGlyphs == 0);
}

TEST_CASE("ADR 0259: a Japanese line cut short ends with an ellipsis inside its box")
{
    Faces faces(SquaresAfter);
    const std::string cut = engine::ui::ellipsizedText(Japanese, Latin, 16.0f, 0.0f, Vec2{120.0f, 24.0f});
    CHECK(cut.size() < Japanese.size());
    CHECK(cut.ends_with("\xE2\x80\xA6"));
    CHECK(measureText(cut, Latin, 16.0f, 0.0f).size.x <= 120.0f);
}

TEST_CASE("ADR 0259: rich text in Japanese wraps by the same rules")
{
    Faces faces(SquaresAfter);
    resetGlyphCache();
    const std::string markup = "<b>吾輩は猫である。</b>" + std::string(Japanese.substr(24));
    const engine::ui::TextRunMetrics metrics = engine::ui::measureRichText(markup, Latin, 16.0f, 120.0f);
    CHECK(metrics.lineCount > 2);
    CHECK(metrics.size.x <= 120.0f + 0.01f);
    CHECK(glyphCacheStats().missingGlyphs == 0);
}

TEST_CASE("ADR 0259: letter spacing widens Latin text by its share of the size, and not CJK")
{
    Faces faces(SquaresAfter);
    resetGlyphCache();
    const f32 plain = textWidth("Hello", Latin, 20.0f);
    const std::string_view tracked = engine::ui::labelFont(Latin, {}, 0.1f);
    // Two px after each of five letters at 20 px.
    CHECK(textWidth("Hello", tracked, 20.0f) == doctest::Approx(static_cast<double>(plain + 10.0f)).epsilon(0.001));
    // Ideographs are set at their full width: no room is added after them.
    CHECK(textWidth("日本語", tracked, 20.0f) == doctest::Approx(static_cast<double>(textWidth("日本語", Latin, 20.0f))));
    // Negative draws closer, and a value past the clamp is the clamp.
    CHECK(textWidth("Hello", engine::ui::labelFont(Latin, {}, -0.05f), 20.0f) < plain);
    CHECK(engine::ui::labelFont(Latin, {}, 50.0f) == engine::ui::labelFont(Latin, {}, 2.0f));
    // It shares the glyphs of the label without it.
    const auto fills = glyphCacheStats().fills;
    (void)measureText("Hello", tracked, 20.0f, 0.0f);
    CHECK(glyphCacheStats().fills == fills);
}

TEST_CASE("ADR 0259: wrapping, ellipsis and rich text measure with the letter spacing")
{
    Faces faces(SquaresAfter);
    const std::string_view tracked = engine::ui::labelFont(Latin, {}, 0.2f);
    constexpr std::string_view Words = "spread letters wrap sooner than tight ones do";
    const f32 width = textWidth("spread letters", Latin, 16.0f) + 1.0f;
    const std::vector<TextLine> tight = textLines(Words, Latin, 16.0f, width);
    const std::vector<TextLine> spread = textLines(Words, tracked, 16.0f, width);
    CHECK(spread.size() > tight.size());
    for (const TextLine& line : spread)
        CHECK(line.width <= width * 1.0001f + 0.01f);
    const std::string cut = engine::ui::ellipsizedText(Words, tracked, 16.0f, 0.0f, Vec2{100.0f, 24.0f});
    CHECK(measureText(cut, tracked, 16.0f, 0.0f).size.x <= 100.0f);
    CHECK(engine::ui::measureRichText("<b>spread</b>", tracked, 16.0f, 0.0f).size.x >
          engine::ui::measureRichText("<b>spread</b>", Latin, 16.0f, 0.0f).size.x + 6.0f * 16.0f * 0.2f - 0.5f);
}
