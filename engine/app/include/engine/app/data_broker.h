// **Who holds which record** (ADR 0238 §2): a player's record is held by one
// world at a time. The manager keeps one of these for all its worlds, and a
// server with no manager one for itself; each world's requests arrive by name.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/data_store.h"
#include "engine/app/data_worker.h"
#include "engine/core/types.h"

namespace engine::app {

class DataBroker
{
public:
    // How long a world waits for another to let a record go before it is
    // refused as busy.
    static constexpr core::i64 BusyAfterMs = 10'000;

    // **The store is the worker's** (`DataWorker`): every read and write is
    // done on its thread, and its answers are taken by `pump`.
    explicit DataBroker(std::unique_ptr<IDataStore> store, core::usize most = DataWorker::DefaultMost);

    // **An answer to a world's `load`**: the record, where it was given --
    // none for an account that has none yet -- or `busy` where another world
    // held it past the wait, or `failed` where the store could not be read.
    struct Answer
    {
        std::string world;
        core::u32 id = 0;
        bool ok = false;
        bool busy = false;
        std::optional<StoredRecord> record;
    };

    // `world` asks for `account`'s record: given at once where no other world
    // holds it, else when that world lets it go, or refused as busy after
    // `BusyAfterMs`. A world asking again for a record it holds is given it.
    void load(std::string_view world, core::u32 id, std::string_view account, core::i64 nowMs);
    // A world's write of a record it holds, and whether it lets it go with it.
    // False, and nothing written, where the world does not hold it -- a world
    // that lost it to `worldGone` and came back -- or the queue is full; a
    // write the store then fails is said in the log.
    [[nodiscard]] bool write(std::string_view world, std::string_view account, const StoredRecord& record, bool release,
                             core::i64 nowMs);
    // **A request of `runDataOp`'s** -- a transaction, items, money, a shared
    // value -- done on the store's thread and answered on this one by `done`
    // with its answer's text; false, and nothing done, when the queue is full.
    using Answered = std::function<void(std::string)>;
    [[nodiscard]] bool ask(std::string world, std::string request, Answered done);
    // Anything else, on the store's thread; what it returns runs here.
    [[nodiscard]] bool post(DataWorker::Job job) { return m_worker.post(std::move(job)); }
    // Done before it returns: a console's command, a test.
    void wait(const std::function<void(IDataStore&)>& job) { m_worker.wait(job); }
    // The store's answers since, each on this thread: records given, writes
    // that failed, requests answered.
    void pump() { m_worker.pump(); }
    [[nodiscard]] DataWorker::Stats stats() const { return m_worker.stats(); }
    core::f64 takeWorst() { return m_worker.takeWorst(); }
    // Lets go of every record `world` held: it stopped, or died, and what it
    // had not written is lost.
    void worldGone(std::string_view world, core::i64 nowMs);
    // The waits that ran out, refused.
    void tick(core::i64 nowMs);

    [[nodiscard]] std::vector<Answer> takeAnswers();
    // The world holding `account`'s record, or empty.
    [[nodiscard]] std::string_view holder(std::string_view account) const;
    [[nodiscard]] std::size_t held() const noexcept { return m_holders.size(); }

private:
    void give(std::string world, core::u32 id, std::string account);
    void released(std::string_view account, core::i64 nowMs);

    std::map<std::string, std::string, std::less<>> m_holders;
    struct Waiting
    {
        std::string world;
        core::u32 id = 0;
        std::string account;
        core::i64 deadlineMs = 0;
    };
    std::deque<Waiting> m_waiting;
    std::vector<Answer> m_answers;
    // Last: it stops first, finishing what was asked while the rest is whole.
    DataWorker m_worker;
};

} // namespace engine::app
