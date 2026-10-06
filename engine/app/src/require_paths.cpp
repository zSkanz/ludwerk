// What a string handed to `require` names. See require_paths.h.
#include "engine/app/require_paths.h"

#include <optional>

#include "engine/core/content_path.h"

namespace engine::app {

std::string_view requireDirectoryOf(std::string_view path)
{
    const std::string::size_type slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

bool resolveRequire(std::string_view fromPath, std::string_view specifier, const RequireAliases& aliases,
                    const std::function<bool(const std::string&)>& present, std::string& outPath)
{
    if (specifier.empty())
        return false;

    std::string candidate;
    if (specifier.front() == '@') {
        // `@self` is the requiring file's own directory, and an alias is
        // whatever `.luaurc` said. Resolved here rather than through
        // `Luau::parseConfig` because that treats an unrecognised key as a
        // hard error that aborts the require (U-42) -- a `$schema` line
        // would break `require` at runtime.
        const std::string::size_type slash = specifier.find('/');
        const std::string_view head = specifier.substr(1, slash == std::string_view::npos ? slash : slash - 1);
        const std::string_view tail =
            slash == std::string_view::npos ? std::string_view{} : specifier.substr(slash + 1);

        if (head == "self") {
            candidate.assign(requireDirectoryOf(fromPath));
        }
        else {
            const auto alias = aliases.find(std::string(head));
            if (alias == aliases.end())
                return false;
            candidate = alias->second;
        }

        if (!tail.empty()) {
            if (!candidate.empty())
                candidate.push_back('/');
            candidate.append(tail);
        }
    }
    else if (specifier.starts_with("./") || specifier.starts_with("../")) {
        candidate.assign(requireDirectoryOf(fromPath));
        if (!candidate.empty())
            candidate.push_back('/');
        candidate.append(specifier);
    }
    else {
        // A bare specifier is project-root relative. Deliberately not a
        // search path: one place to look means one answer, and an ambiguity
        // a search path would resolve silently is a bug worth an error.
        candidate.assign(specifier);
    }

    // Resolves `.` and `..` without touching the filesystem, so a specifier
    // cannot escape the project root by spelling enough `..`s and so the
    // answer does not depend on what happens to exist -- by the one check
    // every path from outside the engine goes through (audit F5): a
    // backslash, a drive or a share is not a module name.
    const std::optional<std::string> safe = core::safeRelativePath(candidate);
    if (!safe.has_value())
        return false;
    const std::string& normalised = *safe;

    // The extension is added rather than required, and `init.luau` is the
    // directory form. Each is tried in a fixed order so the answer never
    // depends on which file was created first.
    const std::string withExtension = normalised.ends_with(".luau") ? normalised : normalised + ".luau";
    if (present(withExtension)) {
        outPath = withExtension;
        return true;
    }
    // A module outside `src/shared` says so in its name (`Tool.module.luau`)
    // and is required as `Tool` all the same.
    if (!normalised.ends_with(".luau")) {
        const std::string asModule = normalised + ".module.luau";
        if (present(asModule)) {
            outPath = asModule;
            return true;
        }
    }

    const std::string asDirectory = normalised + "/init.luau";
    if (present(asDirectory)) {
        outPath = asDirectory;
        return true;
    }
    return false;
}

} // namespace engine::app
