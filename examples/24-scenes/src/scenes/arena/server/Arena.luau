--!strict
--[[
	The arena scene's rules (ADR 0105): a `Script` under this scene's
	`ServerScriptService`, from `src/scenes/arena/server/`. It runs where the
	world is decided -- solo, or a host -- and it builds the arena, which a
	client that joined receives by replication.

	**A character per player**, made when the arena loads or when they join,
	gone when they leave. `player.Character` is what makes it theirs: their own
	machine moves it at once, and is sent what is near it (ADR 0076). The
	characters live in the arena's world, so they go with the arena when the
	scene changes, and the next scene's rules make their own.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local Walk = require("@shared/Walk")

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Anchored = true
ground.Size = vector.create(30, 1, 30)
ground.Position = vector.create(0, -0.5, 0)
ground:SetMaterialParameter("Color", Color3.fromRGB(90, 120, 96))
ground.Parent = workspace

local spinner = Instance.new("Part")
spinner.Name = "Spinner"
spinner.Anchored = true
spinner.Size = vector.create(10, 0.6, 1)
spinner:SetMaterialParameter("Color", Color3.fromRGB(230, 170, 40))
spinner.Parent = workspace

local characters: { [Player]: BasePart } = {}

local function spawnCharacter(player: Player)
    if characters[player] then
        return
    end
    local character = Instance.new("Part")
    character.Name = `Player{player.UserId}`
    character.Anchored = true
    character.Size = vector.create(1.2, 1.6, 1.2)
    character:SetMaterialParameter("Color", Color3.fromHSV((player.UserId * 0.37) % 1, 0.75, 1))
    -- Round the edge of the arena, one place per player, clear of the spinner.
    local angle = player.UserId * 1.1
    character.Position = vector.create(math.cos(angle) * 9, Walk.Height, math.sin(angle) * 9)
    character.Parent = workspace
    characters[player] = character
    player.Character = character
end

for _, player in NetworkService:GetPlayers() do
    spawnCharacter(player)
end
NetworkService.PlayerAdded:Connect(spawnCharacter)
NetworkService.PlayerRemoving:Connect(function(player: Player)
    local character = characters[player]
    if character then
        character:Destroy()
        characters[player] = nil
    end
end)

RunService.Heartbeat:Connect(function(dt: number)
    spinner.CFrame = CFrame.new(0, 1, 0) * CFrame.fromEuler(0, RunService.SimTime, 0)
    -- What each player pressed, read the same way for the one at this machine
    -- and for the ones across the network.
    for player, character in characters do
        Walk.step(player, character, dt)
    end
end)
