--!strict
--[[
	Where the CLI looks for the binaries it drives (ADR 0054).

	**The order used to be a comment, and the comment was wrong.** The last
	candidate was the working directory, described as "beside this CLI, which is
	the shape an installed release has" — and the working directory is wherever
	the person is standing, so an installed CLI found its own engine only when
	somebody happened to be inside the installation. Nothing here could have
	caught that, because nothing asserted the list.

	So these drive the three searches as pure functions of their roots and check
	the paths themselves. Fabricated roots rather than real directories: what is
	under test is the ORDER and the LAYOUT, and a test that needed a build tree
	on disk would be a test that only runs on a machine that has one.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local test = require("@std/test")

local projectlib = require("../project")

local Roots: projectlib.Roots = {
    explicit = "/somewhere/engine-host",
    buildRoot = "/build",
    install = "/install",
}

--- Whether `needle` appears in `haystack`, comparing with the separator this
--- platform writes: `joined` normalises, and a literal with forward slashes
--- would pass on one tier and fail on the other.
local function contains(haystack: { string }, needle: string): boolean
    local normalised = projectlib.joined(needle)
    for _, entry in haystack do
        if entry == normalised then
            return true
        end
    end
    return false
end

local function indexOf(haystack: { string }, needle: string): number
    local normalised = projectlib.joined(needle)
    for index, entry in haystack do
        if entry == normalised then
            return index
        end
    end
    return -1
end

test.suite("install", function(suite)
    suite:case("the engine search is explicit, then the build tree, then the installation", function()
        local candidates = projectlib.engineCandidates(Roots)

        assert.eq(candidates[1], "/somewhere/engine-host", "an explicit ENG_HOST is not first")

        local build =
            indexOf(candidates, "/build/win-msvc-player/engine/app/" .. projectlib.executableName("engine-host"))
        local install = indexOf(candidates, "/install/" .. projectlib.executableName("engine-host"))
        assert.that(build > 1, "the build tree is not searched")
        assert.that(install > build, "the installation is searched before the build tree")
        assert.eq(install, #candidates, "the installation is not the last resort")
    end)

    suite:case("with no environment at all, the installation is the only answer", function()
        -- The case that has never worked: a downloaded folder, run from a
        -- project directory somewhere else entirely. Before this there was no
        -- candidate here at all except the working directory, which is not it.
        local candidates = projectlib.engineCandidates({ install = "/install" })
        assert.eq(#candidates, 1)
        assert.eq(candidates[1], projectlib.joined("/install/" .. projectlib.executableName("engine-host")))
    end)

    suite:case("the working directory is not a candidate", function()
        -- Explicit, because removing it is the fix rather than a side effect:
        -- with it in the list, `ludwerk dev` in a directory that happens to hold
        -- a `engine-host` would run that one.
        local candidates = projectlib.engineCandidates({ buildRoot = "/build", install = "/install" })
        for _, candidate in candidates do
            assert.that(
                string.find(candidate, "build", 1, true) ~= nil or string.find(candidate, "install", 1, true) ~= nil,
                `a candidate belongs to neither root: {candidate}`
            )
        end
    end)

    suite:case("an installation keeps the player host somewhere else, and it is not the editor", function()
        -- D057, expressed as a path. The two binaries differ by profile and a
        -- package that shipped one would put the editor inside somebody's game.
        local editor = projectlib.engineCandidates({ install = "/install" })
        local player = projectlib.playerCandidates({ install = "/install" })

        -- Under this machine's target's directory (ADR 0104 §6).
        local tag = if projectlib.executableName("x") == "x.exe" then "windows-x64" else "linux-x64"
        assert.eq(player[1], projectlib.joined(`/install/player/{tag}/` .. projectlib.executableName("engine-host")))
        assert.that(editor[1] ~= player[1], "the editor host and the player host resolve to the same file")
    end)

    suite:case("ENG_HOST does not decide what a release contains", function()
        -- Also D057: `ludwerk dev` honours it and `ludwerk build` must not, or
        -- "I pointed the dev server at a build" silently picks the binary a
        -- game ships with. The searches take separate explicit roots, so the
        -- test is that the player list never contains the dev one.
        local player = projectlib.playerCandidates({ buildRoot = "/build", install = "/install" })
        assert.that(not contains(player, "/somewhere/engine-host"), "a player search accepted ENG_HOST")
        for _, candidate in player do
            assert.that(
                string.find(candidate, "dev", 1, true) == nil and string.find(candidate, "editor", 1, true) == nil,
                `a player search reached a non-player preset: {candidate}`
            )
        end
    end)

    suite:case("a tool is beside the CLI in an installation and under its own directory in a build", function()
        local candidates = projectlib.toolCandidates("assetc", Roots)
        assert.that(
            contains(candidates, "/build/win-msvc-editor/tools/assetc/" .. projectlib.executableName("assetc")),
            "the build tree layout for a tool is wrong"
        )
        assert.eq(candidates[#candidates], projectlib.joined("/install/" .. projectlib.executableName("assetc")))
    end)

    suite:case("D517: an export's asset compiler is the one in the player's own build or installation", function()
        -- The newest compiler of every build was taken, so a player from one
        -- build shipped meshes written by another's -- in a newer format it
        -- could not read.
        local host = "/build/win-msvc-player/engine/app/" .. projectlib.executableName("engine-host")
        assert.eq(
            projectlib.toolBesideCandidate("assetc", host),
            projectlib.joined("/build/win-msvc-player", "tools", "assetc", projectlib.executableName("assetc"))
        )
        local installed = "/install/player/windows-x64/" .. projectlib.executableName("engine-host")
        assert.eq(
            projectlib.toolBesideCandidate("assetc", installed),
            projectlib.joined("/install", projectlib.executableName("assetc"))
        )
        assert.eq(projectlib.toolBesideCandidate("assetc", "/somewhere/else.exe"), nil)
    end)

    suite:case("the engine search takes a player host last, and used to take it first", function()
        -- D087. A player build has no ImGui compiled into it at all, so
        -- `ludwerk edit` launching one is a window with the world rendered into
        -- an offscreen texture and nothing but the backdrop on screen. It cost
        -- nothing while no machine had a player build; this repository grew a
        -- command that makes one.
        local candidates = projectlib.engineCandidates({ buildRoot = "/build" })
        local dev = indexOf(candidates, "/build/win-msvc-dev/engine/app/" .. projectlib.executableName("engine-host"))
        local editor =
            indexOf(candidates, "/build/win-msvc-editor/engine/app/" .. projectlib.executableName("engine-host"))
        local player =
            indexOf(candidates, "/build/win-msvc-player/engine/app/" .. projectlib.executableName("engine-host"))

        assert.that(dev > 0 and editor > 0 and player > 0, "a preset group is missing from the engine search")
        assert.that(dev < editor, "an artifact is preferred over the build somebody is working in")
        assert.that(editor < player, "a host with no editor in it is preferred over one that has an editor")
    end)

    suite:case("a host says which profile it was built from", function()
        -- Asked of the binary rather than inferred from the path, because a path
        -- is not a profile: `ENG_HOST` points anywhere, an installation holds
        -- two hosts and a build tree can hold five.
        local engine = projectlib.findEngine()
        if engine == nil then
            print("no engine built on this machine, so the probe was not exercised")
            return
        end
        local profile = projectlib.hostProfile(engine)
        assert.that(profile ~= nil, `{engine} did not report a profile under --version`)
        assert.that(projectlib.hostHasDebugUi(engine), `ludwerk edit would refuse the host it found: {profile}`)
    end)

    suite:case("this repository is its own installation root", function()
        -- What makes the installed shape the same code path as the developed
        -- one rather than a second one that only runs where nobody is looking.
        local root = projectlib.installRoot()
        assert.that(root ~= nil, "the CLI cannot find the tree it lives in")
        assert.that(
            fs.exists(projectlib.joined(root :: string, "templates", "starter")),
            "the install root has no template tree"
        )
        assert.that(
            fs.exists(projectlib.joined(root :: string, "runtime", "types", "engine.d.luau")),
            "the install root has no engine definitions"
        )
    end)
end)
