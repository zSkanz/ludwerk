--!strict

-- **The terrain gallery** (terrain audit T0): every shape the audit found
-- broken, on one terrain, photographed from every distance the owner flies at.
--
-- No image test of terrain existed before this; every earlier audit read code
-- and ran unit tests, and the owner found the rest by looking. This is the
-- looking, done the same way every run.
--
-- **One case per `FramesPerCase` frames.** The host takes a picture at the last
-- frame of each (`--screenshot-every`), so a case has that many frames for the
-- levels of detail to load before it is photographed. The cases run in the
-- order `Cases` lists them: the gate's nine first -- every distance, the sun at
-- 30 degrees, from the south -- then the sun at 15 and 60 degrees and from the
-- other three sides, for the full gallery.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local FramesPerCase = 24
local Distances = { 5, 15, 30, 60, 120, 250, 500, 1000, 2000 }
local Elevations = { 30, 15, 60 }
-- Where the camera stands, around the gallery, in degrees from +Z: turning
-- the camera round a fixed sun is turning the sun round the gallery.
local Bearings = { 0, 90, 180, 270 }

type Case = { distance: number, elevation: number, bearing: number }

local Cases: { Case } = {}
for _, elevation in Elevations do
    for _, bearing in Bearings do
        for _, distance in Distances do
            table.insert(Cases, { distance = distance, elevation = elevation, bearing = bearing })
        end
    end
end

-- --- The shapes -----------------------------------------------------------------

local Grass = 1
local Sand = 2
local Rock = 3

local terrain = Instance.new("Terrain")
terrain.Name = "Gallery"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({
    "engine://terrain/grass",
    "engine://terrain/sand",
    "engine://terrain/rock",
})
terrain.Parent = workspace

-- Two slabs side by side, floating: ground that does not reach the floor, the
-- owner's own kind. A 4 m one to the west, a 2 m one to the east.
terrain:FillBlock(vector.create(-48, -2, 0), vector.create(96, 4, 160), Grass)
terrain:FillBlock(vector.create(48, -1, 0), vector.create(96, 2, 160), Grass)

-- Balls of radius 2, 4 and 8 on the 4 m slab, sunk a little way in.
terrain:FillBall(vector.create(-84, 1.2, -48), 2, Grass)
terrain:FillBall(vector.create(-70, 2.4, -48), 4, Grass)
terrain:FillBall(vector.create(-48, 4.8, -48), 8, Grass)
-- Two balls merged into one shape.
terrain:FillBall(vector.create(-26, 3, -48), 4, Grass)
terrain:FillBall(vector.create(-20, 3, -48), 4, Grass)
-- One floating clear of the ground.
terrain:FillBall(vector.create(-8, 12, -48), 4, Grass)

-- An overhang: a block, with a ball taken out of the foot of its east face.
terrain:FillBlock(vector.create(-72, 6, 24), vector.create(16, 12, 16), Rock)
terrain:FillBall(vector.create(-64, 5, 24), 4, 0)
-- A cave: a mound with a tunnel through it, east to west.
terrain:FillBlock(vector.create(-32, 6, 24), vector.create(24, 12, 20), Rock)
for x = -46, -18, 2 do
    terrain:FillBall(vector.create(x, 4, 24), 3, 0)
end

-- A 45-degree slope on the 2 m slab, rising to the north: each metre across
-- one metre higher, as columns.
local columns = 24
local heights = {}
for z = 0, 23 do
    for x = 0, columns - 1 do
        heights[z * columns + x + 1] = z + 1
    end
end
terrain:WriteHeights(vector.create(20, 0, 20), columns, heights, Grass)

-- A painted area of two materials.
terrain:PaintBall(vector.create(40, 0, -40), 8, Sand)
terrain:PaintBall(vector.create(52, 0, -40), 6, Rock)

-- **Ground with no material**: a second terrain with no layers, behind the
-- first as the cameras look -- plain matte grey (ADR 0113).
local plain = Instance.new("Terrain")
plain.Name = "Plain"
plain.MinHeight = -64
plain.MaxHeight = 64
plain.Parent = workspace
plain:FillBlock(vector.create(0, -2, -112), vector.create(64, 4, 48), 1)
plain:FillBall(vector.create(0, 2, -112), 6, 1)

-- --- The camera and the sun ------------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.NearPlane = 0.1
camera.FarPlane = 8000
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.GeographicLatitude = 0
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- **The clock time that stands the sun at `degrees` above the horizon**: at
-- latitude zero it climbs the east all morning, so a search between dawn and
-- noon finds it.
local function clockFor(degrees: number): number
    local wanted = math.sin(math.rad(degrees))
    local low, high = 6, 12
    for _ = 1, 40 do
        local middle = (low + high) / 2
        Lighting.ClockTime = middle
        if Lighting.SunDirection.y < wanted then
            low = middle
        else
            high = middle
        end
    end
    return (low + high) / 2
end

-- What the camera looks at: the gallery's middle, a little above the ground.
local Target = vector.create(0, 2, -20)

local current = 0
local function applyCase()
    local frame = math.floor(RunService.SimTime * 60 + 0.5)
    local index = math.clamp(frame // FramesPerCase + 1, 1, #Cases)
    if index == current then
        return
    end
    current = index
    local case = Cases[index]
    Lighting.ClockTime = clockFor(case.elevation)
    -- Looking down at 25 degrees from `distance` away, from the case's side.
    local pitch = math.rad(25)
    local turn = math.rad(case.bearing)
    local across = case.distance * math.cos(pitch)
    local eye = Target
        + vector.create(math.sin(turn) * across, case.distance * math.sin(pitch), math.cos(turn) * across)
    camera.CFrame = CFrame.lookAt(eye, Target)
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
