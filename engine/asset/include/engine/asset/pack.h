// The `.lpack` content-addressed archive (architecture.md §10, ADR 0010).
//
// A pack is a header, a run of blobs, and a table of contents sorted by content
// hash. Sorted for two reasons and both matter: a lookup is a binary search
// over the TOC, and the FILE ITSELF is then a function of its contents alone --
// same inputs, same bytes, which is what makes an asset build reproducible and
// a CI cache legitimate.
//
// **Every offset is checked against the file length before it is followed.**
// That is a rule this reader is written under rather than a bug it was patched
// for: the milestone gate is a fuzz test that truncates and corrupts real packs
// and requires a structured error and no crash. A reader that trusts its own
// header is a reader that segfaults on a half-downloaded file.
//
// **The header carries a hash of the table of contents, and every open checks
// it.** The first run of the fuzz case found why: a flipped bit in an entry's
// `kind` turns a mesh into a prefab and passes every structural check, because
// nothing else in the file says what that entry should have been. Blob bytes
// are covered by their own names, header fields by their consistency with each
// other -- the TOC was the one region with neither, and 16 bytes closes it.
// Checking it on EVERY open rather than only a verifying one is deliberate: the
// TOC is what offsets are read from, and it is small enough that hashing it
// costs nothing next to the read that delivered the file.
//
// Errors are i18n keys (R3), and there are several rather than one, because a
// person holding a pack that will not open deserves to know whether it is the
// wrong kind of file, a truncated one, or one whose bytes no longer hash to
// their own name.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/content_hash.h"
#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::platform {
class MappedFile;
} // namespace engine::platform

namespace engine::asset {

using core::ContentHash;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

// What a blob is, so a reader can refuse to interpret a mesh as a texture. The
// pack does not care -- it stores bytes -- but the manifest and the loader do,
// and putting the kind in the TOC means a wrong lookup fails at the index
// rather than inside a decoder.
enum class AssetKind : u32
{
    Unknown = 0,
    Mesh = 1,
    Texture = 2,
    Prefab = 3,
    Chunk = 4,
    // Anything the pipeline copies through untouched: a font file, a shader
    // blob, a catalog.
    Raw = 5,
    // A compiled material: the parameter block plus the hashes of the textures
    // it samples. Its own kind rather than `Raw` so that a mesh asking for a
    // material and getting a font fails at the index (`content.h`), which is
    // the whole reason this enum is in the table of contents.
    Material = 6,
    // A compiled surface shader (ADR 0091): its source and its bytecode for
    // each target the build could compile (`surface_build.h`).
    Surface = 7,
    // **What a sealed pack answers to** (ADR 0183): a table of the names it
    // holds, each as the hash of the name beside the hash of what it names
    // (`PackName`). One a pack at most, and a pack with one needs no manifest
    // beside it -- nor carries a single name anybody can read.
    Names = 8,
};

// Stable, lowercase, and written into the content manifest -- so it is a name
// a tool may compare against rather than an integer that moves when the enum
// grows.
[[nodiscard]] const char* assetKindName(AssetKind kind) noexcept;

// How a blob is stored. Only `None` exists in v1 -- the mesh format already
// carries meshopt-compressed streams and a texture is already a compressed
// container, so a second general-purpose pass over them would spend CPU to save
// almost nothing. The field exists so adding one later is a version bump rather
// than a format break.
//
// **`Deflate`, in format 2** (ADR 0183): a zlib stream, for what is text and
// was stored as text -- a scene, a stamp, a material, a script's bytecode, a
// catalogue, the names. An entry's hash is the hash of what it holds once
// inflated, so a compressed entry is named as the same bytes stored plain.
enum class BlobCodec : u32
{
    None = 0,
    Deflate = 1,
};

// The newest format this build writes and reads. **A pack that uses nothing
// of format 2 is written as format 1**, byte for byte what it always was: a
// project's own pack, every pack a test holds.
inline constexpr u32 PackFormatVersion = 2;
inline constexpr u32 PackFormatPlain = 1;

// "LGPK". Written and compared as bytes, never as a u32, so the file does not
// mean something different on a big-endian machine.
inline constexpr char PackMagic[4] = {'L', 'G', 'P', 'K'};

inline constexpr usize PackHeaderBytes = 48;
inline constexpr usize PackEntryBytes = 48;

struct PackEntry
{
    ContentHash hash;
    u64 offset = 0;
    u64 storedSize = 0;
    u64 originalSize = 0;
    AssetKind kind = AssetKind::Unknown;
    BlobCodec codec = BlobCodec::None;
};

// Builds a pack in memory, then writes it. In memory because a pack is small
// enough to hold -- a chunk's worth of meshes, not a game's worth -- and
// because the TOC cannot be written until every blob's size is known.
class PackWriter
{
public:
    // Adding the same hash twice is a no-op that returns the existing entry:
    // content addressing means two identical blobs ARE one blob, and dedupe
    // across chunks is the property that buys.
    void add(const ContentHash& hash, AssetKind kind, std::span<const std::byte> bytes);

    // Hashes the bytes and adds them under that hash. The normal path -- the
    // one that cannot disagree with itself about what a blob is called.
    ContentHash addContent(AssetKind kind, std::span<const std::byte> bytes);

    [[nodiscard]] bool contains(const ContentHash& hash) const noexcept;

    // Every blob added, by hash and kind, in insertion order.
    //
    // **For a caller that has to write the same content somewhere a pack is
    // not** -- an object store, which is one file per blob and can be added to
    // one import at a time where a pack has to be rewritten whole (E9 step 12).
    // Spans into this writer, so they last as long as it does and no longer.
    struct BlobView
    {
        ContentHash hash;
        AssetKind kind = AssetKind::Unknown;
        std::span<const std::byte> bytes;
    };
    [[nodiscard]] std::vector<BlobView> blobs() const;
    [[nodiscard]] usize count() const noexcept { return m_blobs.size(); }
    [[nodiscard]] u64 payloadBytes() const noexcept;

    // The finished file. Entries are sorted by hash and blobs are written in
    // that order, so this is a pure function of what was added and NOT of the
    // order it was added in.
    [[nodiscard]] std::vector<std::byte> build() const;

    // **The same, sealed** (ADR 0183): what is text is deflated where that
    // makes it smaller -- `Raw`, `Material`, `Surface` and `Names` entries --
    // and the file is format 2. Still a pure function of what was added.
    [[nodiscard]] std::vector<std::byte> buildSealed() const;

private:
    [[nodiscard]] std::vector<std::byte> write(bool sealed) const;

    struct Blob
    {
        ContentHash hash;
        AssetKind kind = AssetKind::Unknown;
        std::vector<std::byte> bytes;
    };

    std::vector<Blob> m_blobs;
};

// A pack held in memory, validated once at open.
//
// Whole-file rather than a file handle with seeks: a pack is read by the
// streaming pipeline through `platform::readFileAsync`, which delivers bytes,
// and a reader that also knew how to open files would have two ways in and one
// of them untested. A pack opened from disk (`openPackFile`) is the whole file
// MAPPED, not read (audit F10): the same bytes, paged in as they are touched.
class Pack
{
public:
    // Validates the header and every TOC entry before returning. On failure the
    // Pack is left empty and the error names which check failed.
    [[nodiscard]] static std::optional<core::EngineError> open(std::vector<std::byte> bytes, Pack& out);

    // The same, plus rehashing every BLOB and comparing it against its own
    // name. Off by default because it costs a pass over the whole file; the
    // asset build and the fuzz gate turn it on, and a shipped game trusts the
    // structure -- and the table of contents, which `open` hashes either way.
    [[nodiscard]] static std::optional<core::EngineError> openVerified(std::vector<std::byte> bytes, Pack& out);

    // The same over a file mapped into memory, which the pack keeps mapped
    // for as long as it lives (audit F10).
    [[nodiscard]] static std::optional<core::EngineError> openMapped(std::shared_ptr<platform::MappedFile> file,
                                                                     bool verify, Pack& out);

    [[nodiscard]] usize count() const noexcept { return m_entries.size(); }
    [[nodiscard]] std::span<const PackEntry> entries() const noexcept { return m_entries; }

    [[nodiscard]] const PackEntry* find(const ContentHash& hash) const noexcept;
    [[nodiscard]] bool contains(const ContentHash& hash) const noexcept { return find(hash) != nullptr; }

    // A view into this Pack's own storage: valid while the Pack is, and empty
    // for a hash it does not hold.
    //
    // **A deflated entry is inflated the first time it is asked for, checked
    // against its name, and kept** (ADR 0183): the view is into what the pack
    // keeps, valid while the Pack is, as any other. One that does not inflate
    // to its own hash is empty, and `damaged` says so from then on. Safe from
    // any thread.
    [[nodiscard]] std::span<const std::byte> blob(const ContentHash& hash) const noexcept;

    // The pack's one `Names` entry, or null: what makes it a sealed pack.
    [[nodiscard]] const PackEntry* names() const noexcept;

    // **Every entry against its own name**: each blob hashed -- inflated first
    // where it is deflated -- and compared with the hash it is filed under.
    // What `openVerified` does at open, for a pack already open: a shipped game
    // runs it on a worker while it loads, and a file that was cut short or
    // changed on the way is said to be damaged instead of read. Keeps nothing.
    [[nodiscard]] std::optional<core::EngineError> verify() const;

    // Whether an entry has been found not to be what its name says.
    [[nodiscard]] bool damaged() const noexcept;

private:
    [[nodiscard]] static std::optional<core::EngineError> validate(std::span<const std::byte> bytes, bool verify,
                                                                   std::vector<PackEntry>& entries);
    [[nodiscard]] static std::optional<core::EngineError> verifyEntries(std::span<const std::byte> bytes,
                                                                        std::span<const PackEntry> entries);
    [[nodiscard]] std::span<const std::byte> storage() const noexcept;

    // What the pack's bytes are: a buffer it owns, or a file mapped into
    // memory (audit F10), whichever `storage` finds.
    std::vector<std::byte> m_bytes;
    std::shared_ptr<platform::MappedFile> m_mapped;
    std::vector<PackEntry> m_entries;
    // What has been inflated, and whether anything was found damaged. Apart,
    // behind a pointer, so a Pack is still a thing that can be moved.
    struct Inflated;
    std::shared_ptr<Inflated> m_inflated;
};

// **One name a sealed pack answers to** (ADR 0183): the hash of the name --
// `asset://textures/base.png`, as the game says it -- and the hash and kind
// of what it names. The name itself is nowhere in the pack.
struct PackName
{
    ContentHash name;
    ContentHash content;
    AssetKind kind = AssetKind::Unknown;
};

// The bytes of a `Names` entry for these rows, sorted by name; and the rows of
// one, refused where it is not what `encodePackNames` writes.
[[nodiscard]] std::vector<std::byte> encodePackNames(std::vector<PackName> names);
[[nodiscard]] bool decodePackNames(std::span<const std::byte> bytes, std::vector<PackName>& out);
// The row for `name` in rows sorted as `decodePackNames` leaves them, or null.
[[nodiscard]] const PackName* findPackName(std::span<const PackName> names, std::string_view name) noexcept;

// Reads a pack file from disk through `platform::readFile`, so an APK entry
// works exactly as a loose file does.
[[nodiscard]] std::optional<core::EngineError> openPackFile(const std::filesystem::path& path, Pack& out,
                                                            bool verify = false);

} // namespace engine::asset
