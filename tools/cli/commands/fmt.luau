--!strict
--[[
	`ludwerk fmt` — StyLua, with the repository's own `stylua.toml`.

	A separate command from `check` because formatting is the one diagnostic
	with an obvious fix, and a developer who has been told to run something
	should be able to run it.
]]

local process = require("@std/process")

local catalog = require("../catalog")
local check = require("./check")
local projectlib = require("../project")
local tools = require("../tools")

local tr = catalog.tr

export type Options = {
    path: string,
    ignore: { string },
}

local function run(options: Options): number
    local files = check.collectLuau(options.path, options.ignore)

    local formatter = tools.find("stylua", projectlib.installRoot())
    if formatter == nil then
        print(tr("cli.fmt.err.no_stylua"))
        return 2
    end
    local result = process.run(check.styluaCommand({ formatter }, options.path, options.ignore), { stdio = "inherit" })
    if not result.ok then
        return 1
    end

    print(tr("cli.fmt.formatted", { count = #files }))
    return 0
end

return {
    run = run,
}
