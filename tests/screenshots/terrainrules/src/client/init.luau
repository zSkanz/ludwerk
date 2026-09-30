--!strict

-- **The CPU's rules are the shader's** (terrain audit T3; ADR 0113 §2 promised
-- it): hills steep enough for the slope rock and high enough for snow, and a
-- patch painted with sand, photographed from above as the layer each pixel is
-- drawn as (`--debug-view=material`).
--
-- Run twice by `run_terrain_rules.cmake`: once drawn by the rules, and once
-- after `ApplyRules` has written what the CPU says they draw into the voxels
-- and the rules are off. The two pictures agree but for the edges, where a
-- voxel and a pixel sample the slope a little apart.

local Lighting = game:GetService("Lighting")
local Applied = require("@shared/applied")

local Grass = 1
local Sand = 2
local Rock = 3
local Snow = 4

local terrain = Instance.new("Terrain")
terrain.Name = "Hills"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({
    "engine://terrain/grass",
    "engine://terrain/sand",
    "engine://terrain/rock",
    "engine://terrain/snow",
})
terrain.Parent = workspace

-- Rounded hills from 20 to 45 degrees at their steepest, and one tall enough
-- for snow on its top.
local Size = 128
local Hills = {
    { x = -30, z = -20, height = 18, reach = 30 },
    { x = 28, z = 10, height = 10, reach = 28 },
    { x = -5, z = 35, height = 12, reach = 18 },
}
local heights = table.create(Size * Size, 0)
for z = 0, Size - 1 do
    for x = 0, Size - 1 do
        local wx, wz = x - Size / 2 + 0.5, z - Size / 2 + 0.5
        local height = 2
        for _, hill in Hills do
            local r = math.sqrt((wx - hill.x) ^ 2 + (wz - hill.z) ^ 2)
            if r < hill.reach then
                height += hill.height * (1 + math.cos(math.pi * r / hill.reach)) / 2
            end
        end
        heights[z * Size + x + 1] = height
    end
end
terrain:WriteHeights(vector.create(-Size / 2, 0, -Size / 2), Size, heights, Grass)
terrain:PaintBall(vector.create(20, 4, -35), 10, Sand)

terrain:SetRules({
    { Material = Rock, SlopeMin = 28, Blend = 6, Noise = 0.12 },
    { Material = Snow, HeightMin = 16, Blend = 2, Noise = 0.3 },
})
if Applied then
    terrain:ApplyRules(vector.create(-70, -8, -70), vector.create(70, 40, 70))
    terrain:SetRules({})
end

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(0, 120, 30), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.FogStart = 100000
Lighting.FogEnd = 200000
