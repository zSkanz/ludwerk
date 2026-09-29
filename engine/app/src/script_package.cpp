#include "engine/app/script_package.h"

#include <algorithm>
#include <span>
#include <vector>

#include "engine/core/base64.h"
#include "engine/core/json.h"
#include "engine/platform/file.h"
#include "engine/scene/scene_file.h"
#include "engine/script/bytecode.h"

namespace engine::app {

bool compileGameScripts(const std::filesystem::path& game, ScriptPackageReport& report)
{
    report = ScriptPackageReport{};
    const std::filesystem::path src = game / "src";
    std::error_code ec;
    if (!std::filesystem::is_directory(src, ec))
        return true;

    // Collected, then sorted, then compiled: a directory walk's order is the
    // file system's, and which script a failure names should not be.
    std::vector<std::filesystem::path> scripts;
    for (std::filesystem::recursive_directory_iterator it(src, ec), end; it != end && !ec; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        if (it->is_regular_file(ec) && path.extension() == ".luau" && !path.stem().string().ends_with(".d"))
            scripts.push_back(path);
    }
    std::sort(scripts.begin(), scripts.end());

    for (const std::filesystem::path& path : scripts) {
        const std::string relative = std::filesystem::relative(path, game, ec).generic_string();
        std::vector<std::byte> bytes;
        if (!platform::readFile(path, bytes)) {
            report.failed = relative;
            return false;
        }
        const std::string_view source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::string bytecode;
        if (!script::compileForPackage(source, bytecode, report.message)) {
            report.failed = relative;
            return false;
        }
        std::filesystem::path compiled = path;
        compiled.replace_extension(script::CompiledExtension);
        const std::span<const std::byte> out(reinterpret_cast<const std::byte*>(bytecode.data()), bytecode.size());
        if (!platform::writeFile(compiled, out) || !platform::removeFile(path)) {
            report.failed = relative;
            return false;
        }
        ++report.compiled;
    }
    return true;
}

namespace {

// One past the closing quote of the JSON string literal whose opening quote is
// at `open`, or `npos` for one that does not close.
[[nodiscard]] std::size_t endOfString(std::string_view text, std::size_t open) noexcept
{
    for (std::size_t at = open + 1; at < text.size(); ++at) {
        if (text[at] == '\\')
            ++at;
        else if (text[at] == '"')
            return at + 1;
    }
    return std::string_view::npos;
}

// Whether `name` is a file whose scripts a package compiles.
[[nodiscard]] bool carriesScripts(const std::string& name) noexcept
{
    return name.ends_with(".scene.json") || name.ends_with(".stamp.json") || name == "global.json";
}

} // namespace

bool compileContentScripts(const std::filesystem::path& content, ScriptPackageReport& report)
{
    report = ScriptPackageReport{};
    std::error_code ec;
    if (!std::filesystem::is_directory(content, ec))
        return true;

    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && carriesScripts(it->path().filename().string()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    constexpr std::string_view Key = "\"Source\"";
    for (const std::filesystem::path& path : files) {
        const std::string relative = std::filesystem::relative(path, content, ec).generic_string();
        std::string text;
        if (!platform::readTextFile(path, text)) {
            report.failed = relative;
            return false;
        }
        std::string out;
        out.reserve(text.size());
        std::size_t copied = 0;
        bool changed = false;
        for (std::size_t key = text.find(Key); key != std::string::npos; key = text.find(Key, key + Key.size())) {
            // The key, a colon, and a string: anything else is not a property
            // called `Source` and is left as it is.
            std::size_t at = key + Key.size();
            while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
                ++at;
            if (at >= text.size() || text[at] != ':')
                continue;
            ++at;
            while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
                ++at;
            if (at >= text.size() || text[at] != '"')
                continue;
            const std::size_t close = endOfString(text, at);
            if (close == std::string::npos)
                break;
            core::JsonDocument literal;
            if (!literal.parse(std::string_view(text).substr(at, close - at), relative))
                continue;
            const std::string_view source = literal.root().asString();
            // Nothing to compile, or compiled already.
            if (source.empty() || source.starts_with(scene::CompiledSourcePrefix))
                continue;
            std::string bytecode;
            if (!script::compileForPackage(source, bytecode, report.message)) {
                report.failed = relative;
                return false;
            }
            const std::span<const core::u8> bytes(reinterpret_cast<const core::u8*>(bytecode.data()), bytecode.size());
            out.append(text, copied, at - copied);
            out += '"';
            out += scene::CompiledSourcePrefix;
            out += core::base64Encode(bytes);
            out += '"';
            copied = close;
            changed = true;
            ++report.compiled;
        }
        if (!changed)
            continue;
        out.append(text, copied, std::string::npos);
        // Removed first: a staged file is a hard link to the project's own.
        const std::span<const std::byte> written(reinterpret_cast<const std::byte*>(out.data()), out.size());
        if (!platform::removeFile(path) || !platform::writeFile(path, written)) {
            report.failed = relative;
            return false;
        }
    }
    return true;
}

} // namespace engine::app
