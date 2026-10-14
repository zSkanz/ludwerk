--!strict
-- What a shipped game asks at its start.

local RunService = game:GetService("RunService")

local last = RunService:GetLastRun()
print(
    `LASTRUN outcome={last.Outcome.Name} errors={last.ScriptErrors} first={last.FirstScriptError or "none"} report={last.Report or "none"} dump={if last.Dump
        then "yes"
        else "none"}`
)
game:Shutdown()
