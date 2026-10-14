--!strict
--[[
	The scene for `frame_generation_gate` (ADR 0165): a camera sliding sideways
	past things at several depths, a box crossing in front of them and a bar
	turning -- motion the game knows, each pixel's, and a made frame has to put
	every one of them half way.

	The driver draws it twice. With frame generation, time moves two steps a
	frame, and between every two frames drawn one is made. Without, it moves
	one step a frame -- so the frame made between steps 2n and 2n + 2 has a
	frame DRAWN at step 2n + 1 to be held against, and the made one must be
	nearer to it than either of its neighbours is.
]]

local Graphics = game:GetService("GraphicsService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 14

local function bar(name: string, size: vector, at: CFrame, colour: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.CFrame = at
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
    return part
end

bar("Floor", vector.create(200, 1, 200), CFrame.new(0, -0.5, 0), Color3.fromRGB(70, 90, 60))
bar("Wall", vector.create(40, 12, 1), CFrame.new(0, 6, -14), Color3.fromRGB(60, 62, 70))

-- Things at three depths, so the camera's slide moves each a different way.
local pale = Color3.fromRGB(230, 230, 235)
for index = 0, 11 do
    bar(`Post{index}`, vector.create(0.3, 3, 0.3), CFrame.new(-8 + index * 1.5, 1.5, -10), pale)
end
for index = 0, 9 do
    local turn = CFrame.fromEuler(0, math.rad(index * 9), math.rad(index * 5))
    bar(
        `Step{index}`,
        vector.create(0.6, 0.6, 0.6),
        CFrame.new(-4.5 + index * 1.0, 0.9, -5) * turn,
        Color3.fromRGB(200, 120 + index * 10, 60)
    )
end

local mover = bar("Mover", vector.create(0.9, 0.9, 0.9), CFrame.new(-6, 2.2, -4), Color3.fromRGB(220, 60, 60))
local spinner = bar("Spinner", vector.create(3, 0.25, 0.25), CFrame.new(3, 2.6, -4), Color3.fromRGB(240, 220, 80))

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.Parent = workspace
workspace.CurrentCamera = camera

-- A step of time a frame, or two where a frame is made between.
local stride = if Graphics.FrameGeneration then 2 else 1
local step = 0
local function place()
    local x = -1.2 + step * 0.02
    camera.CFrame = CFrame.lookAt(vector.create(x, 2.4, 3), vector.create(x * 0.5, 2.2, -8))
    mover.CFrame = CFrame.new(-6 + step * 0.07, 2.2, -4)
    spinner.CFrame = CFrame.new(3, 2.6, -4) * CFrame.fromEuler(0, 0, step * 0.035)
end
place()
-- On the tick: headless, a frame is a tick, and a render step needs a window.
RunService.Heartbeat:Connect(function()
    step += stride
    place()
end)
