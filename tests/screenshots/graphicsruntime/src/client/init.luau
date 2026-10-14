--!strict
--[[
	The scene for `graphics_runtime_differential` (ADR 0147): a floor, a row of
	blocks that cast on it and a few that glow, still -- and a script that asks
	for the `Low` quality level half a second in.

	The driver draws it three ways and compares the pictures:

	- with no flag: the level changes while the game runs, by this script;
	- with `--quality=low`: the level was `Low` from the start, and the write
	  below changes nothing;
	- with `--quality=ultra`: the command line pinned the level, and the write
	  below is not in force.

	The first two must be the same picture and the third must not be: what a
	script changes at run time is what starting that way would have drawn, and
	a flag outranks a script.
]]

local Graphics = game:GetService("GraphicsService")
local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 15

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Size = Vector3.new(60, 1, 60)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(120, 124, 130))
floor.Anchored = true
floor.Parent = workspace

for index = 1, 7 do
    local block = Instance.new("Part")
    block.Name = `Block{index}`
    -- Thin and tall, so the shadow map's resolution shows in what they cast.
    block.Size = Vector3.new(0.4, 2 + index * 0.6, 0.4)
    block.CFrame = CFrame.new((index - 4) * 2.2, block.Size.y / 2, -2 - index * 1.5)
    block:SetMaterialParameter("Color", Color3.fromRGB(200, 90, 60))
    block.Anchored = true
    block.Parent = workspace
end

-- Three that glow: bloom is on at every level but the lowest.
for index = 1, 3 do
    local lamp = Instance.new("Part")
    lamp.Name = `Lamp{index}`
    lamp.Size = Vector3.new(1, 1, 1)
    lamp.CFrame = CFrame.new((index - 2) * 4, 1.5, 2)
    lamp:SetMaterialParameter("Color", Color3.fromRGB(255, 240, 200))
    lamp:SetMaterialParameter("Emissive", Color3.new(12, 10, 7))
    lamp.Anchored = true
    lamp.Parent = workspace
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(Vector3.new(0, 6, 14), Vector3.new(0, 1.5, -4))
camera.Parent = workspace
workspace.CurrentCamera = camera

task.wait(0.5)
Graphics.QualityLevel = Enum.GraphicsQuality.Low
