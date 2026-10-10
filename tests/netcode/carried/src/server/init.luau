--!strict
--[[
	The authority: for each player, a thing made three ways -- from the stamp,
	as a `Clone` of a template kept in `ReplicatedStorage`, and with
	`Instance.new` -- each carrying one of every class protocol 44 sends. It
	answers what it holds of each, so the joiner compares its own copy with
	the authority's and not with a list written twice, and on `Change` writes
	one property of each.
]]

local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Workspace = game:GetService("Workspace")

local Carried = require("@shared/Carried")

Carried.light(false)

local template = Instance.new("Part")
template.Name = "Template"
template.Anchored = true
template.Size = vector.create(1, 2, 1)
Carried.fill(template)
template.Parent = ReplicatedStorage

local function arrive(player: Player)
    local stamped = Instance.stamp("thing") :: Part
    stamped.Name = `Stamped{player.UserId}`
    stamped.Position = vector.create(player.UserId * 4, 1, 0)
    stamped.Parent = Workspace

    local cloned = template:Clone() :: Part
    cloned.Name = `Cloned{player.UserId}`
    cloned.Position = vector.create(player.UserId * 4, 1, 6)
    cloned.Parent = Workspace

    local made = Instance.new("Part")
    made.Name = `Made{player.UserId}`
    made.Anchored = true
    made.Size = vector.create(1, 2, 1)
    made.Position = vector.create(player.UserId * 4, 1, 12)
    Carried.fill(made)
    made.Parent = Workspace
end

-- What the authority holds of every class under the asking player's things:
-- a line each, keyed `<way>.<class>`, and nothing for one that is not there.
local held = Instance.new("RemoteFunction")
held.Name = "Held"
held.OnServerInvoke = function(player: Player): { [string]: string }
    local lines: { [string]: string } = {}
    for _, way in Carried.Ways do
        local body = Workspace:FindFirstChild(`{way}{player.UserId}`)
        if body then
            for _, class in Carried.Classes do
                local line = Carried.describe(body, class)
                if line then
                    lines[`{way}.{class}`] = line
                end
            end
        end
    end
    for _, name in Carried.World do
        local line = Carried.describeWorld(name)
        if line then
            lines[`World.{name}`] = line
        end
    end
    return lines
end
held.Parent = Workspace

local change = Instance.new("RemoteFunction")
change.Name = "Change"
change.OnServerInvoke = function(player: Player): boolean
    for _, way in Carried.Ways do
        local body = Workspace:FindFirstChild(`{way}{player.UserId}`)
        if body then
            Carried.change(body)
        end
    end
    Carried.light(true)
    return true
end
change.Parent = Workspace

for _, player in NetworkService:GetPlayers() do
    arrive(player)
end
NetworkService.PlayerAdded:Connect(arrive)
-- The joiner leaves when it has said what it saw; there is nobody else.
NetworkService.PlayerRemoving:Connect(function()
    game:Shutdown()
end)
