--!strict
--[[
	The scene for `anti_aliasing_flicker` (ADR 0158): what crawls under a moving
	camera -- a fence of thin wires and poles, and a far row of thinner ones --
	panned slowly sideways, a fraction of a pixel a frame.

	The driver draws it once for each anti-aliasing mode, a picture a frame,
	and measures how much the frames pop rather than change steadily
	(`imgflicker`). The temporal pass must flicker least.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 14

local function bar(name: string, size: vector, at: vector, colour: Color3)
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.CFrame = CFrame.new(at)
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
end

bar("Floor", vector.create(200, 1, 200), vector.create(0, -0.5, 0), Color3.fromRGB(70, 90, 60))

-- The fence: wires two centimetres thick, a hand apart, and posts.
local wire = Color3.fromRGB(230, 230, 235)
for index = 0, 24 do
    bar(`Wire{index}`, vector.create(30, 0.02, 0.02), vector.create(0, 0.3 + index * 0.12, -6), wire)
end
for index = -7, 7 do
    bar(`Post{index}`, vector.create(0.04, 3.4, 0.04), vector.create(index * 2, 1.7, -6), wire)
end
-- And far, thinner still: poles a pixel or less across.
for index = -30, 30 do
    bar(`Pole{index}`, vector.create(0.06, 6, 0.06), vector.create(index * 1.3, 3, -40), Color3.fromRGB(40, 40, 45))
end

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.Parent = workspace
workspace.CurrentCamera = camera

-- About two fifths of a pixel a frame at the fence, steady -- a slow walk
-- sideways: the case a single frame's anti-aliasing cannot fix.
local frame = 0
local function place()
    local x = -1 + frame * 0.01
    camera.CFrame = CFrame.lookAt(vector.create(x, 1.6, 2), vector.create(x, 1.6, -6))
end
place()
-- On the tick: headless, a frame is a tick, and a render step needs a window.
RunService.Heartbeat:Connect(function()
    frame += 1
    place()
end)
