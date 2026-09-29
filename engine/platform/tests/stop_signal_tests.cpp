// Asking a running engine to stop from outside it (`stop_signal.h`).
#include <chrono>
#include <csignal>
#include <doctest/doctest.h>
#include <thread>

#include "engine/platform/stop_signal.h"

using namespace engine;

TEST_CASE("an interrupt asks the engine to stop rather than killing it")
{
    // **The first request only asks**: the process is still here to run the
    // game's close handlers. The second would end it, so a test raises one.
    platform::installStopSignals();
    platform::clearStopRequest();
    CHECK_FALSE(platform::stopRequested());
    REQUIRE(std::raise(SIGINT) == 0);
    CHECK(platform::stopRequested());
    platform::clearStopRequest();
    CHECK_FALSE(platform::stopRequested());
}

TEST_CASE("a termination request asks the same way")
{
    platform::installStopSignals();
    platform::clearStopRequest();
    REQUIRE(std::raise(SIGTERM) == 0);
    CHECK(platform::stopRequested());
    platform::clearStopRequest();
}

TEST_CASE("a console closed waits for the engine to close, within the time Windows allows (audit A7)")
{
    // The handler used to answer at once, and Windows ended the process
    // before the first `BindToClose` handler ran: a dedicated server closed
    // from its console lost what a match was holding.
    platform::installStopSignals();
    platform::clearStopRequest();
    CHECK(platform::stopDeadlineSeconds() > 1e9);

    // The engine closes a little later, on its own thread, as the frame loop
    // does -- and the handler is still there when it says so.
    std::thread engine([] {
        while (!platform::stopRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Told how long it has, and inside what Windows allows.
        CHECK(platform::stopDeadlineSeconds() < 5.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        platform::stopFinished();
    });
    CHECK(platform::simulateConsoleClose(4.8));
    engine.join();
    platform::clearStopRequest();

    // An engine that never answers is not waited for past the allowance.
    const auto began = std::chrono::steady_clock::now();
    CHECK_FALSE(platform::simulateConsoleClose(0.05));
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    platform::clearStopRequest();
}
