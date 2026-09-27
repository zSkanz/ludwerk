--!strict
--[[
	Gradients and strokes (ADR 0110).

	**Every shape of `UIGradient` and every kind of `UIStroke`, on one
	screen**, each labelled with what it is -- the picture the manual page
	describes, and the one its screenshot is taken of.

	Three things move, and each moves the cheap way: the health bar's shine is
	its gradient's `Offset`, the ring fills by rebuilding a four-stop
	`NumberSequence`, and the spinner turns its conical gradient's `Rotation`.
	A sequence is a value, so a new one each frame is an allocation and not a
	re-layout.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")
local Workspace = game:GetService("Workspace")

Lighting.ClockTime = 15

local screen = Instance.new("ScreenGui")
screen.Name = "Gallery"
screen.Parent = UIService

local backdrop = Instance.new("Frame")
backdrop.Size = UDim2.fromScale(1, 1)
-- White, so the gradient's colours are the backdrop's colours exactly.
backdrop.BackgroundColor = Color3.new(1, 1, 1)
backdrop.BackgroundTransparency = 0.35
backdrop.Parent = screen

-- A dark vignette over the whole screen: a radial gradient's transparency,
-- clear in the middle and dim at the corners.
local vignette = Instance.new("UIGradient")
vignette.Type = Enum.GradientType.Radial
vignette.Scale = 1.6
vignette.Color = ColorSequence.new(Color3.fromRGB(28, 32, 48), Color3.fromRGB(8, 8, 12))
vignette.Parent = backdrop

local function box(parent: Instance, x: number, y: number, width: number, height: number, color: Color3): Frame
    local frame = Instance.new("Frame")
    frame.Position = UDim2.fromOffset(x, y)
    frame.Size = UDim2.fromOffset(width, height)
    frame.BackgroundColor = color
    frame.Parent = parent
    return frame
end

local function caption(parent: Instance, x: number, y: number, text: string)
    local label = Instance.new("TextLabel")
    label.Position = UDim2.fromOffset(x, y)
    label.Size = UDim2.fromOffset(200, 20)
    label.BackgroundTransparency = 1
    label.TextColor = Color3.fromRGB(170, 178, 196)
    label.TextSize = 15
    label.TextXAlignment = Enum.HorizontalAlignment.Left
    label.Text = text
    label.Parent = parent
end

local function corner(parent: Instance, pixels: number)
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, pixels)
    rounded.Parent = parent
end

local function stroke(parent: Instance, thickness: number, color: Color3): UIStroke
    local outline = Instance.new("UIStroke")
    outline.Thickness = thickness
    outline.Color = color
    outline.Parent = parent
    return outline
end

local White = Color3.new(1, 1, 1)

-- --- Top row: gradients -------------------------------------------------------

-- A health bar: red, yellow, green along a white frame, and a shine -- a second
-- frame on top whose gradient is a narrow bright band, slid by `Offset`.
local health = box(backdrop, 60, 60, 300, 28, White)
corner(health, 6)
local healthColors = Instance.new("UIGradient")
healthColors.Color = ColorSequence.new({
    ColorSequenceKeypoint.new(0, Color3.fromRGB(220, 50, 50)),
    ColorSequenceKeypoint.new(0.5, Color3.fromRGB(240, 200, 50)),
    ColorSequenceKeypoint.new(1, Color3.fromRGB(70, 200, 90)),
})
healthColors.Parent = health

local shine = box(health, 0, 0, 300, 28, White)
shine.Size = UDim2.fromScale(1, 1)
corner(shine, 6)
local shineBand = Instance.new("UIGradient")
shineBand.Rotation = 20
shineBand.Transparency = NumberSequence.new({
    NumberSequenceKeypoint.new(0, 1),
    NumberSequenceKeypoint.new(0.45, 1),
    NumberSequenceKeypoint.new(0.5, 0.55),
    NumberSequenceKeypoint.new(0.55, 1),
    NumberSequenceKeypoint.new(1, 1),
})
shineBand.Parent = shine
caption(backdrop, 60, 94, "Linear, three stops, a shine by Offset")

-- A glow: a radial gradient fading a bright disc to nothing.
local glow = box(backdrop, 420, 30, 110, 110, Color3.fromRGB(120, 200, 255))
local glowFade = Instance.new("UIGradient")
glowFade.Type = Enum.GradientType.Radial
glowFade.Transparency = NumberSequence.new({
    NumberSequenceKeypoint.new(0, 0),
    NumberSequenceKeypoint.new(0.35, 0.2),
    NumberSequenceKeypoint.new(1, 1),
})
glowFade.Parent = glow
caption(backdrop, 420, 144, "Radial")

-- A progress ring: a conical gradient's transparency with a hard edge that
-- moves, on a round frame with a round hole cut by an inner stroke's colour.
local ring = box(backdrop, 580, 30, 110, 110, Color3.fromRGB(255, 170, 60))
corner(ring, 55)
local ringFill = Instance.new("UIGradient")
ringFill.Type = Enum.GradientType.Conical
ringFill.Rotation = -90
ringFill.Parent = ring
local hole = box(ring, 22, 22, 66, 66, Color3.fromRGB(22, 24, 34))
corner(hole, 33)
caption(backdrop, 580, 144, "Conical, filling")

-- A spinner: a conical colour wheel, turned by its `Rotation`.
local wheel = box(backdrop, 740, 30, 110, 110, White)
corner(wheel, 55)
local wheelColors = Instance.new("UIGradient")
wheelColors.Type = Enum.GradientType.Conical
wheelColors.Color = ColorSequence.new({
    ColorSequenceKeypoint.new(0, Color3.fromRGB(255, 60, 60)),
    ColorSequenceKeypoint.new(1 / 3, Color3.fromRGB(60, 255, 90)),
    ColorSequenceKeypoint.new(2 / 3, Color3.fromRGB(70, 110, 255)),
    ColorSequenceKeypoint.new(1, Color3.fromRGB(255, 60, 60)),
})
wheelColors.Parent = wheel
caption(backdrop, 740, 144, "Conical, turning")

-- Stripes: a quarter-scale sequence repeated, and the same mirrored.
local stripes = box(backdrop, 900, 40, 150, 40, White)
local stripesRepeat = Instance.new("UIGradient")
stripesRepeat.Color = ColorSequence.new(Color3.fromRGB(40, 40, 60), Color3.fromRGB(240, 220, 90))
stripesRepeat.Scale = 0.25
stripesRepeat.Rotation = 30
stripesRepeat.TileMode = Enum.GradientTileMode.Repeat
stripesRepeat.Parent = stripes
caption(backdrop, 1060, 50, "Repeat")

local mirrored = box(backdrop, 900, 92, 150, 40, White)
local stripesMirror = Instance.new("UIGradient")
stripesMirror.Color = ColorSequence.new(Color3.fromRGB(40, 40, 60), Color3.fromRGB(240, 220, 90))
stripesMirror.Scale = 0.25
stripesMirror.Rotation = 30
stripesMirror.TileMode = Enum.GradientTileMode.Mirror
stripesMirror.Parent = mirrored
caption(backdrop, 1060, 102, "Mirror")

-- --- Middle row: border strokes -------------------------------------------------

local Panel = Color3.fromRGB(46, 52, 70)
local Accent = Color3.fromRGB(255, 196, 70)

local positions: { { position: EnumBorderStrokePosition, label: string } } = {
    { position = Enum.BorderStrokePosition.Outer, label = "Outer" },
    { position = Enum.BorderStrokePosition.Center, label = "Center" },
    { position = Enum.BorderStrokePosition.Inner, label = "Inner" },
}
for index, entry in positions do
    local x = 60 + (index - 1) * 130
    local panel = box(backdrop, x, 230, 100, 70, Panel)
    local outline = stroke(panel, 8, Accent)
    outline.Transparency = 0.2
    outline.BorderStrokePosition = entry.position
    caption(backdrop, x, 314, entry.label)
end

local joins: { { join: EnumLineJoinMode, label: string } } = {
    { join = Enum.LineJoinMode.Round, label = "Round" },
    { join = Enum.LineJoinMode.Bevel, label = "Bevel" },
    { join = Enum.LineJoinMode.Miter, label = "Miter" },
}
for index, entry in joins do
    local x = 470 + (index - 1) * 130
    local panel = box(backdrop, x, 230, 100, 70, Panel)
    local outline = stroke(panel, 10, Color3.fromRGB(110, 190, 255))
    outline.LineJoinMode = entry.join
    caption(backdrop, x, 314, entry.label)
end

-- A double border: two strokes, the second offset past the first and behind it.
local double = box(backdrop, 880, 230, 110, 70, Panel)
stroke(double, 3, Color3.fromRGB(255, 230, 150))
local outerRing = stroke(double, 3, Color3.fromRGB(160, 90, 30))
outerRing.BorderOffset = UDim.new(0, 5)
outerRing.ZIndex = 0
caption(backdrop, 880, 314, "Two strokes")

-- A rounded button whose stroke has a gradient of its own, turning.
local button = box(backdrop, 1040, 230, 170, 70, Color3.fromRGB(34, 40, 58))
corner(button, 16)
local buttonEdge = stroke(button, 4, White)
local rainbow = Instance.new("UIGradient")
rainbow.Color = ColorSequence.new({
    ColorSequenceKeypoint.new(0, Color3.fromRGB(255, 80, 120)),
    ColorSequenceKeypoint.new(0.5, Color3.fromRGB(90, 220, 255)),
    ColorSequenceKeypoint.new(1, Color3.fromRGB(255, 80, 120)),
})
rainbow.Parent = buttonEdge
local buttonText = Instance.new("TextLabel")
buttonText.Size = UDim2.fromScale(1, 1)
buttonText.BackgroundTransparency = 1
buttonText.TextColor = White
buttonText.TextSize = 24
buttonText.Text = "PLAY"
buttonText.Parent = button
caption(backdrop, 1040, 314, "A stroke with a gradient")

-- --- Bottom row: text strokes ---------------------------------------------------

local function words(x: number, y: number, size: number, text: string): TextLabel
    local label = Instance.new("TextLabel")
    label.Position = UDim2.fromOffset(x, y)
    label.Size = UDim2.fromOffset(360, size + 20)
    label.BackgroundTransparency = 1
    label.TextColor = White
    label.TextSize = size
    label.TextXAlignment = Enum.HorizontalAlignment.Left
    label.Text = text
    label.Parent = backdrop
    return label
end

local title = words(60, 400, 56, "GAME OVER")
stroke(title, 4, Color3.fromRGB(180, 30, 40))
caption(backdrop, 60, 480, "Outlined")

local hollow = words(440, 400, 56, "HOLLOW")
hollow.TextTransparency = 1
stroke(hollow, 2, Color3.fromRGB(120, 220, 255))
caption(backdrop, 440, 480, "TextTransparency = 1")

local scaled = words(760, 400, 56, "SCALED")
local scaledEdge = stroke(scaled, 0.08, Color3.fromRGB(40, 20, 60))
scaledEdge.StrokeSizingMode = Enum.StrokeSizingMode.ScaledSize
local scaledColors = Instance.new("UIGradient")
scaledColors.Rotation = 90
scaledColors.Color = ColorSequence.new(Color3.fromRGB(255, 240, 150), Color3.fromRGB(255, 120, 40))
scaledColors.Parent = scaled
caption(backdrop, 760, 480, "Scaled stroke, gradient text")

local rich = words(60, 530, 32, "")
rich.Size = UDim2.fromOffset(900, 50)
rich.RichText = true
rich.Text = 'You won <stroke color="#00A2FF" thickness="2" joins="miter"><b>25 gems</b></stroke> '
    .. 'and <stroke color="#FF4060" th="3" tr="0.2">a crown</stroke>.'
caption(backdrop, 60, 580, "Rich text: <stroke>")

-- --- In the world ---------------------------------------------------------------

local ground = Instance.new("Part")
ground.Anchored = true
ground.Size = vector.create(40, 1, 40)
ground.Position = vector.create(0, -0.5, 0)
ground.Parent = Workspace

local wall = Instance.new("Part")
wall.Anchored = true
wall.Size = vector.create(8, 3, 0.4)
wall.CFrame = CFrame.lookAt(vector.create(0, 1.5, -6), vector.create(0, 1.5, 10))
wall.Parent = Workspace

local sign = Instance.new("SurfaceGui")
sign.Face = Enum.Face.Front
sign.Parent = wall

local signPanel = Instance.new("Frame")
signPanel.Size = UDim2.new(1, -40, 1, -40)
signPanel.Position = UDim2.fromOffset(20, 20)
signPanel.BackgroundColor = White
signPanel.Parent = sign
corner(signPanel, 20)
local signColors = Instance.new("UIGradient")
signColors.Rotation = 90
signColors.Color = ColorSequence.new(Color3.fromRGB(60, 120, 220), Color3.fromRGB(20, 30, 70))
signColors.Parent = signPanel
stroke(signPanel, 6, Color3.fromRGB(255, 210, 90))

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 2, 6), vector.create(0, 1.5, -6))
camera.Parent = Workspace
Workspace.CurrentCamera = camera

-- --- Movement -------------------------------------------------------------------

local elapsed = 0
RunService.Heartbeat:Connect(function(dt: number)
    elapsed += dt

    -- The shine crosses the bar every two seconds, from off one end to off the
    -- other.
    local sweep = (elapsed % 2) / 2
    shineBand.Offset = Vector2.new(sweep * 2 - 1, 0)

    -- The ring fills over three seconds and starts again.
    local fill = math.clamp((elapsed % 3) / 3, 0.001, 0.999)
    ringFill.Transparency = NumberSequence.new({
        NumberSequenceKeypoint.new(0, 0),
        NumberSequenceKeypoint.new(fill, 0),
        NumberSequenceKeypoint.new(fill, 0.85),
        NumberSequenceKeypoint.new(1, 0.85),
    })

    wheelColors.Rotation = (elapsed * 90) % 360
    rainbow.Rotation = (elapsed * 60) % 360
end)
