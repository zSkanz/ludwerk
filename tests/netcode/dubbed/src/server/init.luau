--!strict
--[[
	The authority: it says two lines once somebody has joined -- one that is
	recorded, in two languages of two lengths, and one nobody has recorded yet
	-- and writes down how long each was and how many ticks it lasted HERE,
	where the joiner can read it and say it beside its own.

	A server has no player: it resolves in the project's default language,
	whose recording of the first line is half a second. The line is nine
	tenths long all the same, because that is its longest language (ADR 0200).
]]

local DialogueService = game:GetService("DialogueService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

-- Where the server's own measurements are left: an attribute a line.
local board = Instance.new("Part")
board.Name = "Board"
board.Anchored = true
board.Parent = Workspace

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
end)

local function say(line: string, name: string)
    local sound = DialogueService:Say(line)
    local from = ticks
    local ended = false
    sound.Ended:Connect(function()
        ended = true
    end)
    while not ended do
        task.wait()
    end
    board:SetAttribute(
        name,
        `category={sound.Category.Name} length={string.format("%.2f", sound.TimeLength)} ticks={ticks - from}`
    )
end

NetworkService.PlayerAdded:Connect(function()
    -- A moment for the joiner's own scripts to be listening.
    task.wait(1.5)
    say("act1.hello", "Hello")
    task.wait(0.5)
    say("act1.later", "Later")
    board:SetAttribute("Done", true)
end)
-- The joiner leaves when it has said what it heard; there is nobody else.
NetworkService.PlayerRemoving:Connect(function()
    game:Shutdown()
end)
