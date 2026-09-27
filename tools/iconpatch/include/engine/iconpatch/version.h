#pragma once

// A Windows VERSIONINFO resource: what Explorer's Details tab shows for an
// executable -- the product, the company, the version, the description (ADR
// 0104 §7).
//
// **Built and read as bytes, here, rather than compiled from an `.rc`**: the
// player is one prebuilt binary that every game copies, so its file properties
// are stamped onto the copy the way its icon is. The layout is a tree of
// `{length, value length, type, UTF-16 key, value, children}` blocks, each on a
// 32-bit boundary, and being wrong about the padding is silent -- Explorer
// shows an empty tab -- so the reader lives beside the writer and a test holds
// them to each other on every tier.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace engine::iconpatch {

struct VersionInfo
{
    // `X.Y.Z` or `X.Y.Z.W`; the fixed part carries the four numbers.
    std::string version;
    // Keyed by the names Windows defines: ProductName, CompanyName,
    // FileDescription, FileVersion, ProductVersion, OriginalFilename.
    std::map<std::string, std::string> strings;
};

// Up to four dot-separated numbers of at most 65535 each, or false.
[[nodiscard]] bool parseVersionNumbers(const std::string& text, std::uint16_t (&parts)[4]);

// The resource bytes. Empty when the version does not parse.
[[nodiscard]] std::vector<std::uint8_t> buildVersionInfo(const VersionInfo& info);

// The strings and the fixed version back out of resource bytes. False on
// anything that is not a VS_VERSIONINFO.
[[nodiscard]] bool parseVersionInfo(std::span<const std::uint8_t> data, VersionInfo& out);

} // namespace engine::iconpatch
