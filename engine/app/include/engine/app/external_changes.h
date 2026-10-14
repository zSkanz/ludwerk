// **A script file changed outside the editor, while it is open** (the
// file-watching batch): what the editor does with each change the watcher
// reports (`project_watch.h`).
//
// Nothing is lost and nothing is guessed at. A script nobody has edited here
// takes the disk's text -- its `Source`, so the next Play compiles it (ADR
// 0058), and its tab if one is open. A script edited here AND changed on disk
// is a question for the person: its tab says so and offers to keep the
// editor's text or take the disk's, and until it is answered the save keeps
// the disk and puts the editor's text beside it, as it always has. A new file
// under `src/` is mounted where the project's open would have mounted it; a
// file that went takes its script with it, unless the script was edited here.
// **A touch without a change does nothing**: a file is compared by what it
// holds, not by when it was written.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/project_watch.h"

namespace engine::app {

class WorldHost;
class ScriptEditor;
struct OpenScript;

struct ExternalScriptChanges
{
    // Took the disk's text: the script's `Source`, and its tab when open.
    std::vector<std::string> reloaded;
    // A new file, mounted as a script.
    std::vector<std::string> mounted;
    // A file gone, and its script with it.
    std::vector<std::string> removed;
    // Changed on disk and edited here: the tab asks.
    std::vector<std::string> conflicts;
    // Gone from disk but edited here: the script stays, and the next save
    // writes it back.
    std::vector<std::string> keptEdited;

    [[nodiscard]] bool any() const noexcept
    {
        return !reloaded.empty() || !mounted.empty() || !removed.empty() || !conflicts.empty() || !keptEdited.empty();
    }
};

// What the editor does with the changes, in edit mode. Only paths under `src/`
// ending in `.luau` are a script's; the rest are left for their own handler.
ExternalScriptChanges applyExternalScriptChanges(WorldHost& host, ScriptEditor& scripts,
                                                 std::span<const FileChange> changes);

// The tab's answers to "changed on disk": keep the editor's text (it is
// written over the file), or take the disk's (the editor's is replaced). Each
// returns false, changing nothing, when the file cannot be written or read.
bool keepEditorText(WorldHost& host, OpenScript& tab);
bool takeDiskText(WorldHost& host, OpenScript& tab);

// **Where a file under `src/` mounts**, as the project's open mounts it
// (ADR 0105): its folder, the container its scripts go under, and whether that
// folder makes modules. Nothing for a file no folder mounts -- another scene's
// code, a file outside the five folders.
struct ScriptMountPlace
{
    std::string root;
    std::string container;
    bool module = false;
};
[[nodiscard]] std::optional<ScriptMountPlace> scriptMountPlace(std::string_view path, std::string_view sceneName);

} // namespace engine::app
