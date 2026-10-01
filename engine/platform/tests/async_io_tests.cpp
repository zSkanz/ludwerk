#include <chrono>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/platform/async_io.h"

using namespace engine::platform;
using engine::core::u32;
using engine::core::usize;

namespace {

// A directory of real files, because the point of this module is that SDL --
// not a mock -- reads them. Removed on the way out so a failing case does not
// leave the machine dirty.
struct Fixture
{
    std::filesystem::path root;

    // Deliberately does NOT call platform::init: SDL's async file IO needs no
    // subsystem, and a fixture that brought one up would leave it up for the
    // cases in this suite that assert the process starts uninitialized.
    // `harvestOnPump` is for the cases about the QUEUE: one read has to stay
    // in flight until the case pumps, and with the service's own harvester it
    // lands -- and frees its place -- as soon as the disk answers.
    explicit Fixture(u32 maxInFlight, bool harvestOnPump = false)
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / "engine-async-io-tests";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        REQUIRE(std::filesystem::is_directory(root));

        REQUIRE(initIo(maxInFlight, harvestOnPump));
        resetIoStats();
    }

    ~Fixture()
    {
        shutdownIo();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::filesystem::path write(const std::string& name, const std::string& contents) const
    {
        const std::filesystem::path path = root / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        return path;
    }
};

// Pumps until the request leaves `Pending` or the budget runs out. Bounded
// rather than open-ended, because the bound is what turns "the disk hung" into
// a failing assertion instead of a hung suite -- and it is bounded in TIME
// rather than in iterations, since how many pumps fit into one disk read is
// exactly the thing that differs between this machine and a CI runner.
constexpr int MaxWaitMillis = 5000;

// Waits until the submitter has actually handed a read to SDL.
//
// Needed since D038 moved admission onto its own thread: `readFileAsync` now
// queues and returns, so "submitted" and "in flight" are two moments with a
// scheduler between them. Every case below that reasons about QUEUE ORDER has
// to pin the first one down first, or it is racing the submitter rather than
// testing the queue.
void waitUntilInFlight(u32 count)
{
    for (int i = 0; i < MaxWaitMillis && ioStats().inFlight < count; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(ioStats().inFlight >= count);
}

[[nodiscard]] IoStatus pumpUntilSettled(IoRequest request)
{
    for (int i = 0; i < MaxWaitMillis; ++i) {
        const IoStatus status = ioStatus(request);
        if (status != IoStatus::Pending) {
            return status;
        }
        pumpIo();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ioStatus(request);
}

// The same bound, for the cases that watch a callback rather than a status.
template <class Predicate>
void pumpUntil(Predicate done)
{
    for (int i = 0; i < MaxWaitMillis && !done(); ++i) {
        pumpIo();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

[[nodiscard]] std::string asString(const std::vector<std::byte>& bytes)
{
    std::string out;
    out.resize(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return out;
}

} // namespace

TEST_CASE("an async read delivers the file's bytes")
{
    Fixture fixture(4);
    // Built with an explicit length: a std::string from this literal would stop
    // at the nul, and the point of the case is that the reader does not.
    const std::string contents("the quick brown fox\0with an embedded nul", 40);
    const std::filesystem::path path = fixture.write("plain.bin", contents);

    const IoRequest request = readFileAsync(path, IoPriority::Normal);
    REQUIRE(request.valid());
    REQUIRE(pumpUntilSettled(request) == IoStatus::Ready);

    std::vector<std::byte> bytes;
    REQUIRE(takeIoResult(request, bytes));
    CHECK(bytes.size() == 40);
    CHECK(asString(bytes) == contents);

    // Taking releases the slot, so the handle now names nothing.
    CHECK(ioStatus(request) == IoStatus::Unknown);
    CHECK_FALSE(takeIoResult(request, bytes));

    const IoStats stats = ioStats();
    CHECK(stats.completed == 1);
    CHECK(stats.bytesRead == 40);
}

TEST_CASE("an empty file is a success with no bytes")
{
    Fixture fixture(4);
    const std::filesystem::path path = fixture.write("empty.bin", "");

    const IoRequest request = readFileAsync(path, IoPriority::Normal);
    REQUIRE(pumpUntilSettled(request) == IoStatus::Ready);

    std::vector<std::byte> bytes;
    REQUIRE(takeIoResult(request, bytes));
    CHECK(bytes.empty());
}

TEST_CASE("a missing file fails rather than hanging")
{
    Fixture fixture(4);
    const IoRequest request = readFileAsync(fixture.root / "does-not-exist.bin", IoPriority::Normal);
    REQUIRE(request.valid());

    const IoStatus status = pumpUntilSettled(request);
    CHECK(status == IoStatus::Failed);
    CHECK(ioStats().failed == 1);
}

TEST_CASE("a callback fires during the pump and releases the request")
{
    Fixture fixture(4);
    const std::filesystem::path path = fixture.write("callback.bin", "payload");

    int calls = 0;
    std::string received;
    IoStatus seen = IoStatus::Unknown;
    const IoRequest request =
        readFileAsync(path, IoPriority::High, [&](IoRequest, IoStatus status, std::vector<std::byte>&& bytes) {
            calls += 1;
            seen = status;
            received = asString(bytes);
        });
    REQUIRE(request.valid());

    pumpUntil([&calls] { return calls > 0; });

    CHECK(calls == 1);
    CHECK(seen == IoStatus::Ready);
    CHECK(received == "payload");
    // The callback was the one chance to keep the bytes.
    CHECK(ioStatus(request) == IoStatus::Unknown);
}

TEST_CASE("priority decides which queued read is admitted next")
{
    // One in flight at a time, held until the pump, which is what makes the
    // queue observable at all.
    Fixture fixture(1, true);
    fixture.write("first.bin", "1");
    fixture.write("low.bin", "2");
    fixture.write("critical.bin", "3");

    std::vector<std::string> order;
    const auto record = [&order](const char* name) {
        return [&order, name](IoRequest, IoStatus, std::vector<std::byte>&&) { order.emplace_back(name); };
    };

    // The first is admitted immediately and occupies the only slot; the other
    // two wait, and the more urgent of them must go next even though it was
    // submitted last.
    const IoRequest first = readFileAsync(fixture.root / "first.bin", IoPriority::Normal, record("first"));
    waitUntilInFlight(1);
    const IoRequest low = readFileAsync(fixture.root / "low.bin", IoPriority::Low, record("low"));
    const IoRequest critical = readFileAsync(fixture.root / "critical.bin", IoPriority::Critical, record("critical"));
    REQUIRE(first.valid());
    REQUIRE(low.valid());
    REQUIRE(critical.valid());

    pumpUntil([&order] { return order.size() >= 3; });

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "first");
    CHECK(order[1] == "critical");
    CHECK(order[2] == "low");
}

TEST_CASE("raising a queued request's priority moves it up the queue")
{
    Fixture fixture(1, true);
    fixture.write("blocker.bin", "1");
    fixture.write("a.bin", "2");
    fixture.write("b.bin", "3");

    std::vector<std::string> order;
    const auto record = [&order](const char* name) {
        return [&order, name](IoRequest, IoStatus, std::vector<std::byte>&&) { order.emplace_back(name); };
    };

    const IoRequest blocker = readFileAsync(fixture.root / "blocker.bin", IoPriority::Normal, record("blocker"));
    // The blocker has to be IN FLIGHT before the other two are queued, or the
    // submitter may pick one of them first and this stops being a test about
    // the queue. See `waitUntilInFlight`.
    waitUntilInFlight(1);
    const IoRequest a = readFileAsync(fixture.root / "a.bin", IoPriority::Normal, record("a"));
    const IoRequest b = readFileAsync(fixture.root / "b.bin", IoPriority::Normal, record("b"));
    REQUIRE(blocker.valid());
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    // Same band, so `a` would go first on submission order alone. A chunk the
    // focus turned towards is why this call exists.
    setIoPriority(b, IoPriority::Critical);

    pumpUntil([&order] { return order.size() >= 3; });

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "blocker");
    CHECK(order[1] == "b");
    CHECK(order[2] == "a");
}

TEST_CASE("cancelling a queued read releases it without reading anything")
{
    Fixture fixture(1, true);
    fixture.write("held.bin", "1");
    fixture.write("dropped.bin", "2");

    const IoRequest held = readFileAsync(fixture.root / "held.bin", IoPriority::Normal);
    // The held read has to occupy the only in-flight place before the second is
    // queued, or "queued" below counts a read the submitter simply has not got
    // to yet (D038 put admission on its own thread).
    waitUntilInFlight(1);
    const IoRequest dropped = readFileAsync(fixture.root / "dropped.bin", IoPriority::Normal);
    REQUIRE(held.valid());
    REQUIRE(dropped.valid());
    CHECK(ioStats().queued == 1);

    cancelIo(dropped);
    CHECK(ioStatus(dropped) == IoStatus::Unknown);
    CHECK(ioStats().queued == 0);
    CHECK(ioStats().cancelled == 1);

    REQUIRE(pumpUntilSettled(held) == IoStatus::Ready);
    std::vector<std::byte> bytes;
    CHECK(takeIoResult(held, bytes));
    // Only the one that was not cancelled ever reached SDL.
    CHECK(ioStats().issued == 1);
}

TEST_CASE("many reads in flight all land")
{
    Fixture fixture(4);
    constexpr int count = 64;
    std::vector<IoRequest> requests;
    requests.reserve(count);
    for (int i = 0; i < count; ++i) {
        const std::string name = "file" + std::to_string(i) + ".bin";
        fixture.write(name, std::string(static_cast<usize>(i) + 1, 'x'));
        requests.push_back(readFileAsync(fixture.root / name, IoPriority::Normal));
        REQUIRE(requests.back().valid());
    }

    for (int i = 0; i < count; ++i) {
        REQUIRE(pumpUntilSettled(requests[static_cast<usize>(i)]) == IoStatus::Ready);
        std::vector<std::byte> bytes;
        REQUIRE(takeIoResult(requests[static_cast<usize>(i)], bytes));
        CHECK(bytes.size() == static_cast<usize>(i) + 1);
    }
    CHECK(ioStats().completed == static_cast<engine::core::u64>(count));
}

// --- Reads land as the disk answers, not as the frame collects (C1) ---------
//
// The service's in-flight budget was freed only inside `pumpIo`, on the frame's
// thread: four reads a pump, 240 a second at sixty frames, on a disk that reads
// thousands. A flight over streamed ground outran it.

TEST_CASE("reads land without a pump, and the budget frees as each one does")
{
    // One at a time, and thirty-two of them: before, the second would have
    // waited for a pump that this case never gives.
    Fixture fixture(1);
    constexpr int count = 32;
    std::vector<IoRequest> requests;
    for (int i = 0; i < count; ++i) {
        const std::string name = "landed" + std::to_string(i) + ".bin";
        fixture.write(name, std::string(64, 'y'));
        requests.push_back(readFileAsync(fixture.root / name, IoPriority::Normal));
        REQUIRE(requests.back().valid());
    }
    for (int i = 0; i < MaxWaitMillis && ioStats().ready < static_cast<u32>(count); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ioStats().ready == static_cast<u32>(count));
    CHECK(ioStats().inFlight == 0);
    CHECK(ioStats().readyBytes == 64u * static_cast<engine::core::u64>(count));
    for (const IoRequest request : requests) {
        CHECK(ioStatus(request) == IoStatus::Ready);
        std::vector<std::byte> bytes;
        CHECK(takeIoResult(request, bytes));
        CHECK(bytes.size() == 64);
    }
    CHECK(ioStats().readyBytes == 0);
}

TEST_CASE("a callback still fires on the pump's thread, in the order the reads landed")
{
    Fixture fixture(1);
    std::vector<int> order;
    const std::thread::id here = std::this_thread::get_id();
    bool onThisThread = true;
    for (int i = 0; i < 8; ++i) {
        const std::string name = "ordered" + std::to_string(i) + ".bin";
        fixture.write(name, "z");
        const IoRequest request = readFileAsync(fixture.root / name, IoPriority::Normal,
                                                [&, i](IoRequest, IoStatus, std::vector<std::byte>&&) {
                                                    order.push_back(i);
                                                    onThisThread = onThisThread && std::this_thread::get_id() == here;
                                                });
        REQUIRE(request.valid());
    }
    // Landed, and nothing said yet: nobody has pumped.
    for (int i = 0; i < MaxWaitMillis && ioStats().completed < 8; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(ioStats().completed == 8);
    CHECK(order.empty());

    pumpIo();
    REQUIRE(order.size() == 8);
    // One in flight at a time and one band: submission order is landing order.
    for (int i = 0; i < 8; ++i)
        CHECK(order[static_cast<usize>(i)] == i);
    CHECK(onThisThread);
}

TEST_CASE("what waits to be collected is bounded, and collecting lets the rest land")
{
    Fixture fixture(4);
    // Room for four kilobyte files, and sixteen asked for.
    setIoReadyCeiling(4096);
    constexpr int count = 16;
    std::vector<IoRequest> requests;
    for (int i = 0; i < count; ++i) {
        const std::string name = "bounded" + std::to_string(i) + ".bin";
        fixture.write(name, std::string(1024, 'w'));
        requests.push_back(readFileAsync(fixture.root / name, IoPriority::Normal));
        REQUIRE(requests.back().valid());
    }
    // It settles with the disk idle and most of them still queued: the ceiling,
    // and at most the four that were in flight when it was reached.
    for (int i = 0; i < MaxWaitMillis && !(ioStats().inFlight == 0 && ioStats().readyBytes >= 4096); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const IoStats full = ioStats();
    CHECK(full.inFlight == 0);
    CHECK(full.readyBytes >= 4096);
    CHECK(full.readyBytes <= 4096 + 4 * 1024);
    CHECK(full.queued >= static_cast<u32>(count) - 8);

    // Collected, one at a time, and every one of them lands.
    int taken = 0;
    for (int i = 0; i < MaxWaitMillis * 4 && taken < count; ++i) {
        for (const IoRequest request : requests) {
            std::vector<std::byte> bytes;
            if (ioStatus(request) == IoStatus::Ready && takeIoResult(request, bytes)) {
                CHECK(bytes.size() == 1024);
                ++taken;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(taken == count);
    CHECK(ioStats().readyBytes == 0);
}

TEST_CASE("a read requested without the service running is refused rather than lost")
{
    shutdownIo();
    CHECK_FALSE(isIoInitialized());

    const IoRequest request = readFileAsync("anything.bin", IoPriority::Normal);
    // An invalid handle is a refusal the caller can see. The alternative -- a
    // valid-looking request that never lands -- is a bug that surfaces as a
    // chunk that never appears.
    CHECK_FALSE(request.valid());
    CHECK(ioStatus(request) == IoStatus::Unknown);
}

// --- The pool's ceiling, and what happens when it is reached (S8.5, D131) ----
//
// D131 was a leaked slot: every read that FAILED kept its slot for ever, and
// past 512 thumbnails, material textures and world streaming all stopped being
// asynchronous at once, process-wide, in silence. The leak was fixed at four
// call sites. **The silence was not**, and that is what these cover: the
// contract at the ceiling, and the fact that reaching it now says so.
//
// Nothing here asserts the log LINE -- there is no sink to read it back from at
// this layer. What is asserted is the behaviour that made the leak invisible:
// the refusal is a refusal rather than a hang, and capacity comes back.

TEST_CASE("the pool refuses past its ceiling and recovers when slots come back")
{
    Fixture fixture(4);

    const std::filesystem::path file = fixture.write("ceiling.bin", "x");

    // Filled with reads that are QUEUED rather than in flight -- `maxInFlight`
    // is far below the pool size, so all but a handful sit waiting and hold
    // their slots without touching the disk. Nothing is pumped, so nothing
    // completes and nothing is released.
    std::vector<engine::platform::IoRequest> held;
    held.reserve(engine::platform::MaxIoRequests);
    for (engine::core::u32 index = 0; index < engine::platform::MaxIoRequests; ++index) {
        const engine::platform::IoRequest request = engine::platform::readFileAsync(file, IoPriority::Normal);
        REQUIRE_MESSAGE(request.valid(), "slot " << index << " of the pool would not allocate");
        held.push_back(request);
    }

    // **The 513th is refused, not queued and not blocked.** A caller that got a
    // hang here would be a frame that stopped; a caller that got a valid handle
    // to a slot that does not exist would be worse. It falls back to the
    // synchronous read, which is why nothing ever broke -- and why nobody
    // noticed for a milestone.
    const engine::platform::IoRequest refused = engine::platform::readFileAsync(file, IoPriority::Normal);
    CHECK_FALSE(refused.valid());

    // Give one slot back. Capacity returns immediately: the ceiling is a
    // ceiling and not a one-way door.
    engine::platform::cancelIo(held.back());
    held.pop_back();

    const engine::platform::IoRequest afterRelease = engine::platform::readFileAsync(file, IoPriority::Normal);
    CHECK(afterRelease.valid());

    engine::platform::cancelIo(afterRelease);
    for (const engine::platform::IoRequest& request : held) {
        engine::platform::cancelIo(request);
    }

    // **Pumped, because cancelling an IN-FLIGHT read cannot release its slot on
    // the spot.** SDL is reading into that request's buffer; the slot has to
    // survive until the completion lands, and `pumpIo` is what collects it.
    // Four of these were in flight -- `maxInFlight` above -- so without this the
    // refill below falls exactly four short, which looks like a leak and is the
    // opposite of one.
    pumpUntil([]() { return ioStats().inFlight == 0; });

    // And the pool is whole again -- which is the assertion D131 would have
    // failed. A leak leaves this short by one for every read that failed.
    std::vector<engine::platform::IoRequest> refilled;
    refilled.reserve(engine::platform::MaxIoRequests);
    for (engine::core::u32 index = 0; index < engine::platform::MaxIoRequests; ++index) {
        const engine::platform::IoRequest request = engine::platform::readFileAsync(file, IoPriority::Normal);
        REQUIRE_MESSAGE(request.valid(), "the pool came back short: slot " << index << " would not allocate");
        refilled.push_back(request);
    }
    for (const engine::platform::IoRequest& request : refilled) {
        engine::platform::cancelIo(request);
    }
}
