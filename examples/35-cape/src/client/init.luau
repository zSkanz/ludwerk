--!strict

-- **A cape on a figure** (ADR 0194): `SpringBone`s on three columns of joints,
-- two `SpringCollider`s for the body they must not pass through, and a figure
-- that does the things a hero does to a cape -- runs, turns, stops dead,
-- dashes, and is put somewhere else in one frame.
--
-- What to look at: the cape streams out behind the run and swings past
-- vertical on the stop; the dash stretches it flat without tearing it off; and
-- the teleport leaves it hanging at rest where the figure lands, not flung
-- across the map from where it was. A second figure stands in the wind and
-- does nothing, which is the other half of what a cape is for.
--
-- The model is drawn by `tools/make_figure.py`: each column of the cape is a
-- chain of its own under a top joint on the shoulders, and the top joint is
-- the one a `SpringBone` names.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local Columns = { "Cape_L0", "Cape_M0", "Cape_R0" }

local function figure(at: vector): MeshPart
    local body = Instance.new("MeshPart")
    body.Name = "Figure"
    body.MeshContent = "asset://models/figure.gltf" :: Content
    body.Anchored = true
    body.CFrame = CFrame.new(at.x, at.y, at.z)
    body.Parent = workspace

    -- A chain a column. The defaults are a cloth's; a tail would be stiffer
    -- and a rope would be none.
    for _, column in Columns do
        local spring = Instance.new("SpringBone")
        spring.Name = column
        spring.RootJoint = column
        spring.Stiffness = 0.12
        spring.Damping = 0.18
        spring.LimitAngle = 80
        spring.Radius = 0.04
        spring.Parent = body
    end

    -- The back and the legs: a capsule the height of the body, just in front
    -- of where the cape hangs.
    local back = Instance.new("SpringCollider")
    back.Name = "Back"
    back.JointName = "Body"
    back.Offset = vector.create(0, 0.25, 0)
    back.Length = 1.3
    back.Radius = 0.2
    back.Parent = body
    return body
end

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(80, 1, 80)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor.Parent = workspace

local runner = figure(vector.create(0, 0, 0))
local stander = figure(vector.create(-3, 0, -2))
stander.Name = "InTheWind"

-- A wind from the figures' left, with gusts in it.
workspace.GlobalWind = vector.create(5, 0, 1)
workspace.WindGusts = 0.6

local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.CFrame = CFrame.lookAt(vector.create(1.5, 2.6, 8.5), vector.create(1, 1.0, -1.5))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 15
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- **The runner's eight seconds**, over and over: a circle at a run, a dead
-- stop, a dash, a pause, and a jump across the floor in one frame.
local Radius = 4
local Lap = 3.2
local clock = 0
local dashFrom = vector.create(0, 0, 0)

local function facing(at: vector, towards: vector): CFrame
    return CFrame.lookAt(at, at + towards)
end

RunService.Heartbeat:Connect(function(delta: number)
    clock = (clock + delta) % 8
    if clock < Lap then
        -- A lap of the circle, facing along it.
        local angle = clock / Lap * math.pi * 2
        local at = vector.create(math.sin(angle) * Radius, 0, math.cos(angle) * Radius - Radius)
        runner.CFrame = facing(at, vector.create(math.cos(angle), 0, -math.sin(angle)))
        dashFrom = at
    elseif clock < 4.2 then
        -- Stopped where the lap ended.
        runner.CFrame = facing(dashFrom, vector.create(1, 0, 0))
    elseif clock < 4.5 then
        -- A dash: nine metres in three tenths of a second.
        local along = (clock - 4.2) / 0.3
        runner.CFrame = facing(dashFrom + vector.create(along * 9, 0, 0), vector.create(1, 0, 0))
    elseif clock < 6 then
        runner.CFrame = facing(dashFrom + vector.create(9, 0, 0), vector.create(1, 0, 0))
    else
        -- Somewhere else, at once.
        runner.CFrame = facing(vector.create(-6, 0, 3), vector.create(0, 0, -1))
    end
end)
