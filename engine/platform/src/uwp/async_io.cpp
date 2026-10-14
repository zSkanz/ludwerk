#include "engine/platform/async_io.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "engine/platform/file.h"

namespace engine::platform {
namespace {
struct Slot
{
    u32 generation = 0;
    IoStatus status = IoStatus::Unknown;
    IoPriority priority = IoPriority::Normal;
    u64 order = 0;
    bool flight = false, cancelled = false, succeeded = false;
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    IoCallback callback;
};
std::array<Slot, MaxIoRequests> slots;
std::mutex guard;
std::condition_variable changed;
std::vector<std::thread> workers;
bool running = false;
bool harvest = false;
std::deque<IoRequest> arrivals, callbacks;
u64 ceiling = DefaultIoReadyCeiling, sequence = 0;
IoStats stats;
Slot* find(IoRequest h)
{
    if (h.index >= slots.size() || !h.generation || slots[h.index].generation != h.generation ||
        slots[h.index].status == IoStatus::Unknown)
        return nullptr;
    return &slots[h.index];
}
void release(Slot& s)
{
    if (s.status == IoStatus::Ready) {
        --stats.ready;
        stats.readyBytes -= s.bytes.size();
    }
    s.status = IoStatus::Unknown;
    s.bytes = {};
    s.path.clear();
    s.callback = {};
    changed.notify_all();
}
void land(Slot& s)
{
    s.flight = false;
    --stats.inFlight;
    if (s.cancelled || !running) {
        release(s);
        return;
    }
    if (s.succeeded) {
        s.status = IoStatus::Ready;
        ++stats.ready;
        ++stats.completed;
        stats.readyBytes += s.bytes.size();
        stats.bytesRead += s.bytes.size();
    }
    else {
        s.status = IoStatus::Failed;
        ++stats.failed;
    }
    if (s.callback)
        callbacks.push_back({static_cast<u32>(&s - slots.data()), s.generation});
    changed.notify_all();
}
void work()
{
    std::unique_lock lock(guard);
    while (running) {
        Slot* next = nullptr;
        for (auto& s : slots)
            if (s.status == IoStatus::Pending && !s.flight &&
                (!next || s.priority < next->priority || (s.priority == next->priority && s.order < next->order)))
                next = &s;
        if (!next || stats.readyBytes >= ceiling) {
            changed.wait(lock);
            continue;
        }
        auto& s = *next;
        s.flight = true;
        --stats.queued;
        ++stats.inFlight;
        ++stats.issued;
        const u32 generation = s.generation;
        const auto path = s.path;
        lock.unlock();
        std::vector<std::byte> bytes;
        bool okay = false;
        try {
            okay = readFile(path, bytes);
        } catch (...) {
            okay = false;
        }
        lock.lock();
        s.bytes = std::move(bytes);
        s.succeeded = okay;
        if (harvest && running) {
            arrivals.push_back({static_cast<u32>(&s - slots.data()), s.generation});
            changed.wait(lock, [&] { return !running || !s.flight || s.generation != generation; });
        }
        else
            land(s);
    }
}
} // namespace
bool initIo(u32 count, bool harvestOnPump)
{
    std::unique_lock lock(guard);
    if (running)
        return true;
    running = true;
    harvest = harvestOnPump;
    stats = {};
    arrivals.clear();
    callbacks.clear();
    try {
        for (u32 i = 0; i < std::clamp(count, 1u, MaxIoRequests); ++i)
            workers.emplace_back(work);
    } catch (...) {
        running = false;
        changed.notify_all();
        lock.unlock();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
        workers.clear();
        return false;
    }
    return true;
}
void shutdownIo()
{
    {
        const std::lock_guard lock(guard);
        running = false;
        changed.notify_all();
    }
    for (auto& worker : workers)
        if (worker.joinable())
            worker.join();
    workers.clear();
    const std::lock_guard lock(guard);
    for (auto& s : slots) {
        s.status = IoStatus::Unknown;
        s.bytes = {};
        s.path.clear();
        s.callback = {};
        s.flight = s.cancelled = false;
    }
    stats.inFlight = stats.queued = stats.ready = 0;
    stats.readyBytes = 0;
    arrivals.clear();
    callbacks.clear();
}
bool isIoInitialized() noexcept
{
    const std::lock_guard lock(guard);
    return running;
}
IoRequest readFileAsync(const std::filesystem::path& p, IoPriority priority, IoCallback callback)
{
    const std::lock_guard lock(guard);
    if (!running)
        return {};
    for (u32 i = 0; i < slots.size(); ++i)
        if (slots[i].status == IoStatus::Unknown) {
            auto& s = slots[i];
            if (++s.generation == 0)
                ++s.generation;
            s.status = IoStatus::Pending;
            s.cancelled = false;
            s.flight = false;
            s.path = p;
            s.callback = std::move(callback);
            s.priority = priority;
            s.order = ++sequence;
            ++stats.queued;
            changed.notify_one();
            return {i, s.generation};
        }
    return {};
}
void pumpIo()
{
    std::deque<IoRequest> completed;
    {
        const std::lock_guard lock(guard);
        while (!arrivals.empty()) {
            const auto request = arrivals.front();
            arrivals.pop_front();
            if (auto* s = find(request))
                land(*s);
        }
        completed.swap(callbacks);
    }
    for (const auto request : completed) {
        IoCallback callback;
        std::vector<std::byte> bytes;
        IoStatus status;
        {
            const std::lock_guard lock(guard);
            auto* value = find(request);
            if (!value || !value->callback || value->status == IoStatus::Pending)
                continue;
            auto& s = *value;
            status = s.status;
            callback = std::move(s.callback);
            bytes = std::move(s.bytes);
            if (status == IoStatus::Ready) {
                --stats.ready;
                stats.readyBytes -= bytes.size();
            }
            s.status = IoStatus::Unknown;
            s.path.clear();
            changed.notify_all();
        }
        callback(request, status, std::move(bytes));
    }
}
IoStatus ioStatus(IoRequest h) noexcept
{
    const std::lock_guard lock(guard);
    const auto* s = find(h);
    return s ? s->status : IoStatus::Unknown;
}
bool takeIoResult(IoRequest h, std::vector<std::byte>& out)
{
    const std::lock_guard lock(guard);
    auto* s = find(h);
    if (!s || s->status != IoStatus::Ready)
        return false;
    --stats.ready;
    stats.readyBytes -= s->bytes.size();
    out = std::move(s->bytes);
    s->status = IoStatus::Unknown;
    s->callback = {};
    s->path.clear();
    changed.notify_all();
    return true;
}
void cancelIo(IoRequest h)
{
    const std::lock_guard lock(guard);
    auto* s = find(h);
    if (!s)
        return;
    if (s->cancelled)
        return;
    ++stats.cancelled;
    s->cancelled = true;
    s->callback = {};
    if (s->flight)
        return;
    if (s->status == IoStatus::Pending)
        --stats.queued;
    release(*s);
}
void setIoPriority(IoRequest h, IoPriority p)
{
    const std::lock_guard lock(guard);
    if (auto* s = find(h); s && !s->flight)
        s->priority = p;
    changed.notify_all();
}
void setIoReadyCeiling(u64 b) noexcept
{
    const std::lock_guard lock(guard);
    ceiling = std::max(u64{1}, b);
    changed.notify_all();
}
IoStats ioStats() noexcept
{
    const std::lock_guard lock(guard);
    return stats;
}
void resetIoStats() noexcept
{
    const std::lock_guard lock(guard);
    stats.issued = stats.completed = stats.failed = stats.cancelled = stats.bytesRead = 0;
}
} // namespace engine::platform
