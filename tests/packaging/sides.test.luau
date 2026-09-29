--!strict
--[[
	**The sentinel test** (ADR 0105, ADR 0104 stage 2): a dedicated game's
	client package carries none of the server's code or storage, and its server
	package none of the client's.

	One project, with a distinct string in every place code or storage can
	live on each side -- a script in `src/server/` and `src/client/`, one in
	each of the scene's own script folders, an instance in each scene service
	and each folder of `global.json` -- exported both ways. Every byte of each
	output is then searched for the other side's strings, the content pack
	included: a pack stores what it carries uncompressed, so a scene that
	leaked into it would be found here. And each side's OWN strings have to be
	found, or the search proves nothing.

	On this machine's own desktop target: `windows` and `windows-server` here,
	`linux` and `linux-server` in the Linux container. `--dev-host` when no
	player is built, because what is under test is what the package leaves
	out, not which binary it copies.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local platform = require("@std/system/platform")
local test = require("@std/test")

local build = require("../../tools/cli/commands/build")
local new = require("../../tools/cli/commands/new")
local projectlib = require("../../tools/cli/project")

local ServerStrings = {
    "SENTINEL-SERVER-SCRIPT",
    "SENTINEL-SCENE-SERVER-SCRIPT",
    "SENTINEL-SERVER-STORAGE",
    "SENTINEL-SERVER-SCRIPT-SERVICE",
    "SENTINEL-GLOBAL-SERVER",
}

local ClientStrings = {
    "SENTINEL-CLIENT-SCRIPT",
    "SENTINEL-SCENE-CLIENT-SCRIPT",
    "SENTINEL-CLIENT-SCRIPT-SERVICE",
    "SENTINEL-GLOBAL-CLIENT",
}

--- **In both sides' packages, compiled** (S0.3): a script in the scene's
--- `Workspace` and one in a stamp run everywhere, so both packages carry them --
--- as bytecode, so only their names are searchable, and their source is not.
local BothStrings = {
    "SENTINEL-WORKSPACE-SCRIPT",
    "SENTINEL-STAMP-SCRIPT",
}

--- `own` and `BothStrings`, one list.
local function withBoth(own: { string }): { string }
    local all = table.clone(own)
    for _, sentinel in BothStrings do
        table.insert(all, sentinel)
    end
    return all
end

--- **In neither side's package** (S0.1): what the editor moved to
--- `.engine/trash/` when a script's file was orphaned is the editor's, and it
--- was server code as often as not.
local NowhereStrings = {
    "SENTINEL-TRASH",
}

--- `foreign` and `NowhereStrings`, one list.
local function withNowhere(foreign: { string }): { string }
    local all = table.clone(foreign)
    for _, sentinel in NowhereStrings do
        table.insert(all, sentinel)
    end
    return all
end

local function repositoryRoot(): string
    local here = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tests/packaging"
    return projectlib.joined(here, "..", "..")
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

local function write(file: string, text: string)
    local parent = string.match(file, "^(.*)[/\\][^/\\]+$")
    if parent ~= nil then
        fs.createDirectory(parent, { makeParents = true })
    end
    fs.writeStringToFile(file, text)
end

--- In a comment of every script, so only its source carries it: the compiled
--- package must not (ADR 0112), while the strings the scripts print stay in
--- the bytecode and prove the search reached them.
local SourceOnly = "SENTINEL-SOURCE-ONLY"

local function script(name: string): string
    return `--!strict\n-- {SourceOnly}\nprint("{name}")\n`
end

--- `text` as a JSON string literal.
local function jsonString(text: string): string
    local escaped = string.gsub(text, '[%c"\\]', function(c: string): string
        if c == '"' then
            return '\\"'
        elseif c == "\\" then
            return "\\\\"
        elseif c == "\n" then
            return "\\n"
        end
        return string.format("\\u%04x", string.byte(c) or 0)
    end)
    return `"{escaped}"`
end

local function folder(name: string): string
    return `\{"class": "Folder", "name": "{name}"\}`
end

local function service(class: string, child: string): string
    return `"{class}": \{"class": "{class}", "name": "{class}", "children": [{folder(child)}]\}`
end

--- The starter, made dedicated, with a sentinel in every place.
local function scaffold(parent: string): string
    removeTree(parent)
    fs.createDirectory(parent, { makeParents = true })
    assert.eq(new.run({ name = "sidestest", template = "starter", parent = parent }), 0)
    local project = projectlib.joined(parent, "sidestest")

    local config = projectlib.joined(project, "project.toml")
    fs.writeStringToFile(config, fs.readFileToString(config) .. '\n[export]\nmultiplayer = "dedicated"\n')

    local src = projectlib.joined(project, "src")
    write(projectlib.joined(src, "server", "Secret.luau"), script("SENTINEL-SERVER-SCRIPT"))
    write(projectlib.joined(src, "client", "Hud.luau"), script("SENTINEL-CLIENT-SCRIPT"))
    write(projectlib.joined(src, "scenes", "main", "server", "Rules.luau"), script("SENTINEL-SCENE-SERVER-SCRIPT"))
    write(projectlib.joined(src, "scenes", "main", "client", "View.luau"), script("SENTINEL-SCENE-CLIENT-SCRIPT"))
    write(
        projectlib.joined(project, ".engine", "trash", "20260929-120000", "src", "server", "Old.luau"),
        script("SENTINEL-TRASH")
    )

    local content = projectlib.joined(project, "content")
    write(
        projectlib.joined(content, "scenes", "main.scene.json"),
        table.concat({
            "{",
            '  "format": "scene",',
            '  "version": 2,',
            '  "root": {"class": "Workspace", "name": "Workspace", "children": [',
            `    \{"class": "Script", "name": "SENTINEL-WORKSPACE-SCRIPT", "properties": \{"Source": {jsonString(
                script("SENTINEL-WORKSPACE-PRINT")
            )}\}\}`,
            "  ]},",
            '  "storage": {',
            "    " .. service("ServerStorage", "SENTINEL-SERVER-STORAGE") .. ",",
            "    " .. service("ServerScriptService", "SENTINEL-SERVER-SCRIPT-SERVICE") .. ",",
            "    " .. service("ClientScriptService", "SENTINEL-CLIENT-SCRIPT-SERVICE"),
            "  }",
            "}",
            "",
        }, "\n")
    )
    write(
        projectlib.joined(content, "stamps", "door.stamp.json"),
        table.concat({
            "{",
            '  "format": "scene",',
            '  "version": 2,',
            `  "root": \{"class": "Model", "name": "Door", "children": [\{"class": "Script", "name": "SENTINEL-STAMP-SCRIPT", "properties": \{"Source": {jsonString(
                script("SENTINEL-STAMP-PRINT")
            )}\}\}]\}`,
            "}",
            "",
        }, "\n")
    )
    write(
        projectlib.joined(content, "global.json"),
        table.concat({
            "{",
            '  "format": "global",',
            '  "version": 1,',
            '  "root": {"class": "GlobalScriptService", "name": "GlobalScriptService", "children": [',
            `    \{"class": "Folder", "name": "Server", "children": [{folder("SENTINEL-GLOBAL-SERVER")}]\},`,
            `    \{"class": "Folder", "name": "Client", "children": [{folder("SENTINEL-GLOBAL-CLIENT")}]\}`,
            "  ]}",
            "}",
            "",
        }, "\n")
    )
    return project
end

--- Every file under `root`, read whole.
local function everyFile(root: string, out: { [string]: string })
    for _, entry in fs.listDirectory(root) do
        local file = projectlib.joined(root, entry.name)
        if entry.type == "dir" then
            everyFile(file, out)
        elseif entry.type == "file" then
            out[file] = fs.readFileToString(file)
        end
    end
end

local function checkSide(output: string, own: { string }, foreign: { string })
    local files: { [string]: string } = {}
    everyFile(projectlib.joined(output, "game"), files)
    for _, sentinel in foreign do
        for file, text in files do
            assert.that(string.find(text, sentinel, 1, true) == nil, `{sentinel} leaked into {file}`)
        end
    end
    -- Bytecode, not source: no script's text and no `.luau` in the game.
    for file, text in files do
        assert.that(string.find(text, SourceOnly, 1, true) == nil, `a script's source shipped in {file}`)
        assert.that(string.sub(file, -5) ~= ".luau", `{file} shipped as source`)
    end
    for _, sentinel in own do
        local found = false
        for _, text in files do
            if string.find(text, sentinel, 1, true) ~= nil then
                found = true
                break
            end
        end
        assert.that(found, `{sentinel} is missing from its own side's package under {output}`)
    end
end

test.suite("dedicated sides", function(suite)
    suite:case("each side's package carries none of the other's code or storage", function()
        local desktop = if platform.win32 then "windows" elseif platform.linux then "linux" else nil
        if desktop == nil then
            return
        end
        local parent = projectlib.joined(repositoryRoot(), ".engine-sides-test")
        local project = scaffold(parent)

        assert.eq(build.run({ path = project, output = nil, target = desktop, force = false, devHost = true }), 0)
        assert.eq(
            build.run({ path = project, output = nil, target = `{desktop}-server`, force = false, devHost = true }),
            0
        )

        checkSide(projectlib.joined(project, "dist", desktop), withBoth(ClientStrings), withNowhere(ServerStrings))
        checkSide(
            projectlib.joined(project, "dist", `{desktop}-server`),
            withBoth(ServerStrings),
            withNowhere(ClientStrings)
        )

        -- The server's package says what it is, so it serves with no arguments.
        local toml = fs.readFileToString(projectlib.joined(project, "dist", `{desktop}-server`, "game", "project.toml"))
        assert.that(string.find(toml, 'role = "server"', 1, true) ~= nil, "the server package has no role")

        -- And a server target of a game that is not dedicated is refused.
        local config = projectlib.joined(project, "project.toml")
        fs.writeStringToFile(config, string.gsub(fs.readFileToString(config), '"dedicated"', '"host"'))
        assert.eq(
            build.run({ path = project, output = nil, target = `{desktop}-server`, force = false, devHost = true }),
            1
        )

        removeTree(parent)
    end)
end)
