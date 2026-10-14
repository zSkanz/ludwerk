#include "engine/asset/pack.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/platform/file.h"

// `image.cpp` compiles stb_image and stb_image_write, and these are their
// zlib: declared here rather than through headers that would compile a second
// copy of either.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);
extern "C" int stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

namespace engine::asset {
namespace {

using core::I18nArg;

// Scalars are little-endian, hashes are their own canonical byte order (the
// digest's, which is what `core::toBytes` writes). Stated rather than inherited
// from the host: a format whose meaning depends on the machine that wrote it is
// not a format.
void writeU32(std::vector<std::byte>& out, u32 value)
{
    for (usize i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void writeU64(std::vector<std::byte>& out, u64 value)
{
    for (usize i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFu));
    }
}

[[nodiscard]] u32 readU32(const std::byte* bytes) noexcept
{
    u32 value = 0;
    for (usize i = 0; i < 4; ++i) {
        value |= static_cast<u32>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    }
    return value;
}

[[nodiscard]] u64 readU64(const std::byte* bytes) noexcept
{
    u64 value = 0;
    for (usize i = 0; i < 8; ++i) {
        value |= static_cast<u64>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    }
    return value;
}

// The highest kind, not `Raw`: `Material` came after it, and a reader that
// stopped at `Raw` refused the first pack that held one -- the kind had been in
// the table since ADR 0060 with no writer, so nothing had ever tried (ADR 0090).
// What format 1 knows, and what format 2 adds to it.
[[nodiscard]] bool knownKind(u32 value, u32 version) noexcept
{
    return value <= static_cast<u32>(version >= 2 ? AssetKind::Names : AssetKind::Surface);
}

[[nodiscard]] bool knownCodec(u32 value, u32 version) noexcept
{
    return value == static_cast<u32>(BlobCodec::None) ||
           (version >= 2 && value == static_cast<u32>(BlobCodec::Deflate));
}

// The largest entry a pack may claim to inflate to: a bound on what a damaged
// or hostile table of contents can make this build allocate.
constexpr u64 MostInflated = 1ull << 30;

// What is small is deflated whatever that gives: a short script or a line of
// JSON does not shrink, and stored plain it would be there to read.
constexpr usize AlwaysDeflatedUnder = 64 * 1024;

// A zlib stream of `bytes`; or nothing for what is large and would not be
// smaller by a twentieth -- a font, a sound, a file that is already
// compressed, stored as it is and read where it lies.
[[nodiscard]] std::vector<std::byte> deflated(std::span<const std::byte> bytes)
{
    if (bytes.empty() || bytes.size() > static_cast<usize>(std::numeric_limits<int>::max()))
        return {};
    int length = 0;
    unsigned char* const made =
        stbi_zlib_compress(const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data())),
                           static_cast<int>(bytes.size()), &length, 8);
    if (made == nullptr)
        return {};
    std::vector<std::byte> out;
    if (length > 0 &&
        (bytes.size() < AlwaysDeflatedUnder || static_cast<usize>(length) + bytes.size() / 20 < bytes.size()))
        out.assign(reinterpret_cast<const std::byte*>(made), reinterpret_cast<const std::byte*>(made) + length);
    std::free(made);
    return out;
}

// `stored` inflated to exactly `size` bytes; false where it is not a zlib
// stream of that many.
[[nodiscard]] bool inflate(std::span<const std::byte> stored, u64 size, std::vector<std::byte>& out)
{
    if (size == 0 || size > MostInflated || stored.size() > static_cast<usize>(std::numeric_limits<int>::max()))
        return false;
    out.assign(static_cast<usize>(size), std::byte{0});
    const int written =
        stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()),
                                reinterpret_cast<const char*>(stored.data()), static_cast<int>(stored.size()));
    return written >= 0 && static_cast<u64>(written) == size;
}

[[nodiscard]] bool sealable(AssetKind kind) noexcept
{
    return kind == AssetKind::Raw || kind == AssetKind::Material || kind == AssetKind::Surface ||
           kind == AssetKind::Names;
}

constexpr char NamesMagic[4] = {'L', 'G', 'N', 'M'};
constexpr u32 NamesVersion = 1;
constexpr usize NamesHeaderBytes = 12;
constexpr usize NameRowBytes = 40;

} // namespace

// What a pack has inflated, by the hash it is filed under. An entry present
// and empty is one that did not inflate to its name: asked for again, it is
// not tried again.
struct Pack::Inflated
{
    std::mutex guard;
    std::unordered_map<ContentHash, std::vector<std::byte>> blobs;
    bool damaged = false;
};

const char* assetKindName(AssetKind kind) noexcept
{
    switch (kind) {
    case AssetKind::Mesh:
        return "mesh";
    case AssetKind::Texture:
        return "texture";
    case AssetKind::Prefab:
        return "prefab";
    case AssetKind::Chunk:
        return "chunk";
    case AssetKind::Raw:
        return "raw";
    case AssetKind::Material:
        return "material";
    case AssetKind::Surface:
        return "surface";
    case AssetKind::Names:
        return "names";
    case AssetKind::Unknown:
        break;
    }
    return "unknown";
}

void PackWriter::add(const ContentHash& hash, AssetKind kind, std::span<const std::byte> bytes)
{
    if (contains(hash)) {
        return;
    }
    Blob blob;
    blob.hash = hash;
    blob.kind = kind;
    blob.bytes.assign(bytes.begin(), bytes.end());
    m_blobs.push_back(std::move(blob));
}

ContentHash PackWriter::addContent(AssetKind kind, std::span<const std::byte> bytes)
{
    const ContentHash hash = core::hashBytes(bytes);
    add(hash, kind, bytes);
    return hash;
}

std::vector<PackWriter::BlobView> PackWriter::blobs() const
{
    std::vector<BlobView> out;
    out.reserve(m_blobs.size());
    for (const Blob& blob : m_blobs)
        out.push_back(BlobView{blob.hash, blob.kind, std::span<const std::byte>(blob.bytes)});
    return out;
}

bool PackWriter::contains(const ContentHash& hash) const noexcept
{
    return std::any_of(m_blobs.begin(), m_blobs.end(), [&hash](const Blob& blob) { return blob.hash == hash; });
}

u64 PackWriter::payloadBytes() const noexcept
{
    u64 total = 0;
    for (const Blob& blob : m_blobs) {
        total += blob.bytes.size();
    }
    return total;
}

std::vector<std::byte> PackWriter::build() const
{
    return write(false);
}

std::vector<std::byte> PackWriter::buildSealed() const
{
    return write(true);
}

std::vector<std::byte> PackWriter::write(bool sealed) const
{
    // Sorted by hash, which is what makes the output a function of the CONTENT
    // rather than of the order the caller happened to add things in. Two builds
    // of the same assets are byte-identical, and that is the property the CI
    // determinism check asserts.
    std::vector<const Blob*> ordered;
    ordered.reserve(m_blobs.size());
    for (const Blob& blob : m_blobs) {
        ordered.push_back(&blob);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Blob* a, const Blob* b) { return a->hash < b->hash; });

    std::vector<std::byte> out;
    out.reserve(PackHeaderBytes + static_cast<usize>(payloadBytes()) + ordered.size() * PackEntryBytes);

    out.insert(out.end(), reinterpret_cast<const std::byte*>(PackMagic),
               reinterpret_cast<const std::byte*>(PackMagic) + 4);
    writeU32(out, sealed ? PackFormatVersion : PackFormatPlain);
    writeU32(out, 0); // flags, reserved
    writeU32(out, static_cast<u32>(ordered.size()));
    writeU64(out, 0);                          // tocOffset, patched below
    writeU64(out, 0);                          // tocLength, patched below
    out.resize(out.size() + 16, std::byte{0}); // tocHash, patched below

    std::vector<u64> offsets;
    std::vector<u64> storedSizes;
    std::vector<BlobCodec> codecs;
    offsets.reserve(ordered.size());
    storedSizes.reserve(ordered.size());
    codecs.reserve(ordered.size());
    for (const Blob* blob : ordered) {
        offsets.push_back(out.size());
        const std::vector<std::byte> smaller =
            sealed && sealable(blob->kind) ? deflated(blob->bytes) : std::vector<std::byte>{};
        if (!smaller.empty()) {
            out.insert(out.end(), smaller.begin(), smaller.end());
            storedSizes.push_back(smaller.size());
            codecs.push_back(BlobCodec::Deflate);
        }
        else {
            out.insert(out.end(), blob->bytes.begin(), blob->bytes.end());
            storedSizes.push_back(blob->bytes.size());
            codecs.push_back(BlobCodec::None);
        }
    }

    const u64 tocOffset = out.size();
    for (usize i = 0; i < ordered.size(); ++i) {
        const Blob& blob = *ordered[i];
        const std::array<std::byte, 16> hashBytes = core::toBytes(blob.hash);
        out.insert(out.end(), hashBytes.begin(), hashBytes.end());
        writeU64(out, offsets[i]);
        writeU64(out, storedSizes[i]);
        writeU64(out, blob.bytes.size());
        writeU32(out, static_cast<u32>(blob.kind));
        writeU32(out, static_cast<u32>(codecs[i]));
    }
    const u64 tocLength = out.size() - tocOffset;
    const ContentHash tocHash =
        core::hashBytes(std::span<const std::byte>(out.data() + tocOffset, static_cast<usize>(tocLength)));
    const std::array<std::byte, 16> tocHashBytes = core::toBytes(tocHash);

    std::byte* const header = out.data();
    for (usize i = 0; i < 8; ++i) {
        header[16 + i] = static_cast<std::byte>((tocOffset >> (i * 8)) & 0xFFu);
        header[24 + i] = static_cast<std::byte>((tocLength >> (i * 8)) & 0xFFu);
    }
    std::memcpy(header + 32, tocHashBytes.data(), tocHashBytes.size());
    return out;
}

std::optional<core::EngineError> Pack::open(std::vector<std::byte> bytes, Pack& out)
{
    out = Pack{};
    std::vector<PackEntry> entries;
    if (auto error = validate(bytes, false, entries))
        return error;
    out.m_bytes = std::move(bytes);
    out.m_entries = std::move(entries);
    out.m_inflated = std::make_shared<Inflated>();
    return std::nullopt;
}

std::optional<core::EngineError> Pack::openVerified(std::vector<std::byte> bytes, Pack& out)
{
    out = Pack{};
    std::vector<PackEntry> entries;
    if (auto error = validate(bytes, true, entries))
        return error;
    out.m_bytes = std::move(bytes);
    out.m_entries = std::move(entries);
    out.m_inflated = std::make_shared<Inflated>();
    return std::nullopt;
}

std::optional<core::EngineError> Pack::openMapped(std::shared_ptr<platform::MappedFile> file, bool verify, Pack& out)
{
    out = Pack{};
    if (file == nullptr)
        return core::makeError(ENG_TR("asset.pack.err.truncated"));
    std::vector<PackEntry> entries;
    if (auto error = validate(file->bytes(), verify, entries))
        return error;
    out.m_mapped = std::move(file);
    out.m_entries = std::move(entries);
    out.m_inflated = std::make_shared<Inflated>();
    return std::nullopt;
}

std::span<const std::byte> Pack::storage() const noexcept
{
    if (m_mapped != nullptr)
        return m_mapped->bytes();
    return m_bytes;
}

std::optional<core::EngineError> Pack::validate(std::span<const std::byte> bytes, bool verify,
                                                std::vector<PackEntry>& out)
{
    const u64 fileSize = bytes.size();
    if (fileSize < PackHeaderBytes) {
        return core::makeError(ENG_TR("asset.pack.err.truncated"));
    }
    if (std::memcmp(bytes.data(), PackMagic, 4) != 0) {
        return core::makeError(ENG_TR("asset.pack.err.magic"));
    }

    const u32 version = readU32(bytes.data() + 4);
    if (version != PackFormatPlain && version != PackFormatVersion) {
        const I18nArg args[] = {{"found", std::to_string(version)}, {"expected", std::to_string(PackFormatVersion)}};
        return core::makeError(ENG_TR("asset.pack.err.version"), args);
    }

    const u32 flags = readU32(bytes.data() + 8);
    if (flags != 0) {
        return core::makeError(ENG_TR("asset.pack.err.header"));
    }

    const u32 entryCount = readU32(bytes.data() + 12);
    const u64 tocOffset = readU64(bytes.data() + 16);
    const u64 tocLength = readU64(bytes.data() + 24);
    const ContentHash tocHash = core::fromBytes(std::span<const std::byte, 16>(bytes.data() + 32, 16));

    // Computed in u64 from a u32 count, so the multiply cannot wrap -- the
    // classic way a header claims a table that overlaps its own file.
    if (tocLength != static_cast<u64>(entryCount) * PackEntryBytes) {
        return core::makeError(ENG_TR("asset.pack.err.header"));
    }
    if (tocOffset < PackHeaderBytes || tocOffset > fileSize || tocLength > fileSize - tocOffset) {
        return core::makeError(ENG_TR("asset.pack.err.truncated"));
    }

    // Before a single offset is read out of it. The TOC is the one region of
    // the file with neither its own name nor a consistency relation to check it
    // against, and a flipped bit in an entry is a pack that answers a lookup
    // with somebody else's bytes.
    if (core::hashBytes(std::span<const std::byte>(bytes.data() + tocOffset, static_cast<usize>(tocLength))) !=
        tocHash) {
        return core::makeError(ENG_TR("asset.pack.err.toc_hash"));
    }

    std::vector<PackEntry> entries;
    entries.reserve(entryCount);
    for (u32 i = 0; i < entryCount; ++i) {
        const std::byte* const record = bytes.data() + tocOffset + static_cast<u64>(i) * PackEntryBytes;

        PackEntry entry;
        entry.hash = core::fromBytes(std::span<const std::byte, 16>(record, 16));
        entry.offset = readU64(record + 16);
        entry.storedSize = readU64(record + 24);
        entry.originalSize = readU64(record + 32);
        const u32 kind = readU32(record + 40);
        const u32 codec = readU32(record + 44);

        if (!knownKind(kind, version) || !knownCodec(codec, version)) {
            return core::makeError(ENG_TR("asset.pack.err.entry"));
        }
        // Blobs live between the header and the TOC. Checked as a subtraction
        // against a bound rather than as `offset + size <= tocOffset`, which is
        // the same statement with an overflow in it.
        if (entry.offset < PackHeaderBytes || entry.offset > tocOffset || entry.storedSize > tocOffset - entry.offset) {
            return core::makeError(ENG_TR("asset.pack.err.entry"));
        }
        // An entry stored plain is as long as it is; a deflated one says how
        // long it becomes, within what this build will allocate for one.
        if (codec == static_cast<u32>(BlobCodec::None) ? entry.originalSize != entry.storedSize
                                                       : entry.originalSize == 0 || entry.originalSize > MostInflated) {
            return core::makeError(ENG_TR("asset.pack.err.entry"));
        }

        entry.kind = static_cast<AssetKind>(kind);
        entry.codec = static_cast<BlobCodec>(codec);

        // Strictly ascending, which validates the sort AND rejects duplicates
        // in one comparison -- two entries under one name is a pack with two
        // answers to the same question.
        if (i > 0 && !(entries.back().hash < entry.hash)) {
            return core::makeError(ENG_TR("asset.pack.err.order"));
        }
        entries.push_back(entry);
    }

    // One `Names` at most: two would be two answers to what the pack holds.
    usize tables = 0;
    for (const PackEntry& entry : entries)
        tables += entry.kind == AssetKind::Names ? 1u : 0u;
    if (tables > 1) {
        return core::makeError(ENG_TR("asset.pack.err.entry"));
    }

    if (verify) {
        if (auto error = verifyEntries(bytes, entries))
            return error;
    }

    out = std::move(entries);
    return std::nullopt;
}

std::optional<core::EngineError> Pack::verifyEntries(std::span<const std::byte> bytes,
                                                     std::span<const PackEntry> entries)
{
    std::vector<std::byte> scratch;
    for (const PackEntry& entry : entries) {
        const std::span<const std::byte> stored(bytes.data() + entry.offset, static_cast<usize>(entry.storedSize));
        std::span<const std::byte> blob = stored;
        if (entry.codec == BlobCodec::Deflate) {
            if (!inflate(stored, entry.originalSize, scratch)) {
                const I18nArg args[] = {{"hash", entry.hash.toHex()}};
                return core::makeError(ENG_TR("asset.pack.err.hash_mismatch"), args);
            }
            blob = scratch;
        }
        if (core::hashBytes(blob) != entry.hash) {
            const I18nArg args[] = {{"hash", entry.hash.toHex()}};
            return core::makeError(ENG_TR("asset.pack.err.hash_mismatch"), args);
        }
    }
    return std::nullopt;
}

std::optional<core::EngineError> Pack::verify() const
{
    return verifyEntries(storage(), m_entries);
}

bool Pack::damaged() const noexcept
{
    if (m_inflated == nullptr)
        return false;
    const std::lock_guard lock(m_inflated->guard);
    return m_inflated->damaged;
}

const PackEntry* Pack::names() const noexcept
{
    for (const PackEntry& entry : m_entries) {
        if (entry.kind == AssetKind::Names)
            return &entry;
    }
    return nullptr;
}

const PackEntry* Pack::find(const ContentHash& hash) const noexcept
{
    const auto at = std::lower_bound(m_entries.begin(), m_entries.end(), hash,
                                     [](const PackEntry& entry, const ContentHash& key) { return entry.hash < key; });
    if (at == m_entries.end() || at->hash != hash) {
        return nullptr;
    }
    return &*at;
}

std::span<const std::byte> Pack::blob(const ContentHash& hash) const noexcept
{
    const PackEntry* const entry = find(hash);
    if (entry == nullptr) {
        return {};
    }
    const std::span<const std::byte> stored =
        storage().subspan(static_cast<usize>(entry->offset), static_cast<usize>(entry->storedSize));
    if (entry->codec == BlobCodec::None)
        return stored;
    if (m_inflated == nullptr)
        return {};
    // Inflated once, under the lock, and never moved after: an element of an
    // unordered map stays where it is whatever is added beside it.
    const std::lock_guard lock(m_inflated->guard);
    const auto kept = m_inflated->blobs.find(hash);
    if (kept != m_inflated->blobs.end())
        return kept->second;
    std::vector<std::byte> whole;
    if (!inflate(stored, entry->originalSize, whole) || core::hashBytes(whole) != hash) {
        whole.clear();
        m_inflated->damaged = true;
    }
    return m_inflated->blobs.emplace(hash, std::move(whole)).first->second;
}

std::vector<std::byte> encodePackNames(std::vector<PackName> names)
{
    std::sort(names.begin(), names.end(), [](const PackName& a, const PackName& b) { return a.name < b.name; });
    names.erase(
        std::unique(names.begin(), names.end(), [](const PackName& a, const PackName& b) { return a.name == b.name; }),
        names.end());
    std::vector<std::byte> out;
    out.reserve(NamesHeaderBytes + names.size() * NameRowBytes);
    out.insert(out.end(), reinterpret_cast<const std::byte*>(NamesMagic),
               reinterpret_cast<const std::byte*>(NamesMagic) + 4);
    writeU32(out, NamesVersion);
    writeU32(out, static_cast<u32>(names.size()));
    for (const PackName& row : names) {
        const std::array<std::byte, 16> name = core::toBytes(row.name);
        const std::array<std::byte, 16> content = core::toBytes(row.content);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), content.begin(), content.end());
        writeU32(out, static_cast<u32>(row.kind));
        writeU32(out, 0);
    }
    return out;
}

bool decodePackNames(std::span<const std::byte> bytes, std::vector<PackName>& out)
{
    out.clear();
    if (bytes.size() < NamesHeaderBytes || std::memcmp(bytes.data(), NamesMagic, 4) != 0 ||
        readU32(bytes.data() + 4) != NamesVersion)
        return false;
    const u32 count = readU32(bytes.data() + 8);
    if (bytes.size() - NamesHeaderBytes != static_cast<u64>(count) * NameRowBytes)
        return false;
    out.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        const std::byte* const row = bytes.data() + NamesHeaderBytes + static_cast<usize>(index) * NameRowBytes;
        PackName name;
        name.name = core::fromBytes(std::span<const std::byte, 16>(row, 16));
        name.content = core::fromBytes(std::span<const std::byte, 16>(row + 16, 16));
        const u32 kind = readU32(row + 32);
        if (!knownKind(kind, PackFormatVersion) || (index > 0 && !(out.back().name < name.name))) {
            out.clear();
            return false;
        }
        name.kind = static_cast<AssetKind>(kind);
        out.push_back(name);
    }
    return true;
}

const PackName* findPackName(std::span<const PackName> names, std::string_view name) noexcept
{
    const ContentHash key = core::hashText(name);
    const auto at = std::lower_bound(names.begin(), names.end(), key,
                                     [](const PackName& row, const ContentHash& wanted) { return row.name < wanted; });
    return at != names.end() && at->name == key ? &*at : nullptr;
}

std::optional<core::EngineError> openPackFile(const std::filesystem::path& path, Pack& out, bool verify)
{
    // Mapped where the system can map it; read whole where it cannot -- an
    // APK entry, which is no file at all.
    if (auto mapped = std::make_shared<platform::MappedFile>(); mapped->open(path))
        return Pack::openMapped(std::move(mapped), verify, out);
    std::vector<std::byte> bytes;
    if (!platform::readFile(path, bytes)) {
        const I18nArg args[] = {{"content", path.string()}};
        return core::makeError(ENG_TR("asset.pack.err.open_failed"), args);
    }
    return verify ? Pack::openVerified(std::move(bytes), out) : Pack::open(std::move(bytes), out);
}

} // namespace engine::asset
