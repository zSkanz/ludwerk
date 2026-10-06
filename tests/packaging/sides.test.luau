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
local process = require("@std/process")
local test = require("@std/test")

local build = require("../../tools/cli/commands/build")
local new = require("../../tools/cli/commands/new")
local projectlib = require("../../tools/cli/project")

--- The last five are CODE, not names (ADR 0138 §5): a `Script` anywhere else
--- whose `RunContext` is one side's keeps its instance in both packages and its
--- code in that side's only. Found in the compiled scripts' bytecode, which the
--- search decodes (`decodedScripts`).
local ServerStrings = {
    "SENTINEL-SERVER-SCRIPT",
    "SENTINEL-SCENE-SERVER-SCRIPT",
    "SENTINEL-SERVER-STORAGE",
    "SENTINEL-SERVER-SCRIPT-SERVICE",
    "SENTINEL-GLOBAL-SERVER",
    "SENTINEL-WORKSPACE-SERVER-CODE",
    "SENTINEL-PART-SERVER-CODE",
    "SENTINEL-STAMP-SERVER-CODE",
    "SENTINEL-SHARED-FOLDER-SERVER-CODE",
    "SENTINEL-STORAGE-MODULE-CODE",
}

local ClientStrings = {
    "SENTINEL-CLIENT-SCRIPT",
    "SENTINEL-SCENE-CLIENT-SCRIPT",
    "SENTINEL-CLIENT-SCRIPT-SERVICE",
    "SENTINEL-GLOBAL-CLIENT",
    "SENTINEL-WORKSPACE-CLIENT-CODE",
    "SENTINEL-STAMP-CLIENT-CODE",
}

--- **In both sides' packages, compiled** (S0.3): a script in the scene's
--- `Workspace` and one in a stamp run everywhere, so both packages carry them --
--- as bytecode, so only their names are searchable, and their source is not.
local BothStrings = {
    "SENTINEL-WORKSPACE-SCRIPT",
    "SENTINEL-STAMP-SCRIPT",
    -- The instances a side's code was taken out of: in both packages.
    "SENTINEL-WORKSPACE-SERVER-NAME",
    "SENTINEL-WORKSPACE-CLIENT-NAME",
    "SENTINEL-STAMP-CLIENT-NAME",
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
    -- The editor's copy of a script file that changed under it, and the
    -- developer's own save slots (the script-sides audit).
    "SENTINEL-CONFLICT",
    "SENTINEL-SAVE-SLOT",
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

--- A `Script` node named `name`, printing `code`, on `side` (nil: `Shared`).
local function sided(name: string, code: string, side: string?): string
    local context = if side ~= nil then `, "RunContext": "{side}"` else ""
    return `\{"class": "Script", "name": "{name}", "properties": \{"Source": {jsonString(script(code))}{context}\}\}`
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
    write(
        projectlib.joined(project, ".engine", "conflicts", "src", "server", "Secret.luau"),
        script("SENTINEL-CONFLICT")
    )
    write(projectlib.joined(project, ".engine", "saves", "slot.json"), '{"note": "SENTINEL-SAVE-SLOT"}')

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
            )}\}\},`,
            "    " .. sided("SENTINEL-WORKSPACE-SERVER-NAME", "SENTINEL-WORKSPACE-SERVER-CODE", "Server") .. ",",
            "    " .. sided("SENTINEL-WORKSPACE-CLIENT-NAME", "SENTINEL-WORKSPACE-CLIENT-CODE", "Client") .. ",",
            `    \{"class": "Part", "name": "Crate", "children": [{sided(
                "Guard",
                "SENTINEL-PART-SERVER-CODE",
                "Server"
            )}]\}`,
            "  ]},",
            '  "storage": {',
            `    "ServerStorage": \{"class": "ServerStorage", "name": "ServerStorage", "children": [{folder(
                "SENTINEL-SERVER-STORAGE"
            )}, \{"class": "ModuleScript", "name": "Keys", "properties": \{"Source": {jsonString(
                "return '" .. "SENTINEL-STORAGE-MODULE-CODE" .. "'\n"
            )}\}\}]\},`,
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
            )}\}\}, {sided("Open", "SENTINEL-STAMP-SERVER-CODE", "Server")}, {sided(
                "SENTINEL-STAMP-CLIENT-NAME",
                "SENTINEL-STAMP-CLIENT-CODE",
                "Client"
            )}]\}`,
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
            `    \{"class": "Folder", "name": "Client", "children": [{folder("SENTINEL-GLOBAL-CLIENT")}]\},`,
            `    \{"class": "Folder", "name": "Shared", "children": [{sided(
                "Tally",
                "SENTINEL-SHARED-FOLDER-SERVER-CODE",
                "Server"
            )}]\}`,
            "  ]}",
            "}",
            "",
        }, "\n")
    )
    return project
end

local Base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64Decode(text: string): string
    local values: { [string]: number } = {}
    for index = 1, #Base64 do
        values[string.sub(Base64, index, index)] = index - 1
    end
    local out: { string } = {}
    local bits, count = 0, 0
    for index = 1, #text do
        local value = values[string.sub(text, index, index)]
        if value ~= nil then
            bits = bit32.bor(bit32.lshift(bits, 6), value)
            count += 6
            if count >= 8 then
                count -= 8
                table.insert(out, string.char(bit32.band(bit32.rshift(bits, count), 0xFF)))
            end
        end
    end
    return table.concat(out)
end

--- **Every compiled scene, stamp and `global.json` script in `text`, decoded**
--- (S0.3: `luauc:` and the bytecode in base64). A string a script prints is a
--- constant in its bytecode, so a side's code is found here or nowhere.
local function decodedScripts(text: string): string
    local out: { string } = {}
    for encoded in string.gmatch(text, "luauc:([A-Za-z0-9+/=]+)") do
        table.insert(out, base64Decode(encoded))
    end
    return table.concat(out, "\n")
end

--- Every file under `root`, read whole.
local function everyFile(root: string, out: { [string]: string })
    for _, entry in fs.listDirectory(root) do
        local file = projectlib.joined(root, entry.name)
        if entry.type == "dir" then
            everyFile(file, out)
        elseif entry.type == "file" then
            local text = fs.readFileToString(file)
            out[file] = text .. decodedScripts(text)
        end
    end
end

--- **What a sealed game holds, as files to search** (ADR 0183): its own
--- files at the paths they had, and every scene, stamp and material by its
--- hash -- written beside the output by `assetc unseal`. A sealed pack is
--- compressed, so a string that leaked into one would be found here or nowhere;
--- the raw files are searched as well, which is what says the sealing worked.
local function unsealed(output: string): string?
    local game = projectlib.joined(output, "game")
    if fs.exists(projectlib.joined(game, "src")) or fs.exists(projectlib.joined(game, "project.toml")) then
        return nil
    end
    local assetc = projectlib.findTool("assetc")
    assert.that(assetc ~= nil, "no asset compiler was found to unseal the game with")
    local into = output .. "-unsealed"
    removeTree(into)
    local opened = process.run({ assetc :: string, "unseal", game, "--out", into })
    assert.that(opened.ok, `the game at {game} could not be unsealed:\n{opened.stdout}{opened.stderr}`)
    return into
end

local function checkSide(output: string, own: { string }, foreign: { string }, sealed: boolean?)
    local files: { [string]: string } = {}
    everyFile(projectlib.joined(output, "game"), files)
    local opened = unsealed(output)
    assert.eq(opened ~= nil, sealed ~= false)
    if opened ~= nil then
        -- Sealed: nothing of the game is a file of its own, and nothing of
        -- either side is readable in what is.
        for file, text in files do
            assert.that(string.sub(file, -6) ~= ".luauc", `{file} is a script beside a sealed pack`)
            for _, sentinel in own do
                assert.that(string.find(text, sentinel, 1, true) == nil, `{sentinel} can be read in {file}`)
            end
        end
        everyFile(opened, files)
    end
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
        local toml =
            fs.readFileToString(projectlib.joined(project, "dist", `{desktop}-server-unsealed`, "project.toml"))
        assert.that(string.find(toml, 'role = "server"', 1, true) ~= nil, "the server package has no role")

        -- And a server target of a game that is not dedicated is refused.
        local config = projectlib.joined(project, "project.toml")
        fs.writeStringToFile(config, string.gsub(fs.readFileToString(config), '"dedicated"', '"host"'))
        assert.eq(
            build.run({ path = project, output = nil, target = `{desktop}-server`, force = false, devHost = true }),
            1
        )

        -- **A game that is not dedicated carries both sides** -- players host,
        -- or one plays alone -- with every script's code compiled and none of
        -- the editor's trash (the script-sides audit, S3).
        local everything = withBoth(ServerStrings)
        for _, sentinel in ClientStrings do
            table.insert(everything, sentinel)
        end
        for _, mode in { "host", "none" } do
            local text = fs.readFileToString(config)
            text = string.gsub(text, 'multiplayer = "[a-z]+"', `multiplayer = "{mode}"`)
            fs.writeStringToFile(config, text)
            assert.eq(build.run({ path = project, output = nil, target = desktop, force = true, devHost = true }), 0)
            checkSide(projectlib.joined(project, "dist", desktop), everything, NowhereStrings)
        end

        -- **And a game that says `packed = false` is the folder of files it
        -- was** (ADR 0183): every script compiled under its own name, the
        -- settings and the manifest beside them, the same strings in them.
        local keyed, added = string.gsub(
            fs.readFileToString(config),
            '(multiplayer = "[a-z]+")',
            "%1" .. string.char(10) .. "packed = false",
            1
        )
        assert.eq(added, 1)
        fs.writeStringToFile(config, keyed)
        assert.eq(build.run({ path = project, output = nil, target = desktop, force = true, devHost = true }), 0)
        checkSide(projectlib.joined(project, "dist", desktop), everything, NowhereStrings, false)
        local looseGame = projectlib.joined(project, "dist", desktop, "game")
        assert.that(
            fs.exists(projectlib.joined(looseGame, "project.toml")),
            "a game that is not packed lost its settings"
        )
        assert.that(
            fs.exists(projectlib.joined(looseGame, ".engine", "content.manifest.json")),
            "a game that is not packed lost its manifest"
        )

        removeTree(parent)
    end)
end)
