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
local fs = require("@std/fs")
local test = require("@std/test")

local android = require("../export/android")
local brand = require("../brand")
local game = require("../export/game")
local progress = require("../export/progress")
local projectlib = require("../project")
local targets = require("../export/targets")
local versionCode = require("../export/version_code")

--- A folder beside the tests, emptied first.
local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    local folder = `{base}/scratch-export-{name}`
    if fs.exists(folder) then
        fs.removeDirectory(folder, { recursive = true })
    end
    fs.createDirectory(folder, { makeParents = true })
    return folder
end

local function write(file: string, text: string)
    local folder = string.match(file, "^(.*)[/\\][^/\\]+$")
    if folder ~= nil then
        fs.createDirectory(folder, { makeParents = true })
    end
    fs.writeStringToFile(file, text)
end

local function listed(folder: string): { string }
    local names: { string } = {}
    local function walk(at: string, relative: string)
        for _, entry in fs.listDirectory(at) do
            local rel = if relative == "" then entry.name else `{relative}/{entry.name}`
            if entry.type == "dir" then
                walk(`{at}/{entry.name}`, rel)
            else
                table.insert(names, rel)
            end
        end
    end
    walk(folder, "")
    table.sort(names)
    return names
end

test.suite("export", function(suite)
    -- A package that had no brand file signed with one key and the
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

    -- A store requires an app that targets Android 15 to load where a page is
    -- 16 KB, and what decides it is one number in each library's header. The
    -- reader is held still on headers written here: a library that says 16 KB
    -- for two segments and 4 KB for a third is a 4 KB library.
    suite:case("a library's page alignment is the smallest of its loadable segments", function()
        local function library(alignments: { number }, wide: boolean): string
            local headerSize = if wide then 64 else 52
            local entry = if wide then 56 else 32
            -- One segment that is not loadable first (PT_DYNAMIC), aligned to 8:
            -- it must not count.
            local count = #alignments + 1
            local data = buffer.create(headerSize + count * entry)
            buffer.writestring(data, 0, "\127ELF")
            buffer.writeu8(data, 4, if wide then 2 else 1)
            buffer.writeu8(data, 5, 1)
            if wide then
                buffer.writeu32(data, 0x20, headerSize)
                buffer.writeu16(data, 0x36, entry)
                buffer.writeu16(data, 0x38, count)
            else
                buffer.writeu32(data, 0x1C, headerSize)
                buffer.writeu16(data, 0x2A, entry)
                buffer.writeu16(data, 0x2C, count)
            end
            buffer.writeu32(data, headerSize, 2)
            buffer.writeu32(data, headerSize + (if wide then 0x30 else 0x1C), 8)
            for index, alignment in alignments do
                local at = headerSize + index * entry
                buffer.writeu32(data, at, 1)
                buffer.writeu32(data, at + (if wide then 0x30 else 0x1C), alignment)
            end
            return buffer.tostring(data)
        end

        assert.eq(android.PageBytes, 16384)
        assert.eq(android.loadAlignment(library({ 16384, 16384, 16384 }, true)), 16384)
        assert.eq(android.loadAlignment(library({ 16384, 4096, 16384 }, true)), 4096)
        assert.eq(android.loadAlignment(library({ 65536, 16384 }, true)), 16384)
        assert.eq(android.loadAlignment(library({ 4096 }, false)), 4096)
        -- What is not a library is said to be none, not taken for a good one.
        local nothing, why =
            android.loadAlignment("PK\3\4 this is a zip, not a library, and long enough to have a header in")
        assert.eq(nothing, nil)
        assert.that(why ~= nil, "no reason given")
        local none = android.loadAlignment(library({}, true))
        assert.eq(none, nil)
    end)

    -- A store's review asks about every permission a package declares, and a
    -- player reads the list: a game that plays alone declares none.
    suite:case(
        "an Android package declares the permissions its game uses, and the project has the last word",
        function()
            local function configured(internet: boolean?, vibrate: boolean?): android.Settings
                return {
                    package = "dev.ludwerk.test",
                    versionCode = 1,
                    orientation = "sensorLandscape",
                    release = false,
                    bundle = false,
                    internet = internet,
                    vibrate = vibrate,
                }
            end
            local alone = { network = false, haptics = false }
            local online = { network = true, haptics = true }
            assert.eq(#android.permissions(configured(nil, nil), alone), 0)
            assert.eq(
                table.concat(android.permissions(configured(nil, nil), online), " "),
                "android.permission.INTERNET android.permission.ACCESS_NETWORK_STATE android.permission.VIBRATE"
            )
            -- A name built at run time is not seen, which is what the switch is for.
            assert.eq(
                table.concat(android.permissions(configured(true, nil), alone), " "),
                "android.permission.INTERNET android.permission.ACCESS_NETWORK_STATE"
            )
            assert.eq(
                table.concat(android.permissions(configured(false, nil), online), " "),
                "android.permission.VIBRATE"
            )
            assert.eq(#android.permissions(configured(false, false), online), 0)
        end
    )

    suite:case("the manifest's permissions are written between its two marks, and nowhere else", function()
        local template = table.concat({
            "<manifest>",
            "    <!-- engine:permissions -->",
            '    <uses-permission android:name="android.permission.INTERNET" />',
            '    <uses-permission android:name="android.permission.VIBRATE" />',
            "    <!-- /engine:permissions -->",
            '    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />',
            "</manifest>",
        }, "\n")
        local none = android.manifestWith(template, {}) :: string
        assert.that(string.find(none, "uses-permission", 1, true) == nil, none)
        assert.that(string.find(none, "android.hardware.touchscreen", 1, true) ~= nil, "lost what follows the marks")
        assert.that(string.find(none, "<manifest>", 1, true) == 1, "lost what precedes the marks")

        local one = android.manifestWith(template, { "android.permission.VIBRATE" }) :: string
        assert.that(string.find(one, 'android:name="android.permission.VIBRATE"', 1, true) ~= nil, one)
        assert.that(string.find(one, "INTERNET", 1, true) == nil, one)
        -- Written again from its own output: an export of an export's manifest.
        assert.eq(android.manifestWith(one, { "android.permission.VIBRATE" }), one)

        -- A template that lost its marks is refused, not shipped as it is.
        assert.eq(android.manifestWith("<manifest></manifest>", {}), nil)
    end)

    suite:case("a headset's manifest starts the game in the headset, and a phone's has none of it", function()
        local template = table.concat({
            "<manifest>",
            "    <!-- engine:permissions -->",
            "    <!-- /engine:permissions -->",
            "    <!-- engine:xr-manifest -->",
            "    <!-- /engine:xr-manifest -->",
            "    <application>",
            "        <!-- engine:xr-application -->",
            "        <!-- /engine:xr-application -->",
            "        <activity><intent-filter>",
            "                <!-- engine:xr-categories -->",
            "                <!-- /engine:xr-categories -->",
            "        </intent-filter></activity>",
            "    </application>",
            "</manifest>",
        }, "\n")
        local entries = android.xrEntries(fs.readFileToString("platforms/android/player/xr-entries.xml"))
        assert.that(entries ~= nil, "the template's xr-entries.xml has its three sections")
        local headset = android.manifestWith(template, {}, entries) :: string
        assert.that(android.startsInHeadset(headset), headset)
        for _, entry in
            {
                "org.khronos.openxr.intent.category.IMMERSIVE_HMD",
                "android.hardware.vr.headtracking",
                "org.khronos.openxr.permission.OPENXR",
                "org.khronos.openxr.runtime_broker",
                "quest3|quest3s|questpro",
            }
        do
            assert.that(string.find(headset, entry, 1, true) ~= nil, entry)
        end
        -- A phone's build of the headset's manifest empties the marks again.
        local phone = android.manifestWith(headset, {}) :: string
        assert.that(not android.startsInHeadset(phone), phone)
        assert.that(string.find(phone, "openxr", 1, true) == nil, phone)
        assert.eq(android.manifestWith(phone, {}), phone)
        -- A template from before the headset's marks: a phone's build as
        -- ever, and no headset's.
        local older = "<manifest>\n    <!-- engine:permissions -->\n    <!-- /engine:permissions -->\n</manifest>"
        assert.that(android.manifestWith(older, {}) ~= nil, "a phone's build of an older template")
        assert.eq(android.manifestWith(older, {}, entries), nil)
    end)

    suite:case("the targets, and v1's name for the first", function()
        local names: { string } = {}
        for _, target in targets.All do
            table.insert(names, target.name)
        end
        assert.eq(table.concat(names, ","), "windows,linux,android,quest,windows-server,linux-server")
        assert.that((targets.find("quest") :: targets.Target).xr, "quest is a headset's build")
        assert.that(not (targets.find("android") :: targets.Target).xr, "android is a phone's build")
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

    suite:case("inside a script service the service decides, not RunContext", function()
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

    suite:case("a linked stamp's overrides follow the side the instance runs on", function()
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

    suite:case("the editor's conflicts and save slots are not the game's", function()
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
        -- A game that is not dedicated keeps both.
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
        -- And what no list named: the key saves are sealed with, a run's
        -- log, a folder some tool wrote.
        for _, name in { "keys", "engine.log", "engine.run", "cutscene-migration" } do
            assert.that(game.editorOnly(name), `{name} would ship`)
        end
        -- The settings of the last person to run the project from its
        -- folder are theirs, not every player's.
        assert.that(game.editorOnly("settings.json"), "a developer's settings would ship")
        for _, name in
            { "content.lpack", "content.manifest.json", "content.chunks.json", "partition", "l10n-pt-BR.lpack" }
        do
            assert.that(not game.editorOnly(name), `{name} would not ship`)
        end
    end)

    suite:case("a game's .engine holds what the build made and nothing of the developer's machine", function()
        local folder = scratch("engine")
        local project = `{folder}/project`
        for _, name in
            {
                "keys/saves.key",
                "keys/saves.key.lock",
                "engine.log",
                "engine_2.previous.log",
                "engine.run",
                "cutscene-migration/intro.before.luau",
                "editor.json",
                "settings.json",
                "content.lpack",
                "content.manifest.json",
                "content.chunks.json",
                "content/chunks/a.chunk",
                "terrain-pyramid/main/level-1.bin",
                "l10n-pt-BR.lpack",
            }
        do
            write(`{project}/.engine/{name}`, `bytes of {name}`)
        end
        write(`{project}/project.toml`, '[project]\nname = "g"\n')
        game.compose({
            project = ({ root = project, config = {} } :: any) :: projectlib.Project,
            game = `{folder}/out/game`,
            side = "all",
            content = `{project}/content`,
        })
        assert.eq(
            table.concat(listed(`{folder}/out/game/.engine`), ","),
            "content.chunks.json,content.lpack,content.manifest.json,content/chunks/a.chunk,l10n-pt-BR.lpack,"
                .. "terrain-pyramid/main/level-1.bin"
        )
        fs.removeDirectory(folder, { recursive = true })
    end)

    suite:case("what a boot of the package wrote in its .engine is gone before it ships", function()
        -- The partition boots the game on the package, and a boot makes this
        -- machine's match key: shipped, every player had the developer's.
        local folder = scratch("prune")
        for _, name in { "keys/saves.key", "keys/saves.key.lock", "engine.log", "content.lpack", "partition/cell.bin" } do
            write(`{folder}/game/.engine/{name}`, `bytes of {name}`)
        end
        game.pruneEngine(`{folder}/game`)
        assert.eq(table.concat(listed(`{folder}/game/.engine`), ","), "content.lpack,partition/cell.bin")
        fs.removeDirectory(folder, { recursive = true })
    end)

    suite:case("an export carries the engine's notices, byte for byte, and its fonts' licenses", function()
        local folder = scratch("notices")
        local install = `{folder}/engine`
        write(`{install}/LICENSE`, "the license")
        write(`{install}/NOTICE`, "the notice")
        write(`{install}/THIRD_PARTY_NOTICES.md`, "the third parties")
        write(`{install}/licenses/lute.txt`, "lute's")
        local content = `{folder}/content`
        write(`{content}/fonts/Display/OFL.txt`, "a font's license")
        write(`{content}/fonts/Body/OFL-Body.txt`, "another font's")
        write(`{content}/fonts/Body/Body.ttf`, "not a license")
        local to = `{folder}/out/licenses`
        local missing = game.copyNotices(install, content, to)
        assert.eq(#missing, 0)
        assert.eq(
            table.concat(listed(to), ","),
            "LICENSE,NOTICE,THIRD_PARTY_NOTICES.md,fonts/fonts-Body-OFL-Body.txt,fonts/fonts-Display-OFL.txt,lute.txt"
        )
        for _, name in game.NoticeFiles do
            assert.eq(fs.readFileToString(`{to}/{name}`), fs.readFileToString(`{install}/{name}`))
        end
        -- The engine this test runs from has them, as an installation does.
        local here = `{folder}/here`
        assert.eq(#game.copyNotices(projectlib.installRoot(), nil, here), 0)
        for _, name in game.NoticeFiles do
            assert.eq(
                fs.readFileToString(`{here}/{name}`),
                fs.readFileToString(projectlib.joined(projectlib.installRoot() :: string, name))
            )
        end
        -- And where there are none, the build is told which.
        assert.eq(#game.copyNotices(nil, nil, `{folder}/none`), 3)
        fs.removeDirectory(folder, { recursive = true })
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
            "check,pack,layout,compile,partition,seal,icon,stamp,archive,assemble,sign"
        )
    end)
end)
