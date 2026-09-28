#pragma once

// **The files in `src/` follow the tree, at every save** (ADR 0105).
//
// Code under a script service is a file: a script made there, pasted there,
// duplicated there, dragged there from another service, renamed or moved into
// another folder is written to the file its place says, and one deleted or
// moved out of the file tree gives its file up. Done when the scene is saved,
// never before -- so undo is the world's alone, and closing without saving
// leaves the disk as it was.
//
// **Nothing is ever deleted.** A file a script no longer claims is moved to
// `.engine/trash/<when>/` with its path, and a file somebody else put where a
// script wants to go is never written over: the script takes the next free
// name instead, and the instance is renamed to match, so the tree and the
// folder say the same thing. A script whose file cannot be written stays in the
// scene as it was, so it is saved there rather than lost.

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/id.h"

namespace engine::app {

class WorldHost;

struct ScriptFileSync
{
    // Project-relative paths, '/' separators.
    std::vector<std::string> written;
    std::vector<std::string> moved;
    std::vector<std::string> trashed;
    // Instances renamed so their file's name could be theirs.
    std::vector<core::InstanceId> renamed;
    // What could not be done, said to the person.
    std::vector<std::string> problems;
    // The folder a trashed file went to, project-relative.
    std::string trash;

    [[nodiscard]] bool changedAnything() const noexcept
    {
        return !written.empty() || !moved.empty() || !trashed.empty() || !renamed.empty();
    }
    // One line for the status bar, empty when nothing happened.
    [[nodiscard]] std::string summary() const;
};

// Makes `src/` match the script services of `host`'s world: `GlobalScriptService`
// always, and the scene's own two when `sceneName` (the scene file's name
// without `.scene.json`) is not empty.
ScriptFileSync syncScriptFiles(WorldHost& host, std::string_view sceneName);

} // namespace engine::app
