#include "engine/app/ui_text.h"

#include <algorithm>
#include <cstring>

#include "engine/asset/image.h"
#include "engine/asset/texture.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"
#include "engine/ui/ui.h"

namespace engine::app {
namespace {

// Resolves a `TextLabel.Font` to bytes out of the project's content.
//
// A free function with a `void*` because `ui::FaceProvider` is a C-style
// callback: `ui` is handed a pointer and a function and knows nothing about
// either, which is what keeps a content system out of a layout module.
bool resolveFace(void* user, std::string_view name, std::vector<core::u8>& out)
{
    const auto* const mounts = static_cast<const asset::ContentMounts*>(user);
    if (mounts == nullptr) {
        return false;
    }

    const asset::ResolvedContent resolved = mounts->resolve(name);
    if (!resolved.bytes.empty()) {
        // Out of a pack: the bytes are already resident, so this is a copy and
        // not a read.
        out.resize(resolved.bytes.size());
        std::memcpy(out.data(), resolved.bytes.data(), resolved.bytes.size());
        return true;
    }
    if (resolved.source != asset::ResolvedContent::Source::Loose) {
        return false;
    }

    std::vector<std::byte> bytes;
    if (!platform::readFile(resolved.path, bytes)) {
        return false;
    }
    // Read SYNCHRONOUSLY, and that is defensible where the streaming pump's
    // reads are not: a face is loaded once, the first time a label names it, and
    // the alternative is a frame of text in the wrong font while an async read
    // lands. A font is small and a font is needed now.
    out.resize(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

// The asset format enumeration, mapped onto the RHI's. A copy of the one in
// `mesh_loader.cpp` and deliberately so: `render` does not export it, and a
// third module reaching into another's translation unit to borrow a switch is a
// worse dependency than eight lines.
[[nodiscard]] rhi::TextureFormat toRhiFormat(asset::TextureFormat format) noexcept
{
    switch (format) {
    case asset::TextureFormat::Bc1Rgb:
        return rhi::TextureFormat::Bc1RgbaUnorm;
    case asset::TextureFormat::Bc3Rgba:
        return rhi::TextureFormat::Bc3RgbaUnorm;
    case asset::TextureFormat::Bc5Rg:
        return rhi::TextureFormat::Bc5RgUnorm;
    case asset::TextureFormat::Bc7Rgba:
        return rhi::TextureFormat::Bc7RgbaUnorm;
    case asset::TextureFormat::Astc4x4Rgba:
        return rhi::TextureFormat::Astc4x4RgbaUnorm;
    case asset::TextureFormat::Rgba8:
    case asset::TextureFormat::Unknown:
        break;
    }
    return rhi::TextureFormat::Rgba8Unorm;
}

// **A picture, uploaded with every level it has** (D578). It went up at its
// top level alone, "drawn at or near its own size" -- and a game's icons are
// 256 texels drawn seventeen to forty-four pixels tall: up to fifteen texels
// a pixel, of which a sampler reads four. Measured against a proper
// reduction, the top level alone is three times further from it than the
// levels are (`ui_renderer.cpp`, the sampler). A third more memory a picture.
// Returns the handle and adds the levels sent to `levels`.
[[nodiscard]] rhi::TextureHandle uploadTexture(rhi::IDevice& device, rhi::ICmdList& cmd,
                                               const asset::TextureAsset& texture, core::u64& levels)
{
    if (texture.mips.empty()) {
        return {};
    }
    const rhi::TextureHandle handle = device.createTexture({
        .format = toRhiFormat(texture.format),
        .usage = rhi::TextureUsage::Sampled,
        .width = texture.width,
        .height = texture.height,
        .mipLevels = static_cast<core::u32>(texture.mips.size()),
        .debugName = "ui-image",
    });
    if (!handle.valid()) {
        return {};
    }
    for (core::u32 level = 0; level < static_cast<core::u32>(texture.mips.size()); ++level) {
        const asset::TextureMip& mip = texture.mips[level];
        cmd.uploadTexture(handle, std::span<const std::byte>(texture.pixels).subspan(mip.offset, mip.size), level);
    }
    levels += texture.mips.size();
    return handle;
}

} // namespace

void UiText::setMounts(const asset::ContentMounts* mounts)
{
    if (mounts_ == mounts) {
        return;
    }
    mounts_ = mounts;
    // Installing a provider drops every loaded face (`ui::setFaceProvider`), so
    // this must not run every frame -- hence the identity check above.
    ui::setFaceProvider(mounts != nullptr ? &resolveFace : nullptr, const_cast<asset::ContentMounts*>(mounts));
    ui::setImageProvider(mounts != nullptr ? &UiText::requestImageThunk : nullptr, this);
}

void UiText::refreshViews()
{
    for (core::usize slot = 0; slot < imageEntries_.size(); ++slot) {
        Image& image = imageEntries_[slot];
        if (!image.borrowed)
            continue;
        ViewPicture picture;
        const bool drawn =
            viewLookup_ && viewLookup_(std::string_view(image.urn).substr(7), picture) && picture.texture.valid();
        const rhi::TextureHandle now = drawn ? picture.texture : rhi::TextureHandle{};
        if (drawn) {
            image.width = picture.width;
            image.height = picture.height;
        }
        if (image.texture == now)
            continue;
        image.texture = now;
        imagesChanged_ = true;
        if (slot < images_.size())
            images_[slot] = now;
    }
}

bool UiText::requestImageThunk(void* user, std::string_view urn, ui::ResolvedImage& out)
{
    return static_cast<UiText*>(user)->requestImage(urn, out);
}

bool UiText::requestImage(std::string_view urn, ui::ResolvedImage& out)
{
    // A view's picture: looked up every time, because the texture behind the
    // name is remade when its size changes.
    if (urn.starts_with("view://")) {
        ViewPicture picture;
        const bool drawn = viewLookup_ && viewLookup_(urn.substr(7), picture) && picture.texture.valid();
        auto entry = std::find_if(imageEntries_.begin(), imageEntries_.end(),
                                  [&](const Image& image) { return image.urn == urn; });
        if (entry == imageEntries_.end()) {
            if (!drawn || imageEntries_.size() >= 1024)
                return false;
            Image image;
            image.urn = std::string(urn);
            image.borrowed = true;
            image.state = ImageState::Ready;
            imageEntries_.push_back(std::move(image));
            entry = imageEntries_.end() - 1;
        }
        if (!drawn)
            return false;
        if (!(entry->texture == picture.texture)) {
            entry->texture = picture.texture;
            imagesChanged_ = true;
            // **Into the table at once**, not at the next `sync`: the frame
            // copies the table before that, and the texture this replaces was
            // destroyed when its view was remade -- a renderer given it binds a
            // texture that is gone (the 26-security-cameras crash).
            //
            // **And a picture asked for the first time is given its place in
            // the table here** (D575). The table had one for it only from the
            // next `sync`, a frame on: the draw list named a place past the
            // table's end, which is "no texture", and a `ViewportFrame` was a
            // white square for the frame it was first shown in.
            const auto slot = static_cast<core::usize>(entry - imageEntries_.begin());
            if (slot >= images_.size())
                images_.resize(slot + 1);
            images_[slot] = picture.texture;
        }
        entry->width = picture.width;
        entry->height = picture.height;
        out.texture = static_cast<core::u32>(entry - imageEntries_.begin()) + ui::kFirstImageTexture;
        out.width = picture.width;
        out.height = picture.height;
        return true;
    }

    for (core::usize index = 0; index < imageEntries_.size(); ++index) {
        const Image& image = imageEntries_[index];
        if (image.urn != urn) {
            continue;
        }
        if (image.state != ImageState::Ready || !image.texture.valid()) {
            return false;
        }
        // Index 0 is "no texture" and index 1 is the glyph atlas, so the first
        // picture is 2. The frame loop builds the table in exactly that order.
        out.texture = static_cast<core::u32>(index) + ui::kFirstImageTexture;
        out.width = image.width;
        out.height = image.height;
        return true;
    }

    // Bounded, because the index is handed to `ui` as a small integer and a
    // world that named ten thousand pictures would be a world that leaked one
    // texture per name. Past the cap a new name simply does not load, which
    // draws as the flat tint rather than as a crash.
    constexpr core::usize MaxImages = 1024;
    if (imageEntries_.size() >= MaxImages) {
        return false;
    }

    Image image;
    image.urn = std::string(urn);
    imageEntries_.push_back(std::move(image));
    return false;
}

void UiText::loadPendingImages(rhi::IDevice& device, rhi::ICmdList& cmd)
{
    if (mounts_ == nullptr) {
        return;
    }

    if (deferredImages_) {
        pumpImages(device, cmd);
    }

    bool changed = false;
    core::usize started = 0;
    for (Image& image : imageEntries_) {
        if (image.state != ImageState::Requested) {
            continue;
        }

        // A full pipeline leaves later names queued. Falling through here
        // decoded the overflow synchronously, making the bound ineffective.
        // Also bound attempts when async IO is unavailable and falls back.
        if (deferredImages_) {
            if (bytesInFlight_ >= MaxImagesInFlight || started >= MaxImagesInFlight)
                break;
            ++started;
        }

        const asset::ResolvedContent resolved = mounts_->resolve(image.urn);

        if (deferredImages_ && !resolved.bytes.empty()) {
            image.work = std::make_unique<ImageWork>();
            // Mounts may be cleared/replaced while a decode is outstanding.
            // Resolve on the frame, then give the job its own bounded storage.
            image.work->bytes.assign(resolved.bytes.begin(), resolved.bytes.end());
            queueImageDecode(image);
            continue;
        }

        // **Queued rather than read, and only up to a bound.** A screen naming
        // three hundred icons must not open three hundred files and hold three
        // hundred decoded images at the same time.
        if (deferredImages_ && resolved.source == asset::ResolvedContent::Source::Loose) {
            image.read = platform::readFileAsync(resolved.path, platform::IoPriority::Low);
            if (image.read.valid()) {
                image.state = ImageState::Reading;
                ++bytesInFlight_;
                continue;
            }
            // No IO service, or its pool is full. Deferring is a way of doing
            // the work and not permission to skip it, so this falls through to
            // the synchronous read below.
        }

        // From here the entry is being resolved on THIS frame, so the old
        // assume-failed idiom is sound again: the read, the decode and the
        // upload are adjacent statements.
        image.state = ImageState::Failed;
        changed = true;

        std::vector<std::byte> owned;
        std::span<const std::byte> bytes = resolved.bytes;
        if (bytes.empty() && resolved.source == asset::ResolvedContent::Source::Loose) {
            if (!platform::readFile(resolved.path, owned)) {
                continue;
            }
            bytes = owned;
        }
        if (bytes.empty()) {
            const core::I18nArg args[] = {{"content", image.urn}};
            core::log(core::LogLevel::Warn, ENG_TR("app.warn.image_missing"), args);
            continue;
        }

        // A compiled texture first, then an encoded one. A project built through
        // `assetc` carries KTX2; a project run straight out of its source tree
        // carries the PNG the artist saved. Both have to work, because dev mode
        // is the mode people spend their time in.
        asset::TextureAsset compiled;
        if (!asset::transcodeTexture(bytes, asset::TranscodeOptions{}, compiled).has_value() && compiled.valid()) {
            image.texture = uploadTexture(device, cmd, compiled, imageLevels_);
            image.width = compiled.width;
            image.height = compiled.height;
        }
        else {
            asset::Image decoded;
            if (asset::decodeImage(bytes, decoded).has_value()) {
                const core::I18nArg args[] = {{"content", image.urn}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.image_undecodable"), args);
                continue;
            }
            // A picture the compiler has not seen has no levels of its own:
            // made here, as the compiler would have.
            image.texture = uploadTexture(device, cmd, asset::mipChainOf(decoded, true), imageLevels_);
            image.width = decoded.width;
            image.height = decoded.height;
        }
        image.state = image.texture.valid() ? ImageState::Ready : ImageState::Failed;
    }

    // **Rebuilt where a HANDLE is assigned, not where a queue is drained**
    // (D128). `changed` used to be set the moment an entry left the pending
    // list, which was sound only because the upload happened two statements
    // later. Deferred, that writes an invalid handle into the table on the
    // queueing pass and never rebuilds on the completion pass -- and the lengths
    // agree, so no size check catches it. The picture then draws as flat tint
    // for ever with nothing logged.
    if (changed || imagesChanged_) {
        imagesChanged_ = false;
        images_.clear();
        images_.reserve(imageEntries_.size());
        for (const Image& image : imageEntries_) {
            images_.push_back(image.texture);
        }
    }
}

bool UiText::imageInFlight(std::string_view urn) const noexcept
{
    for (const Image& image : imageEntries_) {
        if (image.urn == urn && ((deferredImages_ && image.state == ImageState::Requested) ||
                                 image.state == ImageState::Reading || image.state == ImageState::Decoding))
            return true;
    }
    return false;
}

core::usize UiText::imagesInFlight() const noexcept
{
    core::usize count = 0;
    for (const Image& image : imageEntries_) {
        if ((deferredImages_ && image.state == ImageState::Requested) || image.state == ImageState::Reading ||
            image.state == ImageState::Decoding)
            ++count;
    }
    return count;
}

void UiText::releasePendingImages() noexcept
{
    for (Image& image : imageEntries_) {
        // **Waited for, not abandoned.** The job writes into memory this object
        // owns, and returning while one runs is a use-after-free that reproduces
        // on a fast machine and never on a slow one.
        if (image.decode.valid())
            jobs::wait(image.decode);
        if (image.read.valid())
            platform::cancelIo(image.read);
        image.decode = {};
        image.read = {};
        image.work.reset();
    }
    bytesInFlight_ = 0;
}

void UiText::queueImageDecode(Image& image)
{
    // **One pointer, to memory that does not move.** Another label
    // naming a new URN reallocates `imageEntries_`, so anything the job
    // addresses has to live somewhere the vector is not.
    ImageWork* work = image.work.get();
    image.decode = jobs::schedule("ui-image-decode", jobs::Domain::AssetIo, [work]() noexcept {
        // A compiled texture first, then an encoded one -- the same
        // order and the same reason as the synchronous path: a project
        // built through `assetc` carries KTX2, one run out of its source
        // tree carries the PNG the artist saved.
        if (!asset::transcodeTexture(work->bytes, asset::TranscodeOptions{}, work->compiled).has_value() &&
            work->compiled.valid()) {
            work->ok = true;
        }
        else if (asset::Image decoded; !asset::decodeImage(work->bytes, decoded).has_value()) {
            // With its smaller levels, made here and not on the frame.
            work->compiled = asset::mipChainOf(decoded, true);
            work->ok = work->compiled.valid();
        }
        work->bytes.clear();
        work->bytes.shrink_to_fit();
    });
    if (!image.decode.valid()) {
        image.state = ImageState::Failed;
        image.work.reset();
        imagesChanged_ = true;
        return;
    }
    image.state = ImageState::Decoding;
    ++bytesInFlight_;
}

// The read and the decode stages of a deferred image. Only the upload is left
// on the frame, because only the frame has a command list.
void UiText::pumpImages(rhi::IDevice& device, rhi::ICmdList& cmd)
{
    // **Only in the deferred branch.** An unconditional pump would drain
    // completions for other subsystems on frames where they do not currently
    // land, which can move streaming-dependent output.
    platform::pumpIo();

    for (Image& image : imageEntries_) {
        if (image.state == ImageState::Reading) {
            const platform::IoStatus status = platform::ioStatus(image.read);
            if (status == platform::IoStatus::Pending)
                continue;

            image.work = std::make_unique<ImageWork>();
            const bool got =
                status == platform::IoStatus::Ready && platform::takeIoResult(image.read, image.work->bytes);
            // **A request that ended without being TAKEN still holds its
            // slot** -- `takeIoResult` releases one only for `Ready`, and the
            // pool is a fixed 512. A project with missing files would fill it
            // and every later read would be refused, which reads as "textures
            // stopped loading after a while" and has nothing in the log.
            // `cancelIo` is the documented way to let a terminal one go.
            if (!got)
                platform::cancelIo(image.read);
            image.read = {};
            if (bytesInFlight_ > 0)
                --bytesInFlight_;
            if (!got || image.work->bytes.empty()) {
                image.state = ImageState::Failed;
                image.work.reset();
                imagesChanged_ = true;
                continue;
            }

            queueImageDecode(image);
            continue;
        }

        if (image.state != ImageState::Decoding || !jobs::finished(image.decode))
            continue;

        image.decode = {};
        if (bytesInFlight_ > 0)
            --bytesInFlight_;
        // **No logging from inside the job.** The flag comes back and the
        // sentence is said here, on the frame thread.
        if (image.work != nullptr && image.work->ok) {
            image.texture = uploadTexture(device, cmd, image.work->compiled, imageLevels_);
            image.width = image.work->compiled.width;
            image.height = image.work->compiled.height;
        }
        else {
            const core::I18nArg args[] = {{"content", image.urn}};
            core::log(core::LogLevel::Warn, ENG_TR("app.warn.image_undecodable"), args);
        }
        image.state = image.texture.valid() ? ImageState::Ready : ImageState::Failed;
        image.work.reset();
        imagesChanged_ = true;
    }
}

UiText::~UiText()
{
    // No device here, so no texture can be freed -- `destroy` does that. What
    // cannot be left is a job still writing into memory this object is about to
    // release.
    releasePendingImages();
}

void UiText::sync(rhi::IDevice& device, rhi::ICmdList& cmd)
{
    loadPendingImages(device, cmd);

    // **A page of the atlas a texture** (ADR 0169). A page that fills is
    // followed by another, so nothing already sent is sent again; and one
    // that was emptied to make room is sent as it is filled, a glyph's rows
    // at a time (D543) -- what is left of the old pixels beside them is on no
    // glyph's rows, and nothing samples it.
    const core::u32 count = ui::glyphAtlasPages();
    for (core::u32 index = 0; index < pages_.size(); ++index) {
        AtlasPage& page = pages_[index];
        if (index >= count) {
            // A page the store no longer has: its texture goes with it.
            if (page.texture.valid())
                device.destroy(page.texture);
            page = AtlasPage{};
            continue;
        }
        const ui::GlyphAtlas source = ui::glyphAtlas(index);
        if (source.pixels.empty() || source.width == 0 || source.height == 0)
            continue;
        if (page.texture.valid() && source.version == page.uploaded)
            continue;
        if (!page.texture.valid()) {
            page.texture = device.createTexture({
                .format = rhi::TextureFormat::Rgba8Unorm,
                .usage = rhi::TextureUsage::Sampled,
                .width = source.width,
                .height = source.height,
                .debugName = "ui-glyph-atlas",
            });
            page.uploaded = 0;
            if (!page.texture.valid())
                continue;
        }

        // **RGBA rather than R8**, and the reason is that there is one UI shader.
        //
        // A glyph is coverage, and what the shader wants is `tint * sample` with the
        // coverage in ALPHA -- but the same multiplication has to serve a picture,
        // which carries its own colour. An R8 texture samples as (r, 0, 0, 1), which
        // is neither. Expanding to white-with-coverage-alpha makes one multiplication
        // correct for both, and the four megabytes buys not having a second pipeline,
        // a second sort and a state change per element.
        //
        // The CPU-side atlas stays one byte a texel, which is the honest storage;
        // this is the upload.
        const auto expand = [&](core::u32 firstRow, core::u32 rowCount) {
            const core::usize texels = static_cast<core::usize>(source.width) * rowCount;
            const core::u8* const from = source.pixels.data() + static_cast<core::usize>(firstRow) * source.width;
            staging_.resize(texels * 4u);
            for (core::usize i = 0; i < texels; ++i) {
                staging_[i * 4 + 0] = std::byte{0xFF};
                staging_[i * 4 + 1] = std::byte{0xFF};
                staging_[i * 4 + 2] = std::byte{0xFF};
                staging_[i * 4 + 3] = std::byte{from[i]};
            }
            atlasBytes_ += staging_.size();
        };
        // Each run of rows written since, the width of the page: a shelf's
        // worth for the glyphs of one frame, most often one run. A page that
        // is new on the GPU has every row it was written on to send, and
        // those are the rows above its cursor -- never the whole of it.
        for (core::u32 row = 0; row < source.height;) {
            if (source.rowVersions[row] <= page.uploaded) {
                row += 1;
                continue;
            }
            core::u32 last = row + 1;
            while (last < source.height && source.rowVersions[last] > page.uploaded)
                last += 1;
            expand(row, last - row);
            cmd.uploadTextureRegion(page.texture, 0, row, source.width, last - row, staging_);
            row = last;
        }
        page.uploaded = source.version;
    }
}

void UiText::destroy(rhi::IDevice& device)
{
    releasePendingImages();
    for (AtlasPage& page : pages_) {
        if (page.texture.valid())
            device.destroy(page.texture);
        page = AtlasPage{};
    }
    staging_.clear();
    staging_.shrink_to_fit();
    for (const Image& image : imageEntries_) {
        if (image.texture.valid() && !image.borrowed) {
            device.destroy(image.texture);
        }
    }
    imageEntries_.clear();
    images_.clear();
    setMounts(nullptr);
}

} // namespace engine::app
