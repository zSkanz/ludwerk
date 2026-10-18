--!strict
-- A `ScreenGui` in a headset is a panel in front of the player (ADR 0246 §1):
-- laid out against the panel's canvas, not the window's, and drawn in each
-- eye. Run under the simulated headset with a GPU and `--screenshot`.
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(40, 1, 40)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

-- The camera is where the play space is: the floor under the player.
local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 0)
camera.Parent = workspace
workspace.CurrentCamera = camera

local screen = Instance.new("ScreenGui")
-- 1000 tall makes the panel's canvas 1777.8 by 1000 at 1000 pixels a metre:
-- 1.78 metres across, 1.5 metres ahead.
screen.ReferenceHeight = 1000
screen.Parent = UIService

local label = Instance.new("TextLabel")
label.Size = UDim2.fromScale(0.5, 0.3)
label.Position = UDim2.fromScale(0.25, 0.35)
label.BackgroundColor = Color3.fromRGB(200, 40, 40)
label.TextColor = Color3.fromRGB(255, 255, 255)
label.TextSize = 90
label.Text = "PANEL"
label.Parent = screen

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 15 then
        print(`[panel] size={math.round(label.AbsoluteSize.X)}x{math.round(label.AbsoluteSize.Y)}`)
    end
end)
