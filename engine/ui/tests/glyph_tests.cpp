// The glyph store (M6, human decision 2026-08-21: "a cache, not a bake").
//
// These cases are about the SHAPE of the store rather than about how a letter
// looks, and the shape is the decision: keyed by face, size and codepoint,
// filled on demand, bounded, and with a chosen answer for a codepoint the face
// cannot draw. When M7 hands over a real face, these are the cases that say the
// key did not have to change.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ui/glyph_outline.h"
#include "engine/ui/ui.h"

using engine::core::Color3;
using engine::core::f32;
using engine::core::Rect;
using engine::core::Vec2;
using engine::ui::buildTextGeometry;
using engine::ui::DrawQuad;
using engine::ui::GlyphAtlas;
using engine::ui::glyphAtlas;
using engine::ui::glyphCacheStats;
using engine::ui::measureText;
using engine::ui::resetGlyphCache;

namespace {

[[nodiscard]] std::vector<DrawQuad> quadsOf(std::string_view text, std::string_view font = {}, f32 size = 12.0f)
{
    std::vector<DrawQuad> out;
    buildTextGeometry(text, font, size, 0.0f, Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 40.0f}}, 0, 0,
                      Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, out);
    return out;
}

} // namespace

TEST_CASE("the store fills on demand and answers a repeat from what it has")
{
    resetGlyphCache();
    (void)measureText("abc", {}, 12.0f, 0.0f);
    const engine::core::u64 afterFirst = glyphCacheStats().fills;
    CHECK(afterFirst == 3);

    // The property that makes this a cache rather than a bake, and the one a
    // second frame of a static label depends on: asking again costs nothing.
    (void)measureText("abc", {}, 12.0f, 0.0f);
    CHECK(glyphCacheStats().fills == afterFirst);
    CHECK(glyphCacheStats().hits >= 3);
}

TEST_CASE("H9: text is broken into lines once -- a label measured, moved and drawn again asks the cache")
{
    // A screen laid out again because one damage number moved measured every
    // label on it, every frame.
    resetGlyphCache();
    const char* const words = "the quick brown fox jumps over the lazy dog";
    const auto first = measureText(words, {}, 12.0f, 60.0f);
    CHECK(glyphCacheStats().lineMisses == 1);
    CHECK(glyphCacheStats().lineHits == 0);
    const auto second = measureText(words, {}, 12.0f, 60.0f);
    CHECK(glyphCacheStats().lineHits == 1);
    CHECK(second.lineCount == first.lineCount);
    CHECK(second.size.x == first.size.x);
    // Drawn, at the same size and width: the same lines.
    std::vector<DrawQuad> out;
    buildTextGeometry(words, {}, 12.0f, 60.0f, Rect{Vec2{0.0f, 0.0f}, Vec2{60.0f, 200.0f}}, 0, 0,
                      Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, out);
    CHECK(glyphCacheStats().lineHits == 2);
    // Another width is another question.
    (void)measureText(words, {}, 12.0f, 90.0f);
    CHECK(glyphCacheStats().lineMisses == 2);

    // **And wrapping is the same answer it was**, line for line: every line
    // fits, and none could have taken the next word.
    CHECK(first.lineCount > 1);
    CHECK(first.size.x <= 60.0f);
}

TEST_CASE("G6: a frame whose text filled the glyph store is built again against the store it left")
{
    // The FPS game: name tags filled the store mid-frame, it was cleared, and
    // every label built before the clear drew with places in an atlas that
    // were no longer its glyphs -- garbage until the next frame, and every
    // frame while the clears went on.
    resetGlyphCache();
    int builds = 0;
    engine::core::u64 clearsInLast = 0;
    engine::ui::buildWithSettledGlyphs([&] {
        ++builds;
        const engine::core::u64 before = glyphCacheStats().clears;
        (void)quadsOf("HUD");
        // The first build asks for more glyphs than the store holds: sixteen
        // thousand characters the face has none of, each its own box.
        if (builds == 1) {
            for (unsigned codepoint = 0x800; codepoint < 0x800 + 16500; ++codepoint) {
                const char encoded[4] = {static_cast<char>(0xE0u | (codepoint >> 12)),
                                         static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)),
                                         static_cast<char>(0x80u | (codepoint & 0x3Fu)), '\0'};
                (void)measureText(encoded, {}, 12.0f, 0.0f);
            }
        }
        clearsInLast = glyphCacheStats().clears - before;
    });
    CHECK(builds == 2);
    // The second build cleared nothing: what it drew is in the atlas it left.
    CHECK(clearsInLast == 0);

    // A frame that cleared nothing is built once.
    builds = 0;
    engine::ui::buildWithSettledGlyphs([&] {
        ++builds;
        (void)quadsOf("HUD");
    });
    CHECK(builds == 1);
}

TEST_CASE("the built-in face is one set of glyphs at every size")
{
    // A vector glyph scales by multiplication: the size was in its key all the
    // same, and a label animated through a hundred sizes made a hundred copies
    // of the same rectangles.
    resetGlyphCache();
    (void)measureText("a", {}, 12.0f, 0.0f);
    (void)measureText("a", {}, 24.0f, 0.0f);
    (void)measureText("a", {}, 37.3f, 0.0f);
    CHECK(glyphCacheStats().fills == 1);
    CHECK(glyphCacheStats().entries == 1);
    // And it is as wide as its size says.
    CHECK(measureText("a", {}, 24.0f, 0.0f).size.x ==
          doctest::Approx(static_cast<double>(2.0f * measureText("a", {}, 12.0f, 0.0f).size.x)));
}

TEST_CASE("two names that resolve to the same face share its glyphs")
{
    // Changed at M7, when `Font` stopped being `Inert` and started SELECTING a
    // face. Two names neither of which can be resolved both fall back to the
    // default, and two names for one face should share one set of glyphs --
    // caching them separately would rasterise the same face twice and spend an
    // atlas on it. Which face a name resolves to is what varies the key now.
    resetGlyphCache();
    (void)measureText("a", "asset://fonts/one.ttf", 12.0f, 0.0f);
    (void)measureText("a", "asset://fonts/two.ttf", 12.0f, 0.0f);
    CHECK(glyphCacheStats().fills == 1);

    // Quantized, because a `TextSize` a tween is animating would otherwise mint
    // an entry per frame -- and a quarter of a pixel is below what any face
    // resolves.
    const engine::core::u64 before = glyphCacheStats().fills;
    (void)measureText("a", "asset://fonts/one.ttf", 12.05f, 0.0f);
    CHECK(glyphCacheStats().fills == before);
}

TEST_CASE("a codepoint the face cannot draw becomes a visible box")
{
    // The decision stated where it is read: a missing glyph that drew NOTHING
    // would make a Portuguese label silently lose its accents, and one that drew
    // `?` would be indistinguishable from a question mark somebody typed.
    resetGlyphCache();
    const std::vector<DrawQuad> boxes = quadsOf("ç");
    CHECK(glyphCacheStats().missingGlyphs == 1);
    // Four sides.
    CHECK(boxes.size() == 4);
    for (const DrawQuad& quad : boxes) {
        CHECK(quad.max.x > quad.min.x);
        CHECK(quad.max.y > quad.min.y);
    }
}

TEST_CASE("UTF-8 is decoded, so an accented word is one glyph per letter")
{
    // The failure this prevents is mojibake: reading the bytes one at a time
    // would make `ç` two glyphs, both of them boxes, and a label twice as wide
    // as it should be.
    resetGlyphCache();
    (void)measureText("ação", {}, 12.0f, 0.0f);
    // a, ç, ã, o -- four codepoints from six bytes, two of them missing, and
    // four entries because a missing glyph is cached under the character that
    // was ASKED for. Sharing one entry for every missing character would make
    // the counter say "1" for a label that is entirely boxes.
    CHECK(glyphCacheStats().fills == 4);
    CHECK(glyphCacheStats().missingGlyphs == 2);
}

TEST_CASE("an invalid byte advances by one and does not run off the end")
{
    // A truncated sequence is what a string cut mid-character looks like, and a
    // decoder that trusted the length byte would read past the view.
    resetGlyphCache();
    const std::string truncated = "a\xC3";
    const engine::ui::TextRunMetrics metrics = measureText(truncated, {}, 12.0f, 0.0f);
    CHECK(metrics.size.x > 0.0f);
    CHECK(glyphCacheStats().missingGlyphs == 1);

    const std::string stray = "\x80\x80";
    (void)measureText(stray, {}, 12.0f, 0.0f);
    // Still one: every byte sequence that is not a character decodes to the same
    // "not text" codepoint and shares one entry, because that is one fact
    // however many times it happens. A missing LETTER is counted per letter.
    CHECK(glyphCacheStats().missingGlyphs == 1);
}

TEST_CASE("measurement and drawing agree about how wide a character is")
{
    // Both go through the same cached advance, which is what makes this true by
    // construction rather than by two implementations happening to match. A
    // centred label whose measure disagreed with its draw sits visibly off.
    resetGlyphCache();
    const engine::ui::TextRunMetrics metrics = measureText("Hello", {}, 12.0f, 0.0f);
    const std::vector<DrawQuad> quads = quadsOf("Hello");
    REQUIRE_FALSE(quads.empty());

    f32 rightmost = 0.0f;
    for (const DrawQuad& quad : quads)
        rightmost = std::fmax(rightmost, quad.max.x);
    // The drawn ink ends inside the measured advance -- a glyph's cell is wider
    // than its ink, and it must never be narrower.
    CHECK(rightmost <= metrics.size.x + 0.001f);
    CHECK(rightmost > metrics.size.x * 0.5f);
}

TEST_CASE("a missing glyph still advances the pen")
{
    // Otherwise every letter after the accent piles up on the same column, which
    // is a worse failure than the box itself.
    resetGlyphCache();
    const engine::ui::TextRunMetrics plain = measureText("aa", {}, 12.0f, 0.0f);
    const engine::ui::TextRunMetrics accented = measureText("aça", {}, 12.0f, 0.0f);
    CHECK(accented.size.x > plain.size.x);
}

TEST_CASE("wrapping breaks on codepoints, never inside one")
{
    // A break landing inside a multi-byte character would produce two invalid
    // sequences out of one valid one -- two boxes where there was a letter.
    resetGlyphCache();
    const engine::ui::TextRunMetrics wrapped = measureText("ção ção ção", {}, 12.0f, 30.0f);
    CHECK(wrapped.lineCount > 1);
    // Three distinct codepoints the face cannot draw, filled once each: if a
    // break had split one, there would be more.
    CHECK(glyphCacheStats().missingGlyphs == 2);
}

// ---------------------------------------------------------------------------
// The real face (roadmap M7, human decision 2026-08-20: Inter, OFL 1.1).

namespace {

// Reads the vendored font straight off disk. The face provider is how a project
// supplies a font at runtime; here it is how a test supplies one without a
// content system.
bool provideTestFace(void* user, std::string_view name, std::vector<engine::core::u8>& out)
{
    (void)user;
    if (name != "asset://fonts/test.ttf") {
        return false;
    }
    std::ifstream file(ENG_TEST_FONT, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    out.resize(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return !out.empty();
}

struct FaceGuard
{
    FaceGuard() { engine::ui::setFaceProvider(&provideTestFace, nullptr); }
    ~FaceGuard() { engine::ui::setFaceProvider(nullptr, nullptr); }
};

} // namespace

TEST_CASE("a real face rasterises into the atlas and measures by its own metrics")
{
    FaceGuard guard;

    // Nothing rasterised yet, so there is no atlas. A build with no font stays
    // in this state forever and still draws text with the built-in face, which
    // is the property the fallback exists for.
    resetGlyphCache();
    CHECK(glyphAtlas().pixels.empty());

    const engine::ui::TextRunMetrics metrics = measureText("Hamburgefonstiv", "asset://fonts/test.ttf", 32.0f, 0.0f);
    CHECK(metrics.lineCount == 1);
    CHECK(metrics.size.x > 0.0f);
    // The line height is the FACE's own: `stbtt_ScaleForPixelHeight` makes
    // ascent-minus-descent exactly the requested size, and the line gap is added
    // on top -- so a face with no gap measures exactly 32 and one with a gap
    // measures more. Either is right; a constant twelve would not be.
    CHECK(metrics.size.y >= 32.0f);
    CHECK(metrics.size.y < 64.0f);
    CHECK(metrics.ascent > 0.0f);
    CHECK(metrics.ascent < metrics.size.y);

    const GlyphAtlas atlas = glyphAtlas();
    REQUIRE_FALSE(atlas.pixels.empty());
    CHECK(atlas.width == 1024);
    CHECK(atlas.height == 1024);
    CHECK(atlas.version > 0);
}

TEST_CASE("the rasterised face is Regular weight rather than the thinnest master")
{
    // **This is the case that justifies vendoring a VARIABLE font.** Upstream
    // ships no static TTF at the pinned tag, and stb_truetype knows nothing about
    // variation axes -- it draws the outlines in `glyf`, which for a variable
    // font is one particular master. If that master were Thin, every label in
    // the engine would be hairline and nobody would know why.
    //
    // Measured as INK COVERAGE of a capital H at 64 px: a Regular H covers
    // roughly a tenth of its own box, a Thin one a twentieth, a Black one nearly
    // a fifth. The band below is wide enough to survive a hinting change and
    // narrow enough to catch the wrong master.
    FaceGuard guard;
    resetGlyphCache();

    std::vector<engine::ui::DrawQuad> quads;
    buildTextGeometry("H", "asset://fonts/test.ttf", 64.0f, 0.0f, engine::core::Rect{{0.0f, 0.0f}, {200.0f, 200.0f}}, 0,
                      0, engine::core::Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, quads);
    REQUIRE(quads.size() == 1);
    // Texture 1 is the glyph atlas: a rasterised glyph SAMPLES rather than being
    // a solid rectangle, which is the whole difference from the built-in face.
    CHECK(quads[0].texture == 1);
    CHECK(quads[0].uvMax.x > quads[0].uvMin.x);
    CHECK(quads[0].uvMax.y > quads[0].uvMin.y);

    const GlyphAtlas atlas = glyphAtlas();
    REQUIRE_FALSE(atlas.pixels.empty());

    const auto x0 = static_cast<engine::core::usize>(quads[0].uvMin.x * static_cast<float>(atlas.width) + 0.5f);
    const auto y0 = static_cast<engine::core::usize>(quads[0].uvMin.y * static_cast<float>(atlas.height) + 0.5f);
    const auto x1 = static_cast<engine::core::usize>(quads[0].uvMax.x * static_cast<float>(atlas.width) + 0.5f);
    const auto y1 = static_cast<engine::core::usize>(quads[0].uvMax.y * static_cast<float>(atlas.height) + 0.5f);
    REQUIRE(x1 > x0);
    REQUIRE(y1 > y0);

    double ink = 0.0;
    for (engine::core::usize y = y0; y < y1; ++y) {
        for (engine::core::usize x = x0; x < x1; ++x) {
            ink += static_cast<double>(atlas.pixels[y * atlas.width + x]) / 255.0;
        }
    }
    const double coverage = ink / static_cast<double>((x1 - x0) * (y1 - y0));

    // Measured against the glyph's own TIGHT box rather than the em box, which
    // is what the atlas rectangle is -- so the numbers are much higher than an
    // em-relative stem width would suggest. An H is two stems and a crossbar:
    // for Regular that is a bit under two fifths of its bounding box, for Thin
    // around an eighth, and for Black close to two thirds.
    CHECK(coverage > 0.28);
    CHECK(coverage < 0.50);
}

TEST_CASE("scaled text steps through a few sizes, never past what fits, and stops at 100 px")
{
    // A label whose handles are dragged asks for the size that fills it on
    // every frame. On a ladder, a drag from 10 px to 300 px is a few dozen
    // sizes in the glyph cache rather than one per frame (the flood reported).
    std::set<float> sizes;
    for (float fits = 10.0f; fits <= 300.0f; fits += 0.37f) {
        const float size = engine::ui::scaledTextSize(fits);
        CHECK(size <= std::fmax(fits, 1.0f));
        CHECK(size <= engine::ui::kMaxScaledTextSize);
        sizes.insert(size);
    }
    CHECK(sizes.size() < 50);
    CHECK(engine::ui::scaledTextSize(17.9f) == 17.0f);
    CHECK(engine::ui::scaledTextSize(47.0f) == 46.0f);
    CHECK(engine::ui::scaledTextSize(99.0f) == 96.0f);
    CHECK(engine::ui::scaledTextSize(640.0f) == 100.0f);
    CHECK(engine::ui::scaledTextSize(0.2f) == 1.0f);
}

TEST_CASE("a label whose size is animated costs a few sets of glyphs, not one a quarter pixel")
{
    // **What a damage number does**: its `TextSize` popped from 14 to 40 and
    // back, and a name over a head in the world is a different size at every
    // distance. A raster glyph was cached at the size it was asked for, to the
    // quarter pixel: a hundred and five sizes of ten digits, and the store
    // filled and was emptied -- every glyph of every label rasterised again,
    // and a warning in the log.
    FaceGuard guard;
    resetGlyphCache();
    for (float size = 14.0f; size <= 40.0f; size += 0.25f)
        (void)measureText("0123456789", "asset://fonts/test.ttf", size, 0.0f);
    const engine::core::u64 fills = glyphCacheStats().fills;
    MESSAGE("a hundred and five sizes of ten digits: ", fills, " glyphs rasterised");
    // Twenty-seven: every whole pixel from 14 to 40, each the rung of the
    // sizes just under it.
    CHECK(fills <= 10u * 30u);
    CHECK(glyphCacheStats().clears == 0);

    // The store that filled: every digit at every quarter pixel to 300.
    for (float size = 8.0f; size <= 300.0f; size += 0.25f)
        (void)measureText("0123456789", "asset://fonts/test.ttf", size, 0.0f);
    CHECK(glyphCacheStats().clears == 0);

    // Past 64 a whole pixel is not its own rung: a title is made at the rung
    // above it and drawn down.
    resetGlyphCache();
    (void)measureText("a", "asset://fonts/test.ttf", 70.0f, 0.0f);
    (void)measureText("a", "asset://fonts/test.ttf", 77.5f, 0.0f);
    (void)measureText("a", "asset://fonts/test.ttf", 80.0f, 0.0f);
    CHECK(glyphCacheStats().fills == 1);
    CHECK(measureText("a", "asset://fonts/test.ttf", 70.0f, 0.0f).size.x ==
          doctest::Approx(
              static_cast<double>(measureText("a", "asset://fonts/test.ttf", 80.0f, 0.0f).size.x * 70.0f / 80.0f))
              .epsilon(0.001));
}

TEST_CASE("text between two rasterised sizes is the larger one made smaller, and measures as its own size")
{
    FaceGuard guard;
    resetGlyphCache();
    // A whole pixel is rasterised at itself: text at a size somebody chose is
    // drawn as it always was.
    const float exact = measureText("Hamburgefonstiv", "asset://fonts/test.ttf", 23.0f, 0.0f).size.x;
    const engine::core::u64 afterExact = glyphCacheStats().fills;
    // Half a pixel smaller takes the same glyphs and scales them: nothing new
    // is rasterised, and the line is as much narrower as the size is smaller.
    const float between = measureText("Hamburgefonstiv", "asset://fonts/test.ttf", 22.5f, 0.0f).size.x;
    CHECK(glyphCacheStats().fills == afterExact);
    CHECK(between == doctest::Approx(static_cast<double>(exact * 22.5f / 23.0f)).epsilon(0.001));
    CHECK(measureText("Hamburgefonstiv", "asset://fonts/test.ttf", 22.5f, 0.0f).size.y ==
          doctest::Approx(
              static_cast<double>(measureText("Hamburgefonstiv", "asset://fonts/test.ttf", 23.0f, 0.0f).size.y * 22.5f /
                                  23.0f))
              .epsilon(0.001));

    // And it is drawn that much smaller: the same quads, scaled.
    const std::vector<DrawQuad> whole = quadsOf("H", "asset://fonts/test.ttf", 23.0f);
    const std::vector<DrawQuad> part = quadsOf("H", "asset://fonts/test.ttf", 22.5f);
    REQUIRE(whole.size() == 1);
    REQUIRE(part.size() == 1);
    CHECK(part[0].max.y - part[0].min.y ==
          doctest::Approx(static_cast<double>((whole[0].max.y - whole[0].min.y) * 22.5f / 23.0f)).epsilon(0.001));
    CHECK(part[0].uvMin.x == whole[0].uvMin.x);
    CHECK(part[0].uvMax.x == whole[0].uvMax.x);

    // Past 24 the rungs are two pixels apart, and a whole pixel between them
    // is still itself.
    resetGlyphCache();
    (void)measureText("a", "asset://fonts/test.ttf", 31.0f, 0.0f);
    (void)measureText("a", "asset://fonts/test.ttf", 32.0f, 0.0f);
    CHECK(glyphCacheStats().fills == 2);
    (void)measureText("a", "asset://fonts/test.ttf", 30.5f, 0.0f);
    (void)measureText("a", "asset://fonts/test.ttf", 31.25f, 0.0f);
    CHECK(glyphCacheStats().fills == 2);
}

TEST_CASE(
    "D552: large text at many sizes fills pages of the atlas, and what is not shown makes room -- nothing is cleared")
{
    // **Reported from play, on a phone**: an interface laid out for 540 lines
    // on a 1080-line screen rasterises every glyph at twice the size, and the
    // one megabyte the atlas was held a few sizes of them. It filled two
    // minutes into a run and was emptied whole -- every glyph on the screen
    // rasterised again, a frame of 60 to 110 ms -- and again a minute later.
    FaceGuard guard;
    resetGlyphCache();
    const std::string letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const auto show = [&](f32 size, f32 outline) {
        std::vector<DrawQuad> quads;
        engine::ui::TextStroke stroke;
        stroke.thickness = outline;
        buildTextGeometry(letters, "asset://fonts/test.ttf", size, 0.0f, Rect{Vec2{0.0f, 0.0f}, Vec2{4000.0f, 200.0f}},
                          0, 0, Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, quads, stroke);
        return quads;
    };

    // **One screen**: thirty sizes at once, frame after frame -- a settings
    // page and a HUD on a 2160-line window. More than a page holds.
    engine::core::u64 afterFirst = 0;
    for (int frame = 0; frame < 3; ++frame) {
        engine::ui::beginGlyphFrame();
        for (int step = 0; step < 30; ++step)
            (void)show(24.0f + 2.0f * static_cast<f32>(step), 0.0f);
        if (frame == 0)
            afterFirst = glyphCacheStats().fills;
    }
    CHECK(glyphCacheStats().clears == 0);
    CHECK(glyphCacheStats().evictions == 0);
    CHECK(engine::ui::glyphAtlasPages() > 1);
    CHECK(engine::ui::glyphAtlasPages() <= engine::ui::kGlyphPages);
    // The second frame and the third rasterised nothing.
    CHECK(glyphCacheStats().fills == afterFirst);

    // **Then screen after screen**, each with its own sizes and outlines: more
    // text in all than the atlas may hold. The page nobody has shown for
    // longest makes room; nothing is cleared.
    std::vector<std::pair<f32, f32>> last;
    for (int screen = 0; screen < 16; ++screen) {
        last.clear();
        for (int step = 0; step < 8; ++step) {
            last.emplace_back(40.0f + 4.0f * static_cast<f32>((screen * 3 + step) % 15),
                              static_cast<f32>(1 + screen % 4));
        }
        for (int frame = 0; frame < 2; ++frame) {
            engine::ui::beginGlyphFrame();
            for (const auto& [size, outline] : last) {
                for (const DrawQuad& quad : show(size, outline)) {
                    // Every glyph names a page there is.
                    REQUIRE(quad.texture >= 1);
                    REQUIRE(quad.texture <= engine::ui::glyphAtlasPages());
                }
            }
        }
    }
    CHECK(glyphCacheStats().clears == 0);
    CHECK(glyphCacheStats().evictions > 0);
    CHECK(engine::ui::glyphAtlasPages() == engine::ui::kGlyphPages);

    // **What the frame being built shows is never what goes.** The last
    // screen, built again in its own frame, rasterises nothing: all of it is
    // still there.
    engine::ui::beginGlyphFrame();
    for (const auto& [size, outline] : last)
        (void)show(size, outline);
    const engine::core::u64 held = glyphCacheStats().fills;
    for (const auto& [size, outline] : last)
        (void)show(size, outline);
    CHECK(glyphCacheStats().fills == held);

    // And a page that was emptied says so to whoever uploads it: its rows are
    // newer than anything sent before.
    engine::core::u64 emptied = 0;
    for (engine::core::u32 page = 0; page < engine::ui::glyphAtlasPages(); ++page)
        emptied = std::max(emptied, glyphAtlas(page).clearedAt);
    CHECK(emptied > 0);

    // **One frame that asks for more than the whole atlas** is the case
    // nothing can serve: it is cleared, said, and the frame built again -- as
    // it always was.
    resetGlyphCache();
    int builds = 0;
    engine::ui::buildWithSettledGlyphs([&] {
        ++builds;
        if (builds == 1) {
            for (int step = 0; step < 15; ++step) {
                for (int outline = 0; outline < 6; ++outline)
                    (void)show(44.0f + 4.0f * static_cast<f32>(step), static_cast<f32>(outline));
            }
        }
        (void)show(24.0f, 0.0f);
    });
    CHECK(builds == 2);
    CHECK(glyphCacheStats().clears > 0);
}

TEST_CASE("the atlas says which rows a glyph was written on, and when it was emptied")
{
    // D543: what lets an uploader send a glyph's rows and not the atlas.
    FaceGuard guard;
    resetGlyphCache();
    (void)measureText("a", "asset://fonts/test.ttf", 20.0f, 0.0f);
    const engine::ui::GlyphAtlas first = glyphAtlas();
    REQUIRE(first.rowVersions.size() == first.height);
    const engine::core::u64 before = first.version;
    (void)measureText("b", "asset://fonts/test.ttf", 20.0f, 0.0f);
    const engine::ui::GlyphAtlas second = glyphAtlas();
    CHECK(second.version > before);
    engine::core::u32 written = 0;
    for (const engine::core::u64 row : second.rowVersions)
        written += row > before ? 1u : 0u;
    CHECK(written > 0);
    CHECK(written <= 24);
    CHECK(second.clearedAt <= before);

    resetGlyphCache();
    CHECK(glyphAtlas().clearedAt > second.version);
}

namespace {

// What a stroke's shape IS: every tap of the kernel tried at every texel. The
// way it was made until D563, and what the way it is made now must equal byte
// for byte -- an outline that moved by one level would move every golden with
// stroked text in it.
std::vector<engine::core::u8> everyTap(const std::vector<engine::core::u8>& coverage, int width, int height,
                                       float radius, unsigned join, int pad)
{
    const int outWidth = width + pad * 2;
    const int outHeight = height + pad * 2;
    std::vector<engine::core::u8> out(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight), 0u);
    const int reach = static_cast<int>(std::ceil(radius)) + 1;
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            float best = 0.0f;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const auto ax = static_cast<float>(std::abs(dx));
                    const auto ay = static_cast<float>(std::abs(dy));
                    float distance = std::sqrt(ax * ax + ay * ay);
                    if (join == 2)
                        distance = std::fmax(ax, ay);
                    else if (join == 1)
                        distance = std::fmax(std::fmax(ax, ay), (ax + ay) * 0.70710678f);
                    const float weight = std::fmin(std::fmax(radius + 0.5f - distance, 0.0f), 1.0f);
                    const int readX = x - pad + dx;
                    const int readY = y - pad + dy;
                    if (weight <= 0.0f || readX < 0 || readY < 0 || readX >= width || readY >= height)
                        continue;
                    const float value =
                        static_cast<float>(coverage[static_cast<std::size_t>(readY) * static_cast<std::size_t>(width) +
                                                    static_cast<std::size_t>(readX)]) *
                        weight;
                    best = std::fmax(best, value);
                }
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(x)] =
                static_cast<engine::core::u8>(std::fmin(best + 0.5f, 255.0f));
        }
    }
    return out;
}

} // namespace

TEST_CASE("D563: a stroke's outline is the bytes that trying every tap at every texel gives")
{
    // Coverage of three kinds: a letter's -- solid shapes with soft edges --
    // noise, where no two neighbours agree, and a single texel. A seeded
    // generator of its own, so the shapes are the same on every machine.
    engine::core::u32 state = 0x9E3779B9u;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    const float radii[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.3f, 3.0f, 4.25f, 6.5f, 9.75f, 12.0f};
    int compared = 0;
    for (const float radius : radii) {
        for (unsigned join = 0; join < 3; ++join) {
            for (int kind = 0; kind < 3; ++kind) {
                const int width = kind == 2 ? 1 : 5 + static_cast<int>(next() % 28);
                const int height = kind == 2 ? 1 : 4 + static_cast<int>(next() % 30);
                std::vector<engine::core::u8> coverage(
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        engine::core::u8& texel =
                            coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)];
                        if (kind == 0) {
                            // Two bars and what is between them, soft at the edges.
                            const bool bar = (x > width / 5 && x < width / 2) || (y > height / 2 && y < height - 2);
                            texel = bar ? engine::core::u8{255}
                                        : static_cast<engine::core::u8>(next() % 4 == 0 ? next() % 256 : 0);
                        }
                        else if (kind == 1) {
                            texel = static_cast<engine::core::u8>(next() % 256);
                        }
                        else {
                            texel = static_cast<engine::core::u8>(1 + next() % 255);
                        }
                    }
                }
                // The padding a stroke is given, and one short of it and one
                // over: the outline is cut at the edge and no texel moves.
                const int reach = static_cast<int>(std::ceil(radius)) + 1;
                for (const int pad : {reach, reach - 1, reach + 2}) {
                    std::vector<engine::core::u8> made;
                    engine::ui::dilateCoverage(coverage, static_cast<engine::core::u32>(width),
                                               static_cast<engine::core::u32>(height), radius, join,
                                               static_cast<engine::core::u32>(pad), made);
                    const std::vector<engine::core::u8> expected = everyTap(coverage, width, height, radius, join, pad);
                    REQUIRE(made.size() == expected.size());
                    const bool same = made == expected;
                    CHECK_MESSAGE(same,
                                  "radius " << radius << ", join " << join << ", kind " << kind << ", pad " << pad);
                    ++compared;
                }
            }
        }
    }
    CHECK(compared == 11 * 3 * 3 * 3);

    // And nothing to outline is nothing.
    std::vector<engine::core::u8> none;
    engine::ui::dilateCoverage({}, 0, 0, 2.0f, 0, 3, none);
    CHECK(none.size() == 36);
    CHECK(std::all_of(none.begin(), none.end(), [](engine::core::u8 texel) { return texel == 0; }));
}

TEST_CASE("D623: words that were measured to fit a width fit it at every scale of the screen")
{
    FaceGuard guard;
    resetGlyphCache();

    // A label sized to its words is measured in its own units and drawn in
    // pixels: the same words, at the size times the screen's scale, in the
    // width times that scale. The two sums are the same number on paper and a
    // few millionths apart in floats, and a line that wrapped on that put its
    // last word on a line of its own -- outside the box that was made for one.
    const std::array<std::string_view, 4> lines{
        "Bruno: A minha segue o timbre da minha voz.",
        "Clara: And mine only how loud I am.",
        "Anna: Change the voice language and listen again.",
        "Ana: Minha boca segue uma trilha feita para esta gravação.",
    };
    const std::array<engine::core::f32, 7> scales{1.0f, 1.1f, 1.25f, 1.3333334f, 1.5f, 1.7f, 2.0f};
    for (const std::string_view line : lines) {
        for (const engine::core::f32 size : {17.0f, 20.0f, 26.0f}) {
            const engine::ui::TextRunMetrics plain = measureText(line, "asset://fonts/test.ttf", size, 0.0f);
            const engine::ui::TextRunMetrics rich =
                engine::ui::measureRichText(line, "asset://fonts/test.ttf", size, 0.0f);
            REQUIRE(plain.lineCount == 1);
            for (const engine::core::f32 scale : scales) {
                CAPTURE(line);
                CAPTURE(size);
                CAPTURE(scale);
                CHECK(measureText(line, "asset://fonts/test.ttf", size * scale, plain.size.x * scale).lineCount == 1);
                CHECK(engine::ui::measureRichText(line, "asset://fonts/test.ttf", size * scale, rich.size.x * scale)
                          .lineCount == 1);
            }
        }
    }

    // And a width that is really too small still wraps.
    const engine::ui::TextRunMetrics whole = measureText(lines[0], "asset://fonts/test.ttf", 26.0f, 0.0f);
    CHECK(measureText(lines[0], "asset://fonts/test.ttf", 26.0f, whole.size.x * 0.99f).lineCount == 2);
}
