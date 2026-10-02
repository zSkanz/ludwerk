--!strict
--[[
	The parts of `ludwerk build` that decide something without running anything
	(ADR 0104 stage 2, ADR 0105): which targets exist and where each one's
	player is looked for, what each side of a dedicated game leaves out, and
	the role a server package's `project.toml` is given.

	Each is a pure function, so the order and the cut are held still here
	rather than found out from an export that shipped the wrong thing.
]]

local assert = require("@std/test/assert")
local test = require("@std/test")

local android = require("../export/android")
local brand = require("../brand")
local game = require("../export/game")
local progress = require("../export/progress")
local targets = require("../export/targets")
local versionCode = require("../export/version_code")

test.suite("export", function(suite)
    -- D465: a package that had no brand file signed with one key and the
    -- repository's CLI with another, and an APK from one would not install
    -- over the other's. What a debug build is signed with, and where the
    -- toolchains are, is no longer a function of what the engine is called.
    suite:case("the debug key and the tools folder do not carry the brand's name", function()
        local name = brand.read().Name
        local key = string.gsub(android.debugKeystorePath(nil), "\\", "/")
        assert.that(string.find(key, "/.android/debug.keystore", 1, true) ~= nil, key)
        assert.eq(string.find(key, name, 1, true), nil)
        assert.eq(android.DebugAlias, "androiddebugkey")
        assert.eq(android.DebugPassword, "android")
        local tools = string.gsub(android.toolsFolder(), "\\", "/")
        assert.eq(string.find(tools, `/{name}`, 1, true), nil)
        assert.eq(android.debugKeystorePath("keys/team.keystore"), "keys/team.keystore")
    end)

    suite:case("the targets, and v1's name for the first", function()
        local names: { string } = {}
        for _, target in targets.All do
            table.insert(names, target.name)
        end
        assert.eq(table.concat(names, ","), "windows,linux,android,windows-server,linux-server")
        assert.eq((targets.find("win64") :: targets.Target).name, "windows")
        assert.that((targets.find("linux-server") :: targets.Target).server, "linux-server is a server")
        assert.eq(targets.find("macos"), nil)
    end)

    suite:case("a player is looked for: the variable, the installation, then the build tree", function()
        local linux = targets.find("linux") :: targets.Target
        -- Separators as this machine joins them, compared as forward slashes.
        local function slashed(list: { string }): { string }
            local out: { string } = {}
            for _, entry in list do
                table.insert(out, (string.gsub(entry, "[/\\]+", "/")))
            end
            return out
        end
        local candidates = slashed(targets.playerCandidates(linux, {
            explicit = "/opt/player",
            install = "/ludwerk",
            buildRoot = "/build",
        }))
        assert.eq(candidates[1], "/opt/player")
        assert.that(string.find(candidates[2], "player/linux-x64/engine-host", 1, true) ~= nil, candidates[2])
        assert.that(
            string.find(candidates[#candidates], "linux-clang-player/engine/app/engine-host", 1, true) ~= nil,
            candidates[#candidates]
        )

        local android = targets.find("android") :: targets.Target
        local phone = slashed(targets.playerCandidates(android, { install = "/ludwerk", buildRoot = "/build" }))
        assert.eq(#phone, 2)
        assert.that(string.find(phone[1], "player/android-arm64/libmain.so", 1, true) ~= nil, phone[1])
    end)

    suite:case("each side leaves the other's script folders out", function()
        assert.that(game.foreignScripts("client", "server"), "a client carries src/server")
        assert.that(game.foreignScripts("client", "scenes/arena/server"), "a client carries a scene's server")
        assert.that(not game.foreignScripts("client", "scenes/arena/client"), "a client lost its scene's code")
        assert.that(not game.foreignScripts("client", "shared"), "a client lost the shared code")
        assert.that(game.foreignScripts("server", "client"), "a server carries src/client")
        -- `src/scripts/` is the legacy spelling of `src/client/`.
        assert.that(game.foreignScripts("server", "scripts"), "a server carries src/scripts")
        assert.that(game.foreignScripts("server", "scenes/arena/client"), "a server carries a scene's client")
        assert.that(not game.foreignScripts("all", "server"), "a solo game lost its server")
    end)

    suite:case("a scene loses the other side's services, and keeps its key order", function()
        local scene = '{\n  "format": "scene",\n  "root": {"class": "Workspace"},\n  "storage": {\n'
            .. '    "ReplicatedStorage": {"class": "ReplicatedStorage"},\n'
            .. '    "ServerStorage": {"class": "ServerStorage"},\n'
            .. '    "ServerScriptService": {"class": "ServerScriptService"},\n'
            .. '    "ClientScriptService": {"class": "ClientScriptService"}\n  }\n}\n'
        local client = game.stripScene(scene, "client") :: string
        assert.that(string.find(client, "ServerStorage", 1, true) == nil, client)
        assert.that(string.find(client, "ServerScriptService", 1, true) == nil, client)
        assert.that(string.find(client, "ClientScriptService", 1, true) ~= nil, client)
        assert.that(
            (string.find(client, "format", 1, true) :: number) < (string.find(client, "storage", 1, true) :: number),
            client
        )

        local server = game.stripScene(scene, "server") :: string
        assert.that(string.find(server, "ClientScriptService", 1, true) == nil, server)
        assert.that(string.find(server, "ServerStorage", 1, true) ~= nil, server)

        -- Nothing of the other side's in it: nothing to rewrite.
        assert.eq(game.stripScene('{"root": {"class": "Workspace"}}', "client"), nil)
    end)

    suite:case("a script keeps its instance and loses the other side's code, wherever it is (ADR 0138 §5)", function()
        local door = '{"format": "scene", "root": {"class": "Model", "name": "Door", "children": ['
            .. '{"class": "Script", "name": "Rules", "properties": {"Source": "RULES", "RunContext": "Server"}}, '
            .. '{"class": "Part", "name": "Leaf", "children": ['
            .. '{"class": "Script", "name": "Creak", "properties": {"Source": "CREAK", "RunContext": "Client"}}]}, '
            .. '{"class": "Script", "name": "Both", "properties": {"Source": "BOTH"}}]}}'
        local client = game.stripStamp(door, "client") :: string
        assert.that(string.find(client, "RULES", 1, true) == nil, client)
        assert.that(string.find(client, '"Rules"', 1, true) ~= nil, client)
        assert.that(string.find(client, "CREAK", 1, true) ~= nil, client)
        assert.that(string.find(client, "BOTH", 1, true) ~= nil, client)
        local server = game.stripStamp(door, "server") :: string
        assert.that(string.find(server, "CREAK", 1, true) == nil, server)
        assert.that(string.find(server, '"Creak"', 1, true) ~= nil, server)
        assert.that(string.find(server, "RULES", 1, true) ~= nil, server)
        -- A solo or host package keeps everything.
        assert.eq(game.stripStamp(door, "all"), nil)

        -- A scene's world and a global.json's Shared folder likewise.
        local scene = '{"format": "scene", "root": {"class": "Workspace", "children": ['
            .. '{"class": "Script", "name": "Rules", "properties": {"Source": "RULES", "RunContext": "Server"}}]}}'
        assert.that(string.find(game.stripScene(scene, "client") :: string, "RULES", 1, true) == nil)
        local global = '{"format": "global", "root": {"class": "GlobalScriptService", "children": ['
            .. '{"class": "Folder", "name": "Shared", "children": ['
            .. '{"class": "Script", "name": "Sound", "properties": {"Source": "SOUND", "RunContext": "Client"}}]}]}}'
        assert.that(string.find(game.stripGlobal(global, "server") :: string, "SOUND", 1, true) == nil)
    end)

    suite:case("inside a script service the service decides, not RunContext (the script-sides audit)", function()
        local scene = '{"format": "scene", "root": {"class": "Workspace"}, "storage": {'
            .. '"ClientScriptService": {"class": "ClientScriptService", "name": "ClientScriptService", "children": ['
            .. '{"class": "Script", "name": "Hud", "properties": {"Source": "KEEP-CLIENT-SERVICE", "RunContext": "Server"}}]}}}'
        local client = game.stripScene(scene, "client")
        assert.that(client == nil or string.find(client, "KEEP-CLIENT-SERVICE", 1, true) ~= nil, client)
        local global = '{"format": "global", "root": {"class": "GlobalScriptService", "children": ['
            .. '{"class": "Folder", "name": "Server", "children": ['
            .. '{"class": "Script", "name": "Rules", "properties": {"Source": "KEEP-GLOBAL-SERVER", "RunContext": "Client"}}]}]}}'
        local server = game.stripGlobal(global, "server")
        assert.that(server == nil or string.find(server, "KEEP-GLOBAL-SERVER", 1, true) ~= nil, server)
    end)

    suite:case("a linked stamp's overrides follow the side the instance runs on (the script-sides audit)", function()
        local stamps: { [string]: string } = {
            ["stamps/door.stamp.json"] = '{"format": "scene", "root": {"class": "Model", "name": "Door", "children": ['
                .. '{"class": "Script", "name": "Latch", "properties": {"Source": "LATCH-SHARED"}}, '
                .. '{"class": "Script", "name": "Guard", "properties": {"Source": "GUARD-SERVER", "RunContext": "Server"}}]}}',
        }
        local function read(name: string): string?
            return stamps[name]
        end
        -- The latch made server code in this instance, with its text edited;
        -- the guard made client code, with none of its own.
        local scene = '{"format": "scene", "root": {"class": "Workspace", "children": ['
            .. '{"name": "Front", "stamp": "stamps/door.stamp.json", "overrides": {'
            .. '"Latch": {"RunContext": "Server", "Source": "LATCH-EDITED"}, '
            .. '"Guard": {"RunContext": "Client"}}}]}}'
        local client = game.stripScene(scene, "client", read) :: string
        assert.that(client ~= nil, "nothing was stripped")
        assert.that(string.find(client, "LATCH-EDITED", 1, true) == nil, client)
        -- The client runs the guard, whose code its package's stamp file lost.
        assert.that(string.find(client, "GUARD-SERVER", 1, true) ~= nil, client)
        local server = game.stripScene(scene, "server", read) :: string
        assert.that(string.find(server, "LATCH-EDITED", 1, true) ~= nil, server)
        assert.that(string.find(server, "GUARD-SERVER", 1, true) == nil, server)
    end)

    suite:case("the editor's conflicts and save slots are not the game's (the script-sides audit)", function()
        assert.eq(game.editorOnly("conflicts"), true)
        assert.eq(game.editorOnly("saves"), true)
    end)

    suite:case("global.json loses the other side's folder", function()
        local global = '{"format": "global", "root": {"class": "GlobalScriptService", "children": ['
            .. '{"class": "Folder", "name": "Server"}, {"class": "Folder", "name": "Client"}, '
            .. '{"class": "Folder", "name": "Shared"}]}}'
        local client = game.stripGlobal(global, "client") :: string
        assert.that(string.find(client, '"Server"', 1, true) == nil, client)
        assert.that(string.find(client, '"Shared"', 1, true) ~= nil, client)
        local server = game.stripGlobal(global, "server") :: string
        assert.that(string.find(server, '"Client"', 1, true) == nil, server)
        -- A game that is not dedicated keeps both (the script-sides audit).
        assert.eq(game.stripGlobal(global, "all"), nil)
    end)

    suite:case("a server package's project.toml says it is one", function()
        assert.eq(
            game.withServerRole('[project]\nname = "g"\n'),
            '[project]\nname = "g"\n\n[network]\nrole = "server"\n'
        )
        assert.eq(
            game.withServerRole('[network]\nserver = "a:1"\n\n[window]\n'),
            '[network]\nserver = "a:1"\nrole = "server"\n\n[window]\n'
        )
        assert.eq(game.withServerRole('[network]\nrole = "client"\n'), '[network]\nrole = "server"\n')
        assert.eq(game.withServerRole('[network]\nserver = "a:1"\n'), '[network]\nserver = "a:1"\nrole = "server"\n')
    end)

    suite:case("the editor's files stay out of a game", function()
        for _, name in { "editor.json", "import", "check", "types", "match", "export", "editor-layout.v3.ini" } do
            assert.that(game.editorOnly(name), `{name} would ship`)
        end
        for _, name in { "content.lpack", "content.manifest.json", "partition" } do
            assert.that(not game.editorOnly(name), `{name} would not ship`)
        end
    end)

    suite:case("--bump-version-code adds one in place, comments and line endings kept", function()
        local function bumped(text: string): (string, number)
            return versionCode.bump(text)
        end
        local text, code = bumped('[project]\nname = "g"\n')
        assert.eq(text, '[project]\nname = "g"\n\n[export.android]\nversion_code = 2\n')
        assert.eq(code, 2)

        text, code = bumped("[export.android]\nversion_code = 7 # the store's\n\n[window]\n")
        assert.eq(text, "[export.android]\nversion_code = 8 # the store's\n\n[window]\n")
        assert.eq(code, 8)

        text = bumped("[export.android]\nrelease = true\n\n[window]\n")
        assert.eq(text, "[export.android]\nrelease = true\nversion_code = 2\n\n[window]\n")

        text = bumped("[export.android]\nrelease = true\n")
        assert.eq(text, "[export.android]\nrelease = true\nversion_code = 2\n")

        text, code = bumped("[export.android]\r\nversion_code = 3\r\n")
        assert.eq(text, "[export.android]\r\nversion_code = 4\r\n")
        assert.eq(code, 4)
    end)

    suite:case("the step names are the documented list", function()
        assert.eq(
            table.concat(progress.Steps, ","),
            "check,pack,layout,compile,partition,icon,stamp,archive,assemble,sign"
        )
    end)
end)
