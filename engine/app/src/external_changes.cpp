#include "engine/app/external_changes.h"

#include <algorithm>
#include <array>
#include <filesystem>

#include "engine/app/script_editor.h"
#include "engine/app/world_host.h"
#include "engine/core/utf8_mark.h"
#include "engine/platform/file.h"
#include "engine/script/modules.h"

namespace engine::app {
namespace {

[[nodiscard]] bool isScriptPath(std::string_view path) noexcept
{
    return path.starts_with("src/") && path.ends_with(".luau") && !path.ends_with(".d.luau");
}

[[nodiscard]] std::optional<script::ModuleRegistry::Entry> entryOf(lua_State* L, std::string_view path)
{
    for (script::ModuleRegistry::Entry& entry : script::mountedEntries(L)) {
        if (entry.path == path)
            return std::move(entry);
    }
    return std::nullopt;
}

[[nodiscard]] std::string sourceOf(const scene::World& world, core::InstanceId id)
{
    const std::optional<scene::Value> value = world.getProperty(id, world.atoms().lookup("Source"));
    const auto* text = value.has_value() ? std::get_if<std::string>(&*value) : nullptr;
    return text != nullptr ? *text : std::string{};
}

// The tab a mounted script is open in, if any.
[[nodiscard]] OpenScript* tabOf(ScriptEditor& scripts, core::InstanceId instance, std::string_view path)
{
    for (std::size_t index = 0; OpenScript* tab = scripts.at(index); ++index) {
        if (tab->instance == instance && tab->file == path)
            return tab;
    }
    return nullptr;
}

[[nodiscard]] bool readScript(const std::filesystem::path& file, std::string& text)
{
    if (!platform::readTextFile(file, text))
        return false;
    core::dropUtf8Mark(text);
    return true;
}

// The disk's text, into the script and its tab, and remembered as read.
void takeText(WorldHost& host, core::InstanceId instance, std::string_view path, const std::string& text,
              OpenScript* tab)
{
    scene::World& world = host.world();
    (void)world.setProperty(instance, world.atoms().intern("Source"), scene::Value{text});
    script::setMountedHash(host.runtime().state(), path, script::scriptTextHash(text));
    if (tab != nullptr) {
        // Where the caret was, as near as the new text allows: the view does
        // not jump to the top because somebody else saved.
        const Position caret = tab->caret.head;
        (void)tab->document.setText(text);
        tab->caret.head = tab->caret.anchor = tab->document.clamp(caret);
        tab->extraCarets.clear();
        tab->markSavedNow();
        tab->changedOnDisk = false;
    }
}

} // namespace

std::optional<ScriptMountPlace> scriptMountPlace(std::string_view path, std::string_view sceneName)
{
    const auto under = [path](std::string_view folder) {
        return path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/';
    };
    if (under("src/client"))
        return ScriptMountPlace{"src/client", "GlobalScriptService/Client", false};
    if (under("src/server"))
        return ScriptMountPlace{"src/server", "GlobalScriptService/Server", false};
    if (under("src/shared"))
        return ScriptMountPlace{"src/shared", "GlobalScriptService/Shared", true};
    // The folder before ADR 0105, read as the client's.
    if (under("src/scripts"))
        return ScriptMountPlace{"src/scripts", "GlobalScriptService/Client", false};
    if (!sceneName.empty()) {
        const std::string scene = "src/scenes/" + std::string(sceneName);
        if (under(scene + "/server"))
            return ScriptMountPlace{scene + "/server", "ServerScriptService", false};
        if (under(scene + "/client"))
            return ScriptMountPlace{scene + "/client", "ClientScriptService", false};
    }
    return std::nullopt;
}

ExternalScriptChanges applyExternalScriptChanges(WorldHost& host, ScriptEditor& scripts,
                                                 std::span<const FileChange> changes)
{
    ExternalScriptChanges out;
    lua_State* L = host.runtime().state();
    scene::World& world = host.world();
    for (const FileChange& change : changes) {
        if (!isScriptPath(change.path))
            continue;
        const std::optional<script::ModuleRegistry::Entry> entry = entryOf(L, change.path);
        const std::filesystem::path file = host.projectRoot() / std::filesystem::path(change.path);

        if (change.kind == FileChangeKind::Removed) {
            if (!entry.has_value() || !world.alive(entry->instance))
                continue;
            OpenScript* tab = tabOf(scripts, entry->instance, change.path);
            const bool edited = script::scriptTextHash(sourceOf(world, entry->instance)) != entry->diskHash ||
                                (tab != nullptr && tab->dirty());
            if (edited) {
                out.keptEdited.push_back(change.path);
                continue;
            }
            for (std::size_t index = 0; const OpenScript* open = scripts.at(index); ++index) {
                if (open == tab) {
                    (void)scripts.close(index);
                    break;
                }
            }
            script::forgetMountedPath(L, change.path);
            (void)world.destroy(entry->instance);
            out.removed.push_back(change.path);
            continue;
        }

        std::string text;
        if (!readScript(file, text))
            continue;
        if (!entry.has_value() || !world.alive(entry->instance)) {
            // A new file, where the project's open would have mounted one.
            const std::optional<ScriptMountPlace> place = scriptMountPlace(change.path, host.sceneName());
            if (!place.has_value())
                continue;
            if (host.mountScriptFile(change.path, place->root, place->container, place->module).valid())
                out.mounted.push_back(change.path);
            continue;
        }

        const core::u64 onDisk = script::scriptTextHash(text);
        if (onDisk == entry->diskHash)
            continue;
        OpenScript* tab = tabOf(scripts, entry->instance, change.path);
        const bool edited = script::scriptTextHash(sourceOf(world, entry->instance)) != entry->diskHash ||
                            (tab != nullptr && tab->dirty());
        if (edited) {
            if (tab != nullptr)
                tab->changedOnDisk = true;
            out.conflicts.push_back(change.path);
            continue;
        }
        takeText(host, entry->instance, change.path, text, tab);
        out.reloaded.push_back(change.path);
    }
    return out;
}

bool keepEditorText(WorldHost& host, OpenScript& tab)
{
    if (tab.file.empty())
        return false;
    const std::string text = tab.document.text();
    if (!platform::writeTextFile(host.projectRoot() / std::filesystem::path(tab.file), text))
        return false;
    script::setMountedHash(host.runtime().state(), tab.file, script::scriptTextHash(text));
    tab.markSavedNow();
    tab.changedOnDisk = false;
    return true;
}

bool takeDiskText(WorldHost& host, OpenScript& tab)
{
    if (tab.file.empty() || !host.world().alive(tab.instance))
        return false;
    std::string text;
    if (!readScript(host.projectRoot() / std::filesystem::path(tab.file), text))
        return false;
    takeText(host, tab.instance, tab.file, text, &tab);
    return true;
}

} // namespace engine::app
