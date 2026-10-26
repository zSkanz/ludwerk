// Text that joins, stacks and runs right to left (ADR 0260): shaped, ordered
// by the bidirectional algorithm, broken between clusters and, for Thai and
// Lao, between words -- and a caret that steps through it as it is drawn.
//
// The face is the vendored Inter, which has Latin with its combining marks
// and no right-to-left script: the order on screen is asked of it with the
// marks that override a direction, which the algorithm treats as it treats an
// Arabic word. The Thai cases use a word list of their own and draw each
// letter as the box a missing character is, which is what they measure.
#include <algorithm>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../src/text_field.h"
#include "../src/word_break.h"
#include "engine/ui/text_edit.h"
#include "engine/ui/ui.h"

using engine::core::f32;
using engine::core::Rect;
using engine::core::usize;
using engine::core::Vec2;
using engine::ui::DrawQuad;
using engine::ui::TextCluster;

namespace {

constexpr std::string_view Font = "asset://fonts/shaping.ttf";

bool provideFace(void* user, std::string_view name, std::vector<engine::core::u8>& out)
{
    (void)user;
    if (name != Font)
        return false;
    std::ifstream file(ENG_TEST_FONT, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    out.resize(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return !out.empty();
}

struct FaceGuard
{
    FaceGuard() { engine::ui::setFaceProvider(&provideFace, nullptr); }
    ~FaceGuard() { engine::ui::setFaceProvider(nullptr, nullptr); }
    FaceGuard(const FaceGuard&) = delete;
    FaceGuard& operator=(const FaceGuard&) = delete;
};

// "abc" with its direction overridden to right to left: read a, b, c, and
// drawn c, b, a from the left.
constexpr std::string_view Overridden = "\xE2\x80\xAE"
                                        "abc"
                                        "\xE2\x80\xAC";

[[nodiscard]] std::vector<DrawQuad> drawn(std::string_view text, f32 size = 32.0f)
{
    std::vector<DrawQuad> quads;
    engine::ui::buildTextGeometry(text, Font, size, 0.0f, Rect{Vec2{0.0f, 0.0f}, Vec2{1000.0f, 200.0f}}, 0, 0,
                                  engine::core::Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, quads);
    return quads;
}

} // namespace

TEST_CASE("ADR 0260: a letter and its combining mark are shaped into the letter the face draws")
{
    FaceGuard guard;
    // e and a combining acute, as the face's own precomposed \u00E9: one glyph at
    // its width, not an e with an accent beside it.
    const f32 combined = engine::ui::textWidth("e\xCC\x81", Font, 32.0f);
    const f32 precomposed = engine::ui::textWidth("\xC3\xA9", Font, 32.0f);
    CHECK(combined == doctest::Approx(static_cast<double>(precomposed)).epsilon(0.01));
    CHECK(drawn("e\xCC\x81").size() == drawn("\xC3\xA9").size());
}

TEST_CASE("ADR 0260: text overridden to run right to left is drawn in that order")
{
    FaceGuard guard;
    // a then b, which is the taller: drawn b first when overridden.
    const auto tallness = [](const DrawQuad& quad) { return quad.max.y - quad.min.y; };
    const auto byLeft = [](std::vector<DrawQuad> quads) {
        std::sort(quads.begin(), quads.end(), [](const DrawQuad& a, const DrawQuad& b) { return a.min.x < b.min.x; });
        return quads;
    };
    const std::vector<DrawQuad> forward = byLeft(drawn("ab"));
    const std::vector<DrawQuad> backward = byLeft(drawn("\xE2\x80\xAE"
                                                        "ab"
                                                        "\xE2\x80\xAC"));
    REQUIRE(forward.size() == 2);
    REQUIRE(backward.size() == 2);
    CHECK(tallness(forward.front()) < tallness(forward.back()));
    CHECK(tallness(backward.front()) > tallness(backward.back()));
    // And as wide, but for the face's kerning of the pairs it is drawn in --
    // c b and b a, not a b and b c -- which a shaped run takes and text laid
    // out a character at a time does not.
    CHECK(engine::ui::textWidth(Overridden, Font, 32.0f) ==
          doctest::Approx(static_cast<double>(engine::ui::textWidth("abc", Font, 32.0f))).epsilon(0.02));
}

TEST_CASE("ADR 0260: a caret steps through text that runs right to left where it is drawn")
{
    FaceGuard guard;
    const std::vector<TextCluster> clusters = engine::ui::textClusters(Overridden, Font, 32.0f);
    // The override's two marks, and the three letters drawn c, b, a.
    std::vector<TextCluster> letters;
    for (const TextCluster& cluster : clusters) {
        if (cluster.right > cluster.left)
            letters.push_back(cluster);
    }
    REQUIRE(letters.size() == 3);
    CHECK(letters[0].begin == 5);
    CHECK(letters[1].begin == 4);
    CHECK(letters[2].begin == 3);
    CHECK(letters[0].rightToLeft);
    // Before a -- the run's start -- is at a's right edge; before b, at b's,
    // which is where a ends on the left.
    CHECK(engine::ui::caretX(clusters, 3) == doctest::Approx(static_cast<double>(letters[2].right)));
    CHECK(engine::ui::caretX(clusters, 4) == doctest::Approx(static_cast<double>(letters[1].right)));
    CHECK(letters[1].right == doctest::Approx(static_cast<double>(letters[2].left)));

    // A selection of b alone is the span b is drawn in.
    const auto spans = engine::ui::selectionSpans(clusters, 4, 5);
    REQUIRE(spans.size() == 1);
    CHECK(spans.front().first == doctest::Approx(static_cast<double>(letters[1].left)));
    CHECK(spans.front().second == doctest::Approx(static_cast<double>(letters[1].right)));
}

TEST_CASE("ADR 0260: text laid out a character at a time has its caret where it always was")
{
    FaceGuard guard;
    const std::string text = "Hamburg fonts";
    const std::vector<TextCluster> clusters = engine::ui::textClusters(text, Font, 32.0f);
    REQUIRE(clusters.size() == text.size());
    for (usize at = 0; at <= text.size(); ++at) {
        // To the bit: the same advances, summed in the same order.
        CHECK(engine::ui::caretX(clusters, at) ==
              engine::ui::textWidth(std::string_view(text).substr(0, at), Font, 32.0f));
    }
}

TEST_CASE("ADR 0260: the arrows step left and right on screen in a field of right-to-left text")
{
    FaceGuard guard;
    // Three Arabic letters: drawn as boxes in a face without them, and read
    // from the right all the same.
    engine::ui::FieldView view;
    view.display = "\xD8\xA8\xD8\xA7\xD8\xAA";
    view.font = std::string(Font);
    view.size = 32.0f;
    view.box = Rect{Vec2{0.0f, 0.0f}, Vec2{500.0f, 40.0f}};
    view.lines = engine::ui::textLines(view.display, view.font, view.size, 0.0f);
    for (engine::core::u32 at = 0; at <= view.display.size(); at = engine::ui::nextCharacter(view.display, at)) {
        view.boundaries.emplace_back(at, at);
        if (at == view.display.size())
            break;
    }
    // From the text's start, drawn at its right, the right arrow has nowhere
    // on the line to go, and the left goes to after the first letter.
    const auto right = engine::ui::visualStep(view, 0, true);
    const auto left = engine::ui::visualStep(view, 0, false);
    CHECK_FALSE(right.has_value());
    REQUIRE(left.has_value());
    CHECK(*left == 2);
    // The start is drawn right of the end, and a press at the far left is the
    // end.
    CHECK(engine::ui::caretPoint(view, 0).x > engine::ui::caretPoint(view, 6).x);
    CHECK(engine::ui::displayOffsetAt(view, Vec2{view.box.min.x + 0.5f, 10.0f}) == 6);
}

TEST_CASE("ADR 0260: Thai breaks between the words its list has, never inside one")
{
    FaceGuard guard;
    // Two words of two letters each, with nothing between them.
    engine::ui::detail::setWordListForTests(U'\u0E01', {U"\u0E01\u0E02", U"\u0E04\u0E07"});
    std::vector<usize> breaks;
    engine::ui::detail::dictionaryBreaks("\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84\xE0\xB8\x87", breaks);
    REQUIRE(breaks.size() == 1);
    CHECK(breaks.front() == 6);

    // Room for three letters: the line ends after the first word, not
    // after three letters.
    const std::string_view text = "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84\xE0\xB8\x87";
    const f32 letter = engine::ui::textWidth("\xE0\xB8\x81", Font, 20.0f);
    const auto lines = engine::ui::textLines(text, Font, 20.0f, letter * 3.2f);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].end == 6);
    CHECK(lines[1].begin == 6);
    engine::ui::detail::setWordListForTests(U'\u0E01', {});
}

TEST_CASE("ADR 0260: a mark stays with its letter when a line must break mid-word")
{
    FaceGuard guard;
    // No spaces, no list: a line too narrow for the word breaks between
    // characters, and never between an e and the accent on it.
    const std::string text = "e\xCC\x81"
                             "e\xCC\x81"
                             "e\xCC\x81"
                             "e\xCC\x81";
    const f32 one = engine::ui::textWidth("\xC3\xA9", Font, 20.0f);
    const auto lines = engine::ui::textLines(text, Font, 20.0f, one * 2.5f);
    REQUIRE(lines.size() >= 2);
    for (const auto& line : lines)
        CHECK((line.begin % 3) == 0);
}

TEST_CASE("ADR 0260: Latin, Greek, Cyrillic and CJK are not shaped, and Arabic, Thai and marks are")
{
    FaceGuard guard;
    // Laid out a character at a time: each character its own cluster in the
    // text's order, left to right.
    for (const std::string_view plain : {"Hello", "\xCE\xB1\xCE\xB2", "\xD0\xB0\xD0\xB1", "\xE6\x97\xA5\xE6\x9C\xAC"}) {
        const auto clusters = engine::ui::textClusters(plain, Font, 20.0f);
        for (usize at = 1; at < clusters.size(); ++at)
            CHECK(clusters[at].begin > clusters[at - 1].begin);
    }
    // Arabic read right to left: its first letter drawn rightmost.
    const auto arabic = engine::ui::textClusters("\xD8\xA8\xD8\xA7", Font, 20.0f);
    REQUIRE(arabic.size() == 2);
    CHECK(arabic.front().begin == 2);
    CHECK(arabic.front().rightToLeft);
}

TEST_CASE("ADR 0260: rich text is shaped too, each run in its own style")
{
    FaceGuard guard;
    // a in red, then b, overridden to run right to left: b drawn first, and
    // the red a on its right.
    std::vector<DrawQuad> quads;
    engine::ui::buildRichTextGeometry("\xE2\x80\xAE<font color=\"#ff0000\">a</font>b\xE2\x80\xAC", Font, 32.0f, 0.0f,
                                      Rect{Vec2{0.0f, 0.0f}, Vec2{1000.0f, 200.0f}}, 0, 0,
                                      engine::core::Color3{1.0f, 1.0f, 1.0f}, 1.0f, 0, quads);
    REQUIRE(quads.size() == 2);
    const DrawQuad& red = quads[0].color.g < 0.5f ? quads[0] : quads[1];
    const DrawQuad& white = quads[0].color.g < 0.5f ? quads[1] : quads[0];
    CHECK(red.color.r > 0.9f);
    CHECK(red.min.x > white.min.x);

    // A letter and its combining mark measure as the face's own accented one.
    const f32 combined = engine::ui::measureRichText("<b>e\xCC\x81</b>", Font, 32.0f, 0.0f).size.x;
    const f32 precomposed = engine::ui::measureRichText("<b>\xC3\xA9</b>", Font, 32.0f, 0.0f).size.x;
    CHECK(combined == doctest::Approx(static_cast<double>(precomposed)).epsilon(0.01));
}

TEST_CASE("ADR 0260: a joiner or a zero-width space in Latin text leaves it laid out exactly as without")
{
    FaceGuard guard;
    // What a game's own letter spacing marks a title with, and the other
    // characters drawn as nothing: the title is the same quads, to the bit.
    const std::vector<DrawQuad> plain = drawn("FORJA DE ALMAS");
    for (const std::string_view marked : {std::string_view{"\xE2\x80\x8C"
                                                           "FORJA DE ALMAS"},
                                          std::string_view{"\xE2\x80\x8D"
                                                           "FORJA DE ALMAS"},
                                          std::string_view{"\xE2\x80\x8B"
                                                           "FORJA DE ALMAS"},
                                          std::string_view{"\xE2\x81\xA0"
                                                           "FORJA DE ALMAS"}}) {
        const std::vector<DrawQuad> quads = drawn(marked);
        REQUIRE(quads.size() == plain.size());
        for (usize at = 0; at < quads.size(); ++at) {
            CHECK(quads[at].min.x == plain[at].min.x);
            CHECK(quads[at].min.y == plain[at].min.y);
            CHECK(quads[at].max.x == plain[at].max.x);
            CHECK(quads[at].uvMin.x == plain[at].uvMin.x);
        }
        // And measured the same, letter spacing and all.
        CHECK(engine::ui::textWidth(marked, Font, 32.0f) == engine::ui::textWidth("FORJA DE ALMAS", Font, 32.0f));
    }
}
