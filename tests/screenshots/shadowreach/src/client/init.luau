--!strict
-- A stationary floor point is behind a distant building at a low sun.
-- Moving the camera changes cascade selection, never whether that point
-- is covered. The paired view without the caster must be fully lit.
local Lighting = game:GetService("Lighting")
local SceneService = game:GetService("SceneService")
local Workspace = game:GetService("Workspace")

local data = SceneService:GetLoadData() :: any
local offset = if type(data) == "table" and type(data.CameraX) == "number" then data.CameraX else 0
local casts = not (type(data) == "table" and data.Cast == false)

Lighting.ClockTime = 17.55
Lighting.GeographicLatitude = 30
Lighting.AutoExposure = false
Lighting.EnvironmentDiffuseScale = 0
Lighting.EnvironmentSpecularScale = 0
Lighting.OutdoorAmbient = Color3.new(0, 0, 0)
Lighting.Brightness = 5

local ground = Instance.new("Part")
ground.Size = vector.create(300, 0.2, 300)
ground.CFrame = CFrame.new(0, -0.1, 0)
ground.Anchored = true
ground.Parent = Workspace

local building = Instance.new("Part")
building.Size = vector.create(20, 40, 40)
building.CFrame = CFrame.new(-120, 20, 0)
building.Anchored = true
building.CastShadow = casts
building.Parent = Workspace

local camera = Instance.new("Camera")
camera.NearPlane = 1
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(vector.create(offset, 5, 6), vector.zero)
camera.Parent = Workspace
Workspace.CurrentCamera = camera
