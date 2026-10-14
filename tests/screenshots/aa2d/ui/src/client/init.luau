--!strict
--[[
	The screen's UI is drawn after the world's picture is resolved and scaled
	(ADR 0158), at the window's own resolution: `run_aa_2d_gate.cmake` draws
	this scene at half scale through FSR 1 and the temporal pass, and at full
	scale with no anti-aliasing at all, and the label must be the same pixels
	in both while the world behind it is not.

	A world of 3D surfaces -- a floor, a row of thin posts, a turning block --
	so the scale, the upscale and the jitter all reach it, and one opaque label
	at 40, 40 to 340, 120 of a 640 by 360 window.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 14

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 4, 12), vector.create(0, 1, 0))

local function block(size: vector, at: vector, colour: Color3): Part
    local part = Instance.new("Part")
    part.Size = size
    part.CFrame = CFrame.new(at)
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
    return part
end

block(vector.create(60, 1, 60), vector.create(0, -0.5, 0), Color3.fromRGB(70, 90, 60))
for index = -10, 10 do
    block(vector.create(0.05, 3, 0.05), vector.create(index * 0.6, 1.5, -2), Color3.fromRGB(230, 230, 235))
end
local turning = block(vector.create(2, 2, 2), vector.create(0, 1, 2), Color3.fromRGB(200, 80, 40))

-- Turned by the frame, not the clock, so both runs draw the same frames.
local frame = 0
RunService.Heartbeat:Connect(function()
    frame += 1
    turning.CFrame = CFrame.new(vector.create(0, 1, 2)) * CFrame.fromEuler(0, frame * 0.05, 0)
end)

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local label = Instance.new("TextLabel")
label.Text = "Ludwerk 0158"
label.TextSize = 40
label.TextColor = Color3.fromRGB(255, 255, 255)
label.BackgroundColor = Color3.fromRGB(20, 24, 36)
label.Position = UDim2.fromOffset(40, 40)
label.Size = UDim2.fromOffset(300, 80)
label.Parent = screen
