--!strict
--[[
	A view drawn into a texture is UI (D528): a `ViewportFrame`'s picture is
	drawn at the frame's own pixel size whatever the world's render scale says.
	`run_aa_2d_gate.cmake` draws this scene at half scale through FSR 1 and at
	full scale, SMAA both times, and the frame must be the same pixels while
	the world behind it is not.

	A world of 3D surfaces behind, and an opaque frame at 200, 60 to 440, 300
	of a 640 by 360 window with a turned cube and a thin bar in it -- edges a
	reduced scale shows as steps.
]]

local Lighting = game:GetService("Lighting")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 14

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 4, 12), vector.create(0, 1, 0))

local function block(parent: Instance, size: vector, at: CFrame, colour: Color3)
    local part = Instance.new("Part")
    part.Size = size
    part.CFrame = at
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = parent
end

block(workspace, vector.create(60, 1, 60), CFrame.new(0, -0.5, 0), Color3.fromRGB(70, 90, 60))
for index = -10, 10 do
    block(workspace, vector.create(0.05, 3, 0.05), CFrame.new(index * 0.6, 1.5, -2), Color3.fromRGB(230, 230, 235))
end

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local frame = Instance.new("ViewportFrame")
frame.Position = UDim2.fromOffset(200, 60)
frame.Size = UDim2.fromOffset(240, 240)
frame.BackgroundColor = Color3.fromRGB(20, 24, 36)
frame.BackgroundTransparency = 0
frame.Parent = screen

block(frame, vector.create(2, 2, 2), CFrame.fromEuler(0.4, 0.7, 0.2), Color3.fromRGB(230, 200, 40))
block(
    frame,
    vector.create(3, 0.08, 0.08),
    CFrame.new(0, -1.6, 0) * CFrame.fromEuler(0, 0, 0.3),
    Color3.fromRGB(240, 240, 240)
)
