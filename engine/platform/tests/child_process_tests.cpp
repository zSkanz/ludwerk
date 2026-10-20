// A child's output read as it comes (`ProcessOptions::onOutput`): the parent
// is told the moment the child writes, not at its next look -- what lets a
// manager answer a world at once.
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/wake.h"
#include "engine/platform/process.h"

using namespace engine;
using Clock = std::chrono::steady_clock;

namespace {

[[nodiscard]] double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

TEST_CASE("a child's output wakes its parent the moment it is written")
{
    auto wake = std::make_shared<core::Wake>();
    platform::ProcessOptions options;
    options.onOutput = [wake]() { wake->signal(); };
    std::unique_ptr<platform::ChildProcess> child =
        platform::ChildProcess::start({ENG_TEST_TALK_PROBE, "400", "0"}, options);
    REQUIRE(child != nullptr);

    // **The first line**, however long the child takes to start.
    std::string heard;
    const Clock::time_point started = Clock::now();
    while (heard.find("one") == std::string::npos && secondsSince(started) < 20.0) {
        (void)wake->waitFor(std::chrono::seconds(5));
        heard += child->readAvailable();
    }
    REQUIRE(heard.find("one") != std::string::npos);

    // **The second, as it is written**: four tenths of a second after the
    // first, not at the end of a wait of five.
    const Clock::time_point first = Clock::now();
    while (heard.find("two") == std::string::npos && secondsSince(first) < 5.0) {
        (void)wake->waitFor(std::chrono::seconds(5));
        heard += child->readAvailable();
    }
    const double between = secondsSince(first);
    CHECK(heard.find("two") != std::string::npos);
    CHECK(between >= 0.2);
    CHECK(between < 2.0);
}

TEST_CASE("a child still running is ended with its reader, and nothing waits on it")
{
    platform::ProcessOptions options;
    options.onOutput = []() {};
    const Clock::time_point started = Clock::now();
    {
        std::unique_ptr<platform::ChildProcess> child =
            platform::ChildProcess::start({ENG_TEST_TALK_PROBE, "0", "60000"}, options);
        REQUIRE(child != nullptr);
        CHECK(child->running());
    }
    CHECK(secondsSince(started) < 10.0);
}
