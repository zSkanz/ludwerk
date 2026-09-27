#include <charconv>
#include <engine/iconpatch/version.h>

namespace engine::iconpatch {
namespace {

constexpr std::uint32_t kFixedSignature = 0xFEEF04BDu;
constexpr std::uint16_t kTypeBinary = 0;
constexpr std::uint16_t kTypeText = 1;
// US English, Unicode: the one string table this writes, and the translation
// the reader of it is told to look up.
constexpr const char* kStringTable = "040904B0";
constexpr std::uint32_t kTranslation = 0x04B00409u;

void put16(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

void pad(std::vector<std::uint8_t>& out)
{
    while (out.size() % 4 != 0)
        out.push_back(0);
}

// UTF-8 to UTF-16LE, NUL-terminated. Four-byte sequences become surrogate
// pairs; anything malformed becomes U+FFFD rather than a truncated name.
void putWide(std::vector<std::uint8_t>& out, const std::string& text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = 0xFFFD;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        }
        else if ((lead >> 5) == 0x6 && index + 1 < text.size()) {
            code = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3Fu);
            length = 2;
        }
        else if ((lead >> 4) == 0xE && index + 2 < text.size()) {
            code = ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[index + 2]) & 0x3Fu);
            length = 3;
        }
        else if ((lead >> 3) == 0x1E && index + 3 < text.size()) {
            code = ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 12) |
                   ((static_cast<unsigned char>(text[index + 2]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[index + 3]) & 0x3Fu);
            length = 4;
        }
        index += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            put16(out, 0xD800 | (code >> 10));
            put16(out, 0xDC00 | (code & 0x3FF));
        }
        else {
            put16(out, code);
        }
    }
    put16(out, 0);
}

// One block: its header, key and value, then each child on a 32-bit boundary,
// with the length written last. `valueLength` is in bytes for a binary value
// and in UTF-16 units, NUL included, for text -- as the format has it.
[[nodiscard]] std::vector<std::uint8_t> block(const std::string& key, std::uint16_t type,
                                              const std::vector<std::uint8_t>& value, std::uint16_t valueLength,
                                              const std::vector<std::vector<std::uint8_t>>& children)
{
    std::vector<std::uint8_t> out;
    put16(out, 0);
    put16(out, valueLength);
    put16(out, type);
    putWide(out, key);
    pad(out);
    out.insert(out.end(), value.begin(), value.end());
    for (const std::vector<std::uint8_t>& child : children) {
        pad(out);
        out.insert(out.end(), child.begin(), child.end());
    }
    const std::size_t length = out.size();
    out[0] = static_cast<std::uint8_t>(length & 0xFF);
    out[1] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
    return out;
}

[[nodiscard]] std::uint32_t read16(std::span<const std::uint8_t> data, std::size_t at)
{
    return at + 2 <= data.size()
               ? static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8)
               : 0;
}

[[nodiscard]] std::uint32_t read32(std::span<const std::uint8_t> data, std::size_t at)
{
    return read16(data, at) | (read16(data, at + 2) << 16);
}

// UTF-16LE at `at`, up to its NUL, as UTF-8. `at` moves past the NUL.
[[nodiscard]] std::string readWide(std::span<const std::uint8_t> data, std::size_t& at, std::size_t end)
{
    std::string out;
    while (at + 2 <= end) {
        std::uint32_t code = read16(data, at);
        at += 2;
        if (code == 0)
            break;
        if (code >= 0xD800 && code < 0xDC00 && at + 2 <= end) {
            const std::uint32_t low = read16(data, at);
            at += 2;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

[[nodiscard]] std::size_t aligned(std::size_t at)
{
    return (at + 3) & ~static_cast<std::size_t>(3);
}

struct Header
{
    std::size_t end = 0;
    std::uint32_t valueLength = 0;
    std::uint32_t type = 0;
    std::string key;
    // Where the value starts, on its boundary.
    std::size_t value = 0;
};

[[nodiscard]] bool readHeader(std::span<const std::uint8_t> data, std::size_t at, std::size_t limit, Header& out)
{
    const std::uint32_t length = read16(data, at);
    if (length < 6 || at + length > limit)
        return false;
    out.end = at + length;
    out.valueLength = read16(data, at + 2);
    out.type = read16(data, at + 4);
    std::size_t cursor = at + 6;
    out.key = readWide(data, cursor, out.end);
    out.value = aligned(cursor);
    return out.value <= out.end;
}

} // namespace

bool parseVersionNumbers(const std::string& text, std::uint16_t (&parts)[4])
{
    parts[0] = parts[1] = parts[2] = parts[3] = 0;
    const char* cursor = text.data();
    const char* end = text.data() + text.size();
    for (int index = 0; index < 4; ++index) {
        unsigned int value = 0;
        const auto [next, error] = std::from_chars(cursor, end, value);
        if (error != std::errc{} || value > 0xFFFF)
            return false;
        parts[index] = static_cast<std::uint16_t>(value);
        cursor = next;
        if (cursor == end)
            return index >= 1;
        if (*cursor != '.')
            return false;
        ++cursor;
    }
    return false;
}

std::vector<std::uint8_t> buildVersionInfo(const VersionInfo& info)
{
    std::uint16_t parts[4];
    if (!parseVersionNumbers(info.version, parts))
        return {};

    std::vector<std::uint8_t> fixed;
    put32(fixed, kFixedSignature);
    put32(fixed, 0x00010000u); // structure version 1.0
    for (int twice = 0; twice < 2; ++twice) {
        // The file version, then the product version: the same four numbers.
        put32(fixed, (static_cast<std::uint32_t>(parts[0]) << 16) | parts[1]);
        put32(fixed, (static_cast<std::uint32_t>(parts[2]) << 16) | parts[3]);
    }
    put32(fixed, 0x3Fu);       // flags mask
    put32(fixed, 0);           // flags
    put32(fixed, 0x00040004u); // VOS_NT_WINDOWS32
    put32(fixed, 0x1u);        // VFT_APP
    put32(fixed, 0);           // subtype
    put32(fixed, 0);           // date, high
    put32(fixed, 0);           // date, low

    std::vector<std::vector<std::uint8_t>> strings;
    for (const auto& [name, value] : info.strings) {
        std::vector<std::uint8_t> wide;
        putWide(wide, value);
        strings.push_back(block(name, kTypeText, wide, static_cast<std::uint16_t>(wide.size() / 2), {}));
    }
    const std::vector<std::uint8_t> table = block(kStringTable, kTypeText, {}, 0, strings);
    const std::vector<std::uint8_t> stringFileInfo = block("StringFileInfo", kTypeText, {}, 0, {table});

    std::vector<std::uint8_t> translation;
    put32(translation, kTranslation);
    const std::vector<std::uint8_t> var = block("Translation", kTypeBinary, translation, 4, {});
    const std::vector<std::uint8_t> varFileInfo = block("VarFileInfo", kTypeText, {}, 0, {var});

    return block("VS_VERSION_INFO", kTypeBinary, fixed, static_cast<std::uint16_t>(fixed.size()),
                 {stringFileInfo, varFileInfo});
}

bool parseVersionInfo(std::span<const std::uint8_t> data, VersionInfo& out)
{
    Header root;
    if (!readHeader(data, 0, data.size(), root) || root.key != "VS_VERSION_INFO" || root.valueLength < 52)
        return false;
    if (read32(data, root.value) != kFixedSignature)
        return false;
    const std::uint32_t high = read32(data, root.value + 8);
    const std::uint32_t low = read32(data, root.value + 12);
    out.version = std::to_string(high >> 16) + "." + std::to_string(high & 0xFFFF) + "." + std::to_string(low >> 16) +
                  "." + std::to_string(low & 0xFFFF);

    for (std::size_t child = aligned(root.value + root.valueLength); child < root.end;) {
        Header info;
        if (!readHeader(data, child, root.end, info))
            return false;
        if (info.key == "StringFileInfo") {
            for (std::size_t tableAt = info.value; tableAt < info.end;) {
                Header table;
                if (!readHeader(data, tableAt, info.end, table))
                    return false;
                for (std::size_t stringAt = table.value; stringAt < table.end;) {
                    Header entry;
                    if (!readHeader(data, stringAt, table.end, entry))
                        return false;
                    std::size_t valueAt = entry.value;
                    out.strings[entry.key] =
                        entry.valueLength == 0 ? std::string{} : readWide(data, valueAt, entry.end);
                    stringAt = aligned(entry.end);
                }
                tableAt = aligned(table.end);
            }
        }
        child = aligned(info.end);
    }
    return true;
}

} // namespace engine::iconpatch
