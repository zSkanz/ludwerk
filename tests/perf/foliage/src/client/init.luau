--!strict
--[[
	The foliage benchmark (ADR 0116): a dense field of grass to the horizon in a
	gusting wind, seen from a standing height -- the case the GPU cull exists
	for, and the one `docs/perf-baselines.md` records a budget for.

	A 1,024 m square of flat ground, ten instances a square metre drawn to
	250 m: some two million instances resident around the camera, every one of
	them tested by the cull every frame. The camera turns slowly on the spot, so
	the cull sees a different quarter of the field each second.
]]

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local terrain = Instance.new("Terrain")
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain.Parent = Workspace
terrain:FillBlock(vector.create(0, -40, 0), vector.create(1024, 80, 1024), 1)

local field = Instance.new("FoliageLayer")
field.Density = 10
field.MinSpacing = 0
field.DrawDistance = 250
field.FadeDistance = 40
field.Parent = terrain

local grass = Instance.new("FoliageMesh")
grass.Mesh = "asset://models/grass.gltf"
grass.Parent = field

Workspace.GlobalWind = vector.create(7, 0, 3)
Workspace.WindGusts = 0.7
Workspace.WindTurbulence = 0.4

local camera = Instance.new("Camera")
camera.Parent = Workspace
Workspace.CurrentCamera = camera

RunService.Heartbeat:Connect(function()
    local angle = RunService.SimTime * 0.1
    local eye = vector.create(0, 1.7, 0)
    camera.CFrame = CFrame.lookAt(eye, eye + vector.create(math.cos(angle), -0.08, math.sin(angle)))
end)
