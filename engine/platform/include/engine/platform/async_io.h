// Asynchronous whole-file reads (architecture.md §2, §10).
//
// Streaming is the caller this exists for: a chunk arriving without a hitch is
// a read that was issued several seconds earlier, at a priority derived from
// how badly the world needs it, and collected at a known point in the frame.
//
// **Priority is ours, not SDL's.** SDL_AsyncIO has a completion queue and no
// notion of priority at all: every task it is handed is equally urgent. So the
// queue that matters is the one here -- requests wait in a priority order this
// module maintains, and only `maxInFlight` of them are handed to SDL at a time.
// That also bounds how much memory in-flight reads can hold, which a service
// with an unbounded submit cannot promise.
//
// **Completions land during `pumpIo()` and nowhere else.** The sketch in
// architecture.md had the callback fire from the IO thread; that would put an
// arbitrary thread and an arbitrary moment between a read and the world, which
// is exactly what R10 forbids for anything the simulation can see. Pumping at
// a FrameStart safe point costs one call per frame and makes "when did this
// land" a question with an answer.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

#include "engine/core/types.h"

namespace engine::platform {

using core::u32;
using core::u64;
using core::u8;
using core::usize;

// Ordered most urgent first. `Critical` is the must-have ring architecture.md
// §10 guarantees resident before a focus may advance into it; `Low` is
// speculative prefetch that may wait behind everything else indefinitely.
enum class IoPriority : u8
{
    Critical,
    High,
    Normal,
    Low,
};

enum class IoStatus : u8
{
    // Queued here, or handed to SDL and not yet finished.
    Pending,
    // Finished; the bytes are waiting to be taken.
    Ready,
    Failed,
    Cancelled,
    // The handle names a request that has already been taken or released.
    Unknown,
};

// A slot handle with a generation, so a stale handle is detectable rather than
// dangerous: it reports `Unknown` instead of somebody else's file.
struct IoRequest
{
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const IoRequest&) const noexcept = default;
};

// Invoked during `pumpIo()`, on the pumping thread, exactly once per request
// that carries one. The bytes are moved in; the request is released
// immediately afterwards, so the callback is the only chance to keep them.
using IoCallback = std::function<void(IoRequest request, IoStatus status, std::vector<std::byte>&& bytes)>;

// The most requests that may be outstanding. A fixed pool for the same reason
// the job pool has one: a runaway caller should hit a named limit.
inline constexpr u32 MaxIoRequests = 512;

// How much may have been read and not collected yet before the service stops
// asking the disk for more: a frame that stalls comes back to this much read
// ahead of it. Sixty-four megabytes is a thousand cells of ground.
inline constexpr u64 DefaultIoReadyCeiling = 64ull * 1024ull * 1024ull;

// `maxInFlight` is how many reads the disk is asked for at once -- and only
// that: a read stops counting when it has landed, not when a frame collects
// it. Idempotent, like `platform::init`.
//
// `harvestOnPump` is for a test that needs a read to stay in flight until it
// pumps: results are then taken from SDL inside `pumpIo`, on its thread, as
// they were before the service had a thread for it -- and the budget is
// reads a pump again.
[[nodiscard]] bool initIo(u32 maxInFlight = 8, bool harvestOnPump = false);

// Cancels what is still queued, waits for what SDL already has, and frees the
// queue. Safe without a successful `initIo`.
void shutdownIo();

[[nodiscard]] bool isIoInitialized() noexcept;

// Queues a whole-file read. Returns an invalid request when the service is not
// running or the pool is full -- both of which the caller must handle, because
// the alternative is a read that silently never lands.
//
// The path is taken by value and read on the IO thread, so the caller need not
// keep it alive.
[[nodiscard]] IoRequest readFileAsync(const std::filesystem::path& path, IoPriority priority, IoCallback callback = {});

// Fires the callbacks of the reads that have landed, on this thread and in the
// order they landed. Call once per frame from the thread that owns the frame.
// It collects nothing and admits nothing: a read lands, and the next is
// admitted, as the disk answers.
void pumpIo();

[[nodiscard]] IoStatus ioStatus(IoRequest request) noexcept;

// Moves a `Ready` request's bytes into `out` and releases the slot. False for
// any other status, and `out` is left untouched.
[[nodiscard]] bool takeIoResult(IoRequest request, std::vector<std::byte>& out);

// Drops a request. One still waiting here never reaches SDL; one SDL already
// has is left to finish and its bytes are discarded on arrival -- SDL_AsyncIO
// has no cancel, and pretending otherwise would mean a buffer nobody frees.
void cancelIo(IoRequest request);

// Re-prioritises a request that has not been handed to SDL yet. A chunk the
// focus turned towards is the reason this exists; one already in flight keeps
// its place, because there is nothing left to reorder.
void setIoPriority(IoRequest request, IoPriority priority);

struct IoStats
{
    u64 issued = 0;
    u64 completed = 0;
    u64 failed = 0;
    u64 cancelled = 0;
    u64 bytesRead = 0;
    // Handed to SDL and not yet finished.
    u32 inFlight = 0;
    // Waiting in this module's priority queue.
    u32 queued = 0;
    // Finished, with bytes nobody has taken yet.
    u32 ready = 0;
    // Bytes read and not collected yet, against the ceiling.
    u64 readyBytes = 0;
};

// The ceiling on what waits to be collected (`DefaultIoReadyCeiling`).
void setIoReadyCeiling(u64 bytes) noexcept;

[[nodiscard]] IoStats ioStats() noexcept;
void resetIoStats() noexcept;

} // namespace engine::platform
