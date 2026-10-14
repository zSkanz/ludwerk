--!strict

-- The lobby: a floor and a camera, and nothing a map has.
local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(12, 1, 12)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 3, 10), vector.create(0, 1, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
