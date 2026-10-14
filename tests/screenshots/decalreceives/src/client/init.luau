--!strict
--[[
	`BasePart.ReceivesDecals`: a red mark six metres square on a grey floor,
	seen from straight above, and three boxes of the floor's own grey:

	  left    inside the mark, receiving -- painted, as the floor is.
	  middle  inside the mark, `ReceivesDecals` off -- left as it is.
	  right   outside the mark -- what a box looks like with no decal on it.

	The mark is laid down at tick 20. `run_decal_receives_gate.cmake` takes a
	picture before it and one after, and asks that the middle box is the same
	pixels in both while the left one and the floor round them are darker.

	An orthographic camera twenty metres wide over a 1000 by 500 window: a
	thing's middle is at x = (metres + 10) / 20.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 12

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.lookAt(vector.create(0, 30, 0), vector.create(0, 0, 0), vector.create(0, 0, -1))

local grey = Color3.fromRGB(110, 110, 110)

local floor = Instance.new("Part")
floor.Size = vector.create(60, 1, 60)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", grey)
floor.Anchored = true
floor.Parent = workspace

local function box(x: number, receives: boolean)
    local part = Instance.new("Part")
    part.Size = vector.create(1.6, 1, 1.6)
    part.CFrame = CFrame.new(x, 0.5, 0)
    part:SetMaterialParameter("Color", grey)
    part.Anchored = true
    part.ReceivesDecals = receives
    part.Parent = workspace
end

box(-1.8, true)
box(1.8, false)
box(7, true)

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks == 20 then
        local decal = Instance.new("Decal")
        -- Looking down, and deep enough to take in the boxes' tops.
        decal.CFrame = CFrame.lookAt(vector.create(0, 0.5, 0), vector.create(0, 1.5, 0))
        decal.Size = vector.create(6, 6, 3)
        decal.Color = Color3.fromRGB(255, 40, 30)
        decal.Parent = workspace
    end
end)
