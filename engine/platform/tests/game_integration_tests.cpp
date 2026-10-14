#include <doctest/doctest.h>

#include "engine/platform/game_integration.h"

TEST_CASE("optional provider loading rejects relative paths and missing libraries")
{
    CHECK_FALSE(engine::platform::loadGameIntegration("engine_test_provider.dll", ""));
    CHECK_FALSE(engine::platform::loadGameIntegration(ENG_TEST_PROVIDER_PATH ".missing", ""));
}
TEST_CASE("a separately built provider preserves ABI ownership and explicit async failures")
{
    auto provider = engine::platform::loadGameIntegration(ENG_TEST_PROVIDER_PATH, "");
    REQUIRE(provider != nullptr);
    CHECK(provider->available());
    CHECK_FALSE(provider->supports("Identity")); // ABI v1 library has no capability extension.
    CHECK(provider->response().empty());
    CHECK_FALSE(provider->signedIn());
    CHECK(provider->user().id.empty());
    CHECK(provider->begin("SignIn", "").empty());
    CHECK(provider->begin("SignIn", "") == "Busy");
    bool success = true;
    std::string reason;
    REQUIRE(provider->poll(success, reason));
    CHECK_FALSE(success);
    CHECK(reason == "NotSignedIn");
    CHECK_FALSE(provider->poll(success, reason));
}
