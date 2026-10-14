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

-- **The authority's own long frames**, for the bot to count beside its own: a
-- server that stalls catches up and answers late, and on a machine running
-- the rest of the suite -- a CI runner's two cores -- that is a correction
-- the client did nothing to cause.
local longFrames = 0
local lastFrame = os.clock()
RunService.Heartbeat:Connect(function()
    local now = os.clock()
    if now - lastFrame > 0.05 then
        longFrames += 1
    end
    lastFrame = now
end)
local longFramesAsked = Instance.new("RemoteFunction")
longFramesAsked.Name = "LongFrames"
longFramesAsked.Parent = Workspace
longFramesAsked.OnServerInvoke = function(): number
    return longFrames
end

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

-- Where the asker's character is -- or, named by its number, the character
-- of the guest playing at the asker's machine (ADR 0208).
local function bodyOf(player: Player, user: number?): CharacterBody?
    if user ~= nil then
        for other, body in bodies do
            if other.UserId == user then
                return body
            end
        end
        return nil
    end
    return bodies[player]
end

where.OnServerInvoke = function(player: Player, user: number?): vector
    local body = bodyOf(player, user)
    return if body then body.Position else vector.zero
end

-- **The authority's word a metre off, for one character**: put aside, as a
-- game's own rule might -- a hit, a gust, a door. The bot asks for it in the
-- middle of a walk and measures what its OTHER character does.
local nudge = Instance.new("RemoteFunction")
nudge.Name = "Nudge"
nudge.Parent = Workspace
nudge.OnServerInvoke = function(player: Player, user: number?): boolean
    local body = bodyOf(player, user)
    if body then
        body.Position += vector.create(1, 0, 0)
    end
    return body ~= nil
end

RunService.Heartbeat:Connect(function()
    for player, body in bodies do
        Drive.step(player, body)
    end
end)
