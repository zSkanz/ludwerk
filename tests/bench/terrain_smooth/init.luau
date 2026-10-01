--!strict
--- What the smooth brush costs, a stamp a tick (the owner's editing lag,
--- 2026-10-01).
---
--- **The brush the owner felt and no bench held.** `terrain_sculpt` stamps
--- fills, digs and paint; nobody had timed a smooth, and one stamp at radius 8
--- on rolling ground cost 82 ms -- read through a margin four times the brush,
--- every voxel by a clamped lookup. A held smooth in the editor was a frame of
--- half a second, and grew.
---
--- Rolling hills, so every stamp has surfaces to move, and the brush the
--- editor's default and twice it. The 16 ms budget is a catastrophe detector
--- like every scene here; `docs/perf-baselines.md` carries the number.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local terrain = Instance.new("Terrain")
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain.Parent = Workspace

-- 128 m of hills, from heights.
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
    -- A stroke that wanders, so the cost is not one spot's settled ground.
    local angle = tick * 0.07
    local reach = 20 + 14 * math.sin(tick * 0.031)
    local radius = if tick % 4 == 0 then 16 else 8
    terrain:SmoothBall(vector.create(math.cos(angle) * reach, 10, math.sin(angle) * reach), radius, 0.5)
end)
