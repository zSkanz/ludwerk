--!strict
--[[
	`ludwerk check` and `ludwerk fmt`: what they walk and how they name what
	they load.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local test = require("@std/test")

local check = require("../commands/check")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    -- Beside the tests rather than under `.lute/`, which is itself ignored.
    local root = `{base}/scratch-check-{name}`
    if fs.exists(root) then
        fs.removeDirectory(root, { recursive = true })
    end
    fs.createDirectory(root, { makeParents = true })
    return root
end

test.suite("check", function(suite)
    suite:case("an exported game and the engine's own folder are never walked", function()
        -- **D211.** `ludwerk build` writes `dist/`, and check and fmt went
        -- through every exported copy of the game with the source.
        local root = scratch("walk")
        for _, folder in { "src", "dist/windows/content/src", ".engine/types", "src/distance" } do
            fs.createDirectory(`{root}/{folder}`, { makeParents = true })
        end
        fs.writeStringToFile(`{root}/src/main.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/src/distance/near.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/dist/windows/content/src/main.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/.engine/types/stale.luau`, "return nil\n")

        local files = check.collectLuau(root, { "third_party", ".lute", "luau_packages", ".engine", "dist" })
        local names: { string } = {}
        for _, file in files do
            table.insert(names, (string.gsub(string.sub(file, #root + 2), "\\", "/")))
        end
        table.sort(names)
        -- `distance/` is a folder that only starts like one ignored.
        assert.eq(table.concat(names, ","), "src/distance/near.luau,src/main.luau")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("stylua is told to skip what check skips", function()
        local command = check.styluaCommand({ "stylua", "--check" }, ".", { ".engine", "dist" })
        local joined = table.concat(command, " ")
        assert.strContains(joined, "--glob=!**/dist/**")
        assert.strContains(joined, "--glob=!**/.engine/**")
        assert.strContains(joined, "--glob=!**/*.d.luau")
        assert.eq(command[#command], ".")
    end)

    suite:case("definitions load under the engine's own name", function()
        -- **D212.** Unnamed, the analyser announced them under another
        -- platform's.
        assert.eq(check.definitionName("runtime/types/engine.d.luau"), "engine")
        assert.eq(check.definitionName("C:\\game\\.engine\\types\\scene.d.luau"), "scene")
    end)
end)
