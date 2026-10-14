--!strict
--[[
	The joiner: it plays nothing. It says what it saw of the three sequences
	the server played -- where the one already going was when it arrived, each
	event it heard and whether it was marked skipped, how each ended, and what
	the lamp the server's sequence keyed was here.

	Lines `[cutscene] ...`, which `run_cutscene.cmake` reads.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

if NetworkService.Authority then
    return
end

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
end)

-- **A wait that gives up, and says for what.** A joiner that waited for ever
-- said nothing more, and the gate could tell that it failed and not why
-- (an earlier defect was found behind exactly that silence).
local function waitFor(what: string, done: () -> boolean)
    local waited = 0
    while not done() do
        task.wait()
        waited += 1
        if waited > 900 then
            print(`[cutscene] gave up waiting for {what} at its tick {ticks}`)
            return
        end
    end
end

local function listen(made: SequencePlayer): { string }
    local heard: { string } = {}
    made.EventReached:Connect(function(name: string, data: any, skipped: boolean)
        table.insert(heard, `{name}{if data ~= nil then `={data}` else ""}{if skipped then "!" else ""}`)
    end)
    made.Finished:Connect(function(skipped: boolean)
        table.insert(heard, if skipped then "Finished!" else "Finished")
    end)
    return heard
end

local long = Workspace:WaitForChild("Long") :: SequencePlayer
local act = Workspace:WaitForChild("Act") :: SequencePlayer
local cut = Workspace:WaitForChild("Cut") :: SequencePlayer
local lamp = Workspace:WaitForChild("Lamp") :: Part
local light = lamp:WaitForChild("Light") :: PointLight
local board = Workspace:WaitForChild("Board") :: Part

local longHeard = listen(long)
local actHeard = listen(act)
local cutHeard = listen(cut)

-- **Met already going**: it is playing here from the first tick it is seen,
-- and where it is is where the server's has got to -- not nought.
task.wait(0.2)
print(
    `[cutscene] arrived long playing={long.Playing} at={string.format("%.2f", long.TimePosition)} `
        .. `length={string.format("%.2f", long.Length)}`
)

-- The server says where its own was a second and a half after this machine
-- joined: this machine's, read at the tick the word of it arrives, is
-- beside it.
waitFor("the server's word of the long sequence", function()
    return board:GetAttribute("LongAt") ~= nil
end)
print(
    `[cutscene] long server={board:GetAttribute("LongAt")} here={string.format("%.2f", long.TimePosition)} `
        .. `heard={table.concat(longHeard, ",")}`
)

-- `Act`, to its end: counted in this machine's own ticks from when it was
-- seen to play to when it was seen to stop.
waitFor("the act to play", function()
    return act.Playing
end)
local from = ticks
local half = 0
waitFor("the act to end", function()
    -- The lamp, half way: it arrives as the lamp does, keyed over there.
    if half == 0 and act.TimePosition >= 1 then
        half = light.Brightness
    end
    return not act.Playing
end)
local lasted = ticks - from
-- Read the moment it is seen to end: the server plays the next one a
-- moment later, and that one keys the same lamp from nought.
print(
    `[cutscene] act heard={table.concat(actHeard, ",")} ticks={lasted} `
        .. `lamp={string.format("%.2f", light.Brightness)} half={string.format("%.1f", half)} `
        .. `at={string.format("%.2f", act.TimePosition)} length={string.format("%.2f", act.Length)}`
)
waitFor("the server's account of the act", function()
    return board:GetAttribute("Act") ~= nil
end)
print(`[cutscene] server act {board:GetAttribute("Act")}`)

-- `Cut`: played, and skipped a second in.
waitFor("the cut to play", function()
    return cut.Playing
end)
from = ticks
waitFor("the cut to be skipped", function()
    return not cut.Playing
end)
lasted = ticks - from
-- The lamp's last value arrives as the lamp does, with the skip or a
-- snapshot after it: waited for, and not for long -- the server plays the act
-- again four tenths of a second on, which keys the same lamp.
local settling = 0
while light.Brightness < 4.99 and settling < 15 do
    task.wait()
    settling += 1
end
print(
    `[cutscene] cut heard={table.concat(cutHeard, ",")} ticks={lasted} `
        .. `lamp={string.format("%.2f", light.Brightness)} at={string.format("%.2f", cut.TimePosition)}`
)
waitFor("the server's account of the cut", function()
    return board:GetAttribute("Cut") ~= nil
end)
print(`[cutscene] server cut {board:GetAttribute("Cut")}`)

-- Played again and stopped: one event more, and nothing finished.
waitFor("the server to be done", function()
    return board:GetAttribute("Done") == true
end)
task.wait(0.2)
print(
    `[cutscene] stopped heard={table.concat(actHeard, ",")} at={string.format("%.2f", act.TimePosition)} `
        .. `playing={act.Playing}`
)
print(`[cutscene] server stopped {board:GetAttribute("Stopped")}`)
print("[cutscene] done")
-- Nothing more to see: the run ends here and the server's with it.
game:Shutdown()
