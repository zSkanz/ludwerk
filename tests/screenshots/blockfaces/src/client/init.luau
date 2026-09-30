--!strict

-- **Every face of a block in its block's colour** (D380): a pile of red blocks
-- seen from above and from two sides, the sun behind the camera. The palette
-- the vertex stage reads was a uniform block of 16 KiB, and SDL_GPU binds one
-- to Vulkan 4 KiB at a time: a block's top colour was in the first 4 KiB, its
-- sides, its underside, its images and the block size past them.

local Lighting = game:GetService("Lighting")
local voxels = game:GetService("VoxelService")

local Red = voxels:RegisterBlock("Red", Color3.new(0.8, 0.1, 0.1))
voxels:FillBlocks(vector.create(-4, 0, -4), vector.create(4, 6, 4), Red)

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(16, 14, 16), vector.create(0, 3, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 14
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
