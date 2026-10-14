--!strict
--[[
	The authority: a floor and a wall; on the joiner's word the wall is broken
	into fifty pieces and struck at its top, and the pieces fall where they
	will. Each player stands aside, out of the way.
]]

local NetworkService = game:GetService("NetworkService")
local Workspace = game:GetService("Workspace")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(200, 1, 200)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

local wall = Instance.new("Part")
wall.Name = "Wall"
wall.Anchored = true
wall.Size = vector.create(8, 4, 0.5)
wall.Position = vector.create(0, 2, -10)
wall.Parent = Workspace

local function arrive(player: Player)
    local body = Instance.new("CharacterBody")
    body.Name = `Body{player.UserId}`
    body.Size = vector.create(1, 2, 1)
    body.Position = vector.create(player.UserId * 3, 1.5, 6)
    body.Parent = Workspace
    player.Character = body
end
for _, player in NetworkService:GetPlayers() do
    arrive(player)
end
NetworkService.PlayerAdded:Connect(arrive)

local model: Model? = nil

local cut = Instance.new("RemoteFunction")
cut.Name = "Cut"
cut.Parent = Workspace
cut.OnServerInvoke = function(): number
    local made = wall:Fracture({ Pieces = 50, Seed = 9 })
    model = made
    local count = 0
    for _, child in made:GetChildren() do
        if child:IsA("BasePart") and child.Name ~= "Footing" then
            count += 1
        end
    end
    return count
end

local strike = Instance.new("RemoteFunction")
strike.Name = "Strike"
strike.Parent = Workspace
strike.OnServerInvoke = function(): boolean
    local held = model
    if held == nil then
        return false
    end
    local top: BasePart? = nil
    for _, child in held:GetChildren() do
        if child:IsA("BasePart") and child.Name ~= "Footing" then
            local piece = child :: BasePart
            if top == nil or piece.Position.y > (top :: BasePart).Position.y then
                top = piece
            end
        end
    end
    if top then
        top:ApplyImpulse(vector.create(0, 0, -20) * top.Mass)
    end
    return true
end
