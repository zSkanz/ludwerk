#include <doctest/doctest.h>

#include "engine/core/text_key.h"

using engine::core::hashTextKey;
using engine::core::TextKey;

TEST_CASE("text key hashing")
{
    SUBCASE("is a compile-time constant")
    {
        // The whole point of ENG_TR is that a key costs nothing at runtime.
        static_assert(ENG_TR("engine.boot.hello").hash != 0);
        constexpr TextKey key = ENG_TR("engine.boot.hello");
        CHECK(key.hash == hashTextKey("engine.boot.hello"));
    }

    SUBCASE("matches FNV-1a reference values")
    {
        // Pinned so the hash cannot drift between the engine and the tools
        // that generate the key inventory. These are the published FNV-1a
        // 32-bit results for the given inputs.
        CHECK(hashTextKey("") == 0x811C9DC5u);
        CHECK(hashTextKey("a") == 0xE40C292Cu);
        CHECK(hashTextKey("foobar") == 0xBF9CF968u);
    }

    SUBCASE("distinguishes distinct keys")
    {
        CHECK(hashTextKey("scene.err.parent_cycle") != hashTextKey("scene.err.invalid_size"));
        CHECK(ENG_TR("a.b") == ENG_TR("a.b"));
        CHECK_FALSE(ENG_TR("a.b") == ENG_TR("a.c"));
    }
}
