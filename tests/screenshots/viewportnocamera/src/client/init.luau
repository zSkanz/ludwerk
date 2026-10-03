--!strict

-- **A ViewportFrame with its own camera, in a world with none** (G7). The
-- views were drawn only when the world had a camera to look through, so a
-- menu scene -- an empty world and a screen of item previews -- showed every
-- preview empty. Nothing here sets `Workspace.CurrentCamera`.

local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService
local frame = Instance.new("ViewportFrame")
frame.Size = UDim2.new(0.5, 0, 0.5, 0)
frame.Position = UDim2.new(0.25, 0, 0.25, 0)
frame.BackgroundTransparency = 1
frame.Parent = screen

local crate = Instance.new("Part")
crate.Anchored = true
crate.Size = vector.create(2, 2, 2)
crate:SetMaterialParameter("Color", Color3.new(0.9, 0.1, 0.1))
crate.Parent = frame

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 0, 4), vector.zero)
camera.Parent = frame
frame.CurrentCamera = camera
