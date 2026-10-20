#include "engine/app/data_worker.h"

#include <algorithm>
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
    std::mutex finishing;
    std::condition_variable finished;
    bool over = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Queued{[&](IDataStore& store) -> Then {
                                    job(store);
                                    {
                                        const std::lock_guard<std::mutex> done(finishing);
                                        over = true;
                                    }
                                    finished.notify_one();
                                    return {};
                                },
                                Clock::now()});
    }
    m_wake.notify_one();
    std::unique_lock<std::mutex> lock(finishing);
    finished.wait(lock, [&]() { return over; });
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
        const core::f64 tookMs =
            std::chrono::duration<core::f64, std::milli>(Clock::now() - next.posted).count();
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_doneCount;
        m_worstMs = std::max(m_worstMs, tookMs);
        if (then)
            m_done.push_back(std::move(then));
    }
}

} // namespace engine::app
