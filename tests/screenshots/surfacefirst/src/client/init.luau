--!strict
-- A floor and a camera, and after four seconds of play five blocks wearing a
-- material whose surface shader nothing has drawn yet.
local RunService = game:GetService("RunService")

local look = Material.load("asset://materials/look.material.json")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(24, 1, 24)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(0, 6, 12), vector.create(0, 1, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

local elapsed = 0
local shown = false
RunService.Heartbeat:Connect(function(dt: number)
    elapsed += dt
    if shown or elapsed < 4 then
        return
    end
    shown = true
    print("MATERIAL ON")
    for index = 0, 4 do
        local block = Instance.new("Part")
        block.Anchored = true
        block.Size = vector.create(1.5, 1 + index * 0.5, 1.5)
        block.Position = vector.create(index * 2.5 - 5, (1 + index * 0.5) / 2, 0)
        block.Material = look
        block.Parent = workspace
    end
end)
