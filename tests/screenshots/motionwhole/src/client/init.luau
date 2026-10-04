--!strict
--[[
	The scene for `motion_whole_gate` (D548): a still camera, a still wall, and
	a slab crossing in front of it a little further every tick -- seen from a
	side, so its face is at every depth along it.

	Drawn with `--debug-view=motion`, every pixel of the slab has moved and no
	pixel of the wall has. The slab's motion is drawn over the camera's own,
	against the depth the picture's depth pass wrote: where the two do not
	agree to the last bit about how far a pixel of that face is, the pixel
	keeps the wall's motion -- none -- and the slab's face has streaks of
	still pixels across it.
]]

local RunService = game:GetService("RunService")

local function bar(name: string, size: vector, at: CFrame, colour: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.CFrame = at
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
    return part
end

bar("Wall", vector.create(60, 30, 1), CFrame.new(0, 6, -14), Color3.fromRGB(60, 62, 70))
local slab = bar("Slab", vector.create(6, 3.5, 0.9), CFrame.new(-1.5, 2.2, -1), Color3.fromRGB(220, 60, 60))

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.Parent = workspace
workspace.CurrentCamera = camera

local step = 0
local function place()
    local x = -1.2 + step * 0.02
    camera.CFrame = CFrame.lookAt(vector.create(x, 2.4, 6), vector.create(x * 0.5, 2.2, -8))
    slab.CFrame = CFrame.new(-1.5 + step * 0.15, 2.2, -1)
end
place()
-- On the tick: headless, a frame is a tick.
RunService.Heartbeat:Connect(function()
    step += 1
    place()
end)
