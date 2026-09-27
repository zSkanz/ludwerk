// The VERSIONINFO a Windows export is stamped with, built and read back on
// every tier: a padding mistake here is an empty Details tab in Explorer and
// nothing else, so the writer and the reader are held to each other.
#include <doctest/doctest.h>
#include <engine/iconpatch/version.h>

using engine::iconpatch::buildVersionInfo;
using engine::iconpatch::parseVersionInfo;
using engine::iconpatch::parseVersionNumbers;
using engine::iconpatch::VersionInfo;

TEST_CASE("a version is two to four numbers of at most 65535")
{
    std::uint16_t parts[4];
    REQUIRE(parseVersionNumbers("1.2.3", parts));
    CHECK(parts[0] == 1);
    CHECK(parts[1] == 2);
    CHECK(parts[2] == 3);
    CHECK(parts[3] == 0);
    CHECK(parseVersionNumbers("10.0", parts));
    CHECK(parseVersionNumbers("1.2.3.4", parts));
    CHECK_FALSE(parseVersionNumbers("1", parts));
    CHECK_FALSE(parseVersionNumbers("1.2.3.4.5", parts));
    CHECK_FALSE(parseVersionNumbers("1.x", parts));
    CHECK_FALSE(parseVersionNumbers("1.70000", parts));
    CHECK_FALSE(parseVersionNumbers("", parts));
}

TEST_CASE("the file properties come back out as they went in")
{
    VersionInfo info;
    info.version = "1.2.0";
    info.strings["ProductName"] = "Sky Hopper";
    info.strings["CompanyName"] = "Skanz";
    info.strings["FileDescription"] = "Sky Hopper \xE2\x80\x94 a game, caf\xC3\xA9 \xF0\x9F\x8E\xAE";
    info.strings["ProductVersion"] = "1.2.0";
    info.strings["FileVersion"] = "1.2.0";

    const std::vector<std::uint8_t> bytes = buildVersionInfo(info);
    REQUIRE_FALSE(bytes.empty());
    // The root's own length is the whole resource.
    CHECK((static_cast<std::size_t>(bytes[0]) | (static_cast<std::size_t>(bytes[1]) << 8)) == bytes.size());

    VersionInfo read;
    REQUIRE(parseVersionInfo(bytes, read));
    CHECK(read.version == "1.2.0.0");
    CHECK(read.strings == info.strings);
}

TEST_CASE("an unparseable version writes nothing, and garbage reads as nothing")
{
    VersionInfo info;
    info.version = "one";
    CHECK(buildVersionInfo(info).empty());

    const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8};
    VersionInfo read;
    CHECK_FALSE(parseVersionInfo(garbage, read));
}
