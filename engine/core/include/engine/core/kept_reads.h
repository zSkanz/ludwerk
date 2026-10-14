#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace engine::core {

// **What was loaded before a change notice, kept until a read of the new file
// succeeds** (ADR 0212, section 3).
//
// A notice can come in the middle of its author's save, and the read it starts
// then finds the file short, or one that does not parse. A cache that forgot
// first had nothing to fall back on, and showed the asset as missing until the
// next notice. One that keeps what it had here puts it back after such a read
// and says nothing; the failure is said once, and only when no further notice
// has brought a read that succeeds within the patience -- a file saved whole
// and wrong is not hidden.
//
// Keyed by whatever the cache keys by, kept sorted; used from the cache's own thread alone.
template <class Key, class Value>
class KeptReads
{
public:
    // A notice for `key`, with what was loaded for it: kept, and the wait for
    // a failure to be said starts over.
    void keep(const Key& key, Value value)
    {
        Entry& entry = entryFor(key);
        entry.value = std::move(value);
        entry.held = true;
        entry.failed = false;
        entry.said = false;
    }

    // A notice for `key` with nothing loaded: only the wait starts over.
    void notice(const Key& key)
    {
        if (Entry* entry = find(key); entry != nullptr) {
            entry->failed = false;
            entry->said = false;
        }
    }

    // A read of `key` that failed, and why: the value kept for it, or null
    // when nothing was. The first failure after a notice starts the wait.
    [[nodiscard]] const Value* fallBack(const Key& key, std::string why)
    {
        Entry* entry = find(key);
        if (entry == nullptr || !entry->held)
            return nullptr;
        if (!entry->failed) {
            entry->failed = true;
            entry->at = std::chrono::steady_clock::now();
        }
        entry->why = std::move(why);
        return &entry->value;
    }

    // A read of `key` that succeeded: what was kept is let go.
    void release(const Key& key)
    {
        const auto at = place(key);
        if (at != m_entries.end() && at->key == key)
            m_entries.erase(at);
    }

    // Each failure the patience has run out on, once: its key and why.
    [[nodiscard]] std::vector<std::pair<Key, std::string>> due()
    {
        std::vector<std::pair<Key, std::string>> said;
        if (m_entries.empty())
            return said;
        const auto now = std::chrono::steady_clock::now();
        for (Entry& entry : m_entries) {
            if (!entry.failed || entry.said)
                continue;
            if (std::chrono::duration<double>(now - entry.at).count() < m_patience)
                continue;
            entry.said = true;
            said.emplace_back(entry.key, entry.why);
        }
        return said;
    }

    // How long a failed read waits for another notice before it is said, in
    // seconds. Two unless a test asks for less.
    void setPatience(double seconds) noexcept { m_patience = std::max(seconds, 0.0); }
    void clear() noexcept { m_entries.clear(); }

private:
    struct Entry
    {
        Key key{};
        Value value{};
        bool held = false;
        bool failed = false;
        bool said = false;
        std::string why;
        std::chrono::steady_clock::time_point at{};
    };

    [[nodiscard]] auto place(const Key& key)
    {
        return std::lower_bound(m_entries.begin(), m_entries.end(), key,
                                [](const Entry& entry, const Key& wanted) { return entry.key < wanted; });
    }
    [[nodiscard]] Entry* find(const Key& key)
    {
        const auto at = place(key);
        return at != m_entries.end() && at->key == key ? &*at : nullptr;
    }
    Entry& entryFor(const Key& key)
    {
        const auto at = place(key);
        if (at != m_entries.end() && at->key == key)
            return *at;
        Entry entry;
        entry.key = key;
        return *m_entries.insert(at, std::move(entry));
    }

    std::vector<Entry> m_entries;
    double m_patience = 2.0;
};

} // namespace engine::core
