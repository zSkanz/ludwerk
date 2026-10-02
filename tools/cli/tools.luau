--!strict
--[[
	The programs the CLI's commands run (ADR 0152): the analyser `check` types
	a project with, and the formatter `fmt` formats it with.

	**The installation's own copy first, then whatever is on PATH.** A project
	made by `new` on a machine that has nothing else installed has to pass its
	first `check`: the analyser used to be found on PATH and nowhere else, and
	the command's answer to its absence was the name of a tool manager the
	person had never heard of. An installation carries its analyser in
	`tools/bin/`, beside the interpreter that runs this, at the version the
	engine's definitions were generated for -- which a copy found on PATH need
	not be.
]]

local fs = require("@std/fs")
local platform = require("@std/system/platform")
local process = require("@std/process")

local projectlib = require("./project")

local function executableName(tool: string): string
    return if platform.win32 then `{tool}.exe` else tool
end

--- Where an installation rooted at `root` keeps its own copy of `tool`.
local function bundledPath(root: string, tool: string): string
    return projectlib.joined(root, "tools", "bin", executableName(tool))
end

--- What to run for `tool`: this installation's copy, or the bare name when
--- one on PATH answers, or nil when there is none at all. `root` is the
--- installation (the repository, when run from it); nil looks on PATH alone.
local function find(tool: string, root: string?): string?
    if root ~= nil then
        local bundled = bundledPath(root, tool)
        if fs.exists(bundled) then
            return bundled
        end
    end
    -- A program that is not there is an error to the runner, not an answer:
    -- either way it is not there.
    local ran, answered = pcall(function(): boolean
        return process.run({ tool, "--version" }, { stdio = "none" }).ok
    end)
    return if ran and answered then tool else nil
end

return {
    find = find,
    bundledPath = bundledPath,
    executableName = executableName,
}
