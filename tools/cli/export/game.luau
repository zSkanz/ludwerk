--!strict
--[[
	The `game/` folder every target carries (ADR 0104 §7), and what a
	dedicated game's two sides leave out of it (ADR 0105).

	**A side is decided by leaving files out, never by a flag the game reads.**
	A client package that carried the server's scripts and told the engine not
	to run them would still hand every player the server's code. So a
	`client` package has no `src/server/`, no `src/scenes/*/server/`, no
	scene's `ServerScriptService` or `ServerStorage`, and no `Server` folder in
	`global.json`; a `server` package has none of the client's. **And per
	script** (ADR 0138 §5): a `Script` anywhere else whose `RunContext` is the
	other side's keeps its instance and loses its `Source`, in every scene,
	stamp and `global.json` -- so a stamp can carry a door's server half and
	its client half, and each package gets its own. The sentinel
	test (`tests/packaging/sides.test.luau`) searches each side's output for the
	other's strings, pack included, because a pack stores what it carries
	uncompressed and a miss there is a leak.

	The pack is rebuilt per side from a staged copy of `content/` -- hard links
	where the file system allows, so a game with a gigabyte of art does not copy
	it twice -- in which only the scenes, the stamps and `global.json` are
	rewritten.
]]

local fs = require("@std/fs")
local path = require("@std/path")

local jsondoc = require("../jsondoc")
local projectlib = require("../project")

export type Side = "all" | "client" | "server"

--- **What `.engine/` holds that is the editor's, not the game's**: its layout,
--- its settings, the import and check caches, and the trash -- script files the
--- editor moved aside when their instance went, server code as often as not
--- (S0.1) -- and the conflicts, the editor's copy of a script file that changed
--- on disk under it, server code as often as not, and the developer's own save
--- slots (the script-sides audit). A game carries the pack, its manifest and the
--- partition cache, and none of these.
--- And `session`: a run's own cache of changed ground (ADR 0149), which a run
--- that died leaves behind -- tens of thousands of files of nobody's.
--- And `settings.json`: the settings of whoever last ran the project from its
--- folder -- a language, a window, a quality -- which were shipped to every
--- player as the game's own (D573). A player's are kept in the player's folder.
local EditorOnly = {
    "editor.json",
    "import",
    "check",
    "types",
    "match",
    "export",
    "trash",
    "conflicts",
    "saves",
    "session",
    "settings.json",
}
local EditorOnlyPrefixes = { "editor-layout" }

local function editorOnly(name: string): boolean
    for _, entry in EditorOnly do
        if name == entry then
            return true
        end
    end
    for _, prefix in EditorOnlyPrefixes do
        if string.sub(name, 1, #prefix) == prefix then
            return true
        end
    end
    return false
end

--- The pack and its manifest: a side builds its own, so it copies neither.
local PackFiles: { [string]: boolean } = { ["content.lpack"] = true, ["content.manifest.json"] = true }

--- Made again on the package itself by `ludwerk build`, so the project's own is
--- never copied: it holds the scene as the WHOLE project mounts it, the other
--- side's scripts included.
local Regenerated: { [string]: boolean } = { partition = true }

--- The script folders the OTHER side runs, relative to `src/` (ADR 0105).
--- `src/scripts/` is the legacy spelling of `src/client/` and goes with it.
local function foreignScripts(side: Side, relative: string): boolean
    if side == "all" then
        return false
    end
    local other = if side == "client" then "server" else "client"
    if relative == other or (side == "server" and relative == "scripts") then
        return true
    end
    -- `scenes/<scene>/<side>`.
    local sceneSide = string.match(relative, "^scenes/[^/]+/([^/]+)$")
    return sceneSide == other
end

--- The services of a scene the other side's code and storage live in.
local function foreignServices(side: Side): { string }
    if side == "client" then
        return { "ServerScriptService", "ServerStorage" }
    elseif side == "server" then
        return { "ClientScriptService" }
    end
    return {}
end

local function copyTree(from: string, to: string, skip: ((string) -> boolean)?, relative: string?)
    fs.createDirectory(to, { makeParents = true })
    for _, entry in fs.listDirectory(from) do
        local rel = if relative == nil then entry.name else `{relative}/{entry.name}`
        if skip ~= nil and skip(rel) then
            continue
        end
        local source = projectlib.joined(from, entry.name)
        local target = projectlib.joined(to, entry.name)
        if entry.type == "dir" then
            copyTree(source, target, skip, rel)
        elseif entry.type == "file" then
            -- `fs.copy` rather than read-then-write: the trees here carry a
            -- font, a content pack and an executable, and a text round-trip
            -- would corrupt all three.
            fs.copy(source, target)
        end
    end
end

--- **A streamed terrain's cells are files, and stay files** (D411). A terrain
--- of 256 cells or more is saved as a folder of them beside its scene (ADR
--- 0087), and the engine streams them from disk by name: a cell is read off
--- the frame's thread as a file, never through the content mounts. Packed with
--- the rest of `content/` they were in the game and out of its reach -- an
--- exported game with a large terrain had no ground at all, and a pack four
--- gigabytes larger for it.
local CellExtension = ".lterrain"

local function isCell(name: string): boolean
    return string.sub(name, -#CellExtension) == CellExtension
end

--- Whether `directory` is a terrain's folder of cells: it holds one.
local function holdsCells(directory: string): boolean
    for _, entry in fs.listDirectory(directory) do
        if entry.type == "file" and isCell(entry.name) then
            return true
        end
    end
    return false
end

--- A cell, or the index of the folder it is in.
local function cellFile(name: string, cells: boolean): boolean
    return isCell(name) or (cells and name == "index.json")
end

--- A tree of hard links to `from`, or copies where a link is refused (another
--- volume, a file system without them). Only for trees nothing writes into.
--- Terrain cells are left out: they ship beside the pack (`copyCells`).
local function linkTree(from: string, to: string)
    fs.createDirectory(to, { makeParents = true })
    local cells = holdsCells(from)
    for _, entry in fs.listDirectory(from) do
        local source = projectlib.joined(from, entry.name)
        local target = projectlib.joined(to, entry.name)
        if entry.type == "dir" then
            linkTree(source, target)
        elseif entry.type == "file" and not cellFile(entry.name, cells) then
            if not pcall(fs.link, source, target) then
                fs.copy(source, target)
            end
        end
    end
end

--- Every terrain's folder of cells under `from`, copied under `to` at the
--- path it has -- and nothing else of the tree. Returns how many files.
local function copyCells(from: string, to: string): number
    local copied = 0
    local cells = holdsCells(from)
    for _, entry in fs.listDirectory(from) do
        local source = projectlib.joined(from, entry.name)
        local target = projectlib.joined(to, entry.name)
        if entry.type == "dir" then
            copied += copyCells(source, target)
        elseif entry.type == "file" and cellFile(entry.name, cells) then
            fs.createDirectory(to, { makeParents = true })
            fs.copy(source, target)
            copied += 1
        end
    end
    return copied
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

--- Where a stamp's original text comes from, by its content-relative name: the
--- project's own file, never the staged one, which may already have lost a
--- side's code.
export type StampReader = (string) -> string?

--- The scripts of a stamp's tree, by the path an override names each with:
--- names joined by `.`, `#n` after a name its siblings share (`overridePath`
--- in `engine/scene/src/scene_file.cpp`).
local function stampScripts(node: jsondoc.Node, at: string, out: { [string]: jsondoc.Node })
    if jsondoc.stringOf(jsondoc.get(node, "class")) == "Script" then
        out[at] = node
    end
    local children = jsondoc.get(node, "children")
    if children == nil or children.kind ~= "array" then
        return
    end
    local shared: { [string]: number } = {}
    for _, child in children.items do
        local name = jsondoc.stringOf(jsondoc.get(child, "name")) or ""
        shared[name] = (shared[name] or 0) + 1
    end
    local seen: { [string]: number } = {}
    for _, child in children.items do
        local name = jsondoc.stringOf(jsondoc.get(child, "name")) or ""
        seen[name] = (seen[name] or 0) + 1
        local segment = if (shared[name] or 0) > 1 then `{name}#{seen[name]}` else name
        stampScripts(child, if at == "" then segment else `{at}.{segment}`, out)
    end
end

--- **A linked stamp's instance runs its scripts on the side ITS overrides say**
--- (the script-sides audit). The stamp file is stripped by its own sides; this
--- instance's code follows its own: an override that made a script the other
--- side's is given an empty `Source`, and one that made it this side's, when
--- the stamp file loses its code here, is given the stamp's own.
local function stripLinked(node: jsondoc.Node, side: Side, readStamp: StampReader): boolean
    local name = jsondoc.stringOf(jsondoc.get(node, "stamp"))
    local text = if name ~= nil then readStamp(name) else nil
    if text == nil then
        return false
    end
    local stamp = jsondoc.parse(text)
    local root = jsondoc.get(stamp, "root")
    if root == nil or root.kind ~= "object" then
        return false
    end
    local scripts: { [string]: jsondoc.Node } = {}
    stampScripts(root, "", scripts)
    local paths: { string } = {}
    for at in scripts do
        table.insert(paths, at)
    end
    table.sort(paths)

    local foreign = if side == "client" then "Server" else "Client"
    local changed = false
    local overrides: jsondoc.Node? = jsondoc.get(node, "overrides")
    local function overrideAt(at: string): jsondoc.Node
        local holder: jsondoc.Node = if overrides ~= nil then overrides else jsondoc.object()
        if overrides == nil then
            jsondoc.set(node, "overrides", holder)
            overrides = holder
        end
        local entry: jsondoc.Node = jsondoc.get(holder, at) or jsondoc.object()
        -- Set again when it was there: `set` replaces a key in its place.
        jsondoc.set(holder, at, entry)
        return entry
    end
    for _, at in paths do
        local properties = jsondoc.get(scripts[at], "properties")
        local stampSide = (if properties ~= nil then jsondoc.stringOf(jsondoc.get(properties, "RunContext")) else nil)
            or "Shared"
        local entry = if overrides ~= nil then jsondoc.get(overrides, at) else nil
        local own = (if entry ~= nil then jsondoc.stringOf(jsondoc.get(entry, "RunContext")) else nil) or stampSide
        if own == foreign then
            if entry == nil or jsondoc.stringOf(jsondoc.get(entry, "Source")) ~= "" then
                jsondoc.set(overrideAt(at), "Source", jsondoc.string(""))
                changed = true
            end
        elseif stampSide == foreign then
            local source = if properties ~= nil then jsondoc.get(properties, "Source") else nil
            if source ~= nil and (entry == nil or jsondoc.get(entry, "Source") == nil) then
                jsondoc.set(overrideAt(at), "Source", source)
                changed = true
            end
        end
    end
    return changed
end

--- **The other side's code, taken out of every `Script` in a node's tree that
--- carries the other side's `RunContext`** (ADR 0138 §5). The instance stays,
--- with no `Source`: the tree is the same on every machine, for replication and
--- the hash, and a script with no code is never live. A linked stamp's
--- instance is resolved against the stamp's own file when `readStamp` can read
--- it. Says whether anything changed.
local function stripScripts(node: jsondoc.Node?, side: Side, readStamp: StampReader?): boolean
    if node == nil or node.kind ~= "object" or side == "all" then
        return false
    end
    local foreign = if side == "client" then "Server" else "Client"
    local changed = false
    if jsondoc.stringOf(jsondoc.get(node, "class")) == "Script" then
        local properties = jsondoc.get(node, "properties")
        if
            properties ~= nil
            and jsondoc.stringOf(jsondoc.get(properties, "RunContext")) == foreign
            and jsondoc.get(properties, "Source") ~= nil
        then
            jsondoc.remove(properties, "Source")
            changed = true
        end
    end
    if readStamp ~= nil and jsondoc.get(node, "stamp") ~= nil then
        changed = stripLinked(node, side, readStamp) or changed
    end
    local children = jsondoc.get(node, "children")
    if children ~= nil and children.kind == "array" then
        for _, child in children.items do
            changed = stripScripts(child, side, readStamp) or changed
        end
    end
    return changed
end

--- The storages whose service decides its scripts' side (ADR 0138 §2): a
--- script in one is that side's whatever its `RunContext` says, and the other
--- side's is left out whole.
local DecidingServices: { [string]: boolean } =
    { ServerScriptService = true, ClientScriptService = true, ScriptService = true }

--- A scene's JSON without the other side's services, and without the other
--- side's code in the scripts left. Nil when nothing in it belonged to the
--- other side, so the staged file stays a link.
local function stripScene(text: string, side: Side, readStamp: StampReader?): string?
    local document, layout = jsondoc.parse(text)
    local changed = false
    local storage = jsondoc.get(document, "storage")
    if storage ~= nil and storage.kind == "object" then
        for _, service in foreignServices(side) do
            if jsondoc.get(storage, service) ~= nil then
                jsondoc.remove(storage, service)
                changed = true
            end
        end
        for _, key in storage.keys do
            if not DecidingServices[key] then
                changed = stripScripts(storage.values[key], side, readStamp) or changed
            end
        end
    end
    changed = stripScripts(jsondoc.get(document, "root"), side, readStamp) or changed
    return if changed then jsondoc.print(document, layout) else nil
end

--- A stamp's JSON without the other side's code (ADR 0138 §5). Nil when it
--- carried none.
local function stripStamp(text: string, side: Side, readStamp: StampReader?): string?
    local document, layout = jsondoc.parse(text)
    return if stripScripts(jsondoc.get(document, "root"), side, readStamp) then jsondoc.print(document, layout) else nil
end

--- `global.json` without the other side's folder of `GlobalScriptService`.
local function stripGlobal(text: string, side: Side): string?
    -- A game that is not dedicated carries both folders: its one package is
    -- the server and the player at once (the script-sides audit, D345).
    if side == "all" then
        return nil
    end
    local document, layout = jsondoc.parse(text)
    local root = jsondoc.get(document, "root")
    local children = if root ~= nil then jsondoc.get(root, "children") else nil
    if children == nil or children.kind ~= "array" then
        return nil
    end
    local foreign = if side == "client" then "Server" else "Client"
    local kept: { jsondoc.Node } = {}
    for _, child in children.items do
        if jsondoc.stringOf(jsondoc.get(child, "name")) ~= foreign then
            table.insert(kept, child)
        end
    end
    local changed = #kept ~= #children.items
    children.items = kept
    -- The `Server` and `Client` folders decide their scripts' side; anywhere
    -- else -- the `Shared` folder -- a script's own `RunContext` does (ADR 0138
    -- §2).
    for _, child in kept do
        local name = jsondoc.stringOf(jsondoc.get(child, "name"))
        if name ~= "Server" and name ~= "Client" then
            changed = stripScripts(child, side) or changed
        end
    end
    return if changed then jsondoc.print(document, layout) else nil
end

--- Every `*.scene.json` under `root`, and `global.json` at it, rewritten in
--- place for `side`. The staged file is unlinked before it is written, so the
--- project's own is never touched through the link.
local function stripStaged(root: string, side: Side, readStamp: StampReader, relative: string?)
    for _, entry in fs.listDirectory(root) do
        local file = projectlib.joined(root, entry.name)
        if entry.type == "dir" then
            stripStaged(file, side, readStamp, entry.name)
        elseif entry.type == "file" then
            local stripped: string? = nil
            if string.sub(entry.name, -11) == ".scene.json" then
                stripped = stripScene(fs.readFileToString(file), side, readStamp)
            elseif string.sub(entry.name, -11) == ".stamp.json" then
                stripped = stripStamp(fs.readFileToString(file), side, readStamp)
            elseif relative == nil and entry.name == "global.json" then
                stripped = stripGlobal(fs.readFileToString(file), side)
            end
            if stripped ~= nil then
                fs.remove(file)
                fs.writeStringToFile(file, stripped)
            end
        end
    end
end

--- `content/`, staged for `side` under `stage`. Returns the staged root.
local function stageContent(content: string, stage: string, side: Side): string
    removeTree(stage)
    local staged = projectlib.joined(stage, path.basename(content) or "content")
    linkTree(content, staged)
    -- A linked stamp's original, from the project: the staged copy may have
    -- lost a side's code already.
    stripStaged(staged, side, function(name: string): string?
        local file = projectlib.joined(content, name)
        return if fs.exists(file) then fs.readFileToString(file) else nil
    end)
    return staged
end

--- A server package says so in its own `project.toml`: `[network] role =
--- "server"` makes the player start as `--serve` with no arguments. Edited as
--- lines, because the file is the author's and a re-serialisation would lose
--- their comments.
local function withServerRole(text: string): string
    local lines = string.split(text, "\n")
    local inNetwork = false
    for index, line in lines do
        local header = string.match(line, "^%s*%[([^%]]+)%]")
        if header ~= nil then
            if inNetwork then
                -- Before the blank lines that end the section, not after them.
                local at = index
                while at > 1 and string.match(lines[at - 1], "^%s*$") do
                    at -= 1
                end
                table.insert(lines, at, 'role = "server"')
                return table.concat(lines, "\n")
            end
            inNetwork = header == "network"
        elseif inNetwork and string.match(line, "^%s*role%s*=") then
            lines[index] = 'role = "server"'
            return table.concat(lines, "\n")
        end
    end
    if inNetwork then
        return text .. (if string.sub(text, -1) == "\n" then "" else "\n") .. 'role = "server"\n'
    end
    return text .. (if string.sub(text, -1) == "\n" then "" else "\n") .. '\n[network]\nrole = "server"\n'
end

export type ComposeOptions = {
    project: projectlib.Project,
    -- `game/` in the output.
    game: string,
    side: Side,
    -- The content directory the project compiles (`[assets] content`).
    content: string,
    -- The pack a side's build wrote, which replaces the project's own.
    sidePack: string?,
    sideManifest: string?,
}

--- Lays `game/` out: the project's files, the source less the other side's,
--- `.engine/` less the editor's, and the pack -- or the loose content when
--- there is none.
local function compose(options: ComposeOptions)
    local project = options.project
    local game = options.game
    fs.createDirectory(game, { makeParents = true })

    local config = projectlib.joined(project.root, "project.toml")
    if fs.exists(config) then
        local text = fs.readFileToString(config)
        if options.side == "server" then
            text = withServerRole(text)
        end
        fs.writeStringToFile(projectlib.joined(game, "project.toml"), text)
    end
    local luaurc = projectlib.joined(project.root, ".luaurc")
    if fs.exists(luaurc) then
        fs.copy(luaurc, projectlib.joined(game, ".luaurc"))
    end

    -- The game's Luau, as source (ADR 0045).
    local src = projectlib.joined(project.root, "src")
    if fs.exists(src) then
        copyTree(src, projectlib.joined(game, "src"), function(relative: string): boolean
            return foreignScripts(options.side, relative)
        end)
    end

    -- The game's catalogs (ADR 0154): its text, which no pack holds.
    local catalogs = projectlib.joined(project.root, "i18n")
    if fs.exists(catalogs) then
        copyTree(catalogs, projectlib.joined(game, "i18n"))
    end

    local generated = projectlib.joined(project.root, ".engine")
    local target = projectlib.joined(game, ".engine")
    if fs.exists(generated) then
        fs.createDirectory(target, { makeParents = true })
        for _, entry in fs.listDirectory(generated) do
            if
                editorOnly(entry.name)
                or Regenerated[entry.name]
                or (options.sidePack ~= nil and PackFiles[entry.name])
            then
                continue
            end
            local from = projectlib.joined(generated, entry.name)
            local to = projectlib.joined(target, entry.name)
            if entry.type == "dir" then
                copyTree(from, to)
            elseif entry.type == "file" then
                fs.copy(from, to)
            end
        end
    end
    if options.sidePack ~= nil then
        fs.createDirectory(target, { makeParents = true })
        fs.copy(options.sidePack, projectlib.joined(target, "content.lpack"))
        if options.sideManifest ~= nil and fs.exists(options.sideManifest) then
            fs.copy(options.sideManifest, projectlib.joined(target, "content.manifest.json"))
        end
        -- The streamed terrain's cells, beside the pack that left them out:
        -- at `content/`, where the engine looks for them (D411). Both sides
        -- of a dedicated game carry them -- the server stands on the ground
        -- too.
        if fs.exists(options.content) then
            copyCells(options.content, projectlib.joined(game, "content"))
        end
        return
    end

    -- The loose source trees ship only when there is no pack to replace them.
    -- Two directories rather than one: `content/` is what the engine mounts and
    -- `build-assets` compiles, `assets/` is what the starter scaffolds
    -- (api-design.md §4).
    local pack = projectlib.joined(generated, "content.lpack")
    if not fs.exists(pack) then
        if fs.exists(options.content) then
            copyTree(options.content, projectlib.joined(game, path.basename(options.content) or "content"))
        end
        local assets = projectlib.joined(project.root, "assets")
        if fs.exists(assets) and assets ~= options.content then
            copyTree(assets, projectlib.joined(game, "assets"))
        end
    end
end

return {
    editorOnly = editorOnly,
    foreignScripts = foreignScripts,
    stripScene = stripScene,
    stripGlobal = stripGlobal,
    stripStamp = stripStamp,
    stageContent = stageContent,
    copyCells = copyCells,
    withServerRole = withServerRole,
    compose = compose,
    copyTree = copyTree,
    removeTree = removeTree,
}
