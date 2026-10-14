--!strict

-- Full-quality terrain permutations must bind the complete material layout.
local terrain = Instance.new("Terrain")
terrain.MinHeight = -16
terrain.MaxHeight = 16
terrain:SetLayers({ "engine://terrain/grass" })
terrain.Parent = workspace
terrain:FillBlock(vector.create(0, -2, 0), vector.create(32, 4, 32), 1)

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(8, 12, 8), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
