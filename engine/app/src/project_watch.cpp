#include "engine/app/project_watch.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace engine::app {

bool watcherIgnores(std::string_view path) noexcept
{
    // The engine's own folder, wherever it is, and any hidden folder or file.
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? path.npos : slash - start);
        if (part.starts_with('.'))
            return true;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    // What editors write beside a file while saving it.
    return path.ends_with('~') || path.ends_with(".tmp") || path.ends_with(".swp") || path.ends_with(".swx") ||
           path.ends_with(".crswap");
}

ProjectWatcher::ProjectWatcher(std::filesystem::path root, std::vector<std::string> folders)
    : m_root(std::move(root)), m_folders(std::move(folders))
{}

std::map<std::string, ProjectWatcher::Seen> ProjectWatcher::look() const
{
    std::map<std::string, Seen> seen;
    std::error_code ec;
    for (const std::string& folder : m_folders) {
        const std::filesystem::path base = m_root / std::filesystem::path(folder);
        if (!std::filesystem::is_directory(base, ec))
            continue;
        for (std::filesystem::recursive_directory_iterator
                 it(base, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec; it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc))
                continue;
            // Lexical, not `relative`: the walk's paths are built on the root,
            // and `relative` canonicalises both sides with several system
            // calls a file -- most of a pass's cost on a large project.
            const std::string relative = it->path().lexically_relative(m_root).generic_string();
            if (fileEc || relative.empty() || watcherIgnores(relative))
                continue;
            Seen one;
            one.written = it->last_write_time(fileEc);
            one.size = it->file_size(fileEc);
            if (!fileEc)
                seen.emplace(relative, one);
        }
        ec.clear();
    }
    return seen;
}

std::vector<FileChange> ProjectWatcher::poll()
{
    std::map<std::string, Seen> now = look();
    std::vector<FileChange> changes;
    if (!m_primed) {
        m_primed = true;
        m_reported = now;
        m_previous = std::move(now);
        return changes;
    }
    const auto same = [](const Seen& a, const Seen& b) { return a.written == b.written && a.size == b.size; };
    // Settled: what this look saw is what the last look saw too.
    for (const auto& [path, seen] : now) {
        const auto before = m_previous.find(path);
        if (before == m_previous.end() || !same(before->second, seen))
            continue;
        const auto reported = m_reported.find(path);
        if (reported == m_reported.end()) {
            changes.push_back(FileChange{path, FileChangeKind::Added});
            m_reported.emplace(path, seen);
        }
        else if (!same(reported->second, seen)) {
            changes.push_back(FileChange{path, FileChangeKind::Modified});
            reported->second = seen;
        }
    }
    // Gone at two looks in a row: a save that replaces a file is not a removal.
    for (auto it = m_reported.begin(); it != m_reported.end();) {
        if (!now.contains(it->first) && !m_previous.contains(it->first)) {
            changes.push_back(FileChange{it->first, FileChangeKind::Removed});
            it = m_reported.erase(it);
        }
        else {
            ++it;
        }
    }
    std::sort(changes.begin(), changes.end(), [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    m_previous = std::move(now);
    return changes;
}

void ProjectWatcher::noteWritten(const std::string& path)
{
    std::error_code ec;
    const std::filesystem::path file = m_root / std::filesystem::path(path);
    if (!std::filesystem::is_regular_file(file, ec)) {
        m_reported.erase(path);
        m_previous.erase(path);
        return;
    }
    Seen seen;
    seen.written = std::filesystem::last_write_time(file, ec);
    seen.size = std::filesystem::file_size(file, ec);
    m_reported[path] = seen;
    m_previous[path] = seen;
}

ProjectWatcherThread::ProjectWatcherThread(std::filesystem::path root, std::vector<std::string> folders,
                                           std::chrono::milliseconds every)
    : m_watcher(std::move(root), std::move(folders)), m_every(every)
{
    // **The first look here, not on the thread**: it is what every later
    // change is a change from, and taken on the thread it came whenever the
    // thread first ran -- on a busy machine after a file had already changed,
    // which then was where the project began, and the change was never told.
    (void)m_watcher.poll();
    m_thread = std::thread([this] { run(); });
}

ProjectWatcherThread::~ProjectWatcherThread()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<FileChange> ProjectWatcherThread::take()
{
    const std::lock_guard lock(m_mutex);
    std::vector<FileChange> out;
    out.swap(m_ready);
    return out;
}

void ProjectWatcherThread::run()
{
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            if (m_wake.wait_for(lock, m_every, [this] { return m_stop; }))
                return;
        }
        std::vector<FileChange> changes = m_watcher.poll();
        if (changes.empty())
            continue;
        const std::lock_guard lock(m_mutex);
        for (FileChange& change : changes) {
            std::erase_if(m_ready, [&change](const FileChange& held) { return held.path == change.path; });
            m_ready.push_back(std::move(change));
        }
        std::sort(m_ready.begin(), m_ready.end(),
                  [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    }
}

} // namespace engine::app
