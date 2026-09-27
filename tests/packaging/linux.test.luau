--!strict
--[[
	The Linux export, unpacked and run where it is meant to run (ADR 0104
	stage 2): in the gate's Linux container, from the `.tar.gz` alone.

	**The archive is the claim.** The folder `ludwerk build` leaves behind is
	what this machine wrote; the `.tar.gz` is what a player downloads, and on
	Windows nothing but its tar headers carries the execute bit. So the folder
	is deleted, the archive is unpacked with the system's own `tar`, and the
	binary it produces has to run -- one tick, headless -- with no `chmod`.

	The dedicated server's package the same way: unpacked, started with no
	posture on the command line (its `project.toml` carries the role), run for a
	few ticks.

	Linux only; `scripts/gates/shipping-build.sh` runs it after building the
	`linux-clang-player` it packages.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local platform = require("@std/system/platform")
local process = require("@std/process")
local test = require("@std/test")

local build = require("../../tools/cli/commands/build")
local new = require("../../tools/cli/commands/new")
local projectlib = require("../../tools/cli/project")

local function repositoryRoot(): string
    local here = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tests/packaging"
    return projectlib.joined(here, "..", "..")
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

local function scaffold(parent: string, name: string): string
    removeTree(parent)
    fs.createDirectory(parent, { makeParents = true })
    assert.eq(new.run({ name = name, template = "starter", parent = parent }), 0)
    return projectlib.joined(parent, name)
end

--- Unpacks `archive` into `into` with the system's tar, and says whether the
--- player came out executable.
local function unpack(archive: string, into: string)
    removeTree(into)
    fs.createDirectory(into, { makeParents = true })
    local unpacked = process.run({ "tar", "-xzf", archive, "-C", into })
    assert.that(unpacked.ok, `tar could not unpack {archive}:\n{unpacked.stdout}{unpacked.stderr}`)
end

test.suite("linux export", function(suite)
    suite:case("the .tar.gz unpacks into a game that runs, with its execute bit", function()
        if not platform.linux then
            return
        end
        local parent = projectlib.joined(repositoryRoot(), ".engine-linux-export-test")
        local project = scaffold(parent, "linuxtest")
        fs.writeStringToFile(
            projectlib.joined(project, "project.toml"),
            fs.readFileToString(projectlib.joined(project, "project.toml"))
                .. '\n[export.linux]\nexecutable = "linuxtest"\n'
        )

        assert.eq(build.run({ path = project, output = nil, target = "linux", force = false, devHost = false }), 0)
        local output = projectlib.joined(project, "dist", "linux")
        assert.that(fs.exists(projectlib.joined(output, "linuxtest.desktop")), "no .desktop entry")
        local archive = projectlib.joined(project, "dist", "linuxtest-0.1.0-linux.tar.gz")
        assert.that(fs.exists(archive), `no archive at {archive}`)

        -- The folder goes: only the archive is what a player has.
        removeTree(output)
        local unpacked = projectlib.joined(parent, "unpacked")
        unpack(archive, unpacked)
        local executable = projectlib.joined(unpacked, "linuxtest", "linuxtest")
        local mode = process.run({ "stat", "-c", "%a", executable })
        assert.eq(string.gsub(mode.stdout, "%s", ""), "755")

        local ran = process.run({ executable, "--headless", "--rhi=null", "--frames=1", "--exit" })
        local text = ran.stdout .. ran.stderr
        assert.that(ran.ok, `the unpacked game exited non-zero:\n{text}`)
        assert.that(string.find(text, "Hello from the engine", 1, true) ~= nil, `the game did not run:\n{text}`)

        removeTree(parent)
    end)

    suite:case("a dedicated server's package starts serving with no arguments", function()
        if not platform.linux then
            return
        end
        local parent = projectlib.joined(repositoryRoot(), ".engine-linux-server-test")
        local project = scaffold(parent, "servertest")
        fs.writeStringToFile(
            projectlib.joined(project, "project.toml"),
            fs.readFileToString(projectlib.joined(project, "project.toml")) .. '\n[export]\nmultiplayer = "dedicated"\n'
        )
        fs.createDirectory(projectlib.joined(project, "src", "server"), { makeParents = true })
        fs.writeStringToFile(
            projectlib.joined(project, "src", "server", "Serve.luau"),
            '--!strict\nprint("server package is serving")\n'
        )

        assert.eq(
            build.run({ path = project, output = nil, target = "linux-server", force = false, devHost = false }),
            0
        )
        local archive = projectlib.joined(project, "dist", "servertest-0.1.0-linux-server.tar.gz")
        local unpacked = projectlib.joined(parent, "unpacked")
        unpack(archive, unpacked)
        local root = projectlib.joined(unpacked, "servertest")
        -- No face: a server has no icon and no launcher entry.
        assert.that(not fs.exists(projectlib.joined(root, "icon.png")), "a server package carries an icon")

        local ran = process.run({
            projectlib.joined(root, "servertest"),
            "--rhi=null",
            "--frames=5",
            "--exit",
        })
        local text = ran.stdout .. ran.stderr
        assert.that(ran.ok, `the server package exited non-zero:\n{text}`)
        assert.that(string.find(text, "server package is serving", 1, true) ~= nil, `no server code ran:\n{text}`)

        removeTree(parent)
    end)
end)
