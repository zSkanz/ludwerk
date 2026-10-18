--!strict
-- The room seen through a headset (ADR 0247 §1): with `XRService.Passthrough`
-- on, the eyes' pictures are clear where the game draws nothing -- the sky --
-- and opaque where it draws the floor. Run under the simulated headset with a
-- GPU and `--screenshot`; `imgprobe` reads the alpha of the left eye.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(40, 1, 40)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 0)
camera.Parent = workspace
workspace.CurrentCamera = camera

XRService.Passthrough = true

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 10 then
        print(`[passthrough] supported={XRService:SupportsPassthrough()} on={XRService.Passthrough}`)
    end
end)
