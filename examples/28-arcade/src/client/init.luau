--!strict
--[[
	An arcade (ADR 0107 §3): two cabinets in a dark hall, each running a small
	game in a world of its own.

	**Each cabinet is a `SubWorld`**: `Load()` starts `scenes/arcade.scene.json`
	in a world with its own instances, scripts, physics and clock, and its
	camera draws into `view://arcade1` or `view://arcade2`, shown on the
	cabinet's screen by an ordinary `ImageLabel`. Until somebody plays, each
	one plays itself.

	**Nothing crosses but what is sent.** Press E at the left cabinet: the hall
	sends it a coin (`Send`), passes the arrow keys on as its `Move` action
	(`SetInputState`), and reads the score it sends back (`Received`) onto the
	cabinet's marquee. The game cannot see the hall, and the hall cannot see
	the game's parts.

	Keys: E plays or leaves; A and D, or the arrows, move.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 20.5
Lighting.Ambient = Color3.fromRGB(40, 36, 60)
Lighting.OutdoorAmbient = Color3.fromRGB(40, 36, 60)

local function block(name: string, cframe: CFrame, size: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = size
    part.CFrame = cframe
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

block("Floor", CFrame.new(0, -0.5, 0), vector.create(30, 1, 20), Color3.fromRGB(40, 30, 50))
block("BackWall", CFrame.new(0, 4, -4), vector.create(30, 9, 0.5), Color3.fromRGB(55, 40, 80))

for index, x in { -7, 0, 7 } do
    local lamp = Instance.new("PointLight")
    lamp.Color = if index == 2 then Color3.fromRGB(255, 120, 220) else Color3.fromRGB(120, 180, 255)
    lamp.Brightness = 2
    lamp.Range = 14
    local holder = block("Lamp", CFrame.new(x, 7.5, 0), vector.create(0.4, 0.4, 0.4), Color3.fromRGB(255, 255, 255))
    lamp.Parent = holder
end

-- --- The cabinets ------------------------------------------------------------

type Cabinet = {
    World: SubWorld,
    Marquee: TextLabel,
    Screen: Part,
    Best: number,
}

local Facing = vector.create(0, 0, 1)

local function cabinet(index: number, x: number, color: Color3): Cabinet
    local base = vector.create(x, 0, -2)
    block("Body", CFrame.new(x, 2.4, -2.4), vector.create(2.4, 4.8, 1.6), color)
    block("Panel", CFrame.new(x, 2.2, -1.35) * CFrame.fromEuler(-0.5, 0, 0), vector.create(2.4, 0.15, 0.8), color)

    -- The screen faces the room: a `SurfaceGui` on its front face shows the
    -- game's picture.
    local screenAt = base + vector.create(0, 3.4, 0.42)
    local screen = block(
        "Screen",
        CFrame.lookAt(screenAt, screenAt + Facing),
        vector.create(2.0, 1.125, 0.05),
        Color3.new(0, 0, 0)
    )
    local gui = Instance.new("SurfaceGui")
    gui.Face = Enum.Face.Front
    -- As many canvas pixels as the picture has: 2 metres at 256 is 512.
    gui.PixelsPerMetre = 256
    gui.Parent = screen
    local picture = Instance.new("ImageLabel")
    picture.Size = UDim2.fromScale(1, 1)
    picture.BackgroundTransparency = 1
    picture.Image = `view://arcade{index}`
    picture.Parent = gui

    -- The marquee on top, where the score the game sends back is written.
    local marqueeAt = base + vector.create(0, 4.35, 0.46)
    local marquee = block(
        "Marquee",
        CFrame.lookAt(marqueeAt, marqueeAt + Facing),
        vector.create(2.4, 0.5, 0.1),
        Color3.fromRGB(20, 10, 30)
    )
    local sign = Instance.new("SurfaceGui")
    sign.Face = Enum.Face.Front
    sign.PixelsPerMetre = 100
    sign.Parent = marquee
    local text = Instance.new("TextLabel")
    text.Size = UDim2.fromScale(1, 1)
    text.BackgroundTransparency = 1
    text.TextColor = Color3.fromRGB(255, 230, 120)
    text.TextSize = 22
    text.Text = "INSERT COIN"
    text.Parent = sign

    local world = Instance.new("SubWorld")
    world.Name = `Cabinet{index}`
    world.Scene = "scenes/arcade.scene.json"
    world.ViewName = `arcade{index}`
    world.Resolution = Vector2.new(512, 288)
    world.Parent = screen
    world:Load()
    -- Its own seed, so the two cabinets are not playing the same game.
    world:Send("seed", index * 7919)

    local self: Cabinet = { World = world, Marquee = text, Screen = screen, Best = 0 }
    world.Received:Connect(function(kind: string, score: number)
        if kind == "score" then
            text.Text = `SCORE {score}`
        elseif kind == "gameover" then
            self.Best = math.max(self.Best, score)
            text.Text = `GAME OVER  {score}  BEST {self.Best}`
        end
    end)
    return self
end

local Cabinets = {
    cabinet(1, -2.2, Color3.fromRGB(150, 40, 110)),
    cabinet(2, 2.2, Color3.fromRGB(40, 90, 160)),
}

-- --- The player --------------------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.Parent = workspace
workspace.CurrentCamera = camera

local Standing = CFrame.lookAt(vector.create(0, 3.2, 7), vector.create(0, 2.8, -2))
local left = Cabinets[1].Screen.Position
local Playing = CFrame.lookAt(left + vector.create(0, 0.1, 2.2), left)

local Controls = Instance.new("InputContext")
Controls.Name = "Arcade"
Controls.Parent = workspace

local Use = Instance.new("InputAction")
Use.Name = "Use"
Use.Parent = Controls
local UseKey = Instance.new("InputBinding")
UseKey.KeyCode = Enum.KeyCode.E
UseKey.Parent = Use

local Steer = Instance.new("InputAction")
Steer.Name = "Steer"
Steer.Type = Enum.InputActionType.Direction1D
Steer.Parent = Controls
for _, keys in { { Enum.KeyCode.D, Enum.KeyCode.A }, { Enum.KeyCode.Right, Enum.KeyCode.Left } } do
    local binding = Instance.new("InputBinding")
    binding.Up = keys[1]
    binding.Down = keys[2]
    binding.Parent = Steer
end

local hud = Instance.new("ScreenGui")
hud.Parent = UIService
local hint = Instance.new("TextLabel")
hint.AnchorPoint = Vector2.new(0.5, 1)
hint.Position = UDim2.new(0.5, 0, 1, -24)
hint.Size = UDim2.fromOffset(640, 28)
hint.BackgroundTransparency = 1
hint.TextColor = Color3.fromRGB(230, 220, 255)
hint.TextSize = 18
hint.Text = "E  play the left cabinet"
hint.Parent = hud

local playing = false
Use.Pressed:Connect(function()
    playing = not playing
    local machine = Cabinets[1].World
    if playing then
        machine:Send("coin")
        hint.Text = "A / D  move     E  leave"
    else
        machine:SetInputState("Move", 0)
        hint.Text = "E  play the left cabinet"
    end
end)

-- The hall's keys reach the game only while somebody is playing it.
Steer.StateChanged:Connect(function()
    if playing then
        Cabinets[1].World:SetInputState("Move", Steer:GetState() :: number)
    end
end)

local view = Standing
RunService.Heartbeat:Connect(function(dt: number)
    local goal = if playing then Playing else Standing
    view = view:Lerp(goal, math.min(1, dt * 4))
    camera.CFrame = view
end)
camera.CFrame = view
