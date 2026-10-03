--!strict
--[[
	A picture of sprites alone is drawn at the window's resolution whatever the
	render scale says, upscales nothing and takes no temporal pass (ADR 0158):
	`run_aa_2d_gate.cmake` draws this scene at half scale through FSR 1 and
	TAA, and at full scale through SMAA, and the two must be the same frame.

	Sprites of every kind the 2D layer draws, none of them on a 3D surface:
	lit ones (`ExactColor` off), which the exposure and the tone curve move,
	turned ones, round ones with anti-aliased edges and exact ones -- under an
	orthographic camera ten metres tall.
]]

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.new(0, 0, 30)

local function sprite(x: number, y: number, size: number, color: Color3): Part2D
    local created = Instance.new("Part2D")
    created.Position = Vector2.new(x, y)
    created.Size = Vector2.new(size, size)
    created.Color = color
    created.Anchored = true
    created.CanCollide = false
    created.Parent = workspace
    return created
end

for index = 0, 7 do
    local lit = sprite(-8 + index * 2.2, 2.5, 1.5, Color3.fromRGB(40 + index * 25, 200 - index * 20, 90))
    lit.ExactColor = false
    lit.Rotation = index * 11
end
for index = 0, 7 do
    local round = sprite(-8 + index * 2.2, 0, 1.3 + index * 0.05, Color3.fromRGB(230, 120 + index * 15, 40))
    round.Shape = Enum.Shape2D.Circle
    round.ExactColor = index % 2 == 0
end
for index = 0, 7 do
    sprite(-8 + index * 2.2, -2.5, 1.2, Color3.fromRGB(10, 60 + index * 20, 250))
end
