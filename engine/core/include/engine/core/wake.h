// **A loop's wait that work ends early.** A loop that sleeps a fixed time
// between its frames makes everything it answers wait up to that time: the
// manager slept twenty milliseconds a frame, and every data operation a world
// asked waited up to two of them. Each thread that brings the loop something
// -- a child's output, an answer from the store -- signals; the loop waits
// for a signal, or for its frame's time when nothing comes, and costs nothing
// while it waits.
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace engine::core {

class Wake
{
public:
    // From any thread. Signals since the last wait are one.
    void signal()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_signalled = true;
        }
        m_ready.notify_one();
    }

    // Until a signal since the last wait, or `most`; whether one came.
    bool waitFor(std::chrono::nanoseconds most)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool came = m_ready.wait_for(lock, most, [this]() { return m_signalled; });
        m_signalled = false;
        return came;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    bool m_signalled = false;
};

} // namespace engine::core
