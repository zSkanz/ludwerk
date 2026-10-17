// The keys a machine plays by (ADR 0224): its own, kept and held by one copy
// of the game at a time, and the hosts it has met, pinned by where it met them.
#include <doctest/doctest.h>
#include <filesystem>
#include <string>

#include "engine/app/match_keys.h"
#include "engine/core/crypto.h"
#include "engine/platform/file.h"
#include "project_fixture.h"

using namespace engine;
using namespace engine::app;
using engine::app::testing::Captured;
using engine::app::testing::Project;

TEST_CASE("a machine's match key is kept, and a second copy of the game running beside it plays with its own")
{
    Captured log;
    Project project;
    const std::filesystem::path file = project.root / "keys" / "saves.key";

    core::X25519Pair first;
    {
        platform::FileLock held;
        first = loadOrMakeMatchKey(file, held);
        CHECK(held.held());
        CHECK(std::filesystem::exists(file));
        // Read again by the same copy, after it let go: the same player.
        held.unlock();
        platform::FileLock again;
        CHECK(loadOrMakeMatchKey(file, again).pub == first.pub);

        // **A second copy while the first holds it**: a key of its own for
        // the run, said in the log -- given the first one's, the server took
        // it for the first player reconnecting and let the first one go.
        platform::FileLock second;
        const core::X25519Pair other = loadOrMakeMatchKey(file, second);
        CHECK_FALSE(second.held());
        CHECK(other.pub != first.pub);
        CHECK(log.contains("another player"));
    }
    // Every copy gone: the kept key is the machine's again.
    platform::FileLock later;
    CHECK(loadOrMakeMatchKey(file, later).pub == first.pub);
    // The secret is the file's, and makes the same public half.
    CHECK(core::x25519Public(loadOrMakeMatchKey(file, later).secret) == first.pub);
}

TEST_CASE("a host is pinned the first time, refused when its key changes, and taken again once forgotten")
{
    Project project;
    const std::filesystem::path file = project.root / "keys" / "saves.hosts.json";
    const std::array<core::u8, 32> met = core::x25519Generate().pub;
    const std::array<core::u8, 32> stranger = core::x25519Generate().pub;
    {
        KeyPins pins(file);
        CHECK(pins.trust("play.example.com:7777", met));
        CHECK(pins.trust("play.example.com:7777", met));
        CHECK_FALSE(pins.trust("play.example.com:7777", stranger));
        // Another address is another host.
        CHECK(pins.trust("play.example.com:7778", stranger));
    }
    // Kept across runs.
    KeyPins again(file);
    CHECK_FALSE(again.trust("play.example.com:7777", stranger));
    again.forget("play.example.com:7777");
    CHECK(again.trust("play.example.com:7777", stranger));
    KeyPins after(file);
    CHECK(after.trust("play.example.com:7777", stranger));
    CHECK_FALSE(after.trust("play.example.com:7777", met));
}

TEST_CASE("a key written in hexadecimal is read as a host's log printed it, and anything else is no key")
{
    const core::X25519Pair pair = core::x25519Generate();
    CHECK(matchKeyFromHex(core::toHex(pair.pub)) == pair.pub);
    CHECK(matchKeyFromHex(core::toHex(pair.pub) + "\n") == pair.pub);
    CHECK_FALSE(matchKeyFromHex("abc").has_value());
    CHECK_FALSE(matchKeyFromHex(std::string(64, 'z')).has_value());
}
