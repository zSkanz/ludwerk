// `CryptoService` as a script reaches it (ADR 0151). What the hashes ARE is
// `engine/core`'s test, against their standards' vectors; this one is that a
// script gets them, and that a password is hashed without the tick waiting.
#include <chrono>
#include <doctest/doctest.h>
#include <string_view>
#include <thread>

#include "script_fixture.h"

using namespace engine;

namespace {

// Ticks until the log says `marker`, for as long as a password takes on a
// machine with every core busy -- and no longer than ten seconds.
[[nodiscard]] bool tickUntil(script::testing::Fixture& fixture, std::string_view marker)
{
    for (int i = 0; i < 2000; ++i) {
        fixture.tick(1);
        if (fixture.logContains(marker))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("D442: a script has chance that is not the simulation's, an id, and two hashes")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    CHECK(fixture.failure(R"(
        local crypto = game:GetService("CryptoService")

        -- The standards' own vectors, through the binding: a string and a
        -- buffer of the same bytes are the same thing to hash.
        local abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        assert(crypto:Sha256("abc") == abc)
        assert(crypto:Sha256(buffer.fromstring("abc")) == abc)
        assert(crypto:HmacSha256("Jefe", "what do ya want for nothing?")
            == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

        local a, b = crypto:RandomBytes(32), crypto:RandomBytes(32)
        assert(buffer.len(a) == 32 and buffer.tostring(a) ~= buffer.tostring(b))
        assert(buffer.len(crypto:RandomBytes(0)) == 0)
        assert(not pcall(function() return crypto:RandomBytes(-1) end))
        assert(not pcall(function() return crypto:RandomBytes(1.5) end))
        assert(not pcall(function() return crypto:RandomBytes(2e9) end))

        local id = crypto:UniqueId()
        assert(#id == 36 and string.sub(id, 15, 15) == "4" and id ~= crypto:UniqueId())

        -- In its range, both ends reached, whole.
        local seen = {}
        for _ = 1, 400 do
            local roll = crypto:RandomInteger(1, 6)
            assert(roll >= 1 and roll <= 6 and roll == math.floor(roll))
            seen[roll] = true
        end
        for face = 1, 6 do
            assert(seen[face], "a die never rolled " .. face)
        end
        assert(crypto:RandomInteger(7, 7) == 7)
        assert(crypto:RandomInteger(-3, -3) == -3)
        assert(not pcall(function() return crypto:RandomInteger(2, 1) end))
        assert(not pcall(function() return crypto:RandomInteger(0.5, 3) end))

        assert(crypto:SecureEquals("token", "token"))
        assert(not crypto:SecureEquals("token", "tokeN"))
        assert(crypto:SecureEquals("abc", buffer.fromstring("abc")))
        print("crypto-done")
    )") == "");
    fixture.tick(1);
    CHECK(fixture.errors() == "");
    CHECK(fixture.logContains("crypto-done"));
}

TEST_CASE("D442: a password is hashed on a thread of its own, and the tick does not wait for it")
{
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    CHECK(fixture.failure(R"(
        local crypto = game:GetService("CryptoService")
        task.spawn(function()
            local hash = crypto:HashPasswordAsync("hunter2")
            assert(string.sub(hash, 1, 10) == "$argon2id$", hash)
            assert(hash ~= crypto:HashPasswordAsync("hunter2"), "the salt is the hash's own")
            assert(crypto:VerifyPasswordAsync("hunter2", hash))
            assert(not crypto:VerifyPasswordAsync("hunter3", hash))
            assert(not crypto:VerifyPasswordAsync("hunter2", "not a hash"))
            assert(not pcall(function() return crypto:HashPasswordAsync(string.rep("x", 1025)) end))
            print("passwords-done")
        end)
        -- The call above parked: this line runs before any hash exists.
        print("asked")
    )") == "");
    CHECK(fixture.logContains("asked"));
    CHECK_FALSE(fixture.logContains("passwords-done"));
    CHECK(tickUntil(fixture, "passwords-done"));
    CHECK(fixture.errors() == "");
}

TEST_CASE("D442: a VM that ends with passwords still queued ends")
{
    // The worker finishes the one it is in and drops the rest; nothing waits
    // on a thread whose VM is gone.
    script::testing::Fixture fixture;
    REQUIRE(fixture.booted);
    CHECK(fixture.failure(R"(
        local crypto = game:GetService("CryptoService")
        for _ = 1, 8 do
            task.spawn(function()
                crypto:HashPasswordAsync("left behind")
            end)
        end
    )") == "");
    fixture.tick(1);
}
