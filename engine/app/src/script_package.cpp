#include "engine/app/script_package.h"

#include <algorithm>
#include <span>
#include <vector>

#include "engine/platform/file.h"
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

} // namespace engine::app
