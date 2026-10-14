--!strict
--[[
	The authority: it plays three sequences, and writes down what it heard of
	each where the joiner can read it and say it beside its own.

	- `Long` is playing before anybody joins: a minute of it. A joiner meets
	  it part way through.
	- `Act` is played once somebody has joined, to its end: its lamp is keyed
	  here, on the authority, and reaches the joiner as the lamp does.
	- `Cut` is the same file, skipped a second in.

	Nothing of a sequence's content travels: both ends have the file.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

-- Where the server's own account is left: an attribute a fact.
local board = Instance.new("Part")
board.Name = "Board"
board.Anchored = true
board.Parent = Workspace

local lamp = Instance.new("Part")
lamp.Name = "Lamp"
lamp.Anchored = true
lamp.Parent = Workspace
local light = Instance.new("PointLight")
light.Name = "Light"
light.Brightness = 1
light.Parent = lamp

local function player(name: string, file: string): SequencePlayer
    local made = Instance.new("SequencePlayer")
    made.Name = name
    made.Sequence = file :: Content
    local binding = Instance.new("SequenceBinding")
    binding.Name = "Lamp"
    binding.Target = lamp
    binding.Parent = made
    made.Parent = Workspace
    return made
end

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
end)

-- What a player said here, and on which of this machine's ticks after it began.
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

local long = player("Long", "asset://sequences/long.sequence.json")
local act = player("Act", "asset://sequences/act.sequence.json")
local cut = player("Cut", "asset://sequences/act.sequence.json")
local actHeard = listen(act)
local cutHeard = listen(cut)
long:Play()

NetworkService.PlayerAdded:Connect(function()
    -- A moment for the joiner's own scripts to be listening.
    task.wait(1.5)
    board:SetAttribute("LongAt", string.format("%.2f", long.TimePosition))
    board:SetAttribute("LongTick", ticks)

    local from = ticks
    act:Play()
    while act.Playing do
        task.wait()
    end
    local lasted = ticks - from
    task.wait(0.4)
    board:SetAttribute(
        "Act",
        `heard={table.concat(actHeard, ",")} ticks={lasted} lamp={string.format("%.2f", light.Brightness)} `
            .. `at={string.format("%.2f", act.TimePosition)}`
    )
    task.wait(0.2)

    cut:Play()
    task.wait(1)
    cut:Skip()
    task.wait(0.4)
    board:SetAttribute(
        "Cut",
        `heard={table.concat(cutHeard, ",")} lamp={string.format("%.2f", light.Brightness)} `
            .. `at={string.format("%.2f", cut.TimePosition)}`
    )

    -- And one stopped, which finishes nothing.
    act.TimePosition = 0
    act:Play()
    task.wait(0.5)
    act:Stop()
    task.wait(0.3)
    board:SetAttribute("Stopped", `at={string.format("%.2f", act.TimePosition)} playing={act.Playing}`)
    board:SetAttribute("Done", true)
end)
-- The joiner leaves when it has said what it saw; there is nobody else.
NetworkService.PlayerRemoving:Connect(function()
    game:Shutdown()
end)
