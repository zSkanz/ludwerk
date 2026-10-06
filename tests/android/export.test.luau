--!strict
--[[
	The Android export, debug and release (ADR 0104 stages 2 and 3): an APK
	from the prebuilt player and the shipped Gradle template, signed, and the
	signature checked by the SDK's own `apksigner` rather than trusted.

	**Release is the one that matters.** A debug APK installs anywhere with the
	machine's key; a release APK is what a store keeps, and it is signed with a
	key `ludwerk keystore new` just made, its passwords handed over in the
	environment the way a CI job has them -- never on a command line and never
	in `project.toml`. `--bump-version-code` is exercised on the way, and the APK
	is read back for the code it was given.

	Runs where the Android tools and the arm64 player are (the local gate's
	`android` stage); anywhere else it says what it skipped.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local process = require("@std/process")
local test = require("@std/test")

local android = require("../../tools/cli/export/android")
local build = require("../../tools/cli/commands/build")
local keystore = require("../../tools/cli/commands/keystore")
local new = require("../../tools/cli/commands/new")
local projectlib = require("../../tools/cli/project")
local targets = require("../../tools/cli/export/targets")

local function repositoryRoot(): string
    local here = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tests/android"
    return projectlib.joined(here, "..", "..")
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

--- The tools and the player, or the reason there are none here.
local function ready(): android.Tools?
    local tools = android.findTools()
    if tools == nil then
        print("no Android tools on this machine, so the Android export was not exercised")
        return nil
    end
    if targets.findPlayer(targets.find("android") :: targets.Target) == nil then
        print("no arm64 player built on this machine, so the Android export was not exercised")
        return nil
    end
    return tools
end

--- `aapt2 dump badging`'s first line: the package, the version code and name.
local function badging(tools: android.Tools, apk: string): string
    local aapt = projectlib.joined(
        tools.sdk,
        "build-tools",
        "35.0.0",
        if fs.exists(projectlib.joined(tools.sdk, "build-tools", "35.0.0", "aapt2.exe")) then "aapt2.exe" else "aapt2"
    )
    local dumped = process.run({ aapt, "dump", "badging", apk })
    assert.that(dumped.ok, `aapt2 could not read {apk}:\n{dumped.stdout}{dumped.stderr}`)
    return dumped.stdout
end

test.suite("android export", function(suite)
    suite:case("an APK of a game with a streamed terrain carries its cells and its far ground (D411)", function()
        local tools = ready()
        if tools == nil then
            return
        end
        local host = projectlib.findEngine()
        if host == nil then
            print("no engine host on this machine, so the terrain's export to Android was not exercised")
            return
        end
        local parent = projectlib.joined(repositoryRoot(), ".engine-android-terrain-test")
        removeTree(parent)
        fs.createDirectory(parent, { makeParents = true })
        assert.eq(new.run({ name = "androidterrain", template = "starter", parent = parent }), 0)
        local project = projectlib.joined(parent, "androidterrain")
        -- 324 cells: past the 256 at which a terrain is a folder of cells.
        local laid = process.run({
            host :: string,
            project,
            "--import-terrain=hills",
            "--import-size=1100",
            "--import-high=40",
        })
        assert.that(laid.ok, `the terrain was not laid:\n{laid.stdout}{laid.stderr}`)

        assert.eq(build.run({ path = project, output = nil, target = "android", force = false, devHost = false }), 0)

        -- **What the phone extracts is what `payload.txt` lists** -- an APK's
        -- assets cannot be listed from native code -- so a cell that is not
        -- in the list is a cell the game never has, whatever the APK holds.
        local assets = projectlib.joined(project, ".engine", "export", "android", "stage", "assets")
        local listed = fs.readFileToString(projectlib.joined(assets, "payload.txt"))
        local cells = 0
        local far = 0
        for line in string.gmatch(listed, "[^\r\n]+") do
            if string.sub(line, -9) == ".lterrain" then
                cells += 1
                assert.that(
                    string.sub(line, 1, 21) == "game/content/terrain/",
                    `a cell is listed where the engine does not look: {line}`
                )
                assert.that(fs.exists(projectlib.joined(assets, line)), `{line} is listed and not staged`)
            elseif string.sub(line, -6) == ".lnode" then
                far += 1
                assert.that(
                    string.sub(line, 1, 29) == "game/.engine/terrain-pyramid/",
                    `a file of the far ground is listed where the engine does not look: {line}`
                )
            end
        end
        assert.that(cells >= 256, `the payload lists {cells} cells of the terrain`)
        assert.that(far > 0, "the payload lists none of the far ground's files")
        assert.that(
            string.find(listed, "game/content/terrain/scenes/main.scene/index.json", 1, true) ~= nil,
            "the payload does not list the index of the terrain's cells"
        )
        assert.that(string.find(listed, "/session/", 1, true) == nil, "the payload carries a run's session cache")
        -- And the pack, which the engine never reads a cell through, has none.
        -- The pack is sealed and lists nothing (ADR 0183): no manifest, no
        -- script and no settings beside it, in the folder or in the list.
        assert.that(
            not fs.exists(projectlib.joined(assets, "game", ".engine", "content.manifest.json")),
            "a sealed game carries a manifest"
        )
        assert.that(fs.exists(projectlib.joined(assets, "game", ".engine", "content.lpack")), "the game has no pack")
        for _, loose in { "game/src/", "game/i18n/", "game/project.toml", "game/.luaurc", "content.manifest.json" } do
            assert.that(string.find(listed, loose, 1, true) == nil, `the payload lists {loose}, of a sealed game`)
        end
        assert.that(
            string.find(listed, "game/.engine/content.lpack", 1, true) ~= nil,
            "the payload does not list the game's pack"
        )

        -- The APK itself, read back with the JDK's own tool: the template
        -- drops what it is not told to keep, and a dot-folder is one.
        local apk = projectlib.joined(project, "dist", "android", "androidterrain-0.1.0.apk")
        assert.that(fs.exists(apk), `no APK at {apk}`)
        local jar = projectlib.joined(
            (tools :: android.Tools).jdk,
            "bin",
            if fs.exists(projectlib.joined((tools :: android.Tools).jdk, "bin", "jar.exe")) then "jar.exe" else "jar"
        )
        local entries = process.run({ jar, "tf", apk })
        assert.that(entries.ok, `the APK could not be listed:\n{entries.stdout}{entries.stderr}`)
        local inApk = 0
        for line in string.gmatch(entries.stdout, "[^\r\n]+") do
            if string.sub(line, -9) == ".lterrain" then
                inApk += 1
            end
        end
        assert.eq(inApk, cells)
        assert.that(
            string.find(entries.stdout, "assets/game/.engine/terrain-pyramid/", 1, true) ~= nil,
            "the APK dropped the far ground's files"
        )

        removeTree(parent)
    end)

    suite:case("a release APK is signed with the project's own key, and its code went up", function()
        local tools = ready()
        if tools == nil then
            return
        end
        local parent = projectlib.joined(repositoryRoot(), ".engine-android-test")
        removeTree(parent)
        fs.createDirectory(parent, { makeParents = true })
        assert.eq(new.run({ name = "androidtest", template = "starter", parent = parent }), 0)
        local project = projectlib.joined(parent, "androidtest")
        local config = projectlib.joined(project, "project.toml")

        -- A debug APK first: the machine's key, no setup.
        assert.eq(build.run({ path = project, output = nil, target = "android", force = false, devHost = false }), 0)
        local apk = projectlib.joined(project, "dist", "android", "androidtest-0.1.0.apk")
        assert.that(fs.exists(apk), `no APK at {apk}`)
        local badge = badging(tools :: android.Tools, apk)
        assert.that(string.find(badge, "versionCode='1'", 1, true) ~= nil, "not code 1")
        -- **A game on a phone can open a socket** (D431): without INTERNET
        -- Android refuses every one, and no game could join, host or fetch.
        for _, permission in { "android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE" } do
            assert.that(
                string.find(badge, `uses-permission: name='{permission}'`, 1, true) ~= nil,
                `the APK does not declare {permission}`
            )
        end

        -- A release key, made the way the editor's button makes one.
        local key = projectlib.joined(parent, "keys", "release.keystore")
        process.env.ENG_ANDROID_KEYSTORE_PASSWORD = "export-test-secret"
        assert.eq(
            keystore.run({ alias = "androidtest", out = key, name = "Export Test", validity = 1, remember = false }),
            0
        )
        -- And refused a second time: a keystore replaced is every update lost.
        assert.eq(
            keystore.run({ alias = "androidtest", out = key, name = "Export Test", validity = 1, remember = false }),
            1
        )

        fs.writeStringToFile(
            config,
            fs.readFileToString(config)
                .. '\n[export.android]\n# kept\nrelease = true\nkeystore = "../keys/release.keystore"\n'
                .. 'key_alias = "androidtest"\n'
        )
        assert.eq(
            build.run({
                path = project,
                output = nil,
                target = "android",
                force = false,
                devHost = false,
                bumpVersionCode = true,
            }),
            0
        )
        process.env.ENG_ANDROID_KEYSTORE_PASSWORD = nil

        local text = fs.readFileToString(config)
        assert.that(string.find(text, "version_code = 2", 1, true) ~= nil, text)
        assert.that(string.find(text, "# kept", 1, true) ~= nil, "the bump lost a comment")
        assert.that(string.find(badging(tools :: android.Tools, apk), "versionCode='2'", 1, true) ~= nil, "not code 2")

        -- The certificate is the project's: its common name, not the debug key's.
        local jar = projectlib.joined(tools.sdk, "build-tools", "35.0.0", "lib", "apksigner.jar")
        local checked = process.run({
            projectlib.joined(
                tools.jdk,
                "bin",
                if fs.exists(projectlib.joined(tools.jdk, "bin", "java.exe")) then "java.exe" else "java"
            ),
            "-jar",
            jar,
            "verify",
            "--print-certs",
            apk,
        })
        assert.that(checked.ok, `apksigner refused the release APK:\n{checked.stdout}{checked.stderr}`)
        assert.that(string.find(checked.stdout, "CN=Export Test", 1, true) ~= nil, checked.stdout)

        removeTree(parent)
    end)
end)
