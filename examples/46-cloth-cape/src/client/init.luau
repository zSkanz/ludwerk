--!strict

-- **A cape that is cloth** (ADR 0214): a mesh of its own on each figure, worn
-- over the body's pose, and a `Cloth` that hangs it from a joint on the
-- shoulders. Every vertex of it moves: it folds, it fills with the wind, it
-- rests on the body's back and on the wall behind the third figure.
--
-- What to look at: the runner's cape streams out behind the run and swings
-- past vertical on the stop; the dash stretches it out without tearing it; and
-- the teleport leaves it hanging at rest where the figure lands. The second
-- figure stands in the wind. The third has its back to a wall and the wind
-- blows its cape onto it: the cape lies on the wall and does not go through.
--
-- The models are drawn by `tools/make_models.py`: the cape is modelled hanging
-- straight down behind the body, its top row weighted to the `CapePin` joint
-- the `Cloth` pins, the rest to the body.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

-- The floor, the camera, the wall, the wind and the three figures are the
-- scene (`content/scenes/cloth.scene.json`, written by `tools/make_scene.py`):
-- open the project in the editor and they are there, capes already hanging.
-- What this script does is move one of them.
local runner = workspace:WaitForChild("Runner") :: Model
local body = runner:WaitForChild("Body") :: MeshPart

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
        body.CFrame = facing(at, vector.create(math.cos(angle), 0, -math.sin(angle)))
        dashFrom = at
    elseif clock < 4.2 then
        -- Stopped where the lap ended.
        body.CFrame = facing(dashFrom, vector.create(1, 0, 0))
    elseif clock < 4.5 then
        -- A dash: nine metres in three tenths of a second.
        local along = (clock - 4.2) / 0.3
        body.CFrame = facing(dashFrom + vector.create(along * 9, 0, 0), vector.create(1, 0, 0))
    elseif clock < 6 then
        body.CFrame = facing(dashFrom + vector.create(9, 0, 0), vector.create(1, 0, 0))
    else
        -- Somewhere else, at once.
        body.CFrame = facing(vector.create(-6, 0, 3), vector.create(0, 0, -1))
    end
end)
