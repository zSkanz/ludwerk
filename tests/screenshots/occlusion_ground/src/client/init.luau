--!strict

-- **What the ground hides is not drawn** (ADR 0262): a ridge of terrain on
-- one side and a wall of blocks on the other, each with a crowd of small
-- things behind it, and some tall enough to stand over its line. The camera
-- turns a full circle every two seconds and rises and falls as it does, so the
-- line of sight grazes the top of each. Drawn once with the occlusion culler
-- and once without, every frame must be the same picture.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local voxels = game:GetService("VoxelService")

Lighting.ClockTime = 14
Lighting.GlobalShadows = true

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({ "engine://terrain/grass", "engine://terrain/rock" })
-- Flat ground under everything, its top at nought.
terrain:FillBlock(vector.create(0, -12, 0), vector.create(320, 24, 320), 1)
-- A ridge across the north, fourteen metres high, rounded by hills along it.
terrain:FillBlock(vector.create(0, 6, -50), vector.create(220, 12, 24), 2)
for index = -4, 4 do
    terrain:FillBall(vector.create(index * 24, 10, -50), 10, 2)
end
terrain.Parent = workspace

-- A wall of blocks across the south, ten high, with battlements.
local Stone = voxels:RegisterBlock("Stone", Color3.fromRGB(140, 130, 120))
voxels:FillBlocks(vector.create(-80, 0, 40), vector.create(80, 10, 46), Stone)
for index = -10, 10, 2 do
    voxels:FillBlocks(vector.create(index * 8, 10, 40), vector.create(index * 8 + 4, 13, 46), Stone)
end

-- Behind each, things on the ground -- hidden from the camera but where it
-- rises over the line -- and a few towers that stand over it.
local things = Instance.new("Folder")
things.Name = "Things"
things.Parent = workspace
for index = 0, 299 do
    local thing = Instance.new("Part")
    thing.Shape = index % 2 == 0 and Enum.PartShape.Ball or Enum.PartShape.Block
    thing.Anchored = true
    thing.Size = vector.create(1.5, 2, 1.5)
    local x = ((index * 37) % 200) - 100
    local depth = 70 + (index * 23) % 60
    local z = index % 2 == 0 and -depth or depth
    thing.CFrame = CFrame.new(x + 0.3, 1, z + 0.7)
    thing:SetMaterialParameter("Color", Color3.fromHSV((index % 12) / 12, 0.7, 0.9))
    thing.Parent = things
end
for index = -3, 3 do
    for _, side in { -1, 1 } do
        local tower = Instance.new("Part")
        tower.Anchored = true
        tower.Size = vector.create(3, 30, 3)
        tower.CFrame = CFrame.new(index * 25 + 7, 15, side * 90)
        tower:SetMaterialParameter("Color", Color3.fromRGB(200, 200, 210))
        tower.Parent = things
    end
end

local camera = Instance.new("Camera")
camera.FieldOfView = 70
camera.NearPlane = 0.1
camera.FarPlane = 500
camera.Parent = workspace
workspace.CurrentCamera = camera

local frame = 0
RunService.PreRender:Connect(function()
    frame += 1
    local angle = frame * (math.pi * 2 / 120)
    local eye = vector.create(0, 3 + 4 * math.sin(frame * 0.21), 0)
    camera.CFrame = CFrame.lookAt(eye, eye + vector.create(math.cos(angle), 0.03, math.sin(angle)))
end)
