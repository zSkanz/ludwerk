--!strict

-- **The mesh flight** (ADR 0264): a camera flying the length of a strip of
-- cells, each wearing a mesh no other cell wears, with the streaming focus
-- on it. What is behind it leaves; what it carried must leave the card too.

local RunService = game:GetService("RunService")
local StreamingService = game:GetService("StreamingService")

local camera = Instance.new("Camera")
camera.Name = "FlyCam"
camera.FieldOfView = 70
camera.Parent = workspace
workspace.CurrentCamera = camera

StreamingService.LoadRadius = 600
StreamingService.MinRadius = 300
StreamingService:AddFocus(camera)

-- Along the strip, at two hundred metres a second of simulated time: its
-- whole length, twelve kilometres, in a minute. On the simulation's clock,
-- which a headless run has.
local Speed = 200
RunService.Heartbeat:Connect(function()
    local x = RunService.SimTime * Speed
    local position = vector.create(x, 40, 120)
    camera.CFrame = CFrame.lookAt(position, position + vector.create(1, -0.3, 0))
end)
