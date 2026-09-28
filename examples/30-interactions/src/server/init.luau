--!strict
--[[
	The room and what can be used in it (ADR 0126), built by the authority: a
	`Script` under `GlobalScriptService.Server`, from `src/server/`. Solo this
	machine is the authority; in a match it is the server, and the events below
	fire here with the player who pressed -- after the engine has checked that
	player's character could reach what they pressed.

	- A button on the wall, with a `ClickDetector`: click it and the lamp turns
	  on or off.
	- A door, with a `ProximityPrompt`: come near and hold E -- or tap it -- and
	  it swings open, or shut.

	There is no raycast, no key handler and no distance check in this file.
]]

local Lighting = game:GetService("Lighting")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local Walk = { Speed = 6, Height = 0.9, Bound = 7.4 }

Lighting.Ambient = Color3.fromRGB(28, 30, 36)
Lighting.ClockTime = 20

local function block(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

-- The room: sixteen metres square, one wall with a doorway.
local Wall = Color3.fromRGB(196, 190, 178)
block("Floor", vector.create(16, 1, 16), vector.create(0, -0.5, 0), Color3.fromRGB(120, 96, 72))
block("WallNorth", vector.create(16, 4, 0.4), vector.create(0, 2, -8), Wall)
block("WallWest", vector.create(0.4, 4, 16), vector.create(-8, 2, 0), Wall)
block("WallEast", vector.create(0.4, 4, 16), vector.create(8, 2, 0), Wall)
block("WallSouthLeft", vector.create(6.5, 4, 0.4), vector.create(-4.75, 2, 8), Wall)
block("WallSouthRight", vector.create(6.5, 4, 0.4), vector.create(4.75, 2, 8), Wall)

-- --- The lamp and its button ------------------------------------------------------

local lamp = block("Lamp", vector.create(0.6, 0.6, 0.6), vector.create(0, 3.4, 0), Color3.fromRGB(255, 236, 190))
local light = Instance.new("PointLight")
light.Range = 14
light.Brightness = 0
light.Parent = lamp

local button = block("Button", vector.create(0.5, 0.5, 0.2), vector.create(-3, 1.4, -7.7), Color3.fromRGB(200, 40, 40))
local click = Instance.new("ClickDetector")
click.MaxActivationDistance = 6
click.Parent = button

local lit = false
click.MouseClick:Connect(function(player: Player)
    lit = not lit
    light.Brightness = if lit then 3 else 0
    button:SetMaterialParameter("Color", if lit then Color3.fromRGB(60, 200, 90) else Color3.fromRGB(200, 40, 40))
    print(`30-interactions: player {player.UserId} turned the lamp {if lit then "on" else "off"}`)
end)
click.MouseHoverEnter:Connect(function()
    button.Size = vector.create(0.55, 0.55, 0.25)
end)
click.MouseHoverLeave:Connect(function()
    button.Size = vector.create(0.5, 0.5, 0.2)
end)

-- --- The door -------------------------------------------------------------------------

-- Hinged at its left edge: the door turns about the hinge, so its centre is
-- half its width out from it.
local Hinge = vector.create(-1.5, 1.75, 8)
local door = block("Door", vector.create(3, 3.5, 0.2), Hinge + vector.create(1.5, 0, 0), Color3.fromRGB(110, 70, 40))
local prompt = Instance.new("ProximityPrompt")
prompt.ActionText = "Open"
prompt.ObjectText = "Door"
prompt.HoldDuration = 0.4
prompt.MaxActivationDistance = 4
prompt.Parent = door

local open = false
local angle = 0
prompt.Triggered:Connect(function(player: Player)
    open = not open
    prompt.ActionText = if open then "Close" else "Open"
    print(`30-interactions: player {player.UserId} {if open then "opened" else "closed"} the door`)
end)

-- --- A character for every player -----------------------------------------------

local characters: { [Player]: BasePart } = {}

local function spawnCharacter(player: Player)
    local body = block(
        `Player{player.UserId}`,
        vector.create(0.8, 1.8, 0.8),
        vector.create((player.UserId - 1) * 1.5, Walk.Height, 3),
        Color3.fromHSV((player.UserId * 0.37) % 1, 0.6, 0.95)
    )
    characters[player] = body
    -- What makes it theirs, and what the engine measures reach from.
    player.Character = body
end

for _, player in NetworkService:GetPlayers() do
    spawnCharacter(player)
end
NetworkService.PlayerAdded:Connect(spawnCharacter)
NetworkService.PlayerRemoving:Connect(function(player: Player)
    local body = characters[player]
    if body then
        body:Destroy()
        characters[player] = nil
    end
end)

RunService.Heartbeat:Connect(function(dt: number)
    -- The door eases towards where it was asked to be.
    local target = if open then -math.pi * 0.5 else 0
    angle += (target - angle) * math.min(1, dt * 6)
    door.CFrame = CFrame.new(Hinge) * CFrame.fromEuler(0, angle, 0) * CFrame.new(1.5, 0, 0)

    -- What each player pressed, the same way for this machine's and for the
    -- ones across the network; the room's walls are its bounds, and the doorway
    -- is shut while the door is.
    for player, body in characters do
        local intent = player:GetIntent("Move")
        if typeof(intent) == "Vector2" then
            local at = body.Position + vector.create(intent.X, 0, -intent.Y) * Walk.Speed * dt
            local x = math.clamp(at.x, -Walk.Bound, Walk.Bound)
            local z = math.clamp(at.z, -Walk.Bound, if open and math.abs(x) < 1.2 then 12 else Walk.Bound)
            body.Position = vector.create(x, Walk.Height, z)
        end
    end
end)

print("30-interactions: WASD walks. Click the red button; hold E by the door.")
