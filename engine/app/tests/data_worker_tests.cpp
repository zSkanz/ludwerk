// **The game's database off the loop that asks it** (ADR 0238 §5, C6): the
// worker does what it is asked in order, bounds what waits, and says how it
// keeps up; a game's own backend is asked over HTTP with an id per request,
// sent again on a lost answer, and refused in the clear beyond this network.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/backend_data_store.h"
#include "engine/app/data_broker.h"
#include "engine/app/data_ops.h"
#include "engine/app/data_worker.h"
#include "engine/core/json.h"

using engine::app::DataWorker;
using engine::app::IDataStore;
namespace core = engine::core;

TEST_CASE("data worker > jobs are done in the order asked, and answered on the asker's thread")
{
    DataWorker worker(engine::app::makeMemoryDataStore());
    std::vector<int> done;
    const std::thread::id asker = std::this_thread::get_id();
    std::atomic<bool> onWorker{true};
    for (int at = 0; at < 50; ++at) {
        REQUIRE(worker.post([&, at](IDataStore&) -> DataWorker::Then {
            onWorker = onWorker && std::this_thread::get_id() != asker;
            return [&done, at]() { done.push_back(at); };
        }));
    }
    worker.wait([](IDataStore&) {});
    CHECK(done.empty());
    worker.pump();
    REQUIRE(done.size() == 50);
    for (int at = 0; at < 50; ++at)
        CHECK(done[static_cast<std::size_t>(at)] == at);
    CHECK(onWorker);
    CHECK(worker.stats().done >= 50);
}

TEST_CASE("data worker > past its bound a job is refused, counted, and nothing of it done")
{
    DataWorker worker(engine::app::makeMemoryDataStore(), 4);
    // The first job holds the thread while the queue fills behind it.
    std::atomic<bool> release{false};
    REQUIRE(worker.post([&](IDataStore&) -> DataWorker::Then {
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return {};
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int queued = 0;
    for (int at = 0; at < 10; ++at)
        queued += worker.post([](IDataStore&) -> DataWorker::Then { return {}; }) ? 1 : 0;
    CHECK(queued == 4);
    CHECK(worker.stats().refused == 6);
    CHECK(worker.stats().queued == 4);
    release = true;
    worker.wait([](IDataStore&) {});
    CHECK(worker.stats().queued == 0);
    // The held job waited and ran: the worst since asked, and then none.
    CHECK(worker.takeWorst() > 10.0);
    CHECK(worker.takeWorst() < 10.0);
}

TEST_CASE("data broker > a record is read on the worker, and the next holder reads what the last one wrote")
{
    engine::app::DataBroker broker(engine::app::makeMemoryDataStore());
    broker.load("town", 1, "ana", 0);
    REQUIRE(broker.write("town", "ana", engine::app::StoredRecord{"level=7", 1.0}, true, 0));
    broker.load("fields", 2, "ana", 0);
    broker.wait([](IDataStore&) {});
    broker.pump();
    const std::vector<engine::app::DataBroker::Answer> answers = broker.takeAnswers();
    REQUIRE(answers.size() == 2);
    CHECK_FALSE(answers[0].record.has_value());
    REQUIRE(answers[1].record.has_value());
    CHECK(answers[1].record->body == "level=7");
}

TEST_CASE("backend > http only to this machine or the local network, https anywhere")
{
    std::string why;
    CHECK(engine::app::backendUrlAllowed("https://data.example.com/game", why));
    CHECK(engine::app::backendUrlAllowed("http://localhost:7860", why));
    CHECK(engine::app::backendUrlAllowed("http://127.0.0.1:7860", why));
    CHECK(engine::app::backendUrlAllowed("http://10.1.2.3", why));
    CHECK(engine::app::backendUrlAllowed("http://172.16.0.9", why));
    CHECK(engine::app::backendUrlAllowed("http://172.31.255.1", why));
    CHECK(engine::app::backendUrlAllowed("http://192.168.0.20:80", why));
    CHECK(engine::app::backendUrlAllowed("http://169.254.10.1", why));
    CHECK(engine::app::backendUrlAllowed("http://[::1]:7860", why));
    CHECK(engine::app::backendUrlAllowed("http://[fe80::1]:7860", why));
    CHECK_FALSE(engine::app::backendUrlAllowed("http://172.32.0.1", why));
    CHECK_FALSE(engine::app::backendUrlAllowed("http://8.8.8.8", why));
    CHECK_FALSE(engine::app::backendUrlAllowed("http://data.example.com", why));
    CHECK(why.find("data.example.com") != std::string::npos);
    CHECK_FALSE(engine::app::backendUrlAllowed("ftp://10.0.0.1", why));
    CHECK_FALSE(engine::app::backendUrlAllowed("not a url", why));
}

TEST_CASE("backend > an answer lost is asked again with the same id; none at all is Unavailable")
{
    std::vector<std::string> sent;
    int lose = 1;
    engine::app::BackendOptions options;
    options.url = "http://127.0.0.1:1/";
    options.tries = 3;
    options.totalMs = 10'000;
    options.send = [&](const std::string& url, const std::string& body, const std::string&,
                       core::u32) -> std::optional<std::string> {
        CHECK(url == "http://127.0.0.1:1/v1/data");
        sent.push_back(body);
        // The first answer is lost on the way back; the backend did it.
        if (lose-- > 0)
            return std::nullopt;
        return std::string(R"({"ok":true,"transaction":41})");
    };
    std::unique_ptr<IDataStore> store = engine::app::makeBackendDataStore(options);
    CHECK_FALSE(store->keepsItsLog());
    const engine::app::DataTransaction done =
        store->transact("town", {engine::app::DataMove{0, "Gold", 5, "", "ana"}});
    CHECK(done.ok);
    CHECK(done.id == 41);
    CHECK_FALSE(store->unavailable());
    // **Twice, the same**: the backend tells the second from a new trade by
    // the id, and does it once.
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == sent[1]);
    core::JsonDocument body;
    REQUIRE(body.parse(sent[0], "sent"));
    CHECK(body.root()["request"].asString().size() == 32);
    CHECK(body.root()["op"].asString() == "transact");

    // A new request, a new id.
    sent.clear();
    (void)store->balance("ana", "Gold");
    core::JsonDocument other;
    REQUIRE(other.parse(sent.at(0), "sent"));
    CHECK(other.root()["request"].asString() != body.root()["request"].asString());

    // **Never answered**: three tries, then the store is away, and what the
    // data service answers says so rather than an empty inventory.
    lose = 1000;
    sent.clear();
    const std::string answer = engine::app::runDataOp(*store, "town", R"({"op":"items","owner":"ana"})");
    CHECK(sent.size() == 3);
    CHECK(answer.find("Unavailable") != std::string::npos);
    CHECK_FALSE(store->readRecord("ana").ok);
}
