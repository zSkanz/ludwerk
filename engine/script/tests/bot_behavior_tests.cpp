// A load test's bots, as a game writes them (ADR 0236 section 3): a behavior
// in its own sandboxed VM, a thread a bot, stepped by the bot process's tick.

#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

#include "engine/script/bot_behavior.h"

using namespace engine;

namespace {

class OnePortal final : public script::BotWorldView
{
public:
    void tagged(std::string_view tag, std::vector<core::DVec3>& out) const override
    {
        if (tag == "Portal")
            out.push_back(core::DVec3{7.0, 0.0, 0.0});
    }
};

[[nodiscard]] script::BotSight at(double x)
{
    script::BotSight sight;
    sight.hasBody = true;
    sight.position = core::DVec3{x, 0.0, 0.0};
    return sight;
}

} // namespace

TEST_CASE("a bot's behavior walks, waits, reads the world and ends")
{
    const std::string source = R"(
local Bot = require("@engine/bot")
assert(Bot.Index == 2)
assert(Bot:GetPosition() == Bot.Spawn)
assert(Bot:WalkTo(Bot.Spawn + vector.create(10, 0, 0), 5))
Bot:Wait(1)
local found = Bot:FindTagged("Portal")
assert(#found == 1 and found[1].x == 7)
assert(not Bot:WalkTo(vector.create(100, 0, 0), 1))
)";
    std::string error;
    std::unique_ptr<script::BotBehaviors> behaviors = script::BotBehaviors::create(source, "walk.luau", error);
    REQUIRE_MESSAGE(behaviors != nullptr, error);
    const OnePortal view;

    behaviors->restart(2);
    // No body yet: it has not begun.
    behaviors->step(2, script::BotSight{}, view, 0.0);
    CHECK_FALSE(behaviors->walkTarget(2).has_value());

    behaviors->step(2, at(0.0), view, 0.0);
    REQUIRE(behaviors->walkTarget(2).has_value());
    CHECK(behaviors->walkTarget(2)->x == doctest::Approx(10.0));
    behaviors->step(2, at(5.0), view, 1.0);
    CHECK(behaviors->walkTarget(2).has_value());

    // Within two metres is there, and then it waits a second.
    behaviors->step(2, at(9.0), view, 2.0);
    CHECK_FALSE(behaviors->walkTarget(2).has_value());
    behaviors->step(2, at(9.0), view, 2.5);
    CHECK_FALSE(behaviors->walkTarget(2).has_value());
    behaviors->step(2, at(9.0), view, 3.0);
    REQUIRE(behaviors->walkTarget(2).has_value());
    CHECK(behaviors->walkTarget(2)->x == doctest::Approx(100.0));

    // A walk that takes longer than it was given ends false.
    behaviors->step(2, at(10.0), view, 3.5);
    CHECK(behaviors->walkTarget(2).has_value());
    behaviors->step(2, at(11.0), view, 4.1);
    CHECK_FALSE(behaviors->walkTarget(2).has_value());
    CHECK(behaviors->finished() == 1);
    CHECK(behaviors->failed() == 0);
}

TEST_CASE("a bot's behavior begins again when the bot is somewhere new")
{
    const std::string source = R"(
local Bot = require("@engine/bot")
Bot:WalkTo(Bot.Spawn + vector.create(0, 0, 4))
)";
    std::string error;
    std::unique_ptr<script::BotBehaviors> behaviors = script::BotBehaviors::create(source, "again.luau", error);
    REQUIRE_MESSAGE(behaviors != nullptr, error);
    const OnePortal view;
    behaviors->restart(0);
    behaviors->step(0, at(1.0), view, 0.0);
    REQUIRE(behaviors->walkTarget(0).has_value());
    CHECK(behaviors->walkTarget(0)->x == doctest::Approx(1.0));
    // Sent on: a new world, a new spawn.
    behaviors->restart(0);
    CHECK_FALSE(behaviors->walkTarget(0).has_value());
    behaviors->step(0, at(50.0), view, 1.0);
    REQUIRE(behaviors->walkTarget(0).has_value());
    CHECK(behaviors->walkTarget(0)->x == doctest::Approx(50.0));
}

TEST_CASE("a bot's behavior that fails stops that bot and no other")
{
    // Bot 0 never waits; bot 1 asks for the game; bot 2 does as it should.
    const std::string source = R"(
local Bot = require("@engine/bot")
if Bot.Index == 0 then
    local n = 0
    while true do
        n += 1
    end
elseif Bot.Index == 1 then
    local _ = require("@game/anything")
end
Bot:Wait(10)
)";
    std::string error;
    std::unique_ptr<script::BotBehaviors> behaviors = script::BotBehaviors::create(source, "fails.luau", error);
    REQUIRE_MESSAGE(behaviors != nullptr, error);
    const OnePortal view;
    for (core::u32 bot = 0; bot < 3; ++bot) {
        behaviors->restart(bot);
        behaviors->step(bot, at(0.0), view, 0.0);
    }
    CHECK(behaviors->failed() == 2);
    CHECK(behaviors->finished() == 0);
    // The third is still waiting, and ends when its wait does.
    behaviors->step(2, at(0.0), view, 10.0);
    CHECK(behaviors->finished() == 1);
}

TEST_CASE("a bot's behavior that does not compile is refused before any bot runs it")
{
    std::string error;
    CHECK(script::BotBehaviors::create("local x = ", "broken.luau", error) == nullptr);
    CHECK(error.find("broken.luau") != std::string::npos);
}

TEST_CASE("a bot's behavior is sandboxed: the libraries' tables cannot be changed")
{
    const std::string source = R"(
local ok = pcall(function()
    (string :: any).upper = nil
end)
assert(not ok)
)";
    std::string error;
    std::unique_ptr<script::BotBehaviors> behaviors = script::BotBehaviors::create(source, "sandbox.luau", error);
    REQUIRE_MESSAGE(behaviors != nullptr, error);
    const OnePortal view;
    behaviors->restart(0);
    behaviors->step(0, at(0.0), view, 0.0);
    CHECK(behaviors->finished() == 1);
    CHECK(behaviors->failed() == 0);
}
