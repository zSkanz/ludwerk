--!strict

-- **A streamed terrain drawn whole** (ADR 0144): ground three kilometres
-- across -- some two thousand 64 m cells -- with a 2 m slab, a hill and a ball
-- on it, photographed from a kilometre and from two.
--
-- Run first with no scene, the script builds the ground and the field is the
-- whole of it: the picture of everything resident. The driver saves that
-- world as the project's scene; run again, the scene's ground is cut into
-- cells and streamed round the camera, a 300 m radius, and the script builds
-- nothing. What is past the radius is drawn from the cells on disk, and the
-- two runs' outlines of the ground against the sky must be one.

local RunService = game:GetService("RunService")
local StreamingService = game:GetService("StreamingService")

local FramesPerCase = 120

type Case = { distance: number, bearing: number }
local Cases: { Case } = {
    { distance = 1000, bearing = 35 },
    { distance = 2000, bearing = 35 },
    { distance = 1000, bearing = 200 },
}

if workspace:FindFirstChildOfClass("Terrain") == nil then
    local terrain = Instance.new("Terrain")
    terrain.Name = "Wide"
    terrain.MinHeight = -32
    terrain.MaxHeight = 96
    terrain.Parent = workspace
    -- A slab 2 m thick, floating, as the owner's place is.
    terrain:FillBlock(vector.create(0, 1, 0), vector.create(3000, 2, 3000), 1)
    terrain:FillBall(vector.create(-600, -20, 500), 90, 1)
    terrain:FillBall(vector.create(700, 10, -400), 40, 1)
    terrain:FillBlock(vector.create(300, 4, 900), vector.create(500, 2, 80), 1)
end

StreamingService.TerrainLoadRadius = 300
StreamingService.TerrainMinRadius = 200

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.FarPlane = 10000
camera.Parent = workspace
workspace.CurrentCamera = camera

local current = 0
local function applyCase()
    local frame = math.floor(RunService.SimTime * 60 + 0.5)
    local index = math.clamp(frame // FramesPerCase + 1, 1, #Cases)
    if index == current then
        return
    end
    current = index
    local case = Cases[index]
    local turn = math.rad(case.bearing)
    local pitch = math.rad(28)
    local across = case.distance * math.cos(pitch)
    local eye = vector.create(math.sin(turn) * across, case.distance * math.sin(pitch), math.cos(turn) * across)
    camera.CFrame = CFrame.lookAt(eye, vector.create(0, 0, 0))
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
