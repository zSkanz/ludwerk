--!strict
-- A `ScreenGui` in a headset is a panel in front of the player (ADR 0246 §1):
-- laid out against the panel's canvas, not the window's, and drawn in each
-- eye; and the right hand points at its button and pulls the trigger, then
-- puts the controller down and pokes the button with a fingertip (§2).
-- Run under the simulated headset with a GPU and `--screenshot`.
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")
local XRService = game:GetService("XRService")

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

local button = Instance.new("TextButton")
button.Size = UDim2.fromScale(0.5, 0.3)
button.Position = UDim2.fromScale(0.25, 0.35)
button.BackgroundColor = Color3.fromRGB(200, 40, 40)
button.TextColor = Color3.fromRGB(255, 255, 255)
button.TextSize = 90
button.Text = "PANEL"
button.Parent = screen

local activated = 0
button.Activated:Connect(function()
    activated += 1
end)

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 5 then
        -- The right hand held low and to the right, pointing at the
        -- panel's middle: 1.5 metres ahead at eye height.
        XRService:SimulatePose(
            Enum.XRPose.RightHand,
            CFrame.lookAt(vector.create(0.3, 1.3, -0.3), vector.create(0, 1.6, -1.5))
        )
    elseif ticks == 15 then
        print(`[panel] size={math.round(button.AbsoluteSize.X)}x{math.round(button.AbsoluteSize.Y)}`)
        local from, along = XRService:GetPointerRay(Enum.XRHand.Right)
        if from and along then
            print(`[panel] ray z={string.format("%.2f", from.z)} along={string.format("%.2f", along.z)}`)
        end
        XRService:SimulateInput(Enum.KeyCode.XRTriggerRight, 1)
    elseif ticks == 20 then
        XRService:SimulateInput(Enum.KeyCode.XRTriggerRight, 0)
    elseif ticks == 26 then
        print(`[panel] activated={activated}`)
        -- The controller put down, the right index finger five centimetres
        -- short of the button's face, pointing at it.
        XRService:SimulatePose(Enum.XRPose.RightIndexTip, CFrame.new(0, 1.6, -1.45))
    elseif ticks == 32 then
        -- Through the face by a centimetre: a press.
        XRService:SimulatePose(Enum.XRPose.RightIndexTip, CFrame.new(0, 1.6, -1.51))
    elseif ticks == 38 then
        -- And out again: the release, and the button activated.
        XRService:SimulatePose(Enum.XRPose.RightIndexTip, CFrame.new(0, 1.6, -1.45))
    elseif ticks == 44 then
        print(`[panel] poked={activated}`)
    end
end)
