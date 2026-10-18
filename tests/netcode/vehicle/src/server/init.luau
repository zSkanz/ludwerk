--!strict
--[[
	The authority: a floor, and for each player a character and a car they
	drive -- `Driver` set, so the car takes their `Drive` and `Brake` intents
	every tick with no script (ADR 0213). An answer to "where is my car" for
	the bot to measure against, and, asked for, a parked car in its way, or
	the car moved half a metre aside once.

	For two drivers (`duel`), the second player's car is put twenty-four
	metres ahead of the first's, facing it.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Car = require("@shared/Car")
local Condition = require("@shared/Condition")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(600, 1, 600)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

local function remote(name: string): RemoteFunction
    local made = Instance.new("RemoteFunction")
    made.Name = name
    made.Parent = Workspace
    return made
end

-- The authority's own long frames, for the bot to count beside its own.
local longFrames = 0
local lastFrame = os.clock()
RunService.Heartbeat:Connect(function()
    local now = os.clock()
    if now - lastFrame > 0.05 then
        longFrames += 1
    end
    lastFrame = now
end)
remote("LongFrames").OnServerInvoke = function(): number
    return longFrames
end

local cars: { [Player]: Part } = {}

local function arrive(player: Player)
    -- A character, standing aside: a correction steps the car by its commands.
    local body = Instance.new("CharacterBody")
    body.Name = `Body{player.UserId}`
    body.Size = vector.create(1, 2, 1)
    body.WalkSpeed = 8
    body.Position = vector.create(player.UserId * 6 - 3, 1.5, 6)
    body.Parent = Workspace
    player.Character = body
    local second = false
    for _ in cars do
        second = true
    end
    local at = vector.create(player.UserId * 6, 1.2, 0)
    local facing = vector.create(0, 0, -1)
    if Condition.Mode == "duel" and second then
        local first: Part? = nil
        for _, car in cars do
            first = car
        end
        at = (first :: Part).Position + vector.create(0, 0, -24)
        facing = vector.create(0, 0, 1)
    end
    local chassis, controller = Car.build(`Car{player.UserId}`, at, Workspace, facing)
    controller.DriveIntent = "Drive"
    controller.BrakeIntent = "Brake"
    cars[player] = chassis
    if Condition.Mode == "late" then
        -- **Taken over moving** (`late`): the car drives itself for three
        -- seconds -- the player's machine sees it as anybody's, born on it
        -- mid-drive -- and is handed to the player still rolling.
        controller.Throttle = 0.6
        task.delay(3, function()
            controller.Throttle = 0
            controller.Driver = player
        end)
    else
        controller.Driver = player
    end
end

for _, player in NetworkService:GetPlayers() do
    arrive(player)
end
NetworkService.PlayerAdded:Connect(arrive)

-- **What the authority's own car does over the same stretch** (`Jumps`): the
-- most its place moved off the line of its last two ticks. A crash stops a
-- car in a tick on every machine; what a replica draws beyond that is the
-- link's.
local watching: { [Player]: { at: { vector }, most: number } } = {}
RunService.Heartbeat:Connect(function()
    for player, watch in watching do
        local car = cars[player]
        if car == nil then
            continue
        end
        table.insert(watch.at, car.Position)
        if #watch.at > 3 then
            table.remove(watch.at, 1)
        end
        if #watch.at == 3 then
            watch.most = math.max(watch.most, vector.magnitude(watch.at[3] - watch.at[2] * 2 + watch.at[1]))
        end
    end
end)
remote("Jumps").OnServerInvoke = function(player: Player, start: boolean): number
    if start then
        watching[player] = { at = {}, most = 0 }
        return 0
    end
    local watch = watching[player]
    watching[player] = nil
    return if watch then watch.most else 0
end

remote("Where").OnServerInvoke = function(player: Player): vector
    local car = cars[player]
    return if car then car.Position else vector.zero
end

-- **A parked car in the way** (`Condition.Touch`): twenty metres ahead of
-- the asker's, nobody's, which the bot drives into at a walking pace.
remote("Park").OnServerInvoke = function(player: Player): boolean
    local car = cars[player]
    if car == nil then
        return false
    end
    Car.build("Parked", car.Position + vector.create(0, 0, -20), Workspace)
    return true
end

-- **The car moved aside, once** (`nudge`): half a metre, by the authority
-- and by nothing the driver's machine could have predicted.
remote("Nudge").OnServerInvoke = function(player: Player): boolean
    local car = cars[player]
    if car == nil then
        return false
    end
    car.Position = car.Position + vector.create(0.5, 0, 0)
    return true
end

-- Every character walks by its player's `Move` intent, as in the character's
-- own gate: what the car's answer is measured against.
RunService.Heartbeat:Connect(function()
    for _, player in NetworkService:GetPlayers() do
        local body = player.Character
        if body and body:IsA("CharacterBody") then
            local move = player:GetIntent("Move")
            local walk = if typeof(move) == "Vector2" then vector.create(move.X, 0, -move.Y) else vector.zero
            local walker = body :: CharacterBody
            walker:Move(walk)
        end
    end
end)
