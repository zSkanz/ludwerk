// **The game's database, off the loop that asks it** (ADR 0238 §5, as built
// in C6): one thread owns the store and does what it is asked, in the order
// it was asked; the asker's loop takes the answers when it chooses. A
// database's fsync, or a game's backend a hundred milliseconds away, is then
// never a frame of the manager's.
//
// **One thread, first in first out**, and that is the guarantee the data
// service rests on: a world's write is done before its next read, and a
// record let go of is read by its next holder as it was written.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "engine/app/data_store.h"
#include "engine/core/types.h"

namespace engine::app {

class DataWorker
{
public:
    // How many jobs may wait before a new one is refused: a backend that has
    // stopped answering fills it in seconds, and a queue without a bound is
    // the manager's memory given to it.
    static constexpr core::usize DefaultMost = 4096;

    // What runs on the asker's thread once a job is done: the job's answer,
    // carried in the closure.
    using Then = std::function<void()>;
    // What runs on the store's thread, and returns what to do with it.
    using Job = std::function<Then(IDataStore&)>;

    explicit DataWorker(std::unique_ptr<IDataStore> store, core::usize most = DefaultMost);
    // Finishes what was asked, then stops: a write a world sent is written.
    ~DataWorker();

    DataWorker(const DataWorker&) = delete;
    DataWorker& operator=(const DataWorker&) = delete;

    // Queued, or false -- nothing queued -- when `most` wait already.
    [[nodiscard]] bool post(Job job);
    // **Done before it returns**, after everything asked before it: a
    // console's command, a test. Never refused for the bound.
    void wait(const std::function<void(IDataStore&)>& job);
    // The answers of the jobs done since, each on this thread, in order.
    void pump();

    struct Stats
    {
        core::usize queued = 0;
        core::usize most = 0;
        core::u64 done = 0;
        core::u64 refused = 0;
        // The longest a job waited and ran, since `takeWorst` last asked.
        core::f64 worstMs = 0.0;
    };
    [[nodiscard]] Stats stats() const;
    // The worst since the last call, which starts the count again.
    core::f64 takeWorst();

private:
    using Clock = std::chrono::steady_clock;
    struct Queued
    {
        Job job;
        Clock::time_point posted;
    };

    void run();

    std::unique_ptr<IDataStore> m_store;
    core::usize m_most;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Queued> m_jobs;
    std::deque<Then> m_done;
    bool m_stopping = false;
    core::u64 m_doneCount = 0;
    core::u64 m_refused = 0;
    core::f64 m_worstMs = 0.0;
    std::thread m_thread;
};

} // namespace engine::app
