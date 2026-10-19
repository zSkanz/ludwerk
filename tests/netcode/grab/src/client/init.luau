--!strict
--[[
	A joiner's hand (`@engine/xrgrab`). The first to join holds block A,
	lifts it 30 cm and lets go; the second reaches for A while it is held,
	then for B from three metres off, then for B from where it is:

	    [grab] first held A=true rose 0.30 m
	    [grab] second A refused=true, B from afar refused=true, B near held=true
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local xrgrab = require("@engine/xrgrab")

if NetworkService.Authority then
    return
end

local function welcomed(): Player
    while true do
        local player = NetworkService.LocalPlayer
        if player ~= nil and player.UserId >= 2 then
            return player
        end
        task.wait()
    end
end
local me = welcomed()
local a = Workspace:WaitForChild("A", 20) :: Part
local b = Workspace:WaitForChild("B", 20) :: Part

local function ticks(count: number)
    for _ = 1, count do
        RunService.PostSimulation:Wait()
    end
end

local function mine(part: Part): boolean
    return part:GetNetworkOwner() == me
end

if me.UserId == 2 then
    ticks(60)
    local start = a.Position
    local hold = xrgrab.hold(a, a.CFrame)
    local held = false
    for step = 1, 90 do
        hold:Move(CFrame.new(start + vector.create(0, 0.3 * math.min(step / 60, 1), 0)))
        RunService.PostSimulation:Wait()
        held = held or mine(a)
    end
    local rose = a.Position.y - start.y
    -- Still, then let go, so it is not thrown.
    ticks(30)
    hold:Release()
    print(`[grab] first held A={held} rose {string.format("%.2f", rose)} m`)
    -- Long enough for the server to see A go back and say so.
    ticks(240)
else
    -- The first has had A for a second by now.
    ticks(150)
    local same = xrgrab.hold(a, a.CFrame)
    ticks(45)
    local sameRefused = same.Refused
    same:Release()

    local far = xrgrab.hold(b, b.CFrame * CFrame.new(0, 3, 0))
    ticks(45)
    local farRefused = far.Refused
    far:Release()

    local near = xrgrab.hold(b, b.CFrame)
    local held = false
    for _ = 1, 60 do
        near:Move(b.CFrame)
        RunService.PostSimulation:Wait()
        held = held or mine(b)
    end
    near:Release()
    print(`[grab] second A refused={sameRefused}, B from afar refused={farRefused}, B near held={held}`)
    ticks(120)
end
game:Shutdown()
