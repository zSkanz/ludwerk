--!strict
--[[
	The probe scene for `UIGradient` and `UIStroke` (ADR 0110).

	Still, flat and at known pixels, so `run_ui_appearance_gate.cmake` can ask
	`imgprobe` what colour each place is: the claims are "the left end of the
	bar is red", "a stroke is outside an Outer box and inside an Inner one", "a
	mitred corner is filled where a round one is not" -- each a point, never a
	golden of the whole frame. Rendered at 1000 by 500.
]]

local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local Background = Color3.fromRGB(20, 20, 30)
local Yellow = Color3.fromRGB(255, 220, 0)

local backdrop = Instance.new("Frame")
backdrop.Size = UDim2.fromScale(1, 1)
backdrop.BackgroundColor = Background
backdrop.Parent = screen

local function box(x: number, y: number, width: number, height: number, color: Color3): Frame
    local frame = Instance.new("Frame")
    frame.Position = UDim2.fromOffset(x, y)
    frame.Size = UDim2.fromOffset(width, height)
    frame.BackgroundColor = color
    frame.Parent = backdrop
    return frame
end

local function gradient(parent: Instance, color: ColorSequence): UIGradient
    local g = Instance.new("UIGradient")
    g.Color = color
    g.Parent = parent
    return g
end

local function stroke(parent: Instance, thickness: number): UIStroke
    local s = Instance.new("UIStroke")
    s.Thickness = thickness
    s.Color = Yellow
    s.Parent = parent
    return s
end

local White = Color3.new(1, 1, 1)
local Red = Color3.new(1, 0, 0)
local Green = Color3.new(0, 1, 0)
local Blue = Color3.new(0, 0, 1)

-- 1. Linear, red to green: x 40..440, y 40..80.
gradient(box(40, 40, 400, 40, White), ColorSequence.new(Red, Green))

-- 2. Radial, red at the centre to blue at the radius: centre (140, 220),
--    radius (200 + 200) / 4 = 100.
local radial = gradient(box(40, 120, 200, 200, White), ColorSequence.new(Red, Blue))
radial.Type = Enum.GradientType.Radial

-- 3. Conical, red at 0 and 1, blue at a half turn: centre (560, 100).
local conical = gradient(
    box(500, 40, 120, 120, White),
    ColorSequence.new({
        ColorSequenceKeypoint.new(0, Red),
        ColorSequenceKeypoint.new(0.5, Blue),
        ColorSequenceKeypoint.new(1, Red),
    })
)
conical.Type = Enum.GradientType.Conical

-- 4. Repeat at half scale, black to white: x 660..860, y 40..60. Just past the
--    middle the second run has barely begun, so it is dark where a clamped
--    gradient would be white.
local stripes = gradient(box(660, 40, 200, 20, White), ColorSequence.new(Color3.new(0, 0, 0), White))
stripes.Scale = 0.5
stripes.TileMode = Enum.GradientTileMode.Repeat

-- 5. Transparency: a white box fading from opaque to clear, x 660..860, y 90..130.
local fade = gradient(box(660, 90, 200, 40, White), ColorSequence.new(White))
fade.Transparency = NumberSequence.new(0, 1)

-- 6. Border strokes, 10 px, on grey boxes: Outer at x 300..400 and Inner at
--    x 460..560, both y 200..260.
local Grey = Color3.fromRGB(60, 60, 60)
stroke(box(300, 200, 100, 60, Grey), 10)
stroke(box(460, 200, 100, 60, Grey), 10).BorderStrokePosition = Enum.BorderStrokePosition.Inner

-- 7. Corners, 12 px outer strokes: Miter at x 300..400, Round at x 460..560,
--    both y 340..400. Ten pixels out along both axes from a corner is inside a
--    mitred stroke (10 < 12) and outside a round one (14.1 > 12).
stroke(box(300, 340, 100, 60, Grey), 12).LineJoinMode = Enum.LineJoinMode.Miter
stroke(box(460, 340, 100, 60, Grey), 12)

-- 8. A stroke's own gradient, left to right red to blue, on an Outer stroke
--    around x 660..860, y 340..400.
local coloured = stroke(box(660, 340, 200, 60, Grey), 10)
coloured.Color = White
gradient(coloured, ColorSequence.new(Red, Blue))
