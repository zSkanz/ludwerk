--!strict
--[[
	`BasePart.CastShadow`: two slabs floating over a grey floor, seen from
	straight above -- the left one casts a shadow and the right one was asked
	to cast none. `run_cast_shadow_gate.cmake` asks, of the sun and then of a
	lamp, that the floor beside the left slab is darker than the floor beside
	the right one, and that both slabs are still drawn.

	One scene for both lights, by the frame it is on:

	  before frame 30   the sun, low in the sky, and no lamp
	  from frame 30     no sun, and a spot lamp straight above each slab

	An orthographic camera twenty metres wide over a 1000 by 500 window: a
	point x metres along and z metres down the floor is at ((x + 10) / 20,
	(z + 5) / 10) of the picture.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.lookAt(vector.create(0, 30, 0), vector.create(0, 0, 0), vector.create(0, 0, -1))

local floor = Instance.new("Part")
floor.Size = vector.create(60, 1, 60)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(150, 150, 150))
floor.Anchored = true
floor.Parent = workspace

local function slab(x: number, casts: boolean)
    local part = Instance.new("Part")
    part.Size = vector.create(3, 0.4, 3)
    part.CFrame = CFrame.new(x, 3, 0)
    part:SetMaterialParameter("Color", Color3.fromRGB(200, 60, 50))
    part.CastShadow = casts
    part.Anchored = true
    part.Parent = workspace
end

slab(-5, true)
slab(5, false)

local function lamp(x: number): SpotLight
    local host = Instance.new("Part")
    host.Size = vector.create(0.2, 0.2, 0.2)
    -- A spot shines along its host's -Z: aimed down, not placed.
    host.CFrame = CFrame.lookAt(vector.create(x, 8, 0), vector.create(x, 0, 0), vector.create(0, 0, -1))
    host:SetMaterialParameter("Transparency", 1)
    host.Anchored = true
    host.Parent = workspace
    local spot = Instance.new("SpotLight")
    spot.Range = 24
    spot.Angle = 70
    spot.Shadows = true
    spot.Parent = host
    return spot
end

local lamps = { lamp(-5), lamp(5) }

local function applyCase()
    local frame = math.floor(RunService.SimTime * 60 + 0.5) + 1
    local sun = frame < 30
    Lighting.ClockTime = 15.5
    Lighting.Brightness = if sun then 3 else 0
    local dim = if sun then Color3.fromRGB(70, 70, 70) else Color3.fromRGB(6, 6, 8)
    Lighting.Ambient = dim
    Lighting.OutdoorAmbient = dim
    for _, spot in lamps do
        spot.Brightness = if sun then 0 else 40
    end
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
