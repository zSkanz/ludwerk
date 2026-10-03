--!strict
--[[
	Pixel art among 3D surfaces (ADR 0158): the sprites follow the world's
	picture -- its scale, its upscale, its temporal pass -- but a sprite drawn
	in its own colours is never jittered, blended with the frames before it,
	filtered or sharpened. Scaled up, it is scaled by the nearest texel.

	`run_aa_2d_gate.cmake` draws this scene at half scale -- through FSR 1 and
	TAA, through TAA alone, and through FSR 1 and SMAA -- and asks that every
	pixel of the checkerboard is one of its two colours. A filter makes colours
	between them at every one of its edges.

	An orthographic camera ten metres tall over a 640 by 360 window: at half
	scale the world is 180 pixels tall, eighteen to a metre, and each square of
	the board is one of them. A square is drawn a little larger than its step
	and the next one over it, so no crack between two can show what is behind.
	Behind the board, a slab and posts: 3D surfaces, so the frame is not a
	picture of sprites alone.
]]

local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 14

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.new(0, 0, 30)

local function block(size: vector, at: vector, colour: Color3)
    local part = Instance.new("Part")
    part.Size = size
    part.CFrame = CFrame.new(at)
    part:SetMaterialParameter("Color", colour)
    part.Anchored = true
    part.Parent = workspace
end

block(vector.create(40, 20, 1), vector.create(0, 0, -3), Color3.fromRGB(70, 90, 60))
for index = -12, 12 do
    block(vector.create(0.04, 8, 0.5), vector.create(index * 0.7 + 0.13, 0, -2), Color3.fromRGB(230, 230, 235))
end

-- The board: sixteen by sixteen squares of one render pixel, its corner at
-- (-4, -0.5), on a pixel boundary of the half-scale picture.
local Step = 1 / 18
local Light = Color3.fromRGB(200, 80, 40)
local Dark = Color3.fromRGB(10, 200, 250)
for row = 0, 15 do
    for column = 0, 15 do
        local square = Instance.new("Part2D")
        square.Size = Vector2.new(Step * 1.25, Step * 1.25)
        square.Position = Vector2.new(-4 + (column + 0.5) * Step, -0.5 + (row + 0.5) * Step)
        square.Color = if (row + column) % 2 == 0 then Light else Dark
        square.ZIndex = row * 16 + column
        square.Anchored = true
        square.CanCollide = false
        square.Parent = workspace
    end
end
