#include "engine/app/data_worker.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace engine::app {

DataWorker::DataWorker(std::unique_ptr<IDataStore> store, core::usize most)
    : m_store(std::move(store)), m_most(std::max<core::usize>(most, 1)), m_thread([this]() { run(); })
{}

DataWorker::~DataWorker()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool DataWorker::post(Job job)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_jobs.size() >= m_most) {
            ++m_refused;
            return false;
        }
        m_jobs.push_back(Queued{std::move(job), Clock::now()});
    }
    m_wake.notify_one();
    return true;
}

void DataWorker::wait(const std::function<void(IDataStore&)>& job)
{
    // **What says the job is done lives as long as the job, not as long as
    // this call.** It was on this stack, and the worker told it after letting
    // go of its lock: a waiter woken in between -- a spurious wake is allowed
    // -- saw the job done, returned, and the worker then told a condition
    // variable that was gone, in memory the next call's stood in. On macOS
    // that left the next wait asleep for good, the worker idle beside it: the
    // `app` tests' hang there. Shared with the job instead, and told while
    // its lock is held.
    struct Done
    {
        std::mutex mutex;
        std::condition_variable told;
        bool over = false;
    };
    const auto done = std::make_shared<Done>();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Queued{[&job, done](IDataStore& store) -> Then {
                                    job(store);
                                    const std::lock_guard<std::mutex> finished(done->mutex);
                                    done->over = true;
                                    done->told.notify_one();
                                    return {};
                                },
                                Clock::now()});
    }
    m_wake.notify_one();
    std::unique_lock<std::mutex> lock(done->mutex);
    done->told.wait(lock, [&]() { return done->over; });
}

void DataWorker::pump()
{
    std::deque<Then> done;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        done.swap(m_done);
    }
    for (Then& then : done) {
        if (then)
            then();
    }
}

DataWorker::Stats DataWorker::stats() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return Stats{m_jobs.size(), m_most, m_doneCount, m_refused, m_worstMs};
}

core::f64 DataWorker::takeWorst()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const core::f64 worst = m_worstMs;
    m_worstMs = 0.0;
    return worst;
}

void DataWorker::run()
{
    for (;;) {
        Queued next;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]() { return m_stopping || !m_jobs.empty(); });
            // **Stopping finishes the queue first**: a write a world sent
            // before the manager stopped is written.
            if (m_jobs.empty())
                return;
            next = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        Then then = next.job(*m_store);
        const core::f64 tookMs = std::chrono::duration<core::f64, std::milli>(Clock::now() - next.posted).count();
        std::function<void()> told;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_doneCount;
            m_worstMs = std::max(m_worstMs, tookMs);
            if (then) {
                m_done.push_back(std::move(then));
                told = m_told;
            }
        }
        if (told)
            told();
    }
}

void DataWorker::onDone(std::function<void()> told)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_told = std::move(told);
}

} // namespace engine::app
