--!strict
-- The world inside `subworld_gate`'s two sub-worlds (ADR 0107 §3): a camera
-- facing a wall that fills its picture, green until the host says "blue".

local SceneService = game:GetService("SceneService")

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 10)
camera.Parent = workspace
workspace.CurrentCamera = camera

local wall = Instance.new("Part")
wall.Anchored = true
wall.Size = vector.create(60, 60, 1)
wall.CFrame = CFrame.new(0, 0, 0)
wall:SetMaterialParameter("Color", Color3.fromRGB(0, 200, 0))
wall.Parent = workspace

SceneService.HostMessageReceived:Connect(function(color: string)
    if color == "blue" then
        wall:SetMaterialParameter("Color", Color3.fromRGB(0, 0, 220))
    end
end)
