--!strict

-- **Ground wider than the camera sees** (D403): a flat slab 12 km across,
-- and a camera over its middle that sees 5 km. The ground was drawn to
-- 4 096 m from the camera and no further, and by whole nodes: towards the
-- sides of the picture, where a point inside the far plane is further from
-- the camera than one at its middle, the ground was missing in scallops.
--
-- Flat ground under a pitched camera ends, at the far plane, on one row of
-- the picture: with the camera 400 m up, pitched 10 degrees down, seeing
-- 5 000 m through 50 degrees, the row two fifths of the way down
-- (0.398). The driver probes a row under it for ground and a row over it
-- for sky, from one side of the picture to the other.

local terrain = Instance.new("Terrain")
terrain.Name = "Wide"
terrain.VoxelSize = 2
terrain.MinHeight = -16
terrain.MaxHeight = 48
terrain.Parent = workspace
-- In strips, each inside what one edit may write.
for strip = -3, 2 do
    terrain:FillBlock(vector.create(0, -2, strip * 2000 + 1000), vector.create(12000, 4, 2000), 1)
end

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.FarPlane = 5000
local pitch = math.rad(10)
local eye = vector.create(0, 400, 0)
camera.CFrame = CFrame.lookAt(eye, eye + vector.create(0, -math.sin(pitch), -math.cos(pitch)))
camera.Parent = workspace
workspace.CurrentCamera = camera
