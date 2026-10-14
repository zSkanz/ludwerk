--!strict
--[[
	The scene for `temporal_upscale_gate` (ADR 0164): what an upscaler has to
	put back -- edges at every angle, bars a pixel or two across at half the
	resolution, and a row of steps -- under a camera that does not move.

	The driver draws it at the output's resolution and at half of it, upscaled
	each way, and measures how far each is from the first (`imgprobe`). The
	temporal upscaler, which has every frame before this one to build from,
	must come nearest.
]]

local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 14

local function bar(name: string, size: vector, at: CFrame, colour: Color3)
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.CFrame = at
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
end

bar("Floor", vector.create(200, 1, 200), CFrame.new(0, -0.5, 0), Color3.fromRGB(70, 90, 60))
bar("Wall", vector.create(40, 12, 1), CFrame.new(0, 6, -14), Color3.fromRGB(60, 62, 70))

-- A fan of bars, an edge at every angle from level to upright.
local pale = Color3.fromRGB(230, 230, 235)
for index = 0, 17 do
    local turn = CFrame.fromEuler(0, 0, math.rad(index * 10))
    bar(`Fan{index}`, vector.create(5, 0.06, 0.06), CFrame.new(-4, 3.2, -8) * turn, pale)
end
-- Bars side by side, each as wide as the gap beside it, narrower and narrower.
local at = 1.0
for index = 0, 23 do
    local width = 0.16 - index * 0.005
    bar(`Comb{index}`, vector.create(width, 2.4, 0.05), CFrame.new(at, 3.6, -8), pale)
    at += width * 2
end
-- Steps: boxes turned a little, a short edge after a short edge.
for index = 0, 11 do
    local turn = CFrame.fromEuler(0, math.rad(index * 7), math.rad(index * 4))
    bar(
        `Step{index}`,
        vector.create(0.5, 0.5, 0.5),
        CFrame.new(-5 + index * 0.9, 0.9, -6) * turn,
        Color3.fromRGB(200, 120 + index * 8, 60)
    )
end

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(vector.create(0, 2.4, 2), vector.create(0, 2.4, -8))
camera.Parent = workspace
workspace.CurrentCamera = camera
