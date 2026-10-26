// Text measurement and glyph geometry for the UI (ADR 0011, api-design.md
// §2.2's `TextLabel`).
//
// **The face is a real TrueType one, and `TextLabel.Font` is what chooses it.**
// ADR 0011 named stb_truetype and the streaming work is where it arrived: the default is Inter
// (OFL 1.1, a deliberate choice of 2026-08-21), vendored and staged beside the binary,
// and any other name is resolved by the app's content mounts through
// `setFaceProvider`. The property is HONOURED rather than `Inert` -- the
// api-dump carries no `Inert` mark on it, and `measureText` below is where the
// name becomes a face.
//
// **`stb_easy_font` is the FALLBACK, not the face.** A build whose content
// directory has no font still draws text, and a test that runs without staged
// content still measures it; text vanishing because an asset is missing is the
// failure mode a fallback exists to prevent. What it costs, stated so nobody
// has to discover it from a screenshot: ASCII only, one weight, no kerning, and
// a fixed glyph shape that scales by multiplication.
//
// **The glyph store is a CACHE and not a bake, and that is the decision this
// file exists to have made** (a deliberate choice of 2026-08-21). An atlas baked once
// at boot works exactly as long as there is one face at one size, and the streaming work ended
// both halves of that: a project names its own font, and a `TextSize` a tween is
// animating asks for sizes nobody declared. So the store below is keyed by
// **face, size and codepoint** and filled on demand. For the fallback vector
// face the size half of that key is redundant, because the glyph scales by
// multiplication; for a rasterised one it is not, and it was put there a
// milestone before anything needed it.
//
// **Unicode is the same decision from the other side.** The text is decoded as
// UTF-8 into codepoints rather than read a byte at a time, because a game
// written in Portuguese needs á ç ã õ and the fallback face has none of them.
// A codepoint the face in hand cannot draw gets a **visible replacement box**
// rather than nothing, a question mark, or the mojibake that reading the bytes
// one at a time would produce. A player seeing boxes knows the font is missing
// glyphs; a player seeing two strange letters where one accented one should
// be learns nothing.
//
// **Before the box, the faces a label falls back on** (ADR 0259): the label's
// own `FallbackFont`, then the project's `[ui] font_fallbacks`, then the
// default face, and the first that has a character draws it. One face holds
// one family of scripts -- a Latin one has no ideographs, and a CJK one is
// tens of megabytes no game wants for its Latin text -- so a label in a game
// translated into Japanese draws its Latin letters in the game's face and its
// kana in the fallback, in one line.
//
// The seam is `measureText` and `buildTextGeometry`, and it held: the real face
// arrived without moving either signature, the cache's key or its miss path.
// What changed was what fills an entry.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/core/text_key.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/ui/glyph_outline.h"
#include "engine/ui/text_edit.h"
#include "engine/ui/ui.h"
#include "word_break.h"

// The one translation unit that defines them. Both are
// implementation-in-header, so these includes ARE the definitions -- which is
// why nothing else in the module may include either.
//
// `stb_easy_font` is still here as the FALLBACK, and that is deliberate: a
// build whose content directory has no font file still draws text, and a test
// that runs without staged content still measures it. Text vanishing because an
// asset is missing is the failure mode a fallback exists to prevent.
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_easy_font.h>
#include <stb_truetype.h>

// The shaper and the bidirectional algorithm (ADR 0260), for the text the
// codepoint-at-a-time layout cannot draw.
#include <SheenBidi/SheenBidi.h>
#include <hb.h>

namespace engine::ui {
namespace {

using core::Color3;
using core::Vec2;

// The default face's name, as `TextLabel.Font` spells it. An empty `Font` and
// this name are the same request.
constexpr std::string_view DefaultFaceName = "Inter";

// What `stb_easy_font` draws at scale 1: capitals are seven pixels tall and a
// line is twelve. Named because three places divide by them, and a magic 12 in
// three files is a magic 12 nobody can change.
constexpr f32 BuiltInLineHeight = 12.0f;
constexpr f32 BuiltInAscent = 7.0f;

// The codepoints the built-in face covers: printable ASCII.
constexpr u32 FirstFaceCodepoint = 32;
constexpr u32 LastFaceCodepoint = 126;

// What a codepoint the face cannot draw becomes. Not a real character: it is the
// index the replacement box is cached under, above every Unicode scalar so it
// can never collide with one.
constexpr u32 ReplacementCodepoint = 0x11000000u;

// How many (face, size, codepoint) entries the store holds before it is emptied.
//
// **Clear-on-full rather than least-recently-used**, and the reason is honesty
// about what this milestone knows: an eviction policy is tuned against a real
// working set, and v1 has one face whose entire repertoire is ninety-five
// glyphs. Two thousand entries is twenty sizes of the whole face; reaching it
// means something is asking for a new size every frame, which is a bug worth a
// log line rather than a policy worth writing. The streaming work's milestone -- the one with a
// face large enough to need one -- is where a policy is measured.
constexpr usize MaxGlyphEntries = 16384;

// --- UTF-8 -------------------------------------------------------------------

// One codepoint and how many bytes it took. An invalid sequence consumes exactly
// one byte and yields the replacement, so a malformed string advances rather
// than looping and never reads past its end.
struct Decoded
{
    u32 codepoint = ReplacementCodepoint;
    usize length = 1;
};

[[nodiscard]] Decoded decodeUtf8(std::string_view text, usize at) noexcept
{
    const auto byte = static_cast<unsigned char>(text[at]);
    if (byte < 0x80)
        return Decoded{byte, 1};

    usize length = 0;
    u32 codepoint = 0;
    if ((byte & 0xE0u) == 0xC0u) {
        length = 2;
        codepoint = byte & 0x1Fu;
    }
    else if ((byte & 0xF0u) == 0xE0u) {
        length = 3;
        codepoint = byte & 0x0Fu;
    }
    else if ((byte & 0xF8u) == 0xF0u) {
        length = 4;
        codepoint = byte & 0x07u;
    }
    else {
        return Decoded{};
    }

    if (at + length > text.size())
        return Decoded{};
    for (usize index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(text[at + index]);
        if ((continuation & 0xC0u) != 0x80u)
            return Decoded{};
        codepoint = (codepoint << 6) | (continuation & 0x3Fu);
    }
    return Decoded{codepoint, length};
}

// --- The glyph store ---------------------------------------------------------

// One glyph's geometry, in FACE units with the origin at the top left of its
// cell. The caller multiplies by the scale and adds the pen position, which is
// what makes one cached entry serve every size of a vector face -- and what a
// raster face will replace with an atlas rectangle without the key changing.
struct GlyphQuad
{
    f32 minX = 0.0f;
    f32 minY = 0.0f;
    f32 maxX = 0.0f;
    f32 maxY = 0.0f;

    // Where this quad samples the atlas. Meaningless for the built-in vector
    // face, whose quads are solid rectangles -- the entry says which it is.
    f32 u0 = 0.0f;
    f32 v0 = 0.0f;
    f32 u1 = 0.0f;
    f32 v1 = 0.0f;
};

struct GlyphEntry
{
    u64 key = 0;
    f32 advance = 0.0f;
    u32 firstQuad = 0;
    u32 quadCount = 0;
    // Whether the quads sample the atlas or are solid rectangles. Here and in
    // the metrics is where the two faces differ, and nowhere else -- which is
    // what the input and UI work's seam was built for.
    bool textured = false;
    // Which page of the atlas its texels are on (ADR 0169), when it has any:
    // a glyph with no ink is on none, and is not lost with a page.
    bool paged = false;
    core::u8 page = 0;
    // Which face of its chain drew it (ADR 0259): 0 the label's own, and the
    // box a missing character is. What a line's height is measured from.
    core::u8 face = 0;
};

// A shelf packer: glyphs go left to right on a row whose height is the tallest
// glyph placed on it so far, and one that does not fit starts a new row. Not
// the tightest packing there is, and it does not need to be -- UI glyphs at one
// or two sizes are close to the same height, which is the case a shelf packer
// wastes nothing on.
struct AtlasPacker
{
    u32 cursorX = 0;
    u32 cursorY = 0;
    u32 rowHeight = 0;
};

// **One page of the atlas** (ADR 0169): a square of coverage with a packer of
// its own, the version each of its rows was last written at, the version it
// was last emptied at, and the frame anything on it was last shown in.
struct AtlasPage
{
    std::vector<core::u8> pixels;
    std::vector<u64> rows;
    AtlasPacker packer;
    u64 clearedAt = 0;
    u64 shownAt = 0;
};

struct GlyphStore
{
    // Sorted by key. A UI has a few hundred distinct glyphs at most, so a binary
    // search over a flat array beats a hash table on every axis that matters
    // here -- and a flat array has an order, which an unordered container does
    // not (R10).
    std::vector<GlyphEntry> entries;
    std::vector<GlyphQuad> quads;
    GlyphCacheStats stats;

    // Single-channel coverage, a page at a time. None until a raster face has
    // drawn something, and none forever with the built-in one.
    std::vector<AtlasPage> pages;
    // The page new glyphs go on.
    usize filling = 0;
    // One count for every page: what `GlyphAtlas::version` is.
    u64 atlasVersion = 0;
    // What every page was last emptied at by a clear or a reset, kept for the
    // pages there are none of now: an uploader of page 0 must know.
    u64 emptiedAt = 0;
    // Which frame is being built (`beginGlyphFrame`).
    u64 frame = 1;
};

GlyphStore& store()
{
    // Process-global like the layout stats beside it, and for the same reason:
    // the face is compiled in, so there is exactly one of it, and threading it
    // through `measureText` would put a cache in the signature of a pure
    // question.
    static GlyphStore instance;
    return instance;
}

// A text stroke as the glyph store keys it (ADR 0110): the outline's radius in
// quarter pixels, the join, and whether it is the ring alone. Zero quarters is
// the plain glyph.
//
// **The ring** is the outline without the letter inside it -- what hollow
// lettering shows. It is only for text that is not drawn: under a visible
// letter the ring's inner edge and the letter's edge are each half-covered, and
// the background shows through the seam between them, where a whole outline
// under the letter has none.
struct GlyphStroke
{
    u32 quarters = 0;
    u32 join = 0;
    bool ring = false;
};

// The widest outline a glyph is given: 63 pixels and three quarters, which is
// what eight bits of quarters hold.
constexpr u32 MaxStrokeQuarters = 255;

// The key. Face, size, codepoint and stroke, packed: 16 bits of face hash, 16
// of size in quarter-pixels, and in the low 32 the join (2 bits), the ring (1),
// the stroke's radius in quarter pixels (8) and the codepoint (21 -- every
// Unicode scalar fits). A plain glyph's stroke bits are zero, so its key is
// what it was before strokes existed.
//
// The size is quantized rather than taken raw because a `TextSize` animated by a
// tween would otherwise mint an entry per frame, and a quarter of a pixel is
// below what any face resolves.
[[nodiscard]] u64 glyphKey(u32 face, f32 pixelSize, u32 codepoint, GlyphStroke stroke = {}) noexcept
{
    const f32 clamped = std::fmin(std::fmax(pixelSize, 0.0f), 16383.0f);
    const auto quarters = static_cast<u64>(clamped * 4.0f) & 0xFFFFu;
    const u64 low = (static_cast<u64>(stroke.join & 3u) << 30) | (static_cast<u64>(stroke.ring ? 1u : 0u) << 29) |
                    (static_cast<u64>(std::min(stroke.quarters, MaxStrokeQuarters)) << 21) |
                    static_cast<u64>(codepoint & 0x1FFFFFu);
    return (static_cast<u64>(face & 0xFFFFu) << 48) | (quarters << 32) | low;
}

// FNV-1a over the font's content URN. A hash rather than an interned atom
// because `ui` is handed a `string_view` and interning would need the world.
[[nodiscard]] u32 faceHash(std::string_view font) noexcept
{
    u32 hash = 2166136261u;
    for (const char character : font) {
        hash ^= static_cast<u32>(static_cast<unsigned char>(character));
        hash *= 16777619u;
    }
    return hash;
}

// The face's own advance for a printable ASCII codepoint. Read from the table
// rather than through `stb_easy_font_width`, which rounds the whole string up:
// summing rounded per-glyph widths would drift from the line width the face
// actually draws.
[[nodiscard]] f32 faceAdvance(u32 codepoint) noexcept
{
    if (codepoint < FirstFaceCodepoint || codepoint > LastFaceCodepoint)
        return 0.0f;
    return static_cast<f32>(stb_easy_font_charinfo[codepoint - FirstFaceCodepoint].advance & 15);
}

// --- The faces ---------------------------------------------------------------

// A loaded face: the file's bytes, kept alive because `stbtt_fontinfo` points
// into them, and the metrics every size scales from.
struct Face
{
    u32 hash = 0;
    std::string name;
    std::vector<core::u8> data;
    stbtt_fontinfo info{};
    // Font units, from `hhea`. Scaled per size rather than stored per size,
    // because the scale is one multiplication and a per-size copy is a cache.
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    bool ready = false;
    // This name could not be loaded and resolves to the default face.
    //
    // Recorded rather than simply not cached, and the difference is a file read
    // per frame: a label drawn every frame with a typo in its font name would
    // otherwise re-attempt the load every frame. Recorded rather than left as a
    // not-ready face, because a not-ready face still has its OWN hash -- which
    // would key its glyphs separately from the default face it is drawing with,
    // and rasterise the same face twice.
    bool fallsBack = false;
    // **The face as the shaper reads it** (ADR 0260), made the first time a
    // text that needs shaping is drawn in it: over the same bytes, which it
    // never copies, at one unit to the font unit.
    struct ShaperRelease
    {
        void operator()(hb_font_t* font) const noexcept { hb_font_destroy(font); }
    };
    std::unique_ptr<hb_font_t, ShaperRelease> shaper;
};

// **A face and the faces it falls back on, in order** (ADR 0259): the
// label's own first, then its `FallbackFont`, then the project's `[ui]
// font_fallbacks`, then the default face -- each once, and only those that
// loaded. A character is drawn by the first that has it.
//
// The glyph store keys by the CHAIN, not by the face that drew: which face
// that is depends on the faces before it, and a hit must not have to ask
// them. Chains of the same faces share a key, so two names for one face
// still share its glyphs.
struct FaceChain
{
    // As asked: a name, and after `kFontFallbackSeparator` the label's own
    // fallbacks and its letter spacing (`labelFont`).
    std::string font;
    u32 hash = 0;
    std::vector<Face*> faces;
    // `TextLabel.LetterSpacing`: room after each character, in ems. Not part
    // of `hash`: it moves glyphs, it does not change them, so a label with it
    // and one without share every glyph.
    f32 tracking = 0.0f;

    [[nodiscard]] Face& primary() const noexcept { return *faces.front(); }
};

// The most faces one chain tries. A bit each in a line's mask of the faces it
// used, and more than any game names: a label's, two or three of its own, the
// project's few, and the default.
constexpr usize MaxChainFaces = 16;

struct FaceTable
{
    // Stable addresses: `faceFor` hands out references and a caller holds one
    // for the length of a `measureText`. A vector of values would move them all
    // the next time a name is looked up.
    std::vector<std::unique_ptr<Face>> faces;
    FaceProvider provider = nullptr;
    void* providerUser = nullptr;
    // Names that were asked for and could not be loaded. Kept so the warning is
    // once per name rather than once per frame -- a label drawn every frame with
    // a typo in its font name would otherwise fill the log.
    std::vector<u32> warned;
    bool defaultAttempted = false;
    // The project's fallbacks (`setFontFallbacks`), and the chains built from
    // them and the faces above -- dropped with the faces.
    std::vector<std::string> fallbacks;
    std::vector<std::unique_ptr<FaceChain>> chains;
};

FaceTable& faceTable()
{
    static FaceTable instance;
    return instance;
}

// Where the default face lives beside the binary. Staged there by the build
// (engine/app/CMakeLists.txt) for the same reason the message catalog is: the
// engine resolves content relative to its own location, which is the shape a
// packaged build uses.
[[nodiscard]] std::filesystem::path defaultFacePath()
{
    return platform::paths().contentDir / "fonts" / "Inter.ttf";
}

// Loads a face's metrics. False leaves `ready` false, and the caller falls back.
[[nodiscard]] bool initFace(Face& face)
{
    if (face.data.empty())
        return false;
    // A collection's first face: the CJK families ship as one file of several
    // (`.ttc`), and the first is the one a game names the file for. A single
    // face is at offset 0, which is what this answers for one.
    const int offset = stbtt_GetFontOffsetForIndex(face.data.data(), 0);
    if (offset < 0 || stbtt_InitFont(&face.info, face.data.data(), offset) == 0)
        return false;
    stbtt_GetFontVMetrics(&face.info, &face.ascent, &face.descent, &face.lineGap);
    face.ready = true;
    return true;
}

// The face for a name, loading it on first use. Never null: a name that cannot
// be loaded resolves to the default, and a default that cannot be loaded
// resolves to a face with `ready == false`, which is the built-in vector
// fallback.
[[nodiscard]] Face& faceFor(std::string_view name)
{
    FaceTable& table = faceTable();
    const u32 hash = faceHash(name);

    for (const std::unique_ptr<Face>& known : table.faces) {
        if (known->hash != hash || known->name != name)
            continue;
        // A name that failed to load resolves to the DEFAULT face, every time,
        // rather than to the empty entry that remembers it failed.
        return known->fallsBack ? faceFor({}) : *known;
    }

    Face face;
    face.hash = hash;
    face.name = std::string(name);

    // An empty name and the default's own name are the same request. Anything
    // else goes to the provider, which is the app's content mounts.
    const bool wantsDefault = name.empty() || name == DefaultFaceName;
    if (wantsDefault) {
        std::vector<std::byte> bytes;
        if (platform::readFile(defaultFacePath(), bytes)) {
            face.data.resize(bytes.size());
            std::memcpy(face.data.data(), bytes.data(), bytes.size());
        }
    }
    else if (table.provider != nullptr) {
        (void)table.provider(table.providerUser, name, face.data);
    }

    if (!initFace(face) && !wantsDefault) {
        // Named face, not loadable: fall back to the default rather than to
        // nothing. A label that vanished because its font name had a typo is a
        // bug report about the label.
        if (std::find(table.warned.begin(), table.warned.end(), hash) == table.warned.end()) {
            table.warned.push_back(hash);
            const core::I18nArg args[] = {{"font", std::string(name)}};
            core::log(core::LogLevel::Warn, ENG_TR("ui.warn.font_missing"), args);
        }
        face.fallsBack = true;
        table.faces.push_back(std::make_unique<Face>(std::move(face)));
        return faceFor({});
    }

    if (!face.ready && wantsDefault && !table.defaultAttempted) {
        // Once, and only for the default: a build whose content directory has no
        // font still draws text, with the built-in vector face, and should say
        // so rather than looking subtly wrong.
        table.defaultAttempted = true;
        const core::I18nArg args[] = {{"path", defaultFacePath().string()}};
        core::log(core::LogLevel::Warn, ENG_TR("ui.warn.default_font_missing"), args);
    }

    table.faces.push_back(std::make_unique<Face>(std::move(face)));
    return *table.faces.back();
}

// The chain for a `font` as the text functions take it, built on first use.
// **No chain past a face that did not load**: the built-in vector face's glyphs
// are rectangles in its own units, and a raster glyph beside one in a line
// would be measured in neither.
[[nodiscard]] FaceChain& chainFor(std::string_view font)
{
    FaceTable& table = faceTable();
    for (const std::unique_ptr<FaceChain>& known : table.chains) {
        if (known->font == font)
            return *known;
    }

    auto chain = std::make_unique<FaceChain>();
    chain->font = std::string(font);
    const usize cut = font.find(kFontFallbackSeparator);
    Face& primary = faceFor(font.substr(0, cut));
    chain->faces.push_back(&primary);
    chain->hash = primary.hash;
    if (primary.ready) {
        const Face& standard = faceFor({});
        const auto add = [&](std::string_view name) {
            if (chain->faces.size() >= MaxChainFaces)
                return;
            Face& face = faceFor(name);
            // A name that did not load resolves to the default face, which
            // comes last whatever is named.
            if (!face.ready || (&face == &standard && !name.empty()))
                return;
            if (std::find(chain->faces.begin(), chain->faces.end(), &face) != chain->faces.end())
                return;
            chain->faces.push_back(&face);
            chain->hash = (chain->hash ^ face.hash) * 16777619u;
        };
        std::string_view own = cut == std::string_view::npos ? std::string_view{} : font.substr(cut + 1);
        while (!own.empty()) {
            const usize next = own.find(kFontFallbackSeparator);
            const std::string_view name = own.substr(0, next);
            if (!name.empty() && name.front() == kLetterSpacingMark) {
                // A number `labelFont` wrote: read back the way it was written.
                char digits[32] = {};
                const usize length = std::min(name.size() - 1, sizeof(digits) - 1);
                std::memcpy(digits, name.data() + 1, length);
                chain->tracking = std::strtof(digits, nullptr);
            }
            else if (!name.empty()) {
                add(name);
            }
            own = next == std::string_view::npos ? std::string_view{} : own.substr(next + 1);
        }
        for (const std::string& name : table.fallbacks)
            add(name);
        add({});
    }
    table.chains.push_back(std::move(chain));
    return *table.chains.back();
}

// **The size a raster glyph is made at, for a size it is asked at.**
//
// A glyph was rasterised at the size it was drawn at, to the quarter pixel. A
// `TextSize` a tween animates -- a damage number that pops -- and a label in
// the world, a different size at every distance, asked for a new size every
// frame: a hundred and five sizes of ten digits between 14 and 40, the store
// filled, was emptied, and every glyph of every label was made again.
//
// So a size is made at the next rung of a ladder at or above it and drawn
// smaller: whole pixels to 24, every second to 64, and a quarter larger each
// time past that. Never more than a twelfth smaller below 64, a fifth above --
// a minification the bilinear filter does without anybody seeing it. **A whole
// number of pixels up to 64 is its own rung**: text at a size somebody chose
// is drawn exactly as it was. Past 64 a glyph is thousands of texels, and one
// set a pixel would be the atlas; a title that large is made a little larger
// and drawn down, which it survives better than small text would. Past 128 it
// is drawn UP from 128, and is as soft as that makes it. How distance fields would answer the same
// question -- one set of glyphs for every size -- is a second texture format
// and a second shader, and softer small text; that is the trade not taken.
constexpr f32 LargestRasterSize = 128.0f;

[[nodiscard]] f32 rasterSizeFor(f32 pixelSize) noexcept
{
    const f32 size = std::fmax(pixelSize, 1.0f);
    const f32 whole = std::round(size);
    if (size <= 64.0f) {
        if (std::fabs(size - whole) < 0.01f)
            return whole;
        return size <= 24.0f ? std::ceil(size) : std::ceil(size * 0.5f) * 2.0f;
    }
    // 80, 100, 128 -- and no larger: a glyph past that is a tenth of the
    // atlas by itself, and text that large is drawn up from 128.
    f32 rung = 64.0f;
    while (rung < size - 0.01f && rung < LargestRasterSize)
        rung = std::ceil(rung * 1.25f * 0.25f) * 4.0f;
    return std::fmin(rung, LargestRasterSize);
}

// The multiplier between a cached glyph's units and pixels.
//
// For a raster face, the size asked for over the size its glyphs were made at
// (`rasterSizeFor`): one for a whole pixel, a little under it between rungs.
// `pixelSize / 12` for the built-in vector one, whose glyphs are cached once
// and scaled. Every caller multiplies by this and neither has to know which
// kind of face it is looking at.
[[nodiscard]] f32 scaleFor(const Face& face, f32 pixelSize) noexcept
{
    if (face.ready) {
        return std::fmax(pixelSize, 1.0f) / rasterSizeFor(pixelSize);
    }
    // A `TextSize` of 12 is the built-in face's own size. Below about 6 the
    // vector strokes collapse into each other, which is a property of the face
    // rather than something to clamp here -- a caller asking for 3-pixel text
    // gets 3-pixel text.
    return pixelSize / BuiltInLineHeight;
}

// The distance from one baseline to the next, in pixels.
[[nodiscard]] f32 lineHeightOf(const Face& face, f32 pixelSize) noexcept
{
    if (!face.ready) {
        return BuiltInLineHeight * scaleFor(face, pixelSize);
    }
    // `ascent - descent + lineGap`, which is the face's own opinion about line
    // spacing. `descent` is negative in font units, hence the subtraction.
    const f32 scale = stbtt_ScaleForPixelHeight(&face.info, pixelSize);
    return static_cast<f32>(face.ascent - face.descent + face.lineGap) * scale;
}

[[nodiscard]] f32 ascentOf(const Face& face, f32 pixelSize) noexcept
{
    if (!face.ready) {
        return BuiltInAscent * scaleFor(face, pixelSize);
    }
    return static_cast<f32>(face.ascent) * stbtt_ScaleForPixelHeight(&face.info, pixelSize);
}

// A line's reach above its baseline and its whole height, in pixels.
struct LineMetrics
{
    f32 ascent = 0.0f;
    f32 height = 0.0f;
};

// **A line is as tall as the faces its characters were drawn with** (ADR
// 0259): the highest ascent and the deepest descent among them, and the label's
// own face always among them, with its own line gap. `faces` is a bit for each
// face of the chain used. A line in the label's own face alone measures what
// it always did -- to the bit, since the sums below are not taken for it.
[[nodiscard]] LineMetrics lineMetricsOf(const FaceChain& chain, f32 pixelSize, u32 faces) noexcept
{
    const Face& primary = chain.primary();
    LineMetrics metrics{ascentOf(primary, pixelSize), lineHeightOf(primary, pixelSize)};
    if (!primary.ready || (faces >> 1) == 0)
        return metrics;
    const f32 gap = static_cast<f32>(primary.lineGap) * stbtt_ScaleForPixelHeight(&primary.info, pixelSize);
    f32 below = metrics.height - metrics.ascent;
    for (usize slot = 1; slot < chain.faces.size(); ++slot) {
        if ((faces & (1u << slot)) == 0)
            continue;
        const Face& face = *chain.faces[slot];
        const f32 scale = stbtt_ScaleForPixelHeight(&face.info, pixelSize);
        metrics.ascent = std::fmax(metrics.ascent, static_cast<f32>(face.ascent) * scale);
        below = std::fmax(below, static_cast<f32>(-face.descent) * scale + gap);
    }
    metrics.height = metrics.ascent + below;
    return metrics;
}

// --- The atlas ---------------------------------------------------------------

// **A page is 1024 square, and there are up to eight** (ADR 0169). One page
// holds several thousand glyphs at the sizes a 1080-line screen draws them,
// which is more than a HUD has. An interface laid out for half the lines of
// its screen -- a phone, a 2160-line window -- rasterises every glyph at twice
// the size, four times the texels, and one page was a few sizes of them: it
// filled and was emptied whole, every glyph on the screen rasterised again in
// one frame, a minute into a run and every minute after. So a page that is
// full is followed by another, each a texture of its own -- nothing already
// sent is sent again and no cached place moves -- and past the last, the page
// nobody has shown for longest is emptied for the next glyph.
constexpr u32 AtlasSize = 1024;

// **The page the next glyph goes on, when the one being filled is full**
// (ADR 0169): a new one while there may be more; after that, the page nobody
// has shown for longest, emptied -- its glyphs forgotten, to be rasterised
// again if they come back. False when every page holds something the frame
// being built has shown: then nothing can go without what is on the screen
// going, and the caller clears.
[[nodiscard]] bool nextPage(GlyphStore& cache)
{
    if (cache.pages.size() < kGlyphPages) {
        AtlasPage page;
        page.pixels.assign(static_cast<usize>(AtlasSize) * AtlasSize, 0u);
        page.rows.assign(AtlasSize, 0u);
        page.clearedAt = cache.emptiedAt;
        page.shownAt = cache.frame;
        cache.pages.push_back(std::move(page));
        cache.filling = cache.pages.size() - 1;
        return true;
    }
    usize oldest = cache.pages.size();
    for (usize at = 0; at < cache.pages.size(); ++at) {
        if (cache.pages[at].shownAt >= cache.frame)
            continue;
        if (oldest == cache.pages.size() || cache.pages[at].shownAt < cache.pages[oldest].shownAt)
            oldest = at;
    }
    if (oldest == cache.pages.size())
        return false;

    // Its glyphs go, and the quads they had with them: the entries that stay
    // are told where theirs are now.
    std::vector<GlyphEntry> kept;
    std::vector<GlyphQuad> quads;
    kept.reserve(cache.entries.size());
    quads.reserve(cache.quads.size());
    for (GlyphEntry entry : cache.entries) {
        if (entry.paged && entry.page == oldest)
            continue;
        const u32 first = static_cast<u32>(quads.size());
        quads.insert(quads.end(), cache.quads.begin() + static_cast<std::ptrdiff_t>(entry.firstQuad),
                     cache.quads.begin() + static_cast<std::ptrdiff_t>(entry.firstQuad + entry.quadCount));
        entry.firstQuad = first;
        kept.push_back(entry);
    }
    cache.entries = std::move(kept);
    cache.quads = std::move(quads);
    cache.stats.entries = cache.entries.size();
    ++cache.stats.evictions;

    AtlasPage& page = cache.pages[oldest];
    std::fill(page.pixels.begin(), page.pixels.end(), core::u8{0});
    std::fill(page.rows.begin(), page.rows.end(), u64{0});
    page.packer = AtlasPacker{};
    ++cache.atlasVersion;
    page.clearedAt = cache.atlasVersion;
    page.shownAt = cache.frame;
    cache.filling = oldest;
    return true;
}

// Rasterises one codepoint into the atlas at `pixelSize`, filling `entry` --
// outlined by `stroke` when it has a radius. False means it did not fit, which
// the caller answers by clearing the store. `top` is how far the baseline is
// below the top of the line in pixels at this size: the label's own face's
// ascent, whichever face of its chain draws the glyph, so every glyph of a
// line is placed from one top.
//
// By the face's own glyph index: what a shaped run names (ADR 0260), and what
// a codepoint is found as first (`rasteriseGlyph`).
[[nodiscard]] bool rasteriseGlyphIndex(Face& face, f32 top, f32 pixelSize, int glyph, GlyphEntry& entry,
                                       GlyphStore& cache, GlyphStroke stroke = {})
{
    // A scope of its own: a glyph is rasterised once, the first time a size
    // of it is shown, and a report should say how many a slow frame made.
    ENG_PROFILE_SCOPE("ui.glyphs");
    const f32 scale = stbtt_ScaleForPixelHeight(&face.info, pixelSize);
    if (glyph == 0) {
        return false;
    }

    int advanceUnits = 0;
    int bearingUnits = 0;
    stbtt_GetGlyphHMetrics(&face.info, glyph, &advanceUnits, &bearingUnits);
    entry.advance = static_cast<f32>(advanceUnits) * scale;

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&face.info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    const auto plainWidth = static_cast<u32>(x1 - x0);
    const auto plainHeight = static_cast<u32>(y1 - y0);

    entry.textured = true;
    if (plainWidth == 0 || plainHeight == 0) {
        // A space, and every other glyph with no ink. It has an advance and no
        // quad, which is exactly right -- and it must still be CACHED, or every
        // space in a paragraph is a rasterisation.
        entry.quadCount = 0;
        return true;
    }

    // A glyph larger than a page is on none of them: the box a missing one is.
    if (plainWidth + 2 > AtlasSize || plainHeight + 2 > AtlasSize)
        return false;

    // An outline is the glyph drawn somewhere else first, and grown: `pad`
    // texels on every side, which is where the outline goes.
    const f32 radius = static_cast<f32>(stroke.quarters) * 0.25f;
    const u32 pad = stroke.quarters == 0 ? 0u : static_cast<u32>(std::ceil(radius)) + 1u;
    std::vector<core::u8> outlined;
    if (pad > 0) {
        std::vector<core::u8> plain(static_cast<usize>(plainWidth) * plainHeight, 0u);
        stbtt_MakeGlyphBitmap(&face.info, plain.data(), static_cast<int>(plainWidth), static_cast<int>(plainHeight),
                              static_cast<int>(plainWidth), scale, scale, glyph);
        dilateCoverage(plain, plainWidth, plainHeight, radius, stroke.join, pad, outlined);
        if (stroke.ring) {
            // The letter taken back out, texel for texel.
            const u32 grownWidth = plainWidth + pad * 2;
            for (u32 row = 0; row < plainHeight; ++row) {
                for (u32 column = 0; column < plainWidth; ++column) {
                    core::u8& texel =
                        outlined[static_cast<usize>(row + pad) * grownWidth + static_cast<usize>(column + pad)];
                    const core::u8 letter = plain[static_cast<usize>(row) * plainWidth + column];
                    texel = texel > letter ? static_cast<core::u8>(texel - letter) : core::u8{0};
                }
            }
        }
    }
    const u32 width = plainWidth + pad * 2;
    const u32 height = plainHeight + pad * 2;

    // One texel of padding on each side, so bilinear sampling at the edge of a
    // glyph cannot reach into its neighbour. Without it, text at a fractional
    // scale grows faint marks nobody can account for.
    constexpr u32 Padding = 1;
    if (width + Padding * 2 > AtlasSize || height + Padding * 2 > AtlasSize)
        return false;
    // Whether the page being filled has room, moving its cursor to where the
    // glyph goes when it has.
    const auto fits = [&](AtlasPage& page) {
        AtlasPacker& packer = page.packer;
        if (packer.cursorX + width + Padding * 2 > AtlasSize) {
            packer.cursorX = 0;
            packer.cursorY += packer.rowHeight;
            packer.rowHeight = 0;
        }
        return packer.cursorY + height + Padding * 2 <= AtlasSize;
    };
    if (cache.pages.empty() || !fits(cache.pages[cache.filling])) {
        if (!nextPage(cache))
            return false;
        if (!fits(cache.pages[cache.filling]))
            return false;
    }
    AtlasPage& page = cache.pages[cache.filling];
    AtlasPacker& packer = page.packer;

    const u32 originX = packer.cursorX + Padding;
    const u32 originY = packer.cursorY + Padding;
    if (pad == 0) {
        stbtt_MakeGlyphBitmap(&face.info, page.pixels.data() + static_cast<usize>(originY) * AtlasSize + originX,
                              static_cast<int>(width), static_cast<int>(height), static_cast<int>(AtlasSize), scale,
                              scale, glyph);
    }
    else {
        for (u32 row = 0; row < height; ++row) {
            std::copy_n(outlined.data() + static_cast<usize>(row) * width, width,
                        page.pixels.data() + static_cast<usize>(originY + row) * AtlasSize + originX);
        }
    }

    packer.cursorX += width + Padding * 2;
    packer.rowHeight = std::max(packer.rowHeight, height + Padding * 2);
    ++cache.atlasVersion;
    // The glyph's rows **and the row of padding on either side of it**: what
    // an uploader sends. A page emptied to make room is not sent again whole
    // -- four megabytes, the hitch this was written to end -- so the rows a
    // glyph's edge can sample must be the ones that travel with it.
    for (u32 row = originY - Padding; row < originY + height + Padding; ++row)
        page.rows[row] = cache.atlasVersion;
    page.shownAt = cache.frame;
    entry.paged = true;
    entry.page = static_cast<core::u8>(cache.filling);

    // The quad is in PIXELS at this size, measured from the top-left of the
    // line rather than from the baseline: everything downstream places text from
    // the top of its box, and converting once here beats converting at every
    // call site.
    const f32 ascentPixels = top;
    entry.firstQuad = static_cast<u32>(cache.quads.size());
    entry.quadCount = 1;
    cache.quads.push_back(GlyphQuad{
        .minX = static_cast<f32>(x0) - static_cast<f32>(pad),
        .minY = ascentPixels + static_cast<f32>(y0) - static_cast<f32>(pad),
        .maxX = static_cast<f32>(x0) - static_cast<f32>(pad) + static_cast<f32>(width),
        .maxY = ascentPixels + static_cast<f32>(y0) - static_cast<f32>(pad) + static_cast<f32>(height),
        .u0 = static_cast<f32>(originX) / static_cast<f32>(AtlasSize),
        .v0 = static_cast<f32>(originY) / static_cast<f32>(AtlasSize),
        .u1 = static_cast<f32>(originX + width) / static_cast<f32>(AtlasSize),
        .v1 = static_cast<f32>(originY + height) / static_cast<f32>(AtlasSize),
    });
    return true;
}

[[nodiscard]] bool rasteriseGlyph(Face& face, f32 top, f32 pixelSize, u32 codepoint, GlyphEntry& entry,
                                  GlyphStore& cache, GlyphStroke stroke = {})
{
    return rasteriseGlyphIndex(face, top, pixelSize, stbtt_FindGlyphIndex(&face.info, static_cast<int>(codepoint)),
                               entry, cache, stroke);
}

// Appends the quads for one printable ASCII codepoint, in face units.
void fillFaceGlyph(u32 codepoint, GlyphEntry& entry, std::vector<GlyphQuad>& quads)
{
    entry.advance = faceAdvance(codepoint);

    // `stb_easy_font_print` writes 16-byte vertices, four per quad. One glyph is
    // a handful of segments; the buffer is sized for the worst of them.
    static constexpr int VertexBytes = 16;
    static constexpr int MaxQuadsPerGlyph = 64;
    char vertices[static_cast<usize>(MaxQuadsPerGlyph) * 4u * VertexBytes] = {};

    char text[2] = {static_cast<char>(codepoint), '\0'};
    const int count = stb_easy_font_print(0.0f, 0.0f, text, nullptr, vertices, static_cast<int>(sizeof(vertices)));

    entry.firstQuad = static_cast<u32>(quads.size());
    for (int quad = 0; quad < count; ++quad) {
        // The face emits axis-aligned rectangles as four corners; only the first
        // and third are needed, and reading them as floats out of the byte
        // buffer is what upstream's own example does.
        f32 corners[4];
        std::memcpy(&corners[0], vertices + static_cast<usize>(quad * 4 * VertexBytes), sizeof(f32) * 2);
        std::memcpy(&corners[2], vertices + static_cast<usize>((quad * 4 + 2) * VertexBytes), sizeof(f32) * 2);
        quads.push_back(GlyphQuad{corners[0], corners[1], corners[2], corners[3]});
    }
    entry.quadCount = static_cast<u32>(quads.size()) - entry.firstQuad;
}

// The replacement: a hollow box, one face unit thick, at the width of a question
// mark. **Visible on purpose.** A missing glyph that drew nothing would make a
// Portuguese label silently lose its accents, and one that drew `?` would be
// indistinguishable from a `?` somebody typed.
void fillReplacementGlyph(GlyphEntry& entry, std::vector<GlyphQuad>& quads)
{
    const f32 advance = faceAdvance('?');
    entry.advance = advance;
    entry.firstQuad = static_cast<u32>(quads.size());

    constexpr f32 top = 1.0f;
    constexpr f32 bottom = BuiltInAscent;
    const f32 left = 1.0f;
    const f32 right = std::fmax(advance - 1.0f, left + 2.0f);

    quads.push_back(GlyphQuad{left, top, right, top + 1.0f});
    quads.push_back(GlyphQuad{left, bottom - 1.0f, right, bottom});
    quads.push_back(GlyphQuad{left, top, left + 1.0f, bottom});
    quads.push_back(GlyphQuad{right - 1.0f, top, right, bottom});

    entry.quadCount = 4;
}

// **Empties the store, and says so -- once, then at every doubling.**
//
// A clear is a signal rather than a routine eviction, which is why it is said
// at all. But a clear that recurs recurs every frame -- somebody dragging a
// `TextScaled` label's handles asks for a new size per frame (reported as a
// console flooded with this line) -- and one line per frame buries every
// other message while saying nothing new. So: the first clear, then the 2nd,
// 4th, 8th..., each with how many there have been.
void clearStore(GlyphStore& cache)
{
    ++cache.stats.clears;
    const u64 clears = cache.stats.clears;
    if ((clears & (clears - 1)) == 0) {
        const core::I18nArg args[] = {{"entries", static_cast<core::i64>(cache.entries.size())},
                                      {"clears", static_cast<core::i64>(clears)}};
        core::log(core::LogLevel::Warn, ENG_TR("ui.warn.glyph_cache_cleared"), args);
    }
    cache.entries.clear();
    cache.quads.clear();
    // The atlas goes with them, every page of it: keeping the pixels while
    // dropping the entries that name them would leave coverage nothing can
    // find and no room for more.
    cache.pages.clear();
    cache.filling = 0;
    ++cache.atlasVersion;
    cache.emptiedAt = cache.atlasVersion;
    cache.stats.entries = 0;
}

// The entry for one (face, size, codepoint), filling it if this is the first
// time anything asked. Returns an INDEX rather than a pointer, because filling
// may reallocate and a caller that held a pointer across the call would be
// holding a dangling one -- which is the classic way a cache becomes a crash.
// The quads of a solid-rectangle glyph (the built-in face, the replacement
// box) grown by `amount` face units on every side: its outline.
void growQuads(GlyphEntry& entry, std::vector<GlyphQuad>& quads, f32 amount)
{
    for (u32 index = 0; index < entry.quadCount; ++index) {
        GlyphQuad& quad = quads[entry.firstQuad + index];
        quad.minX -= amount;
        quad.minY -= amount;
        quad.maxX += amount;
        quad.maxY += amount;
    }
}

// Whether a glyph that was not placed was refused for want of room, and not
// because the face has none of it: the atlas has its every page, and the face
// knows the codepoint.
[[nodiscard]] bool atlasFull(const GlyphStore& cache, Face& face, u32 codepoint)
{
    return cache.pages.size() >= kGlyphPages && !cache.entries.empty() &&
           stbtt_FindGlyphIndex(&face.info, static_cast<int>(codepoint)) != 0;
}

// Characters that are not drawn and take no room: the zero-width space and
// joiners, the word joiner, the byte-order mark, and the marks that steer the
// direction of text. A face that lacks them -- most do -- would draw each as
// the box a missing character is, in the middle of a translated line.
[[nodiscard]] bool zeroWidth(u32 codepoint) noexcept
{
    return (codepoint >= 0x200B && codepoint <= 0x200F) || (codepoint >= 0x202A && codepoint <= 0x202E) ||
           (codepoint >= 0x2060 && codepoint <= 0x2064) || (codepoint >= 0x2066 && codepoint <= 0x2069) ||
           codepoint == 0xFEFF;
}

[[nodiscard]] usize glyphIndex(FaceChain& chain, f32 pixelSize, u32 codepoint, GlyphStroke stroke = {})
{
    GlyphStore& cache = store();
    Face& face = chain.primary();
    const bool drawable = codepoint >= FirstFaceCodepoint && codepoint <= LastFaceCodepoint;
    // Cached under the codepoint that was ASKED for, even when the answer is the
    // replacement box, so `missingGlyphs` counts DISTINCT characters the face
    // cannot draw. Sharing one entry for all of them would make the counter say
    // "1" for a label that is entirely boxes, which is the number nobody needs.
    // The one exception is a byte sequence that is not a character at all: those
    // all decode to `ReplacementCodepoint` and share one entry, because "this is
    // not text" is one fact however many times it happens.
    //
    // **By the size it is MADE at, not the size it is asked at**: a raster
    // glyph's rung (`rasterSizeFor`), with its outline as thick as that rung
    // makes it -- and no size at all for the built-in face, whose glyphs are
    // the same rectangles at every size. Its outline is the one thing of it
    // that is in face units a size decides, so an outlined one keeps its size.
    const f32 madeAt = face.ready ? rasterSizeFor(pixelSize) : (stroke.quarters > 0 ? pixelSize : 0.0f);
    GlyphStroke made = stroke;
    if (face.ready && stroke.quarters > 0) {
        const f32 grown = static_cast<f32>(stroke.quarters) * madeAt / std::fmax(pixelSize, 1.0f);
        made.quarters = static_cast<u32>(std::fmin(std::fmax(grown + 0.5f, 1.0f), static_cast<f32>(MaxStrokeQuarters)));
    }
    const u64 key = glyphKey(chain.hash, madeAt, codepoint, made);

    const auto position = std::lower_bound(cache.entries.begin(), cache.entries.end(), key,
                                           [](const GlyphEntry& entry, u64 value) { return entry.key < value; });
    if (position != cache.entries.end() && position->key == key) {
        ++cache.stats.hits;
        // Shown in this frame: its page is not one to empty for another glyph.
        if (position->paged)
            cache.pages[position->page].shownAt = cache.frame;
        return static_cast<usize>(std::distance(cache.entries.begin(), position));
    }

    if (cache.entries.size() >= MaxGlyphEntries) {
        clearStore(cache);
        return glyphIndex(chain, pixelSize, codepoint, stroke);
    }

    GlyphEntry entry;
    entry.key = key;
    bool filled = false;
    if (zeroWidth(codepoint)) {
        // No ink and no advance, cached as a space is.
        entry.quadCount = 0;
        filled = true;
    }
    else if (face.ready) {
        // The first face of the chain that has the character draws it.
        usize slot = 0;
        while (slot < chain.faces.size() &&
               stbtt_FindGlyphIndex(&chain.faces[slot]->info, static_cast<int>(codepoint)) == 0)
            ++slot;
        if (slot < chain.faces.size()) {
            Face& drawing = *chain.faces[slot];
            const f32 top = static_cast<f32>(face.ascent) * stbtt_ScaleForPixelHeight(&face.info, madeAt);
            filled = rasteriseGlyph(drawing, top, madeAt, codepoint, entry, cache, made);
            if (filled)
                entry.face = static_cast<core::u8>(slot);
            else if (atlasFull(cache, drawing, codepoint)) {
                // Every page holds something this frame has shown, and the face
                // does have the glyph: one frame asked for more than the whole
                // atlas. Clearing is the answer the entry limit gets, and the
                // frame is built again (`buildWithSettledGlyphs`).
                clearStore(cache);
                return glyphIndex(chain, pixelSize, codepoint, stroke);
            }
        }
        if (!filled) {
            // No face of the chain has a glyph for this codepoint. The visible
            // box, same as the built-in face gives -- a player seeing boxes
            // knows the font is missing glyphs.
            fillReplacementGlyph(entry, cache.quads);
            entry.textured = false;
            if (stroke.quarters == 0)
                ++cache.stats.missingGlyphs;
            else
                growQuads(entry, cache.quads, static_cast<f32>(stroke.quarters) * 0.25f / scaleFor(face, pixelSize));
            filled = true;
        }
    }
    else if (drawable) {
        fillFaceGlyph(codepoint, entry, cache.quads);
        // A vector glyph is rectangles, so its outline is the rectangles
        // grown: the join has no corner to shape at this size.
        if (stroke.quarters > 0)
            growQuads(entry, cache.quads, static_cast<f32>(stroke.quarters) * 0.25f / scaleFor(face, pixelSize));
        filled = true;
    }
    if (!filled) {
        fillReplacementGlyph(entry, cache.quads);
        if (stroke.quarters == 0)
            ++cache.stats.missingGlyphs;
        else
            growQuads(entry, cache.quads, static_cast<f32>(stroke.quarters) * 0.25f / scaleFor(face, pixelSize));
    }

    // Looked for again: making room for this glyph may have taken a page's
    // entries out from before where it goes.
    const auto place = std::lower_bound(cache.entries.begin(), cache.entries.end(), key,
                                        [](const GlyphEntry& held, u64 value) { return held.key < value; });
    const auto inserted = cache.entries.insert(place, entry);
    ++cache.stats.fills;
    cache.stats.entries = cache.entries.size();
    return static_cast<usize>(std::distance(cache.entries.begin(), inserted));
}

// **The room a line has, with a hair over**. A label sized to its
// words is measured at one size and drawn at that size times the screen's
// scale, in the measured width times the same: one number on paper, and two
// sums of floats a few millionths apart. Wrapping on that difference put the
// last word of a line that fits exactly on a line of its own, outside the box
// made for one. A ten-thousandth of the width is a tenth of a pixel in a
// thousand, and far more than the sums can differ by.
[[nodiscard]] f32 wrapRoomOf(f32 maxWidth) noexcept
{
    return maxWidth * 1.0001f;
}

[[nodiscard]] bool breaksAround(u32 c) noexcept;

// **The letter spacing after a character**, in face units at `pixelSize`
// (`TextLabel.LetterSpacing`): none after an ideograph, a kana or a Hangul
// syllable -- set at their full width, they are spaced by the font and
// tracked text of them reads as gaps -- and none after what takes no room.
[[nodiscard]] f32 trackingOf(const FaceChain& chain, f32 pixelSize, u32 codepoint) noexcept
{
    if (chain.tracking == 0.0f || codepoint == '\n' || zeroWidth(codepoint) || breaksAround(codepoint))
        return 0.0f;
    return chain.tracking * pixelSize / scaleFor(chain.primary(), pixelSize);
}

// The advance of one run, in face units. Every width in this file goes through
// the cache, so a measurement and a draw can never disagree about how wide a
// character is. `faces`, given, gains a bit for each face of the chain the run
// was drawn with.
[[nodiscard]] bool needsShaping(std::string_view text) noexcept;
[[nodiscard]] f32 shapedWidth(std::string_view run, FaceChain& chain, f32 pixelSize, u32* faces);

[[nodiscard]] f32 widthOf(std::string_view run, FaceChain& chain, f32 pixelSize, u32* faces = nullptr)
{
    // Text that needs shaping is as wide as its shaped glyphs (ADR 0260).
    if (chain.primary().ready && needsShaping(run))
        return shapedWidth(run, chain, pixelSize, faces);
    f32 total = 0.0f;
    usize index = 0;
    while (index < run.size()) {
        const Decoded decoded = decodeUtf8(run, index);
        const GlyphEntry& entry = store().entries[glyphIndex(chain, pixelSize, decoded.codepoint)];
        total += entry.advance + trackingOf(chain, pixelSize, decoded.codepoint);
        if (faces != nullptr)
            *faces |= 1u << entry.face;
        index += decoded.length;
    }
    return total;
}

// The faces of the chain a text is drawn with, the label's own always among
// them: what its lines are measured by (`lineMetricsOf`).
[[nodiscard]] u32 facesOf(std::string_view text, FaceChain& chain, f32 pixelSize)
{
    u32 faces = 1u;
    usize index = 0;
    while (index < text.size()) {
        const Decoded decoded = decodeUtf8(text, index);
        if (decoded.codepoint != '\n')
            faces |= 1u << store().entries[glyphIndex(chain, pixelSize, decoded.codepoint)].face;
        index += decoded.length;
    }
    return faces;
}

// --- Text that joins, stacks and runs right to left (ADR 0260) --------------
//
// **Laid out a codepoint at a time unless that would draw it wrong.** Latin,
// Greek, Cyrillic, Chinese, Japanese and Korean are one glyph a character at
// its own advance, and every label of them is laid out as it always was, to
// the bit. A text with a character of a script whose letters change shape by
// their neighbours, stack marks or run right to left -- or a mark combined
// with a letter, or a mark steering direction -- is laid out here instead:
// split into paragraphs and each resolved by the bidirectional algorithm
// (SheenBidi), into runs of one direction, one script and one face of the
// chain, each shaped (HarfBuzz) into the face's own glyphs at its own
// positions; broken into lines between clusters, never inside one, by the
// rules above and, for Thai and Lao, a word list; and each line put in the
// order it is read on screen. The glyphs are drawn by their index in the
// face, from the same store and atlas.

// What the codepoint-at-a-time layout cannot draw: the scripts whose letters
// join, reorder or stack, the right-to-left ones, the combining marks, and the
// marks that set a direction -- the left-to-right and right-to-left marks,
// the embeddings, overrides and isolates.
//
// **Not the joiners, the zero-width space, the word joiner or the no-break
// space.** They change nothing in text without one of the above -- each is
// drawn as nothing, or as a space -- and a game puts them in plain text on
// purpose: one before a title is what a game's own letter spacing marks its
// titles with. Seen as a reason to shape, they moved every such Latin title
// by a fraction of a pixel. In text that has a reason, they are shaped with
// it.
[[nodiscard]] bool complexCodepoint(u32 c) noexcept
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0590 && c <= 0x08FF) ||
           (c >= 0x0900 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0FFF) || (c >= 0x1000 && c <= 0x109F) ||
           (c >= 0x1700 && c <= 0x18AF) || (c >= 0x1900 && c <= 0x1C7F) || (c >= 0x1CD0 && c <= 0x1CFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xA800 && c <= 0xA8FF) ||
           (c >= 0xA900 && c <= 0xA9FF) || (c >= 0xAA00 && c <= 0xAAFF) || (c >= 0xABC0 && c <= 0xABFF) ||
           (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0xFE70 && c <= 0xFEFE) ||
           (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x11000 && c <= 0x11FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
}

[[nodiscard]] bool needsShaping(std::string_view text) noexcept
{
    for (usize at = 0; at < text.size();) {
        // Below U+0300 is ASCII, Latin-1 and the extended Latin letters, and a
        // continuation byte starts nothing.
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead < 0xCC) {
            ++at;
            continue;
        }
        const Decoded decoded = decodeUtf8(text, at);
        if (complexCodepoint(decoded.codepoint))
            return true;
        at += decoded.length;
    }
    return false;
}

// The face as HarfBuzz reads it, made on first use. Null for the built-in
// vector face, which has nothing to shape with -- and a chain of it is never
// shaped.
[[nodiscard]] hb_font_t* shaperOf(Face& face)
{
    if (!face.ready)
        return nullptr;
    if (face.shaper == nullptr) {
        hb_blob_t* blob =
            hb_blob_create(reinterpret_cast<const char*>(face.data.data()), static_cast<unsigned>(face.data.size()),
                           HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        // The collection's first face, as `initFace` takes.
        hb_face_t* read = hb_face_create(blob, 0);
        hb_blob_destroy(blob);
        hb_font_t* font = hb_font_create(read);
        const auto units = static_cast<int>(hb_face_get_upem(read));
        hb_face_destroy(read);
        hb_font_set_scale(font, units, units);
        face.shaper.reset(font);
    }
    return face.shaper.get();
}

// The key a glyph of a shaped run is stored under: the chain and which of its
// faces, since a glyph index means nothing outside its face, and the index
// above every Unicode scalar, so it can never collide with a codepoint's.
constexpr u32 GlyphIndexBase = 0x110000u;

[[nodiscard]] u32 slotHash(const FaceChain& chain, usize slot) noexcept
{
    return (chain.hash ^ (static_cast<u32>(slot) + 1u) * 0x9E3779B1u) * 16777619u;
}

// `glyphIndex` for a glyph a shaped run names by its index in the face of
// slot `slot`: filled from that face, placed from the label's own face's top.
[[nodiscard]] usize shapedGlyphIndex(FaceChain& chain, usize slot, f32 pixelSize, u32 glyph, GlyphStroke stroke = {})
{
    GlyphStore& cache = store();
    Face& face = chain.primary();
    const f32 madeAt = rasterSizeFor(pixelSize);
    GlyphStroke made = stroke;
    if (stroke.quarters > 0) {
        const f32 grown = static_cast<f32>(stroke.quarters) * madeAt / std::fmax(pixelSize, 1.0f);
        made.quarters = static_cast<u32>(std::fmin(std::fmax(grown + 0.5f, 1.0f), static_cast<f32>(MaxStrokeQuarters)));
    }
    const u64 key = glyphKey(slotHash(chain, slot), madeAt, GlyphIndexBase + (glyph & 0xFFFFu), made);
    const auto position = std::lower_bound(cache.entries.begin(), cache.entries.end(), key,
                                           [](const GlyphEntry& entry, u64 value) { return entry.key < value; });
    if (position != cache.entries.end() && position->key == key) {
        ++cache.stats.hits;
        if (position->paged)
            cache.pages[position->page].shownAt = cache.frame;
        return static_cast<usize>(std::distance(cache.entries.begin(), position));
    }
    if (cache.entries.size() >= MaxGlyphEntries) {
        clearStore(cache);
        return shapedGlyphIndex(chain, slot, pixelSize, glyph, stroke);
    }

    GlyphEntry entry;
    entry.key = key;
    Face& drawing = *chain.faces[std::min(slot, chain.faces.size() - 1)];
    const f32 top = static_cast<f32>(face.ascent) * stbtt_ScaleForPixelHeight(&face.info, madeAt);
    if (rasteriseGlyphIndex(drawing, top, madeAt, static_cast<int>(glyph), entry, cache, made)) {
        entry.face = static_cast<core::u8>(slot);
    }
    else if (cache.pages.size() >= kGlyphPages && !cache.entries.empty()) {
        clearStore(cache);
        return shapedGlyphIndex(chain, slot, pixelSize, glyph, stroke);
    }
    else {
        fillReplacementGlyph(entry, cache.quads);
        entry.textured = false;
        if (stroke.quarters > 0)
            growQuads(entry, cache.quads, static_cast<f32>(stroke.quarters) * 0.25f / scaleFor(face, pixelSize));
    }
    const auto place = std::lower_bound(cache.entries.begin(), cache.entries.end(), key,
                                        [](const GlyphEntry& held, u64 value) { return held.key < value; });
    const auto inserted = cache.entries.insert(place, entry);
    ++cache.stats.fills;
    cache.stats.entries = cache.entries.size();
    return static_cast<usize>(std::distance(cache.entries.begin(), inserted));
}

// One glyph of a shaped line, in the order it is drawn: its index in the face
// of the chain that drew it, where its pen is from the line's left, and how
// far the face's own positioning moves it -- in face units at the size the
// glyph is made at, as every advance in this file is. A glyph no face of the
// chain has keeps the character it stands for, and is drawn as the box.
struct PlacedGlyph
{
    u32 glyph = 0;
    u32 codepoint = 0;
    f32 pen = 0.0f;
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    core::u8 slot = 0;
    bool missing = false;
};

// A character of a shaped line as a caret sees it: the bytes it covers, where
// it is drawn on the line, and which way it reads.
struct PlacedCluster
{
    usize begin = 0;
    usize end = 0;
    f32 left = 0.0f;
    f32 right = 0.0f;
    bool rightToLeft = false;
};

struct ShapedLine
{
    // In the order they are drawn, left to right.
    std::vector<PlacedGlyph> glyphs;
    // Left to right too, a character each, with offsets into the paragraph.
    std::vector<PlacedCluster> clusters;
    f32 width = 0.0f;
    u32 faces = 1u;
};

// **A paragraph as the bidirectional algorithm resolved it** (UAX #9), and
// the script of each of its bytes: what its lines are ordered and its runs
// split by. Its direction is its first strong character's, and left to right
// when it has none.
class Paragraph
{
public:
    explicit Paragraph(std::string_view text) : m_text(text)
    {
        SBCodepointSequence sequence{SBStringEncodingUTF8, const_cast<char*>(text.data()), text.size()};
        m_algorithm = SBAlgorithmCreate(&sequence);
        m_paragraph = SBAlgorithmCreateParagraph(m_algorithm, 0, text.size(), SBLevelDefaultLTR);
        m_scripts.assign(text.size(), SBScriptZYYY);
        SBScriptLocatorRef locator = SBScriptLocatorCreate();
        SBScriptLocatorLoadCodepoints(locator, &sequence);
        while (SBScriptLocatorMoveNext(locator) != 0) {
            const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator);
            for (SBUInteger at = agent->offset; at < agent->offset + agent->length && at < text.size(); ++at)
                m_scripts[at] = agent->script;
        }
        SBScriptLocatorRelease(locator);
    }
    ~Paragraph()
    {
        if (m_paragraph != nullptr)
            SBParagraphRelease(m_paragraph);
        if (m_algorithm != nullptr)
            SBAlgorithmRelease(m_algorithm);
    }
    Paragraph(const Paragraph&) = delete;
    Paragraph& operator=(const Paragraph&) = delete;

    [[nodiscard]] std::string_view text() const noexcept { return m_text; }
    [[nodiscard]] SBParagraphRef resolved() const noexcept { return m_paragraph; }
    [[nodiscard]] SBScript scriptAt(usize at) const noexcept { return m_scripts[at]; }

private:
    std::string_view m_text;
    SBAlgorithmRef m_algorithm = nullptr;
    SBParagraphRef m_paragraph = nullptr;
    std::vector<SBScript> m_scripts;
};

// The face of the chain a character is drawn in: the first that has it.
// `chain.faces.size()` when none has.
[[nodiscard]] usize slotFor(const FaceChain& chain, u32 codepoint) noexcept
{
    for (usize slot = 0; slot < chain.faces.size(); ++slot) {
        if (stbtt_FindGlyphIndex(&chain.faces[slot]->info, static_cast<int>(codepoint)) != 0)
            return slot;
    }
    return chain.faces.size();
}

// The scripts whose letters join: tracked text of them would pull the joins
// apart, so `LetterSpacing` leaves them as they are.
[[nodiscard]] bool joiningScript(SBScript script) noexcept
{
    return script == SBScriptARAB || script == SBScriptSYRC || script == SBScriptNKOO || script == SBScriptMONG ||
           script == SBScriptMAND || script == SBScriptADLM || script == SBScriptROHG;
}

// The buffer every run is shaped in: one, emptied for each, since a layout
// is one thread's work at a time.
[[nodiscard]] hb_buffer_t* shapingBuffer()
{
    static hb_buffer_t* buffer = hb_buffer_create();
    return buffer;
}

// **One line of a paragraph, shaped and in the order it is read on screen**:
// the bytes [`begin`, `end`) of it, split into the runs the algorithm orders,
// each split again where the script or the face changes, and each piece shaped
// with the whole paragraph around it -- so a letter at a line's end joins as
// it does mid-line.
[[nodiscard]] ShapedLine shapeLine(const Paragraph& paragraph, usize begin, usize end, FaceChain& chain, f32 pixelSize)
{
    ShapedLine out;
    if (end <= begin)
        return out;
    const std::string_view text = paragraph.text();
    const f32 madeAt = rasterSizeFor(pixelSize);
    hb_buffer_t* buffer = shapingBuffer();

    SBLineRef line = SBParagraphCreateLine(paragraph.resolved(), begin, end - begin);
    const SBUInteger runCount = line != nullptr ? SBLineGetRunCount(line) : 0;
    const SBRun* runs = line != nullptr ? SBLineGetRunsPtr(line) : nullptr;

    // Each byte that starts a cluster, its extent on the line, and its
    // direction: what the caret's characters are cut from.
    struct Extent
    {
        usize start = 0;
        f32 left = 0.0f;
        f32 right = 0.0f;
        bool rightToLeft = false;
    };
    std::vector<Extent> extents;

    f32 pen = 0.0f;
    for (SBUInteger runIndex = 0; runIndex < runCount; ++runIndex) {
        const SBRun& run = runs[runIndex];
        const bool rightToLeft = (run.level & 1u) != 0;
        // The run cut where the script or the face changes, in logical order.
        // A mark, a joiner and a character no face has go with what is before
        // them, so a cluster is shaped in one face.
        struct Item
        {
            usize begin = 0;
            usize end = 0;
            usize slot = 0;
            SBScript script = SBScriptZYYY;
        };
        std::vector<Item> items;
        for (usize at = run.offset; at < run.offset + run.length;) {
            const Decoded decoded = decodeUtf8(text, at);
            const SBScript script = paragraph.scriptAt(at);
            usize slot = slotFor(chain, decoded.codepoint);
            const bool follows =
                !items.empty() &&
                (slot == chain.faces.size() || zeroWidth(decoded.codepoint) ||
                 SBGeneralCategoryIsMark(SBCodepointGetGeneralCategory(static_cast<SBCodepoint>(decoded.codepoint))));
            if (follows)
                slot = items.back().slot;
            else if (slot == chain.faces.size())
                slot = 0;
            if (items.empty() || items.back().slot != slot || items.back().script != script)
                items.push_back(Item{at, at, slot, script});
            items.back().end = at + decoded.length;
            at += decoded.length;
        }
        if (rightToLeft)
            std::reverse(items.begin(), items.end());

        for (const Item& item : items) {
            Face& face = *chain.faces[item.slot];
            hb_font_t* font = shaperOf(face);
            const f32 units = stbtt_ScaleForPixelHeight(&face.info, madeAt);
            hb_buffer_clear_contents(buffer);
            hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), static_cast<unsigned>(item.begin),
                               static_cast<int>(item.end - item.begin));
            hb_buffer_set_direction(buffer, rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(SBScriptGetUnicodeTag(item.script)));
            hb_buffer_guess_segment_properties(buffer);
            hb_shape(font, buffer, nullptr, 0);
            unsigned count = 0;
            const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
            const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
            const bool tracked = chain.tracking != 0.0f && !joiningScript(item.script);
            for (unsigned at = 0; at < count; ++at) {
                PlacedGlyph glyph;
                glyph.glyph = infos[at].codepoint;
                glyph.slot = static_cast<core::u8>(item.slot);
                glyph.pen = pen;
                glyph.dx = static_cast<f32>(positions[at].x_offset) * units;
                glyph.dy = static_cast<f32>(positions[at].y_offset) * units;
                const usize cluster = infos[at].cluster;
                f32 advance = static_cast<f32>(positions[at].x_advance) * units;
                const u32 codepoint = decodeUtf8(text, cluster).codepoint;
                if (glyph.glyph == 0 && !zeroWidth(codepoint)) {
                    // No face of the chain has it: the box, at the box's width.
                    glyph.missing = true;
                    glyph.codepoint = codepoint;
                    advance = store().entries[glyphIndex(chain, pixelSize, codepoint)].advance;
                }
                // Tracked after a cluster's last glyph, in the order it reads.
                const bool lastOfCluster = at + 1 == count || infos[at + 1].cluster != infos[at].cluster;
                if (tracked && lastOfCluster)
                    advance += trackingOf(chain, pixelSize, codepoint);
                out.faces |= 1u << item.slot;
                out.glyphs.push_back(glyph);
                const auto known = std::find_if(extents.begin(), extents.end(),
                                                [&](const Extent& extent) { return extent.start == cluster; });
                if (known == extents.end())
                    extents.push_back(Extent{cluster, pen, pen + advance, rightToLeft});
                else {
                    known->left = std::fmin(known->left, pen);
                    known->right = std::fmax(known->right, pen + advance);
                }
                pen += advance;
            }
        }
    }
    if (line != nullptr)
        SBLineRelease(line);
    out.width = pen;

    // **The caret's characters**: each cluster from its start to the next
    // one's, and one the face joined into a single glyph -- a ligature --
    // shared out among the characters in it, in the order it reads.
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
    for (usize index = 0; index < extents.size(); ++index) {
        const Extent& extent = extents[index];
        const usize stop = index + 1 < extents.size() ? extents[index + 1].start : end;
        std::vector<std::pair<usize, usize>> characters;
        for (usize at = extent.start; at < stop;) {
            const usize next = std::min<usize>(nextCharacter(text, static_cast<u32>(at)), stop);
            characters.emplace_back(at, next);
            at = next;
        }
        const f32 share = (extent.right - extent.left) / static_cast<f32>(std::max<usize>(characters.size(), 1));
        for (usize part = 0; part < characters.size(); ++part) {
            const f32 from = extent.rightToLeft ? extent.right - share * static_cast<f32>(part + 1)
                                                : extent.left + share * static_cast<f32>(part);
            out.clusters.push_back(
                PlacedCluster{characters[part].first, characters[part].second, from, from + share, extent.rightToLeft});
        }
    }
    std::sort(out.clusters.begin(), out.clusters.end(),
              [](const PlacedCluster& a, const PlacedCluster& b) { return a.left < b.left; });
    return out;
}

f32 shapedWidth(std::string_view run, FaceChain& chain, f32 pixelSize, u32* faces)
{
    f32 widest = 0.0f;
    for (usize begin = 0; begin <= run.size();) {
        usize end = run.find('\n', begin);
        if (end == std::string_view::npos)
            end = run.size();
        const Paragraph paragraph(run.substr(begin, end - begin));
        const ShapedLine line = shapeLine(paragraph, 0, end - begin, chain, pixelSize);
        widest = std::fmax(widest, line.width);
        if (faces != nullptr)
            *faces |= line.faces;
        begin = end + 1;
    }
    return widest;
}

// --- Where a line may end ----------------------------------------------------
//
// **The part of the Unicode line-breaking algorithm (UAX #14) that text with no
// spaces needs** (ADR 0259). Chinese and Japanese put no space between words,
// and a line of them breaks between any two characters -- except where the
// typesetting rule the Japanese call kinsoku forbids it: no line begins with a
// closing bracket, a comma, a full stop or a small kana, and none ends with an
// opening bracket. Korean puts spaces between words and breaks between
// syllables too. Latin text still breaks at its spaces, and a word in it is
// never cut while a better place exists. A zero-width space marks a place a
// line may end, and a no-break space is one it may not.

// Ideographs, kana, Hangul syllables and the CJK punctuation and full-width
// forms: what a line may break before or after.
[[nodiscard]] bool breaksAround(u32 c) noexcept
{
    return (c >= 0x2E80 && c <= 0x2FFF) || // radicals, ideographic description
           (c >= 0x3000 && c <= 0x30FF) || // CJK punctuation, hiragana, katakana
           (c >= 0x3105 && c <= 0x312F) || // bopomofo
           (c >= 0x3190 && c <= 0x31FF) || // kanbun, CJK strokes, small katakana
           (c >= 0x3200 && c <= 0x4DBF) || // enclosed and compatibility CJK, extension A
           (c >= 0x4E00 && c <= 0x9FFF) || // the unified ideographs
           (c >= 0xAC00 && c <= 0xD7A3) || // Hangul syllables
           (c >= 0xF900 && c <= 0xFAFF) || // compatibility ideographs
           (c >= 0xFE30 && c <= 0xFE4F) || // CJK compatibility forms
           (c >= 0xFF01 && c <= 0xFFEF) || // full-width and half-width forms
           (c >= 0x20000 && c <= 0x3FFFF); // the supplementary ideographic planes
}

// What no line may begin with: closing brackets, the stops and commas, the
// marks that lengthen or repeat what is before them, and the small kana.
[[nodiscard]] bool startsNoLine(u32 c) noexcept
{
    static constexpr u32 Closers[] = {
        0x21,   0x29,   0x2C,   0x2E,   0x3A,   0x3B,   0x3F,   0x5D,   0x7D,   0x2019, 0x201D, 0x203C, 0x2047, 0x2048,
        0x2049, 0x3001, 0x3002, 0x3005, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017, 0x3019, 0x301B, 0x301C,
        0x301E, 0x301F, 0x303B, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E, 0x3095,
        0x3096, 0x309B, 0x309C, 0x309D, 0x309E, 0x30A0, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5,
        0x30E7, 0x30EE, 0x30F5, 0x30F6, 0x30FB, 0x30FC, 0x30FD, 0x30FE, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B,
        0xFF1F, 0xFF3D, 0xFF5D, 0xFF60, 0xFF61, 0xFF63, 0xFF64, 0xFF65, 0xFF9E, 0xFF9F};
    if ((c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF67 && c <= 0xFF70))
        return true;
    return std::binary_search(std::begin(Closers), std::end(Closers), c);
}

// What no line may end with: the opening brackets and quotes.
[[nodiscard]] bool endsNoLine(u32 c) noexcept
{
    static constexpr u32 Openers[] = {0x28,   0x5B,   0x7B,   0x2018, 0x201C, 0x3008, 0x300A, 0x300C, 0x300E, 0x3010,
                                      0x3014, 0x3016, 0x3018, 0x301A, 0x301D, 0xFF08, 0xFF3B, 0xFF5B, 0xFF5F, 0xFF62};
    return std::binary_search(std::begin(Openers), std::end(Openers), c);
}

// Whether a line may end between `before` and `after`. Spaces are not asked
// about: a line ends at a space and drops it, which the callers do.
[[nodiscard]] bool breaksBetween(u32 before, u32 after) noexcept
{
    // A no-break space (and its narrow form) and the word joiner hold what is
    // either side of them together; a zero-width space is a place to break,
    // after it and not before.
    const auto glue = [](u32 c) { return c == 0x00A0 || c == 0x202F || c == 0x2060 || c == 0xFEFF; };
    if (before == ' ' || after == ' ' || glue(before) || glue(after) || after == 0x200B)
        return false;
    if (before == 0x200B)
        return true;
    if (startsNoLine(after) || endsNoLine(before))
        return false;
    return breaksAround(before) || breaksAround(after);
}

// One measured line: how wide it is, which bytes it covers, and a bit for each
// face of the chain its characters were drawn with.
// A line of text that needs shaping carries its glyphs, already in the order
// they are drawn.
struct Line
{
    usize begin = 0;
    usize end = 0;
    f32 width = 0.0f;
    u32 faces = 1u;
    std::shared_ptr<const ShapedLine> shaped;
};

// **A paragraph that needs shaping, broken into lines** at `maxWidth` by the
// rules `breakLines` follows -- a space, a place between two ideographs -- and
// between two words of Thai or Lao, but only ever between two clusters: a
// letter and its marks, or the letters a face joined into one glyph, are never
// parted. Measured whole and in logical order; each line then shaped again by
// itself, in display order.
void breakShapedParagraph(std::string_view text, usize offset, FaceChain& chain, f32 pixelSize, f32 scale, f32 maxWidth,
                          std::vector<Line>& out)
{
    const Paragraph paragraph(text);
    const auto lineOf = [&](usize begin, usize end) {
        auto shaped = std::make_shared<ShapedLine>(shapeLine(paragraph, begin, end, chain, pixelSize));
        Line line;
        line.begin = offset + begin;
        line.end = offset + end;
        line.width = shaped->width * scale;
        line.faces = shaped->faces | 1u;
        line.shaped = std::move(shaped);
        return line;
    };
    if (maxWidth <= 0.0f || text.empty()) {
        out.push_back(lineOf(0, text.size()));
        return;
    }

    // Each cluster's advance at the byte it starts at, from the paragraph
    // laid out as one line.
    std::vector<f32> advanceAt(text.size() + 1, 0.0f);
    std::vector<bool> clusterAt(text.size() + 1, false);
    {
        const ShapedLine whole = shapeLine(paragraph, 0, text.size(), chain, pixelSize);
        for (const PlacedCluster& cluster : whole.clusters) {
            advanceAt[cluster.begin] += cluster.right - cluster.left;
            clusterAt[cluster.begin] = true;
        }
    }
    std::vector<bool> wordAt(text.size() + 1, false);
    {
        std::vector<usize> words;
        detail::dictionaryBreaks(text, words);
        for (const usize at : words)
            wordAt[at] = true;
    }
    const auto breakable = [&](usize at, u32 before, u32 after) {
        return clusterAt[at] && (breaksBetween(before, after) || (wordAt[at] && before != ' ' && after != ' '));
    };

    usize lineBegin = 0;
    usize breakEnd = std::string_view::npos;
    usize breakNext = std::string_view::npos;
    u32 previous = 0;
    f32 running = 0.0f;
    const auto flush = [&](usize end, usize nextBegin) {
        out.push_back(lineOf(lineBegin, end));
        lineBegin = nextBegin;
        breakEnd = std::string_view::npos;
        breakNext = std::string_view::npos;
        previous = 0;
        running = 0.0f;
    };
    usize index = 0;
    while (index < text.size()) {
        const Decoded decoded = decodeUtf8(text, index);
        const u32 codepoint = decoded.codepoint;
        if (codepoint == ' ') {
            breakEnd = index;
            breakNext = index + 1;
        }
        const f32 advance = advanceAt[index];
        if (clusterAt[index] && index > lineBegin && (running + advance) * scale > wrapRoomOf(maxWidth)) {
            if (breakable(index, previous, codepoint)) {
                flush(index, index);
                continue;
            }
            if (breakEnd != std::string_view::npos && breakEnd > lineBegin) {
                flush(breakEnd, breakNext);
                index = lineBegin;
                continue;
            }
            flush(index, index);
            continue;
        }
        if (index > lineBegin && breakable(index, previous, codepoint)) {
            breakEnd = index;
            breakNext = index;
        }
        running += advance;
        previous = codepoint;
        index += decoded.length;
    }
    flush(text.size(), text.size());
}

void breakShapedLines(std::string_view text, FaceChain& chain, f32 pixelSize, f32 scale, f32 maxWidth,
                      std::vector<Line>& out)
{
    for (usize begin = 0; begin <= text.size();) {
        usize end = text.find('\n', begin);
        if (end == std::string_view::npos)
            end = text.size();
        breakShapedParagraph(text.substr(begin, end - begin), begin, chain, pixelSize, scale, maxWidth, out);
        begin = end + 1;
    }
}

// Breaks `text` into lines at `maxWidth`: at the last place the rules above
// allow, and mid-word only where a line has none -- a word wider than the box.
// A word cut at a random letter is worse than one that overhangs, which is
// what `TextWrapped`'s doc promises.
void breakLines(std::string_view text, FaceChain& chain, f32 pixelSize, f32 scale, f32 maxWidth, std::vector<Line>& out)
{
    out.clear();
    if (chain.primary().ready && needsShaping(text)) {
        breakShapedLines(text, chain, pixelSize, scale, maxWidth, out);
        return;
    }
    if (text.empty()) {
        out.push_back(Line{});
        return;
    }

    usize lineBegin = 0;
    // The last place this line may end, and where the next then begins: a
    // space ends it and is dropped, a break between two ideographs drops
    // nothing.
    usize breakEnd = std::string_view::npos;
    usize breakNext = std::string_view::npos;
    // The character before `index` on this line; none at its start.
    u32 previous = 0;
    usize index = 0;
    // The line so far, in face units: carried along rather than measured from
    // the line's start at every character, which made wrapping a long text
    // quadratic in its length. The same advances in the same order, so
    // the same sum.
    f32 running = 0.0f;

    const auto flush = [&](usize end, usize nextBegin) {
        Line line;
        line.begin = lineBegin;
        line.end = end;
        line.width = widthOf(text.substr(lineBegin, end - lineBegin), chain, pixelSize, &line.faces) * scale;
        out.push_back(line);
        lineBegin = nextBegin;
        breakEnd = std::string_view::npos;
        breakNext = std::string_view::npos;
        previous = 0;
        running = 0.0f;
    };

    while (index < text.size()) {
        if (text[index] == '\n') {
            flush(index, index + 1);
            ++index;
            continue;
        }
        // Advanced by CODEPOINT, so a multi-byte character is one unit of
        // wrapping rather than two or three -- and so a break can never land in
        // the middle of one and produce two invalid sequences.
        const Decoded decoded = decodeUtf8(text, index);
        const u32 codepoint = decoded.codepoint;
        if (codepoint == ' ') {
            breakEnd = index;
            breakNext = index + 1;
        }
        const f32 advance =
            store().entries[glyphIndex(chain, pixelSize, codepoint)].advance + trackingOf(chain, pixelSize, codepoint);

        if (maxWidth > 0.0f) {
            const f32 width = (running + advance) * scale;
            if (width > wrapRoomOf(maxWidth) && index > lineBegin) {
                // Before this character when the two either side allow it;
                // else at the last place that did.
                if (breaksBetween(previous, codepoint)) {
                    flush(index, index);
                    continue;
                }
                if (breakEnd != std::string_view::npos && breakEnd > lineBegin) {
                    // At a space, it is dropped: a trailing space would make a
                    // centred line sit visibly left of centre.
                    flush(breakEnd, breakNext);
                    index = lineBegin;
                    continue;
                }
                flush(index, index);
                continue;
            }
        }
        if (index > lineBegin && breaksBetween(previous, codepoint)) {
            breakEnd = index;
            breakNext = index;
        }
        running += advance;
        previous = codepoint;
        index += decoded.length;
    }
    flush(text.size(), text.size());
}

// **Text broken into lines once, not every frame**: by its bytes, its
// face, its size and its width. A label that moves, fades or is drawn again
// asks the same question, and a screen of them laid out again because one
// moved asked it of every one -- breaking each into lines and measuring each
// line every frame. The face is part of the key, so the cache is emptied when
// faces are (`resetGlyphCache`); bounded, and emptied when full.
struct LineCache
{
    std::unordered_map<std::string, std::vector<Line>> entries;
    std::string key;
};

[[nodiscard]] LineCache& lineCache()
{
    static LineCache cache;
    return cache;
}

[[nodiscard]] const std::vector<Line>& linesOf(std::string_view text, FaceChain& chain, f32 pixelSize, f32 scale,
                                               f32 maxWidth)
{
    LineCache& cache = lineCache();
    cache.key.assign(text);
    const FaceChain* faceAddress = &chain;
    const auto append = [&cache](const void* data, usize size) {
        cache.key.append(static_cast<const char*>(data), size);
    };
    cache.key.push_back('\0');
    append(&faceAddress, sizeof(faceAddress));
    append(&pixelSize, sizeof(pixelSize));
    append(&maxWidth, sizeof(maxWidth));
    if (const auto found = cache.entries.find(cache.key); found != cache.entries.end()) {
        ++store().stats.lineHits;
        return found->second;
    }
    ++store().stats.lineMisses;
    constexpr usize MostEntries = 4096;
    if (cache.entries.size() >= MostEntries)
        cache.entries.clear();
    std::vector<Line> lines;
    breakLines(text, chain, pixelSize, scale, maxWidth, lines);
    return cache.entries.emplace(cache.key, std::move(lines)).first->second;
}

// --- Rich text ------------------------------------------------------------
//
// **Markup in, styled codepoints out, then the same three steps as plain
// text**: break into lines at spaces, align, emit a quad per glyph. What a
// style changes is the size each codepoint is measured and drawn at, its
// colour, and four decorations. What it does not change is the cache key --
// face, size, codepoint -- which is why nothing below touches the store's
// shape.
//
// The tags are the ones every engine's rich text agrees on: `<b>`, `<i>`,
// `<u>`, `<s>`, `<font color="#rrggbb" size="n" transparency="t">` and `<br/>`,
// with the five XML entities. A tag this parser does not recognise, or one it
// cannot read, is TEXT: a label that shows `<blink>` tells its author exactly
// what went wrong, and one that silently dropped it would not.

struct RichStyle
{
    Color3 color;
    f32 alpha = 1.0f;
    f32 size = 14.0f;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    // The label's `UIStroke`, or a `<stroke>` tag's (ADR 0110).
    TextStroke stroke;
};

struct RichGlyph
{
    u32 codepoint = 0;
    u32 style = 0;
};

struct RichText
{
    std::vector<RichStyle> styles;
    std::vector<RichGlyph> glyphs;
};

[[nodiscard]] bool startsWith(std::string_view text, usize at, std::string_view prefix) noexcept
{
    return text.substr(at, prefix.size()) == prefix;
}

// An attribute's value, quoted with either quote, or nothing.
[[nodiscard]] std::optional<std::string_view> attribute(std::string_view tag, std::string_view name)
{
    usize at = 0;
    while ((at = tag.find(name, at)) != std::string_view::npos) {
        const bool boundary = at == 0 || tag[at - 1] == ' ';
        usize cursor = at + name.size();
        while (cursor < tag.size() && tag[cursor] == ' ')
            ++cursor;
        if (!boundary || cursor >= tag.size() || tag[cursor] != '=') {
            at += name.size();
            continue;
        }
        ++cursor;
        while (cursor < tag.size() && tag[cursor] == ' ')
            ++cursor;
        if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\''))
            return std::nullopt;
        const char quote = tag[cursor];
        const usize end = tag.find(quote, cursor + 1);
        if (end == std::string_view::npos)
            return std::nullopt;
        return tag.substr(cursor + 1, end - cursor - 1);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<f32> numberOf(std::string_view text)
{
    if (text.empty() || text.size() > 32)
        return std::nullopt;
    char buffer[33] = {};
    std::memcpy(buffer, text.data(), text.size());
    char* end = nullptr;
    const f32 value = std::strtof(buffer, &end);
    if (end != buffer + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// `#rrggbb`, or `rgb(r, g, b)` in 0-255.
[[nodiscard]] std::optional<Color3> colourOf(std::string_view text)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if (text.size() == 7 && text[0] == '#') {
        f32 channels[3] = {};
        for (usize index = 0; index < 3; ++index) {
            const int high = hex(text[1 + index * 2]);
            const int low = hex(text[2 + index * 2]);
            if (high < 0 || low < 0)
                return std::nullopt;
            channels[index] = static_cast<f32>(high * 16 + low) / 255.0f;
        }
        return Color3{channels[0], channels[1], channels[2]};
    }
    if (text.size() > 5 && text.substr(0, 4) == "rgb(" && text.back() == ')') {
        const std::string_view inner = text.substr(4, text.size() - 5);
        f32 channels[3] = {};
        usize begin = 0;
        for (int index = 0; index < 3; ++index) {
            const usize comma = index < 2 ? inner.find(',', begin) : inner.size();
            if (comma == std::string_view::npos)
                return std::nullopt;
            std::string_view part = inner.substr(begin, comma - begin);
            while (!part.empty() && part.front() == ' ')
                part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ')
                part.remove_suffix(1);
            const std::optional<f32> value = numberOf(part);
            if (!value.has_value())
                return std::nullopt;
            channels[index] = std::clamp(*value, 0.0f, 255.0f) / 255.0f;
            begin = comma + 1;
        }
        return Color3{channels[0], channels[1], channels[2]};
    }
    return std::nullopt;
}

// Parses `markup` into styled codepoints. `base` is the label's own style.
[[nodiscard]] RichText parseRichText(std::string_view markup, const RichStyle& base)
{
    RichText out;
    out.styles.push_back(base);
    std::vector<u32> stack{0};
    // Which tag opened each stack level, so `</b>` closes a `<b>` and nothing
    // else. A close that matches nothing open is text.
    std::vector<std::string_view> opened{""};

    const auto push = [&](RichStyle style, std::string_view name) {
        out.styles.push_back(style);
        stack.push_back(static_cast<u32>(out.styles.size() - 1));
        opened.push_back(name);
    };
    const auto literal = [&](usize at, usize length) {
        usize index = at;
        while (index < at + length) {
            const Decoded decoded = decodeUtf8(markup, index);
            out.glyphs.push_back(RichGlyph{decoded.codepoint, stack.back()});
            index += decoded.length;
        }
    };

    usize index = 0;
    while (index < markup.size()) {
        const char c = markup[index];
        if (c == '&') {
            static constexpr std::pair<std::string_view, u32> Entities[] = {
                {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool matched = false;
            for (const auto& entity : Entities) {
                if (startsWith(markup, index, entity.first)) {
                    out.glyphs.push_back(RichGlyph{entity.second, stack.back()});
                    index += entity.first.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                literal(index, 1);
                ++index;
            }
            continue;
        }
        if (c != '<') {
            const Decoded decoded = decodeUtf8(markup, index);
            out.glyphs.push_back(RichGlyph{decoded.codepoint, stack.back()});
            index += decoded.length;
            continue;
        }

        const usize close = markup.find('>', index);
        if (close == std::string_view::npos) {
            literal(index, markup.size() - index);
            break;
        }
        std::string_view tag = markup.substr(index + 1, close - index - 1);
        const usize whole = close - index + 1;
        while (!tag.empty() && tag.back() == ' ')
            tag.remove_suffix(1);

        bool understood = true;
        if (tag == "br/" || tag == "br" || tag == "br /") {
            out.glyphs.push_back(RichGlyph{'\n', stack.back()});
        }
        else if (!tag.empty() && tag.front() == '/') {
            const std::string_view name = tag.substr(1);
            if (opened.size() > 1 && opened.back() == name) {
                stack.pop_back();
                opened.pop_back();
            }
            else {
                understood = false;
            }
        }
        else {
            RichStyle style = out.styles[stack.back()];
            const usize space = tag.find(' ');
            const std::string_view name = tag.substr(0, space);
            if (name == "b")
                style.bold = true;
            else if (name == "i")
                style.italic = true;
            else if (name == "u")
                style.underline = true;
            else if (name == "s")
                style.strike = true;
            else if (name == "stroke") {
                // `<stroke color thickness transparency joins sizing>` (ADR
                // 0110), with `th` and `tr` as the short forms. Every attribute
                // is optional: a bare `<stroke>` is a one-pixel black outline.
                const std::string_view rest = space != std::string_view::npos ? tag.substr(space + 1) : "";
                TextStroke outline;
                outline.thickness = 1.0f;
                if (const std::optional<std::string_view> colour = attribute(rest, "color")) {
                    const std::optional<Color3> parsed = colourOf(*colour);
                    understood = understood && parsed.has_value();
                    if (parsed)
                        outline.color = *parsed;
                }
                std::optional<std::string_view> thickness = attribute(rest, "thickness");
                if (!thickness)
                    thickness = attribute(rest, "th");
                if (thickness) {
                    const std::optional<f32> parsed = numberOf(*thickness);
                    understood = understood && parsed.has_value() && *parsed >= 0.0f;
                    if (parsed && *parsed >= 0.0f)
                        outline.thickness = std::min(*parsed, 64.0f);
                }
                std::optional<std::string_view> transparency = attribute(rest, "transparency");
                if (!transparency)
                    transparency = attribute(rest, "tr");
                if (transparency) {
                    const std::optional<f32> parsed = numberOf(*transparency);
                    understood = understood && parsed.has_value();
                    if (parsed)
                        outline.alpha = 1.0f - std::clamp(*parsed, 0.0f, 1.0f);
                }
                if (const std::optional<std::string_view> joins = attribute(rest, "joins")) {
                    if (*joins == "round")
                        outline.join = 0;
                    else if (*joins == "bevel")
                        outline.join = 1;
                    else if (*joins == "miter")
                        outline.join = 2;
                    else
                        understood = false;
                }
                if (const std::optional<std::string_view> sizing = attribute(rest, "sizing")) {
                    if (*sizing == "scaled")
                        outline.scaled = true;
                    else if (*sizing != "fixed")
                        understood = false;
                }
                style.stroke = outline;
            }
            else if (name == "font" && space != std::string_view::npos) {
                const std::string_view rest = tag.substr(space + 1);
                const std::optional<std::string_view> colour = attribute(rest, "color");
                const std::optional<std::string_view> size = attribute(rest, "size");
                const std::optional<std::string_view> transparency = attribute(rest, "transparency");
                if (!colour && !size && !transparency)
                    understood = false;
                if (colour) {
                    const std::optional<Color3> parsed = colourOf(*colour);
                    understood = understood && parsed.has_value();
                    if (parsed)
                        style.color = *parsed;
                }
                if (size) {
                    const std::optional<f32> parsed = numberOf(*size);
                    understood = understood && parsed.has_value() && *parsed > 0.0f;
                    if (parsed && *parsed > 0.0f)
                        style.size = std::min(*parsed, 512.0f);
                }
                if (transparency) {
                    const std::optional<f32> parsed = numberOf(*transparency);
                    understood = understood && parsed.has_value();
                    if (parsed)
                        style.alpha = base.alpha * (1.0f - std::clamp(*parsed, 0.0f, 1.0f));
                }
            }
            else
                understood = false;
            if (understood)
                push(style, name);
        }

        if (!understood)
            literal(index, whole);
        index += whole;
    }
    return out;
}

// Faux bold: the glyph drawn twice, this far apart, and advanced by as much.
// A face with a real bold file is `Font`'s business; this is what one face can
// honestly do.
[[nodiscard]] f32 boldOffsetOf(f32 size) noexcept
{
    return std::max(1.0f, std::round(size / 18.0f));
}

// The slant an italic run is drawn with: the top of a glyph moves this many
// pixels right per pixel of height.
constexpr f32 ItalicSlant = 0.2f;

struct RichLine
{
    usize begin = 0;
    usize end = 0;
    f32 width = 0.0f;
    f32 ascent = 0.0f;
    f32 height = 0.0f;
};

[[nodiscard]] f32 advanceOf(FaceChain& chain, const RichStyle& style, u32 codepoint)
{
    const f32 scale = scaleFor(chain.primary(), style.size);
    const f32 advance =
        (store().entries[glyphIndex(chain, style.size, codepoint)].advance + trackingOf(chain, style.size, codepoint)) *
        scale;
    return advance + (style.bold ? boldOffsetOf(style.size) : 0.0f);
}

// --- Rich text that needs shaping (ADR 0260) ----------------------------------
//
// The styled characters a markup parses into, laid out as plain text that
// needs shaping is: a paragraph resolved by the bidirectional algorithm, cut
// into runs of one direction, script, face and style -- a change of colour or
// size mid-word cuts the run there -- and each shaped at its style's size.

void appendUtf8(std::string& out, u32 c)
{
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    }
    else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

[[nodiscard]] bool richNeedsShaping(const RichText& text) noexcept
{
    return std::any_of(text.glyphs.begin(), text.glyphs.end(),
                       [](const RichGlyph& glyph) { return complexCodepoint(glyph.codepoint); });
}

// One drawn piece of a shaped rich line, in the order it is drawn: a glyph,
// the style and the first character of the cluster it stands for, and its
// advance and the face's offsets, in pixels.
struct RichUnit
{
    u32 glyph = 0;
    u32 codepoint = 0;
    u32 style = 0;
    usize character = 0;
    f32 advance = 0.0f;
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    core::u8 slot = 0;
    bool missing = false;
};

// The characters [`begin`, `end`) of `text` -- one paragraph or one line,
// with no line break in it -- as UTF-8, and the character each byte is of.
struct RichSpan
{
    std::string utf8;
    std::vector<usize> owner;
};

[[nodiscard]] RichSpan richSpan(const RichText& text, usize begin, usize end)
{
    RichSpan span;
    for (usize index = begin; index < end; ++index) {
        appendUtf8(span.utf8, text.glyphs[index].codepoint);
        span.owner.resize(span.utf8.size(), index);
    }
    return span;
}

[[nodiscard]] std::vector<RichUnit> shapeRich(const RichText& text, const RichSpan& span, FaceChain& chain)
{
    std::vector<RichUnit> out;
    if (span.utf8.empty())
        return out;
    const Paragraph paragraph(span.utf8);
    const std::string_view bytes = span.utf8;
    hb_buffer_t* buffer = shapingBuffer();
    SBLineRef line = SBParagraphCreateLine(paragraph.resolved(), 0, bytes.size());
    const SBUInteger runCount = line != nullptr ? SBLineGetRunCount(line) : 0;
    const SBRun* runs = line != nullptr ? SBLineGetRunsPtr(line) : nullptr;
    for (SBUInteger runIndex = 0; runIndex < runCount; ++runIndex) {
        const SBRun& run = runs[runIndex];
        const bool rightToLeft = (run.level & 1u) != 0;
        struct Item
        {
            usize begin = 0;
            usize end = 0;
            usize slot = 0;
            SBScript script = SBScriptZYYY;
            u32 style = 0;
        };
        std::vector<Item> items;
        for (usize at = run.offset; at < run.offset + run.length;) {
            const Decoded decoded = decodeUtf8(bytes, at);
            const SBScript script = paragraph.scriptAt(at);
            const u32 style = text.glyphs[span.owner[at]].style;
            usize slot = slotFor(chain, decoded.codepoint);
            const bool follows =
                !items.empty() &&
                (slot == chain.faces.size() || zeroWidth(decoded.codepoint) ||
                 SBGeneralCategoryIsMark(SBCodepointGetGeneralCategory(static_cast<SBCodepoint>(decoded.codepoint))));
            if (follows)
                slot = items.back().slot;
            else if (slot == chain.faces.size())
                slot = 0;
            if (items.empty() || items.back().slot != slot || items.back().script != script ||
                items.back().style != style)
                items.push_back(Item{at, at, slot, script, style});
            items.back().end = at + decoded.length;
            at += decoded.length;
        }
        if (rightToLeft)
            std::reverse(items.begin(), items.end());

        for (const Item& item : items) {
            const RichStyle& style = text.styles[item.style];
            Face& face = *chain.faces[item.slot];
            const f32 madeAt = rasterSizeFor(style.size);
            const f32 toPixels = stbtt_ScaleForPixelHeight(&face.info, madeAt) * scaleFor(chain.primary(), style.size);
            hb_buffer_clear_contents(buffer);
            hb_buffer_add_utf8(buffer, bytes.data(), static_cast<int>(bytes.size()), static_cast<unsigned>(item.begin),
                               static_cast<int>(item.end - item.begin));
            hb_buffer_set_direction(buffer, rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(SBScriptGetUnicodeTag(item.script)));
            hb_buffer_guess_segment_properties(buffer);
            hb_shape(shaperOf(face), buffer, nullptr, 0);
            unsigned count = 0;
            const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
            const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
            const bool tracked = chain.tracking != 0.0f && !joiningScript(item.script);
            for (unsigned at = 0; at < count; ++at) {
                RichUnit unit;
                unit.glyph = infos[at].codepoint;
                unit.style = item.style;
                unit.slot = static_cast<core::u8>(item.slot);
                const usize cluster = infos[at].cluster;
                unit.character = span.owner[cluster];
                unit.codepoint = decodeUtf8(bytes, cluster).codepoint;
                unit.dx = static_cast<f32>(positions[at].x_offset) * toPixels;
                unit.dy = static_cast<f32>(positions[at].y_offset) * toPixels;
                unit.advance = static_cast<f32>(positions[at].x_advance) * toPixels;
                const bool lastOfCluster = at + 1 == count || infos[at + 1].cluster != infos[at].cluster;
                if (unit.glyph == 0 && !zeroWidth(unit.codepoint)) {
                    unit.missing = true;
                    unit.advance = advanceOf(chain, style, unit.codepoint);
                }
                else if (lastOfCluster) {
                    if (tracked)
                        unit.advance +=
                            trackingOf(chain, style.size, unit.codepoint) * scaleFor(chain.primary(), style.size);
                    if (style.bold)
                        unit.advance += boldOffsetOf(style.size);
                }
                out.push_back(unit);
            }
        }
    }
    if (line != nullptr)
        SBLineRelease(line);
    return out;
}

// **What a rich text's lines are broken by, when it needs shaping**: each
// character's share of the advance -- the whole cluster's on its first
// character, nothing on the rest, which a line never starts at -- and where a
// word of Thai or Lao begins.
struct RichMeasure
{
    std::vector<f32> advance;
    std::vector<bool> clusterStart;
    std::vector<bool> wordStart;
};

[[nodiscard]] RichMeasure measureRich(const RichText& text, FaceChain& chain)
{
    RichMeasure measure;
    const usize count = text.glyphs.size();
    measure.advance.assign(count + 1, 0.0f);
    measure.clusterStart.assign(count + 1, false);
    measure.wordStart.assign(count + 1, false);
    for (usize begin = 0; begin < count;) {
        usize end = begin;
        while (end < count && text.glyphs[end].codepoint != '\n')
            ++end;
        const RichSpan span = richSpan(text, begin, end);
        for (const RichUnit& unit : shapeRich(text, span, chain)) {
            measure.advance[unit.character] += unit.advance;
            measure.clusterStart[unit.character] = true;
        }
        std::vector<usize> words;
        detail::dictionaryBreaks(span.utf8, words);
        for (const usize at : words)
            measure.wordStart[span.owner[at]] = true;
        // The line break is a character of its own.
        measure.clusterStart[end] = true;
        begin = end + 1;
    }
    return measure;
}

// On `breakLines`' rules, over styled glyphs rather than bytes.
void breakRichLines(const RichText& text, FaceChain& chain, f32 maxWidth, std::vector<RichLine>& out)
{
    out.clear();
    // Text that needs shaping is measured by its clusters, and a line ends
    // only where one starts (ADR 0260).
    const bool shaped = chain.primary().ready && richNeedsShaping(text);
    const RichMeasure measure = shaped ? measureRich(text, chain) : RichMeasure{};
    const auto breakable = [&](usize at, u32 before, u32 after) {
        if (!shaped)
            return breaksBetween(before, after);
        return measure.clusterStart[at] &&
               (breaksBetween(before, after) || (measure.wordStart[at] && before != ' ' && after != ' '));
    };
    constexpr usize None = static_cast<usize>(-1);
    usize lineBegin = 0;
    // As in `breakLines`: where this line may end, where the next then
    // begins, and how wide this one is there.
    usize breakEnd = None;
    usize breakNext = None;
    f32 widthAtBreak = 0.0f;
    f32 width = 0.0f;
    const Face& primary = chain.primary();

    const auto finish = [&](usize end, f32 lineWidth, usize nextBegin) {
        RichLine line;
        line.begin = lineBegin;
        line.end = end;
        line.width = lineWidth;
        // The tallest thing on the line sets its height and its baseline, in
        // the face that draws it; an empty line is as tall as whatever style
        // it sits in.
        const usize probeEnd = end > lineBegin ? end : std::min(lineBegin + 1, text.glyphs.size());
        f32 below = 0.0f;
        bool fellBack = false;
        for (usize at = lineBegin; at < probeEnd; ++at) {
            const RichGlyph glyph = text.glyphs[at];
            const RichStyle& style = text.styles[glyph.style];
            const u32 faces = glyph.codepoint == '\n'
                                  ? 1u
                                  : 1u << store().entries[glyphIndex(chain, style.size, glyph.codepoint)].face;
            const LineMetrics metrics = lineMetricsOf(chain, style.size, faces);
            fellBack = fellBack || faces != 1u;
            line.ascent = std::max(line.ascent, metrics.ascent);
            line.height = std::max(line.height, metrics.height);
            below = std::max(below, metrics.height - metrics.ascent);
        }
        // Mixed faces reach as high as the highest and as deep as the deepest;
        // one face alone measures as it always did.
        if (fellBack)
            line.height = line.ascent + below;
        if (line.height <= 0.0f) {
            line.ascent = ascentOf(primary, text.styles.front().size);
            line.height = lineHeightOf(primary, text.styles.front().size);
        }
        out.push_back(line);
        lineBegin = nextBegin;
        breakEnd = None;
        breakNext = None;
        width = 0.0f;
    };

    usize index = 0;
    while (index < text.glyphs.size()) {
        const RichGlyph glyph = text.glyphs[index];
        if (glyph.codepoint == '\n') {
            finish(index, width, index + 1);
            ++index;
            continue;
        }
        const u32 previous = index > lineBegin ? text.glyphs[index - 1].codepoint : 0u;
        const f32 advance =
            shaped ? measure.advance[index] : advanceOf(chain, text.styles[glyph.style], glyph.codepoint);
        if (maxWidth > 0.0f && width + advance > wrapRoomOf(maxWidth) && index > lineBegin &&
            (!shaped || measure.clusterStart[index])) {
            if (breakable(index, previous, glyph.codepoint)) {
                finish(index, width, index);
                continue;
            }
            if (breakEnd != None && breakEnd > lineBegin) {
                finish(breakEnd, widthAtBreak, breakNext);
                index = lineBegin;
                continue;
            }
            finish(index, width, index);
            continue;
        }
        if (glyph.codepoint == ' ') {
            breakEnd = index;
            breakNext = index + 1;
            widthAtBreak = width;
        }
        else if (index > lineBegin && breakable(index, previous, glyph.codepoint)) {
            breakEnd = index;
            breakNext = index;
            widthAtBreak = width;
        }
        width += advance;
        ++index;
    }
    finish(text.glyphs.size(), width, text.glyphs.size());
}

} // namespace

const GlyphCacheStats& glyphCacheStats() noexcept
{
    return store().stats;
}

void resetGlyphCache() noexcept
{
    // Lines are keyed by face, and the faces are going.
    lineCache().entries.clear();
    GlyphStore& cache = store();
    cache.entries.clear();
    cache.quads.clear();
    cache.pages.clear();
    cache.filling = 0;
    ++cache.atlasVersion;
    cache.emptiedAt = cache.atlasVersion;
    cache.stats = GlyphCacheStats{};
}

void beginGlyphFrame() noexcept
{
    ++store().frame;
}

f32 scaledTextSize(f32 fits) noexcept
{
    const f32 capped = std::fmin(std::fmax(fits, 1.0f), kMaxScaledTextSize);
    const f32 step = capped < 24.0f ? 1.0f : capped < 48.0f ? 2.0f : 4.0f;
    return std::fmax(1.0f, std::floor(capped / step) * step);
}

std::vector<TextLine> textLines(std::string_view text, std::string_view font, f32 pixelSize, f32 maxWidth)
{
    FaceChain& chain = chainFor(font);
    std::vector<Line> lines;
    breakLines(text, chain, pixelSize, scaleFor(chain.primary(), pixelSize), maxWidth, lines);
    std::vector<TextLine> out;
    out.reserve(lines.size());
    for (const Line& line : lines)
        out.push_back(TextLine{line.begin, line.end, line.width});
    return out;
}

f32 textWidth(std::string_view run, std::string_view font, f32 pixelSize)
{
    FaceChain& chain = chainFor(font);
    return widthOf(run, chain, pixelSize) * scaleFor(chain.primary(), pixelSize);
}

f32 textLineHeight(std::string_view font, f32 pixelSize, std::string_view text)
{
    FaceChain& chain = chainFor(font);
    return lineMetricsOf(chain, pixelSize, facesOf(text, chain, pixelSize)).height;
}

f32 textAscent(std::string_view font, f32 pixelSize, std::string_view text)
{
    FaceChain& chain = chainFor(font);
    return lineMetricsOf(chain, pixelSize, facesOf(text, chain, pixelSize)).ascent;
}

std::vector<TextCluster> textClusters(std::string_view line, std::string_view font, f32 pixelSize)
{
    FaceChain& chain = chainFor(font);
    const f32 scale = scaleFor(chain.primary(), pixelSize);
    std::vector<TextCluster> out;
    if (chain.primary().ready && needsShaping(line)) {
        const usize end = std::min(line.find('\n'), line.size());
        const Paragraph paragraph(line.substr(0, end));
        const ShapedLine shaped = shapeLine(paragraph, 0, end, chain, pixelSize);
        out.reserve(shaped.clusters.size());
        for (const PlacedCluster& cluster : shaped.clusters)
            out.push_back(TextCluster{cluster.begin, cluster.end, cluster.left * scale, cluster.right * scale,
                                      cluster.rightToLeft});
        return out;
    }
    // A character at a time, left to right, its edges the sums `textWidth`
    // takes of what is before it -- the same advances in the same order.
    f32 running = 0.0f;
    for (usize at = 0; at < line.size() && line[at] != '\n';) {
        const usize next = std::min<usize>(nextCharacter(line, static_cast<u32>(at)), line.size());
        const f32 left = running;
        for (usize index = at; index < next;) {
            const Decoded decoded = decodeUtf8(line, index);
            running += store().entries[glyphIndex(chain, pixelSize, decoded.codepoint)].advance +
                       trackingOf(chain, pixelSize, decoded.codepoint);
            index += decoded.length;
        }
        out.push_back(TextCluster{at, next, left * scale, running * scale, false});
        at = next;
    }
    return out;
}

std::string ellipsizedText(std::string_view text, std::string_view font, f32 pixelSize, f32 maxWidth, Vec2 box)
{
    FaceChain& chain = chainFor(font);
    const Face& face = chain.primary();
    const f32 scale = scaleFor(face, pixelSize);
    // A copy: measuring a cut line below may refill the cache these came from.
    const std::vector<Line> lines = linesOf(text, chain, pixelSize, scale, maxWidth);
    u32 faces = 0;
    for (const Line& line : lines)
        faces |= line.faces;
    const f32 lineHeight = lineMetricsOf(chain, pixelSize, faces).height;
    // The lines the box is tall enough for, and never none: a box shorter
    // than a line still says something. The hair is a height that is exactly
    // some lines, which a sum of floats can find a thousandth short.
    const usize room = lineHeight > 0.0f ? static_cast<usize>(std::fmax(1.0f, std::floor((box.y + 0.01f) / lineHeight)))
                                         : lines.size();
    const usize kept = std::min(lines.size(), std::max<usize>(room, 1));
    bool cut = kept < lines.size();
    for (usize at = 0; at < kept && !cut; ++at)
        cut = lines[at].width > box.x + 0.01f;
    if (!cut)
        return std::string(text);

    // The ellipsis of a face of the chain, or three full stops where none has
    // one -- the built-in face, and a game's font that stops at ASCII, would
    // draw the box a missing character is.
    const bool own = face.ready && std::any_of(chain.faces.begin(), chain.faces.end(), [](const Face* held) {
                         return stbtt_FindGlyphIndex(&held->info, 0x2026) != 0;
                     });
    const std::string_view ellipsis = own ? std::string_view{"\xE2\x80\xA6"} : std::string_view{"..."};
    const f32 ellipsisWidth = widthOf(ellipsis, chain, pixelSize) * scale;

    std::string out;
    out.reserve(text.size());
    for (usize at = 0; at < kept; ++at) {
        const Line& line = lines[at];
        std::string_view run = text.substr(line.begin, line.end - line.begin);
        const bool more = at + 1 == kept && kept < lines.size();
        if (line.width > box.x + 0.01f || more) {
            // Back a character at a time, and past the space a cut would
            // leave before the ellipsis, until what is left fits with it.
            usize end = run.size();
            while (end > 0) {
                const std::string_view head = run.substr(0, end);
                if (head.back() != ' ' && widthOf(head, chain, pixelSize) * scale + ellipsisWidth <= box.x + 0.01f)
                    break;
                // The start of the character before: never inside one.
                do {
                    --end;
                } while (end > 0 && (static_cast<unsigned char>(run[end]) & 0xC0u) == 0x80u);
            }
            out.append(run.substr(0, end));
            out.append(ellipsis);
        }
        else {
            out.append(run);
        }
        if (at + 1 < kept)
            out.push_back('\n');
    }
    return out;
}

TextRunMetrics measureText(std::string_view text, std::string_view font, f32 pixelSize, f32 maxWidth)
{
    ENG_PROFILE_SCOPE("ui.text.measure");
    // `font` SELECTS the face now rather than only keying the cache, which is
    // what took `TextLabel.Font` off the `Inert` list. An empty
    // name is the default face; a name the provider cannot resolve falls back to
    // it and warns once.
    FaceChain& chain = chainFor(font);
    const f32 scale = scaleFor(chain.primary(), pixelSize);
    const std::vector<Line>& lines = linesOf(text, chain, pixelSize, scale, maxWidth);

    TextRunMetrics metrics;
    metrics.lineCount = static_cast<u32>(lines.size());
    // Every line of a label is as tall as the tallest face any of them used:
    // lines of a paragraph a step apart, whichever script each holds.
    u32 faces = 0;
    for (const Line& line : lines) {
        metrics.size.x = std::fmax(metrics.size.x, line.width);
        faces |= line.faces;
    }
    const LineMetrics line = lineMetricsOf(chain, pixelSize, faces);
    metrics.ascent = line.ascent;
    metrics.size.y = static_cast<f32>(lines.size()) * line.height;
    return metrics;
}

// The stroke key an outline of `stroke` at `pixelSize` is cached under: its
// radius in quarter pixels, at least one quarter so a hair of a stroke still
// shows, and the join.
// `hollow` is text whose letters are not drawn, which takes the ring alone.
[[nodiscard]] GlyphStroke glyphStrokeOf(const TextStroke& stroke, f32 pixelSize, bool hollow) noexcept
{
    const f32 pixels = stroke.scaled ? stroke.thickness * pixelSize : stroke.thickness;
    const auto quarters =
        static_cast<u32>(std::fmin(std::fmax(pixels * 4.0f + 0.5f, 1.0f), static_cast<f32>(MaxStrokeQuarters)));
    return GlyphStroke{quarters, std::min(stroke.join, 2u), hollow};
}

// An outline quad takes the stroke's colour and the stroke's own gradient.
void paintOutline(DrawQuad& quad, const TextStroke& stroke)
{
    quad.color = stroke.color;
    quad.alpha = stroke.alpha;
    quad.outline = true;
    quad.gradient = stroke.gradient;
    quad.gradientType = stroke.gradientType;
    quad.gradientTile = stroke.gradientTile;
    quad.gradientAngle = stroke.gradientAngle;
    quad.gradientScale = stroke.gradientScale;
    quad.gradientOffset = stroke.gradientOffset;
}

void buildTextGeometry(std::string_view text, std::string_view font, f32 pixelSize, f32 maxWidth, core::Rect box,
                       i32 horizontalAlignment, i32 verticalAlignment, core::Color3 color, f32 alpha, u32 scissor,
                       std::vector<DrawQuad>& out, const TextStroke& stroke)
{
    FaceChain& chain = chainFor(font);
    const f32 scale = scaleFor(chain.primary(), pixelSize);
    // Held by reference: filling glyphs below never touches the line cache.
    const std::vector<Line>& lines = linesOf(text, chain, pixelSize, scale, maxWidth);

    u32 faces = 0;
    for (const Line& line : lines)
        faces |= line.faces;
    const LineMetrics metrics = lineMetricsOf(chain, pixelSize, faces);
    const f32 lineHeight = metrics.height;
    // A glyph is placed from the top of a line in the label's own face; a
    // taller face in the label lowers every baseline by the difference.
    const f32 lift = metrics.ascent - ascentOf(chain.primary(), pixelSize);
    const f32 totalHeight = static_cast<f32>(lines.size()) * lineHeight;
    const f32 boxWidth = box.max.x - box.min.x;
    const f32 boxHeight = box.max.y - box.min.y;

    f32 top = box.min.y;
    if (verticalAlignment == 1)
        top += (boxHeight - totalHeight) * 0.5f;
    else if (verticalAlignment == 2)
        top += boxHeight - totalHeight;

    // Twice when there is an outline: every outline first, then every glyph,
    // so one letter's outline never lies over the letter beside it.
    const bool outlined = stroke.thickness > 0.0f && stroke.alpha > 0.0f;
    const GlyphStroke key = outlined ? glyphStrokeOf(stroke, pixelSize, alpha <= 0.0f) : GlyphStroke{};
    for (int pass = outlined ? 0 : 1; pass < 2; ++pass) {
        const bool outline = pass == 0;
        f32 y = top;
        for (const Line& line : lines) {
            f32 x = box.min.x;
            if (horizontalAlignment == 1)
                x += (boxWidth - line.width) * 0.5f;
            else if (horizontalAlignment == 2)
                x += boxWidth - line.width;

            if (line.shaped != nullptr) {
                // Shaped (ADR 0260): each glyph where the shaper put it, by
                // its index in the face that drew it.
                for (const PlacedGlyph& placed : line.shaped->glyphs) {
                    const GlyphStroke wanted = outline ? key : GlyphStroke{};
                    const usize slot = placed.missing
                                           ? glyphIndex(chain, pixelSize, placed.codepoint, wanted)
                                           : shapedGlyphIndex(chain, placed.slot, pixelSize, placed.glyph, wanted);
                    const GlyphEntry entry = store().entries[slot];
                    for (u32 quad = 0; quad < entry.quadCount; ++quad) {
                        const GlyphQuad& shape = store().quads[entry.firstQuad + quad];
                        DrawQuad glyph;
                        glyph.min = Vec2{x + (placed.pen + placed.dx + shape.minX) * scale,
                                         y + lift + (shape.minY - placed.dy) * scale};
                        glyph.max = Vec2{x + (placed.pen + placed.dx + shape.maxX) * scale,
                                         y + lift + (shape.maxY - placed.dy) * scale};
                        glyph.color = color;
                        glyph.alpha = alpha;
                        glyph.texture = entry.textured ? 1u + entry.page : 0u;
                        glyph.uvMin = Vec2{shape.u0, shape.v0};
                        glyph.uvMax = Vec2{shape.u1, shape.v1};
                        glyph.scissor = scissor;
                        if (outline)
                            paintOutline(glyph, stroke);
                        out.push_back(glyph);
                    }
                }
                y += lineHeight;
                continue;
            }

            f32 pen = 0.0f;
            usize index = line.begin;
            while (index < line.end) {
                const Decoded decoded = decodeUtf8(text, index);
                // The plain glyph's advance places both passes, so an outline
                // sits exactly under its letter.
                const f32 advance = store().entries[glyphIndex(chain, pixelSize, decoded.codepoint)].advance +
                                    trackingOf(chain, pixelSize, decoded.codepoint);
                const usize slot = glyphIndex(chain, pixelSize, decoded.codepoint, outline ? key : GlyphStroke{});
                // Copied out rather than referenced: the next `glyphIndex` may
                // reallocate the entry vector, and this loop calls it again.
                const GlyphEntry entry = store().entries[slot];

                for (u32 quad = 0; quad < entry.quadCount; ++quad) {
                    const GlyphQuad& shape = store().quads[entry.firstQuad + quad];
                    DrawQuad glyph;
                    glyph.min = Vec2{x + (pen + shape.minX) * scale, y + lift + shape.minY * scale};
                    glyph.max = Vec2{x + (pen + shape.maxX) * scale, y + lift + shape.maxY * scale};
                    glyph.color = color;
                    glyph.alpha = alpha;
                    // Textures 1 to `kGlyphPages` are the atlas's pages (`ui.h`). A
                    // vector glyph is a solid rectangle and samples nothing,
                    // which is texture 0 -- the two faces differ here and in
                    // the metrics, and nowhere else.
                    glyph.texture = entry.textured ? 1u + entry.page : 0u;
                    glyph.uvMin = Vec2{shape.u0, shape.v0};
                    glyph.uvMax = Vec2{shape.u1, shape.v1};
                    glyph.scissor = scissor;
                    if (outline)
                        paintOutline(glyph, stroke);
                    out.push_back(glyph);
                }

                pen += advance;
                index += decoded.length;
            }
            y += lineHeight;
        }
    }
}

TextRunMetrics measureRichText(std::string_view markup, std::string_view font, f32 pixelSize, f32 maxWidth)
{
    ENG_PROFILE_SCOPE("ui.text.measure");
    FaceChain& chain = chainFor(font);
    RichStyle base;
    base.size = pixelSize;
    const RichText text = parseRichText(markup, base);
    std::vector<RichLine> lines;
    breakRichLines(text, chain, maxWidth, lines);

    TextRunMetrics metrics;
    metrics.lineCount = static_cast<u32>(lines.size());
    metrics.ascent = lines.empty() ? ascentOf(chain.primary(), pixelSize) : lines.front().ascent;
    for (const RichLine& line : lines) {
        metrics.size.x = std::fmax(metrics.size.x, line.width);
        metrics.size.y += line.height;
    }
    return metrics;
}

void buildRichTextGeometry(std::string_view markup, std::string_view font, f32 pixelSize, f32 maxWidth, core::Rect box,
                           i32 horizontalAlignment, i32 verticalAlignment, core::Color3 color, f32 alpha, u32 scissor,
                           std::vector<DrawQuad>& out, const TextStroke& labelStroke)
{
    FaceChain& chain = chainFor(font);
    const Face& face = chain.primary();
    RichStyle base;
    base.size = pixelSize;
    base.color = color;
    base.alpha = alpha;
    base.stroke = labelStroke;
    const RichText text = parseRichText(markup, base);
    std::vector<RichLine> lines;
    breakRichLines(text, chain, maxWidth, lines);
    // Text that needs shaping is drawn a line at a time in the order it is
    // read (ADR 0260).
    const bool shaped = face.ready && richNeedsShaping(text);
    std::vector<std::vector<RichUnit>> shapedLines;
    if (shaped) {
        for (const RichLine& line : lines)
            shapedLines.push_back(shapeRich(text, richSpan(text, line.begin, line.end), chain));
    }

    f32 totalHeight = 0.0f;
    for (const RichLine& line : lines)
        totalHeight += line.height;
    const f32 boxWidth = box.max.x - box.min.x;
    const f32 boxHeight = box.max.y - box.min.y;

    f32 firstTop = box.min.y;
    if (verticalAlignment == 1)
        firstTop += (boxHeight - totalHeight) * 0.5f;
    else if (verticalAlignment == 2)
        firstTop += boxHeight - totalHeight;

    const auto rule = [&](f32 left, f32 right, f32 top, f32 thickness, const RichStyle& style) {
        DrawQuad bar;
        bar.min = Vec2{left, top};
        bar.max = Vec2{right, top + thickness};
        bar.color = style.color;
        bar.alpha = style.alpha;
        bar.scissor = scissor;
        out.push_back(bar);
    };

    // Twice: every outline a run asked for (a `UIStroke` or a `<stroke>`),
    // then every glyph, so no outline lies over a neighbouring letter.
    for (int pass = 0; pass < 2; ++pass) {
        const bool outlinePass = pass == 0;
        f32 y = firstTop;
        for (const RichLine& line : lines) {
            f32 x = box.min.x;
            if (horizontalAlignment == 1)
                x += (boxWidth - line.width) * 0.5f;
            else if (horizontalAlignment == 2)
                x += boxWidth - line.width;
            const f32 baseline = y + line.ascent;

            if (shaped) {
                f32 pen = x;
                for (const RichUnit& unit : shapedLines[static_cast<usize>(&line - lines.data())]) {
                    const RichStyle& style = text.styles[unit.style];
                    const f32 scale = scaleFor(face, style.size);
                    const f32 top = baseline - ascentOf(face, style.size);
                    const bool outlined = style.stroke.thickness > 0.0f && style.stroke.alpha > 0.0f;
                    if (outlinePass && !outlined) {
                        pen += unit.advance;
                        continue;
                    }
                    const GlyphStroke wanted =
                        outlinePass ? glyphStrokeOf(style.stroke, style.size, style.alpha <= 0.0f) : GlyphStroke{};
                    const usize slot = unit.missing
                                           ? glyphIndex(chain, style.size, unit.codepoint, wanted)
                                           : shapedGlyphIndex(chain, unit.slot, style.size, unit.glyph, wanted);
                    const GlyphEntry entry = store().entries[slot];
                    const int strokes = style.bold ? 2 : 1;
                    for (int stroke = 0; stroke < strokes; ++stroke) {
                        const f32 offset = stroke == 0 ? 0.0f : boldOffsetOf(style.size);
                        for (u32 quad = 0; quad < entry.quadCount; ++quad) {
                            const GlyphQuad& shape = store().quads[entry.firstQuad + quad];
                            DrawQuad drawn;
                            drawn.min =
                                Vec2{pen + unit.dx + offset + shape.minX * scale, top - unit.dy + shape.minY * scale};
                            drawn.max =
                                Vec2{pen + unit.dx + offset + shape.maxX * scale, top - unit.dy + shape.maxY * scale};
                            drawn.color = style.color;
                            drawn.alpha = style.alpha;
                            drawn.texture = entry.textured ? 1u + entry.page : 0u;
                            drawn.uvMin = Vec2{shape.u0, shape.v0};
                            drawn.uvMax = Vec2{shape.u1, shape.v1};
                            drawn.scissor = scissor;
                            if (style.italic) {
                                drawn.slant = ItalicSlant;
                                const f32 lift = baseline - drawn.max.y;
                                drawn.min.x += lift * ItalicSlant;
                                drawn.max.x += lift * ItalicSlant;
                            }
                            if (outlinePass)
                                paintOutline(drawn, style.stroke);
                            out.push_back(drawn);
                        }
                    }
                    if (!outlinePass) {
                        const f32 thickness = std::max(1.0f, std::round(style.size / 14.0f));
                        if (style.underline)
                            rule(pen, pen + unit.advance, baseline + thickness, thickness, style);
                        if (style.strike)
                            rule(pen, pen + unit.advance, baseline - ascentOf(face, style.size) * 0.32f, thickness,
                                 style);
                    }
                    pen += unit.advance;
                }
                y += line.height;
                continue;
            }

            f32 pen = x;
            for (usize index = line.begin; index < line.end; ++index) {
                const RichGlyph glyph = text.glyphs[index];
                const RichStyle& style = text.styles[glyph.style];
                const f32 scale = scaleFor(face, style.size);
                // A glyph's quads are measured from the top of ITS size's line, so
                // a smaller run is dropped until its baseline meets the line's.
                const f32 top = baseline - ascentOf(face, style.size);
                const f32 advance = advanceOf(chain, style, glyph.codepoint);
                const bool outlined = style.stroke.thickness > 0.0f && style.stroke.alpha > 0.0f;
                if (outlinePass && !outlined) {
                    pen += advance;
                    continue;
                }
                const usize slot = glyphIndex(chain, style.size, glyph.codepoint,
                                              outlinePass ? glyphStrokeOf(style.stroke, style.size, style.alpha <= 0.0f)
                                                          : GlyphStroke{});
                const GlyphEntry entry = store().entries[slot];

                const int strokes = style.bold ? 2 : 1;
                for (int stroke = 0; stroke < strokes; ++stroke) {
                    const f32 offset = stroke == 0 ? 0.0f : boldOffsetOf(style.size);
                    for (u32 quad = 0; quad < entry.quadCount; ++quad) {
                        const GlyphQuad& shape = store().quads[entry.firstQuad + quad];
                        DrawQuad drawn;
                        drawn.min = Vec2{pen + offset + shape.minX * scale, top + shape.minY * scale};
                        drawn.max = Vec2{pen + offset + shape.maxX * scale, top + shape.maxY * scale};
                        drawn.color = style.color;
                        drawn.alpha = style.alpha;
                        drawn.texture = entry.textured ? 1u + entry.page : 0u;
                        drawn.uvMin = Vec2{shape.u0, shape.v0};
                        drawn.uvMax = Vec2{shape.u1, shape.v1};
                        drawn.scissor = scissor;
                        // Sheared about the baseline rather than the quad's own
                        // bottom, so every glyph of a word leans the same way.
                        if (style.italic) {
                            drawn.slant = ItalicSlant;
                            const f32 lift = baseline - drawn.max.y;
                            drawn.min.x += lift * ItalicSlant;
                            drawn.max.x += lift * ItalicSlant;
                        }
                        if (outlinePass)
                            paintOutline(drawn, style.stroke);
                        out.push_back(drawn);
                    }
                }

                if (!outlinePass) {
                    const f32 thickness = std::max(1.0f, std::round(style.size / 14.0f));
                    if (style.underline)
                        rule(pen, pen + advance, baseline + thickness, thickness, style);
                    if (style.strike)
                        rule(pen, pen + advance, baseline - ascentOf(face, style.size) * 0.32f, thickness, style);
                }
                pen += advance;
            }
            y += line.height;
        }
    }
}

std::string plainTextOf(std::string_view markup)
{
    const RichText text = parseRichText(markup, RichStyle{});
    std::string out;
    out.reserve(markup.size());
    for (const RichGlyph glyph : text.glyphs) {
        const u32 cp = glyph.codepoint;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

u32 glyphAtlasPages() noexcept
{
    return static_cast<u32>(store().pages.size());
}

u64 glyphAtlasVersion() noexcept
{
    return store().atlasVersion;
}

GlyphAtlas glyphAtlas(u32 page) noexcept
{
    const GlyphStore& cache = store();
    // A page there is none of: nothing, emptied when the store last was --
    // which is what tells an uploader that the texture it holds is of pixels
    // that are nowhere now.
    if (page >= cache.pages.size())
        return GlyphAtlas{{}, 0, 0, cache.atlasVersion, {}, cache.emptiedAt};
    const AtlasPage& held = cache.pages[page];
    return GlyphAtlas{held.pixels, AtlasSize, AtlasSize, cache.atlasVersion, held.rows, held.clearedAt};
}

void setFaceProvider(FaceProvider provider, void* user) noexcept
{
    FaceTable& table = faceTable();
    table.provider = provider;
    table.providerUser = user;
    // Loaded faces are dropped, not kept: a provider that has just been
    // installed may resolve a name the previous one could not, and a cached
    // fallback would be the wrong face for the rest of the process.
    table.chains.clear();
    table.faces.clear();
    table.warned.clear();
    resetGlyphCache();
}

void setFontFallbacks(std::span<const std::string> names)
{
    FaceTable& table = faceTable();
    if (std::equal(names.begin(), names.end(), table.fallbacks.begin(), table.fallbacks.end()))
        return;
    table.fallbacks.assign(names.begin(), names.end());
    // The faces stay loaded; what each label falls back on is built again.
    table.chains.clear();
    resetGlyphCache();
}

std::string_view labelFont(std::string_view font, std::string_view fallback, f32 letterSpacing)
{
    // Half an em closer to two apart: past either, letters overlap or stand alone.
    letterSpacing = std::isfinite(letterSpacing) ? std::clamp(letterSpacing, -0.5f, 2.0f) : 0.0f;
    if (fallback.empty() && letterSpacing == 0.0f)
        return font;
    std::string wanted(font);
    if (!fallback.empty()) {
        wanted.push_back(kFontFallbackSeparator);
        wanted.append(fallback);
    }
    if (letterSpacing != 0.0f) {
        char digits[32] = {};
        std::snprintf(digits, sizeof(digits), "%.6g", static_cast<double>(letterSpacing));
        wanted.push_back(kFontFallbackSeparator);
        wanted.push_back(kLetterSpacingMark);
        wanted.append(digits);
    }
    // Kept for the process: a few of them a game, and a view a caller may
    // hold across frames.
    static std::vector<std::unique_ptr<std::string>> joined;
    for (const std::unique_ptr<std::string>& held : joined) {
        if (*held == wanted)
            return *held;
    }
    joined.push_back(std::make_unique<std::string>(std::move(wanted)));
    return *joined.back();
}

} // namespace engine::ui
