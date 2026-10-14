// **The editor watches the project's files** (the file-watching batch): a
// file changed outside it -- by VS Code, by an agent, by a sync tool -- is
// noticed while the editor is open, as `ludwerk dev` notices it.
//
// A look at the disk, not the system's change notifications: one walk of
// `src/`, `content/` and `i18n/` comparing each file's time and size with the
// last walk's, which is what works the same on every platform and on a
// network folder, and costs a few milliseconds for a project of thousands of
// files every half second. **A file is said to have changed once it has
// stopped changing**: seen different from the last look and the same at the
// next, so a file still being written -- an editor's save, a copy -- is read
// whole, never half.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

enum class FileChangeKind : core::u8
{
    Added,
    Modified,
    Removed,
};

struct FileChange
{
    // Project-relative, '/' separators.
    std::string path;
    FileChangeKind kind = FileChangeKind::Modified;

    [[nodiscard]] bool operator==(const FileChange&) const = default;
};

class ProjectWatcher
{
public:
    // `folders` are project-relative; one that does not exist is watched for
    // appearing.
    ProjectWatcher(std::filesystem::path root, std::vector<std::string> folders);

    // What has changed and settled since the last look, in path order. The
    // first look reports nothing: it is what the project was when it opened.
    [[nodiscard]] std::vector<FileChange> poll();

    // **What the editor itself wrote is not news**: the file as it is now is
    // taken as seen, so a save does not come back as a change from outside.
    void noteWritten(const std::string& path);

private:
    struct Seen
    {
        std::filesystem::file_time_type written{};
        std::uintmax_t size = 0;
    };
    [[nodiscard]] std::map<std::string, Seen> look() const;

    std::filesystem::path m_root;
    std::vector<std::string> m_folders;
    bool m_primed = false;
    // What the last report was made from, and what the look before this one
    // saw -- a change is reported when the two last looks agree with each
    // other and not with what was reported.
    std::map<std::string, Seen> m_reported;
    std::map<std::string, Seen> m_previous;
};

// **The watcher on a thread of its own.** A walk of a large project is
// milliseconds -- 1.5 ms for the 358 files of one game on this machine, 3.7 ms
// for the 1,203 of another -- which the editor's frame does not pay: the
// thread walks every `every`, and the frame takes what has settled since it
// last asked, which is a lock and a swap.
class ProjectWatcherThread
{
public:
    ProjectWatcherThread(std::filesystem::path root, std::vector<std::string> folders,
                         std::chrono::milliseconds every = std::chrono::milliseconds(500));
    ~ProjectWatcherThread();
    ProjectWatcherThread(const ProjectWatcherThread&) = delete;
    ProjectWatcherThread& operator=(const ProjectWatcherThread&) = delete;

    // What has settled since the last call, one change a path -- the latest --
    // in path order.
    [[nodiscard]] std::vector<FileChange> take();

private:
    void run();

    ProjectWatcher m_watcher;
    std::chrono::milliseconds m_every;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    std::vector<FileChange> m_ready;
    std::thread m_thread;
};

// Whether a path is one the watcher leaves alone: the engine's own folder,
// an editor's temporary file, a hidden file.
[[nodiscard]] bool watcherIgnores(std::string_view path) noexcept;

} // namespace engine::app
