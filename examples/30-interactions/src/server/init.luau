--!strict
--[[
	The room and what can be used in it, built by the authority: a `Script`
	under `GlobalScriptService.Server`, from `src/server/`. Solo this machine is
	the authority; in a match it is the server, and everything below decides
	here, with the player who did it.

	- A button on the wall, with a `ClickDetector`: click it and the lamp turns
	  on or off. The engine picks the part under the pointer, measures the reach
	  from the character, and fires `MouseClick` with the player.
	- A door that opens when you hold E near it. **The game's own code**: the
	  distance from each player's character, the hold, and the hint -- which the
	  client draws -- are a few lines each.
	- A drawer in the desk and a lever on the wall, taken hold of with a click
	  and moved with the pointer: the client sends the pointer's ray while it
	  holds one, and this script works out where it goes, within its limits.

	A prompt and a world drag are a game's design, not the engine's: this file
	is how a game writes them (ADR 0126, as amended on 2026-10-03).
]]

local Lighting = game:GetService("Lighting")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local Walk = { Speed = 6, Height = 0.9, Bound = 7.4 }
local Reach = 4
local HoldSeconds = 0.4

Lighting.Ambient = Color3.fromRGB(28, 30, 36)
Lighting.ClockTime = 20

local function block(name: string, size: vector, position: vector, color: Color3): BasePart
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

-- --- The door: a hold within reach, the game's own -----------------------------------

-- Hinged at its left edge: the door turns about the hinge, so its centre is
-- half its width out from it. Whether it is open is an attribute, which every
-- machine sees, so a client's hint can say "Open" or "Close".
local Hinge = vector.create(-1.5, 1.75, 8)
local door = block("Door", vector.create(3, 3.5, 0.2), Hinge + vector.create(1.5, 0, 0), Color3.fromRGB(110, 70, 40))
door:SetAttribute("Open", false)
door:SetAttribute("Reach", Reach)

local angle = 0
-- How long each player has held Use near the door, and whether that hold has
-- already opened or shut it -- one turn a hold, as a prompt does.
local held: { [Player]: number } = {}
local spent: { [Player]: boolean } = {}

local function stepDoor(player: Player, body: BasePart, dt: number)
    local near = vector.magnitude(body.Position - door.Position) <= Reach
    local using = player:GetIntent("Use") == true
    if not (near and using) then
        held[player] = 0
        spent[player] = false
        return
    end
    held[player] = (held[player] or 0) + dt
    if held[player] >= HoldSeconds and not spent[player] then
        spent[player] = true
        local open = not (door:GetAttribute("Open") == true)
        door:SetAttribute("Open", open)
        print(`30-interactions: player {player.UserId} {if open then "opened" else "closed"} the door`)
    end
end

-- --- The drawer and the lever: taken hold of, and moved by the pointer ------------

local Wood = Color3.fromRGB(150, 110, 70)
block("Desk", vector.create(2.4, 0.9, 1.2), vector.create(5.5, 0.45, -6.8), Wood)
local DrawerRest = vector.create(5.5, 0.62, -6.1)
local drawer = block("Drawer", vector.create(1.6, 0.3, 1), DrawerRest, Color3.fromRGB(170, 130, 85))
local hingeBlock =
    block("LeverHinge", vector.create(0.3, 0.3, 0.3), vector.create(-7.6, 1.2, -3), Color3.fromRGB(60, 60, 66))
local lever =
    block("Lever", vector.create(0.12, 1.2, 0.12), vector.create(-7.6, 1.8, -3), Color3.fromRGB(200, 200, 210))

-- A click takes hold, and the next lets go; the reach is the click's.
for _, part in { drawer, lever } do
    local handle = Instance.new("ClickDetector")
    handle.MaxActivationDistance = 6
    handle.Parent = part
end

-- What the client sends while it holds one: where the pointer looks.
local pointer = Instance.new("RemoteEvent")
pointer.Name = "Pointer"
pointer.Parent = workspace

local holding: { [Player]: BasePart } = {}
-- Where along its runners the drawer is, 0 to half a metre, and where on that
-- line the pointer was when it was taken, so it moves from where it was held.
local drawerOut = 0
local grabbedAt: { [Player]: number } = {}

-- Where along a line a ray passes nearest: the drawer runs along +Z.
local function alongRunners(origin: vector, direction: vector): number
    local axis = vector.create(0, 0, 1)
    local apart = origin - DrawerRest
    local across = vector.dot(axis, direction)
    local denominator = 1 - across * across
    if denominator < 1e-4 then
        return drawerOut
    end
    return (vector.dot(apart, axis) - vector.dot(apart, direction) * across) / denominator
end

local function take(part: BasePart, player: Player)
    if holding[player] == part then
        holding[player] = nil
        if part == drawer then
            print(`30-interactions: player {player.UserId} left the drawer {math.floor(drawerOut * 100)} cm out`)
        end
        return
    end
    holding[player] = part
    grabbedAt[player] = nil
end
for _, part in { drawer, lever } do
    (part:FindFirstChildOfClass("ClickDetector") :: ClickDetector).MouseClick:Connect(function(player: Player)
        take(part, player)
    end)
end

pointer.ServerReceived:Connect(function(player: Player, origin: any, direction: any)
    local part = holding[player]
    if part == nil or typeof(origin) ~= "vector" or typeof(direction) ~= "vector" then
        return
    end
    if part == drawer then
        local at = alongRunners(origin, direction)
        if grabbedAt[player] == nil then
            grabbedAt[player] = at - drawerOut
        end
        drawerOut = math.clamp(at - (grabbedAt[player] :: number), 0, 0.5)
        drawer.Position = DrawerRest + vector.create(0, 0, drawerOut)
    else
        -- The lever turns about its hinge, the X axis: where the pointer's ray
        -- crosses the wall's plane says how far it leans, no more than sixty
        -- degrees either way.
        local hinge = hingeBlock.Position
        if math.abs(direction.x) < 1e-4 then
            return
        end
        local s = (hinge.x - origin.x) / direction.x
        local crossing = origin + direction * s
        local lean = math.clamp(math.atan2(crossing.z - hinge.z, crossing.y - hinge.y), math.rad(-60), math.rad(60))
        lever.CFrame = CFrame.new(hinge) * CFrame.fromEuler(lean, 0, 0) * CFrame.new(0, 0.6, 0)
        -- Straight up is the lamp at half; towards the room, brighter.
        light.Brightness = 1.5 + 1.5 * math.sin(lean) / math.sin(math.rad(60))
    end
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
    -- What makes it theirs, and what the engine measures a click's reach from.
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
    holding[player] = nil
    held[player] = nil
    spent[player] = nil
end)

RunService.Heartbeat:Connect(function(dt: number)
    -- The door eases towards where it was asked to be.
    local open = door:GetAttribute("Open") == true
    local target = if open then -math.pi * 0.5 else 0
    angle += (target - angle) * math.min(1, dt * 6)
    door.CFrame = CFrame.new(Hinge) * CFrame.fromEuler(0, angle, 0) * CFrame.new(1.5, 0, 0)

    -- What each player pressed, the same way for this machine's and for the
    -- ones across the network; the room's walls are its bounds, and the doorway
    -- is shut while the door is.
    for player, body in characters do
        stepDoor(player, body, dt)
        local intent = player:GetIntent("Move")
        if typeof(intent) == "Vector2" then
            local at = body.Position + vector.create(intent.X, 0, -intent.Y) * Walk.Speed * dt
            local x = math.clamp(at.x, -Walk.Bound, Walk.Bound)
            local z = math.clamp(at.z, -Walk.Bound, if open and math.abs(x) < 1.2 then 12 else Walk.Bound)
            body.Position = vector.create(x, Walk.Height, z)
        end
        -- A hold lets go when the player walks away from what they hold.
        local part = holding[player]
        if part and vector.magnitude(part.Position - body.Position) > 8 then
            holding[player] = nil
        end
    end
end)

print(
    "30-interactions: WASD walks. Click the red button; hold E by the door; click the drawer or the lever, move, click again."
)
