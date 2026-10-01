--!strict
--[[
	`ludwerk terrain import` (ADR 0149 §2).

	Through the real command line where it refuses, for `edit.test.luau`'s
	reason, and through `arguments` for what the engine is handed: laying a
	world needs the engine, and what is provable without one is that the
	command asks it for the right thing and refuses the wrong ones itself.
]]

local assert = require("@std/test/assert")
local process = require("@std/process")
local test = require("@std/test")

local terrain = require("../commands/terrain")

local function testsDirectory(): string
    return string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
end

local function runCli(...: string): { code: number, output: string }
    local arguments: { string } = { tostring(process.execPath()), "run", `{testsDirectory()}/../main.luau`, "--" }
    for _, argument in { ... } do
        table.insert(arguments, argument)
    end
    local result = process.run(arguments)
    return { code = result.exitcode, output = result.stdout .. result.stderr }
end

test.suite("terrain", function(suite)
    suite:case("the command list advertises terrain import", function()
        local listed = runCli("--help")
        assert.eq(listed.code, 0, listed.output)
        assert.strContains(listed.output, "terrain import <source>")
    end)

    suite:case("nothing to import, no size and a size that is not a number are each refused", function()
        local bare = runCli("terrain")
        assert.eq(bare.code, 2, bare.output)
        assert.strContains(bare.output, "terrain import <source>")

        local sizeless = runCli("terrain", "import", "hills")
        assert.eq(sizeless.code, 2, sizeless.output)
        assert.strContains(sizeless.output, "--size=<metres>")

        local words = runCli("terrain", "import", "hills", "--size=wide")
        assert.eq(words.code, 2, words.output)
        assert.strContains(words.output, "is not a number")

        local missing = runCli("terrain", "import", "no-such-heightmap.png", "--size=512")
        assert.eq(missing.code, 2, missing.output)
        assert.strContains(missing.output, "nothing at")

        local typo = runCli("terrain", "import", "hills", "--sise=512")
        assert.eq(typo.code, 2, typo.output)
        assert.strContains(typo.output, "Unknown option")
    end)

    suite:case("a path that is not a project is refused before the engine is looked for", function()
        local refused = runCli("terrain", "import", "hills", testsDirectory(), "--size=512")
        assert.eq(refused.code, 2, refused.output)
        assert.strContains(refused.output, "is not a Ludwerk project")
    end)

    suite:case("the engine is handed the source and each number under its own name", function()
        local hills = terrain.arguments({
            action = "import",
            source = "hills",
            path = ".",
            size = "8192",
            high = "160",
            scale = "900",
            seed = "7",
        })
        assert.eq(hills, {
            "--import-terrain=hills",
            "--import-size=8192",
            "--import-high=160",
            "--import-seed=7",
            "--import-scale=900",
        })

        -- A file is named whole: the engine runs in the project, not here.
        local here = `{testsDirectory()}/terrain.test.luau`
        local file = terrain.arguments({ action = "import", source = here, path = ".", size = "512", low = "-20" })
        assert.neq(file, nil)
        local named = (file :: { string })[1]
        assert.strContains(named, "--import-terrain=")
        assert.strContains(named, "terrain.test.luau")
        assert.eq((file :: { string })[2], "--import-size=512")
        assert.eq((file :: { string })[3], "--import-low=-20")
    end)
end)
