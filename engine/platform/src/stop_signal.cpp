#include "engine/platform/stop_signal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::platform {
namespace {

// Lock-free, so a signal handler may touch it (the one thing a handler may do
// besides `_Exit`).
std::atomic<bool> requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

// 128 + SIGINT: what a shell reports for a program interrupted from its
// terminal, and what a process manager reads as "stopped, not crashed".
constexpr int InterruptedStatus = 130;

void request() noexcept
{
    if (requested.exchange(true))
        std::_Exit(InterruptedStatus);
}

void onSignal(int)
{
    request();
}

// **The close the operating system allows** (audit A7): what the engine is
// told to finish its close within, and how long the handler waits for it.
// Windows ends a process some five seconds after a console close; the wait
// stays under that, and the close under the wait.
constexpr double CloseAllowance = 3.5;
#ifdef _WIN32
constexpr double HandlerWait = 4.8;
#endif
std::atomic<double> deadline{std::numeric_limits<double>::infinity()};

// Waited on by the console handler's thread -- never by a signal handler,
// which may not take a lock.
std::mutex finishedLock;
std::condition_variable finishedChanged;
bool finished = false;

[[nodiscard]] bool waitFinished(double seconds)
{
    std::unique_lock<std::mutex> lock(finishedLock);
    return finishedChanged.wait_for(lock, std::chrono::duration<double>(seconds), [] { return finished; });
}

// A stop with a clock on it: said, asked, and answered only once the engine
// has closed or the clock has nearly run out.
bool askWithDeadline(double wait)
{
    deadline.store(CloseAllowance);
    request();
    return waitFinished(wait);
}

#ifdef _WIN32
// Ctrl+C reaches the C runtime's SIGINT as well; Ctrl+Break and closing the
// console reach only this. Answering TRUE keeps the process alive to honour
// the request -- for a close, Windows allows a few seconds before it ends the
// process anyway.
BOOL WINAPI onConsoleEvent(DWORD event)
{
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        request();
        return TRUE;
    // **Ended soon after this returns, whatever it returns**: so it does not
    // return until the engine has closed, or nearly all the time Windows
    // allows has gone (audit A7).
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        (void)askWithDeadline(HandlerWait);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

} // namespace

void installStopSignals()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#endif
}

bool stopRequested() noexcept
{
    return requested.load();
}

void clearStopRequest() noexcept
{
    requested.store(false);
    deadline.store(std::numeric_limits<double>::infinity());
    const std::lock_guard<std::mutex> lock(finishedLock);
    finished = false;
}

double stopDeadlineSeconds() noexcept
{
    return deadline.load();
}

void stopFinished() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(finishedLock);
        finished = true;
    }
    finishedChanged.notify_all();
}

bool simulateConsoleClose(double wait) noexcept
{
    return askWithDeadline(wait);
}

} // namespace engine::platform
