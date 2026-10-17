--!strict
--[[
	A process with a deadline, for the tests that start the engine.

	**A hang fails in minutes, naming the command**, rather than holding a gate
	for an hour -- which a packaged server waiting on an input nobody wrote
	once did. Lute's `process.run` waits for as long as the child lives, so the
	deadline is the system's: coreutils' `timeout` where there is one, and on
	Windows a PowerShell that waits for the child as long as it is allowed to,
	ends it by its process id past that, and hands back what it wrote.

	The exit code of a child ended this way is 124, as `timeout` says it, and
	the line `timed out after <seconds> s: <command>` is the last of its
	standard error.
]]

local platform = require("@std/system/platform")
local process = require("@std/process")

local Bounded = {}

--- How long a child may run when the caller says nothing.
Bounded.DefaultSeconds = 120

--- One argument as Windows splits a command line: quoted where it has to be.
local function windowsArgument(argument: string): string
    if argument ~= "" and string.find(argument, '[%s"]') == nil then
        return argument
    end
    local escaped = string.gsub(argument, '(\\*)"', function(slashes: string)
        return slashes .. slashes .. '\\"'
    end)
    escaped = string.gsub(escaped, "(\\+)$", "%1%1")
    return `"{escaped}"`
end

--- A string as a PowerShell single-quoted literal.
local function literal(text: string): string
    return "'" .. string.gsub(text, "'", "''") .. "'"
end

local function viaPowerShell(arguments: { string }, seconds: number): { string }
    local rest: { string } = {}
    for index = 2, #arguments do
        table.insert(rest, windowsArgument(arguments[index]))
    end
    local command = table.concat(arguments, " ")
    local script = table.concat({
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8",
        "$i = New-Object Diagnostics.ProcessStartInfo",
        `$i.FileName = {literal(arguments[1])}`,
        `$i.Arguments = {literal(table.concat(rest, " "))}`,
        "$i.UseShellExecute = $false",
        "$i.RedirectStandardOutput = $true",
        "$i.RedirectStandardError = $true",
        "$i.StandardOutputEncoding = [Text.Encoding]::UTF8",
        "$i.StandardErrorEncoding = [Text.Encoding]::UTF8",
        "$p = [Diagnostics.Process]::Start($i)",
        "$o = $p.StandardOutput.ReadToEndAsync()",
        "$e = $p.StandardError.ReadToEndAsync()",
        "$code = 0",
        `if (-not $p.WaitForExit({math.floor(seconds * 1000)})) \{ Stop-Process -Id $p.Id -Force; $code = 124 \} else \{ $p.WaitForExit(); $code = $p.ExitCode \}`,
        "[Console]::Out.Write($o.Result)",
        "[Console]::Error.Write($e.Result)",
        `if ($code -eq 124) \{ [Console]::Error.WriteLine({literal(`timed out after {seconds} s: {command}`)}) \}`,
        "exit $code",
    }, "; ")
    return { "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script }
end

--- Runs `arguments` as `process.run` does, ending the child after `seconds`.
function Bounded.run(
    arguments: { string },
    options: process.ProcessRunOptions?,
    seconds: number?
): process.ProcessResult
    local limit = seconds or Bounded.DefaultSeconds
    if platform.win32 then
        return process.run(viaPowerShell(arguments, limit), options)
    end
    local out = { "timeout", "--kill-after=10", tostring(limit) }
    table.move(arguments, 1, #arguments, 4, out)
    return process.run(out, options)
end

return Bounded
