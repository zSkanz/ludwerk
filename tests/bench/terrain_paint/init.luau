--!strict
--- What a wide paint brush costs, a stamp a tick (the owner's editing lag,
--- 2026-10-01).
---
--- A blend at radius 24 over rolling ground walked its brush's whole box and
--- found each voxel's chunk by a search: 3.4 ms a stamp. The 16 ms budget is
--- a catastrophe detector; `docs/perf-baselines.md` carries the number.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local terrain = Instance.new("Terrain")
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({
    "asset://materials/terrain/grass.material.json",
    "asset://materials/terrain/sand.material.json",
    "asset://materials/terrain/rock.material.json",
})
terrain.Parent = Workspace

local Size = 128
local heights = table.create(Size * Size, 0)
for z = 0, Size - 1 do
    for x = 0, Size - 1 do
        heights[z * Size + x + 1] = 8 + 6 * math.sin(x / 9) * math.cos(z / 7) + 3 * math.sin((x + z) / 5)
    end
end
terrain:WriteHeights(vector.create(-Size / 2, 0, -Size / 2), Size, heights, 1)

local tick = 0
RunService.Heartbeat:Connect(function()
    tick += 1
    local angle = tick * 0.07
    local reach = 20 + 14 * math.sin(tick * 0.031)
    local material = 2 + tick % 2
    terrain:PaintBall(vector.create(math.cos(angle) * reach, 10, math.sin(angle) * reach), 24, material, {
        Mode = Enum.TerrainPaintMode.Blend,
        Strength = 0.5,
        Falloff = 0.5,
    })
end)
