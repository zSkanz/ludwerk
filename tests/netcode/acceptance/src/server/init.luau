--!strict
--[[
	The authority: a floor, a character for each player, driven by its intent
	every tick, and an answer to "where am I" for the bot to measure against.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Drive = require("@shared/Drive")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(400, 1, 400)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

local where = Instance.new("RemoteFunction")
where.Name = "Where"
where.Parent = Workspace

local bodies: { [Player]: CharacterBody } = {}

local function arrive(player: Player)
    local body = Instance.new("CharacterBody")
    body.Name = `Body{player.UserId}`
    body.Size = vector.create(1, 2, 1)
    body.WalkSpeed = 8
    body.Position = vector.create(player.UserId * 4, 1.5, 0)
    body.Parent = Workspace
    bodies[player] = body
    player.Character = body
end

for _, player in NetworkService:GetPlayers() do
    arrive(player)
end
NetworkService.PlayerAdded:Connect(arrive)
NetworkService.PlayerRemoving:Connect(function(player: Player)
    local body = bodies[player]
    if body then
        body:Destroy()
    end
    bodies[player] = nil
end)

where.OnServerInvoke = function(player: Player): vector
    local body = bodies[player]
    return if body then body.Position else vector.zero
end

RunService.Heartbeat:Connect(function()
    for player, body in bodies do
        Drive.step(player, body)
    end
end)
