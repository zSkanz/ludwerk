--!strict
--[[
	The M8 packaging chain, end to end: scaffold, build, run, and read the
	identity back out of the artifact.

	Three of M8's scope items meet here and none of them can be checked any
	other way:

	  - `ludwerk build` produces a FOLDER that runs. Not "the command exited zero"
	    -- the executable inside it is launched, with no arguments, and has to
	    play the game beside it.
	  - **Application identity is asserted, not eyeballed.** The built `.exe` is
	    opened, its PE resource table is walked, and the icon it carries has to
	    be present and NOT the engine's default. An icon is the most
	    regression-prone thing in a build precisely because nothing fails when it
	    is wrong: the program runs, the window opens, and it wears the wrong
	    face. The icon starts as ONE PNG (ADR 0104 §2), and the file properties
	    -- product, company, version -- are read back out of the VERSIONINFO.
	  - The `.zip` beside the folder exists (`assetc`'s own tests read one
	    back byte by byte).
	  - `ludwerk new` scaffolds a project that all of the above works on, which is
	    the roadmap's clean-machine chain minus the fresh clone.

	Windows only, because a Windows executable's resources are what it reads.
	The Linux export has its own suite (`linux.test.luau`), run in the gate's
	Linux container where the archive is unpacked and played.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local platform = require("@std/system/platform")
local process = require("@std/process")
local test = require("@std/test")

local build = require("../../tools/cli/commands/build")
local new = require("../../tools/cli/commands/new")
local projectlib = require("../../tools/cli/project")

-- Distinct from `branding/icon/icon.ico` in every byte, which is the whole
-- point: the assertion at the end is that the artifact does NOT carry the
-- engine's icon, and a fixture that resembled it could make that pass by
-- accident. A PNG, because that is what a project names (ADR 0104 §2).
local IconFixture = "tests/identity/game.png"

local function repositoryRoot(): string
    local here = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tests/packaging"
    return projectlib.joined(here, "..", "..")
end

local function removeTree(target: string)
    if not fs.exists(target) then
        return
    end
    -- `fs.remove` is for FILES: on Windows it refuses a directory outright, so
    -- a hand-rolled walk removes the leaves and then fails on the first branch.
    fs.removeDirectory(target, { recursive = true })
end

--- A scaffolded project with an icon of its own, under a directory this test
--- owns. `new.run` rather than a hand-copied template, because the scaffolder
--- is one of the things under test -- D045 was exactly a `ludwerk new` that could
--- not find its own template, and nothing noticed for two milestones.
local function scaffold(name: string): (string, string)
    local root = repositoryRoot()
    local parent = projectlib.joined(root, ".engine-packaging-test")
    removeTree(parent)
    fs.createDirectory(parent, { makeParents = true })

    local created = new.run({ name = name, template = "starter", parent = parent })
    assert.eq(created, 0)

    local project = projectlib.joined(parent, name)
    -- The starter has no `assets/` of its own (ADR 0092 left it a scene and
    -- a project file), so the icon's directory is made here.
    fs.createDirectory(projectlib.joined(project, "assets"), { makeParents = true })
    fs.copy(projectlib.joined(root, IconFixture), projectlib.joined(project, "assets", "icon.png"))

    local config = projectlib.joined(project, "project.toml")
    local text = fs.readFileToString(config)
    local replaced
    text, replaced = string.gsub(text, '# icon = "assets/icon.png"', 'icon = "assets/icon.png"', 1)
    assert.eq(replaced, 1)
    text = string.gsub(text, 'version = "0.1.0"', 'version = "1.2.3"\ncompany = "Packaging Test Co"', 1)
    fs.writeStringToFile(config, text)

    return parent, project
end

test.suite("packaging", function(suite)
    suite:case("a built folder runs, and carries the game's icon rather than the engine's", function()
        if not platform.win32 then
            return
        end

        local root = repositoryRoot()
        local parent, project = scaffold("packagetest")

        -- **`--dev-host`, deliberately, and it is not a workaround.** What this
        -- suite tests is the packaging CHAIN -- scaffold, build, run, read the
        -- icon back -- on whatever host the gate just built, which is the dev
        -- one. That the packaged binary must be the `player` profile is a
        -- different claim with a different owner: the gate builds and links
        -- that profile on every run, and `ludwerk build` refuses it by default,
        -- which is the assertion below.
        -- **The refusal, first, because it is the one that protects a release**
        -- (D057). Without a player host and without the flag, `ludwerk build` has
        -- to fail rather than quietly package a binary carrying the debug
        -- overlay, the inspector and a REPL against its own VM.
        --
        -- Conditional on there being no player host, and it says which: on a
        -- machine where somebody has built `win-msvc-player` the command is
        -- SUPPOSED to succeed, and a test that failed there would be testing
        -- the machine. A silent skip would read as coverage this does not have.
        if projectlib.findPlayerEngine() == nil then
            local refused =
                build.run({ path = project, output = nil, target = "windows", force = false, devHost = false })
            assert.eq(refused, 1)
        else
            print("a player host exists on this machine, so the refusal path was not exercised")
        end

        -- `win64`, v1's name, is still an alias of `windows`, and the folder
        -- is named after the target it resolves to.
        local result = build.run({ path = project, output = nil, target = "win64", force = false, devHost = true })
        assert.eq(result, 0)

        local output = projectlib.joined(project, "dist", "windows")
        local executable = projectlib.joined(output, "packagetest.exe")

        -- The shape, before anything is run. Each of these is a thing a player
        -- needs and a build could plausibly forget: the binary, the engine's own
        -- content, the game, and the game's project file inside it.
        assert.that(fs.exists(executable), `no executable at {executable}`)
        assert.that(fs.exists(projectlib.joined(output, "content", "i18n", "en.json")), "no engine catalog")
        assert.that(fs.exists(projectlib.joined(output, "content", "shaders")), "no engine shaders")
        -- The content pack, which carries the starter's scene -- and with it
        -- its scripts (ADR 0092). The engine reads the scene out of it.
        assert.that(
            fs.exists(projectlib.joined(output, "game", ".engine", "content.lpack")),
            "no content pack in the game"
        )
        -- **Sealed** (ADR 0183): the game's settings, its scripts, its
        -- catalogues and the names of its content are in that pack, and none
        -- of them is a file beside it. That the game then runs -- below -- is
        -- what says it reads them there.
        for _, loose in { "project.toml", ".luaurc", "src", "i18n" } do
            assert.that(
                not fs.exists(projectlib.joined(output, "game", loose)),
                `{loose} is a file of its own in a sealed game`
            )
        end
        assert.that(
            not fs.exists(projectlib.joined(output, "game", ".engine", "content.manifest.json")),
            "a sealed game carries a manifest of its content's names"
        )
        local pack = fs.readFileToString(projectlib.joined(output, "game", ".engine", "content.lpack"))
        for _, word in { "asset://", "main.scene", "packagetest", "Packaging Test Co" } do
            assert.that(string.find(pack, word, 1, true) == nil, `"{word}" can be read in the sealed pack`)
        end
        -- **No shader compiler** (ADR 0091). A game's surface shaders are
        -- compiled when it is built and read out of its pack when it runs; a
        -- player that carried the compiler could be made to compile.
        for _, name in { "shadercross.exe", "dxcompiler.dll", "dxil.dll", "dxc.exe" } do
            assert.that(not fs.exists(projectlib.joined(output, name)), `the game carries a shader compiler: {name}`)
        end

        -- **No arguments.** That is the claim: a packaged game is a folder whose
        -- executable mounts the `game/` directory beside it, so double-clicking
        -- it plays. `--headless --frames --exit` is only how a test watches it
        -- do that without a window.
        local ran = process.run({ executable, "--headless", "--frames=30", "--exit" })
        local output_text = ran.stdout .. ran.stderr
        assert.that(ran.ok, `the packaged game exited non-zero:\n{output_text}`)
        -- The template's own first line, which proves the GAME ran rather than
        -- the engine merely starting.
        assert.that(
            string.find(output_text, "Hello from the engine", 1, true) ~= nil,
            `the packaged game did not run its script:\n{output_text}`
        )

        -- The archive beside the folder, named for the game, its version and
        -- the target -- and without the build's own marker inside it.
        local zip = projectlib.joined(project, "dist", "packagetest-1.2.3-windows.zip")
        assert.that(fs.exists(zip), `no archive at {zip}`)
        local zipped = fs.readFileToString(zip)
        assert.eq(string.sub(zipped, 1, 4), "PK\3\4")
        assert.that(string.find(zipped, "packagetest/packagetest.exe", 1, true) ~= nil, "the archive has no executable")
        assert.that(string.find(zipped, ".engine-build", 1, true) == nil, "the archive carries the build marker")

        -- **No console window** (ADR 0104, stage 0): a packaged `player` is a
        -- Windows-subsystem program. Read from the PE header, which is what
        -- decides whether Windows opens a console for it -- so the check is
        -- automated rather than a person looking. Only when the packaged host
        -- IS the player: a machine that built only `dev` packages that, and says so.
        local player = projectlib.findPlayerEngine()
        if player ~= nil and projectlib.hostProfile(player) == "player" then
            local image = fs.readFileToString(executable)
            local header = string.unpack("<I4", image, 0x3C + 1) :: number
            assert.eq(string.sub(image, header + 1, header + 4), "PE\0\0")
            -- The optional header follows the 20-byte file header; its
            -- `Subsystem` is 68 bytes in. 2 is WINDOWS_GUI, 3 WINDOWS_CUI.
            local subsystem = string.unpack("<I2", image, header + 4 + 20 + 68 + 1)
            assert.eq(subsystem, 2)
        else
            print("no player host on this machine, so the packaged binary's subsystem was not checked")
        end

        -- The identity, read back out of the artifact rather than looked at.
        local iconpatch = projectlib.findTool("iconpatch")
        assert.that(iconpatch ~= nil, "iconpatch was not found; build it or set ENG_ICONPATCH")
        local verified = process.run({
            iconpatch :: string,
            "verify",
            executable,
            "--not",
            projectlib.joined(root, "branding", "icon", "icon.ico"),
        })
        assert.that(verified.ok, `the artifact is still wearing the engine's icon:\n{verified.stdout}{verified.stderr}`)

        -- The file properties Explorer's Details tab shows, read back.
        local info = process.run({ iconpatch :: string, "info", executable })
        assert.that(info.ok, `the artifact carries no version resource:\n{info.stdout}{info.stderr}`)
        for _, line in { "ProductName: packagetest", "CompanyName: Packaging Test Co", "ProductVersion: 1.2.3" } do
            assert.that(string.find(info.stdout, line, 1, true) ~= nil, `no "{line}" in:\n{info.stdout}`)
        end

        removeTree(parent)
    end)

    suite:case("a packaged game with a streamed terrain has its ground (D411)", function()
        if not platform.win32 then
            return
        end
        local host = projectlib.findEngine()
        assert.that(host ~= nil, "no engine host was found; build one or set ENG_HOST")

        local parent, project = scaffold("packageterrain")
        -- Eleven hundred metres of hills: 324 cells, past the 256 at which a
        -- terrain is saved as a folder of cells beside its scene and streamed
        -- (ADR 0087). The command is the one `ludwerk terrain import` runs.
        local laid = process.run({
            host :: string,
            project,
            "--import-terrain=hills",
            "--import-size=1100",
            "--import-high=40",
        })
        assert.that(laid.ok, `the terrain was not laid:\n{laid.stdout}{laid.stderr}`)
        local cells = projectlib.joined(project, "content", "terrain", "scenes", "main.scene")
        assert.that(fs.exists(projectlib.joined(cells, "index.json")), "the import saved no folder of cells")

        local result = build.run({ path = project, output = nil, target = "windows", force = false, devHost = true })
        assert.eq(result, 0)
        local output = projectlib.joined(project, "dist", "windows")
        local game = projectlib.joined(output, "game")

        -- **The cells are files in the game, where the engine streams them
        -- from, and not in the pack**, which it never reads a cell through:
        -- packed, they were a game with no ground and a pack as large as the
        -- world.
        local shipped = projectlib.joined(game, "content", "terrain", "scenes", "main.scene")
        assert.that(fs.exists(projectlib.joined(shipped, "index.json")), "the game has no index of its terrain's cells")
        local count = 0
        for _, entry in fs.listDirectory(shipped) do
            if string.sub(entry.name, -9) == ".lterrain" then
                count += 1
            end
        end
        assert.that(count >= 256, `the game carries {count} cells of its terrain`)
        -- The pack is sealed and lists nothing (ADR 0183), so it is weighed:
        -- a pack that had taken the cells in would be as large as they are.
        local cellBytes = 0
        for _, entry in fs.listDirectory(shipped) do
            if string.sub(entry.name, -9) == ".lterrain" then
                cellBytes += #fs.readFileToString(projectlib.joined(shipped, entry.name))
            end
        end
        local packBytes = #fs.readFileToString(projectlib.joined(game, ".engine", "content.lpack"))
        assert.that(packBytes < cellBytes, `the pack is {packBytes} bytes beside {cellBytes} of cells: it carries them`)
        assert.that(
            not fs.exists(projectlib.joined(game, ".engine", "content.manifest.json")),
            "a sealed game carries a manifest"
        )
        -- The far ground's files (ADR 0150) go with it, made by the import.
        assert.that(
            fs.exists(projectlib.joined(game, ".engine", "terrain-pyramid")),
            "the game carries no far ground for its terrain"
        )
        -- And nothing of a run's own cache.
        assert.that(not fs.exists(projectlib.joined(game, ".engine", "session")), "the game carries a session cache")

        -- Run with no arguments but the test's own, and the index is found:
        -- before, the game said that nothing mounted resolved it, and ran on
        -- over no ground. That the ground then streams in and is waited for
        -- is the engine's (`field_streamer_tests`, D412) -- not asserted here,
        -- where the binary packaged may be the player a gate before this one
        -- built.
        local ran =
            process.run({ projectlib.joined(output, "packageterrain.exe"), "--headless", "--frames=60", "--exit" })
        local said = ran.stdout .. ran.stderr
        assert.that(ran.ok, `the packaged game exited non-zero:\n{said}`)
        assert.that(
            string.find(said, "nothing mounted resolves", 1, true) == nil,
            `the packaged game could not find its terrain's cells:\n{said}`
        )

        removeTree(parent)
    end)
end)
