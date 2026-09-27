--!strict
--[[
	The `game/` folder every target carries (ADR 0104 §7), and what a
	dedicated game's two sides leave out of it (ADR 0105).

	**A side is decided by leaving files out, never by a flag the game reads.**
	A client package that carried the server's scripts and told the engine not
	to run them would still hand every player the server's code. So a
	`client` package has no `src/server/`, no `src/scenes/*/server/`, no
	scene's `ServerScriptService` or `ServerStorage`, and no `Server` folder in
	`global.json`; a `server` package has none of the client's. The sentinel
	test (`tests/packaging/sides.test.luau`) searches each side's output for the
	other's strings, pack included, because a pack stores what it carries
	uncompressed and a miss there is a leak.

	The pack is rebuilt per side from a staged copy of `content/` -- hard links
	where the file system allows, so a game with a gigabyte of art does not copy
	it twice -- in which only the scenes and `global.json` are rewritten.
]]

local fs = require("@std/fs")
local path = require("@std/path")

local jsondoc = require("../jsondoc")
local projectlib = require("../project")

export type Side = "all" | "client" | "server"

--- **What `.engine/` holds that is the editor's, not the game's**: its layout,
--- its settings, the import and check caches. A game carries the pack, its
--- manifest and the partition cache, and none of these.
local EditorOnly = { "editor.json", "import", "check", "types", "match", "export" }
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

--- A tree of hard links to `from`, or copies where a link is refused (another
--- volume, a file system without them). Only for trees nothing writes into.
local function linkTree(from: string, to: string)
    fs.createDirectory(to, { makeParents = true })
    for _, entry in fs.listDirectory(from) do
        local source = projectlib.joined(from, entry.name)
        local target = projectlib.joined(to, entry.name)
        if entry.type == "dir" then
            linkTree(source, target)
        elseif entry.type == "file" then
            if not pcall(fs.link, source, target) then
                fs.copy(source, target)
            end
        end
    end
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

--- A scene's JSON without the other side's services. Nil when nothing in it
--- belonged to the other side, so the staged file stays a link.
local function stripScene(text: string, side: Side): string?
    local document, layout = jsondoc.parse(text)
    local storage = jsondoc.get(document, "storage")
    if storage == nil or storage.kind ~= "object" then
        return nil
    end
    local changed = false
    for _, service in foreignServices(side) do
        if jsondoc.get(storage, service) ~= nil then
            jsondoc.remove(storage, service)
            changed = true
        end
    end
    return if changed then jsondoc.print(document, layout) else nil
end

--- `global.json` without the other side's folder of `GlobalScriptService`.
local function stripGlobal(text: string, side: Side): string?
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
    if #kept == #children.items then
        return nil
    end
    children.items = kept
    return jsondoc.print(document, layout)
end

--- Every `*.scene.json` under `root`, and `global.json` at it, rewritten in
--- place for `side`. The staged file is unlinked before it is written, so the
--- project's own is never touched through the link.
local function stripStaged(root: string, side: Side, relative: string?)
    for _, entry in fs.listDirectory(root) do
        local file = projectlib.joined(root, entry.name)
        if entry.type == "dir" then
            stripStaged(file, side, entry.name)
        elseif entry.type == "file" then
            local stripped: string? = nil
            if string.sub(entry.name, -11) == ".scene.json" then
                stripped = stripScene(fs.readFileToString(file), side)
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
    stripStaged(staged, side)
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
    stageContent = stageContent,
    withServerRole = withServerRole,
    compose = compose,
    copyTree = copyTree,
    removeTree = removeTree,
}
