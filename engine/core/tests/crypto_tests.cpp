// What a game keeps a secret with (ADR 0151). The hashes are checked against
// the vectors their own standards publish: a wrapper that hashed the wrong
// bytes would agree with itself for ever.
#include <doctest/doctest.h>
#include <set>
#include <string>
#include <string_view>

#include "engine/core/crypto.h"

using namespace engine;

namespace {

[[nodiscard]] std::span<const core::u8> bytes(std::string_view text)
{
    return {reinterpret_cast<const core::u8*>(text.data()), text.size()};
}

} // namespace

TEST_CASE("D442: SHA-256 is the standard's, by the standard's own vectors")
{
    // FIPS 180-4's examples, and the empty message.
    CHECK(core::toHex(core::sha256(bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(core::toHex(core::sha256(bytes("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(core::toHex(core::sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("D442: HMAC-SHA-256 is RFC 4231's, a short key and a long one")
{
    // Test case 2: a key shorter than the block.
    CHECK(core::toHex(core::hmacSha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // Test case 6: a key longer than the block, which is hashed first.
    const std::string longKey(131, '\xaa');
    CHECK(core::toHex(
              core::hmacSha256(bytes(longKey), bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("D442: chance is not the same twice, and an id is a version-4 UUID")
{
    // The defect was `Random.new()` giving 713614 on every launch, by design,
    // and nothing else to ask: a session token that every server hands out in
    // the same order.
    std::array<core::u8, 32> first{};
    std::array<core::u8, 32> second{};
    core::secureRandom(first);
    core::secureRandom(second);
    CHECK(first != second);
    CHECK(first != std::array<core::u8, 32>{});

    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const std::string id = core::uniqueId();
        REQUIRE(id.size() == 36);
        CHECK(id[8] == '-');
        CHECK(id[13] == '-');
        CHECK(id[18] == '-');
        CHECK(id[23] == '-');
        CHECK(id[14] == '4');
        CHECK((id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b'));
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("D442: a comparison of secrets tells equal from not, and lengths apart")
{
    CHECK(core::secureEquals(bytes("token"), bytes("token")));
    CHECK_FALSE(core::secureEquals(bytes("token"), bytes("tokeN")));
    CHECK_FALSE(core::secureEquals(bytes("token"), bytes("toke")));
    CHECK(core::secureEquals({}, {}));
}

TEST_CASE("D442: a password's hash verifies the password, and nothing else")
{
    const std::string hash = core::hashPassword("correct horse battery staple");
    REQUIRE_FALSE(hash.empty());
    // It says what it is, so the cost can change without a stored hash dying.
    CHECK(hash.starts_with("$argon2id$"));
    CHECK(core::verifyPassword(hash, "correct horse battery staple"));
    CHECK_FALSE(core::verifyPassword(hash, "correct horse battery stapl"));
    CHECK_FALSE(core::verifyPassword(hash, ""));

    // The same password twice is two hashes: the salt is its own.
    const std::string again = core::hashPassword("correct horse battery staple");
    CHECK(again != hash);
    CHECK(core::verifyPassword(again, "correct horse battery staple"));

    // What is not one of ours is not a match, whatever it is.
    CHECK_FALSE(core::verifyPassword("", "x"));
    CHECK_FALSE(core::verifyPassword("5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8", "password"));
    CHECK_FALSE(core::verifyPassword(std::string(4096, '$'), "x"));
}
