// How a UI picture gets onto the GPU, and when.
//
// **The whole state machine was uncovered.** There was no test file here, and
// the layout tests stub the image provider out -- so the read, the decode, the
// upload and the little table of handles `ui` indexes into were held together by
// nothing but the fact that they all happened between two adjacent statements.
//
// They no longer do. A 1024-square PNG costs 14 to 36 ms to decode (measured for
// D118 on a material's maps, and an `ImageLabel` names the same kind of file),
// and the synchronous path decoded every image a frame newly named, in that
// frame, with no bound. So there are two modes now, and both are asserted here:
// one that finishes before the frame does, because a capture records the frame
// it was told to, and one that lets the frame finish first.
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "engine/app/ui_text.h"
#include "engine/asset/content.h"
#include "engine/core/i18n.h"
#include "engine/platform/async_io.h"
#include "engine/rhi/backends.h"
#include "engine/ui/ui.h"

using namespace engine;
using engine::app::UiText;

namespace {

struct Fixture
{
    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    rhi::ICmdList* cmd = nullptr;
    asset::ContentMounts mounts;

    Fixture()
    {
        REQUIRE(device != nullptr);
        cmd = device->beginFrame();
        REQUIRE(cmd != nullptr);
        // `asset`'s own fixtures, mounted as a project's content directory would
        // be. `checker.png` is the one real encoded picture this repository
        // carries.
        mounts.mountDirectory(std::filesystem::path(ENG_TEST_IMAGE).parent_path());
        REQUIRE(platform::initIo());
    }
};

// Runs the pipeline until nothing is left in it, or gives up. **Bounded**,
// because a test that spins forever on a defect reports as a hung machine rather
// than as a failure -- and it sleeps, because the whole point is that the work
// is on other threads.
void settle(UiText& text, rhi::IDevice& device, rhi::ICmdList& cmd, int frames = 2000)
{
    for (int frame = 0; frame < frames && text.imagesInFlight() > 0; ++frame) {
        text.sync(device, cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // One more, so the pass that uploads is not the pass the loop stopped on.
    text.sync(device, cmd);
}

[[nodiscard]] bool resolves(UiText& text, std::string_view urn)
{
    ui::ResolvedImage out{};
    return text.requestImage(urn, out) && out.texture >= ui::kFirstImageTexture;
}

} // namespace

TEST_CASE("a picture is on the GPU before the frame that asked for it ends")
{
    // The default, and the mode every golden depends on.
    Fixture fixture;
    UiText text;
    text.setMounts(&fixture.mounts);

    // The first ask is a request: nothing has been read yet, and `ui` draws the
    // flat tint meanwhile -- which is what it already does for an image it
    // cannot resolve.
    CHECK_FALSE(resolves(text, "asset://checker.png"));

    text.sync(*fixture.device, *fixture.cmd);
    CHECK(text.imagesInFlight() == 0);
    CHECK(resolves(text, "asset://checker.png"));

    text.destroy(*fixture.device);
}

TEST_CASE("a deferred picture costs the frame that asked for it nothing")
{
    Fixture fixture;
    UiText text;
    text.setMounts(&fixture.mounts);
    text.setDeferredImages(true);

    CHECK_FALSE(resolves(text, "asset://checker.png"));

    // The frame that queues it resolves nothing and decodes nothing.
    text.sync(*fixture.device, *fixture.cmd);
    CHECK(text.imagesInFlight() == 1);
    CHECK_FALSE(resolves(text, "asset://checker.png"));

    settle(text, *fixture.device, *fixture.cmd);

    // **And the table was rebuilt on the pass that produced the handle.** This
    // is the case that catches the real hazard: rebuilding where the queue is
    // drained rather than where a handle is assigned writes an invalid handle
    // into the table, never rebuilds again, and the picture draws as flat tint
    // for ever with nothing logged. The lengths agree, so no size check finds it.
    CHECK(text.imagesInFlight() == 0);
    CHECK(resolves(text, "asset://checker.png"));

    ui::ResolvedImage out{};
    REQUIRE(text.requestImage("asset://checker.png", out));
    // After the atlas's pages (ADR 0169).
    REQUIRE(out.texture >= ui::kFirstImageTexture);
    const core::usize slot = static_cast<core::usize>(out.texture) - ui::kFirstImageTexture;
    REQUIRE(text.images().size() > slot);
    CHECK(text.images()[slot].valid());

    text.destroy(*fixture.device);
}

TEST_CASE("D578: a picture goes up with every level under it, in either mode")
{
    // An icon of 256 texels drawn seventeen pixels tall is fifteen texels a
    // pixel, and a sampler reads four: with the top level alone on the GPU the
    // picture was whichever four the pixel's centre fell among -- ragged where
    // it should be small. `checker.png` is two texels square: two levels.
    for (const bool deferred : {false, true}) {
        CAPTURE(deferred);
        Fixture fixture;
        UiText text;
        text.setMounts(&fixture.mounts);
        text.setDeferredImages(deferred);
        CHECK_FALSE(resolves(text, "asset://checker.png"));
        // The pass that reads it, or that queues it; then until it is in.
        text.sync(*fixture.device, *fixture.cmd);
        settle(text, *fixture.device, *fixture.cmd);
        REQUIRE(resolves(text, "asset://checker.png"));
        CHECK(text.imageLevelsUploaded() == 2);
        text.destroy(*fixture.device);
    }
}

TEST_CASE("a picture that is not there is refused once, in either mode")
{
    Fixture fixture;
    for (const bool deferred : {false, true}) {
        UiText text;
        text.setMounts(&fixture.mounts);
        text.setDeferredImages(deferred);

        CHECK_FALSE(resolves(text, "asset://not-a-real-file.png"));
        text.sync(*fixture.device, *fixture.cmd);
        settle(text, *fixture.device, *fixture.cmd);

        // Nothing left in the pipeline: a name that is not there must not hold
        // one of the four slots for ever.
        CHECK(text.imagesInFlight() == 0);
        CHECK_FALSE(resolves(text, "asset://not-a-real-file.png"));

        // And asking again does not start it over. `requestImage` remembers a
        // refusal, which is what stops a label naming a missing picture costing
        // an attempt every frame.
        text.sync(*fixture.device, *fixture.cmd);
        CHECK(text.imagesInFlight() == 0);

        text.destroy(*fixture.device);
    }
}

TEST_CASE("asking twice for one picture queues it once")
{
    Fixture fixture;
    UiText text;
    text.setMounts(&fixture.mounts);
    text.setDeferredImages(true);

    CHECK_FALSE(resolves(text, "asset://checker.png"));
    CHECK_FALSE(resolves(text, "asset://checker.png"));
    text.sync(*fixture.device, *fixture.cmd);

    // One entry, one read. Two labels naming one picture is the ordinary case.
    CHECK(text.imagesInFlight() == 1);

    settle(text, *fixture.device, *fixture.cmd);
    CHECK(resolves(text, "asset://checker.png"));

    text.destroy(*fixture.device);
}

TEST_CASE("tearing down while a picture is on its way in leaves nothing behind")
{
    // **The shutdown case**, which reads as "it crashed when I closed it" and
    // points at nothing: a decode job writes into buffers `UiText` owns, and a
    // teardown that returned while one was running would free the memory the
    // pool is writing into.
    Fixture fixture;
    {
        UiText text;
        text.setMounts(&fixture.mounts);
        text.setDeferredImages(true);
        CHECK_FALSE(resolves(text, "asset://checker.png"));
        text.sync(*fixture.device, *fixture.cmd);
        REQUIRE(text.imagesInFlight() == 1);

        text.destroy(*fixture.device);
        CHECK(text.imagesInFlight() == 0);
    }

    // And one that goes out of scope without `destroy` at all, which is what a
    // stack unwind does.
    {
        UiText text;
        text.setMounts(&fixture.mounts);
        text.setDeferredImages(true);
        CHECK_FALSE(resolves(text, "asset://checker.png"));
        text.sync(*fixture.device, *fixture.cmd);
    }
}

TEST_CASE("D575: a view's picture is in the frame's table the first time it is asked for, not the frame after")
{
    // A `ViewportFrame` drew a WHITE square for one frame, each time it was
    // first shown: the draw list named the view's picture by a place in the
    // table, the table was copied for the renderer straight after -- and the
    // place was not in it until the next frame's `sync`. A place past the
    // table's end is "no texture", which is drawn white.
    Fixture fixture;
    UiText text;
    text.setMounts(&fixture.mounts);
    const rhi::TextureHandle portrait = fixture.device->createTexture(
        {.format = rhi::TextureFormat::Rgba8Unorm, .usage = rhi::TextureUsage::Sampled, .width = 4, .height = 4});
    const rhi::TextureHandle other = fixture.device->createTexture(
        {.format = rhi::TextureFormat::Rgba8Unorm, .usage = rhi::TextureUsage::Sampled, .width = 4, .height = 4});
    text.setViewLookup([&](std::string_view name, UiText::ViewPicture& out) {
        if (name != "#7" && name != "#9")
            return false;
        out.texture = name == "#7" ? portrait : other;
        out.width = 4;
        out.height = 4;
        return true;
    });
    // A frame under way: the table as the last `sync` left it, and a draw
    // list asking for two pictures nobody has asked for before.
    text.sync(*fixture.device, *fixture.cmd);
    REQUIRE(text.images().empty());
    ui::ResolvedImage first{};
    ui::ResolvedImage second{};
    REQUIRE(text.requestImage("view://#7", first));
    REQUIRE(text.requestImage("view://#9", second));
    // What the frame copies now, before any `sync`: both, where they were named.
    const auto place = [](const ui::ResolvedImage& image) {
        return static_cast<core::usize>(image.texture - ui::kFirstImageTexture);
    };
    REQUIRE(place(first) < text.images().size());
    REQUIRE(place(second) < text.images().size());
    CHECK(text.images()[place(first)] == portrait);
    CHECK(text.images()[place(second)] == other);
    // And the next `sync` leaves them where they are.
    text.sync(*fixture.device, *fixture.cmd);
    CHECK(text.images()[place(first)] == portrait);
    CHECK(text.images()[place(second)] == other);
    fixture.device->destroy(portrait);
    fixture.device->destroy(other);
}

TEST_CASE("a view's picture remade in a frame is the one that frame draws (the 26-security-cameras crash)")
{
    // What `ViewHost` does when a camera texture's size changes: the old
    // target destroyed, a new one made, in the frame's first half -- and the
    // UI's table, handed to the renderer, has to name the new one in that same
    // frame, or the renderer binds a destroyed texture.
    Fixture fixture;
    UiText text;
    text.setMounts(&fixture.mounts);
    rhi::TextureHandle current = fixture.device->createTexture(
        {.format = rhi::TextureFormat::Rgba8Unorm, .usage = rhi::TextureUsage::Sampled, .width = 4, .height = 4});
    bool drawing = true;
    text.setViewLookup([&](std::string_view name, UiText::ViewPicture& out) {
        if (!drawing || name != "Lobby")
            return false;
        out.texture = current;
        out.width = 4;
        out.height = 4;
        return true;
    });
    REQUIRE(resolves(text, "view://Lobby"));
    text.sync(*fixture.device, *fixture.cmd);
    REQUIRE(text.images().size() == 1);
    CHECK(text.images()[0] == current);

    // Remade: the draw that asks for it again is answered with the new one,
    // and so is the table, before anything else runs.
    fixture.device->destroy(current);
    current = fixture.device->createTexture(
        {.format = rhi::TextureFormat::Rgba8Unorm, .usage = rhi::TextureUsage::Sampled, .width = 8, .height = 8});
    REQUIRE(resolves(text, "view://Lobby"));
    CHECK(text.images()[0] == current);

    // Remade again with nothing asking this frame (a world's UI laid out once):
    // the refresh after the views are made answers for it.
    fixture.device->destroy(current);
    current = fixture.device->createTexture(
        {.format = rhi::TextureFormat::Rgba8Unorm, .usage = rhi::TextureUsage::Sampled, .width = 2, .height = 2});
    text.refreshViews();
    CHECK(text.images()[0] == current);

    // And a view that stopped drawing names no texture at all.
    drawing = false;
    text.refreshViews();
    CHECK_FALSE(text.images()[0].valid());
    fixture.device->destroy(current);
}

namespace {

bool provideTestFace(void*, std::string_view name, std::vector<core::u8>& out)
{
    if (name != "asset://fonts/test.ttf")
        return false;
    std::ifstream file(ENG_TEST_FONT, std::ios::binary);
    if (!file)
        return false;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    out.assign(bytes.begin(), bytes.end());
    return !out.empty();
}

} // namespace

TEST_CASE("a new glyph sends the rows it was written on, not the atlas")
{
    // **D543**: the atlas went up whole whenever its version moved -- a
    // million texels made four million bytes and sent, for one new digit in a
    // damage number. 10 to 15 ms of a frame, as often as a game showed a
    // character it had not shown at that size.
    Fixture fixture;
    ui::setFaceProvider(&provideTestFace, nullptr);
    ui::resetGlyphCache();
    {
        UiText text;
        text.setMounts(&fixture.mounts);
        (void)ui::measureText("Score", "asset://fonts/test.ttf", 24.0f, 0.0f);
        const ui::GlyphAtlas first = ui::glyphAtlas();
        REQUIRE_FALSE(first.pixels.empty());
        text.sync(*fixture.device, *fixture.cmd);
        // **The rows that were written, the first time too** (ADR 0169): a
        // page is a megabyte of which five glyphs are a shelf, and nothing
        // samples the rest. All of it was four megabytes sent before a word
        // was drawn.
        const core::u64 whole = static_cast<core::u64>(first.width) * first.height * 4u;
        const core::u64 begun = text.atlasBytesUploaded();
        CHECK(begun > 0);
        CHECK(begun <= static_cast<core::u64>(first.width) * 40u * 4u);
        CHECK(begun < whole / 20u);

        // Nothing new: nothing sent.
        text.sync(*fixture.device, *fixture.cmd);
        CHECK(text.atlasBytesUploaded() == begun);

        // One more character: the rows of one glyph, the width of the atlas.
        (void)ui::measureText("7", "asset://fonts/test.ttf", 24.0f, 0.0f);
        text.sync(*fixture.device, *fixture.cmd);
        const core::u64 added = text.atlasBytesUploaded() - begun;
        MESSAGE("one new glyph at 24 px: ", added, " bytes sent, of ", whole);
        CHECK(added > 0);
        CHECK(added <= static_cast<core::u64>(first.width) * 40u * 4u);

        // An atlas emptied and written again is the rows written again: what
        // the texture held beside them is on no glyph's rows now.
        ui::resetGlyphCache();
        (void)ui::measureText("Score", "asset://fonts/test.ttf", 24.0f, 0.0f);
        text.sync(*fixture.device, *fixture.cmd);
        const core::u64 again = text.atlasBytesUploaded() - begun - added;
        CHECK(again > 0);
        CHECK(again <= static_cast<core::u64>(first.width) * 40u * 4u);
        text.destroy(*fixture.device);
    }
    ui::resetGlyphCache();
    ui::setFaceProvider(nullptr, nullptr);
}
