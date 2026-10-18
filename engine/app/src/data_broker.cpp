#include "engine/app/data_broker.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/log.h"

namespace engine::app {

DataBroker::DataBroker(std::unique_ptr<IDataStore> store) : m_store(std::move(store))
{}

void DataBroker::give(std::string world, core::u32 id, std::string account)
{
    Answer answer;
    answer.world = world;
    answer.id = id;
    answer.ok = true;
    answer.record = m_store->loadRecord(account);
    m_holders.insert_or_assign(std::move(account), std::move(world));
    m_answers.push_back(std::move(answer));
}

void DataBroker::load(std::string_view world, core::u32 id, std::string_view account, core::i64 nowMs)
{
    const auto holding = m_holders.find(account);
    if (holding == m_holders.end() || holding->second == world) {
        give(std::string{world}, id, std::string{account});
        return;
    }
    m_waiting.push_back(Waiting{std::string{world}, id, std::string{account}, nowMs + BusyAfterMs});
}

bool DataBroker::write(std::string_view world, std::string_view account, const StoredRecord& record, bool release,
                       core::i64 nowMs)
{
    const auto holding = m_holders.find(account);
    if (holding == m_holders.end() || holding->second != world) {
        const std::array<core::I18nArg, 2> args{core::I18nArg{"world", world}, core::I18nArg{"account", account}};
        core::log(core::LogLevel::Warn, ENG_TR("data.warn.write_not_held"), args);
        return false;
    }
    const bool written = m_store->writeRecord(account, record);
    if (!written) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"account", account}};
        core::log(core::LogLevel::Error, ENG_TR("data.err.write_failed"), args);
    }
    if (release) {
        m_holders.erase(holding);
        released(account, nowMs);
    }
    return written;
}

void DataBroker::released(std::string_view account, core::i64 nowMs)
{
    (void)nowMs;
    // **The first world that waited** is given it: in the order they asked.
    const auto next = std::find_if(m_waiting.begin(), m_waiting.end(),
                                   [&](const Waiting& waiting) { return waiting.account == account; });
    if (next == m_waiting.end())
        return;
    Waiting given = std::move(*next);
    m_waiting.erase(next);
    give(std::move(given.world), given.id, std::move(given.account));
}

void DataBroker::worldGone(std::string_view world, core::i64 nowMs)
{
    std::erase_if(m_waiting, [&](const Waiting& waiting) { return waiting.world == world; });
    std::vector<std::string> freed;
    for (auto at = m_holders.begin(); at != m_holders.end();) {
        if (at->second == world) {
            freed.push_back(at->first);
            at = m_holders.erase(at);
        }
        else {
            ++at;
        }
    }
    if (!freed.empty()) {
        const std::array<core::I18nArg, 2> args{core::I18nArg{"world", world},
                                                core::I18nArg{"records", static_cast<core::i64>(freed.size())}};
        core::log(core::LogLevel::Warn, ENG_TR("data.warn.world_gone_holding"), args);
    }
    for (const std::string& account : freed)
        released(account, nowMs);
}

void DataBroker::tick(core::i64 nowMs)
{
    for (auto at = m_waiting.begin(); at != m_waiting.end();) {
        if (at->deadlineMs > nowMs) {
            ++at;
            continue;
        }
        Answer busy;
        busy.world = std::move(at->world);
        busy.id = at->id;
        busy.busy = true;
        m_answers.push_back(std::move(busy));
        at = m_waiting.erase(at);
    }
}

std::vector<DataBroker::Answer> DataBroker::takeAnswers()
{
    std::vector<Answer> taken;
    taken.swap(m_answers);
    return taken;
}

std::string_view DataBroker::holder(std::string_view account) const
{
    const auto holding = m_holders.find(account);
    return holding != m_holders.end() ? std::string_view{holding->second} : std::string_view{};
}

} // namespace engine::app
