--!strict

-- **A ViewportFrame the frame it is first shown** (D575). Three of them, made
-- at tick 20 over a blue panel, each with a red crate and a camera of its own
-- -- a sheet of portraits opening in a menu. Each was a WHITE square for one
-- frame before its crate showed: the frame its picture was first asked for.
--
-- `run_viewport_first_gate.cmake` photographs the frames round that one and
-- asks that a slot is the panel's blue or the crate's red in every one of
-- them, and never white.

local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService
local panel = Instance.new("Frame")
panel.Size = UDim2.new(1, 0, 1, 0)
panel.BackgroundColor = Color3.new(0.1, 0.2, 0.5)
panel.Parent = screen

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks ~= 20 then
        return
    end
    for slot = 0, 2 do
        local frame = Instance.new("ViewportFrame")
        frame.Size = UDim2.new(0.25, 0, 0.5, 0)
        frame.Position = UDim2.new(0.05 + slot * 0.3, 0, 0.25, 0)
        frame.BackgroundTransparency = 1
        frame.Parent = panel
        local crate = Instance.new("Part")
        crate.Anchored = true
        crate.Size = vector.create(2, 2, 2)
        crate:SetMaterialParameter("Color", Color3.new(0.9, 0.1, 0.1))
        crate.Parent = frame
        local camera = Instance.new("Camera")
        camera.CFrame = CFrame.lookAt(vector.create(0, 0, 4), vector.zero)
        camera.Parent = frame
        frame.CurrentCamera = camera
    end
end)
