#include "engine/app/script_files.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "engine/app/world_host.h"
#include "engine/core/i18n.h"
#include "engine/platform/file.h"
#include "engine/scene/world.h"
#include "engine/script/modules.h"

namespace engine::app {
namespace {

// One service's file tree: the node, the folder it is written to, the folders
// it may already have been read from, and whether a plain file there is a
// module.
struct Container
{
    core::InstanceId node;
    std::string root;
    std::vector<std::string> accepted;
    bool moduleByDefault = false;
};

// A script in a file tree: where it is, what it is, what it is called.
struct Candidate
{
    core::InstanceId id;
    const Container* container = nullptr;
    std::vector<core::InstanceId> folders;
    bool module = false;
    // Where it is now, if a file holds it.
    std::string current;
    // Where it belongs: filled once its file is decided.
    std::string target;
};

[[nodiscard]] std::string_view classNameOf(const scene::World& world, core::InstanceId id)
{
    const scene::ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    return descriptor != nullptr ? world.atoms().text(descriptor->name) : std::string_view{};
}

// **A name a file system takes** (every one this engine runs on): the
// characters Windows refuses become `_`, a trailing dot or space goes, a name
// Windows reserves gets `_` after it, and a name ending in what a file's name
// uses to say its class (`.module`, `.script`) has that dot made `_`.
[[nodiscard]] std::string fileSafe(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool refused = byte < 0x20u || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
                             c == '|' || c == '?' || c == '*';
        out.push_back(refused ? '_' : c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    if (out.empty())
        out = "Script";
    std::string upper;
    for (const char c : out.substr(0, out.find('.')))
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    static constexpr std::string_view Reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (std::find(std::begin(Reserved), std::end(Reserved), upper) != std::end(Reserved))
        out += "_";
    for (const std::string_view kind : {std::string_view{".module"}, std::string_view{".script"}}) {
        if (out.size() > kind.size() && out.ends_with(kind))
            out[out.size() - kind.size()] = '_';
    }
    return out;
}

// Case folded: two names Windows and macOS think are one file are one claim.
[[nodiscard]] std::string claimKey(std::string_view path)
{
    std::string out(path);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] bool under(std::string_view path, std::string_view root)
{
    return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
}

// Where one script's file belongs under `root`: its folders, its name and, when
// its class is not what the folder makes a plain file, the suffix that says so.
[[nodiscard]] std::string placeOf(const scene::World& world, const Candidate& candidate, std::string_view root,
                                  std::string_view base)
{
    std::string path(root);
    for (const core::InstanceId folder : candidate.folders)
        path += "/" + fileSafe(world.atoms().text(world.name(folder)));
    path += "/";
    path += base;
    if (candidate.module != candidate.container->moduleByDefault)
        path += candidate.module ? ".module" : ".script";
    path += ".luau";
    return path;
}

[[nodiscard]] std::string trashStamp()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    (void)localtime_s(&local, &now);
#else
    (void)localtime_r(&now, &local);
#endif
    char text[32]{};
    (void)std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

} // namespace

std::string ScriptFileSync::summary() const
{
    if (!changedAnything() && problems.empty())
        return {};
    std::string out;
    // Each part is a sentence of its own with its count in it; how two are
    // joined is the catalog's too.
    const auto join = [&out](core::TextKey how, const std::string& next) {
        out = out.empty() ? next : core::tr(how, {{"status", out}, {"note", next}});
    };
    const auto part = [&](std::size_t count, core::TextKey what, std::string_view where = {}) {
        if (count == 0)
            return;
        join(ENG_TR("engine.editor.status.with_item"),
             core::tr(what, {{"count", static_cast<core::i64>(count)}, {"where", where}}));
    };
    part(written.size(), ENG_TR("engine.editor.scripts.saved.written"));
    part(moved.size(), ENG_TR("engine.editor.scripts.saved.moved"));
    part(trashed.size(), ENG_TR("engine.editor.scripts.saved.trashed"),
         trash.empty() ? std::string_view(".engine/trash") : std::string_view(trash));
    part(renamed.size(), ENG_TR("engine.editor.scripts.saved.renamed"));
    part(updated.size(), ENG_TR("engine.editor.scripts.saved.updated"));
    part(conflicts.size(), ENG_TR("engine.editor.scripts.saved.conflicts"));
    for (const std::string& problem : problems)
        join(ENG_TR("engine.editor.status.with_clause"), problem);
    return out;
}

ScriptFileSync syncScriptFiles(WorldHost& host, std::string_view sceneName)
{
    ScriptFileSync report;
    const std::filesystem::path projectRoot = host.projectRoot();
    lua_State* L = host.runtime().state();
    if (projectRoot.empty() || L == nullptr)
        return report;
    scene::World& world = host.world();
    const core::InstanceId dataModel = host.runtime().dataModel();

    // --- The file trees ------------------------------------------------------
    std::vector<Container> containers;
    for (core::InstanceId service = world.firstChild(dataModel); service.valid();
         service = world.nextSibling(service)) {
        const std::string_view className = classNameOf(world, service);
        if (className == "GlobalScriptService") {
            for (core::InstanceId folder = world.firstChild(service); folder.valid();
                 folder = world.nextSibling(folder)) {
                if (!world.fixed(folder))
                    continue;
                const std::string_view name = world.atoms().text(world.name(folder));
                if (name == "Client")
                    containers.push_back(Container{folder, "src/client", {"src/client", "src/scripts"}, false});
                else if (name == "Server")
                    containers.push_back(Container{folder, "src/server", {"src/server"}, false});
                else if (name == "Shared")
                    containers.push_back(Container{folder, "src/shared", {"src/shared"}, true});
            }
        }
        else if (!sceneName.empty() && (className == "ServerScriptService" || className == "ClientScriptService")) {
            const std::string root =
                "src/scenes/" + std::string(sceneName) + (className == "ServerScriptService" ? "/server" : "/client");
            containers.push_back(Container{service, root, {root}, false});
        }
    }
    std::vector<std::string> roots;
    for (const Container& container : containers)
        roots.insert(roots.end(), container.accepted.begin(), container.accepted.end());
    const auto inRoots = [&](std::string_view path) {
        return std::any_of(roots.begin(), roots.end(), [&](const std::string& root) { return under(path, root); });
    };

    // --- Every script in them, in document order -------------------------------
    std::map<core::InstanceId, std::string, bool (*)(core::InstanceId, core::InstanceId)> currentOf(
        [](core::InstanceId a, core::InstanceId b) {
            return a.index != b.index ? a.index < b.index : a.generation < b.generation;
        });
    for (const script::ModuleRegistry::Entry& entry : script::mountedEntries(L))
        currentOf[entry.instance] = entry.path;

    std::vector<Candidate> candidates;
    for (const Container& container : containers) {
        std::vector<std::pair<core::InstanceId, std::vector<core::InstanceId>>> stack;
        std::vector<core::InstanceId> children;
        for (core::InstanceId child = world.firstChild(container.node); child.valid(); child = world.nextSibling(child))
            children.push_back(child);
        for (auto at = children.rbegin(); at != children.rend(); ++at)
            stack.emplace_back(*at, std::vector<core::InstanceId>{});
        while (!stack.empty()) {
            auto [id, folders] = std::move(stack.back());
            stack.pop_back();
            const std::string_view className = classNameOf(world, id);
            if (className == "Script" || className == "ModuleScript") {
                Candidate candidate;
                candidate.id = id;
                candidate.container = &container;
                candidate.folders = folders;
                candidate.module = className == "ModuleScript";
                if (const auto found = currentOf.find(id); found != currentOf.end())
                    candidate.current = found->second;
                candidates.push_back(std::move(candidate));
                // What is inside a script is the scene's, as a mark's children
                // are (ADR 0092): not a file of its own.
                continue;
            }
            if (className != "Folder")
                continue;
            std::vector<core::InstanceId> inner = folders;
            inner.push_back(id);
            std::vector<core::InstanceId> nested;
            for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child))
                nested.push_back(child);
            for (auto at = nested.rbegin(); at != nested.rend(); ++at)
                stack.emplace_back(*at, inner);
        }
    }

    // **A name the file system refuses is changed in the tree too**, folders
    // and scripts alike, so the tree and the folder say the same thing and the
    // next open finds what this one saw.
    for (const Candidate& candidate : candidates) {
        for (const core::InstanceId folder : candidate.folders) {
            const std::string_view name = world.atoms().text(world.name(folder));
            if (const std::string safe = fileSafe(name); safe != name) {
                world.setName(folder, world.atoms().intern(safe));
                report.renamed.push_back(folder);
            }
        }
        const std::string_view name = world.atoms().text(world.name(candidate.id));
        if (const std::string safe = fileSafe(name); safe != name) {
            world.setName(candidate.id, world.atoms().intern(safe));
            report.renamed.push_back(candidate.id);
        }
    }

    // --- Where each belongs --------------------------------------------------
    std::map<std::string, core::InstanceId> claims;
    std::vector<Candidate*> placing;
    std::vector<Candidate*> kept;
    for (Candidate& candidate : candidates) {
        // Read from wherever it is now, if that is one of its tree's folders:
        // a project still in `src/scripts` stays there.
        std::string root = candidate.container->root;
        for (const std::string& accepted : candidate.container->accepted) {
            if (under(candidate.current, accepted))
                root = accepted;
        }
        const std::string wanted =
            placeOf(world, candidate, root, fileSafe(world.atoms().text(world.name(candidate.id))));
        if (!candidate.current.empty() && candidate.current == wanted && !claims.contains(claimKey(wanted))) {
            candidate.target = wanted;
            claims[claimKey(wanted)] = candidate.id;
            kept.push_back(&candidate);
        }
        else {
            placing.push_back(&candidate);
        }
    }

    // **What gives its file up**: a script deleted, or moved out of the file
    // trees, and every script about to move. Trashed first, so a script put
    // where another was can take its name rather than a number after it.
    std::set<std::string> staying;
    for (const Candidate& candidate : candidates) {
        if (!candidate.target.empty())
            staying.insert(candidate.target);
    }
    std::vector<std::string> vacating;
    for (const script::ModuleRegistry::Entry& entry : script::mountedEntries(L)) {
        if (!inRoots(entry.path) || staying.contains(entry.path))
            continue;
        vacating.push_back(entry.path);
    }
    std::sort(vacating.begin(), vacating.end());
    vacating.erase(std::unique(vacating.begin(), vacating.end()), vacating.end());

    const std::string stamp = trashStamp();
    std::error_code ec;
    for (const std::string& path : vacating) {
        const std::filesystem::path from = projectRoot / std::filesystem::path(path);
        if (std::filesystem::is_regular_file(from, ec)) {
            std::filesystem::path to = projectRoot / ".engine" / "trash" / stamp / std::filesystem::path(path);
            for (int suffix = 2; std::filesystem::exists(to, ec) && suffix < 1000; ++suffix)
                to = projectRoot / ".engine" / "trash" / (stamp + "-" + std::to_string(suffix)) /
                     std::filesystem::path(path);
            std::filesystem::create_directories(to.parent_path(), ec);
            std::filesystem::rename(from, to, ec);
            if (ec) {
                // Not moved, so not lost either: it stays where it is, and the
                // next open mounts it. Said.
                report.problems.push_back(core::tr(ENG_TR("engine.editor.scripts.problem.not_trashed"),
                                                   {{"path", path}, {"reason", ec.message()}}));
                ec.clear();
                continue;
            }
            report.trashed.push_back(path);
            report.trash = ".engine/trash/" + stamp;
            // The folders it leaves empty go too, up to its tree's root.
            for (std::filesystem::path dir = from.parent_path();; dir = dir.parent_path()) {
                const std::string relative = std::filesystem::relative(dir, projectRoot, ec).generic_string();
                if (ec || !inRoots(relative) || !std::filesystem::is_empty(dir, ec) || ec)
                    break;
                std::filesystem::remove(dir, ec);
                if (ec)
                    break;
            }
            ec.clear();
        }
        script::forgetMountedPath(L, path);
    }
    // A script that left the file trees is the scene's again, and saved there.
    for (auto& [id, path] : currentOf) {
        if (!world.alive(id) || !inRoots(path))
            continue;
        const bool stillInTree = std::any_of(candidates.begin(), candidates.end(),
                                             [&](const Candidate& candidate) { return candidate.id == id; });
        if (!stillInTree)
            world.setMounted(id, false);
    }

    // --- Written where they belong -------------------------------------------
    const core::NameAtom sourceKey = world.atoms().intern("Source");
    for (Candidate* candidate : placing) {
        std::string root = candidate->container->root;
        for (const std::string& accepted : candidate->container->accepted) {
            if (under(candidate->current, accepted))
                root = accepted;
        }
        const std::string base = fileSafe(world.atoms().text(world.name(candidate->id)));
        std::string target;
        std::string chosen;
        for (int number = 1; number < 1000; ++number) {
            chosen = number == 1 ? base : base + std::to_string(number);
            target = placeOf(world, *candidate, root, chosen);
            // Never over a file that is not this script's: another script's,
            // or one somebody put there that nothing mounted.
            if (!claims.contains(claimKey(target)) &&
                !std::filesystem::exists(projectRoot / std::filesystem::path(target), ec))
                break;
        }
        const std::optional<scene::Value> source = world.getProperty(candidate->id, sourceKey);
        const auto* text = source.has_value() ? std::get_if<std::string>(&*source) : nullptr;
        const std::filesystem::path file = projectRoot / std::filesystem::path(target);
        std::filesystem::create_directories(file.parent_path(), ec);
        ec.clear();
        if (text == nullptr || !platform::writeTextFile(file, *text)) {
            // Kept in the scene rather than lost: not a file, so saved there.
            report.problems.push_back(
                core::tr(ENG_TR("engine.editor.scripts.problem.not_written_stays"), {{"path", target}}));
            world.setMounted(candidate->id, false);
            continue;
        }
        if (chosen != world.atoms().text(world.name(candidate->id))) {
            world.setName(candidate->id, world.atoms().intern(chosen));
            report.renamed.push_back(candidate->id);
        }
        script::setMountedPath(L, candidate->id, target, script::scriptTextHash(*text));
        world.setMounted(candidate->id, true);
        claims[claimKey(target)] = candidate->id;
        candidate->target = target;
        (candidate->current.empty() ? report.written : report.moved).push_back(target);
    }

    // **A script that stayed where it is, edited in the editor**: its file is
    // brought up to date -- unless the file changed on disk as well since it
    // was read, and then the disk is kept and the editor's text put beside
    // the trash, so neither is lost.
    std::map<std::string, core::u64> hashOf;
    for (const script::ModuleRegistry::Entry& entry : script::mountedEntries(L))
        hashOf[entry.path] = entry.diskHash;
    for (Candidate* candidate : kept) {
        const std::optional<scene::Value> source = world.getProperty(candidate->id, sourceKey);
        const auto* text = source.has_value() ? std::get_if<std::string>(&*source) : nullptr;
        if (text == nullptr)
            continue;
        const core::u64 edited = script::scriptTextHash(*text);
        const core::u64 known = hashOf[candidate->target];
        if (edited == known)
            continue;
        const std::filesystem::path file = projectRoot / std::filesystem::path(candidate->target);
        std::string disk;
        const bool read = platform::readTextFile(file, disk);
        const core::u64 onDisk = read ? script::scriptTextHash(disk) : 0;
        if (read && onDisk == edited) {
            // Saved already -- a tab's `Ctrl+S` -- so only remembered.
            script::setMountedHash(L, candidate->target, edited);
            continue;
        }
        if (read && onDisk != known) {
            std::filesystem::path aside =
                projectRoot / ".engine" / "conflicts" / stamp / std::filesystem::path(candidate->target);
            std::filesystem::create_directories(aside.parent_path(), ec);
            ec.clear();
            if (platform::writeTextFile(aside, *text))
                report.conflicts.push_back(candidate->target);
            else
                report.problems.push_back(
                    core::tr(ENG_TR("engine.editor.scripts.problem.not_kept_aside"), {{"path", candidate->target}}));
            continue;
        }
        if (!platform::writeTextFile(file, *text)) {
            report.problems.push_back(
                core::tr(ENG_TR("engine.editor.scripts.problem.not_written"), {{"path", candidate->target}}));
            continue;
        }
        script::setMountedHash(L, candidate->target, edited);
        report.updated.push_back(candidate->target);
    }

    // **The folders on the way are the mount's**, as opening the project would
    // make them, so a scene does not save a second copy of each beside the one
    // the next open makes.
    for (const Candidate& candidate : candidates) {
        if (candidate.target.empty())
            continue;
        for (const core::InstanceId folder : candidate.folders)
            world.setMounted(folder, true);
    }
    return report;
}

ScriptFileSync followSceneScripts(WorldHost& host, std::string_view fromScene, std::string_view toScene, bool copy)
{
    ScriptFileSync report;
    const std::filesystem::path projectRoot = host.projectRoot();
    if (projectRoot.empty() || fromScene.empty() || toScene.empty() || fromScene == toScene)
        return report;
    const std::string fromRoot = "src/scenes/" + std::string(fromScene);
    const std::string toRoot = "src/scenes/" + std::string(toScene);
    const std::filesystem::path from = projectRoot / std::filesystem::path(fromRoot);
    const std::filesystem::path to = projectRoot / std::filesystem::path(toRoot);
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec))
        return report;
    if (std::filesystem::exists(to, ec)) {
        report.problems.push_back(
            core::tr(ENG_TR("engine.editor.scripts.problem.already_there"), {{"to", toRoot}, {"from", fromRoot}}));
        return report;
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    ec.clear();
    if (copy)
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
    else
        std::filesystem::rename(from, to, ec);
    if (ec) {
        report.problems.push_back(core::tr(copy ? ENG_TR("engine.editor.scripts.problem.not_copied")
                                                : ENG_TR("engine.editor.scripts.problem.not_moved"),
                                           {{"path", fromRoot}, {"reason", ec.message()}}));
        return report;
    }
    (copy ? report.written : report.moved).push_back(toRoot);

    // The open scene's scripts now come from the new folder.
    if (!copy && host.sceneName() == fromScene) {
        lua_State* L = host.runtime().state();
        if (L != nullptr) {
            for (const script::ModuleRegistry::Entry& entry : script::mountedEntries(L)) {
                if (under(entry.path, fromRoot))
                    script::setMountedPath(L, entry.instance, toRoot + entry.path.substr(fromRoot.size()),
                                           entry.diskHash);
            }
        }
        host.setSceneName(std::string(toScene));
    }
    return report;
}

} // namespace engine::app
