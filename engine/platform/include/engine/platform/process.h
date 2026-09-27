#pragma once

// Running a tool and reading what it said (ADR 0091).
//
// The editor compiles a user's surface shader by running `shadercross`, the
// same program the engine's own build runs, rather than by linking a compiler
// into itself: a compiler that crashes on somebody's shader takes a process of
// its own down, not the editor, and the editor starts on a machine where the
// compiler's library is missing and says so when asked to compile.
//
// Blocking by design -- the caller is a worker thread -- and never on the
// render thread.

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::platform {

struct ProcessResult
{
    // False when the program could not be started at all.
    bool started = false;
    int exitCode = -1;
    // Standard output and standard error, interleaved as the program wrote them.
    std::string output;
};

// Where a program runs, and what it finds in its environment beyond this
// process's own. The editor's Export window runs `ludwerk` from the directory
// its pinned interpreter resolves from, and hands a keystore password over in
// the environment -- never on a command line, which every other process on the
// machine can read (ADR 0104 §3).
struct ProcessOptions
{
    std::filesystem::path workingDirectory;
    std::vector<std::pair<std::string, std::string>> environment;
};

[[nodiscard]] ProcessResult runProcess(const std::vector<std::string>& arguments);
[[nodiscard]] ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options);

// **A program that runs beside this one, and whose output is read while it
// does** (ADR 0106 §5: the editor's Play with players starts a server and its
// clients this way, and shows what each says).
//
// Non-blocking throughout: `readAvailable` answers what arrived since the last
// call, possibly nothing, and never waits. The child is killed when this is
// destroyed, so a session that ends takes its processes with it -- an editor
// that exited and left four games running would be the defect.
class ChildProcess
{
public:
    using Options = ProcessOptions;

    // Null when the program could not be started.
    [[nodiscard]] static std::unique_ptr<ChildProcess> start(const std::vector<std::string>& arguments);
    [[nodiscard]] static std::unique_ptr<ChildProcess> start(const std::vector<std::string>& arguments,
                                                             const ProcessOptions& options);
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Standard output and standard error, interleaved, as far as they arrived.
    [[nodiscard]] std::string readAvailable();
    // Whether it is still running; a child that exited answers false, and its
    // exit code is `exitCode` from then on.
    [[nodiscard]] bool running();
    [[nodiscard]] int exitCode() const noexcept { return m_exitCode; }
    // Ends it now, if it is running.
    void kill();

private:
    explicit ChildProcess(void* process) noexcept : m_process(process) {}

    void* m_process = nullptr;
    int m_exitCode = -1;
    bool m_exited = false;
};

} // namespace engine::platform
