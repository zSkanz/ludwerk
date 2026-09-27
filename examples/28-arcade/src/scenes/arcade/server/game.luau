--!strict
--[[
	The game inside a cabinet (ADR 0107 §3): catch what falls.

	It runs in a world of its own. This file is the scene's own code, and
	nothing of the arcade hall around it is here -- not its parts, not its
	scripts, not its keyboard. It hears the hall through
	`SceneService.HostMessageReceived` ("coin" starts a game) and answers through
	`SceneService:SendToHost` (the score, and the end of a game). Its one input
	is the `Move` action, which the hall drives with `SubWorld:SetInputState`.

	Until a coin, it plays itself: the attract mode every cabinet has.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

-- A screen is dark around what it draws, and what it draws is bright: no sun,
-- and a strong even light on everything.
Lighting.ClockTime = 0
Lighting.Ambient = Color3.fromRGB(230, 230, 240)
Lighting.OutdoorAmbient = Color3.fromRGB(230, 230, 240)

-- Straight at the playfield, which is twenty metres wide and twelve high.
local camera = Instance.new("Camera")
camera.Name = "Screen"
camera.FieldOfView = 40
camera.CFrame = CFrame.new(0, 0, 20)
camera.Parent = workspace
workspace.CurrentCamera = camera

local function block(name: string, position: vector, size: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = size
    part.CFrame = CFrame.new(position.x, position.y, position.z)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

local Cyan = Color3.fromRGB(60, 230, 255)
local Pink = Color3.fromRGB(255, 70, 170)
local Gold = Color3.fromRGB(255, 200, 60)

block("Backdrop", vector.create(0, 0, -1), vector.create(40, 24, 0.5), Color3.new(0, 0, 0))
-- The playfield's walls: the edges of the game, drawn.
block("Left", vector.create(-10.5, 0, 0), vector.create(0.3, 13, 0.3), Pink)
block("Right", vector.create(10.5, 0, 0), vector.create(0.3, 13, 0.3), Pink)
block("Top", vector.create(0, 6.35, 0), vector.create(21.3, 0.3, 0.3), Pink)

local PaddleY = -5
local paddle = block("Paddle", vector.create(0, PaddleY, 0), vector.create(3, 0.5, 0.6), Cyan)

-- Three lamps, one per life left.
local lamps: { Part } = {}
for index = 1, 3 do
    table.insert(
        lamps,
        block(`Life{index}`, vector.create(-9.6 + index * 0.9, 5.5, 0), vector.create(0.5, 0.5, 0.5), Gold)
    )
end

-- The one input this world has. The hall decides what drives it.
local controls = Instance.new("InputContext")
controls.Parent = script
local move = Instance.new("InputAction")
move.Name = "Move"
move.Type = Enum.InputActionType.Direction1D
move.Parent = controls

type Falling = { Part: Part, Speed: number, Spin: number }

-- Seeded, so a game is the same game on every run of the same inputs. The
-- hall sends each cabinet a seed of its own.
local random = Random.new(1987)
local falling: { Falling } = {}
local playing = false
local score = 0
local lives = 3
local spawnIn = 0
local clock = 0

local Colors = { Cyan, Pink, Gold, Color3.fromRGB(120, 255, 120) }

local function spawn()
    local color = Colors[random:NextInteger(1, #Colors)]
    local speed = random:NextNumber(4, 7) + (if playing then score * 0.15 else 0)
    local part = block("Falling", vector.create(random:NextNumber(-9, 9), 7, 0), vector.create(0.8, 0.8, 0.8), color)
    table.insert(falling, { Part = part, Speed = speed, Spin = random:NextNumber(-3, 3) })
end

local function clear()
    for _, item in falling do
        item.Part:Destroy()
    end
    table.clear(falling)
end

SceneService.HostMessageReceived:Connect(function(kind: string, value: number?)
    if kind == "seed" and value then
        random = Random.new(value)
        clear()
    elseif kind == "coin" and not playing then
        playing = true
        score = 0
        lives = 3
        clear()
        SceneService:SendToHost("score", score)
    end
end)

RunService.Heartbeat:Connect(function(dt: number)
    clock += dt

    -- The paddle: the player's hand, or the attract mode's.
    local x = paddle.Position.x
    if playing then
        x += (move:GetState() :: number) * 15 * dt
    else
        local lowest: Falling? = nil
        for _, item in falling do
            if lowest == nil or item.Part.Position.y < lowest.Part.Position.y then
                lowest = item
            end
        end
        local target = if lowest then lowest.Part.Position.x else math.sin(clock) * 4
        x += math.clamp(target - x, -11 * dt, 11 * dt)
    end
    x = math.clamp(x, -8.8, 8.8)
    paddle.CFrame = CFrame.new(x, PaddleY, 0)

    spawnIn -= dt
    if spawnIn <= 0 then
        spawn()
        spawnIn = random:NextNumber(0.45, 1.0)
    end

    for index = #falling, 1, -1 do
        local item = falling[index]
        local position = item.Part.Position
        local y = position.y - item.Speed * dt
        if y < PaddleY + 0.6 and y > PaddleY - 0.4 and math.abs(position.x - x) < 1.9 then
            item.Part:Destroy()
            table.remove(falling, index)
            if playing then
                score += 1
                SceneService:SendToHost("score", score)
            end
        elseif y < -7.5 then
            item.Part:Destroy()
            table.remove(falling, index)
            if playing then
                lives -= 1
                if lives == 0 then
                    playing = false
                    SceneService:SendToHost("gameover", score)
                end
            end
        else
            item.Part.CFrame = CFrame.new(position.x, y, 0) * CFrame.fromEuler(0, 0, clock * item.Spin)
        end
    end

    for index, lamp in lamps do
        lamp:SetMaterialParameter("Transparency", if not playing or index <= lives then 0 else 0.85)
    end
end)
