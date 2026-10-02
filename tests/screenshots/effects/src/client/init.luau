--!strict
--[[
	The probe scene for `Highlight`, `Beam` and `Trail` (ADR 0129).

	An orthographic camera twenty metres wide and ten tall, so every place in
	the picture is a place in the world: `u = (x + 10) / 20`, `v = (5 - y) / 10`.
	A grey wall fills the back. In front of it, each a claim
	`run_effects_gate.cmake` probes:

	  top left   -- a cube BEHIND a wall, highlighted `AlwaysOnTop`: its yellow
	                fill shows through the wall, in exactly the bytes it was
	                given (a highlight is a mark on the picture, not a lit
	                thing).
	  next       -- the same behind another wall, `Occluded`: the wall is what
	                is seen, and nothing is marked.
	  next       -- a cube nothing hides, `Occluded`: cyan, and a red line
	                round its edge.
	  top right  -- a green beam four metres long and one wide, at no light
	                influence: a band, and wall above and below it.
	  bottom     -- a part carried along X for the whole run with a trail
	                between two attachments a metre above and below it: where
	                it has been is magenta, where it has not is wall.

	Rendered at 1000 by 500, after forty frames.
]]

local RunService = game:GetService("RunService")

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.new(0, 0, 30)

local function block(at: vector, size: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Anchored = true
    part.CanCollide = false
    part.Size = size
    part.CFrame = CFrame.new(at)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

local function mark(on: BasePart, mode: EnumHighlightDepthMode, fill: Color3)
    local highlight = Instance.new("Highlight")
    highlight.DepthMode = mode
    highlight.FillColor = fill
    highlight.FillTransparency = 0
    highlight.OutlineColor = Color3.new(1, 0, 0)
    highlight.Parent = on
end

-- The back wall: what "nothing here" looks like.
block(vector.create(0, 0, -10), vector.create(40, 20, 1), Color3.new(0.5, 0.5, 0.5))

-- Behind a wall, on top of everything.
block(vector.create(-7.5, 2.5, 0), vector.create(4, 4, 1), Color3.new(0.2, 0.2, 0.6))
mark(
    block(vector.create(-7.5, 2.5, -5), vector.create(2, 2, 2), Color3.new(1, 1, 1)),
    Enum.HighlightDepthMode.AlwaysOnTop,
    Color3.new(1, 1, 0)
)

-- Behind a wall, and hidden by it.
block(vector.create(-2.5, 2.5, 0), vector.create(4, 4, 1), Color3.new(0.2, 0.2, 0.6))
mark(
    block(vector.create(-2.5, 2.5, -5), vector.create(2, 2, 2), Color3.new(1, 1, 1)),
    Enum.HighlightDepthMode.Occluded,
    Color3.new(1, 1, 0)
)

-- In plain sight.
mark(
    block(vector.create(2.5, 2.5, 5), vector.create(2, 2, 2), Color3.new(1, 1, 1)),
    Enum.HighlightDepthMode.Occluded,
    Color3.new(0, 1, 1)
)

-- A point in the world to hang an end on: a part nobody sees.
local function point(at: vector): Attachment
    local holder = block(at, vector.create(0.1, 0.1, 0.1), Color3.new(0, 0, 0))
    holder:SetMaterialParameter("Transparency", 1)
    local attachment = Instance.new("Attachment")
    attachment.Parent = holder
    return attachment
end

local beam = Instance.new("Beam")
beam.Attachment0 = point(vector.create(5.5, 3.5, 5))
beam.Attachment1 = point(vector.create(9.5, 3.5, 5))
beam.Color = ColorSequence.new(Color3.new(0, 1, 0))
beam.Width0 = 1
beam.Width1 = 1
beam.LightInfluence = 0
beam.Parent = workspace

-- Something that moves, and what it leaves.
local runner = block(vector.create(-9, -2.5, 5), vector.create(0.2, 0.2, 0.2), Color3.new(1, 1, 1))
local above = Instance.new("Attachment")
above.CFrame = CFrame.new(0, 1, 0)
above.Parent = runner
local below = Instance.new("Attachment")
below.CFrame = CFrame.new(0, -1, 0)
below.Parent = runner

local trail = Instance.new("Trail")
trail.Attachment0 = above
trail.Attachment1 = below
trail.Lifetime = 60
trail.Color = ColorSequence.new(Color3.new(1, 0, 1))
trail.LightInfluence = 0
trail.Parent = runner

-- A fifth of a metre a tick: eight metres in the forty the gate runs.
RunService.Heartbeat:Connect(function()
    runner.Position = runner.Position + vector.create(0.2, 0, 0)
end)
