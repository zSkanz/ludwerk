--!strict

-- **A slab's shadow on itself** (D506): one thin flat slab -- as the cap along
-- the top of an arena's wall is -- floating over nothing, photographed from
-- above on the sun's side at every distance a cascade covers, at a low sun.
--
-- A convex solid cannot shadow a face of its own turned to the sun, and nothing
-- else is in the world, so each case is photographed twice -- with the sun's
-- shadows and with `Lighting.GlobalShadows` off -- and the two pictures should
-- be the same (the driver compares them with `imgcmp`). What darkened them
-- was a depth bias stated in metres for every cascade. The map holds the slab's
-- underside, a hand's breadth below its top; the sun meets the top at a low
-- angle, so across one texel of a far cascade the top rises past that -- and it
-- came out a little shadowed there and not near the eye, a patch of different
-- shading that slid along with the camera as the splits did.
--
-- **One picture per `FramesPerCase` frames**: the host takes it at the last
-- frame of each (`--screenshot-every`), shadows on and then off.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local FramesPerCase = 12
local Elevations = { 8, 15, 25 }
-- How the slab's length turns from the sun's bearing.
local Turns = { 30, 60, 90 }
-- Every cascade: the near one, its neighbour, and the far two.
local Distances = { 12, 40, 120, 300 }
local Centre = vector.create(0, 200, 0)
-- Thin as a cap is, whatever the distance: the far cascades' texels rise past
-- it across the top, which is the case the bias has to survive.
local Thickness = 0.3

type Case = { elevation: number, turn: number, distance: number, shadows: boolean }

local Cases: { Case } = {}
for _, elevation in Elevations do
    for _, turn in Turns do
        for _, distance in Distances do
            table.insert(Cases, { elevation = elevation, turn = turn, distance = distance, shadows = true })
            table.insert(Cases, { elevation = elevation, turn = turn, distance = distance, shadows = false })
        end
    end
end

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.NearPlane = 0.1
camera.FarPlane = 4000
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.GeographicLatitude = 0
-- Held still: an exposure still settling after the sun moved would make the two
-- pictures of a case differ everywhere.
Lighting.AutoExposure = false
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- The clock time that stands the sun at `degrees` above the horizon: at
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

local slab: Part? = nil
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
    Lighting.GlobalShadows = case.shadows

    -- Where the sun stands, flat on the ground: the slab turns from it and the
    -- camera stands on its side.
    local sun = Lighting.SunDirection
    local bearing = math.atan2(sun.x, sun.z)

    -- Sized to the distance so it fills the picture, and never thicker.
    if slab then
        slab:Destroy()
    end
    local made = Instance.new("Part")
    made.Anchored = true
    made.Size = vector.create(case.distance * 0.5, Thickness, case.distance * 0.25)
    made.CFrame = CFrame.new(Centre) * CFrame.fromEuler(0, bearing + math.rad(case.turn), 0)
    made.Parent = workspace
    slab = made

    -- From the sun's side, a little to one side of it and well above, to see the top.
    local view = bearing + math.rad(20)
    local across = case.distance * math.cos(math.rad(40))
    local eye = Centre
        + vector.create(math.sin(view) * across, case.distance * math.sin(math.rad(40)), math.cos(view) * across)
    camera.CFrame = CFrame.lookAt(eye, Centre)
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
