--!strict

-- A map: two walls that wear this map's own materials, and its own model.
for index = 0, 1 do
    local wall = Instance.new("Part")
    wall.Anchored = true
    wall.Size = vector.create(3, 3, 0.5)
    wall.CFrame = CFrame.new(index * 4 - 2, 1.5, 0)
    wall.Material = Material.load(`asset://materials/b_{index}.material.json` :: any)
    wall.Parent = workspace
end

local model = Instance.new("MeshPart")
model.MeshContent = "asset://models/b.gltf" :: Content
model.Anchored = true
model.CFrame = CFrame.new(0, 4, -2)
model.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 3, 10), vector.create(0, 1, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
