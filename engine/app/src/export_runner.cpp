#include "engine/app/export_runner.h"

#include <algorithm>

#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

namespace engine::app {
namespace {

constexpr std::size_t kRecentPerProject = 10;

#if defined(_WIN32)
[[nodiscard]] std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return text;
}
#endif

// A project's key in the recent list: its absolute path, compared without
// case on Windows, where two spellings of one folder are one folder.
[[nodiscard]] std::string projectKey(const std::filesystem::path& root)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(root, ec);
    std::string text = (ec ? root : absolute).generic_string();
#if defined(_WIN32)
    text = lower(std::move(text));
#endif
    return text;
}

[[nodiscard]] std::string stringField(const core::JsonValue& value, std::string_view key)
{
    return std::string(value[key].asString());
}

} // namespace

std::vector<std::string> CliCommand::command(const std::vector<std::string>& arguments) const
{
    std::vector<std::string> out{lute, "run", script.string(), "--"};
    out.insert(out.end(), arguments.begin(), arguments.end());
    return out;
}

std::optional<CliCommand> locateCli(const std::filesystem::path& executableDir, const std::filesystem::path& repository)
{
    std::error_code ec;
#if defined(_WIN32)
    constexpr const char* kLute = "lute.exe";
#else
    constexpr const char* kLute = "lute";
#endif
    // An installation: the interpreter and the CLI beside the editor host.
    const std::filesystem::path installed = executableDir / "tools" / "cli" / "main.luau";
    if (std::filesystem::exists(installed, ec) && std::filesystem::exists(executableDir / kLute, ec))
        return CliCommand{(executableDir / kLute).string(), installed, executableDir};

    // **The repository this build came from, by name** (audit T1): its CLI,
    // with the `lute` on PATH, which rokit resolves from there. Never a folder
    // above the project -- a project is somebody's download, and a
    // `tools/cli/main.luau` in it would be run unsandboxed the moment the Export
    // window opened.
    if (!repository.empty()) {
        const std::filesystem::path script = repository / "tools" / "cli" / "main.luau";
        if (std::filesystem::exists(script, ec))
            return CliCommand{"lute", script, repository};
    }

    // Or the host's own ancestors: a build laid out inside its repository.
    for (const std::filesystem::path& start : {executableDir}) {
        std::filesystem::path current = std::filesystem::weakly_canonical(start, ec);
        for (int depth = 0; depth < 12 && !current.empty(); ++depth) {
            const std::filesystem::path script = current / "tools" / "cli" / "main.luau";
            if (std::filesystem::exists(script, ec))
                return CliCommand{"lute", script, current};
            const std::filesystem::path parent = current.parent_path();
            if (parent == current)
                break;
            current = parent;
        }
    }
    return std::nullopt;
}

const ExportStep* ExportRun::failure() const noexcept
{
    for (const ExportStep& step : m_steps) {
        if (step.state == ExportStep::State::Failed)
            return &step;
    }
    return nullptr;
}

void ExportRun::feed(std::string_view text)
{
    m_partial.append(text);
    std::size_t start = 0;
    for (std::size_t newline = m_partial.find('\n'); newline != std::string::npos;
         newline = m_partial.find('\n', start)) {
        std::string_view line(m_partial.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        applyLine(line);
        start = newline + 1;
    }
    m_partial.erase(0, start);
}

void ExportRun::applyLine(std::string_view line)
{
    m_log.append(line).append("\n");
    if (line.empty() || line.front() != '{')
        return;
    core::JsonDocument document;
    if (!document.parse(line).ok || document.root().type() != core::JsonType::Object)
        return;
    const core::JsonValue root = document.root();

    if (const core::JsonValue step = root["step"]; step.type() == core::JsonType::String) {
        const std::string name(step.asString());
        const std::string_view state = root["state"].asString();
        auto found = std::find_if(m_steps.begin(), m_steps.end(),
                                  [&](const ExportStep& existing) { return existing.name == name; });
        if (found == m_steps.end()) {
            m_steps.push_back(ExportStep{name, ExportStep::State::Running, 0.0, {}});
            found = m_steps.end() - 1;
        }
        if (state == "done")
            found->state = ExportStep::State::Done;
        else if (state == "fail")
            found->state = ExportStep::State::Failed;
        else
            found->state = ExportStep::State::Running;
        found->ms = root["ms"].asNumber(found->ms);
        found->message = stringField(root, "message");
        return;
    }
    if (const core::JsonValue note = root["note"]; note.type() == core::JsonType::String) {
        m_notes.emplace_back(note.asString());
        return;
    }
    if (const core::JsonValue made = root["result"]; made.type() == core::JsonType::Object) {
        ExportResult result;
        result.target = stringField(made, "target");
        result.folder = stringField(made, "folder");
        result.executable = stringField(made, "executable");
        result.archive = stringField(made, "archive");
        result.apk = stringField(made, "apk");
        result.package = stringField(made, "package");
        m_result = std::move(result);
    }
}

std::optional<ExportStatus> parseExportStatus(std::string_view output)
{
    // The last JSON line: anything before it is the interpreter's own noise.
    std::size_t end = output.size();
    while (end > 0) {
        const std::size_t start = output.rfind('\n', end - 1);
        const std::size_t from = start == std::string_view::npos ? 0 : start + 1;
        std::string_view line = output.substr(from, end - from);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (!line.empty() && line.front() == '{') {
            core::JsonDocument document;
            if (!document.parse(line).ok)
                return std::nullopt;
            const core::JsonValue root = document.root();
            const core::JsonValue targets = root["targets"];
            if (targets.type() != core::JsonType::Array)
                return std::nullopt;
            ExportStatus status;
            for (std::size_t index = 0; index < targets.size(); ++index) {
                const core::JsonValue row = targets.at(index);
                status.targets.push_back(TargetStatus{stringField(row, "name"), row["server"].asBool(),
                                                      row["ready"].asBool(), row["player"].asBool(),
                                                      row["tools"].asBool()});
            }
            status.adb = stringField(root, "adb");
            return status;
        }
        if (start == std::string_view::npos)
            break;
        end = start;
    }
    return std::nullopt;
}

std::string parseAdbDevice(std::string_view output)
{
    // `List of devices attached`, then `<serial>  device product:x model:SM_S938B device:y ...`.
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos)
            end = output.size();
        const std::string_view line = output.substr(start, end - start);
        start = end + 1;
        if (line.find(" device ") == std::string_view::npos && !line.ends_with(" device") &&
            line.find("\tdevice") == std::string_view::npos)
            continue;
        const std::size_t model = line.find("model:");
        if (model == std::string_view::npos)
            return core::tr(ENG_TR("engine.editor.export_window.android_device"));
        std::string name(line.substr(model + 6, line.find(' ', model) - (model + 6)));
        std::replace(name.begin(), name.end(), '_', ' ');
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        return name;
    }
    return {};
}

std::filesystem::path recentExportsFile()
{
    const std::filesystem::path& userDir = platform::paths().userDir;
    return userDir.empty() ? std::filesystem::path{} : userDir / "exports.json";
}

std::vector<RecentExport> loadRecentExports(const std::filesystem::path& file, const std::filesystem::path& projectRoot)
{
    std::vector<RecentExport> out;
    std::string text;
    if (file.empty() || !platform::readTextFile(file, text))
        return out;
    core::JsonDocument document;
    if (!document.parse(text).ok)
        return out;
    const core::JsonValue list = document.root()["projects"][projectKey(projectRoot)];
    for (std::size_t index = 0; index < list.size() && out.size() < kRecentPerProject; ++index) {
        const core::JsonValue row = list.at(index);
        out.push_back(RecentExport{stringField(row, "target"), stringField(row, "version"), stringField(row, "when"),
                                   static_cast<core::u64>(row["bytes"].asNumber()), stringField(row, "folder")});
    }
    return out;
}

bool rememberExport(const std::filesystem::path& file, const std::filesystem::path& projectRoot,
                    const RecentExport& entry)
{
    if (file.empty())
        return false;
    // Every project's list, re-read so another editor's writes are kept.
    std::string text;
    core::JsonDocument document;
    const bool read = platform::readTextFile(file, text) && document.parse(text).ok;
    const std::string key = projectKey(projectRoot);

    core::JsonWriter writer;
    writer.beginObject();
    writer.key("projects");
    writer.beginObject();
    const auto writeRow = [&](const RecentExport& row) {
        writer.beginObject();
        writer.field("target", std::string_view(row.target));
        writer.field("version", std::string_view(row.version));
        writer.field("when", std::string_view(row.when));
        writer.field("bytes", static_cast<core::f64>(row.bytes));
        writer.field("folder", std::string_view(row.folder));
        writer.endObject();
    };
    if (read) {
        const core::JsonValue projects = document.root()["projects"];
        for (std::size_t index = 0; index < projects.size(); ++index) {
            const std::string_view other = projects.keyAt(index);
            if (other == key)
                continue;
            writer.key(other);
            writer.beginArray();
            for (const RecentExport& row : loadRecentExports(file, std::filesystem::path(std::string(other))))
                writeRow(row);
            writer.endArray();
        }
    }
    std::vector<RecentExport> mine = loadRecentExports(file, projectRoot);
    mine.insert(mine.begin(), entry);
    if (mine.size() > kRecentPerProject)
        mine.resize(kRecentPerProject);
    writer.key(key);
    writer.beginArray();
    for (const RecentExport& row : mine)
        writeRow(row);
    writer.endArray();
    writer.endObject();
    writer.endObject();
    (void)platform::createDirectories(file.parent_path());
    return platform::writeTextFile(file, writer.text());
}

core::u64 sizeOnDisk(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        return static_cast<core::u64>(std::filesystem::file_size(path, ec));
    core::u64 total = 0;
    for (std::filesystem::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec))
            total += static_cast<core::u64>(it->file_size(ec));
    }
    return total;
}

} // namespace engine::app
