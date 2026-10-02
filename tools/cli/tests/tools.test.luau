--!strict
--[[
	What a fresh installation's first commands need (ADR 0152, D454): the
	programs `check` and `fmt` run are the installation's own before they are
	anybody's PATH, and the project `new` makes has an id a store accepts.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local test = require("@std/test")

local new = require("../commands/new")
local tools = require("../tools")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    local root = `{base}/scratch-tools-{name}`
    if fs.exists(root) then
        fs.removeDirectory(root, { recursive = true })
    end
    fs.createDirectory(root, { makeParents = true })
    return root
end

test.suite("tools", function(suite)
    suite:case("an installation's own copy of a program is the one that runs", function()
        -- A project made a minute ago, on a machine with nothing else on it,
        -- failed its first check: the analyser was looked for on PATH alone.
        local root = scratch("install")
        -- A name nothing on any PATH answers to, so what is found is found
        -- in the installation or not at all.
        local program = "no-such-analyser-anywhere"
        assert.eq(tools.find(program, root), nil)
        assert.eq(tools.find(program, nil), nil)

        local bundled = tools.bundledPath(root, program)
        fs.createDirectory(`{root}/tools/bin`, { makeParents = true })
        fs.writeStringToFile(bundled, "")
        assert.eq(tools.find(program, root), bundled)
        -- And only there: another installation does not see it.
        assert.eq(tools.find(program, scratch("elsewhere")), nil)

        fs.removeDirectory(root, { recursive = true })
        fs.removeDirectory(scratch("elsewhere"), { recursive = true })
    end)

    suite:case("a new project's id is lower case, and never a digit first", function()
        -- `ludwerk new LabirintoDeTinta` wrote `dev.local.LabirintoDeTinta`,
        -- which an Android package may not be called.
        assert.eq(new.identifier("LabirintoDeTinta"), "labirintodetinta")
        assert.eq(new.identifier("my-game"), "my_game")
        assert.eq(new.identifier("Space_Race2"), "space_race2")
        assert.eq(new.identifier("2048"), "game_2048")
    end)
end)
