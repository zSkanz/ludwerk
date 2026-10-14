--!strict
--[[
	A vehicle (ADR 0213): a car of parts on four wheels, driven by the keyboard
	or a controller.

	What does each job:

	- **The chassis** is one unanchored part, and a `VehicleController` under
	  it makes it a car: an engine, a gearbox, and what the driver asks.
	- **The wheels are not parts.** A `VehicleWheel` under the controller, at
	  an `Attachment` of the chassis, is a suspension and a tire: each tick a
	  cylinder is cast down from the attachment, and where it meets the
	  ground it holds the car up and pushes it along. Its `WheelPart` is a
	  picture of it, placed where the wheel is, turned and rolling.
	- **Driving** is two input actions read each frame into the controller's
	  `Throttle`, `Steer` and `HandBrake`. Front wheels steer, rear wheels are
	  driven.
	- **The camera** follows behind, and the speed and the gear the gearbox
	  chose are read back from the controller for the screen.

	`--scene-data` puts it in a state for a picture, as a gate does:
	`{"throttle": 1, "steer": 0.4}` drives on its own.
]]

local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

type Demo = { throttle: number?, steer: number? }
local demo = (SceneService:GetLoadData() or {}) :: Demo

Lighting.ClockTime = 15

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

-- --- The ground ----------------------------------------------------------------------

block("Ground", vector.create(600, 1, 600), vector.create(0, -0.5, 0), Color3.fromRGB(96, 120, 84))
block("Road", vector.create(14, 0.05, 400), vector.create(0, 0.03, -150), Color3.fromRGB(62, 64, 70))
local ramp = block("Ramp", vector.create(10, 0.6, 12), vector.create(0, 1.2, -90), Color3.fromRGB(190, 160, 110))
ramp.CFrame = CFrame.new(0, 1.2, -90) * CFrame.fromEuler(math.rad(14), 0, 0)
-- Cones, unanchored, to knock over.
for row = 0, 5 do
    local cone = Instance.new("Part")
    cone.Name = "Cone"
    cone.Size = vector.create(0.6, 0.9, 0.6)
    cone.Position = vector.create(-4 + (row % 2) * 8, 0.45, -30 - row * 10)
    cone:SetMaterialParameter("Color", Color3.fromRGB(240, 120, 30))
    cone.Parent = workspace
end

-- --- The car -------------------------------------------------------------------------

local chassis = Instance.new("Part")
chassis.Name = "Car"
chassis.Size = vector.create(1.9, 0.5, 4.2)
chassis.Position = vector.create(0, 1.2, 0)
chassis:SetMaterialParameter("Color", Color3.fromRGB(200, 40, 50))
chassis.Parent = workspace

-- A cabin welded on top, for the look of a car.
local cabin = Instance.new("Part")
cabin.Name = "Cabin"
cabin.Size = vector.create(1.6, 0.6, 2)
cabin.CFrame = chassis.CFrame * CFrame.new(0, 0.55, 0.3)
cabin:SetMaterialParameter("Color", Color3.fromRGB(30, 34, 44))
cabin.Parent = chassis
local weld = Instance.new("WeldConstraint")
weld.Part0 = chassis
weld.Part1 = cabin
weld.Parent = cabin

local controller = Instance.new("VehicleController")
controller.Parent = chassis

local wheels: { VehicleWheel } = {}
-- Left before right, so the rear pair is one differential.
for _, corner in { { -0.95, -1.4 }, { 0.95, -1.4 }, { -0.95, 1.4 }, { 0.95, 1.4 } } do
    local front = corner[2] < 0
    local hub = Instance.new("Attachment")
    hub.CFrame = CFrame.new(corner[1], -0.1, corner[2])
    hub.Parent = chassis

    local tire = Instance.new("Part")
    tire.Name = "Tire"
    tire.Anchored = true
    tire.CanCollide = false
    -- Placed with its X along the axle: as wide as the wheel, as tall and
    -- as long as it is round.
    tire.Size = vector.create(0.28, 0.7, 0.7)
    tire:SetMaterialParameter("Color", Color3.fromRGB(24, 24, 26))
    tire.Parent = workspace

    local wheel = Instance.new("VehicleWheel")
    wheel.Attachment = hub
    wheel.WheelPart = tire
    wheel.Radius = 0.35
    wheel.Width = 0.28
    wheel.MaxSteerAngle = if front then 32 else 0
    wheel.Driven = not front
    wheel.MaxHandBrakeTorque = if front then 0 else 8
    wheel.Parent = controller
    table.insert(wheels, wheel)
end

-- --- Driving -------------------------------------------------------------------------

local context = Instance.new("InputContext")
context.Name = "Driving"
context.Parent = InputService

local drive = Instance.new("InputAction")
drive.Name = "Drive"
drive.Type = Enum.InputActionType.Direction2D
drive.Parent = context
local keys = Instance.new("InputBinding")
keys.Left = Enum.KeyCode.A
keys.Right = Enum.KeyCode.D
keys.Up = Enum.KeyCode.W
keys.Down = Enum.KeyCode.S
keys.Parent = drive
local stick = Instance.new("InputBinding")
stick.KeyCode = Enum.KeyCode.LeftThumbstick
stick.Parent = drive

local function button(name: string, codes: { EnumKeyCode }): InputAction
    local action = Instance.new("InputAction")
    action.Name = name
    action.Type = Enum.InputActionType.Bool
    action.Parent = context
    for _, code in codes do
        local binding = Instance.new("InputBinding")
        binding.KeyCode = code
        binding.Parent = action
    end
    return action
end
local handBrake = button("HandBrake", { Enum.KeyCode.Space, Enum.KeyCode.ButtonEast })
local upright = button("Upright", { Enum.KeyCode.R, Enum.KeyCode.ButtonNorth })
upright.Pressed:Connect(function()
    -- Back on its wheels where it is, facing where it faced.
    local look = chassis.CFrame.LookVector
    local at = chassis.Position
    chassis.CFrame = CFrame.lookAt(at + vector.create(0, 2, 0), at + vector.create(look.x, 2, look.z))
    chassis.LinearVelocity = vector.zero
    chassis.AngularVelocity = vector.zero
end)

RunService.PreSimulation:Connect(function()
    local wanted = drive:GetState()
    local throttle, steer = 0, 0
    if typeof(wanted) == "Vector2" then
        throttle, steer = wanted.Y, wanted.X
    end
    controller.Throttle = demo.throttle or throttle
    controller.Steer = demo.steer or steer
    controller.HandBrake = if handBrake:GetState() == true then 1 else 0
end)

-- --- The camera and the screen -------------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 65
camera.Parent = workspace
workspace.CurrentCamera = camera
local behind = chassis.Position + vector.create(0, 3, 8)
RunService.PreRender:Connect(function(dt: number)
    local look = chassis.CFrame.LookVector
    local flat = vector.normalize(vector.create(look.x, 0, look.z))
    local wanted = chassis.Position - flat * 8 + vector.create(0, 3, 0)
    behind = behind + (wanted - behind) * math.min(1, dt * 5)
    camera.CFrame = CFrame.lookAt(behind, chassis.Position + vector.create(0, 1, 0))
end)

local gui = Instance.new("ScreenGui")
gui.Name = "Dashboard"
gui.Parent = UIService

local function label(name: string, position: UDim2, size: number): TextLabel
    local made = Instance.new("TextLabel")
    made.Name = name
    made.BackgroundTransparency = 1
    made.Position = position
    made.Size = UDim2.new(0, 600, 0, size + 8)
    made.TextSize = size
    made.TextColor = Color3.new(1, 1, 1)
    made.TextXAlignment = Enum.HorizontalAlignment.Left
    made.Parent = gui
    return made
end
local speed = label("Speed", UDim2.new(0, 24, 1, -110), 40)
local gear = label("Gear", UDim2.new(0, 24, 1, -62), 22)
local help = label("Keys", UDim2.new(0, 24, 0, 16), 16)

local function say()
    help.Text = LocalizationService:Translate("hud.keys")
end
say()
LocalizationService.LocaleChanged:Connect(say)

-- Driven by `--scene-data`: said once, four seconds in, how far it went --
-- what `example_vehicle_drives` asks of it.
local start = chassis.Position
local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if demo.throttle ~= nil and ticks == 240 then
        local moved = chassis.Position - start
        print(`drove {math.floor(vector.magnitude(vector.create(moved.x, 0, moved.z)))} m`)
    end
end)

RunService.Heartbeat:Connect(function()
    speed.Text = LocalizationService:Translate("hud.speed", { speed = math.floor(math.abs(controller.Speed) * 3.6) })
    local current = controller.CurrentGear
    gear.Text = if current < 0
        then LocalizationService:Translate("hud.reverse")
        elseif current == 0 then LocalizationService:Translate("hud.neutral")
        else LocalizationService:Translate("hud.gear", { gear = current })
end)
