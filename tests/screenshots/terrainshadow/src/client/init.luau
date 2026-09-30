--!strict

-- **The terrain's shadow on itself, and the bend of its normal** (terrain audit
-- T3): ground where nothing stands between the sun and any face turned to it,
-- photographed at every distance a cascade covers, at a low sun and a high one.
--
-- Every face to the sun here should be lit by the shadow map and the contact
-- mask both (`--debug-view=shadow`, counted by `imgshadow`): the ground is a
-- plain with low rounded hills whose flanks are never steeper than the lowest
-- sun, and the balls float over nothing, so what shadows any face to the sun
-- is the ground shadowing itself -- acne. And the normal it is shaded with
-- should bend smoothly (`--debug-view=bend`, counted by `imgsteps`): the ground
-- has no material, so the only bend is the grain's, which is noise with no
-- crease in it. And with no material it is one grey (`--debug-view=albedo`).
--
-- **One case per `FramesPerCase` frames**, as the terrain gallery does it: the
-- host takes a picture at the last frame of each (`--screenshot-every`).

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local FramesPerCase = 12
-- The lowest is where the light meets the plain at N.L 0.14; the hills'
-- flanks are under nine degrees, so none of them is ever turned from it.
local Elevations = { 8, 12, 20, 35 }
local DomeTop = vector.create(0, 16, 0)
-- Clear of the plain by more than the longest shadow they cast, at 8 degrees,
-- whichever way the sun stands: a ball's shadow lands on nothing.
local Balls = {
    { at = vector.create(0, 40, -900), radius = 2 },
    { at = vector.create(0, 40, -940), radius = 4 },
    { at = vector.create(0, 40, -990), radius = 8 },
}

type Case = { target: vector, distance: number, elevation: number, bearing: number, softness: number }

local Cases: { Case } = {}
-- The plain, from 15 m to 900 m -- every cascade -- at the default softness and
-- at the widest filter `Lighting.ShadowSoftness` asks for.
for _, softness in { 0.2, 1 } do
    for _, elevation in Elevations do
        for _, distance in { 15, 40, 120, 350, 900 } do
            table.insert(Cases, {
                target = DomeTop,
                distance = distance,
                elevation = elevation,
                bearing = 0,
                softness = softness,
            })
        end
    end
end
-- The balls from four sides: the terminator of a round shape, where the old
-- push faceted the shadow.
for _, elevation in Elevations do
    for _, bearing in { 0, 90, 180, 270 } do
        for _, view in { { ball = 1, distance = 8 }, { ball = 3, distance = 25 }, { ball = 3, distance = 70 } } do
            table.insert(Cases, {
                target = Balls[view.ball].at,
                distance = view.distance,
                elevation = elevation,
                bearing = bearing,
                softness = 0.2,
            })
        end
    end
end

-- --- The ground ------------------------------------------------------------------

local terrain = Instance.new("Terrain")
terrain.Name = "Plain"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain.Parent = workspace

-- Rounded hills of cosine profile: the steepest flank of each is its height
-- times pi over twice its reach, under nine degrees for every one.
local Size = 768
local Hills = {
    { x = 0, z = 0, height = 12, reach = 120 },
    { x = -150, z = 90, height = 6, reach = 60 },
    { x = 120, z = -110, height = 6, reach = 60 },
    { x = 40, z = 160, height = 2, reach = 20 },
    { x = -60, z = -60, height = 2, reach = 20 },
}
local heights = table.create(Size * Size, 0)
for z = 0, Size - 1 do
    for x = 0, Size - 1 do
        local wx, wz = x - Size / 2 + 0.5, z - Size / 2 + 0.5
        local height = 4
        for _, hill in Hills do
            local r = math.sqrt((wx - hill.x) ^ 2 + (wz - hill.z) ^ 2)
            if r < hill.reach then
                height += hill.height * (1 + math.cos(math.pi * r / hill.reach)) / 2
            end
        end
        heights[z * Size + x + 1] = height
    end
end
terrain:WriteHeights(vector.create(-Size / 2, -4, -Size / 2), Size, heights, 1)
for _, ball in Balls do
    terrain:FillBall(ball.at, ball.radius, 1)
end

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
    Lighting.ShadowSoftness = case.softness
    -- Looking down at 25 degrees from `distance` away, from the case's side.
    local pitch = math.rad(25)
    local turn = math.rad(case.bearing)
    local across = case.distance * math.cos(pitch)
    local eye = case.target
        + vector.create(math.sin(turn) * across, case.distance * math.sin(pitch), math.cos(turn) * across)
    camera.CFrame = CFrame.lookAt(eye, case.target)
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
