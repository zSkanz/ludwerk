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
-- chain of its own under a top joint on the shoulders, and those top joints
-- are what a `SpringBone` finds.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

-- The floor, the camera, the wind and both figures are the scene
-- (`content/scenes/cape.scene.json`, written by `tools/make_scene.py`): a
-- `SpringBone` with `JointPattern = "Cape_*0"` -- the top joint of each column,
-- each a chain of its own -- and a `SpringCollider` on the body, under each
-- figure. Open the project in the editor and they are there, capes already in
-- the wind; nothing in this script makes them. What the script does is move one.
local runner = workspace:WaitForChild("Figure") :: MeshPart

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
