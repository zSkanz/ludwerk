#pragma once

// The editor's side of an export (ADR 0104 §4-5): `ludwerk build
// --progress=json` run as a worker process, its lines read into steps, and
// what it made remembered.
//
// **The editor does not re-implement the exporter.** It runs the CLI, the same
// pipeline a CI script runs, and draws what it says -- so the button and the
// command make the same bytes. Everything here is plain state and parsing,
// with no ImGui, so a test feeds it lines; `debug_overlay.cpp` draws it.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/types.h"
#include "engine/platform/process.h"

namespace engine::app {

// How to run `ludwerk`: the interpreter, the CLI's entry script, and the
// directory to run it from (a rokit shim resolves the pinned `lute` from it).
struct CliCommand
{
    std::string lute;
    std::filesystem::path script;
    std::filesystem::path workingDirectory;

    // `lute run <script> -- <arguments...>`.
    [[nodiscard]] std::vector<std::string> command(const std::vector<std::string>& arguments) const;
};

// An installation keeps `lute` and `tools/cli/main.luau` beside the editor
// host; a build of this repository runs from a tree the host is not inside, so
// the project's own ancestors are searched for the repository too, and `lute`
// is the one on PATH. Nothing when neither is found.
[[nodiscard]] std::optional<CliCommand> locateCli(const std::filesystem::path& executableDir,
                                                  const std::filesystem::path& projectRoot);

struct ExportStep
{
    enum class State : core::u8
    {
        Running,
        Done,
        Failed,
    };
    std::string name;
    State state = State::Running;
    double ms = 0.0;
    std::string message;
};

struct ExportResult
{
    std::string target;
    std::string folder;
    std::string executable;
    std::string archive;
    std::string apk;
    std::string package;
};

struct TargetStatus
{
    std::string name;
    bool server = false;
    bool ready = false;
    bool player = false;
    bool tools = false;
};

// One export, from start to its result.
class ExportRun
{
public:
    // Applies one line of `--progress=json` output. A line that is not one of
    // its JSON shapes is log, kept for **Show log**.
    void applyLine(std::string_view line);
    // Feeds raw output, which may end mid-line.
    void feed(std::string_view text);

    [[nodiscard]] const std::vector<ExportStep>& steps() const noexcept { return m_steps; }
    [[nodiscard]] const std::optional<ExportResult>& result() const noexcept { return m_result; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return m_notes; }
    [[nodiscard]] const std::string& log() const noexcept { return m_log; }
    // The first failed step, if any.
    [[nodiscard]] const ExportStep* failure() const noexcept;

private:
    std::vector<ExportStep> m_steps;
    std::optional<ExportResult> m_result;
    std::vector<std::string> m_notes;
    std::string m_log;
    std::string m_partial;
};

// `ludwerk build --status`'s one line, or nothing when it is not that shape.
struct ExportStatus
{
    std::vector<TargetStatus> targets;
    std::string adb;
};
[[nodiscard]] std::optional<ExportStatus> parseExportStatus(std::string_view output);

// `adb devices -l`: the model of the first device attached, e.g. "SM-S938B".
// Empty when none is, or when the one attached is unauthorised.
[[nodiscard]] std::string parseAdbDevice(std::string_view output);

// What an export made, remembered per project in the per-user editor state.
struct RecentExport
{
    std::string target;
    std::string version;
    std::string when;
    core::u64 bytes = 0;
    std::string folder;
};

// The last ten for `projectRoot`, newest first.
[[nodiscard]] std::vector<RecentExport> loadRecentExports(const std::filesystem::path& file,
                                                          const std::filesystem::path& projectRoot);
// Puts `entry` first for `projectRoot`, keeps ten, and writes the file.
bool rememberExport(const std::filesystem::path& file, const std::filesystem::path& projectRoot,
                    const RecentExport& entry);
// `<userDir>/exports.json`, or empty when there is no per-user folder.
[[nodiscard]] std::filesystem::path recentExportsFile();

// Every byte under `path`: a file's size, or a folder's contents.
[[nodiscard]] core::u64 sizeOnDisk(const std::filesystem::path& path);

} // namespace engine::app
