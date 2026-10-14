--!strict

-- **A terrain layer's repeat, broken up** (ADR 0113's amendment): the same
-- 3 m patch of grass every 3 m is a carpet near and a flat blur far. Three
-- flat fields of the built-in grass, 256 m square each:
--
--   1. `repeating-grass`: the variation and the far sample turned off -- the
--      layer as every terrain drew it before;
--   2. the built-in grass as it is: the default variation and far sample;
--   3. `hex-grass`: the same, on hexagonal cells.
--
-- Each photographed straight down from 12 m (near) and 60 m (far), which the
-- driver measures (`imgtile`), and once across the field from a person's
-- height, for the eye. `--debug-view=albedo` draws the colour alone, so the
-- sun's shading is not part of what is measured.
--
-- **One case per `FramesPerCase` frames**, as the terrain gallery does it: the
-- host takes a picture at the last frame of each (`--screenshot-every`).

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local FramesPerCase = 20
local Size = 256
local Spacing = 400
local Fields = { -Spacing, 0, Spacing }

type Case = { field: number, height: number, across: boolean }

local Cases: { Case } = {}
for field in Fields do
    table.insert(Cases, { field = field, height = 12, across = false })
    table.insert(Cases, { field = field, height = 60, across = false })
    table.insert(Cases, { field = field, height = 1.8, across = true })
end

-- --- The ground ------------------------------------------------------------------

local terrain = Instance.new("Terrain")
terrain.Name = "Fields"
terrain.MinHeight = -8
terrain.MaxHeight = 8
terrain:SetLayers({
    "asset://materials/repeating-grass.material.json",
    "engine://terrain/grass",
    "asset://materials/hex-grass.material.json",
})
terrain.Parent = workspace

local heights = table.create(Size * Size, 0)
for index, x in Fields do
    terrain:WriteHeights(vector.create(x - Size / 2, 0, -Size / 2), Size, heights, index)
end

-- --- The camera ----------------------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.NearPlane = 0.1
camera.FarPlane = 2000
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 12
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

local current = 0
local function applyCase()
    local frame = math.floor(RunService.SimTime * 60 + 0.5)
    local index = math.clamp(frame // FramesPerCase + 1, 1, #Cases)
    if index == current then
        return
    end
    current = index
    local case = Cases[index]
    local centre = vector.create(Fields[case.field], 0, 0)
    if case.across then
        -- From the field's near edge, looking along it and a little down.
        local eye = centre + vector.create(0, case.height, Size / 2 - 4)
        camera.CFrame = CFrame.lookAt(eye, centre + vector.create(0, 0, -Size / 4))
    else
        -- Straight down, north up the picture.
        camera.CFrame = CFrame.lookAt(centre + vector.create(0, case.height, 0), centre, vector.create(0, 0, -1))
    end
end

applyCase()
RunService.Heartbeat:Connect(applyCase)
