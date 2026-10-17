--!strict
--[[
	The conformance suite's runner (api-design.md §3).

	Mounted as an ordinary entry `Script` alongside every `*.spec.luau`, and it
	runs the suite once `game.Loaded` has fired -- so a spec that connects to
	`game.Loaded` at file scope has already been given its turn.

	A real file rather than a string inside `world_host.cpp`. Written that way
	first, and the escaping alone made it unreadable; as a file it is analysed
	by the gate, formatted by StyLua, and diffable like everything else.
]]

local testing = require("@engine/testing")

local DebugService = game:GetService("DebugService")

--- **A spec that fails as it loads is a failure, not an absence.** One whose
--- first line could not run -- a module it requires that is not there --
--- registers no case at all, and the suite used to pass without it, its error
--- in the log and nowhere in the count. The runner is mounted first (its path
--- sorts before every spec's), so every error raised before `game.Loaded` is
--- heard here, and each is a failed case of its own.
local loadErrors: { string } = {}
local loading = true
local listening = DebugService.MessageOut:Connect(function(message: string, level: EnumLogLevel)
    if loading and level == Enum.LogLevel.Error then
        table.insert(loadErrors, message)
    end
end)

--- A JSON encoder of its own, written before the game VM carried `@std/json`,
--- and kept: the runner must be able to report a VM whose `@std/json` is the
--- thing that broke. It only ever sees the two shapes below.
local function quote(text: string): string
    local escaped = string.gsub(text, '[%c"\\]', function(c: string): string
        if c == '"' then
            return '\\"'
        elseif c == "\\" then
            return "\\\\"
        elseif c == "\n" then
            return "\\n"
        elseif c == "\r" then
            return "\\r"
        elseif c == "\t" then
            return "\\t"
        end
        return string.format("\\u%04x", string.byte(c) or 0)
    end)
    return `"{escaped}"`
end

game.Loaded:Connect(function()
    loading = false
    listening:Disconnect()
    local report = testing.run()
    for _, message in loadErrors do
        local path = { "loading the specs", message }
        table.insert(report.Cases, { Path = path, Ok = false, Message = message })
        table.insert(report.Failures, { Path = path, Message = message })
        report.Total += 1
        report.Failed += 1
    end
    print(testing.format(report))

    -- The machine-readable half. `ludwerk test` turns this into TAP or JUnit; it
    -- never parses the console, because every line the engine prints is
    -- catalog-resolved and would change the first time a locale is added.
    local parts: { string } = {}
    for _, case in report.Cases do
        local fields = { `"name":{quote(table.concat(case.Path, " > "))}`, `"ok":{tostring(case.Ok)}` }
        if case.Message ~= nil then
            table.insert(fields, `"message":{quote(case.Message :: string)}`)
        end
        table.insert(parts, "{" .. table.concat(fields, ",") .. "}")
    end

    local summary = {
        `"total":{report.Total}`,
        `"passed":{report.Passed}`,
        `"failed":{report.Failed}`,
        `"cases":[{table.concat(parts, ",")}]`,
    }

    game:SetAttribute("ConformanceTotal", report.Total)
    game:SetAttribute("ConformancePassed", report.Passed)
    game:SetAttribute("ConformanceFailed", report.Failed)
    game:SetAttribute("ConformanceReport", "{" .. table.concat(summary, ",") .. "}")
    game:Shutdown()
end)
