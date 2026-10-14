--!strict
--[[
	A wall broken into pieces (ADR 0215).

	What does each job:

	- **The cut**: `wall:Fracture({ Pieces = 40, Seed = 7 })` makes a model of
	  forty pieces of the wall in its place, each held to the ones beside it
	  by a `FixedConstraint` that breaks under a strength -- twice the wall's
	  weight unless the game says otherwise. It stands as it stood.
	- **The blow**: a heavy ball rolled at it. Where the ball lands, joints
	  carry more than they hold, break, and the pieces fall: they are parts,
	  simulated and, in a match, sent as parts.
	- **The dust is not simulation**: each joint's `Broken` emits a burst of a
	  `ParticleEmitter` where it was -- drawn, and nothing more.

	`--scene-data` puts it in a state for a picture, as a gate does:
	`{"roll": true}` rolls a ball at once.
]]

local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

type Demo = { roll: boolean? }
local demo = (SceneService:GetLoadData() or {}) :: Demo

Lighting.ClockTime = 14

local floor = Instance.new("Part")
floor.Name = "Ground"
floor.Anchored = true
floor.Size = vector.create(200, 1, 200)
floor.Position = vector.create(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(110, 120, 100))
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(vector.create(9, 4, 12), vector.create(0, 1.8, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The dust: one emitter, moved to each joint that gives and burst there.
local dust = Instance.new("Part")
dust.Name = "Dust"
dust.Anchored = true
dust.CanCollide = false
dust:SetMaterialParameter("Transparency", 1)
dust.Size = vector.create(0.2, 0.2, 0.2)
dust.Parent = workspace
local puff = Instance.new("ParticleEmitter")
puff.Rate = 0
puff.Lifetime = 1.2
puff.Speed = 1.5
puff.SpreadAngle = 180
puff.Acceleration = vector.create(0, -1, 0)
puff.Color = Color3.fromRGB(190, 180, 165)
puff.ColorEnd = Color3.fromRGB(160, 150, 140)
puff.Size = 0.35
puff.SizeEnd = 0.9
puff.Transparency = 0.2
puff.TransparencyEnd = 1
puff.Parent = dust

local wall: Model? = nil
local broken = 0

local function build()
    if wall then
        wall:Destroy()
    end
    broken = 0
    local whole = Instance.new("Part")
    whole.Name = "Wall"
    whole.Anchored = true
    whole.Size = vector.create(8, 4, 0.5)
    whole.Position = vector.create(0, 2, 0)
    whole:SetMaterialParameter("Color", Color3.fromRGB(176, 120, 92))
    whole.Parent = workspace
    local pieces = whole:Fracture({ Pieces = 40, Seed = 7 })
    for _, descendant in pieces:GetDescendants() do
        if descendant:IsA("FixedConstraint") then
            local joint = descendant :: FixedConstraint
            joint.Broken:Connect(function()
                broken += 1
                local at = joint.Attachment0
                if at then
                    dust.Position = at.WorldCFrame.Position
                    puff:Emit(12)
                end
            end)
        end
    end
    wall = pieces
end

local balls: { Part } = {}
local function roll()
    local ball = Instance.new("Part")
    ball.Name = "Ball"
    ball.Shape = Enum.PartShape.Ball
    ball.Size = vector.create(1.6, 1.6, 1.6)
    ball.Density = 40
    ball.Position = vector.create(-1, 1.6, 10)
    ball:SetMaterialParameter("Color", Color3.fromRGB(60, 62, 70))
    ball.Parent = workspace
    -- Sixteen metres a second at the wall.
    ball:ApplyImpulse(vector.create(0, 0, -16) * ball.Mass)
    table.insert(balls, ball)
end

build()
if demo.roll then
    roll()
end

local context = Instance.new("InputContext")
context.Name = "Wall"
context.Parent = InputService
local function button(name: string, codes: { EnumKeyCode }, pressed: () -> ())
    local action = Instance.new("InputAction")
    action.Name = name
    action.Type = Enum.InputActionType.Bool
    action.Parent = context
    for _, code in codes do
        local binding = Instance.new("InputBinding")
        binding.KeyCode = code
        binding.Parent = action
    end
    action.Pressed:Connect(pressed)
end
button("Roll", { Enum.KeyCode.Space, Enum.KeyCode.ButtonSouth }, roll)
button("Build", { Enum.KeyCode.R, Enum.KeyCode.ButtonNorth }, function()
    for _, ball in balls do
        ball:Destroy()
    end
    table.clear(balls)
    build()
end)

local gui = Instance.new("ScreenGui")
gui.Name = "Hud"
gui.Parent = UIService
local function label(name: string, y: number, size: number): TextLabel
    local made = Instance.new("TextLabel")
    made.Name = name
    made.BackgroundTransparency = 1
    made.Position = UDim2.new(0, 24, 0, y)
    made.Size = UDim2.new(0, 800, 0, size + 8)
    made.TextSize = size
    made.TextColor = Color3.new(1, 1, 1)
    made.TextXAlignment = Enum.HorizontalAlignment.Left
    made.Parent = gui
    return made
end
local keys = label("Keys", 16, 16)
local count = label("Count", 44, 20)
local function say()
    keys.Text = LocalizationService:Translate("hud.keys")
end
say()
LocalizationService.LocaleChanged:Connect(say)

-- Said once, three seconds after a roll asked for by `--scene-data`: what
-- `example_wall_breaks` asks of it.
local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if demo.roll and ticks == 180 then
        print(`broke {broken} joints`)
    end
end)
RunService.Heartbeat:Connect(function()
    local pieces = if wall then #wall:GetChildren() else 0
    count.Text = LocalizationService:Translate("hud.pieces", { pieces = pieces, broken = broken })
end)
