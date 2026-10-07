--!strict
--[[
	Finding the project and finding the engine.

	Both are "look in the obvious places and say so when they are not there".
	The alternative -- guessing -- produces the failure this milestone keeps
	designing against: a command that runs, reports success, and did nothing.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")

local toml = require("./toml")

export type Project = {
    root: string,
    -- Parsed `project.toml`, or an empty table when the project has none. A
    -- project without one is legal -- a code folder is what makes a directory a
    -- project -- and every default below is what api-design.md §4 documents.
    config: toml.Table,
    diagnostic: string?,
}

local DefaultDevPort = 4560

local function joined(...: string): string
    return path.format(path.join(...))
end

--- The code folders that make a directory a project with no `project.toml`
--- (ADR 0105), and `src/scripts`, the one before them.
local CodeFolders = { "client", "server", "shared", "scenes", "scripts" }

local function hasCodeFolder(root: string): boolean
    for _, folder in CodeFolders do
        if fs.exists(joined(root, "src", folder)) then
            return true
        end
    end
    return false
end

--- Walks up from `start` looking for a `project.toml` or a code folder. Returns
--- `start` itself when neither is found, so the caller reports "not a project"
--- against the directory the user actually named.
local function findRoot(start: string): string
    local current = path.format(path.resolve(start))
    for _ = 1, 32 do
        if fs.exists(joined(current, "project.toml")) or hasCodeFolder(current) then
            return current
        end
        local parent = path.dirname(current)
        if parent == current or parent == "" then
            break
        end
        current = parent
    end
    return path.format(path.resolve(start))
end

local function load(start: string): Project
    local root = findRoot(start)
    local configPath = joined(root, "project.toml")

    if not fs.exists(configPath) then
        return { root = root, config = {} }
    end

    local parsed = toml.parse(fs.readFileToString(configPath))
    if not parsed.ok then
        return {
            root = root,
            config = {},
            diagnostic = `{configPath}:{parsed.line}: {parsed.diagnostic}`,
        }
    end
    return { root = root, config = parsed.table }
end

--- Whether this directory is something the engine can mount at all: a project
--- file, or a code folder that makes one.
local function isProject(project: Project): boolean
    return fs.exists(joined(project.root, "project.toml")) or hasCodeFolder(project.root)
end

local function devPort(project: Project): number
    local dev = project.config.dev
    if typeof(dev) == "table" and typeof(dev.port) == "number" then
        return dev.port
    end
    return DefaultDevPort
end

local function executableName(base: string): string
    return if platform.win32 then `{base}.exe` else base
end

--- The profile `ludwerk build` packages, and the only one it accepts (D057). It
--- carries the Luau compiler and no ImGui, which is exactly what a shipped game
--- needs and exactly what no other profile offered: `shipping` links no compiler
--- (ADR 0002) and `ludwerk build` ships a game's Luau as SOURCE (ADR 0045), so the
--- packager had `dev` as its only option and every released binary carried the
--- debug overlay, the inspector and a REPL against its own VM.
---
--- **It is the LAST thing `findEngine` will accept, and it used to be the first**
--- (D087). Nothing had a player build on the machine it was running on, so
--- putting it first cost nothing -- until this repository grew a command that
--- makes one, and `ludwerk edit` started launching a binary with no editor
--- compiled into it. A black window, because in edit mode the world goes to an
--- offscreen texture and the panels are the only thing that draws it.
local PlayerPresets = { "win-msvc-player", "linux-clang-player" }

--- What somebody working in this repository is iterating on, which is what they
--- mean when they type a command that runs the engine.
---
--- **`linux-clang-asan` is here and it is LAST**, which are two separate
--- decisions. It is here because it is a `dev` profile with the sanitizers
--- turned on, and without it `ludwerk test` could not find the engine at all --
--- the sanitizer gate ran the whole ctest suite green and then failed with no
--- output, because the CLI it calls next did not know that build existed. It is
--- last because an instrumented binary is two to three times slower, and
--- somebody who has both should get the one they are iterating in.
local DevPresets = { "win-msvc-dev", "linux-clang-dev", "macos-clang-dev", "win-msvc-debug", "linux-clang-asan" }

--- The profile the packaged editor is built from (ADR 0054). After `dev`,
--- because an `editor` build is an artifact somebody produced an hour ago and a
--- `dev` build is what they are working in.
local EditorPresets = { "win-msvc-editor", "linux-clang-editor" }

--- The profiles built with no debug UI at all (`cmake/engine_options.cmake`
--- turns `ENG_DEBUG_UI` off for exactly these two). A host built from one of
--- them cannot draw an editor, an inspector or the F3 overlay -- there is no
--- ImGui target linked into it to draw them with.
local NoDebugUiProfiles: { [string]: boolean } = { player = true, shipping = true }

--- Where this CLI is installed: the directory holding its template tree, the
--- generated definitions, and — in an installation rather than a source tree —
--- the engine binaries beside them (ADR 0054).
---
--- **Found by looking for the layout rather than by counting up from this file,
--- and that is D045.** `new.luau` counted two directories, which was right while
--- it lived one level higher and stopped being right the moment it moved; the
--- command still ran and failed inside `listDirectory` on a path with `../..` in
--- the middle of it. A count is a fact about a directory layout that no check
--- holds still.
---
--- Inside this repository this is the repository root, which is what makes the
--- installed shape the same code path as the developed one rather than a second
--- one that only runs where nobody is looking.
local function installRoot(): string?
    local here = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$")
    if here == nil then
        return nil
    end
    local current = path.format(path.resolve(here))
    for _ = 1, 8 do
        if fs.exists(joined(current, "templates", "starter")) then
            return current
        end
        local parent = path.dirname(current)
        if parent == current or parent == "" then
            break
        end
        current = parent
    end
    return nil
end

--- The three places a binary this CLI drives can be, named so the ORDER is a
--- thing a test can hold still rather than a comment about what happens.
export type Roots = {
    -- What somebody pointed at explicitly, from the environment.
    explicit: string?,
    -- `ENG_BUILD_ROOT`: inside this repository the binaries are always there,
    -- because R14 puts builds outside the source tree.
    buildRoot: string?,
    -- Where this CLI is installed, which is where a downloaded release keeps
    -- them. `nil` when the layout is not recognisable.
    install: string?,
}

--- Every path a named binary could be at, in the order worth trying. Pure: the
--- caller reads the environment, this decides the order.
---
--- **`process.cwd()` is deliberately not in it, and used to be** (ADR 0054). It
--- was the last candidate, under a comment calling it "beside this CLI, which is
--- the shape an installed release has" — and it is not beside the CLI, it is
--- wherever the person is standing. It found a host only when somebody happened
--- to be inside the installation, and would otherwise pick up any stray binary
--- in the directory they ran from. `install` is the answer it was reaching for.
---
--- `buildPath` is where a preset directory keeps the binary; `installPath` is
--- where the packaged folder keeps it. They differ because a build tree is
--- organised by the source layout and an installation by what a person opens.
local function hostCandidates(buildPath: string, installPath: string, presets: { string }, roots: Roots): { string }
    local candidates: { string } = {}

    if roots.explicit ~= nil and roots.explicit ~= "" then
        table.insert(candidates, roots.explicit)
    end

    -- **An installation's own binary before a build tree's** (D589). It was
    -- the other way round, and on a machine that also builds the engine
    -- (`ENG_BUILD_ROOT` set) an installed `ludwerk` ran whatever that tree
    -- held -- another version's host, checking a project with the wrong
    -- engine, and one a build was in the middle of writing ("resource busy
    -- or locked"). In this repository there is no binary at the root, so the
    -- build tree is still what is found here.
    if roots.install ~= nil and roots.install ~= "" then
        table.insert(candidates, joined(roots.install, installPath))
    end

    if roots.buildRoot ~= nil and roots.buildRoot ~= "" then
        for _, preset in presets do
            table.insert(candidates, joined(roots.buildRoot, preset, buildPath))
        end
    end

    return candidates
end

--- Where an installation keeps the host a person edits and runs with, and where
--- it keeps the `player` one `ludwerk build` packages (ADR 0054). Two binaries
--- rather than one, each with the content directory the host resolves beside
--- its own executable: a package that shipped only the editor host would make
--- `ludwerk build` package the editor into somebody's game, which is D057.
---
--- The player is this machine's one, under its target's directory: an
--- installation carries a player per target (ADR 0104 §6), and the one this
--- search answers for is the one that runs here.
local InstalledEngine = "engine-host"
local InstalledPlayer = if platform.win32 then "player/windows-x64/engine-host" else "player/linux-x64/engine-host"

--- Every path `ludwerk dev` and `ludwerk edit` would accept a host at, in order.
--- Pure, so the ORDER is a thing a test drives rather than a comment.
local function engineCandidates(roots: Roots): { string }
    local name = executableName("engine-host")
    local presets: { string } = {}
    for _, group in { DevPresets, EditorPresets, PlayerPresets } do
        for _, preset in group do
            table.insert(presets, preset)
        end
    end
    return hostCandidates(joined("engine", "app", name), executableName(InstalledEngine), presets, roots)
end

--- The environment as the three roots. One function, so that every search below
--- reads the same environment in the same order.
local function environmentRoots(explicit: string?): Roots
    return {
        explicit = explicit,
        buildRoot = process.env.ENG_BUILD_ROOT,
        install = installRoot(),
    }
end

local function findEngine(): string?
    for _, candidate in engineCandidates(environmentRoots(process.env.ENG_HOST)) do
        if fs.exists(candidate) then
            return candidate
        end
    end
    return nil
end

--- A host built from the `player` profile, or nothing.
---
--- Separate from `findEngine` because the two questions are different and only
--- one of them has a fallback: `ludwerk dev` runs whatever host exists, and
--- `ludwerk build` PACKAGES one -- so it has to be able to tell "the right binary"
--- from "a binary", and refuse rather than ship the wrong one quietly.
---
--- **`ENG_HOST` is NOT honoured here**, and that is the point rather than an
--- oversight. It is the variable `ludwerk dev` uses, so honouring it would make
--- "I pointed the dev server at a build" silently decide what a release
--- contains -- the exact shape of the defect this is fixing. `ENG_PLAYER_HOST`
--- is the door for somebody who has a player binary somewhere this does not
--- look, and a dev binary reached that way is a choice with a name on it.
--- `player/` in an installation, and it is deliberately not the binary beside
--- it: what a person edits with carries the overlay, the inspector and a REPL
--- against its own VM, and what their players receive must not (D057).
local function playerCandidates(roots: Roots): { string }
    local name = executableName("engine-host")
    return hostCandidates(joined("engine", "app", name), executableName(InstalledPlayer), PlayerPresets, roots)
end

local function findPlayerEngine(): string?
    for _, candidate in playerCandidates(environmentRoots(process.env.ENG_PLAYER_HOST)) do
        if fs.exists(candidate) then
            return candidate
        end
    end
    return nil
end

--- The same search, for a host TOOL rather than for the engine: `assetc` and
--- anything else `tools/` builds. Kept as one function because "where did the
--- build put it" has exactly one answer and two copies of it would drift the
--- first time a preset was renamed.
---
--- `editor` before `dev` here and last in the engine search, which is not an
--- inconsistency: a tool has no profile-dependent behaviour, so the newest build
--- will do, while a HOST somebody is iterating on is the one they mean.
--- Which profile a host binary was built from, asked of the binary rather than
--- inferred from where it was found (D087).
---
--- **A path is not a profile.** The search below prefers presets in an order
--- that is right most of the time, and "most of the time" is exactly the kind of
--- claim that produces a black window: `ENG_HOST` can point anywhere, an
--- installation carries two hosts, and a build tree can hold five. The binary
--- knows what it is and says so under `--version`; nothing else does.
---
--- `nil` when it does not answer, and callers treat that as "carry on": a host
--- this cannot interrogate is a host that may well work, and refusing on a
--- failed probe would be refusing on no evidence.
local function hostProfile(engine: string): string?
    local probe = process.run({ engine, "--version" })
    if not probe.ok then
        return nil
    end
    return string.match(probe.stdout, "Ludwerk%s+[%d%.]+%s+%(([%w_%-]+)%)")
end

--- Whether this host has an editor, an inspector and an overlay compiled into
--- it. Unknown profiles pass: the deny list is the one `engine_options.cmake`
--- writes, and a profile nobody has heard of is not evidence of absence.
local function hostHasDebugUi(engine: string): boolean
    local profile = hostProfile(engine)
    return profile == nil or not NoDebugUiProfiles[profile]
end

local ToolPresets =
    { "win-msvc-editor", "win-msvc-dev", "linux-clang-editor", "linux-clang-dev", "macos-clang-dev", "win-msvc-debug" }

local function toolCandidates(base: string, roots: Roots): { string }
    local name = executableName(base)
    return hostCandidates(joined("tools", base, name), name, ToolPresets, roots)
end

--- **The newest of the builds that have it, not the first preset in the list.**
--- "The newest build will do" was the reasoning for the order above, and an
--- order is not newness: a `win-msvc-editor` tree the gate only ever links the
--- host in keeps an `assetc` from weeks ago, and it won because it was listed
--- first -- `ludwerk build` then ran an `assetc` without the `icon` command it
--- had just been given. The variable, when set, still wins outright.
local function findTool(base: string): string?
    local roots = environmentRoots(process.env[`ENG_{string.upper(base)}`])
    if roots.explicit ~= nil and roots.explicit ~= "" and fs.exists(roots.explicit) then
        return roots.explicit
    end
    local newest: string? = nil
    local newestTime = -1
    for _, candidate in toolCandidates(base, { buildRoot = roots.buildRoot }) do
        if fs.exists(candidate) then
            local ok, metadata = pcall(fs.metadata, candidate)
            local modified = if ok then metadata.modified:toNanoseconds() else 0
            if modified > newestTime then
                newest = candidate
                newestTime = modified
            end
        end
    end
    if newest ~= nil then
        return newest
    end
    for _, candidate in toolCandidates(base, { install = roots.install }) do
        if fs.exists(candidate) then
            return candidate
        end
    end
    return nil
end

--- **Where `base` is in the build or the installation `host` came from**, or
--- nil when `host` is in neither layout (D517). Pure: the paths are matched,
--- not looked at.
---
--- A build tree keeps the host at `<tree>/engine/app/` and its tools at
--- `<tree>/tools/<tool>/`; an installation keeps a player at
--- `player/<target>/` and its tools at its root.
local function toolBesideCandidate(base: string, host: string): string?
    local name = executableName(base)
    local tree = string.match(host, "^(.*)[/\\]engine[/\\]app[/\\][^/\\]+$")
    if tree ~= nil then
        return joined(tree, "tools", base, name)
    end
    local install = string.match(host, "^(.*)[/\\]player[/\\][^/\\]+[/\\][^/\\]+$")
    if install ~= nil then
        return joined(install, name)
    end
    return nil
end

--- **`base` from the same build as the player it compiles for** (D517). The
--- newest of every build was the answer, tool by tool -- so an export ran an
--- asset compiler from the build somebody was working in, writing a mesh
--- format the player from another build could not read, and every animated
--- character shipped as a box. The variable, when set, still wins; a player in
--- neither layout -- one named outright, or a cross-compiled one with no tools
--- of its own -- falls back to the newest, and the format check after it is
--- what stands between that and a broken game.
local function findToolFor(base: string, host: string): string?
    local explicit = process.env[`ENG_{string.upper(base)}`]
    if explicit ~= nil and explicit ~= "" and fs.exists(explicit) then
        return explicit
    end
    local beside = toolBesideCandidate(base, host)
    if beside ~= nil and fs.exists(beside) then
        return beside
    end
    return findTool(base)
end

--- Every directory under `root` that holds Luau the engine mounts or requires,
--- deepest last. One watch per directory, because `fs.watch` reports the
--- top-level entry rather than the changed file and inotify may not report a
--- subdirectory's contents at all (U-58).
local function sourceDirectories(root: string): { string }
    local found: { string } = {}
    local pending: { string } = {}

    for _, name in { "src", "tests" } do
        local candidate = joined(root, name)
        if fs.exists(candidate) then
            table.insert(pending, candidate)
        end
    end

    while #pending > 0 do
        local dir = table.remove(pending) :: string
        table.insert(found, dir)
        for _, entry in fs.listDirectory(dir) do
            if entry.type == "dir" then
                table.insert(pending, joined(dir, entry.name))
            end
        end
    end

    table.sort(found)
    return found
end

--- Every `.luau` file under the project's source directories, with its content
--- hash, sorted by path. This is what a save is compared against: `fs.watch`
--- says only that *something* under a directory changed (U-58), so the watcher
--- rescans and diffs rather than trusting the event.
--- Every directory under `content/`, for the watcher.
---
--- **Separate from `sourceDirectories` because the two answer different
--- questions** (S6.4): a `.luau` under `src/` is a script and reloads the world,
--- and a `.png` under `content/` is an asset and reloads itself. Sending one
--- verb for both would make every texture save restart the game.
local function contentDirectories(root: string): { string }
    local found: { string } = {}
    local start = joined(root, "content")
    if not fs.exists(start) then
        return found
    end

    local pending: { string } = { start }
    while #pending > 0 do
        local dir = table.remove(pending) :: string
        table.insert(found, dir)
        for _, entry in fs.listDirectory(dir) do
            if entry.type == "dir" then
                table.insert(pending, joined(dir, entry.name))
            end
        end
    end
    return found
end

--- What is under `content/`, keyed by path, valued by size and modification
--- time rather than by contents.
---
--- **Not the bytes, which is the difference from `sourceSnapshot`.** A project's
--- scripts are kilobytes and comparing them directly cannot collide; its content
--- is megabytes of texture and mesh, and reading all of it on every filesystem
--- event to find out whether anything changed would cost more than the reload
--- it is deciding about.
local function contentSnapshot(root: string): { [string]: string }
    local snapshot: { [string]: string } = {}
    for _, dir in contentDirectories(root) do
        for _, entry in fs.listDirectory(dir) do
            if entry.type == "file" then
                local file = joined(dir, entry.name)
                local ok, meta = pcall(fs.metadata, file)
                if ok and meta ~= nil then
                    snapshot[file] = `{meta.size}:{meta.modified:toNanoseconds()}`
                end
            end
        end
    end
    return snapshot
end

local function sourceSnapshot(root: string): { [string]: string }
    local snapshot: { [string]: string } = {}
    for _, dir in sourceDirectories(root) do
        for _, entry in fs.listDirectory(dir) do
            if entry.type == "file" and string.sub(entry.name, -5) == ".luau" then
                local file = joined(dir, entry.name)
                -- The content itself rather than a digest: a project's scripts
                -- are kilobytes, and comparing them directly cannot collide.
                snapshot[file] = fs.readFileToString(file)
            end
        end
    end
    return snapshot
end

local function changedFiles(before: { [string]: string }, after: { [string]: string }): { string }
    local changed: { string } = {}
    for file, contents in after do
        if before[file] ~= contents then
            table.insert(changed, file)
        end
    end
    for file in before do
        if after[file] == nil then
            table.insert(changed, file)
        end
    end
    table.sort(changed)
    return changed
end

return {
    load = load,
    isProject = isProject,
    devPort = devPort,
    findEngine = findEngine,
    findPlayerEngine = findPlayerEngine,
    findTool = findTool,
    findToolFor = findToolFor,
    toolBesideCandidate = toolBesideCandidate,
    installRoot = installRoot,
    hostProfile = hostProfile,
    hostHasDebugUi = hostHasDebugUi,
    -- The three searches as pure functions of their roots, which is the only
    -- reason they are reachable: "an explicit variable, then the build tree,
    -- then the installation" is a claim, and a claim in a comment is not one
    -- anything holds to (ADR 0054).
    engineCandidates = engineCandidates,
    playerCandidates = playerCandidates,
    toolCandidates = toolCandidates,
    executableName = executableName,
    sourceDirectories = sourceDirectories,
    sourceSnapshot = sourceSnapshot,
    contentDirectories = contentDirectories,
    contentSnapshot = contentSnapshot,
    changedFiles = changedFiles,
    joined = joined,
}
