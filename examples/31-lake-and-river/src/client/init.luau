--!strict
--[[
	A lake and the river that feeds it (ADR 0118).

	**Two `Water`s over terrain.** The lake is a `Box`: a rectangle of water at
	one level, as deep as its `Size.Y`, drawn only where it is and floating only
	what is in it. The river is a `Spline`: a ribbon `Size.X` wide along the
	`WaterPoint`s under it, whose water moves along it at `FlowSpeed`.

	**The script moves nothing.** A log is a cylinder with a density; dropped in
	at the head of the river, the water holds it up and drags it along with the
	flow, round the bends, until the river runs out in the lake -- where the
	water stands still and the log drifts to a stop.
]]

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

--- Material ids: the terrain's layers, in the order `SetLayers` gives them.
local Grass = 1
local Sand = 2

--- Where the ground is, and where the water stands: a little under it.
local GroundLevel = 0
local WaterLevel = -1

--- Seconds between logs, and how many the river carries at most.
local LogEvery = 4
local MaxLogs = 30

-- --- The ground ---------------------------------------------------------------

local terrain = Instance.new("Terrain")
terrain:SetLayers({
    "asset://materials/terrain/grass.material.json",
    "asset://materials/terrain/sand.material.json",
    "asset://materials/terrain/rock.material.json",
})
terrain.Name = "Ground"
terrain.MinHeight = -32
terrain.MaxHeight = 32
terrain.Parent = Workspace

-- A plain, its top at `GroundLevel`.
terrain:FillBlock(vector.create(0, GroundLevel - 12, 0), vector.create(256, 24, 256), Grass)

-- **The lake's basin**: a big ball of air whose top pokes through the plain,
-- so the shore shelves gently rather than dropping off.
local LakeCentre = vector.create(40, 0, 10)
local LakeRadius = 30
local LakeSink = 12
terrain:FillBall(vector.create(LakeCentre.x, GroundLevel + LakeSink, LakeCentre.z), LakeRadius, 0)
terrain:PaintBall(vector.create(LakeCentre.x, GroundLevel, LakeCentre.z), 30, Sand)

-- **The river's course**, from the hills in the north-west down to the lake.
-- The same points carve the channel and lay the water along it.
local Course = {
    vector.create(-110, 0, -70),
    vector.create(-80, 0, -52),
    vector.create(-62, 0, -20),
    vector.create(-40, 0, -8),
    vector.create(-14, 0, -18),
    vector.create(8, 0, -6),
    vector.create(20, 0, 4),
}
local RiverWidth = 7

for index = 1, #Course - 1 do
    local from = Course[index]
    local to = Course[index + 1]
    local steps = math.ceil(vector.magnitude(to - from) / 2)
    for step = 0, steps do
        local at = from + (to - from) * (step / steps)
        -- A ball whose cap is the channel: as wide as the river at the water
        -- line, about three metres deep in the middle.
        terrain:FillBall(vector.create(at.x, GroundLevel + 3, at.z), 5.5, 0)
        terrain:PaintBall(vector.create(at.x, GroundLevel - 2, at.z), 5, Sand)
    end
end

-- --- The water ----------------------------------------------------------------

local lake = Instance.new("Water")
lake.Name = "Lake"
lake.Shape = Enum.WaterShape.Box
lake.Position = LakeCentre
-- The box's footprint covers the basin; the ground hides what it does not.
lake.Size = vector.create(LakeRadius * 2, LakeSink, LakeRadius * 2)
lake.SurfaceLevel = WaterLevel
-- A breeze across the lake, which is all that moves its water: what the river
-- brings in drifts slowly off towards the far shore instead of piling up at
-- the mouth.
lake.Current = vector.create(0.25, 0, 0.12)
lake.Parent = Workspace

-- A breeze's ripples: short, low waves, two of them across each other.
for index, wave in
    { { Length = 9, Amplitude = 0.06, Direction = 20 }, { Length = 5, Amplitude = 0.03, Direction = 110 } }
do
    local ripple = Instance.new("WaterWave")
    ripple.Name = `Ripple{index}`
    ripple.Wavelength = wave.Length
    ripple.Amplitude = wave.Amplitude
    ripple.Direction = wave.Direction
    ripple.Parent = lake
end

local river = Instance.new("Water")
river.Name = "River"
river.Shape = Enum.WaterShape.Spline
-- Across, and how deep.
river.Size = vector.create(RiverWidth, 3, 0)
river.SurfaceLevel = WaterLevel
-- **The water itself moves**: along the course, from the first point to the
-- last, at this many metres a second. What floats in it is dragged along.
river.FlowSpeed = 3.5
river.Parent = Workspace

for index, at in Course do
    local point = Instance.new("WaterPoint")
    point.Name = `Point{index}`
    point.Position = at
    point.Parent = river
end

-- --- The logs -----------------------------------------------------------------

local logs = Instance.new("Folder")
logs.Name = "Logs"
logs.Parent = Workspace

local LogColor = Color3.fromRGB(118, 84, 56)
local Head = Course[1] + (Course[2] - Course[1]) * 0.1

local dropped = 0
local function dropLog()
    dropped += 1
    local log = Instance.new("Part")
    log.Name = `Log{dropped}`
    log.Shape = Enum.PartShape.Cylinder
    log.Size = vector.create(0.7, 3.2, 0.7)
    log:SetMaterialParameter("Color", LogColor)
    -- Half the water's: it floats half under.
    log.Density = 0.5
    -- Lying down, across the flow, a little above the water.
    log.CFrame = CFrame.new(Head.x, WaterLevel + 1, Head.z)
        * CFrame.fromEuler(0, 0, math.pi / 2, Enum.RotationOrder.YXZ)
    log.Parent = logs
    -- The oldest goes when the river is full.
    local all = logs:GetChildren()
    if #all > MaxLogs then
        all[1]:Destroy()
    end
end

-- --- The camera ---------------------------------------------------------------

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.FieldOfView = 60
camera.Parent = Workspace
Workspace.CurrentCamera = camera

-- High on the bank where the river meets the lake, looking up the river.
camera.CFrame = CFrame.lookAt(vector.create(34, 16, -26), vector.create(-24, WaterLevel, -8))

local nextLog = 0
RunService.Heartbeat:Connect(function()
    if RunService.SimTime >= nextLog then
        nextLog += LogEvery
        dropLog()
    end
end)

print("lake and river: two Waters over terrain, and logs the river carries into the lake")
