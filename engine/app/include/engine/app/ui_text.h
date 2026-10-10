// The GPU half of the UI's text: the glyph atlas as a texture, and the face
// provider that lets `TextLabel.Font` name a font out of a project's content.
//
// **Here rather than in `ui`**, and that is the layering doing its job: `ui` is
// L5 and owns no GPU resources, `render` is L4 and cannot see `ui`, and neither
// of them has content mounts. The app is the only place that can see all three,
// which is exactly what an app is for.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/asset/content.h"
#include "engine/asset/image.h"
#include "engine/asset/texture.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/async_io.h"
#include "engine/rhi/device.h"
#include "engine/ui/ui.h"

namespace engine::app {

class UiText
{
public:
    UiText() = default;
    // Waits for any decode still running, because one is writing into memory
    // this object owns. Abandoning it is a use-after-free at shutdown, which is
    // the hardest kind to attribute -- see `MeshLoader`'s own destructor, which
    // learned this after the fact.
    ~UiText();

    UiText(const UiText&) = delete;
    UiText& operator=(const UiText&) = delete;

    // Installs the face provider, so a `TextLabel.Font` naming `asset://…` can be
    // resolved out of `mounts`. Called once the mounts exist; a null `mounts`
    // uninstalls it, which is what a world going away means.
    void setMounts(const asset::ContentMounts* mounts);
    // **The language pictures are found in** (ADR 0200): a logo or a sign
    // with words in it has a file of the same name under `l10n/<locale>/`,
    // and the interface shows that one. Empty for the project's default
    // language, whose pictures are the names as written. A change lets go of
    // the pictures that are another file in the new language; they are read
    // again when next drawn, like one the budget let go.
    void setLocale(std::string_view locale);

    // Uploads the atlas if a glyph has been added since the last call. Cheap on
    // every frame but the few that rasterise something: it compares one version
    // number and returns.
    //
    // Called inside a frame, because an upload needs a command list.
    void sync(rhi::IDevice& device, rhi::ICmdList& cmd);

    void destroy(rhi::IDevice& device);

    // **How much of the graphics card the interface's pictures may hold**
    // (D609), in bytes; nought is no limit. Every picture an `ImageLabel` ever
    // named used to stay on the card until the engine closed: a game with a
    // shop of four hundred icons, a map and a dozen full-screen menus held all
    // of them through the level where none is shown, and on a phone with two
    // gigabytes that is the memory the level needed. Past this, the pictures
    // nothing has asked for longest are released, oldest first, until what is
    // held fits; one that is asked for again is loaded again, as it was the
    // first time. A picture on the screen is never released: it is asked for
    // every frame.
    void setImageBudget(core::u64 bytes) noexcept { imageBudget_ = bytes; }
    [[nodiscard]] core::u64 imageBudget() const noexcept { return imageBudget_; }
    // What the pictures hold now, how many of them are on the card, and how
    // many have been released since the engine started.
    [[nodiscard]] core::u64 imageBytes() const noexcept { return imageBytes_; }
    [[nodiscard]] core::u32 imagesHeld() const noexcept;
    [[nodiscard]] core::u64 imagesReleased() const noexcept { return imagesReleased_; }
    // The budget a machine with this much memory is given: a thirty-second of
    // it, and no less than 48 MiB nor more than 512. Two gigabytes is 64 MiB;
    // eight is 256. Nought, for a machine that does not say, is 256.
    [[nodiscard]] static core::u64 imageBudgetFor(core::u64 systemMemoryBytes) noexcept;

    // **Whether a UI picture may take more than one frame to arrive** (D128).
    //
    // A 1024-square PNG costs 14 to 36 ms to decode -- measured for D118 on a
    // material's maps, and an `ImageLabel` names the same kind of file. The
    // synchronous path decodes every image a frame newly named, in that frame,
    // with no bound: a HUD that comes up with eight icons pays all eight at once.
    //
    // Deferred, the read goes to `platform::readFileAsync`, the decode to the
    // job pool, and only the upload happens on the frame. An unresolved image
    // already draws as the flat tint everywhere in `ui`, so what deferring costs
    // is a few frames of tint rather than a hole.
    //
    // **Off by default**, for the reason every switch beside it is: a capture
    // records the frame it was told to record.
    void setDeferredImages(bool deferred) noexcept { deferredImages_ = deferred; }

    // How many pictures are queued or on their way in. Zero in synchronous mode.
    [[nodiscard]] core::usize imagesInFlight() const noexcept;

    // **A request, not a load.** The draw list is built before `sync` runs, and
    // an upload needs a command list -- so a URN nobody has seen is RECORDED
    // here and returns false, `sync` loads it with the device it has, and the
    // frame after that draws the picture. One frame of flat tint, which is
    // exactly what "still arriving" looks like and is the same answer a picture
    // genuinely still streaming would give.
    //
    // Public because it IS the provider contract `ui` is handed -- the thunk
    // below only forwards -- and because the state machine behind it had no test
    // at all until one could reach it.
    [[nodiscard]] bool requestImage(std::string_view urn, ui::ResolvedImage& out);

    // **Where a `view://` picture comes from** (ADR 0107): a texture something
    // draws into at run time, lent by whoever draws it and looked up each time
    // it is asked for, because it is remade whenever its size changes. Given
    // the name after `view://`; false while nothing draws under that name,
    // which draws as the element's own tint.
    struct ViewPicture
    {
        rhi::TextureHandle texture{};
        core::u32 width = 0;
        core::u32 height = 0;
    };
    using ViewLookup = std::function<bool(std::string_view name, ViewPicture& out)>;
    void setViewLookup(ViewLookup lookup) { viewLookup_ = std::move(lookup); }
    // Every view picture's texture as it is now, into the table the frame hands
    // the renderer: called after the views are made each frame, so a picture
    // nothing asked for this frame -- a world's UI laid out once -- never names
    // a texture its view destroyed. One that stopped drawing names none.
    void refreshViews();

    // Invalid until something has been rasterised, which is the state a build
    // with no font file stays in -- there the built-in vector face draws solid
    // rectangles and samples nothing.
    // A page of it (ADR 0169): each its own texture, made when the page is.
    [[nodiscard]] rhi::TextureHandle atlasTexture(core::u32 page = 0) const noexcept
    {
        return page < pages_.size() ? pages_[page].texture : rhi::TextureHandle{};
    }
    // How many bytes of the atlas have gone up, in all: what a new glyph costs.
    [[nodiscard]] core::u64 atlasBytesUploaded() const noexcept { return atlasBytes_; }
    // How many levels of pictures have gone up, in all: a picture is sent
    // with every level under it (D578).
    [[nodiscard]] core::u64 imageLevelsUploaded() const noexcept { return imageLevels_; }

    // Every image the UI has asked for, in the order it asked. The frame loop
    // appends these after the atlas's pages, so `ui::kFirstImageTexture` is
    // the first picture -- which is the numbering `ui::ResolvedImage::texture`
    // hands back.
    [[nodiscard]] std::span<const rhi::TextureHandle> images() const noexcept { return images_; }

private:
    const asset::ContentMounts* mounts_ = nullptr;
    std::string locale_;
    bool localeChanged_ = false;
    [[nodiscard]] std::string localizedName(std::string_view urn) const;
    // The atlas's pages as the GPU holds them: the texture, and the version
    // of the atlas its pixels are.
    struct AtlasPage
    {
        rhi::TextureHandle texture{};
        core::u64 uploaded = 0;
    };
    std::array<AtlasPage, ui::kGlyphPages> pages_{};
    core::u64 atlasBytes_ = 0;
    core::u64 imageLevels_ = 0;
    // The atlas expanded from coverage to RGBA. Kept between frames so a
    // re-upload does not allocate four megabytes every time a new glyph appears.
    std::vector<std::byte> staging_;

    // One entry per distinct `Image` URN, in first-asked order. Never removed
    // while the world lives: a picture a HUD shows on one screen is a picture it
    // will show again, and the index handed to `ui` has to stay meaning the same
    // texture for as long as a draw list can hold it.
    // **A named state rather than two booleans** (D128).
    //
    // `pending` and `failed` were sound only because the read, the decode and
    // the upload happened between two adjacent statements: `pending = false;
    // failed = true;` assumed failure and then took it back. Split across
    // frames, an in-flight entry reads as permanently dead -- and leaving
    // `pending` true instead re-queues the same URN every frame for ever. There
    // is no pair of booleans that says "in flight"; there is a state that does.
    enum class ImageState : core::u8
    {
        // Named by a label and waiting for a slot in the pipeline.
        Requested,
        Reading,
        Decoding,
        Ready,
        // Not there, not a picture, or the device refused it. Remembered so a
        // label naming a missing picture costs one lookup rather than one
        // attempt per frame.
        Failed,
        // It was on the card and was let go to keep the budget (D609). Its
        // place in the table stays its own -- the index handed to `ui` means
        // this picture for as long as the engine runs -- and the next ask
        // makes it `Requested` again.
        Released,
    };

    // What a decode job writes into, in ONE heap allocation.
    //
    // Not a field of `Image`, and that is the point: `imageEntries_` grows
    // whenever a label names a URN nobody has named before, which can happen on
    // the same frame a job is running -- so a job holding the address of
    // anything inside an element would be writing into freed memory after the
    // reallocation. `MeshLoader` learned this the hard way one field at a time.
    struct ImageWork
    {
        std::vector<std::byte> bytes;
        // The picture with every level under it: as the compiler made it, or
        // as the job made it from a file the compiler has not seen (D578).
        asset::TextureAsset compiled;
        bool ok = false;
    };

    struct Image
    {
        std::string urn;
        // The file it was read from: `urn`, or its file in the language in
        // force when it was. Empty until it has been looked for.
        std::string resolved;
        rhi::TextureHandle texture{};
        // A view's texture, lent and not owned: never loaded, never destroyed.
        bool borrowed = false;
        core::u32 width = 0;
        core::u32 height = 0;
        // What it holds on the card, every level, and the pass it was last
        // asked for in: what the budget weighs and orders by.
        core::u64 bytes = 0;
        core::u64 lastAsked = 0;
        ImageState state = ImageState::Requested;
        platform::IoRequest read;
        jobs::JobHandle decode;
        std::unique_ptr<ImageWork> work;
    };
    // How many may be in the pipeline at once. `MaxImages` is a lifetime cap on
    // distinct URNs and not a concurrency one; without this, a screen naming
    // three hundred icons would open three hundred files and hold three hundred
    // decoded images at the same time.
    static constexpr core::usize MaxImagesInFlight = 4;

    bool deferredImages_ = false;
    void queueImageDecode(Image& image);
    void pumpImages(rhi::IDevice& device, rhi::ICmdList& cmd);
    void releasePendingImages() noexcept;
    [[nodiscard]] bool imageInFlight(std::string_view urn) const noexcept;

    // How many reads or decodes are outstanding, so the bound above is a bound
    // and not a hope.
    core::usize bytesInFlight_ = 0;
    // Set where a handle is assigned, so the table below is rebuilt on the pass
    // that produced a texture rather than on the pass that queued one.
    bool imagesChanged_ = false;
    std::vector<Image> imageEntries_;
    std::vector<rhi::TextureHandle> images_;
    // The budget (D609): the pass count `lastAsked` is in, what is held, and
    // what has been let go.
    core::u64 imagePass_ = 0;
    core::u64 imageBudget_ = 0;
    core::u64 imageBytes_ = 0;
    core::u64 imagesReleased_ = 0;
    void trimImages(rhi::IDevice& device);
    ViewLookup viewLookup_;

    static bool requestImageThunk(void* user, std::string_view urn, ui::ResolvedImage& out);
    void loadPendingImages(rhi::IDevice& device, rhi::ICmdList& cmd);
};

} // namespace engine::app
