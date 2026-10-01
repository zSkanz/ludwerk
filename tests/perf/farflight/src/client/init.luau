--!strict

-- **The far flight** (ADR 0150): corner to corner over the imported ground and
-- back, 200 m/s along each axis and 150 m up, the ground streamed at 512 m
-- and seen out to three kilometres. What it prices is the far ground: every
-- node past the load radius is built from the files the import made, and the
-- ground under the camera streams in and out the whole way.
--
-- The ground is whatever the scene holds -- `terrain_far_flight` lays four
-- kilometres of hills before it runs this. `Half` below is that world's; by
-- hand over a larger one, it is the number to change.

local RunService = game:GetService("RunService")
local StreamingService = game:GetService("StreamingService")
local Workspace = game:GetService("Workspace")

local Speed = 200
local Height = 150

StreamingService.TerrainLoadRadius = 512
StreamingService.TerrainMinRadius = 384

local camera = Instance.new("Camera")
camera.FarPlane = 3000
camera.Parent = Workspace
Workspace.CurrentCamera = camera

-- Half the world's width, less a margin from its edge.
local Half = 2048 - 300

local function place(t: number)
    -- Out and back, for ever: a place flown over is flown over again.
    local span = Half * 2
    local along = (t * Speed) % (span * 2)
    if along > span then
        along = span * 2 - along
    end
    local x = -Half + along
    local z = -Half + along
    camera.CFrame = CFrame.lookAt(vector.create(x, Height, z), vector.create(x + 400, 20, z + 400))
end
place(0)

RunService.Heartbeat:Connect(function()
    place(RunService.SimTime)
end)
